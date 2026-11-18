// Tests for app/streaming/input/hapticscore.*: report layout and checksum, the
// 48 kHz to 3 kHz decimator, the wire validator, and each gate rule.
//
// Expected checksums come from Python's zlib, not from this code:
//   zlib.crc32(b'\xa2' + report_without_crc)
// computed over reports laid out by hand from the SAxense / P5M spec.

#include <QtTest>

#include "ControllerHaptics.h"
#include "hapticscore.h"

#include <cstring>

using namespace Ds5Haptics;

namespace {

constexpr uint64_t kMs = 1000000;

uint32_t crcField(const uint8_t* report, size_t size)
{
    const uint8_t* p = report + size - 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// S16LE stereo wire samples, every frame set to the same left/right values.
std::vector<uint8_t> wirePcm(size_t frames, int16_t left, int16_t right)
{
    std::vector<uint8_t> pcm(frames * 4);
    for (size_t i = 0; i < frames; i++) {
        pcm[i * 4 + 0] = (uint8_t)((uint16_t)left & 0xff);
        pcm[i * 4 + 1] = (uint8_t)((uint16_t)left >> 8);
        pcm[i * 4 + 2] = (uint8_t)((uint16_t)right & 0xff);
        pcm[i * 4 + 3] = (uint8_t)((uint16_t)right >> 8);
    }
    return pcm;
}

// A full Vibeshine packet: 240 frames at 48 kHz (5 ms), so 15 frames at 3 kHz.
void feed(Gate& gate, uint32_t sequence, int16_t value, uint64_t nowNs, uint16_t frames = 240)
{
    std::vector<uint8_t> pcm = wirePcm(frames, value, value);
    gate.receive(sequence, pcm.data(), frames, nowNs);
}

std::vector<uint8_t> wirePacket(uint8_t version, uint8_t format, uint16_t controller,
                                uint16_t frames, uint16_t reserved, size_t extra = 0)
{
    std::vector<uint8_t> p(ML_HAPTICS_HEADER_SIZE + (size_t)frames * ML_HAPTICS_FRAME_BYTES + extra);
    p[0] = version;
    p[1] = format;
    p[2] = (uint8_t)controller; p[3] = (uint8_t)(controller >> 8);
    p[4] = 7; // sequence
    p[8] = (uint8_t)frames; p[9] = (uint8_t)(frames >> 8);
    p[10] = (uint8_t)reserved; p[11] = (uint8_t)(reserved >> 8);
    return p;
}

int8_t pcmAt(const uint8_t* report, int blocks, int index)
{
    return (int8_t)report[(blocks == 2 ? 12 : 13) + index];
}

}  // namespace

class TestHapticsCore : public QObject
{
    Q_OBJECT

private slots:
    // ---- Checksum ----
    void crcMatchesZlibOverA2Prefix()
    {
        const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
        QCOMPARE(reportCrc(hello, sizeof(hello)), 0x152440ddu);
        QCOMPARE(reportCrc(nullptr, 0), 0xeada2d49u);
    }

    // ---- Report builder ----
    void reportSizes()
    {
        QCOMPARE(reportBytes(1), (size_t)142);
        QCOMPARE(reportBytes(2), (size_t)206);
        QCOMPARE(reportBytes(0), (size_t)0);
        QCOMPARE(reportBytes(3), (size_t)0);
    }

    void oneBlockReportLayout()
    {
        int8_t pcm[kBlockBytes];
        for (int i = 0; i < kBlockBytes; i++) pcm[i] = (int8_t)(i - 32);
        uint8_t r[kMaxReportBytes];
        memset(r, 0xcc, sizeof(r));
        QCOMPARE(buildReport(r, sizeof(r), 1, 5, pcm), (size_t)142);
        const uint8_t header[] = {0x32, 0x50, 0x91, 7, 0xfe, 16, 16, 16, 16, 16, 5, 0x92, 64};
        QCOMPARE(memcmp(r, header, sizeof(header)), 0);
        QCOMPARE(memcmp(r + 13, pcm, kBlockBytes), 0);
        for (int i = 13 + kBlockBytes; i < 138; i++) QCOMPARE(r[i], (uint8_t)0);
        QCOMPARE(crcField(r, 142), 0x31a95524u);
    }

    void twoBlockReportLayout()
    {
        int8_t pcm[2 * kBlockBytes];
        for (int i = 0; i < 2 * kBlockBytes; i++) pcm[i] = (int8_t)(((i * 3) % 256) - 128);
        uint8_t r[kMaxReportBytes];
        memset(r, 0xcc, sizeof(r));
        QCOMPARE(buildReport(r, sizeof(r), 2, 3, pcm), (size_t)206);
        const uint8_t header[] = {0x33, 0x30, 0x91, 6, 0x7e, 16, 16, 16, 16, 6, 0xd2, 64};
        QCOMPARE(memcmp(r, header, sizeof(header)), 0);
        QCOMPARE(memcmp(r + 12, pcm, 2 * kBlockBytes), 0);
        for (int i = 12 + 2 * kBlockBytes; i < 202; i++) QCOMPARE(r[i], (uint8_t)0);
        QCOMPARE(crcField(r, 206), 0x3c61ded2u);

        // The sequence wraps in both counters.
        QCOMPARE(buildReport(r, sizeof(r), 2, 200, pcm), (size_t)206);
        QCOMPARE(r[1], (uint8_t)0x80);
        QCOMPARE(r[9], (uint8_t)144);
        QCOMPARE(crcField(r, 206), 0xaa3fb2afu);
    }

    void reportBuilderRejectsBadInput()
    {
        int8_t pcm[2 * kBlockBytes] = {};
        uint8_t r[kMaxReportBytes];
        QCOMPARE(buildReport(r, sizeof(r), 0, 0, pcm), (size_t)0);
        QCOMPARE(buildReport(r, sizeof(r), 3, 0, pcm), (size_t)0);
        QCOMPARE(buildReport(r, 141, 1, 0, pcm), (size_t)0);
        QCOMPARE(buildReport(r, 205, 2, 0, pcm), (size_t)0);
    }

    void fullPowerReport()
    {
        uint8_t r[kFullPowerReportBytes];
        memset(r, 0xcc, sizeof(r));
        buildFullPowerReport(r);
        for (size_t i = 0; i < kFullPowerReportBytes - 4; i++) {
            uint8_t expected = 0;
            if (i == 0) expected = 0x31;
            else if (i == 2) expected = 0x10;
            else if (i == 4) expected = 0x62;
            else if (i == 42) expected = 0x01;
            QCOMPARE(r[i], expected);
        }
        QCOMPARE(crcField(r, kFullPowerReportBytes), 0xa25a0779u);
    }

    // ---- Decimator ----
    void decimatorAveragesSixteenToOne()
    {
        Decimator d;
        std::vector<int8_t> out;
        std::vector<uint8_t> in = wirePcm(16, 4096, -4096);
        d.push(in.data(), 16, out);
        QCOMPARE(out.size(), (size_t)2);
        QCOMPARE(out[0], (int8_t)16);
        QCOMPARE(out[1], (int8_t)-16);

        // Alternating 0 and 512 averages to 256, which is 1 in 8 bits.
        std::vector<uint8_t> alt(16 * 4);
        for (int i = 0; i < 16; i++) {
            int16_t v = (i % 2) ? 512 : 0;
            alt[i * 4 + 0] = alt[i * 4 + 2] = (uint8_t)(v & 0xff);
            alt[i * 4 + 1] = alt[i * 4 + 3] = (uint8_t)(v >> 8);
        }
        out.clear();
        d.push(alt.data(), 16, out);
        QCOMPARE(out.size(), (size_t)2);
        QCOMPARE(out[0], (int8_t)1);
    }

    void decimatorFullScale()
    {
        Decimator d;
        std::vector<int8_t> out;
        std::vector<uint8_t> in = wirePcm(16, 32767, -32768);
        d.push(in.data(), 16, out);
        QCOMPARE(out[0], (int8_t)127);
        QCOMPARE(out[1], (int8_t)-128);
    }

    void decimatorCarriesPartialGroups()
    {
        Decimator d;
        std::vector<int8_t> out;
        std::vector<uint8_t> in = wirePcm(15, 2048, 2048);
        d.push(in.data(), 15, out);
        QCOMPARE(out.size(), (size_t)0);
        d.push(in.data(), 1, out);
        QCOMPARE(out.size(), (size_t)2);
        QCOMPARE(out[0], (int8_t)8);
        out.clear();
        std::vector<uint8_t> two = wirePcm(32, 2048, 2048);
        d.push(two.data(), 32, out);
        QCOMPARE(out.size(), (size_t)4);
    }

    void decimatorResetDiscardsPartialGroup()
    {
        Decimator d;
        std::vector<int8_t> out;
        std::vector<uint8_t> a = wirePcm(8, 16384, 16384);
        std::vector<uint8_t> b = wirePcm(16, 2048, 2048);
        d.push(a.data(), 8, out);
        d.reset();
        d.push(b.data(), 16, out);
        QCOMPARE(out.size(), (size_t)2);
        QCOMPARE(out[0], (int8_t)8);  // not (8*16384 + 8*2048)/16 >> 8 = 36
    }

    // ---- Wire validator (moonlight-common-c/haptics/ControllerHaptics.h) ----
    void validatorAcceptsWellFormedPackets()
    {
        std::vector<uint8_t> p = wirePacket(1, 1, 0, 240, 0);
        QVERIFY(MlHapticsValidate(p.data(), p.size()));
        p = wirePacket(1, 1, 15, 1, 0);
        QVERIFY(MlHapticsValidate(p.data(), p.size()));
    }

    void validatorRejectsMalformedPackets()
    {
        std::vector<uint8_t> p;
        p = wirePacket(2, 1, 0, 240, 0);  QVERIFY(!MlHapticsValidate(p.data(), p.size()));
        p = wirePacket(1, 2, 0, 240, 0);  QVERIFY(!MlHapticsValidate(p.data(), p.size()));
        p = wirePacket(1, 1, 16, 240, 0); QVERIFY(!MlHapticsValidate(p.data(), p.size()));
        p = wirePacket(1, 1, 0, 240, 1);  QVERIFY(!MlHapticsValidate(p.data(), p.size()));
        p = wirePacket(1, 1, 0, 0, 0);    QVERIFY(!MlHapticsValidate(p.data(), p.size()));
        p = wirePacket(1, 1, 0, 241, 0);  QVERIFY(!MlHapticsValidate(p.data(), p.size()));
        p = wirePacket(1, 1, 0, 240, 0, 1);
        QVERIFY(!MlHapticsValidate(p.data(), p.size()));
        p = wirePacket(1, 1, 0, 240, 0);
        QVERIFY(!MlHapticsValidate(p.data(), p.size() - 1));
        QVERIFY(!MlHapticsValidate(p.data(), ML_HAPTICS_HEADER_SIZE - 1));
        QVERIFY(!MlHapticsValidate(nullptr, p.size()));
    }

    // ---- Gate: send nothing during silence ----
    void silenceWhileIdleIsDropped()
    {
        Gate gate;
        uint8_t r[kMaxReportBytes];
        for (uint32_t s = 1; s <= 5; s++) feed(gate, s, 0, s * 5 * kMs);
        QCOMPARE(gate.stats().packetsSilentIdle, (uint64_t)5);
        QVERIFY(!gate.playing());
        QVERIFY(!gate.hasWork());
        QCOMPARE(gate.tick(30 * kMs, r).action, TickAction::Idle);
        QCOMPARE(gate.stats().reportsSent, (uint64_t)0);
    }

    void nonzeroStartsWaveformAndMutesRumble()
    {
        Gate gate;
        uint8_t r[kMaxReportBytes];
        QVERIFY(!gate.playing());
        feed(gate, 1, 4096, 0);
        QVERIFY(gate.playing());
        QVERIFY(gate.hasWork());

        // 15 frames buffered, 64 needed: wait, but the waveform has started.
        TickResult t = gate.tick(0, r);
        QCOMPARE(t.action, TickAction::Wait);
        QVERIFY(t.waveformStarted);

        for (uint32_t s = 2; s <= 5; s++) feed(gate, s, 4096, 1 * kMs);
        t = gate.tick(2 * kMs, r);
        QCOMPARE(t.action, TickAction::Send);
        QVERIFY(!t.waveformStarted);
        QCOMPARE(t.length, (size_t)206);
        for (int i = 0; i < 2 * kBlockBytes; i++) QCOMPARE(pcmAt(r, 2, i), (int8_t)16);
        QCOMPARE(gate.inFlight(), 1);
        QVERIFY(gate.playing());
    }

    void silentPacketsWhileActiveKeepTheTimeline()
    {
        Gate gate;
        uint8_t r[kMaxReportBytes];
        feed(gate, 1, 4096, 0);
        gate.tick(0, r);
        feed(gate, 2, 0, 5 * kMs);
        QCOMPARE(gate.stats().packetsSilentIdle, (uint64_t)0);
        QVERIFY(gate.playing());
    }

    void oneBlockModeSendsOneBlockReports()
    {
        GateConfig c;
        c.blocksPerReport = 1;
        Gate gate(c);
        uint8_t r[kMaxReportBytes];
        QCOMPARE(gate.periodNs(), kBlockNs);
        feed(gate, 1, 4096, 0);
        feed(gate, 2, 4096, 0);
        feed(gate, 3, 4096, 0);
        TickResult t = gate.tick(0, r);
        QCOMPARE(t.action, TickAction::Send);
        QCOMPARE(t.length, (size_t)142);
        QCOMPARE(r[0], (uint8_t)0x32);
    }

    void reportSequenceAdvancesPerSend()
    {
        GateConfig c;
        c.maxInFlight = 4;
        c.queueLimit = 32;
        Gate gate(c);
        uint8_t r[kMaxReportBytes];
        for (uint32_t s = 1; s <= 9; s++) feed(gate, s, 4096, 0);
        QCOMPARE(gate.tick(0, r).action, TickAction::Send);
        QCOMPARE(r[1], (uint8_t)0x00);
        QCOMPARE(gate.tick(1 * kMs, r).action, TickAction::Send);
        QCOMPARE(r[1], (uint8_t)0x10);
        QCOMPARE(r[9], (uint8_t)2);
    }

    // ---- Gate: stream filter ----
    void lateAndDuplicatePacketsAreDropped()
    {
        Gate gate;
        feed(gate, 10, 4096, 0);
        feed(gate, 9, 4096, 1 * kMs);
        feed(gate, 10, 4096, 2 * kMs);
        QCOMPARE(gate.stats().packetsOutOfOrder, (uint64_t)2);
        feed(gate, 11, 4096, 3 * kMs);
        QCOMPARE(gate.stats().packetsOutOfOrder, (uint64_t)2);
        // Unsigned wrap: 0xffffffff then 0 is in order.
        Gate wrap;
        feed(wrap, 0xffffffffu, 4096, 0);
        feed(wrap, 0, 4096, 1 * kMs);
        QCOMPARE(wrap.stats().packetsOutOfOrder, (uint64_t)0);
    }

    void sequenceFilterResetsAfterLongGap()
    {
        Gate gate;
        feed(gate, 1000, 4096, 0);
        feed(gate, 5, 4096, 999 * kMs);
        QCOMPARE(gate.stats().packetsOutOfOrder, (uint64_t)1);
        feed(gate, 6, 4096, 999 * kMs + 1001 * kMs);
        QCOMPARE(gate.stats().packetsOutOfOrder, (uint64_t)1);
    }

    void staleChunksAreDropped_data()
    {
        QTest::addColumn<uint64_t>("ageNs");
        QTest::addColumn<bool>("stale");
        QTest::newRow("39 ms") << (uint64_t)(39 * kMs) << false;
        QTest::newRow("40 ms") << (uint64_t)(40 * kMs) << false;
        QTest::newRow("41 ms") << (uint64_t)(41 * kMs) << true;
    }

    void staleChunksAreDropped()
    {
        QFETCH(uint64_t, ageNs);
        QFETCH(bool, stale);
        Gate gate;
        uint8_t r[kMaxReportBytes];
        for (uint32_t s = 1; s <= 5; s++) feed(gate, s, 4096, 100 * kMs);
        TickResult t = gate.tick(100 * kMs + ageNs, r);
        QCOMPARE(gate.stats().packetsStale, (uint64_t)(stale ? 5 : 0));
        // Stale audio is never played: with nothing else buffered the gate goes idle.
        QCOMPARE(t.action, stale ? TickAction::Idle : TickAction::Send);
        QCOMPARE(gate.playing(), !stale);
    }

    void sequenceGapResetsDecimator()
    {
        GateConfig c;
        c.blocksPerReport = 1;
        Gate gate(c);
        uint8_t r[kMaxReportBytes];
        feed(gate, 1, 16384, 0, 8);  // half a 16-frame group
        for (uint32_t s = 3; s <= 5; s++) feed(gate, s, 2048, 0);  // seq 2 lost
        QCOMPARE(gate.tick(0, r).action, TickAction::Send);
        QCOMPARE(pcmAt(r, 1, 0), (int8_t)8);  // 36 if the half group leaked across the gap
    }

    void queueOverflowClearsOldPackets()
    {
        Gate gate;
        for (uint32_t s = 1; s <= 9; s++) feed(gate, s, 4096, 0);
        QCOMPARE(gate.stats().packetsOverflow, (uint64_t)8);
        QCOMPARE(gate.stats().packetsIn, (uint64_t)9);
    }

    // ---- Gate: newest wins ----
    void bufferKeepsOnlyTheNewestSamples()
    {
        GateConfig c;
        c.queueLimit = 32;
        Gate gate(c);
        uint8_t r[kMaxReportBytes];
        // 12 packets, 15 frames each at 3 kHz; packet k carries value k+1.
        for (uint32_t k = 0; k < 12; k++) feed(gate, k + 1, (int16_t)((k + 1) * 256), 0);
        QCOMPARE(gate.tick(0, r).action, TickAction::Send);
        // 180 frames, room for 2 reports (128): the oldest 52 go, so frame 52
        // (packet index 3, value 4) leads the report.
        QCOMPARE(gate.stats().framesTrimmed, (uint64_t)52);
        QCOMPARE(pcmAt(r, 2, 0), (int8_t)4);
    }

    // ---- Gate: in-flight cap ----
    void inFlightCapSkipsAndDropsTheSlice_data()
    {
        QTest::addColumn<int>("cap");
        QTest::newRow("1") << 1;
        QTest::newRow("2") << 2;
        QTest::newRow("3") << 3;
    }

    void inFlightCapSkipsAndDropsTheSlice()
    {
        QFETCH(int, cap);
        GateConfig c;
        c.maxInFlight = cap;
        c.blocksPerReport = 1;
        c.queueLimit = 64;
        Gate gate(c);
        uint8_t r[kMaxReportBytes];
        uint64_t now = 0;
        uint32_t seq = 1;
        auto topUp = [&]() {
            // Keep a little over one report buffered (3 packets = 45 frames).
            for (int i = 0; i < 3; i++) feed(gate, seq, (int16_t)(seq * 256), now), seq++;
        };
        for (int i = 0; i < cap; i++) {
            topUp();
            QCOMPARE(gate.tick(now, r).action, TickAction::Send);
            now += kMs;
        }
        QCOMPARE(gate.inFlight(), cap);
        topUp();
        QCOMPARE(gate.tick(now, r).action, TickAction::Skip);
        QCOMPARE(gate.stats().reportsSkipped, (uint64_t)1);
        const uint32_t firstSeqAfterSkip = seq;
        gate.onSent(true);
        QCOMPARE(gate.inFlight(), cap - 1);
        topUp();
        now += kMs;
        QCOMPARE(gate.tick(now, r).action, TickAction::Send);
        // The skipped slice was dropped, so the report holds only samples that
        // arrived after it (value >= firstSeqAfterSkip), never the skipped ones.
        QVERIFY(pcmAt(r, 1, 0) >= (int8_t)(firstSeqAfterSkip - 3));
        QCOMPARE(gate.stats().reportsCompleted, (uint64_t)1);
    }

    void skippedSliceIsNeverPlayedLater()
    {
        GateConfig c;
        c.maxInFlight = 1;
        c.blocksPerReport = 1;  // 32 frames per report
        Gate gate(c);
        uint8_t r[kMaxReportBytes];
        // Packet k carries value k; each packet is 15 frames at 3 kHz.
        for (uint32_t k = 1; k <= 3; k++) feed(gate, k, (int16_t)(k * 256), 0);
        QCOMPARE(gate.tick(0, r).action, TickAction::Send);  // 15x1, 15x2, 2x3
        QCOMPARE(pcmAt(r, 1, 0), (int8_t)1);
        for (uint32_t k = 4; k <= 5; k++) feed(gate, k, (int16_t)(k * 256), 1 * kMs);
        // Buffered: 13x3, 15x4, 15x5. The link is full, so the oldest 32 go.
        QCOMPARE(gate.tick(2 * kMs, r).action, TickAction::Skip);
        gate.onSent(true);
        for (uint32_t k = 6; k <= 7; k++) feed(gate, k, (int16_t)(k * 256), 3 * kMs);
        QCOMPARE(gate.tick(4 * kMs, r).action, TickAction::Send);
        QCOMPARE(pcmAt(r, 1, 0), (int8_t)5);  // 3 if the skipped slice were kept
    }

    void failedSendsAreCountedAndFreeTheSlot()
    {
        Gate gate;
        uint8_t r[kMaxReportBytes];
        for (uint32_t s = 1; s <= 5; s++) feed(gate, s, 4096, 0);
        QCOMPARE(gate.tick(0, r).action, TickAction::Send);
        gate.onSent(false);
        QCOMPARE(gate.inFlight(), 0);
        QCOMPARE(gate.stats().reportsFailed, (uint64_t)1);
        gate.onSent(true);  // spurious completion never goes negative
        QCOMPARE(gate.inFlight(), 0);
    }

    // ---- Gate: idle after 60 ms without a non-zero sample ----
    void idleEndsWaveform_data()
    {
        QTest::addColumn<uint64_t>("quietNs");
        QTest::addColumn<bool>("ended");
        QTest::newRow("60 ms") << (uint64_t)(60 * kMs) << false;
        QTest::newRow("61 ms") << (uint64_t)(61 * kMs) << true;
    }

    void idleEndsWaveform()
    {
        QFETCH(uint64_t, quietNs);
        QFETCH(bool, ended);
        GateConfig c;
        c.maxInFlight = 8;
        Gate gate(c);
        uint8_t r[kMaxReportBytes];
        for (uint32_t s = 1; s <= 5; s++) feed(gate, s, 4096, 0);
        QCOMPARE(gate.tick(0, r).action, TickAction::Send);
        TickResult t = gate.tick(quietNs, r);
        QCOMPARE(t.waveformEnded, ended);
        QCOMPARE(gate.playing(), !ended);
        if (ended) {
            // One silent report clears the coils, then nothing is sent.
            QCOMPARE(t.action, TickAction::Send);
            for (int i = 0; i < 2 * kBlockBytes; i++) QCOMPARE(pcmAt(r, 2, i), (int8_t)0);
            QVERIFY(!gate.hasWork());
            QCOMPARE(gate.tick(quietNs + 30 * kMs, r).action, TickAction::Idle);
            feed(gate, 6, 0, quietNs + 31 * kMs);
            QCOMPARE(gate.stats().packetsSilentIdle, (uint64_t)1);
        }
    }

    void idleWithLinkFullSkipsTheSilentReport()
    {
        GateConfig c;
        c.maxInFlight = 1;
        Gate gate(c);
        uint8_t r[kMaxReportBytes];
        for (uint32_t s = 1; s <= 5; s++) feed(gate, s, 4096, 0);
        QCOMPARE(gate.tick(0, r).action, TickAction::Send);
        TickResult t = gate.tick(100 * kMs, r);
        QVERIFY(t.waveformEnded);
        QCOMPARE(t.action, TickAction::Skip);
        QVERIFY(!gate.playing());
    }

    // The harness's "second block only" pattern (tools/ds5-haptics-probe/
    // haptics_harness.cpp): signal in odd 32-frame blocks, quiet blocks carrying
    // +1, which still decimates to 0 but keeps the packets from being dropped as
    // silence. Every report it produces must hold signal only in its second block.
    void secondBlockPatternStaysInSecondBlock()
    {
        GateConfig c;
        c.maxInFlight = 8;
        Gate gate(c);
        uint8_t r[kMaxReportBytes];
        const int16_t quiet = 1;
        uint64_t nextTick = 0;
        bool ticking = false;
        int sent = 0;
        long frame = 0;
        for (uint32_t p = 0; p < 400; p++) {  // 2 s of 5 ms packets
            const uint64_t now = (uint64_t)p * 5 * kMs;
            std::vector<uint8_t> pcm(240 * 4);
            for (int i = 0; i < 240; i++, frame++) {
                long block = (frame / kDecimation) / kBlockFrames;
                int16_t s = (block % 2) ? (int16_t)(0.8 * 32767 * sin(2 * M_PI * 160 * frame / 48000.0)) : quiet;
                if ((block % 2) && s == 0) s = 1000;
                for (int ch = 0; ch < 2; ch++) {
                    pcm[i * 4 + ch * 2] = (uint8_t)((uint16_t)s & 0xff);
                    pcm[i * 4 + ch * 2 + 1] = (uint8_t)((uint16_t)s >> 8);
                }
            }
            gate.receive(p + 1, pcm.data(), 240, now);
            if (!ticking) {
                ticking = true;
                nextTick = now;  // the worker wakes on the first packet
            }
            while (nextTick <= now) {
                TickResult t = gate.tick(nextTick, r);
                if (t.action == TickAction::Send && !t.waveformEnded) {
                    sent++;
                    bool firstQuiet = true, secondLoud = false;
                    for (int i = 0; i < kBlockBytes; i++) firstQuiet &= pcmAt(r, 2, i) == 0;
                    for (int i = kBlockBytes; i < 2 * kBlockBytes; i++) secondLoud |= pcmAt(r, 2, i) != 0;
                    QVERIFY2(firstQuiet && secondLoud, qPrintable(QString("report %1 misaligned").arg(sent)));
                    gate.onSent(true);
                }
                nextTick += gate.periodNs();
            }
        }
        QVERIFY(sent > 80);
        QCOMPARE(gate.stats().framesTrimmed, (uint64_t)0);
        QCOMPARE(gate.stats().packetsSilentIdle, (uint64_t)0);
    }

    void newWaveformAfterIdleStartsAgain()
    {
        GateConfig c;
        c.maxInFlight = 8;
        Gate gate(c);
        uint8_t r[kMaxReportBytes];
        for (uint32_t s = 1; s <= 5; s++) feed(gate, s, 4096, 0);
        gate.tick(0, r);
        gate.tick(100 * kMs, r);
        QVERIFY(!gate.playing());
        feed(gate, 6, 4096, 110 * kMs);
        QVERIFY(gate.playing());
        QVERIFY(gate.tick(110 * kMs, r).waveformStarted);
    }
};

QTEST_APPLESS_MAIN(TestHapticsCore)
#include "tst_hapticscore.moc"
