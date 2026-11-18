// Feasibility probe: can macOS drive a Bluetooth DualSense's voice-coil
// haptics with raw waveform reports, the way Vibeshine's Moonlight fork does
// on Windows and Linux?
//
// It plays a few test patterns through output report 0x32 and prints how well
// macOS kept up with the 10.67 ms report cadence. Run it with Moonlight closed.
//
// Report layout and CRC follow the SAxense research by Sdore (Egor Vorontsov):
// https://apps.sdore.me/SAxense/ and https://github.com/egormanga/SAxense
// The motor-power setup report, controller queue depth, and sending a few
// reports without waiting follow P5M for Mac's macDualSenseBluetooth.mm:
// https://github.com/beecrepaldi-afk/P5M-mac
//
// Build: clang -O2 -Wall -o ds5_bt_haptics_probe ds5_bt_haptics_probe.c \
//          -framework IOKit -framework CoreFoundation
// Run:   ./ds5_bt_haptics_probe [strength 0.05-1.0] [async|sync] [max in flight] [pack1|pack2]
//        Defaults: 0.8, async (send without waiting), 4 in flight, pack1.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SONY_VID 0x054c
#define DUALSENSE_PID 0x0ce6
#define DUALSENSE_EDGE_PID 0x0df2

#define MAX_REPORT_BYTES 547
#define PCM_BYTES 64      // 32 stereo frames of signed 8-bit samples
#define SAMPLE_RATE 3000  // per channel
#define FRAMES_PER_BLOCK (PCM_BYTES / 2)
#define BLOCK_NS (1000000000ull * FRAMES_PER_BLOCK / SAMPLE_RATE)  // 10.67 ms

// Default: report 0x32, 142 bytes (ID + 137 payload + 4 CRC), one block.
// "pack2": report 0x33, 206 bytes, two blocks under one haptics sub-packet,
// in the layout DS5Dongle and P5M use for report 0x39. Send time grows with
// bytes, so two blocks in 206 bytes cost less link time than two 142s.
static uint8_t report_id = 0x32;
static size_t report_bytes = 142;
static int blocks_per_report = 1;
#define FRAMES_PER_REPORT (FRAMES_PER_BLOCK * blocks_per_report)
#define PERIOD_NS (BLOCK_NS * blocks_per_report)

static mach_timebase_info_data_t timebase;

static uint64_t now_ns(void)
{
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

static void sleep_until_ns(uint64_t deadline)
{
    mach_wait_until(deadline * timebase.denom / timebase.numer);
}

// CRC32 over the report, seeded as if the Bluetooth HID output header byte
// 0xA2 came first. The controller drops reports whose CRC does not match.
static uint32_t report_crc(const uint8_t *data, size_t size)
{
    uint32_t crc = ~UINT32_C(0xEADA2D49);
    while (size--) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & (0u - (crc & 1)));
    }
    return ~crc;
}

static void sign_report(uint8_t *report, size_t size)
{
    uint32_t crc = report_crc(report, size - 4);
    for (int i = 0; i < 4; i++)
        report[size - 4 + i] = (uint8_t)(crc >> (8 * i));
}

// Frames the controller buffers before it starts playing. 16 is the smallest
// value DS5Dongle and P5M found reliable; more buffering adds latency.
#define CONTROLLER_QUEUE 16

static void build_report(uint8_t *report, uint8_t sequence, const int8_t *pcm)
{
    memset(report, 0, report_bytes);
    report[0] = report_id;
    report[1] = (uint8_t)((sequence & 0x0f) << 4);
    if (blocks_per_report == 2) {
        report[2] = 0x91;                  // audio control, sized
        report[3] = 6;
        report[4] = 0x7e;
        report[5] = report[6] = report[7] = report[8] = CONTROLLER_QUEUE;
        report[9] = (uint8_t)(sequence * 2);
        report[10] = 0xd2;                 // haptics, sized, two blocks
        report[11] = PCM_BYTES;
        memcpy(report + 12, pcm, 2 * PCM_BYTES);
        sign_report(report, report_bytes);
        return;
    }
    // Sub-packet 0x11 (sized, 7 bytes): audio control, with a running counter.
    report[2] = 0x91;
    report[3] = 7;
    report[4] = 0xfe;
    report[5] = report[6] = report[7] = report[8] = report[9] = CONTROLLER_QUEUE;
    report[10] = sequence;
    // Sub-packet 0x12 (sized, 64 bytes): interleaved left/right samples.
    report[11] = 0x92;
    report[12] = PCM_BYTES;
    memcpy(report + 13, pcm, PCM_BYTES);
    sign_report(report, report_bytes);
}

// Setup report 0x31: full motor power, haptics filter on, nothing muted.
// Only these fields are marked valid, so rumble emulation stays off.
static IOReturn send_full_power(IOHIDDeviceRef device)
{
    uint8_t report[78] = {0};
    report[0] = 0x31;
    report[2] = 0x10;
    report[4] = 0x02 | 0x20 | 0x40;
    report[42] = 0x01;
    sign_report(report, sizeof(report));
    return IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, 0x31, report, sizeof(report));
}

static long number_property(IOHIDDeviceRef device, CFStringRef key)
{
    long value = -1;
    CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
    if (ref && CFGetTypeID(ref) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)ref, kCFNumberLongType, &value);
    return value;
}

static void string_property(IOHIDDeviceRef device, CFStringRef key, char *out, size_t size)
{
    out[0] = '\0';
    CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
    if (ref && CFGetTypeID(ref) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)ref, out, (CFIndex)size, kCFStringEncodingUTF8);
}

static IOHIDDeviceRef find_dualsense(IOHIDManagerRef manager)
{
    IOHIDManagerSetDeviceMatching(manager, NULL);
    CFSetRef set = IOHIDManagerCopyDevices(manager);
    if (!set) return NULL;
    CFIndex count = CFSetGetCount(set);
    IOHIDDeviceRef *devices = calloc((size_t)count, sizeof(*devices));
    CFSetGetValues(set, (const void **)devices);
    IOHIDDeviceRef found = NULL;
    for (CFIndex i = 0; i < count && !found; i++) {
        long vid = number_property(devices[i], CFSTR(kIOHIDVendorIDKey));
        long pid = number_property(devices[i], CFSTR(kIOHIDProductIDKey));
        long page = number_property(devices[i], CFSTR(kIOHIDPrimaryUsagePageKey));
        long usage = number_property(devices[i], CFSTR(kIOHIDPrimaryUsageKey));
        if (vid == SONY_VID && (pid == DUALSENSE_PID || pid == DUALSENSE_EDGE_PID) &&
            page == 1 && usage == 5) {
            found = devices[i];
            CFRetain(found);
        }
    }
    free(devices);
    CFRelease(set);
    return found;
}

#define MAX_SAMPLES 4096
static double call_ms[MAX_SAMPLES], late_ms[MAX_SAMPLES];

struct stats {
    unsigned sent, failed, late;
    uint64_t worst_call_ns, worst_late_ns;
    IOReturn first_error;
};

typedef double (*waveform)(double t, int channel);

static double buzz_both(double t, int ch) { (void)ch; return sin(2 * M_PI * 160 * t); }
static double buzz_left(double t, int ch) { return ch == 0 ? sin(2 * M_PI * 160 * t) : 0; }
static double buzz_right(double t, int ch) { return ch == 1 ? sin(2 * M_PI * 160 * t) : 0; }
static double sweep(double t, int ch)
{
    (void)ch;
    // 40 Hz to 400 Hz over two seconds.
    double f0 = 40, f1 = 400, d = 2.0;
    double phase = 2 * M_PI * (f0 * t + (f1 - f0) * t * t / (2 * d));
    return sin(phase);
}
static double pulses(double t, int ch)
{
    (void)ch;
    // Five short taps per second, like footsteps.
    return fmod(t, 0.2) < 0.03 ? sin(2 * M_PI * 220 * t) : 0;
}

// Asynchronous sends: macOS completes each Bluetooth output report on its own
// schedule, so keep a few in flight instead of waiting for each one.
static bool use_async = false;
static int max_in_flight = 4;
static int in_flight = 0;
static unsigned async_completed = 0, async_failed = 0, async_skipped = 0;
static IOReturn async_first_error = kIOReturnSuccess;
static uint8_t async_buffers[64][MAX_REPORT_BYTES];
static unsigned async_slot = 0;

static void on_report_sent(void *context, IOReturn result, void *sender, IOHIDReportType type,
                           uint32_t reportID, uint8_t *report, CFIndex length)
{
    (void)context; (void)sender; (void)type; (void)reportID; (void)report; (void)length;
    in_flight--;
    async_completed++;
    if (result != kIOReturnSuccess) {
        if (!async_failed) async_first_error = result;
        async_failed++;
    }
}

// Wait with the precise Mach timer, then collect finished-send callbacks
// without blocking. A run-loop timeout as the wait can be coalesced by macOS
// for a background process, which showed up as ~100 ms stalls.
static void drain_callbacks(void)
{
    while (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) == kCFRunLoopRunHandledSource) {
    }
}

static void pump_until_ns(uint64_t deadline)
{
    drain_callbacks();
    if (now_ns() < deadline) sleep_until_ns(deadline);
    drain_callbacks();
}

static void play(IOHIDDeviceRef device, const char *name, waveform wave, double seconds,
                 double amplitude, uint8_t *sequence, struct stats *st)
{
    printf("  Playing: %s\n", name);
    fflush(stdout);
    unsigned reports = (unsigned)(seconds * SAMPLE_RATE / FRAMES_PER_REPORT);
    uint64_t deadline = now_ns();
    uint64_t frame = 0;
    for (unsigned r = 0; r < reports; r++) {
        int8_t pcm[2 * PCM_BYTES];
        for (int f = 0; f < FRAMES_PER_REPORT; f++, frame++) {
            double t = (double)frame / SAMPLE_RATE;
            for (int ch = 0; ch < 2; ch++) {
                double v = wave ? wave(t, ch) * amplitude * 127.0 : 0.0;
                pcm[f * 2 + ch] = (int8_t)lrint(fmax(-127.0, fmin(127.0, v)));
            }
        }
        uint8_t report[MAX_REPORT_BYTES];
        build_report(report, (*sequence)++, pcm);

        if (use_async) pump_until_ns(deadline); else sleep_until_ns(deadline);
        uint64_t start = now_ns();
        if (start > deadline + PERIOD_NS / 2) {
            st->late++;
            if (start - deadline > st->worst_late_ns) st->worst_late_ns = start - deadline;
        }
        IOReturn rc;
        if (use_async) {
            if (in_flight >= max_in_flight) {
                // The link is behind: drop this block rather than queue stale vibration.
                async_skipped++;
                rc = kIOReturnSuccess;
            } else {
                uint8_t *buffer = async_buffers[async_slot++ % 64];
                memcpy(buffer, report, report_bytes);
                rc = IOHIDDeviceSetReportWithCallback(device, kIOHIDReportTypeOutput, report_id,
                                                      buffer, (CFIndex)report_bytes, 0.2,
                                                      on_report_sent, NULL);
                if (rc == kIOReturnSuccess) in_flight++;
            }
        } else {
            rc = IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, report_id,
                                      report, (CFIndex)report_bytes);
        }
        uint64_t took = now_ns() - start;
        if (took > st->worst_call_ns) st->worst_call_ns = took;
        if (st->sent < MAX_SAMPLES) {
            call_ms[st->sent] = took / 1e6;
            late_ms[st->sent] = start > deadline ? (start - deadline) / 1e6 : 0;
        }
        st->sent++;
        if (rc != kIOReturnSuccess) {
            if (!st->failed) st->first_error = rc;
            st->failed++;
        }
        // Never burst to catch up after a stall: old vibration is worse than a gap.
        deadline += PERIOD_NS;
        uint64_t after = now_ns();
        if (deadline < after) deadline = after;
    }
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void print_percentiles(const char *label, double *values, unsigned count)
{
    if (count > MAX_SAMPLES) count = MAX_SAMPLES;
    if (!count) return;
    qsort(values, count, sizeof(*values), compare_double);
    double sum = 0;
    for (unsigned i = 0; i < count; i++) sum += values[i];
    printf("%s: average %.2f ms, middle %.2f ms, 95%% under %.2f ms, 99%% under %.2f ms, worst %.2f ms\n",
           label, sum / count, values[count / 2], values[count * 95 / 100],
           values[count * 99 / 100], values[count - 1]);
}

// High-priority but ordinary scheduling, as P5M uses. A Mach real-time policy
// gets demoted when a run-loop wait overruns its budget.
static bool raise_priority(void)
{
    return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
}

int main(int argc, char **argv)
{
    double amplitude = 0.8;
    if (argc > 1) amplitude = fmax(0.05, fmin(1.0, atof(argv[1])));
    use_async = !(argc > 2 && strcmp(argv[2], "sync") == 0);
    if (argc > 3) max_in_flight = (int)fmax(1, fmin(32, atoi(argv[3])));
    if (argc > 4 && strcmp(argv[4], "pack2") == 0) {
        report_id = 0x33;
        report_bytes = 206;
        blocks_per_report = 2;
    }
    mach_timebase_info(&timebase);

    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDDeviceRef device = find_dualsense(manager);
    if (!device) {
        printf("RESULT: NO CONTROLLER - no PS5 controller found. Pair it in Bluetooth settings, "
               "turn it on, and run this again.\n");
        return 2;
    }

    char product[128], transport[64];
    string_property(device, CFSTR(kIOHIDProductKey), product, sizeof(product));
    string_property(device, CFSTR(kIOHIDTransportKey), transport, sizeof(transport));
    long max_out = number_property(device, CFSTR(kIOHIDMaxOutputReportSizeKey));
    printf("Found: %s over %s (largest output report macOS allows: %ld bytes)\n",
           product, transport, max_out);

    if (strcasecmp(transport, "Bluetooth") != 0) {
        printf("RESULT: WRONG CONNECTION - the controller is connected over %s. Unplug the cable "
               "so it uses Bluetooth, then run this again.\n", transport);
        return 3;
    }
    if (max_out >= 0 && max_out < (long)report_bytes) {
        printf("RESULT: BLOCKED - macOS reports the controller only accepts %ld-byte output "
               "reports; the vibration report needs %zu.\n", max_out, report_bytes);
        return 4;
    }

    IOReturn rc = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (rc != kIOReturnSuccess) {
        printf("RESULT: BLOCKED - macOS refused to open the controller (IOReturn 0x%08x). "
               "If it says 'not permitted', allow your terminal under System Settings > "
               "Privacy & Security > Input Monitoring.\n", rc);
        return 5;
    }

    // Reading the calibration feature report switches a Bluetooth DualSense
    // into its full report mode, which SDL also does before sending effects.
    uint8_t calibration[64] = {0x05};
    CFIndex length = sizeof(calibration);
    rc = IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature, 0x05, calibration, &length);
    printf("Full report mode request: %s\n",
           rc == kIOReturnSuccess ? "ok" : "failed (continuing anyway)");

    if (use_async) {
        IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        printf("Send mode: without waiting, up to %d in flight\n", max_in_flight);
    } else {
        printf("Send mode: wait for each send\n");
    }
    printf("High-priority scheduling: %s\n", raise_priority() ? "on" : "unavailable");
    rc = send_full_power(device);
    printf("Full motor power setup: %s\n", rc == kIOReturnSuccess ? "ok" : "failed (continuing anyway)");
    printf("Hold the controller. Each pattern lasts 2 seconds, with a pause between.\n");
    struct stats st = {0};
    uint8_t sequence = 0;
    uint64_t run_start = now_ns();
    struct { const char *name; waveform wave; } tests[] = {
        {"1 of 5 - steady buzz, both sides", buzz_both},
        {"2 of 5 - LEFT side only", buzz_left},
        {"3 of 5 - RIGHT side only", buzz_right},
        {"4 of 5 - low-to-high sweep", sweep},
        {"5 of 5 - quick taps", pulses},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        play(device, tests[i].name, tests[i].wave, 2.0, amplitude, &sequence, &st);
        play(device, "(pause)", NULL, 0.8, 0, &sequence, &st);
    }

    uint64_t run_end = now_ns();
    if (use_async) {
        for (int i = 0; i < 30 && in_flight > 0; i++) pump_until_ns(now_ns() + 10000000ull);
        printf("\nWithout-waiting sends: %u finished, %u failed%s, %u dropped because %d were "
               "already waiting\n", async_completed, async_failed,
               async_failed ? " (see first error below)" : "", async_skipped, max_in_flight);
        if (async_failed) printf("First failure: 0x%08x\n", async_first_error);
        st.failed += async_failed;
    }
    printf("Wall time for the patterns: %.1f s (ideal 14.0 s)\n", (run_end - run_start) / 1e9);
    IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
    CFRelease(device);
    CFRelease(manager);

    printf("\n");
    print_percentiles("Time per send", call_ms, st.sent);
    print_percentiles("Lateness at send", late_ms, st.sent);
    printf("Reports sent: %u, rejected by macOS: %u", st.sent, st.failed);
    if (st.failed) printf(" (first error 0x%08x)", st.first_error);
    printf("\nReports sent more than 5 ms late: %u (worst %.1f ms late)\n",
           st.late, st.worst_late_ns / 1e6);
    printf("Slowest single send: %.1f ms (needs to stay under %.1f ms)\n",
           st.worst_call_ns / 1e6, PERIOD_NS / 1e6);
    if (use_async && async_skipped)
        printf("RESULT: SENT WITH PROBLEMS - %u of %u vibration blocks were dropped.\n",
               async_skipped, st.sent);
    else if (st.failed == st.sent)
        printf("RESULT: BLOCKED - macOS rejected every vibration report.\n");
    else if (st.failed || (!use_async && st.worst_call_ns > PERIOD_NS))
        printf("RESULT: SENT WITH PROBLEMS - see the numbers above.\n");
    else
        printf("RESULT: SENT CLEANLY - now say what you felt.\n");
    return 0;
}
