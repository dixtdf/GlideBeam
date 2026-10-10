#pragma once
#include <cstdint>

// Metadata only: the watcher never reads desktop, clipboard, or process memory.
namespace remote_ui_watchdog {
void Start(uint32_t uiThreadId);
void Beat(const char* phase = "message-loop") noexcept;
void Stop() noexcept;
}
