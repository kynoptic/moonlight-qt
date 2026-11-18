#include "hapticscore.h"

#include <algorithm>
#include <cstring>

namespace Ds5Haptics {

namespace {

constexpr uint32_t kCrcPoly = 0xEDB88320u;
// CRC-32 register after the Bluetooth HID output header byte 0xA2.
constexpr uint32_t kCrcSeed = ~0xEADA2D49u;

void signReport(uint8_t* report, size_t size)
{
    uint32_t crc = reportCrc(report, size - 4);
    for (int i = 0; i < 4; i++) {
        report[size - 4 + i] = (uint8_t)(crc >> (8 * i));
    }
}

int16_t readS16(const uint8_t* p)
{
    return (int16_t)(uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

}  // namespace

uint32_t reportCrc(const uint8_t* data, size_t size)
{
    uint32_t crc = kCrcSeed;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (kCrcPoly & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

size_t reportBytes(int blocks)
{
    switch (blocks) {
    case 1: return 142;
    case 2: return 206;
    default: return 0;
    }
}

size_t buildReport(uint8_t* out, size_t capacity, int blocks, uint8_t sequence, const int8_t* pcm)
{
    size_t size = reportBytes(blocks);
    if (size == 0 || capacity < size) {
        return 0;
    }

    memset(out, 0, size);
    out[1] = (uint8_t)((sequence & 0x0f) << 4);
    out[2] = 0x91;  // audio control sub-packet, sized
    if (blocks == 2) {
        out[0] = 0x33;
        out[3] = 6;
        out[4] = 0x7e;
        out[5] = out[6] = out[7] = out[8] = kControllerQueue;
        out[9] = (uint8_t)(sequence * 2);
        out[10] = 0xd2;  // haptics sub-packet, sized, two blocks
        out[11] = kBlockBytes;
        memcpy(out + 12, pcm, 2 * kBlockBytes);
    }
    else {
        out[0] = 0x32;
        out[3] = 7;
        out[4] = 0xfe;
        out[5] = out[6] = out[7] = out[8] = out[9] = kControllerQueue;
        out[10] = sequence;
        out[11] = 0x92;  // haptics sub-packet, sized
        out[12] = kBlockBytes;
        memcpy(out + 13, pcm, kBlockBytes);
    }
    signReport(out, size);
    return size;
}

void buildFullPowerReport(uint8_t out[kFullPowerReportBytes])
{
    memset(out, 0, kFullPowerReportBytes);
    out[0] = 0x31;
    out[2] = 0x10;
    out[4] = 0x02 | 0x20 | 0x40;
    out[42] = 0x01;
    signReport(out, kFullPowerReportBytes);
}

void Decimator::reset()
{
    m_Sum[0] = m_Sum[1] = 0;
    m_Count = 0;
}

void Decimator::push(const uint8_t* pcm, size_t frames, std::vector<int8_t>& out)
{
    for (size_t i = 0; i < frames; i++) {
        m_Sum[0] += readS16(pcm + i * 4);
        m_Sum[1] += readS16(pcm + i * 4 + 2);
        if (++m_Count == kDecimation) {
            for (int ch = 0; ch < 2; ch++) {
                // Average, then keep the top 8 bits (arithmetic shift floors).
                int32_t average = m_Sum[ch] / kDecimation;
                out.push_back((int8_t)std::clamp(average >> 8, -128, 127));
            }
            reset();
        }
    }
}

Gate::Gate(const GateConfig& config) : m_Config(config)
{
    if (m_Config.blocksPerReport != 1) {
        m_Config.blocksPerReport = 2;
    }
    m_Config.maxInFlight = std::max(1, m_Config.maxInFlight);
    m_Config.queueLimit = std::max<size_t>(1, m_Config.queueLimit);
}

void Gate::receive(uint32_t sequence, const uint8_t* pcm, uint16_t frames, uint64_t nowNs)
{
    m_Stats.packetsIn++;

    // Late or duplicate: unsigned subtraction handles wrap. After a long quiet
    // spell any sequence is accepted, in case the host restarted its counter.
    bool longGap = m_HaveInputSequence && nowNs - m_LastPacketNs > m_Config.sequenceResetNs;
    if (m_HaveInputSequence && !longGap && (int32_t)(sequence - m_LatestInputSequence) <= 0) {
        m_Stats.packetsOutOfOrder++;
        return;
    }
    m_HaveInputSequence = true;
    m_LatestInputSequence = sequence;
    m_LastPacketNs = nowNs;

    const size_t bytes = (size_t)frames * 4;
    const bool nonzero = std::any_of(pcm, pcm + bytes, [](uint8_t b) { return b != 0; });
    if (nonzero) {
        m_LastNonzeroNs = nowNs;
    }
    else if (!m_Active && m_Queue.empty()) {
        // Silence while idle: send nothing, so the link stays free.
        m_Stats.packetsSilentIdle++;
        return;
    }

    if (m_Queue.size() >= m_Config.queueLimit) {
        m_Stats.packetsOverflow += m_Queue.size();
        m_Queue.clear();
    }
    m_Queue.push_back(Chunk{sequence, nowNs, std::vector<uint8_t>(pcm, pcm + bytes), frames});
}

void Gate::pull(uint64_t nowNs)
{
    std::vector<int8_t> decimated;
    while (!m_Queue.empty()) {
        Chunk chunk = std::move(m_Queue.front());
        m_Queue.pop_front();

        if (nowNs > chunk.receivedNs && nowNs - chunk.receivedNs > m_Config.staleNs) {
            m_Stats.packetsStale++;
            m_Decimator.reset();
            m_HaveExpected = false;
            continue;
        }
        if (m_HaveExpected && chunk.sequence != m_ExpectedSequence) {
            // A packet was lost: don't blend samples from before the gap.
            m_Decimator.reset();
        }
        m_ExpectedSequence = chunk.sequence + 1;
        m_HaveExpected = true;

        decimated.clear();
        m_Decimator.push(chunk.pcm.data(), chunk.frames, decimated);
        m_Fifo.insert(m_Fifo.end(), decimated.begin(), decimated.end());
    }

    // Newest wins: keep at most two reports' worth; older samples would play late.
    const size_t limit = (size_t)kBlockBytes * m_Config.blocksPerReport * 2;
    if (m_Fifo.size() > limit) {
        const size_t excess = m_Fifo.size() - limit;
        m_Stats.framesTrimmed += excess / 2;
        m_Fifo.erase(m_Fifo.begin(), m_Fifo.begin() + (ptrdiff_t)excess);
    }
}

void Gate::endWaveform()
{
    m_Active = false;
    m_Queue.clear();
    m_Fifo.clear();
    m_Decimator.reset();
    m_HaveExpected = false;
}

TickResult Gate::tick(uint64_t nowNs, uint8_t* out)
{
    TickResult result;
    pull(nowNs);

    if (!m_Active) {
        if (m_Fifo.empty()) {
            return result;  // Idle
        }
        m_Active = true;
        result.waveformStarted = true;
    }

    const size_t reportSamples = (size_t)kBlockBytes * m_Config.blocksPerReport;
    int8_t samples[2 * kBlockBytes] = {};

    if (nowNs > m_LastNonzeroNs && nowNs - m_LastNonzeroNs > m_Config.idleNs) {
        // Quiet long enough: one silent report clears the coils, then stop.
        endWaveform();
        result.waveformEnded = true;
    }
    else if (m_Fifo.size() < reportSamples) {
        result.action = TickAction::Wait;
        return result;
    }
    else {
        // Take this slice now, sent or not, so playback never falls behind.
        std::copy(m_Fifo.begin(), m_Fifo.begin() + (ptrdiff_t)reportSamples, samples);
        m_Fifo.erase(m_Fifo.begin(), m_Fifo.begin() + (ptrdiff_t)reportSamples);
    }

    if (m_InFlight >= m_Config.maxInFlight) {
        m_Stats.reportsSkipped++;
        result.action = TickAction::Skip;
        return result;
    }

    result.length = buildReport(out, kMaxReportBytes, m_Config.blocksPerReport, m_ReportSequence++, samples);
    result.action = TickAction::Send;
    m_InFlight++;
    m_Stats.reportsSent++;
    return result;
}

void Gate::onSent(bool success)
{
    if (m_InFlight == 0) {
        return;
    }
    m_InFlight--;
    if (success) {
        m_Stats.reportsCompleted++;
    }
    else {
        m_Stats.reportsFailed++;
    }
}

bool Gate::playing() const
{
    return m_Active || !m_Queue.empty();
}

bool Gate::hasWork() const
{
    return m_Active || !m_Queue.empty();
}

}  // namespace Ds5Haptics
