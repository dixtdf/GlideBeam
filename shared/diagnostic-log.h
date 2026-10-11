#pragma once
#include <cstddef>
#include <filesystem>
#include <string_view>

// Diagnostic metadata only. Callers must never pass authentication secrets,
// clipboard bodies, or user file names. The writer also removes common secret
// assignments and absolute paths as defense in depth.
namespace remote_log {
enum class Level { Debug, Info, Warning, Error };
struct Config {
    std::filesystem::path directory;
    std::size_t maxFileBytes = 4 * 1024 * 1024;
    std::size_t retainedFiles = 3; // Includes the current remote-native.log.
    std::size_t maxQueuedRecords = 512;
};
bool Initialize(Config config = {}) noexcept;
void Write(Level level, std::string_view component, std::string_view message) noexcept;
#ifdef REMOTE_LOG_TESTING
// Test-only admission receipt; true means queued, not yet flushed to disk.
bool TryWriteForTest(Level level, std::string_view component, std::string_view message) noexcept;
#endif
std::filesystem::path Directory();
// Drain queued metadata at orderly exit. No disk I/O occurs on Write callers.
void Shutdown() noexcept;
const char* ErrorClass(std::string_view message) noexcept;
}
