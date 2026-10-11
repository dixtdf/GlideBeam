#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Snapshot before inspecting work. A notification between the inspection and
// Wait must remain visible, including synchronous VideoToolbox callbacks.
class EncoderWakeSignal {
    std::mutex mutex_;
    std::condition_variable changed_;
    uint64_t generation_ = 0;
    bool stopped_ = false;
public:
    uint64_t Observe() {
        std::lock_guard lock(mutex_);
        return generation_;
    }
    void Notify() {
        { std::lock_guard lock(mutex_); ++generation_; }
        changed_.notify_one();
    }
    bool Wait(uint64_t observed) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return stopped_ || generation_ != observed; });
        return !stopped_;
    }
    void Stop() {
        { std::lock_guard lock(mutex_); stopped_ = true; ++generation_; }
        changed_.notify_all();
    }
};
