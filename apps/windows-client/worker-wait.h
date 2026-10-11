#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace remote_video_worker {
// Function injection is limited to this small wait primitive, so failure and
// shutdown behavior can be checked without global timer-resolution changes.
struct WaitFunctions {
    decltype(&CreateWaitableTimerExW) create = &CreateWaitableTimerExW;
    decltype(&SetWaitableTimerEx) set = &SetWaitableTimerEx;
    decltype(&WaitForSingleObject) wait = &WaitForSingleObject;
    decltype(&CancelWaitableTimer) cancel = &CancelWaitableTimer;
    decltype(&CloseHandle) close = &CloseHandle;
    static void SleepFallback(DWORD milliseconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
    void (*sleep)(DWORD) = &SleepFallback;
};

class WorkerWait {
public:
    // A failed timer must never leave a network worker stuck indefinitely.
    static constexpr DWORD MaximumWaitMs = 20;

    explicit WorkerWait(WaitFunctions functions = {}) : functions_(functions) {
        timer_ = functions_.create(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer_) ++failures_;
    }
    ~WorkerWait() { DisableTimer(); }
    WorkerWait(const WorkerWait&) = delete;
    WorkerWait& operator=(const WorkerWait&) = delete;

    void Wait(const std::atomic<bool>& stop) {
        if (stop.load(std::memory_order_relaxed)) return;
        if (timer_) {
            LARGE_INTEGER due{};
            due.QuadPart = -10000; // Relative 1 ms in 100 ns units.
            if (functions_.set(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
                const DWORD result = functions_.wait(timer_, MaximumWaitMs);
                if (result == WAIT_OBJECT_0) return;
            }
            // Unsupported/broken timers fall back for the remainder of this
            // worker, instead of repeatedly incurring a failed/bounded wait.
            ++failures_;
            DisableTimer();
        }
        if (!stop.load(std::memory_order_relaxed)) {
            ++fallbackWaits_;
            functions_.sleep(1);
        }
    }
    bool HighResolution() const { return timer_ != nullptr; }
    uint64_t FallbackWaits() const { return fallbackWaits_; }
    uint64_t Failures() const { return failures_; }

private:
    void DisableTimer() {
        if (!timer_) return;
        functions_.cancel(timer_);
        functions_.close(timer_);
        timer_ = nullptr;
    }
    WaitFunctions functions_;
    HANDLE timer_ = nullptr;
    uint64_t fallbackWaits_ = 0, failures_ = 0;
};
} // namespace remote_video_worker
