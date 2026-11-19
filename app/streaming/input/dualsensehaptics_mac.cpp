// DualSense waveform haptics over Bluetooth on macOS.
//
// A second IOKit handle beside SDL's sends output reports 0x33 (two blocks) or
// 0x32 (one block) of 3 kHz 8-bit samples, paced by the Mach timer. The rules
// for what fits through the link live in hapticscore.*, which is unit tested.
//
// Report layout and checksum: SAxense by Sdore (Egor Vorontsov),
// https://github.com/egormanga/SAxense. Motor-power setup report, controller
// queue depth, and sending without waiting: P5M for Mac,
// https://github.com/beecrepaldi-afk/P5M-mac. Measurements behind the defaults
// are in tools/ds5-haptics-probe/README.md.

#include "dualsensehaptics.h"
#include "hapticscore.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <mach/mach_time.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace DualSenseHaptics {

namespace {

constexpr long kSonyVid = 0x054c;
constexpr long kDualSensePid = 0x0ce6;
constexpr long kDualSenseEdgePid = 0x0df2;
constexpr int kMaxSlots = 16;
constexpr CFTimeInterval kSendTimeoutSec = 0.2;

mach_timebase_info_data_t timebase()
{
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return tb;
}

uint64_t nowNs()
{
    mach_timebase_info_data_t tb = timebase();
    return mach_absolute_time() * tb.numer / tb.denom;
}

void sleepUntilNs(uint64_t deadlineNs)
{
    mach_timebase_info_data_t tb = timebase();
    mach_wait_until(deadlineNs * tb.denom / tb.numer);
}

int envInt(const char* name, int fallback, int lo, int hi)
{
    const char* value = getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return std::clamp(atoi(value), lo, hi);
}

long numberProperty(IOHIDDeviceRef device, CFStringRef key)
{
    long value = -1;
    CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
    if (ref != nullptr && CFGetTypeID(ref) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)ref, kCFNumberLongType, &value);
    }
    return value;
}

std::string stringProperty(IOHIDDeviceRef device, CFStringRef key)
{
    char buffer[128] = {};
    CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
    if (ref != nullptr && CFGetTypeID(ref) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)ref, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    }
    return buffer;
}

// SDL reports "7c-66-ef-4e-0b-c2"; IOKit reports "7C:66:EF:4E:0B:C2".
std::string normalizeSerial(const char* serial)
{
    std::string out;
    for (const char* p = serial; p != nullptr && *p != '\0'; p++) {
        if (*p != ':' && *p != '-') {
            out.push_back((char)tolower((unsigned char)*p));
        }
    }
    return out;
}

class Player
{
public:
    Player(int slot, SDL_GameController* controller, IOHIDManagerRef manager, IOHIDDeviceRef device,
           const Ds5Haptics::GateConfig& config, bool hapticsFilter)
        : m_Slot(slot), m_Controller(controller), m_Manager(manager), m_Device(device), m_Gate(config),
          m_HapticsFilter(hapticsFilter)
    {
        m_Worker = std::thread([this] { run(); });
    }

    // Stops and joins the worker, so it never touches the controller afterwards.
    // Called by detach() on the thread that closes the controller.
    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Stopped = true;
        }
        m_Wake.notify_all();
        if (m_Worker.joinable() && m_Worker.get_id() != std::this_thread::get_id()) {
            m_Worker.join();
        }
    }

    ~Player()
    {
        stop();
        IOHIDDeviceClose(m_Device, kIOHIDOptionsTypeNone);
        CFRelease(m_Device);
        CFRelease(m_Manager);
    }

    void receive(uint32_t sequence, const uint8_t* pcm, uint16_t frames)
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_Stopped || m_Failed) {
                return;
            }
            m_Gate.receive(sequence, pcm, frames, nowNs());
        }
        m_Wake.notify_one();
    }

    bool playing()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return !m_Failed && m_Gate.playing();
    }

private:
    static void onReportSent(void* context, IOReturn result, void*, IOHIDReportType, uint32_t, uint8_t*, CFIndex)
    {
        Player* self = static_cast<Player*>(context);
        std::lock_guard<std::mutex> lock(self->m_Mutex);
        self->m_Gate.onSent(result == kIOReturnSuccess);
        if (result != kIOReturnSuccess && self->m_FirstError == kIOReturnSuccess) {
            self->m_FirstError = result;
        }
    }

    // Collect finished-send callbacks without blocking.
    static void drainCallbacks()
    {
        while (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) == kCFRunLoopRunHandledSource) {
        }
    }

    void startWaveform()
    {
        if (!m_PoweredUp) {
            // Full motor power, haptics filter per ML_DS5_HAPTICS_FILTER. Sent at the first waveform rather
            // than at attach, so a session without HD vibration leaves the pad alone.
            uint8_t report[Ds5Haptics::kFullPowerReportBytes];
            Ds5Haptics::buildFullPowerReport(report, m_HapticsFilter);
            IOReturn rc = IOHIDDeviceSetReport(m_Device, kIOHIDReportTypeOutput, report[0], report, sizeof(report));
            if (rc != kIOReturnSuccess) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "DualSense haptics: setup report failed on slot %d: 0x%08x",
                            m_Slot, rc);
            }
            m_PoweredUp = true;
        }
        // SDL's rumble emulation flags take the coils out of waveform mode, so
        // clear any rumble SDL remembers before the first report.
        SDL_GameControllerRumble(m_Controller, 0, 0, 0);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DualSense haptics: waveform started on slot %d", m_Slot);
    }

    void send(const uint8_t* report, size_t length)
    {
        uint8_t* buffer = m_Buffers[m_NextBuffer++ % m_Buffers.size()].data();
        memcpy(buffer, report, length);
        IOReturn rc = IOHIDDeviceSetReportWithCallback(m_Device, kIOHIDReportTypeOutput, buffer[0], buffer,
                                                       (CFIndex)length, kSendTimeoutSec, onReportSent, this);
        if (rc != kIOReturnSuccess) {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Gate.onSent(false);
            if (m_FirstError == kIOReturnSuccess) {
                m_FirstError = rc;
            }
            if (rc == kIOReturnNotOpen || rc == kIOReturnNoDevice || rc == kIOReturnNotAttached) {
                m_Failed = true;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "DualSense haptics: controller gone on slot %d (0x%08x); waveforms off until it reconnects",
                            m_Slot, rc);
            }
        }
    }

    void logStats(uint64_t now, bool force = false)
    {
        if (!force && now - m_LastLogNs < 1000000000ull) {
            return;
        }
        Ds5Haptics::GateStats s;
        IOReturn firstError;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            s = m_Gate.stats();
            firstError = m_FirstError;
        }
        const Ds5Haptics::GateStats& p = m_LoggedStats;
        if (s.packetsIn != p.packetsIn || s.reportsSent != p.reportsSent || s.reportsSkipped != p.reportsSkipped) {
            const double seconds = (now - m_LastLogNs) / 1e9;
            const uint64_t blocks = (s.reportsCompleted - p.reportsCompleted) * (uint64_t)m_Gate.config().blocksPerReport;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "DualSense haptics slot %d: %.0f blocks/s delivered | packets in %llu, late %llu, stale %llu, "
                        "overflow %llu, idle-silent %llu | reports sent %llu, done %llu, failed %llu, skipped %llu | "
                        "trimmed %llu frames | first error 0x%08x",
                        m_Slot, blocks / seconds,
                        (unsigned long long)(s.packetsIn - p.packetsIn),
                        (unsigned long long)(s.packetsOutOfOrder - p.packetsOutOfOrder),
                        (unsigned long long)(s.packetsStale - p.packetsStale),
                        (unsigned long long)(s.packetsOverflow - p.packetsOverflow),
                        (unsigned long long)(s.packetsSilentIdle - p.packetsSilentIdle),
                        (unsigned long long)(s.reportsSent - p.reportsSent),
                        (unsigned long long)(s.reportsCompleted - p.reportsCompleted),
                        (unsigned long long)(s.reportsFailed - p.reportsFailed),
                        (unsigned long long)(s.reportsSkipped - p.reportsSkipped),
                        (unsigned long long)(s.framesTrimmed - p.framesTrimmed),
                        firstError);
        }
        m_LoggedStats = s;
        m_LastLogNs = now;
    }

    void run()
    {
        pthread_setname_np("DS5 haptics");
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        IOHIDDeviceScheduleWithRunLoop(m_Device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

        uint8_t report[Ds5Haptics::kMaxReportBytes];
        uint64_t deadline = 0;
        bool active = false;
        m_LastLogNs = nowNs();

        for (;;) {
            drainCallbacks();
            logStats(nowNs());

            if (!active) {
                std::unique_lock<std::mutex> lock(m_Mutex);
                if (m_Stopped || m_Failed) {
                    break;
                }
                if (!m_Gate.hasWork()) {
                    // Wake periodically while reports are still in flight so their
                    // completions are collected; otherwise sleep until receive().
                    if (m_Gate.inFlight() > 0) {
                        m_Wake.wait_for(lock, std::chrono::milliseconds(10));
                    }
                    else {
                        m_Wake.wait_for(lock, std::chrono::seconds(1));
                    }
                    continue;
                }
                active = true;
                deadline = nowNs();
            }

            sleepUntilNs(deadline);
            drainCallbacks();

            Ds5Haptics::TickResult result;
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                if (m_Stopped || m_Failed) {
                    break;
                }
                result = m_Gate.tick(nowNs(), report);
            }

            if (result.waveformStarted) {
                startWaveform();
            }
            if (result.action == Ds5Haptics::TickAction::Send) {
                send(report, result.length);
            }
            if (result.action == Ds5Haptics::TickAction::Idle || result.waveformEnded) {
                active = false;
                continue;
            }

            // Never burst to catch up after a stall: old vibration is worse than a gap.
            deadline = std::max(deadline + m_Gate.periodNs(), nowNs());
        }

        // Let in-flight reports finish before the buffers and device go away.
        const uint64_t giveUp = nowNs() + 300000000ull;
        for (;;) {
            drainCallbacks();
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                if (m_Gate.inFlight() == 0) {
                    break;
                }
            }
            if (nowNs() > giveUp) {
                break;
            }
            sleepUntilNs(nowNs() + 2000000ull);
        }
        logStats(nowNs(), true);
        IOHIDDeviceUnscheduleFromRunLoop(m_Device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }

    const int m_Slot;
    SDL_GameController* const m_Controller;
    IOHIDManagerRef m_Manager;
    IOHIDDeviceRef m_Device;

    std::mutex m_Mutex;
    std::condition_variable m_Wake;
    Ds5Haptics::Gate m_Gate;
    const bool m_HapticsFilter;
    bool m_Stopped = false;
    bool m_Failed = false;
    IOReturn m_FirstError = kIOReturnSuccess;

    // Worker-only state.
    bool m_PoweredUp = false;
    std::array<std::array<uint8_t, Ds5Haptics::kMaxReportBytes>, 16> m_Buffers {};
    size_t m_NextBuffer = 0;
    uint64_t m_LastLogNs = 0;
    Ds5Haptics::GateStats m_LoggedStats;

    std::thread m_Worker;
};

std::mutex g_PlayersMutex;
std::array<std::shared_ptr<Player>, kMaxSlots> g_Players;

// Finds the Bluetooth HID device behind an SDL controller: by SDL's device path
// ("DevSrvsID:<registry entry ID>"), then by serial number, then the only
// DualSense connected. Returns a retained, unopened device or null.
IOHIDDeviceRef findDevice(IOHIDManagerRef manager, SDL_GameController* controller, std::string& why)
{
    IOHIDManagerSetDeviceMatching(manager, nullptr);
    CFSetRef set = IOHIDManagerCopyDevices(manager);
    if (set == nullptr) {
        why = "no HID devices";
        return nullptr;
    }
    CFIndex count = CFSetGetCount(set);
    std::unique_ptr<IOHIDDeviceRef[]> devices(new IOHIDDeviceRef[(size_t)count]);
    CFSetGetValues(set, (const void**)devices.get());

    const char* path = SDL_GameControllerPath(controller);
    uint64_t wantedId = 0;
    if (path != nullptr && strncmp(path, "DevSrvsID:", 10) == 0) {
        wantedId = strtoull(path + 10, nullptr, 10);
    }
    const std::string wantedSerial = normalizeSerial(SDL_GameControllerGetSerial(controller));

    IOHIDDeviceRef byId = nullptr, bySerial = nullptr, only = nullptr;
    int candidates = 0;
    for (CFIndex i = 0; i < count; i++) {
        IOHIDDeviceRef d = devices[i];
        long pid = numberProperty(d, CFSTR(kIOHIDProductIDKey));
        if (numberProperty(d, CFSTR(kIOHIDVendorIDKey)) != kSonyVid ||
            (pid != kDualSensePid && pid != kDualSenseEdgePid) ||
            numberProperty(d, CFSTR(kIOHIDPrimaryUsagePageKey)) != 1 ||
            numberProperty(d, CFSTR(kIOHIDPrimaryUsageKey)) != 5) {
            continue;
        }
        candidates++;
        only = d;
        uint64_t id = 0;
        if (wantedId != 0 && IORegistryEntryGetRegistryEntryID(IOHIDDeviceGetService(d), &id) == KERN_SUCCESS &&
            id == wantedId) {
            byId = d;
        }
        if (!wantedSerial.empty() &&
            normalizeSerial(stringProperty(d, CFSTR(kIOHIDSerialNumberKey)).c_str()) == wantedSerial) {
            bySerial = d;
        }
    }

    IOHIDDeviceRef found = byId ? byId : bySerial ? bySerial : candidates == 1 ? only : nullptr;
    why = byId ? "device path" : bySerial ? "serial number" : candidates == 1 ? "only DualSense" : "no match";
    if (found != nullptr) {
        CFRetain(found);
    }
    CFRelease(set);
    return found;
}

}  // namespace

bool enabled()
{
    const char* value = getenv("ML_DS5_HAPTICS");
    return value == nullptr || strcmp(value, "0") != 0;
}

bool attach(int slot, SDL_GameController* controller)
{
    if (!enabled() || slot < 0 || slot >= kMaxSlots || controller == nullptr ||
        SDL_GameControllerGetType(controller) != SDL_CONTROLLER_TYPE_PS5) {
        return false;
    }
    detach(slot);

    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (manager == nullptr) {
        return false;
    }
    std::string why;
    IOHIDDeviceRef device = findDevice(manager, controller, why);
    if (device == nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DualSense haptics: no HID device for slot %d (%s, SDL path %s)",
                    slot, why.c_str(), SDL_GameControllerPath(controller) ? SDL_GameControllerPath(controller) : "none");
        CFRelease(manager);
        return false;
    }

    std::string transport = stringProperty(device, CFSTR(kIOHIDTransportKey));
    if (transport != "Bluetooth") {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "DualSense haptics: slot %d is on %s; only Bluetooth is supported",
                    slot, transport.empty() ? "an unknown transport" : transport.c_str());
        CFRelease(device);
        CFRelease(manager);
        return false;
    }

    IOReturn rc = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (rc != kIOReturnSuccess) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "DualSense haptics: opening slot %d failed: 0x%08x", slot, rc);
        CFRelease(device);
        CFRelease(manager);
        return false;
    }

    Ds5Haptics::GateConfig config;
    config.blocksPerReport = envInt("ML_DS5_HAPTICS_PACK", 2, 1, 2);
    config.maxInFlight = envInt("ML_DS5_HAPTICS_INFLIGHT", 2, 1, 8);
    // 0 turns off the controller's own haptics filter, which softens sharp detail.
    const bool hapticsFilter = envInt("ML_DS5_HAPTICS_FILTER", 1, 0, 1) != 0;

    auto player = std::make_shared<Player>(slot, controller, manager, device, config, hapticsFilter);
    {
        std::lock_guard<std::mutex> lock(g_PlayersMutex);
        g_Players[(size_t)slot] = player;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "DualSense Bluetooth waveform haptics ready for slot %d (matched by %s; %d block(s) per report, "
                "%d in flight, filter %s)",
                slot, why.c_str(), config.blocksPerReport, config.maxInFlight, hapticsFilter ? "on" : "off");
    return true;
}

void detach(int slot)
{
    if (slot < 0 || slot >= kMaxSlots) {
        return;
    }
    std::shared_ptr<Player> player;
    {
        std::lock_guard<std::mutex> lock(g_PlayersMutex);
        player.swap(g_Players[(size_t)slot]);
    }
    // Outside the lock. A receive() still holding a reference only touches the
    // gate, which ignores packets once stopped.
    if (player) {
        player->stop();
    }
}

bool playing(int slot)
{
    if (slot < 0 || slot >= kMaxSlots) {
        return false;
    }
    std::shared_ptr<Player> player;
    {
        std::lock_guard<std::mutex> lock(g_PlayersMutex);
        player = g_Players[(size_t)slot];
    }
    return player && player->playing();
}

void receive(uint16_t controllerNumber, uint32_t sequence, const uint8_t* pcm, uint16_t frames)
{
    if (controllerNumber >= kMaxSlots) {
        return;
    }
    std::shared_ptr<Player> player;
    {
        std::lock_guard<std::mutex> lock(g_PlayersMutex);
        player = g_Players[controllerNumber];
    }
    if (player) {
        player->receive(sequence, pcm, frames);
    }
}

}  // namespace DualSenseHaptics
