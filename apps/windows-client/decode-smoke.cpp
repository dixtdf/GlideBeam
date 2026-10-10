#include "../../shared/i18n.h"
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "decode-smoke.h"
#include "video.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint64_t kMaximumFile = 64ULL * 1024 * 1024;
constexpr uint32_t kMaximumRecord = 8U * 1024 * 1024;
constexpr wchar_t kClass[] = L"RemoteNativeOfflineDecoder";
static_assert(sizeof(RemoteFrameHeader) == 32);
struct WindowState { std::atomic<bool> closed {false}; };
struct FrameResult {
    uint64_t sequence = 0, presented = 0;
    uint32_t bytes = 0;
    double decodePresentMs = -1, callWallMs = -1;
    bool accepted = false;
};
struct Result {
    std::string reason, decoderError;
    bool initialized = false, hardware = false, completed = false;
    bool finishCalled = false, finishSucceeded = false;
    uint64_t drainedFrames = 0;
    uint64_t inputBytes = 0, submitted = 0, presented = 0;
    uint32_t width = 0, height = 0, win32Error = 0;
    double elapsedMs = 0, finishWallMs = -1;
    std::vector<FrameResult> frames;
};
LRESULT CALLBACK TestWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_CLOSE: state->closed = true; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT paint {}; BeginPaint(window, &paint); EndPaint(window, &paint); return 0; }
    // This diagnostic window never forwards input to any other application.
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR: return 0;
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
std::string Number(double value) {
    if (!std::isfinite(value) || value < 0) return "null";
    std::ostringstream text; text << std::fixed << std::setprecision(3) << value; return text.str();
}
bool ReadExact(HANDLE file, void* output, uint32_t bytes, uint32_t& error) {
    DWORD received = 0;
    if (!ReadFile(file, output, bytes, &received, nullptr)) { error = GetLastError(); return false; }
    if (received != bytes) { error = ERROR_HANDLE_EOF; return false; }
    return true;
}
void SetViewport(HWND window, VideoDecoder& decoder, uint32_t width, uint32_t height) {
    RECT bounds {}; GetClientRect(window, &bounds);
    double scale = std::min(static_cast<double>(std::max(1L, bounds.right)) / width,
                            static_cast<double>(std::max(1L, bounds.bottom)) / height);
    LONG w = static_cast<LONG>(width * scale), h = static_cast<LONG>(height * scale);
    RECT view {(bounds.right - w) / 2, (bounds.bottom - h) / 2,
               (bounds.right + w) / 2, (bounds.bottom + h) / 2};
    decoder.SetViewport(view);
}
void DecodeFixture(HWND window, const wchar_t* path, WindowState& state, Result& result) {
    auto started = std::chrono::steady_clock::now();
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { result.reason = "open_fixture_failed"; result.win32Error = GetLastError(); }
    else {
        LARGE_INTEGER size {};
        if (!GetFileSizeEx(file, &size)) { result.reason = "fixture_size_failed"; result.win32Error = GetLastError(); }
        else if (size.QuadPart <= 0 || static_cast<uint64_t>(size.QuadPart) > kMaximumFile) result.reason = "fixture_size_out_of_range";
        else {
            result.inputBytes = static_cast<uint64_t>(size.QuadPart);
            std::unique_ptr<VideoDecoder> decoder;
            uint64_t position = 0;
            std::vector<uint8_t> record;
            while (position < result.inputBytes && !state.closed.load()) {
                if (result.inputBytes - position < 4) { result.reason = "truncated_record_length"; break; }
                uint8_t lengthBytes[4] {};
                if (!ReadExact(file, lengthBytes, 4, result.win32Error)) { result.reason = "read_record_length_failed"; break; }
                position += 4;
                uint32_t length = lengthBytes[0] | (static_cast<uint32_t>(lengthBytes[1]) << 8) |
                    (static_cast<uint32_t>(lengthBytes[2]) << 16) | (static_cast<uint32_t>(lengthBytes[3]) << 24);
                if (length <= sizeof(RemoteFrameHeader) || length > kMaximumRecord) { result.reason = "record_length_out_of_range"; break; }
                if (length > result.inputBytes - position) { result.reason = "truncated_record_payload"; break; }
                record.resize(length);
                if (!ReadExact(file, record.data(), length, result.win32Error)) { result.reason = "read_record_payload_failed"; break; }
                position += length;
                RemoteFrameHeader frame {}; std::memcpy(&frame, record.data(), sizeof frame);
                if (frame.magic != 0x31524652 || frame.width < 64 || frame.height < 64 || frame.width > 7680 || frame.height > 4320) {
                    result.reason = "invalid_frame_header"; break;
                }
                if ((frame.flags & ~1u) || frame.sequence > static_cast<uint64_t>(LLONG_MAX / 166667)) {
                    result.reason = "invalid_frame_flags_or_sequence"; break;
                }
                if (!decoder) {
                    result.width = frame.width; result.height = frame.height;
                    decoder = std::make_unique<VideoDecoder>();
                    if (!decoder->Initialize(window, frame.width, frame.height)) {
                        result.reason = "gpu_decoder_initialization_failed"; result.decoderError = decoder->Error(); break;
                    }
                    result.initialized = true; result.hardware = decoder->Hardware();
                } else if (frame.width != result.width || frame.height != result.height) {
                    result.reason = "fixture_resolution_changed"; break;
                }
                SetViewport(window, *decoder, frame.width, frame.height);
                uint64_t before = decoder->Presented();
                auto callStarted = std::chrono::steady_clock::now();
                bool accepted = decoder->Decode(frame, record.data() + sizeof frame, record.size() - sizeof frame);
                double callWallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - callStarted).count();
                ++result.submitted;
                result.hardware = decoder->Hardware(); result.presented = decoder->Presented();
                result.frames.push_back({frame.sequence, result.presented - before, length, decoder->LastDecodeMs(), callWallMs, accepted});
                if (!accepted) { result.reason = "gpu_decode_or_present_failed"; result.decoderError = decoder->Error(); break; }
            }
            if (state.closed.load() && result.reason.empty()) result.reason = "diagnostic_window_closed";
            result.completed = result.reason.empty() && position == result.inputBytes;
            if (result.completed && decoder) {
                result.finishCalled = true;
                uint64_t before = decoder->Presented();
                auto finishStarted = std::chrono::steady_clock::now();
                result.finishSucceeded = decoder->Finish();
                result.finishWallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - finishStarted).count();
                result.presented = decoder->Presented();
                result.drainedFrames = result.presented - before;
                result.hardware = decoder->Hardware();
                if (!result.finishSucceeded) {
                    result.reason = "gpu_decoder_finish_failed";
                    result.decoderError = decoder->Error();
                }
            }
            if (result.completed && result.reason.empty() && !result.presented) result.reason = "no_presented_frame";
            if (result.completed && result.reason.empty() && !result.hardware) result.reason = "hardware_decoder_not_used";
            // Destroy decoder/Media Foundation resources while COM remains initialized on this worker.
            decoder.reset();
        }
        CloseHandle(file);
    }
    result.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (SUCCEEDED(com)) CoUninitialize();
}
std::string HResultString(const std::string& error) {
    auto at = error.rfind("(0x");
    if (at == std::string::npos || at + 11 > error.size()) return {};
    std::string value = error.substr(at + 1, 10);
    for (size_t i = 2; i < value.size(); ++i) if (!std::isxdigit(static_cast<unsigned char>(value[i]))) return {};
    return value;
}
bool Save(const wchar_t* path, const std::string& text) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file); return ok;
}
} // namespace

int RunDecodeSmoke(HINSTANCE instance, const wchar_t* inputPath, const wchar_t* reportPath) {
    Result result;
    WindowState state;
    WNDCLASSEXW klass {}; klass.cbSize = sizeof klass; klass.hInstance = instance; klass.lpfnWndProc = TestWndProc;
    klass.lpszClassName = kClass; klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    HWND window = nullptr;
    if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        result.reason = "window_registration_failed"; result.win32Error = GetLastError();
    } else {
        UINT dpi = GetDpiForSystem(); RECT size {0, 0, MulDiv(960, dpi, 96), MulDiv(540, dpi, 96)};
        AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        window = CreateWindowExW(0, kClass, remote_i18n::Text(L"Remote \u00b7 离线 GPU 解码检测"), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top,
            nullptr, nullptr, instance, &state);
        if (!window) { result.reason = "window_creation_failed"; result.win32Error = GetLastError(); }
        else {
            ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
            std::atomic<bool> done {false};
            std::thread worker([&] {
                try { DecodeFixture(window, inputPath, state, result); }
                catch (const std::exception& error) { result.reason = "decoder_worker_exception"; result.decoderError = error.what(); }
                catch (...) { result.reason = "decoder_worker_exception"; }
                done = true;
            });
            while (!done.load()) {
                MSG message {};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) { state.closed = true; break; }
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            worker.join();
        }
    }
    bool passed = result.completed && result.reason.empty() && result.initialized && result.hardware && result.presented > 0;
    std::string hresult = HResultString(result.decoderError);
    std::ostringstream json;
    json << "{\n  \"schema\": \"remote.offline-decode.v1\",\n  \"passed\": " << (passed ? "true" : "false")
         << ",\n  \"reason\": \"" << Escape(result.reason) << "\",\n  \"decoder_error\": \"" << Escape(result.decoderError)
         << "\",\n  \"error_hresult\": " << (hresult.empty() ? "null" : "\"" + hresult + "\"")
         << ",\n  \"win32_error\": " << result.win32Error << ",\n  \"input_bytes\": " << result.inputBytes
         << ",\n  \"fixture_completed\": " << (result.completed ? "true" : "false")
         << ",\n  \"decoder_initialized\": " << (result.initialized ? "true" : "false")
         << ",\n  \"hardware_decode\": " << (result.hardware ? "true" : "false")
         << ",\n  \"submitted_records\": " << result.submitted << ",\n  \"actual_presented_frames\": " << result.presented
         << ",\n  \"finish_called\": " << (result.finishCalled ? "true" : "false")
         << ",\n  \"finish_succeeded\": " << (result.finishSucceeded ? "true" : "false")
         << ",\n  \"finish_presented_frames\": " << result.drainedFrames << ",\n  \"finish_cpu_wall_ms\": " << Number(result.finishWallMs)
         << ",\n  \"video_width\": " << result.width << ",\n  \"video_height\": " << result.height
         << ",\n  \"elapsed_cpu_wall_ms\": " << Number(result.elapsedMs)
         << ",\n  \"measurement\": \"CPU wall-clock across production decode, conversion submission and DXGI Present; includes presentation waiting, not GPU time or live streaming FPS\""
         << ",\n  \"frames\": [";
    for (size_t i = 0; i < result.frames.size(); ++i) {
        const auto& frame = result.frames[i];
        json << (i ? "," : "") << "\n    {\"sequence\":" << frame.sequence << ",\"record_bytes\":" << frame.bytes
             << ",\"accepted\":" << (frame.accepted ? "true" : "false") << ",\"presented_frames\":" << frame.presented
             << ",\"decode_present_ms\":" << Number(frame.decodePresentMs) << ",\"call_cpu_wall_ms\":" << Number(frame.callWallMs) << "}";
    }
    json << "\n  ]\n}\n";
    bool written = Save(reportPath, json.str());
    if (window) DestroyWindow(window);
    return !written ? 6 : passed ? 0 : 5;
}
