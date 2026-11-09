#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace remote_update {

enum class Stage { Idle, Checking, UpToDate, Available, Downloading, Ready, Installing, Error };
struct Snapshot {
    Stage stage = Stage::Idle;
    uint64_t revision = 0;
    std::string currentVersion;
    std::string availableVersion;
    uint64_t downloadedBytes = 0;
    uint64_t totalBytes = 0;
    std::filesystem::path installerPath;
    std::string error;
};
struct ReleaseInfo {
    std::string version;
    std::string architecture;
    std::string assetName;
    std::string assetUrl;
    std::string sumsUrl;
};

// Semantic-version comparison: -1/0/1 for ordering, -2 for invalid input.
int CompareVersions(const std::string& left, const std::string& right);
std::string ExpectedAssetName(const std::string& version, const std::string& architecture);
std::string ExpectedSha256(const std::string& manifest, const std::string& assetName);
std::string Sha256File(const std::filesystem::path& path);
bool ParseRelease(const std::string& json, const std::string& architecture,
                  ReleaseInfo& info, std::string& error);
std::string CurrentVersion();

class Updater {
public:
    Updater();
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;
    void Check();
    void Download();
    // Starts the already verified Setup. The caller exits only when this returns true.
    bool Install(HWND owner);
    Snapshot GetSnapshot() const;
private:
    struct State;
    static void Fail(const std::shared_ptr<State>& state, const std::string& error);
    std::shared_ptr<State> state_;
};

// Offline semantic/manifest/release/hash tests. No network or Setup execution.
bool RunUpdaterSelfTest(const wchar_t* reportPath);

} // namespace remote_update
