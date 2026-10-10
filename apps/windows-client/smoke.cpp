#include "../../shared/i18n.h"
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "smoke.h"
#include "session.h"
#include "../../shared/exchange.h"
#include "../../shared/protocol.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kSmokeClass[] = L"RemoteNativeDiagnostic";
struct SmokeContext {
    IClientSession* session = nullptr;
    uint32_t videoWidth = 1920, videoHeight = 1080;
    bool closed = false;
};
void UpdateViewport(HWND window, SmokeContext& context) {
    RECT bounds {}; GetClientRect(window, &bounds);
    if (!context.videoWidth || !context.videoHeight || bounds.right <= 0 || bounds.bottom <= 0) return;
    double scale = std::min(static_cast<double>(bounds.right) / context.videoWidth,
                            static_cast<double>(bounds.bottom) / context.videoHeight);
    LONG width = static_cast<LONG>(context.videoWidth * scale), height = static_cast<LONG>(context.videoHeight * scale);
    RECT video {(bounds.right - width) / 2, (bounds.bottom - height) / 2,
                (bounds.right + width) / 2, (bounds.bottom + height) / 2};
    context.session->ResizeViewport(video);
}
LRESULT CALLBACK SmokeWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto context = reinterpret_cast<SmokeContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        context = static_cast<SmokeContext*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }
    if (!context) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_SIZE: UpdateViewport(window, *context); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT paint {}; BeginPaint(window, &paint); EndPaint(window, &paint); return 0; }
    case WM_CLOSE: context->closed = true; return 0;
    // This window deliberately does not forward mouse, keyboard or IME input.
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
std::string Escape(const std::string& input) {
    std::ostringstream output;
    for (unsigned char c : input) {
        switch (c) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (c < 32) output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
            else output << static_cast<char>(c);
        }
    }
    return output.str();
}
const char* StateName(SessionState state) {
    switch (state) {
    case SessionState::Idle: return "idle";
    case SessionState::Connecting: return "connecting";
    case SessionState::Connected: return "connected";
    case SessionState::Error: return "error";
    }
    return "unknown";
}
std::string Number(double value) {
    if (!std::isfinite(value) || value < 0) return "null";
    std::ostringstream output; output << std::fixed << std::setprecision(3) << value;
    return output.str();
}
bool WriteReport(const wchar_t* path, const std::string& contents) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) && written == contents.size();
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file); return ok;
}
struct Sample { double at; SessionSnapshot snapshot; };
} // namespace

int RunConnectionSmoke(HINSTANCE instance, unsigned seconds, const wchar_t* reportPath, const std::string& addressOverride) {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SessionSnapshot final;
    std::vector<Sample> samples;
    std::string reason;
    bool connectAccepted = false, sawHardwareFrame = false, sawCursor = false, cursorImage = false;
    bool cursorHidden = false, cursorBaked = false, closed = false;
    uint64_t maximumFrames = 0;
    double firstHardwareMs = -1, elapsedMs = 0;
    // Optional load probe uses its own fixture and an explicit destination; it
    // never reads the user's clipboard or forwards desktop input.
    wchar_t transferDirectory[4096] {};
    DWORD directoryLength = GetEnvironmentVariableW(L"REMOTE_SMOKE_TRANSFER_DIRECTORY", transferDirectory, 4096);
    bool transferRequested = directoryLength > 0 && directoryLength < 4096;
    uint64_t transferId = 0, transferBytes = 0, transferTotal = 64 * 1024 * 1024;
    double transferStartMs = -1, transferCompleteMs = -1;
    remote_exchange::TransferState transferState = remote_exchange::TransferState::Queued;
    std::string transferError, destination;
    std::filesystem::path fixture;
    if (transferRequested) {
        int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, transferDirectory, directoryLength, nullptr, 0, nullptr, nullptr);
        if (length > 0) {
            destination.resize(length);
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, transferDirectory, directoryLength, destination.data(), length, nullptr, nullptr);
        }
        std::error_code error;
        auto fixtureDirectory = std::filesystem::absolute(std::filesystem::path(reportPath), error).parent_path() / "video-file-fixture";
        std::filesystem::create_directories(fixtureDirectory, error);
        fixture = fixtureDirectory / ("video-load-" + std::to_string(GetTickCount64()) + ".bin");
        std::ofstream file(fixture, std::ios::binary);
        std::vector<char> block(64 * 1024);
        for (size_t index = 0; index < block.size(); ++index) block[index] = static_cast<char>((index * 31 + 17) % 251);
        for (uint64_t written = 0; written < transferTotal && file; written += block.size()) file.write(block.data(), block.size());
        if (error || destination.empty() || !file) reason = "load_fixture_creation_failed";
    }
    auto session = CreateClientSession();
    HWND window = nullptr;
    SmokeContext context;
    uint64_t started = GetTickCount64();
    if (!session) reason = "session_creation_failed";
    else {
        context.session = session.get();
        WNDCLASSEXW klass {}; klass.cbSize = sizeof klass; klass.hInstance = instance; klass.lpfnWndProc = SmokeWndProc;
        klass.lpszClassName = kSmokeClass; klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) reason = "window_registration_failed";
        else {
            UINT dpi = GetDpiForSystem(); RECT size {0, 0, MulDiv(960, dpi, 96), MulDiv(540, dpi, 96)};
            AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
            window = CreateWindowExW(0, kSmokeClass, remote_i18n::Text(L"Remote \u00b7 视频链路检测"), WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top,
                nullptr, nullptr, instance, &context);
            if (!window) reason = "window_creation_failed";
            else {
                ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
                ConnectionOptions options = LoadConnectionOptions();
                if(!addressOverride.empty()){options.address=addressOverride;options.persistSettings=false;}
                connectAccepted = session->Connect(window, options);
                // Credentials remain solely in the connection service; the report never serializes options.
                std::fill(options.token.begin(), options.token.end(), '\0');
                UpdateViewport(window, context);
                if (!connectAccepted) { final = session->Snapshot(); reason = "connection_rejected"; }
                else {
                    started = GetTickCount64();
                    uint64_t nextSample = started;
                    while (GetTickCount64() - started < static_cast<uint64_t>(seconds) * 1000 && !context.closed) {
                        MSG message {};
                        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                            if (message.message == WM_QUIT) { context.closed = true; break; }
                            TranslateMessage(&message); DispatchMessageW(&message);
                        }
                        session->Tick(); final = session->Snapshot();
                        uint64_t now = GetTickCount64();
                        maximumFrames = std::max(maximumFrames, final.actualFrames);
                        if (final.hardwareDecode && final.actualFrames > 0 && !sawHardwareFrame) {
                            sawHardwareFrame = true; firstHardwareMs = static_cast<double>(now - started);
                        }
                        if (transferRequested && sawHardwareFrame) {
                            auto exchange = session->Exchange();
                            if (exchange) {
                                auto sharing = exchange->GetSnapshot();
                                if (!transferId && sharing.connected) {
                                    transferId = exchange->SendFiles({fixture}, destination);
                                    transferStartMs = static_cast<double>(now - started);
                                }
                                for (const auto& task : sharing.transfers) if (task.id == transferId) {
                                    transferState = task.state; transferBytes = task.bytes; transferError = task.error;
                                    if (task.state == remote_exchange::TransferState::Complete && transferCompleteMs < 0)
                                        transferCompleteMs = static_cast<double>(now - started);
                                }
                            }
                        }
                        if (context.videoWidth != final.videoWidth || context.videoHeight != final.videoHeight) {
                            context.videoWidth = final.videoWidth; context.videoHeight = final.videoHeight; UpdateViewport(window, context);
                        }
                        std::vector<uint8_t> packet;
                        while (session->TakeCursor(packet)) {
                            // Only inspect cursor metadata; never synthesize or send user input.
                            if (packet.size() >= sizeof(RemoteCursorHeader)) {
                                RemoteCursorHeader header {}; std::memcpy(&header, packet.data(), sizeof header);
                                sawCursor = true; cursorHidden = (header.flags & 1) != 0; cursorBaked = (header.flags & 2) != 0;
                                if (header.width > 0 && header.height > 0 && header.width <= 256 && header.height <= 256 &&
                                    header.rgba_bytes == static_cast<uint64_t>(header.width) * header.height * 4 &&
                                    packet.size() == sizeof header + header.rgba_bytes) cursorImage = true;
                            }
                        }
                        if (now >= nextSample) { samples.push_back({static_cast<double>(now - started), final}); nextSample = now + 500; }
                        if (final.state == SessionState::Error) { reason = "session_error"; break; }
                        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                    }
                    elapsedMs = static_cast<double>(GetTickCount64() - started);
                    final = session->Snapshot(); maximumFrames = std::max(maximumFrames, final.actualFrames);
                    if (final.hardwareDecode && final.actualFrames > 0 && !sawHardwareFrame) {
                        sawHardwareFrame = true; firstHardwareMs = elapsedMs;
                    }
                    closed = context.closed;
                    if (reason.empty() && closed) reason = "diagnostic_window_closed";
                    if (reason.empty() && !sawHardwareFrame) reason = "no_hardware_decoded_frame";
                    if (reason.empty() && transferRequested && transferState != remote_exchange::TransferState::Complete)
                        reason = "load_transfer_incomplete";
                }
            }
        }
    }
    // Retain the last real snapshot before Disconnect resets counters and state.
    bool passed = connectAccepted && sawHardwareFrame && final.state == SessionState::Connected && !closed && reason.empty();
    std::ostringstream json;
    json << "{\n  \"schema\": \"remote.native-smoke.v1\",\n  \"passed\": " << (passed ? "true" : "false")
         << ",\n  \"reason\": \"" << Escape(reason) << "\",\n  \"requested_seconds\": " << seconds
         << ",\n  \"elapsed_ms\": " << Number(elapsedMs) << ",\n  \"state\": \"" << StateName(final.state)
         << "\",\n  \"message\": \"" << Escape(final.message) << "\",\n  \"connect_accepted\": " << (connectAccepted ? "true" : "false")
         << ",\n  \"hardware_decode\": " << (final.hardwareDecode ? "true" : "false")
         << ",\n  \"saw_hardware_frame\": " << (sawHardwareFrame ? "true" : "false")
         << ",\n  \"actual_presented_frames\": " << maximumFrames << ",\n  \"first_hardware_frame_ms\": " << Number(firstHardwareMs)
         << ",\n  \"video_width\": " << final.videoWidth << ",\n  \"video_height\": " << final.videoHeight
         << ",\n  \"fps\": " << Number(final.fps) << ",\n  \"bitrate_mbps\": " << Number(final.bitrateMbps)
         << ",\n  \"network_rtt_ms\": " << Number(final.networkMs) << ",\n  \"decode_present_ms\": " << Number(final.decodeMs)
         << ",\n  \"decode_present_measurement\": \"CPU wall-clock time across decode, GPU conversion submission and DXGI Present; includes any presentation wait, not a GPU duration\""
         << ",\n  \"queued_video_frames\": " << final.queuedFrames
         << ",\n  \"file_load_requested\": " << (transferRequested ? "true" : "false")
         << ",\n  \"file_load_state\": \"" << remote_exchange::StateLabel(transferState) << "\""
         << ",\n  \"file_load_bytes\": " << transferBytes
         << ",\n  \"file_load_total\": " << (transferRequested ? transferTotal : 0)
         << ",\n  \"file_load_started_ms\": " << Number(transferStartMs)
         << ",\n  \"file_load_completed_ms\": " << Number(transferCompleteMs)
         << ",\n  \"file_load_name\": \"" << Escape(fixture.filename().string()) << "\""
         << ",\n  \"file_load_error\": \"" << Escape(transferError) << "\""
         << ",\n  \"cursor_packet_received\": " << (sawCursor ? "true" : "false")
         << ",\n  \"cursor_image_received\": " << (cursorImage ? "true" : "false")
         << ",\n  \"cursor_hidden\": " << (cursorHidden ? "true" : "false")
         << ",\n  \"cursor_baked_in_video\": " << (cursorBaked ? "true" : "false") << ",\n  \"samples\": [";
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i]; const auto& snapshot = sample.snapshot;
        json << (i ? "," : "") << "\n    {\"at_ms\":" << Number(sample.at) << ",\"state\":\"" << StateName(snapshot.state)
             << "\",\"actual_frames\":" << snapshot.actualFrames << ",\"hardware_decode\":" << (snapshot.hardwareDecode ? "true" : "false")
             << ",\"fps\":" << Number(snapshot.fps) << ",\"network_rtt_ms\":" << Number(snapshot.networkMs)
             << ",\"decode_present_ms\":" << Number(snapshot.decodeMs) << ",\"queued_frames\":" << snapshot.queuedFrames << "}";
    }
    json << "\n  ]\n}\n";
    bool written = WriteReport(reportPath, json.str());
    if (session) session->Disconnect();
    if (window) DestroyWindow(window);
    session.reset();
    if (SUCCEEDED(initialized)) CoUninitialize();
    return !written ? 6 : passed ? 0 : 5;
}
