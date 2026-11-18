// Drives the app's real DualSense haptics player (app/streaming/input/
// dualsensehaptics_mac.cpp) with made-up Vibeshine packets, no PC or stream
// involved: SDL opens the controller the way Moonlight does, then 240-frame
// 48 kHz packets arrive every 5 ms, as from the host.
//
// Patterns, two seconds each with silence between: both sides, left only, right
// only, a sweep, taps, and "second block only", which carries signal only in the
// second block of each two-block report. If that last one is silent, the
// controller ignores the second block and the player must use ML_DS5_HAPTICS_PACK=1.
//
// Build from the repository root (Moonlight closed while it runs):
//   clang++ -std=c++17 -O2 -DHAVE_DS5_HAPTICS -Iapp/streaming/input -Ilibs/mac/include/SDL2 \
//     tools/ds5-haptics-probe/haptics_harness.cpp app/streaming/input/hapticscore.cpp \
//     app/streaming/input/dualsensehaptics_mac.cpp -Llibs/mac/lib -lSDL2 \
//     -framework IOKit -framework CoreFoundation -o temp/haptics_harness
// Run:
//   DYLD_LIBRARY_PATH=libs/mac/lib temp/haptics_harness [strength 0.05-1.0]

#include "dualsensehaptics.h"
#include "hapticscore.h"

#include <SDL.h>
#include <mach/mach_time.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kPacketFrames = 240;  // 5 ms at 48 kHz
constexpr int kRate = 48000;

mach_timebase_info_data_t g_Timebase;

uint64_t nowNs()
{
    return mach_absolute_time() * g_Timebase.numer / g_Timebase.denom;
}

void sleepUntilNs(uint64_t deadline)
{
    mach_wait_until(deadline * g_Timebase.denom / g_Timebase.numer);
}

typedef double (*Waveform)(double t, long frame, int channel);

double both(double t, long, int) { return sin(2 * M_PI * 160 * t); }
double left(double t, long, int ch) { return ch == 0 ? sin(2 * M_PI * 160 * t) : 0; }
double right(double t, long, int ch) { return ch == 1 ? sin(2 * M_PI * 160 * t) : 0; }
double sweep(double t, long, int)
{
    const double f0 = 40, f1 = 400, d = 2.0;
    return sin(2 * M_PI * (f0 * t + (f1 - f0) * t * t / (2 * d)));
}
double taps(double t, long, int) { return fmod(t, 0.2) < 0.03 ? sin(2 * M_PI * 220 * t) : 0; }
// 48 kHz frame -> 3 kHz frame -> 32-frame block; odd blocks are a report's second.
double secondBlockOnly(double t, long frame, int)
{
    long block = (frame / Ds5Haptics::kDecimation) / Ds5Haptics::kBlockFrames;
    return (block % 2) ? sin(2 * M_PI * 160 * t) : 0;
}

uint32_t g_Sequence = 1;

void play(const char* name, Waveform wave, double seconds, double strength)
{
    printf("Playing: %s\n", name);
    fflush(stdout);
    std::vector<uint8_t> pcm(kPacketFrames * 4);
    const long packets = (long)(seconds * kRate / kPacketFrames);
    uint64_t deadline = nowNs();
    long frame = 0;
    for (long p = 0; p < packets; p++) {
        for (int i = 0; i < kPacketFrames; i++, frame++) {
            const double t = (double)frame / kRate;
            for (int ch = 0; ch < 2; ch++) {
                const int16_t s = wave ? (int16_t)lrint(wave(t, frame, ch) * strength * 32767) : 0;
                pcm[i * 4 + ch * 2] = (uint8_t)((uint16_t)s & 0xff);
                pcm[i * 4 + ch * 2 + 1] = (uint8_t)((uint16_t)s >> 8);
            }
        }
        sleepUntilNs(deadline);
        DualSenseHaptics::receive(0, g_Sequence++, pcm.data(), kPacketFrames);
        deadline += 5000000ull;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    mach_timebase_info(&g_Timebase);
    const double strength = argc > 1 ? std::fmin(1.0, std::fmax(0.05, atof(argv[1]))) : 0.8;

    // Same rumble hint Moonlight sets before opening controllers.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_LogSetAllPriority(SDL_LOG_PRIORITY_INFO);
    SDL_Delay(500);
    SDL_PumpEvents();

    SDL_GameController* controller = nullptr;
    for (int i = 0; i < SDL_NumJoysticks() && controller == nullptr; i++) {
        if (SDL_IsGameController(i) && SDL_GameControllerTypeForIndex(i) == SDL_CONTROLLER_TYPE_PS5) {
            controller = SDL_GameControllerOpen(i);
        }
    }
    if (controller == nullptr) {
        fprintf(stderr, "No DualSense found. Pair it over Bluetooth and quit Moonlight first.\n");
        return 1;
    }
    printf("SDL path: %s\nSDL serial: %s\n",
           SDL_GameControllerPath(controller) ? SDL_GameControllerPath(controller) : "(none)",
           SDL_GameControllerGetSerial(controller) ? SDL_GameControllerGetSerial(controller) : "(none)");

    if (!DualSenseHaptics::attach(0, controller)) {
        fprintf(stderr, "attach() refused the controller; see the log line above.\n");
        SDL_GameControllerClose(controller);
        return 1;
    }

    struct { const char* name; Waveform wave; } patterns[] = {
        {"both sides", both},
        {"left only", left},
        {"right only", right},
        {"sweep 40-400 Hz", sweep},
        {"taps", taps},
        {"second block only (silent means the controller ignores it)", secondBlockOnly},
    };
    for (const auto& p : patterns) {
        play(p.name, p.wave, 2.0, strength);
        play("(silence)", nullptr, 0.7, strength);
        SDL_PumpEvents();
    }

    DualSenseHaptics::detach(0);
    SDL_GameControllerClose(controller);
    SDL_Quit();
    printf("Done.\n");
    return 0;
}
