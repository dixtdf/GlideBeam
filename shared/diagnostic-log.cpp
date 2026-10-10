#include "diagnostic-log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace remote_log {
namespace {
constexpr std::size_t MaxRecordBytes = 2048;
struct Record {
    std::chrono::system_clock::time_point time;
    std::size_t thread = 0;
    Level level = Level::Info;
    std::string component, message;
};
class FileLock {
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED position_{};
#else
    int handle_ = -1;
#endif
public:
    explicit FileLock(const std::filesystem::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        handle_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
#endif
    }
    ~FileLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (handle_ >= 0) close(handle_);
#endif
    }
    bool Acquire() {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE && LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &position_);
#else
        return handle_ >= 0 && flock(handle_, LOCK_EX) == 0;
#endif
    }
    void Release() {
#ifdef _WIN32
        UnlockFileEx(handle_, 0, 1, 0, &position_);
#else
        flock(handle_, LOCK_UN);
#endif
    }
};
std::filesystem::path DefaultDirectory() {
#ifdef _WIN32
    wchar_t local[32768]{};
    auto count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (count && count < 32768) return std::filesystem::path(local) / L"RemoteNative" / L"Logs";
#else
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home) / "Library" / "Logs" / "RemoteNative";
#endif
    std::error_code error;
    auto directory = std::filesystem::temp_directory_path(error);
    return (error ? std::filesystem::path(".") : directory) / "RemoteNative" / "Logs";
}
unsigned long ProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}
char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
std::string Limited(std::string_view text, std::size_t maximum) {
    if (text.size() <= maximum) return std::string(text);
    const std::string_view suffix = " [truncated]";
    std::size_t end = maximum > suffix.size() ? maximum - suffix.size() : maximum;
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    return std::string(text.substr(0, end)) + std::string(suffix);
}
bool StartsInsensitive(std::string_view text, std::size_t at, std::string_view word) {
    if (at > text.size() || word.size() > text.size() - at) return false;
    for (std::size_t i = 0; i < word.size(); ++i) if (Lower(text[at + i]) != word[i]) return false;
    return true;
}
bool Boundary(char c) { return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '(' || c == '=' || c == ':' || c == '[' || c == '{' || c == ','; }
std::string Sanitize(std::string_view text) {
    std::string result;
    result.reserve(std::min(text.size(), MaxRecordBytes));
    for (std::size_t i = 0; i < text.size() && result.size() < MaxRecordBytes;) {
        bool secret = false;
        for (std::string_view word : {"pairing_token", "pairing-token", "pairingtoken", "tokenprotected", "password", "passwd", "token", "clipboard", "secret", "authorization"}) {
            if ((i == 0 || Boundary(text[i - 1])) && StartsInsensitive(text, i, word)) {
                std::size_t value = i + word.size();
                while (value < text.size() && (text[value] == ' ' || text[value] == '"' || text[value] == '\'')) ++value;
                if (value < text.size() && (text[value] == '=' || text[value] == ':')) {
                    result.append(text.substr(i, word.size())); result += "=[redacted]";
                    ++value; while (value < text.size() && text[value] == ' ') ++value;
                    if (value < text.size() && (text[value] == '"' || text[value] == '\'')) {
                        char quote = text[value++];
                        while (value < text.size() && text[value] != quote) {
                            if (text[value] == '\\' && value + 1 < text.size()) ++value;
                            ++value;
                        }
                        if (value < text.size()) ++value;
                    } else while (value < text.size() && text[value] != ',' && text[value] != ';' && text[value] != '\n' && text[value] != '\r' && text[value] != '}') ++value;
                    i = value; secret = true; break;
                }
            }
        }
        if (secret) continue;
        // Avoid retaining user paths even if an OS error embeds one. Slash-led
        // components, drive paths, UNC paths, and file URLs are all replaced.
        bool path = text[i] == '/' || (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\\') ||
            (i + 2 < text.size() && ((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z')) && text[i + 1] == ':' && (text[i + 2] == '/' || text[i + 2] == '\\')) ||
            StartsInsensitive(text, i, "file://");
        if (path && (i == 0 || Boundary(text[i - 1]))) {
            result += "[path]";
            bool quoted = i > 0 && (text[i - 1] == '"' || text[i - 1] == '\'');
            char quote = quoted ? text[i - 1] : '\0';
            do { ++i; } while (i < text.size() && (quoted ? text[i] != quote : text[i] != '\n' && text[i] != '\r' && text[i] != ',' && text[i] != ';' && text[i] != ')' && text[i] != '"'));
            continue;
        }
        unsigned char c = static_cast<unsigned char>(text[i++]);
        result += c < 32 || c == 127 ? ' ' : static_cast<char>(c);
    }
    return result;
}
std::string Format(const Record& record) {
    auto time = std::chrono::system_clock::to_time_t(record.time);
    std::tm date{};
#ifdef _WIN32
    gmtime_s(&date, &time);
#else
    gmtime_r(&time, &date);
#endif
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(record.time.time_since_epoch()).count() % 1000;
    static constexpr const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    std::ostringstream line;
    line << std::put_time(&date, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << milliseconds
         << "Z " << names[static_cast<unsigned>(record.level)] << " pid=" << ProcessId() << " tid=" << std::hex << record.thread << std::dec
         << " [" << Sanitize(record.component) << "] " << Sanitize(record.message) << '\n';
    return line.str();
}
class Logger {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Record> queue_;
    Config config_;
    std::thread worker_;
    bool running_ = false, stopping_ = false;
    std::atomic<std::size_t> dropped_{0};
    void Work() noexcept {
        try {
            std::error_code error;
            std::filesystem::create_directories(config_.directory, error);
            if (error) return;
#ifndef _WIN32
            chmod(config_.directory.c_str(), 0700);
#endif
            auto current = config_.directory / "remote-native.log";
            FileLock sharedLock(config_.directory / ".remote-native.lock");
            std::ofstream file;
            std::uintmax_t bytes = 0;
            auto open = [&] {
                file.clear();
                file.open(current, std::ios::binary | std::ios::app);
                error.clear(); bytes = std::filesystem::file_size(current, error); if (error) bytes = 0;
#ifndef _WIN32
                chmod(current.c_str(), 0600);
#endif
            };
            auto rotate = [&] {
                file.close();
                for (std::size_t index = config_.retainedFiles; index > 1; --index) {
                    auto target = config_.directory / ("remote-native." + std::to_string(index - 1) + ".log");
                    auto source = index == 2 ? current : config_.directory / ("remote-native." + std::to_string(index - 2) + ".log");
                    error.clear(); std::filesystem::remove(target, error);
                    error.clear(); std::filesystem::rename(source, target, error);
                }
                if (config_.retainedFiles == 1) { error.clear(); std::filesystem::remove(current, error); }
                open();
            };
            auto write = [&](const Record& record) {
                auto line = Format(record);
                if (bytes && bytes + line.size() > config_.maxFileBytes) rotate();
                // A viewer or antivirus can deny a rename. Drop metadata rather
                // than growing an unbounded file while rotation is unavailable.
                if (file && bytes + line.size() <= config_.maxFileBytes) { file.write(line.data(), static_cast<std::streamsize>(line.size())); bytes += line.size(); }
                else ++dropped_;
            };
            for (;;) {
                std::deque<Record> records;
                bool finish = false;
                {
                    std::unique_lock lock(mutex_);
                    ready_.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping_ || !queue_.empty(); });
                    records.swap(queue_); finish = stopping_;
                }
                auto dropped = dropped_.exchange(0);
                if (!records.empty() || dropped) {
                    if (sharedLock.Acquire()) {
                        struct Unlock { FileLock& lock; ~Unlock() { lock.Release(); } } unlock{sharedLock};
                        // Open only while holding the cross-process lock. This
                        // keeps append and rotation consistent when diagnostics
                        // and the main application run at the same time.
                        open();
                        if (dropped) write({std::chrono::system_clock::now(), 0, Level::Warning, "diagnostics", "dropped_records=" + std::to_string(dropped)});
                        for (const auto& record : records) write(record);
                        file.flush(); file.close();
                    } else dropped_ += records.size() + dropped;
                }
                if (finish) break;
            }
        } catch (...) { /* Diagnostics must never break the application. */ }
    }
public:
    ~Logger() { Stop(); }
    bool Start(Config config) {
        std::lock_guard lock(mutex_);
        if (running_) return true;
        if (config.directory.empty()) config.directory = DefaultDirectory();
        config.maxFileBytes = std::clamp<std::size_t>(config.maxFileBytes, 4096, 16 * 1024 * 1024);
        config.retainedFiles = std::clamp<std::size_t>(config.retainedFiles, 1, 8);
        config.maxQueuedRecords = std::clamp<std::size_t>(config.maxQueuedRecords, 8, 4096);
        config_ = std::move(config); stopping_ = false; dropped_ = 0;
        worker_ = std::thread([this] { Work(); }); running_ = true;
        return true;
    }
    void Add(Level level, std::string_view component, std::string_view message) {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) { ++dropped_; return; }
        if (!running_ || stopping_) return;
        if (queue_.size() >= config_.maxQueuedRecords) { ++dropped_; return; }
        queue_.push_back({std::chrono::system_clock::now(), std::hash<std::thread::id>{}(std::this_thread::get_id()), level,
            Limited(component, 64), Limited(message, MaxRecordBytes)});
        lock.unlock(); ready_.notify_one();
    }
    std::filesystem::path Path() { std::lock_guard lock(mutex_); return config_.directory.empty() ? DefaultDirectory() : config_.directory; }
    void Stop() noexcept {
        try {
            {
                std::lock_guard lock(mutex_);
                if (!running_) return;
                stopping_ = true;
            }
            ready_.notify_one(); if (worker_.joinable()) worker_.join();
            std::lock_guard lock(mutex_); running_ = false; queue_.clear();
        } catch (...) {}
    }
};
// Detached system data providers can finish after the main lifecycle drains
// the writer. Keep the stopped singleton valid until process reclamation.
Logger& Instance() { static Logger* logger = new Logger; return *logger; }
}
bool Initialize(Config config) noexcept { try { return Instance().Start(std::move(config)); } catch (...) { return false; } }
void Write(Level level, std::string_view component, std::string_view message) noexcept { try { Instance().Add(level, component, message); } catch (...) {} }
std::filesystem::path Directory() { return Instance().Path(); }
void Shutdown() noexcept { Instance().Stop(); }
const char* ErrorClass(std::string_view message) noexcept {
    auto contains = [&](std::string_view term) {
        for (std::size_t at = 0; at < message.size(); ++at) if (StartsInsensitive(message, at, term)) return true;
        return false;
    };
    if (contains("timeout") || contains("timed out") || contains("超时")) return "timeout";
    if (contains("permission") || contains("access denied") || contains("权限")) return "permission";
    if (contains("hash") || contains("校验")) return "integrity";
    if (contains("path") || contains("filename") || contains("路径")) return "path";
    if (contains("cancel") || contains("取消")) return "cancelled";
    if (contains("pair") || contains("token") || contains("proof") || contains("鉴权") || contains("配对")) return "authentication";
    if (contains("connect") || contains("transport") || contains("network") || contains("连接")) return "transport";
    return message.empty() ? "none" : "other";
}
}
