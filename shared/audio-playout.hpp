#pragma once
#include "protocol.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

// One audio worker owns this buffer. It never waits for missing packets or
// grows its latency to replay an obsolete backlog. Times are monotonic seconds.
namespace remote_audio {
constexpr uint32_t SampleRate = 48000;
constexpr size_t PacketFrames = 240;
constexpr size_t Channels = 2;
constexpr size_t PacketBytes = 32 + PacketFrames * Channels * sizeof(int16_t);

struct PlayoutSnapshot {
    double queuedMs = 0, targetMs = 15, driftPpm = 0;
    uint64_t receivedPackets = 0, lostPackets = 0, latePackets = 0;
    uint64_t underrunEvents = 0, discardedFrames = 0, renderedFrames = 0;
    bool primed = false, receiving = false;
};

class AudioJitterBuffer {
    struct Packet { std::array<float, PacketFrames * Channels> samples{}; };
    std::map<uint64_t, Packet> packets_;
    PlayoutSnapshot stats_{};
    double cursor_ = 0, jitterMs_ = 0, lastArrival_ = 0, targetHoldUntil_ = 0;
    double lastTargetChange_ = 0, correction_ = 0, integral_ = 0, queueMeanMs_ = 0, missingFrames_ = 0;
    double batchMs_ = 5, groupArrival_ = 0, previousGroupArrival_ = 0;
    uint64_t groupFirstSequence_ = 0, groupLastSequence_ = 0, groupCaptureUs_ = 0;
    uint64_t previousGroupSequence_ = 0, previousGroupCaptureUs_ = 0;
    bool haveGroup_ = false, havePreviousGroup_ = false;
    // A windowed-sinc fractional resampler avoids the high-frequency loss and
    // downsampling aliases produced by linear interpolation at 44.1/96 kHz.
    static constexpr size_t FilterTaps = 48, FilterPhases = 1024;
    uint32_t filterRate_ = 0;
    std::vector<std::array<float, FilterTaps>> filter_;
    uint64_t lastArrivalSequence_ = 0, lastCaptureUs_ = 0, lastMissingSequence_ = 0;
    bool haveArrival_ = false, haveMissingSequence_ = false, inGap_ = false;
    std::array<float, 2> lastOutput_{}, fadeFrom_{}, concealFrom_{};
    double fadeFrames_ = 0, volume_ = 1;

    double QueuedFrames() const {
        if (packets_.empty()) return 0;
        return std::max(0.0, static_cast<double>((packets_.rbegin()->first + 1) * PacketFrames) - cursor_);
    }
    void Fade() { fadeFrom_ = lastOutput_; fadeFrames_ = 96; }
    void ClearPlayout() {
        stats_.discardedFrames += packets_.size() * PacketFrames;
        packets_.clear(); stats_.primed = false; cursor_ = 0;
        missingFrames_ = 0; inGap_ = false; haveMissingSequence_ = false;
        correction_ = integral_ = queueMeanMs_ = 0; Fade();
    }
    void ErasePlayed() {
        auto sequence = static_cast<uint64_t>(cursor_) / PacketFrames;
        // One played packet remains available as FIR history. It does not
        // contribute to QueuedFrames, and reprime explicitly discards it.
        while (!packets_.empty() && packets_.begin()->first + 1 < sequence) packets_.erase(packets_.begin());
    }
    void TrimBacklog() {
        // Capture callbacks can deliver two 20 ms blocks together when their
        // device quantum does not divide the block length. Let that bounded
        // burst drain normally; it must not become a larger playout target.
        const double currentBatchMs = haveGroup_ ?
            std::clamp((groupLastSequence_ - groupFirstSequence_ + 1) * 5.0, 5.0, 20.0) : 5.0;
        const double burstMs = std::max(batchMs_, currentBatchMs);
        const double backlogLimitMs = 45 + std::clamp(burstMs - 5, 0.0, 15.0);
        if (packets_.empty() || QueuedFrames() <= SampleRate * backlogLimitMs / 1000) return;
        const double newestEnd = static_cast<double>((packets_.rbegin()->first + 1) * PacketFrames);
        const double wanted = (stats_.targetMs + burstMs * .5) * SampleRate / 1000;
        // Resume in the newest contiguous tail, rather than putting the cursor
        // inside a missing packet and immediately chasing late arrivals.
        auto tail = packets_.rbegin();
        uint64_t first = tail->first;
        while (++tail != packets_.rend() && tail->first + 1 == first) first = tail->first;
        const double next = std::max({cursor_, newestEnd - wanted,
            static_cast<double>(first * PacketFrames)});
        stats_.discardedFrames += static_cast<uint64_t>(next - cursor_);
        cursor_ = next; ErasePlayed(); missingFrames_ = 0; inGap_ = false; Fade();
        if (QueuedFrames() < std::max(15.0, stats_.targetMs + (batchMs_ >= 15 ? 5 : 0)) * SampleRate / 1000)
            stats_.primed = false;
    }
    bool Sample(uint64_t index, std::array<float, 2>& value) const {
        auto found = packets_.find(index / PacketFrames);
        if (found == packets_.end()) return false;
        const size_t offset = static_cast<size_t>(index % PacketFrames) * Channels;
        value = {found->second.samples[offset], found->second.samples[offset + 1]};
        return true;
    }
    void PrepareFilter(uint32_t rate) {
        if (filterRate_ == rate) return;
        constexpr double Pi = 3.14159265358979323846;
        const double cutoff = std::min(1.0, static_cast<double>(rate) / SampleRate) * .94;
        filter_.resize(FilterPhases);
        for (size_t phase = 0; phase < FilterPhases; ++phase) {
            double sum = 0;
            const double fraction = static_cast<double>(phase) / FilterPhases;
            for (size_t tap = 0; tap < FilterTaps; ++tap) {
                const double x = static_cast<double>(tap) - 23 - fraction;
                const double argument = Pi * cutoff * x;
                const double sinc = std::abs(argument) < 1e-12 ? 1 : std::sin(argument) / argument;
                const double window = .42 + .5 * std::cos(Pi * x / 24) + .08 * std::cos(2 * Pi * x / 24);
                const double weight = cutoff * sinc * std::max(0.0, window);
                filter_[phase][tap] = static_cast<float>(weight); sum += weight;
            }
            for (float& weight : filter_[phase]) weight = static_cast<float>(weight / sum);
        }
        filterRate_ = rate;
    }
    void Interpolate(uint64_t index, std::array<float, 2>& value) const {
        const size_t phase = std::min(FilterPhases - 1,
            static_cast<size_t>((cursor_ - static_cast<double>(index)) * FilterPhases));
        std::array<double, 2> sum{};
        uint64_t cachedSequence = UINT64_MAX;
        const Packet* cached = nullptr;
        const auto center = value;
        for (size_t tap = 0; tap < FilterTaps; ++tap) {
            const int offset = static_cast<int>(tap) - 23;
            auto sample = center;
            if (offset >= 0 || index >= static_cast<uint64_t>(-offset)) {
                const uint64_t position = offset >= 0 ? index + static_cast<uint64_t>(offset) : index - static_cast<uint64_t>(-offset);
                const uint64_t sequence = position / PacketFrames;
                if (sequence != cachedSequence) {
                    auto found = packets_.find(sequence);
                    cached = found == packets_.end() ? nullptr : &found->second;
                    cachedSequence = sequence;
                }
                if (cached) {
                    const size_t at = static_cast<size_t>(position % PacketFrames) * Channels;
                    sample = {cached->samples[at], cached->samples[at + 1]};
                }
            }
            for (size_t channel = 0; channel < Channels; ++channel)
                sum[channel] += sample[channel] * filter_[phase][tap];
        }
        value = {static_cast<float>(sum[0]), static_cast<float>(sum[1])};
    }
    void FinishArrivalGroup() {
        if (!haveGroup_) return;
        const double observedBatch = std::clamp((groupLastSequence_ - groupFirstSequence_ + 1) * .005 * 1000, 5.0, 20.0);
        batchMs_ += (observedBatch - batchMs_) * .2;
        if (havePreviousGroup_ && groupLastSequence_ > previousGroupSequence_ && groupArrival_ >= previousGroupArrival_) {
            double sourceDelta = (groupLastSequence_ - previousGroupSequence_) * .005;
            if (groupCaptureUs_ > previousGroupCaptureUs_)
                sourceDelta = (groupCaptureUs_ - previousGroupCaptureUs_) / 1000000.0;
            // Buffering protects against late delivery. A catch-up callback
            // carrying two blocks early does not require more permanent delay.
            const double deviation = std::max(0.0, (groupArrival_ - previousGroupArrival_) - sourceDelta) * 1000;
            jitterMs_ += (std::min(deviation, 100.0) - jitterMs_) / 16;
            // Same-callback 10/20 ms packet bursts are measured as one source
            // group. Tiny clock/rounding deltas do not force an extra 5 ms.
            const double batchFloor = std::ceil(batchMs_ / 5 - .01) * 5;
            const double wanted = std::clamp(std::max(batchFloor, 10 + std::ceil(std::max(0.0, jitterMs_ - .25) * 4 / 5) * 5), 10.0, 30.0);
            if (wanted > stats_.targetMs) { stats_.targetMs = wanted; lastTargetChange_ = groupArrival_; }
            else if (wanted < stats_.targetMs && groupArrival_ >= targetHoldUntil_ &&
                     groupArrival_ - lastTargetChange_ >= 1.0) {
                stats_.targetMs = std::max(wanted, stats_.targetMs - 5); lastTargetChange_ = groupArrival_;
            }
        }
        previousGroupArrival_ = groupArrival_; previousGroupSequence_ = groupLastSequence_;
        previousGroupCaptureUs_ = groupCaptureUs_; havePreviousGroup_ = true;
    }
    void Silence(float* output, size_t frames, uint32_t rate, bool muted) {
        // Do not create a step at an underrun, mute or reprime boundary.
        const double decay = std::exp(-1.0 / (rate * 0.001));
        for (size_t i = 0; i < frames; ++i) {
            const double goal = muted ? 0 : 1;
            volume_ += std::clamp(goal - volume_, -1.0 / (rate * 0.005), 1.0 / (rate * 0.005));
            for (size_t c = 0; c < Channels; ++c) {
                lastOutput_[c] = static_cast<float>(lastOutput_[c] * decay);
                output[i * Channels + c] = static_cast<float>(lastOutput_[c] * volume_);
            }
        }
    }
public:
    void Reset() { *this = AudioJitterBuffer{}; }
    bool Push(const uint8_t* bytes, size_t length, double arrivalSeconds) {
        if (!bytes || length != PacketBytes || !std::isfinite(arrivalSeconds)) return false;
        RemoteAudioHeader header{}; std::memcpy(&header, bytes, sizeof(header));
        // Keep absolute sample positions exactly representable by double.
        constexpr uint64_t MaxSequence = (uint64_t{1} << 48) / PacketFrames - 1;
        if (header.magic != 0x31414652 || header.sample_rate != SampleRate ||
            header.channels != Channels || header.frames != PacketFrames ||
            (header.flags & ~1u) || header.sequence > MaxSequence) return false;
        const double start = static_cast<double>(header.sequence * PacketFrames);
        if (stats_.primed && start + PacketFrames <= cursor_) { ++stats_.latePackets; return false; }
        if (packets_.contains(header.sequence)) return false;
        const bool newer = !haveArrival_ || header.sequence > lastArrivalSequence_;
        const bool restart = newer && ((header.flags & 1u) ||
            (haveArrival_ && arrivalSeconds - lastArrival_ > 0.250) ||
            (stats_.primed && start - cursor_ > SampleRate * .250));
        if (restart) {
            ClearPlayout(); haveArrival_ = false;
            haveGroup_ = havePreviousGroup_ = false;
        }
        Packet packet;
        for (size_t i = 0; i < packet.samples.size(); ++i) {
            int16_t sample; std::memcpy(&sample, bytes + sizeof(header) + i * sizeof(sample), sizeof(sample));
            packet.samples[i] = sample / 32768.0f;
        }
        if (!haveArrival_ || header.sequence > lastArrivalSequence_) {
            if (haveGroup_ && arrivalSeconds - groupArrival_ > .001) {
                FinishArrivalGroup(); haveGroup_ = false;
            }
            if (!haveGroup_) {
                groupArrival_ = arrivalSeconds; groupFirstSequence_ = header.sequence; haveGroup_ = true;
            }
            groupLastSequence_ = header.sequence; groupCaptureUs_ = header.capture_time_us;
            lastArrival_ = arrivalSeconds; lastArrivalSequence_ = header.sequence;
            lastCaptureUs_ = header.capture_time_us; haveArrival_ = true;
        }
        packets_.emplace(header.sequence, std::move(packet));
        ++stats_.receivedPackets; stats_.receiving = true;
        if (!stats_.primed) cursor_ = static_cast<double>(packets_.begin()->first * PacketFrames);
        TrimBacklog();
        return true;
    }
    void Render(float* output, size_t frames, uint32_t outputRate, bool muted = false) {
        if (!output || !frames) return;
        if (outputRate < 8000 || outputRate > 384000) {
            std::fill_n(output, frames * Channels, 0.0f); return;
        }
        const double baseStep = static_cast<double>(SampleRate) / outputRate;
        PrepareFilter(outputRate);
        if(haveGroup_) {
            const double currentBatch=std::clamp((groupLastSequence_-groupFirstSequence_+1)*5.0,5.0,20.0);
            batchMs_=std::max(batchMs_,currentBatch);
            stats_.targetMs=std::max(stats_.targetMs,std::ceil(batchMs_/5-.01)*5);
        }
        if (!stats_.primed) {
            const double primeMs=std::max(15.0,stats_.targetMs+(batchMs_>=15?5:0));
            if (packets_.empty() || QueuedFrames() < primeMs * SampleRate / 1000) {
                Silence(output, frames, outputRate, muted); return;
            }
            stats_.primed = true; missingFrames_ = 0; inGap_ = false;
            queueMeanMs_ = QueuedFrames() * 1000 / SampleRate; Fade();
        }
        // Small continuous rate adjustments absorb independent capture/render
        // clocks. A hard, faded trim is reserved for an exceptional backlog
        // beyond the batch-dependent 45–60 ms ceiling, not an ordinary burst.
        const double elapsed = static_cast<double>(frames) / outputRate;
        const double alpha = 1 - std::exp(-elapsed / .25);
        queueMeanMs_ += (QueuedFrames() * 1000 / SampleRate - queueMeanMs_) * alpha;
        // PI, rather than P alone, tracks either sign of independent clock
        // drift without demanding a permanent queue deficit. Half the source
        // burst span covers ordinary grouped delivery without inflating jitter.
        const double errorMs = queueMeanMs_ - (stats_.targetMs + batchMs_ * .5);
        integral_ = std::clamp(integral_ + errorMs * .00008 * elapsed, -.0015, .0015);
        const double wantedCorrection = std::clamp(integral_ + errorMs * .00035, -.003, .003);
        correction_ += (wantedCorrection - correction_) * (1 - std::exp(-elapsed / .1));
        stats_.driftPpm = correction_ * 1000000;
        const double step = baseStep * (1 + correction_);
        for (size_t i = 0; i < frames; ++i) {
            std::array<float, 2> value{};
            const uint64_t index = static_cast<uint64_t>(cursor_);
            bool available = Sample(index, value);
            if (available) {
                Interpolate(index, value);
                if (inGap_) { Fade(); inGap_ = false; }
                missingFrames_ = 0;
            } else {
                const uint64_t sequence = index / PacketFrames;
                if (!haveMissingSequence_ || sequence != lastMissingSequence_) {
                    ++stats_.lostPackets; lastMissingSequence_ = sequence; haveMissingSequence_ = true;
                }
                if (!inGap_) {
                    inGap_ = true; concealFrom_ = lastOutput_; ++stats_.underrunEvents;
                    stats_.targetMs = std::min(30.0, stats_.targetMs + 5);
                    targetHoldUntil_ = lastArrival_ + 2;
                }
                missingFrames_ += step;
                const double gain = std::max(0.0, 1.0 - missingFrames_ / PacketFrames);
                for (size_t c = 0; c < Channels; ++c) value[c] = static_cast<float>(concealFrom_[c] * gain);
            }
            if (fadeFrames_ > 0 && available) {
                const double gain = 1 - fadeFrames_ / 96;
                for (size_t c = 0; c < Channels; ++c)
                    value[c] = static_cast<float>(fadeFrom_[c] + (value[c] - fadeFrom_[c]) * gain);
                fadeFrames_ = std::max(0.0, fadeFrames_ - baseStep);
            }
            const double goal = muted ? 0 : 1;
            const double gainStep = 1.0 / (outputRate * 0.005);
            volume_ += std::clamp(goal - volume_, -gainStep, gainStep);
            for (size_t c = 0; c < Channels; ++c) {
                output[i * Channels + c] = std::clamp(static_cast<float>(value[c] * volume_), -1.0f, 1.0f);
                lastOutput_[c] = value[c];
            }
            cursor_ += step; ++stats_.renderedFrames;
            if (missingFrames_ >= PacketFrames * 3) {
                // FIR history must never become a new starting packet. Discard
                // it after a sustained gap and prime from newly arrived audio.
                stats_.discardedFrames += packets_.size() * PacketFrames;
                packets_.clear(); stats_.primed = false; cursor_ = 0;
                missingFrames_ = 0; inGap_ = false; haveMissingSequence_ = false;
                Silence(output + (i + 1) * Channels, frames - i - 1, outputRate, muted); return;
            }
        }
        ErasePlayed(); TrimBacklog();
    }
    PlayoutSnapshot Snapshot() const {
        auto result = stats_; result.queuedMs = QueuedFrames() * 1000 / SampleRate; return result;
    }
};
} // namespace remote_audio
