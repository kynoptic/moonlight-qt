#pragma once

// Platform-free core of the DualSense Bluetooth waveform haptics player: turns
// Vibeshine's 48 kHz haptics packets into the controller's 3 kHz output reports
// and decides, tick by tick, what fits through the Bluetooth link.
//
// No threads, clocks, SDL, or IOKit here. Callers pass the time in and serialize
// access, so every rule can be tested by enumerating its inputs.
//
// Report layout and checksum follow the SAxense research by Sdore (Egor
// Vorontsov), https://github.com/egormanga/SAxense; the two-block layout, the
// motor-power setup report, and the controller queue depth follow P5M for Mac
// (https://github.com/beecrepaldi-afk/P5M-mac). The gate rules follow Nonary's
// Moonlight fork (https://github.com/Nonary/moonlight-qt).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace Ds5Haptics {

constexpr int kInputRate = 48000;
constexpr int kOutputRate = 3000;
constexpr int kDecimation = kInputRate / kOutputRate;   // 16
constexpr int kBlockFrames = 32;                         // stereo frames per report block
constexpr int kBlockBytes = kBlockFrames * 2;            // signed 8-bit, interleaved L/R
constexpr uint64_t kBlockNs = 1000000000ull * kBlockFrames / kOutputRate;  // 10.67 ms
constexpr int kControllerQueue = 16;  // frames the controller buffers before playing
constexpr size_t kMaxReportBytes = 206;
constexpr size_t kFullPowerReportBytes = 78;

// CRC-32 the controller expects: standard CRC-32 over the report, seeded as if
// the Bluetooth HID output header byte 0xA2 came first.
uint32_t reportCrc(const uint8_t* data, size_t size);

// Size of a waveform report with 1 block (0x32) or 2 blocks (0x33); 0 otherwise.
size_t reportBytes(int blocks);

// Builds and signs a waveform report from blocks * kBlockBytes samples.
// Returns its size, or 0 if blocks is not 1 or 2 or capacity is too small.
size_t buildReport(uint8_t* out, size_t capacity, int blocks, uint8_t sequence, const int8_t* pcm);

// Builds and signs setup report 0x31: full motor power, the controller's haptics
// filter on or off, and only those fields marked valid, so rumble emulation stays
// off. The filter field is always marked valid, so "off" is applied, not skipped.
void buildFullPowerReport(uint8_t out[kFullPowerReportBytes], bool hapticsFilter = true);

// 48 kHz signed 16-bit stereo in, 3 kHz signed 8-bit stereo out: each output
// frame is the average of 16 input frames, scaled to 8 bits. A partial group is
// carried across calls until reset().
class Decimator {
public:
    void reset();
    // pcm is frames * 2 little-endian S16 samples (the wire format).
    void push(const uint8_t* pcm, size_t frames, std::vector<int8_t>& out);

private:
    int32_t m_Sum[2] = {0, 0};
    int m_Count = 0;
};

struct GateConfig {
    int blocksPerReport = 2;
    int maxInFlight = 2;
    uint64_t staleNs = 40000000;          // drop chunks older than this when pulled
    uint64_t idleNs = 60000000;           // stop after this long without a non-zero sample
    uint64_t sequenceResetNs = 1000000000; // accept any sequence after this long without packets
    size_t queueLimit = 8;                // packets waiting for the worker
};

struct GateStats {
    uint64_t packetsIn = 0;
    uint64_t packetsOutOfOrder = 0;   // late or duplicate, dropped on arrival
    uint64_t packetsSilentIdle = 0;   // all-zero while idle, dropped on arrival
    uint64_t packetsOverflow = 0;     // queue full, cleared
    uint64_t packetsStale = 0;        // too old when the worker pulled them
    uint64_t framesTrimmed = 0;       // oldest 3 kHz frames dropped to keep the newest
    uint64_t reportsSent = 0;
    uint64_t reportsSkipped = 0;      // link busy at a deadline: that slice is dropped
    uint64_t reportsCompleted = 0;
    uint64_t reportsFailed = 0;
};

enum class TickAction {
    Idle,   // nothing to play; the worker can sleep until receive()
    Wait,   // playing, but not a full report buffered yet
    Skip,   // a report was due but the link is full; its samples were dropped
    Send,   // send the report written to the output buffer
};

struct TickResult {
    TickAction action = TickAction::Idle;
    size_t length = 0;            // bytes written when action == Send
    bool waveformStarted = false; // first tick of a waveform: mute ordinary rumble now
    bool waveformEnded = false;   // this tick ended the waveform (a final silent report may be sent)
};

// The gate rules: send nothing during silence, newest wins (never play late),
// cap reports in flight, and report whether a waveform is playing so ordinary
// rumble can be muted meanwhile.
class Gate {
public:
    explicit Gate(const GateConfig& config = GateConfig());

    // Network side. pcm is frames * 4 bytes (S16LE stereo), already validated.
    void receive(uint32_t sequence, const uint8_t* pcm, uint16_t frames, uint64_t nowNs);

    // Worker side, called at each report deadline. out must hold kMaxReportBytes.
    TickResult tick(uint64_t nowNs, uint8_t* out);

    // A report handed out by tick() finished sending.
    void onSent(bool success);

    // True while a waveform is playing or waiting to play.
    bool playing() const;
    // True if the worker has something to do (it may sleep when false).
    bool hasWork() const;

    uint64_t periodNs() const { return kBlockNs * (uint64_t)m_Config.blocksPerReport; }
    int inFlight() const { return m_InFlight; }
    const GateConfig& config() const { return m_Config; }
    const GateStats& stats() const { return m_Stats; }

private:
    struct Chunk {
        uint32_t sequence;
        uint64_t receivedNs;
        std::vector<uint8_t> pcm;
        uint16_t frames;
    };

    void pull(uint64_t nowNs);
    void endWaveform();

    GateConfig m_Config;
    GateStats m_Stats;
    std::deque<Chunk> m_Queue;
    Decimator m_Decimator;
    std::deque<int8_t> m_Fifo;  // 3 kHz interleaved samples ready to send

    bool m_HaveInputSequence = false;
    uint32_t m_LatestInputSequence = 0;
    uint64_t m_LastPacketNs = 0;

    bool m_HaveExpected = false;
    uint32_t m_ExpectedSequence = 0;

    bool m_Active = false;
    uint64_t m_LastNonzeroNs = 0;
    uint8_t m_ReportSequence = 0;
    int m_InFlight = 0;
};

}  // namespace Ds5Haptics
