#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

// Local monotonic-clock durations, in microseconds. Percentiles use nearest
// rank over the most recent samples; this is not an end-to-end clock estimate.
struct LatencySnapshot {
    uint64_t totalSamples = 0;
    size_t windowSamples = 0;
    uint64_t lastUs = 0;
    double meanUs = 0;
    uint64_t p95Us = 0, p99Us = 0, maxUs = 0;
};

class RollingLatencyStats {
public:
    static constexpr size_t WindowCapacity = 256;

    void ObserveUs(uint64_t durationUs) {
        std::lock_guard lock(mutex_);
        samples_[next_] = durationUs;
        next_ = (next_ + 1) % WindowCapacity;
        if (used_ < WindowCapacity) ++used_;
        if (total_ != (std::numeric_limits<uint64_t>::max)()) ++total_;
    }

    LatencySnapshot Snapshot() const {
        std::array<uint64_t, WindowCapacity> ordered{};
        LatencySnapshot result;
        {
            std::lock_guard lock(mutex_);
            result.totalSamples = total_;
            result.windowSamples = used_;
            if (!used_) return result;
            result.lastUs = samples_[(next_ + WindowCapacity - 1) % WindowCapacity];
            std::copy_n(samples_.begin(), used_, ordered.begin());
        }
        // Keep sorting and summation outside the producer's critical section.
        std::sort(ordered.begin(), ordered.begin() + result.windowSamples);
        long double sum = 0;
        for (size_t i = 0; i < result.windowSamples; ++i) sum += ordered[i];
        result.meanUs = static_cast<double>(sum / result.windowSamples);
        result.p95Us = ordered[(result.windowSamples * 95 + 99) / 100 - 1];
        result.p99Us = ordered[(result.windowSamples * 99 + 99) / 100 - 1];
        result.maxUs = ordered[result.windowSamples - 1];
        return result;
    }

    void Reset() {
        std::lock_guard lock(mutex_);
        next_ = used_ = 0;
        total_ = 0;
    }

private:
    mutable std::mutex mutex_;
    std::array<uint64_t, WindowCapacity> samples_{};
    size_t next_ = 0, used_ = 0;
    uint64_t total_ = 0;
};
