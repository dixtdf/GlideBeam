#pragma once
#include "../../shared/protocol.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace remote_audio_capture {
constexpr uint32_t kSampleRate = 48000;
constexpr size_t kChannels = 2;
constexpr size_t kPacketFrames = 240;
constexpr size_t kHeaderBytes = 32;
constexpr size_t kPacketBytes = kHeaderBytes + kPacketFrames * kChannels * sizeof(int16_t);
static_assert(sizeof(RemoteAudioHeader) == kHeaderBytes);

inline int16_t FloatToS16(float value) {
    if (!std::isfinite(value)) return 0;
    if (value <= -1.0f) return std::numeric_limits<int16_t>::min();
    if (value >= 1.0f) return std::numeric_limits<int16_t>::max();
    const long rounded = std::lround(static_cast<double>(value) * 32768.0);
    return static_cast<int16_t>(rounded > 32767 ? 32767 : rounded);
}
inline void Put16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value); out[1] = static_cast<uint8_t>(value >> 8);
}
inline void Put32(uint8_t* out, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8));
}
inline void Put64(uint8_t* out, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8));
}

// Owned only by the serial ScreenCaptureKit audio callback queue. Copies directly
// into a fixed 5 ms packet; no sample-buffer retention, codec, or network waits.
class Packetizer {
    std::array<uint8_t, kPacketBytes> packet_{};
    size_t partial_ = 0;
    uint64_t sequence_ = 0;
    uint64_t packetTimeUs_ = 0;
    long double expectedTimeUs_ = 0;
    bool haveTime_ = false;
    bool discontinuity_ = true;
public:
    void Reset() {
        partial_ = 0; haveTime_ = false; discontinuity_ = true;
        // Sequence never rewinds during this process, including reconnects.
    }
    size_t PendingFrames() const { return partial_; }

    // Interleaved: left points at LRLR and right is ignored. Planar: one float32
    // buffer per channel. Byte bounds are checked before reading unaligned data.
    template<class Send>
    bool PushFloat32(const void* left, size_t leftBytes, const void* right,
                     size_t rightBytes, size_t frames, bool planar,
                     uint64_t captureTimeUs, Send&& send) {
        const size_t stride = planar ? sizeof(float) : kChannels * sizeof(float);
        if (!left || frames == 0 || frames > kSampleRate / 10 ||
            captureTimeUs > std::numeric_limits<uint64_t>::max() - 100000ULL ||
            leftBytes < frames * stride ||
            (planar && (!right || rightBytes < frames * sizeof(float)))) {
            Reset(); return false;
        }
        // A real source/PTS gap discards a partial old packet. Rounding jitter
        // under 2 ms is tolerated without accumulating timestamp drift.
        if (haveTime_ && std::fabs(static_cast<long double>(captureTimeUs) - expectedTimeUs_) > 2000.0L)
            Reset();
        const auto* leftData = static_cast<const uint8_t*>(left);
        const auto* rightData = static_cast<const uint8_t*>(right);
        for (size_t frame = 0; frame < frames; ++frame) {
            if (!partial_) packetTimeUs_ = captureTimeUs + static_cast<uint64_t>(frame * 1000000ULL / kSampleRate);
            float l = 0, r = 0;
            std::memcpy(&l, leftData + frame * stride, sizeof(float));
            std::memcpy(&r, planar ? rightData + frame * sizeof(float) :
                        leftData + frame * stride + sizeof(float), sizeof(float));
            const size_t offset = kHeaderBytes + partial_ * kChannels * sizeof(int16_t);
            Put16(packet_.data() + offset, static_cast<uint16_t>(FloatToS16(l)));
            Put16(packet_.data() + offset + sizeof(int16_t), static_cast<uint16_t>(FloatToS16(r)));
            if (++partial_ != kPacketFrames) continue;
            Put32(packet_.data(), 0x31414652); // RFA1
            Put32(packet_.data() + 4, kSampleRate);
            Put16(packet_.data() + 8, static_cast<uint16_t>(kChannels));
            Put16(packet_.data() + 10, static_cast<uint16_t>(kPacketFrames));
            Put32(packet_.data() + 12, discontinuity_ ? 1u : 0u);
            Put64(packet_.data() + 16, sequence_++);
            Put64(packet_.data() + 24, packetTimeUs_);
            // A rejected live packet must not be retried later. The following
            // accepted packet tells the player to conceal the discontinuity.
            discontinuity_ = send(packet_.data(), packet_.size()) != 1;
            partial_ = 0;
        }
        expectedTimeUs_ = static_cast<long double>(captureTimeUs) +
                          static_cast<long double>(frames) * 1000000.0L / kSampleRate;
        haveTime_ = true;
        return true;
    }
};
} // namespace remote_audio_capture
