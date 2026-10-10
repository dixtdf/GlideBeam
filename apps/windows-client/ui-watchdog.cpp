#include "ui-watchdog.h"
#include "../../shared/diagnostic-log.h"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace remote_ui_watchdog {
namespace {
std::atomic<ULONGLONG> heartbeat{0};
std::atomic<const char*> currentPhase{"message-loop"};
std::mutex wakeMutex;
std::condition_variable wake;
bool stopping = false;
std::thread worker;
}
void Beat(const char* phase) noexcept {
    currentPhase.store(phase, std::memory_order_relaxed);
    heartbeat.store(GetTickCount64(), std::memory_order_release);
}
void Start(uint32_t uiThreadId) {
    Stop();
    stopping = false;
    Beat();
    worker = std::thread([uiThreadId] {
        bool stalled = false;
        ULONGLONG lastReport = 0;
        std::unique_lock lock(wakeMutex);
        while (!wake.wait_for(lock, std::chrono::seconds(1), [] { return stopping; })) {
            const auto now = GetTickCount64();
            const auto elapsed = now - heartbeat.load(std::memory_order_acquire);
            if (elapsed >= 5000 && (!stalled || now - lastReport >= 15000)) {
                remote_log::Write(remote_log::Level::Warning, "ui.watchdog",
                    "heartbeat_stalled ui_tid=" + std::to_string(uiThreadId) +
                    " elapsed_ms=" + std::to_string(elapsed) +
                    " phase=" + currentPhase.load(std::memory_order_relaxed));
                stalled = true;
                lastReport = now;
            } else if (stalled && elapsed < 5000) {
                remote_log::Write(remote_log::Level::Info, "ui.watchdog", "heartbeat_recovered");
                stalled = false;
            }
        }
    });
}
void Stop() noexcept {
    {
        std::lock_guard lock(wakeMutex);
        stopping = true;
    }
    wake.notify_all();
    if (worker.joinable()) worker.join();
}
}
