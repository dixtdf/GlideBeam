#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
namespace remote_exchange { class Session; }

struct ConnectionOptions {
    std::string address;
    std::string token;
    std::string fingerprint;
    bool persistSettings = true;
};
enum class SessionState { Idle, Connecting, Connected, Error };
struct SessionSnapshot {
    SessionState state = SessionState::Idle;
    std::string message;
    // Empty for legacy hosts; otherwise running/starting/suspended/user-stopped/...
    std::string captureState;
    uint32_t videoWidth = 1920, videoHeight = 1080;
    // decodeMs measures CPU wall-clock time across decode, conversion and Present.
    double fps = 0, bitrateMbps = 0, networkMs = -1, decodeMs = -1;
    uint32_t queuedFrames = 0;
    uint64_t actualFrames = 0;
    bool hardwareDecode = false;
    bool restartSupported = false;
    bool restartRequiresLocalAuthorization = true;
    bool restartBusy = false;
    std::string restartState, restartMessage, restartRequestId;
};
class IClientSession {
public:
    virtual ~IClientSession() = default;
    virtual bool Connect(HWND canvas, const ConnectionOptions& options) = 0;
    virtual void Disconnect() = 0;
    virtual void SendInput(const uint8_t* data, size_t size) = 0;
    // Tick and Snapshot execute on the UI thread and must not block.
    virtual void Tick() = 0;
    virtual SessionSnapshot Snapshot() const = 0;
    // Coordinates are canvas-client physical pixels, excluding letterboxes.
    virtual void ResizeViewport(const RECT& videoRect) = 0;
    virtual bool TakeCursor(std::vector<uint8_t>& packet) = 0;
    virtual std::shared_ptr<remote_exchange::Session> Exchange() const { return {}; }
    // UI presents its confirmation dialog before calling. The authenticated
    // host performs its own local macOS authorization.
    virtual bool RestartMac() { return false; }
};
int CreateUi(HINSTANCE instance, int showCommand, IClientSession& session,
             const ConnectionOptions& initialOptions = {}, bool autoConnect = false);
// Renders the same native Direct2D interface to a PNG for visual verification.
enum class UiPreview { Landing, Connected, Fullscreen, ConnectedHidden, FullscreenHidden, Preferences };
bool RenderUiPreview(const wchar_t* path, UiPreview mode = UiPreview::Landing);
// Own-window, synthetic-session native layout/input checks. No network or credentials.
bool RunUiSelfTest(HINSTANCE instance, const wchar_t* reportPath);
