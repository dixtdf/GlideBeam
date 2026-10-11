#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "ui.h"
#include "language.h"
#include "exchange-windows.h"
#include "resources.h"
#include "ui-watchdog.h"
#include "../../shared/diagnostic-log.h"
#include "../../shared/protocol.h"
#include "cursor-native.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
template<class T> void Release(T*& value) { if (value) { value->Release(); value = nullptr; } }
std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring value(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value.data(), size);
    return value;
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string value(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value.data(), size, nullptr, nullptr);
    return value;
}
std::wstring EditText(HWND edit) {
    int size = GetWindowTextLengthW(edit);
    std::wstring value(size + 1, L'\0');
    GetWindowTextW(edit, value.data(), size + 1);
    value.resize(size);
    return value;
}
enum class Action { Connect, KeyboardMode, Fullscreen, Disconnect, Sessions, Settings, Metrics, Clipboard, Files, More, LanguageSystem, LanguageEnglish, LanguageChinese };
struct Hit { D2D1_RECT_F rect; Action action; };
constexpr uint32_t BG = 0x0b1118, NAV = 0x101720, PANEL = 0x121c27;
constexpr uint32_t LINE = 0x24313f, WHITE = 0xebf2fa, MUTED = 0x8b9aad;
constexpr uint32_t ACCENT = 0x68e7c1, SOFT = 0x173d38;
constexpr UINT kPollTimer = 1;
constexpr UINT kConnectMessage = WM_APP + 1;
constexpr wchar_t kWindowClass[] = L"RemoteNativeWindow";
constexpr wchar_t kCanvasClass[] = L"RemoteNativeCanvas";
constexpr wchar_t kOverlayClass[] = L"RemoteNativeSessionOverlay";
constexpr float kToolbarWidth = 762.f, kToolbarHeight = 64.f;
constexpr float kMetricsWidth = 400.f, kMetricsHeight = 456.f;
constexpr float kCaptureNoticeWidth = 480.f, kCaptureNoticeHeight = 172.f;
constexpr float kNavWidth = 206.f;
const wchar_t* MacEncoderText(const SessionSnapshot& snapshot) {
    if (snapshot.state != SessionState::Connected) return L"等待主机报告";
    switch (snapshot.macEncoderMode) {
    case MacVideoEncoderMode::Hardware: return L"硬件 H.264";
    case MacVideoEncoderMode::AppleSoftware: return L"Apple 软件 H.264";
    case MacVideoEncoderMode::Software: return L"软件 H.264";
    default: return L"等待主机报告";
    }
}
struct ComApartment {
    HRESULT result = OleInitialize(nullptr);
    ~ComApartment() { if (SUCCEEDED(result)) OleUninitialize(); }
};

class Painter {
public:
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* write = nullptr;
    ID2D1RenderTarget* target = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1Bitmap* brandIcon = nullptr;
    std::unordered_map<unsigned, IDWriteTextFormat*> fonts;
    ~Painter() { ResetTarget(); for (auto& entry : fonts) Release(entry.second); Release(write); Release(factory); }
    bool Init() {
        return SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory)) &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&write)));
    }
    void ResetTarget() { Release(brandIcon); Release(brush); Release(target); }
    bool SetTarget(ID2D1RenderTarget* value) {
        ResetTarget(); target = value;
        return SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush));
    }
    void Color(uint32_t value, float alpha = 1.f) {
        brush->SetColor(D2D1::ColorF(value, alpha));
    }
    void Rect(float x, float y, float w, float h, uint32_t color) {
        Color(color); target->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush);
    }
    void Round(float x, float y, float w, float h, float radius, uint32_t fill, uint32_t border = 0) {
        auto rect = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius);
        Color(fill); target->FillRoundedRectangle(rect, brush);
        if (border) { Color(border); target->DrawRoundedRectangle(rect, brush, 1.f); }
    }
    void Line(float x1, float y1, float x2, float y2, uint32_t color, float width = 1.f) {
        Color(color); target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, width);
    }
    void Dot(float x, float y, float radius, uint32_t color) {
        Color(color); target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), brush);
    }
    void Text(const std::wstring& source, float x, float y, float w, float h, float size,
              uint32_t color = WHITE, bool bold = false, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
        const auto text = remote_i18n::Translate(source);
        bool chinese = std::any_of(text.begin(), text.end(), [](wchar_t c) { return c >= 0x2e80; });
        unsigned key = static_cast<unsigned>(size * 10) * 4 + (bold ? 2 : 0) + (chinese ? 1 : 0);
        auto& font = fonts[key];
        if (!font) {
            write->CreateTextFormat(chinese ? L"Microsoft YaHei UI" : L"Segoe UI Variable", nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &font);
            if (!font) return;
            font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        font->SetTextAlignment(alignment);
        Color(color);
        target->DrawText(text.data(), static_cast<UINT32>(text.size()), font, D2D1::RectF(x, y, x + w, y + h), brush,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void Paragraph(const std::wstring& source, float x, float y, float w, float h, float size, uint32_t color) {
        const auto text=remote_i18n::Translate(source);
        const bool chinese=remote_i18n::EffectiveLanguage()==remote_i18n::Language::SimplifiedChinese;
        IDWriteTextFormat* format = nullptr;
        if (FAILED(write->CreateTextFormat(chinese?L"Microsoft YaHei UI":L"Segoe UI Variable", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, chinese?L"zh-cn":L"en-us", &format))) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        Color(color);
        target->DrawText(text.data(), static_cast<UINT32>(text.size()), format,
            D2D1::RectF(x, y, x + w, y + h), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        format->Release();
    }
    bool BrandIcon(float x, float y, float size) {
        if (!brandIcon) {
            // Decode the same embedded icon used by Explorer. WIC converts its
            // alpha to the premultiplied format expected by Direct2D, including
            // the offscreen preview target; no background is painted behind it.
            HICON icon=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_REMOTE_NATIVE),IMAGE_ICON,256,256,LR_SHARED));
            IWICImagingFactory* imaging=nullptr;IWICBitmap* bitmap=nullptr;IWICFormatConverter* converter=nullptr;
            if(icon&&SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)))&&
                SUCCEEDED(imaging->CreateBitmapFromHICON(icon,&bitmap))&&SUCCEEDED(imaging->CreateFormatConverter(&converter))&&
                SUCCEEDED(converter->Initialize(bitmap,GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))
                target->CreateBitmapFromWicBitmap(converter,nullptr,&brandIcon);
            Release(converter);Release(bitmap);Release(imaging);
        }
        if(!brandIcon)return false;
        target->DrawBitmap(brandIcon,D2D1::RectF(x,y,x+size,y+size),1.f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return true;
    }
    void Monitor(float x, float y, float size, uint32_t color) {
        Color(color);
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + size, y + size * .65f), 3, 3), brush, 1.6f);
        Line(x + size * .5f, y + size * .66f, x + size * .5f, y + size * .88f, color, 1.6f);
        Line(x + size * .28f, y + size * .9f, x + size * .72f, y + size * .9f, color, 1.6f);
    }
    void Bolt(float x, float y, uint32_t color) {
        Line(x + 12, y, x + 3, y + 12, color, 2.f);
        Line(x + 3, y + 12, x + 11, y + 12, color, 2.f);
        Line(x + 11, y + 12, x + 4, y + 24, color, 2.f);
    }
    void Arrow(float x, float y, uint32_t color) {
        Line(x, y + 5, x + 16, y + 5, color, 1.6f);
        Line(x + 10, y, x + 16, y + 5, color, 1.6f);
        Line(x + 10, y + 10, x + 16, y + 5, color, 1.6f);
    }
};

struct Ui;
struct Overlay {
    Ui* owner = nullptr;
    HWND window = nullptr;
    Painter paint;
    bool metrics = false;
    bool captureNotice = false;
    bool dragging = false;
    POINT dragOffset {};
    int hovered = -1, pressed = -1;
    std::vector<Hit> hits;
};
struct Ui {
    HINSTANCE instance = nullptr;
    HWND window = nullptr, canvas = nullptr, address = nullptr, token = nullptr;
    HFONT editFont = nullptr;
    HBRUSH editBrush = nullptr;
    Painter paint;
    IClientSession* session = nullptr;
    WindowsExchange exchange;
    bool suppressPasteV = false, restoreModifiers = false;
    bool pendingPaste = false;
    uint32_t pendingPasteFlags = 0;
    float pendingPasteX = .5f, pendingPasteY = .5f;
    uint64_t restartNoticeUntil = 0, localNoticeUntil = 0;
    std::wstring localNoticeMessage;
    ConnectionOptions options;
    SessionSnapshot snapshot;
    float scale = 1.f, width = 1280.f, height = 800.f;
    bool windowsKeys = true, fullscreen = false, connected = false, preview = false;
    bool imeComposing = false, autoConnect = false, showSettings = false, localError = false;
    bool syntheticHarness = false;
    bool toolsVisible = false, metricsVisible = false, appActive = true, previewOverlays = true;
    uint64_t lastToolActivity = 0;
    Overlay toolbar, metrics, captureNotice;
    bool metricsPositioned = false;
    float metricsLeft = 0.f, metricsTop = 0.f;
    uint64_t sequence = 0;
    uint32_t mouseButtons = 0;
    int wheelVertical = 0, wheelHorizontal = 0;
    float mouseX = .5f, mouseY = .5f;
    RECT videoRect {};
    DWORD savedStyle = 0;
    WINDOWPLACEMENT savedPlacement {};
    std::vector<Hit> hits;
    int hovered = -1;
    int hoverAction = -1;
    HCURSOR remoteCursor = nullptr;
    bool hiddenCursor = false;
    std::unordered_map<uint64_t, HCURSOR> cursorCache;
    WNDPROC editProc = nullptr;
    ~Ui() {
        for (const auto& item : cursorCache) DestroyCursor(item.second);
        if (editFont) DeleteObject(editFont);
        if (editBrush) DeleteObject(editBrush);
    }
    void HitButton(Action action, float x, float y, float w, float h, const std::wstring& label,
                   bool primary = false, bool compact = false) {
        bool hover = hoverAction == static_cast<int>(action);
        uint32_t fill = primary ? (hover ? 0x8df1d3 : ACCENT) : (hover ? 0x263440 : 0x192633);
        paint.Round(x, y, w, h, compact ? 7.f : 9.f, fill, primary ? 0 : LINE);
        paint.Text(label, x + 10, y, w - 20, h, compact ? 11.f : 14.f,
                   primary ? 0x092d26 : WHITE, true, DWRITE_TEXT_ALIGNMENT_CENTER);
        hits.push_back({D2D1::RectF(x, y, x + w, y + h), action});
    }
    void DrawBrand() {
        paint.BrandIcon(19, 20, 45);
        paint.Text(L"GlideBeam", 70, 25, 125, 37, 21, WHITE, true);
        paint.Text(remote_i18n::EffectiveLanguage()==remote_i18n::Language::SimplifiedChinese?L"光梭 · 原生桌面":L"NATIVE DESKTOP", 25, 77, 165, 20, 9, MUTED, true);
        paint.Line(24, 112, kNavWidth - 24, 112, LINE);
        paint.Round(16, 140, kNavWidth - 32, 45, 8, showSettings ? NAV : 0x19352f);
        paint.Monitor(30, 154, 18, showSettings ? MUTED : ACCENT);
        paint.Text(L"连接", 60, 140, 124, 45, 13, showSettings ? MUTED : ACCENT, true);
        hits.push_back({D2D1::RectF(16, 140, kNavWidth - 16, 185), Action::Sessions});
        paint.Round(16, 195, kNavWidth - 32, 45, 8, showSettings ? 0x19352f : NAV);
        paint.Text(L"\u2699", 28, 195, 25, 45, 21, showSettings ? ACCENT : MUTED);
        paint.Text(L"偏好设置", 60, 195, 124, 45, 13, showSettings ? ACCENT : MUTED);
        hits.push_back({D2D1::RectF(16, 195, kNavWidth - 16, 240), Action::Settings});
        paint.Text(L"让每一次操作，都更跟手。", 25, height - 96, 170, 22, 11, MUTED);
        paint.Dot(29, height - 53, 3, ACCENT);
        paint.Text(L"局域网直连", 40, height - 67, 148, 28, 10, MUTED, true);
        paint.Text(L"v0.2.1", 145, height - 36, 48, 15, 9, 0x516275);
    }
    void Field(float x, float y, float w, const wchar_t* label, const wchar_t* hint, bool password = false) {
        paint.Text(label, x, y, w, 20, 11, MUTED, true);
        paint.Round(x, y + 28, w, 46, 8, 0x0d151e, LINE);
        if (preview) paint.Text(password ? L"\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022" : hint,
                              x + 14, y + 28, w - 28, 46, 14, WHITE);
    }
    void DrawIdle() {
        paint.target->Clear(D2D1::ColorF(BG));
        paint.Rect(0, 0, kNavWidth, height, NAV);
        paint.Line(kNavWidth, 0, kNavWidth, height, LINE);
        DrawBrand();
        float x = kNavWidth + 46, right = width - 46, bodyW = right - x;
        paint.Text(showSettings ? L"用你习惯的方式操作。" : L"你的 Mac，就在眼前。", x, 47, bodyW - 180, 47, 28, WHITE, true);
        paint.Text(showSettings ? L"让远程桌面，适应你的操作习惯。" : L"直接连接，轻快响应。把距离留在屏幕之外。",
                   x, 98, bodyW, 25, 13, MUTED);
        paint.Round(right - 130, 56, 130, 30, 15, SOFT);
        paint.Dot(right - 112, 71, 3, ACCENT);
        paint.Text(L"局域网模式", right - 100, 56, 98, 30, 10, ACCENT, true);
        if (showSettings) { DrawPreferences(x, bodyW); return; }
        float cardY = 159, cardH = 390.f;
        paint.Round(x, cardY, bodyW, cardH, 13, PANEL, LINE);
        float inner = x + 28;
        paint.Round(inner, cardY + 28, 54, 54, 12, 0x20363b);
        paint.Monitor(inner + 12, cardY + 41, 30, ACCENT);
        paint.Text(remote_i18n::Text(L"Mac 桌面"), inner + 72, cardY + 26, bodyW - 245, 31, 21, WHITE, true);
        paint.Text(L"macOS 主机  \u00b7  IP 直连", inner + 72, cardY + 59, bodyW - 245, 24, 12, MUTED);
        paint.Round(right - 135, cardY + 40, 77, 25, 12, 0x202c39);
        paint.Text(L"私人设备", right - 130, cardY + 40, 67, 25, 9, MUTED, true, DWRITE_TEXT_ALIGNMENT_CENTER);
        paint.Line(inner, cardY + 107, right - 28, cardY + 107, LINE);
        float formY = cardY + 130, fieldW = bodyW - 56;
        Field(inner, formY, fieldW, L"主机地址", L"192.168.1.100:47990");
        Field(inner, formY + 96, fieldW, L"配对令牌", L"", true);
        paint.Text(L"输入 Mac 端的配对令牌，成功配对后在本机加密保存。", inner, formY + 173, fieldW, 20, 11, MUTED);
        float bottom = cardY + cardH - 75;
        paint.Text(L"只需地址与配对令牌。", inner, bottom, 240, 42, 12, MUTED);
        HitButton(Action::Connect, right - 202, bottom, 174, 44,
                  snapshot.state == SessionState::Connecting ? L"取消连接" : L"连接 Mac", true);
        if (snapshot.state == SessionState::Error || snapshot.state == SessionState::Connecting) {
            paint.Dot(inner + 4, cardY + cardH + 26, 3, snapshot.state == SessionState::Error ? 0xf3a89a : ACCENT);
            paint.Text(Wide(snapshot.message.empty() ? "正在连接 Mac..." : snapshot.message), inner + 16, cardY + cardH + 10,
                       bodyW - 65, 38, 12, snapshot.state == SessionState::Error ? 0xf3a89a : MUTED);
        }
        float tilesY = cardY + cardH + 69, gap = 16, tileW = (bodyW - gap * 2) / 3;
        if (height >= tilesY + 165) {
            DrawTile(x, tilesY, tileW, L"即时响应", L"本地鼠标", L"移动光标，不等待远端画面。", 0);
            DrawTile(x + tileW + gap, tilesY, tileW, L"硬件加速", L"H.264 视频流", L"GPU 解码，轻量呈现。", 1);
            DrawTile(x + (tileW + gap) * 2, tilesY, tileW, L"私人连接", L"加密直连", L"你的桌面，留在自己的网络。", 2);
        }
        paint.Text(L"原生 Windows 客户端", x, height - 43, bodyW / 2, 24, 10, 0x6b7a8d);
        paint.Text(L"F11 全屏  \u00b7  Ctrl + Alt + Shift + Q 断开", x + bodyW / 2, height - 43, bodyW / 2, 24,
                   10, 0x6b7a8d, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    void DrawTile(float x, float y, float w, const wchar_t* eyebrow, const wchar_t* title, const wchar_t* desc, int icon) {
        paint.Round(x, y, w, 119, 11, 0x101923, LINE);
        if (icon == 0) paint.Arrow(x + 20, y + 23, ACCENT);
        else if (icon == 1) paint.Monitor(x + 20, y + 15, 21, ACCENT);
        else {
            paint.Round(x + 20, y + 22, 16, 13, 3, 0x101923, ACCENT);
            paint.Color(ACCENT); paint.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 28, y + 21), 5, 7), paint.brush, 1.3f);
            paint.Rect(x + 19, y + 27, 19, 10, 0x101923);
            paint.Round(x + 20, y + 24, 16, 13, 3, 0x101923, ACCENT);
        }
        paint.Text(eyebrow, x + 49, y + 14, w - 65, 25, 8, MUTED, true);
        paint.Text(title, x + 19, y + 48, w - 38, 25, 15, WHITE, true);
        paint.Text(desc, x + 19, y + 79, w - 38, 23, 10, MUTED);
    }
    void DrawPreferences(float x, float w) {
        paint.Round(x, 159, w, 184, 13, PANEL, LINE);
        paint.Text(L"键盘快捷键", x + 28, 181, w - 56, 34, 19, WHITE, true);
        paint.Text(L"选择在 Mac 上使用哪一种快捷键习惯。", x + 28, 221, w - 56, 24, 12, MUTED);
        HitButton(Action::KeyboardMode, x + 28, 271, 220, 44, windowsKeys ? L"Windows 习惯  \u2713" : L"Mac 原样  \u2713");
        paint.Text(windowsKeys ? L"Ctrl 快捷键映射至 Command，Alt 映射至 Option。" : L"保留 macOS 原生的修饰键含义。",
                   x + 268, 271, w - 296, 44, 11, MUTED);
        paint.Round(x, 367, w, 169, 13, PANEL, LINE);
        paint.Text(L"把跟手感，放在第一位。", x + 28, 387, w - 56, 34, 19, WHITE, true);
        paint.Text(L"光标在本地显示，视频优先处理最新画面。", x + 28, 431, w - 56, 25, 13, MUTED);
        paint.Text(L"全屏：F11   \u00b7   断开：Ctrl + Alt + Shift + Q", x + 28, 477, w - 56, 25, 12, ACCENT);
        paint.Round(x, 555, w, 144, 13, PANEL, LINE);
        paint.Text(L"界面语言", x + 28, 570, w - 56, 30, 19, WHITE, true);
        const auto selected=remote_i18n::GetLanguage();
        HitButton(Action::LanguageSystem,x+28,614,164,40,L"跟随系统",selected==remote_i18n::Language::System,true);
        HitButton(Action::LanguageChinese,x+205,614,164,40,L"简体中文",selected==remote_i18n::Language::SimplifiedChinese,true);
        HitButton(Action::LanguageEnglish,x+382,614,164,40,L"English",selected==remote_i18n::Language::English,true);
        paint.Text(L"切换后立即生效，不会中断连接。",x+28,666,w-56,23,11,MUTED);
    }
    void DrawConnected() {
        paint.target->Clear(D2D1::ColorF(BG));
        if (preview) {
            DrawSyntheticDesktop();
            if (previewOverlays) {
                paint.target->SetTransform(D2D1::Matrix3x2F::Translation((width - kToolbarWidth) / 2, 12));
                DrawToolbar(paint, toolbar);
                paint.target->SetTransform(D2D1::Matrix3x2F::Translation(width - kMetricsWidth - 24, 94));
                DrawMetrics(paint, metrics);
                paint.target->SetTransform(D2D1::Matrix3x2F::Identity());
            }
            if (NeedsCaptureNotice()) {
                paint.target->SetTransform(D2D1::Matrix3x2F::Translation((width - kCaptureNoticeWidth) / 2, (height - kCaptureNoticeHeight) / 2));
                DrawCaptureNotice(paint); paint.target->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        } else if (!snapshot.hardwareDecode && snapshot.fps == 0) {
            float cy = height / 2.f;
            paint.Monitor(width / 2.f - 22, cy - 54, 44, 0x405869);
            paint.Text(L"正在准备你的桌面", width / 2.f - 180, cy + 2, 360, 38, 20, WHITE, true, DWRITE_TEXT_ALIGNMENT_CENTER);
            paint.Text(Wide(snapshot.message), width / 2.f - 260, cy + 46, 520, 30, 12, MUTED, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    bool CaptureNoticeActive() const {
        return connected && !snapshot.captureState.empty() && snapshot.captureState != "running";
    }
    bool RestartNoticeActive() const {
        return connected && !snapshot.restartState.empty() && GetTickCount64() < restartNoticeUntil;
    }
    bool NeedsCaptureNotice() const {
        return CaptureNoticeActive() || RestartNoticeActive() || (connected && GetTickCount64() < localNoticeUntil);
    }
    bool CaptureNeedsManualRetry() const {
        return snapshot.captureState == "user-stopped" || snapshot.captureState == "stopped";
    }
    void DrawCaptureNotice(Painter& out) {
        out.Round(0, 0, kCaptureNoticeWidth, kCaptureNoticeHeight, 12, NAV, LINE);
        if (!CaptureNoticeActive()) {
            const bool restart = RestartNoticeActive();
            out.Dot(26, 32, 4, ACCENT);
            out.Text(restart ? remote_i18n::Text(L"重启 Mac") : L"剪贴板同步", 40, 15, kCaptureNoticeWidth - 66, 34, 16, WHITE, true);
            out.Paragraph(restart ? Wide(snapshot.restartMessage) : localNoticeMessage,
                24, 55, kCaptureNoticeWidth - 48, 62, 13, WHITE);
            out.Text(restart && snapshot.restartState == "authorizing" ? L"单击此提示收起，然后在 Mac 系统授权窗口确认。" : L"单击此提示收起。",
                24, 127, kCaptureNoticeWidth - 48, 25, 11, MUTED, false, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        const bool manual = CaptureNeedsManualRetry();
        const wchar_t* title = manual ? L"画面采集已停止" : snapshot.captureState == "permission" ? L"需要 Mac 授权" :
            snapshot.captureState == "failed" ? L"画面采集需要重试" :
            snapshot.captureState == "starting" ? L"正在准备画面" : L"画面暂时不可用";
        out.Dot(26, 32, 4, manual ? 0xefc681 : ACCENT);
        out.Text(title, 40, 15, kCaptureNoticeWidth - 66, 34, 16, WHITE, true);
        out.Paragraph(Wide(snapshot.message.empty() ? "正在等待 Mac 更新采集状态。" : snapshot.message),
            24, 55, kCaptureNoticeWidth - 48, 62, 13, WHITE);
        const wchar_t* footer = manual || snapshot.captureState == "failed" ? L"请在 Mac 端点击「重试画面采集」。" :
            snapshot.captureState == "permission" ? L"请在 Mac 端确认屏幕录制授权后重试。" : L"Mac 恢复采集后，这里会自动继续显示。";
        out.Text(footer,
            24, 127, kCaptureNoticeWidth - 48, 25, 11, MUTED, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    void OverlayButton(Painter& out, Overlay& overlay, Action action, float x, float w,
                       const wchar_t* label, bool selected = false) {
        bool hover = overlay.hovered == static_cast<int>(overlay.hits.size());
        out.Round(x, 14, w, 36, 7, selected ? SOFT : (hover ? 0x263440 : 0x192633), selected ? 0x316d5d : LINE);
        out.Text(label, x + 7, 14, w - 14, 36, 11, selected ? ACCENT : WHITE, true, DWRITE_TEXT_ALIGNMENT_CENTER);
        overlay.hits.push_back({D2D1::RectF(x, 14, x + w, 50), action});
    }
    void DrawToolbar(Painter& out, Overlay& overlay) {
        overlay.hits.clear();
        out.Round(0, 0, kToolbarWidth, kToolbarHeight, 11, NAV, LINE);
        out.Round(14, 16, 31, 31, 8, SOFT); out.Monitor(21, 23, 17, ACCENT);
        out.Text(remote_i18n::Text(L"Mac 桌面"), 56, 10, 110, 24, 13, WHITE, true);
        out.Dot(59, 43, 2.5f, ACCENT);
        out.Text(Wide(options.address), 69, 31, 101, 23, 9, MUTED);
        out.Line(174, 18, 174, 46, LINE);
        OverlayButton(out, overlay, Action::KeyboardMode, 182, 94, windowsKeys ? L"Windows 习惯" : L"Mac 原样");
        OverlayButton(out, overlay, Action::Metrics, 282, 64, L"传输信息", metricsVisible);
        OverlayButton(out, overlay, Action::Clipboard, 352, 80, L"剪贴板", exchange.ClipboardEnabled());
        OverlayButton(out, overlay, Action::Files, 438, 64, L"文件传输");
        OverlayButton(out, overlay, Action::Fullscreen, 508, 80, fullscreen ? L"窗口" : L"全屏");
        OverlayButton(out, overlay, Action::More, 594, 58, L"更多");
        OverlayButton(out, overlay, Action::Disconnect, 658, 90, L"断开");
    }
    void DrawMetrics(Painter& out, Overlay& overlay) {
        overlay.hits.clear();
        out.Round(0, 0, kMetricsWidth, kMetricsHeight, 11, NAV, LINE);
        out.Text(L"解码与传输", 18, 12, 215, 27, 14, WHITE, true);
        out.Text(L"\u00d7", kMetricsWidth - 47, 10, 31, 29, 20, MUTED, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        overlay.hits.push_back({D2D1::RectF(kMetricsWidth - 50, 8, kMetricsWidth - 12, 42), Action::Metrics});
        out.Line(18, 44, kMetricsWidth - 18, 44, LINE);
        auto row = [&](const wchar_t* label, const std::wstring& value, float y, uint32_t color = WHITE) {
            out.Text(label, 18, y, 166, 24, 11, MUTED);
            out.Text(value, 184, y, kMetricsWidth - 202, 24, 12, color, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
        };
        wchar_t value[64] {};
        std::swprintf(value, 64, L"%.1f FPS", snapshot.fps); row(L"实际显示帧率", value, 53);
        std::swprintf(value, 64, L"%.2f Mbps", snapshot.bitrateMbps); row(L"视频码率", value, 79);
        if (snapshot.networkMs >= 0) std::swprintf(value, 64, L"%.2f ms", snapshot.networkMs); else std::wcscpy(value, L"\u2014");
        row(L"网络 RTT", value, 105);
        if (snapshot.decodeMs >= 0) std::swprintf(value, 64, L"%.2f ms", snapshot.decodeMs); else std::wcscpy(value, L"\u2014");
        row(L"画面处理", value, 131);
        const bool reported = snapshot.state == SessionState::Connected && snapshot.macEncoderMode != MacVideoEncoderMode::Unknown;
        const bool decoding = snapshot.state == SessionState::Connected && snapshot.hardwareDecode;
        row(L"Mac 视频编码", MacEncoderText(snapshot), 157, reported ? ACCENT : MUTED);
        row(L"Windows 视频解码", decoding ? L"GPU 硬件解码" : L"等待硬件解码", 183, decoding ? ACCENT : MUTED);
        std::swprintf(value, 64, L"%u \u00d7 %u", snapshot.videoWidth, snapshot.videoHeight); row(L"远端分辨率", value, 209);
        std::swprintf(value, 64, remote_i18n::Text(L"%u 帧"), snapshot.queuedFrames); row(L"等待处理", value, 235);
        const wchar_t* audioText=L"等待系统声音";
        if(snapshot.macAudioState=="unavailable"||snapshot.audio.state==AudioPlaybackState::Unavailable)audioText=L"音频不可用";
        else if(snapshot.audio.muted)audioText=L"已静音";
        else if(snapshot.audio.state==AudioPlaybackState::Playing)audioText=L"PCM · 48 kHz · 双声道";
        row(L"系统声音",audioText,261,snapshot.audio.state==AudioPlaybackState::Playing?ACCENT:MUTED);
        std::swprintf(value,64,L"%.1f / %.0f ms",snapshot.audio.queueMs,snapshot.audio.targetMs);row(L"音频缓冲 / 目标",value,287);
        std::swprintf(value,64,L"%.1f / %.1f ms",snapshot.audio.deviceQueuedMs,snapshot.audio.devicePeriodMs);row(L"声卡排队 / 周期",value,313);
        std::swprintf(value,64,L"%llu / %llu",static_cast<unsigned long long>(snapshot.audio.underruns),static_cast<unsigned long long>(snapshot.audio.lost));row(L"音频欠载 / 丢包",value,339);
        std::swprintf(value,64,L"%llu",static_cast<unsigned long long>(snapshot.audio.deviceUnderruns));row(L"声卡耗尽事件",value,365);
        out.Line(18, 401, kMetricsWidth - 18, 401, LINE);
        out.Text(preview ? L"合成数据 · 仅用于界面预览" : L"处理时间为 CPU 计时，含解码、转换与提交。", 18, 407, kMetricsWidth - 36, 22, 9, MUTED);
        out.Text(L"该数值不代表完整交互延迟。", 18, 429, kMetricsWidth - 36, 19, 9, MUTED);
    }
    void DrawSyntheticDesktop() {
        // Entirely synthetic artwork; no screen capture, host connection or credentials.
        paint.Rect(0, 0, width, height, 0x171e36);
        paint.Dot(width * .87f, height * .48f, height * .73f, 0x293354);
        paint.Dot(width * .95f, height * .74f, height * .50f, 0x3b5061);
        paint.Dot(width * .84f, height * .91f, height * .32f, 0x51747a);
        paint.Rect(0, 0, width, 28, 0x101622);
        paint.Text(L"\u25cf   编辑器    文件    编辑    显示    窗口    帮助", 16, 0, 650, 28, 11, WHITE, true);
        paint.Text(L"合成 Mac 桌面 · 界面预览", width - 295, 0, 278, 28, 10, MUTED, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
        float x = 72, y = 105, w = width * .72f, h = height - 192;
        paint.Round(x + 7, y + 12, w, h, 12, 0x101626);
        paint.Round(x, y, w, h, 12, 0x111b28, 0x3a4557);
        paint.Round(x, y, w, 40, 12, 0x202b3b);
        paint.Rect(x, y + 27, w, 13, 0x202b3b);
        paint.Dot(x + 20, y + 20, 5, 0xf08b83); paint.Dot(x + 38, y + 20, 5, 0xe9c27b); paint.Dot(x + 56, y + 20, 5, 0x7ac2a3);
        paint.Text(L"光梭 / 工作区", x + 94, y + 4, w - 120, 32, 12, MUTED);
        paint.Rect(x, y + 40, 175, h - 40, 0x152030);
        paint.Text(L"项目文件", x + 18, y + 56, 135, 25, 11, MUTED, true);
        const wchar_t* files[] = {L"\u2304  remote", L"    apps", L"    shared", L"    transport", L"    README.md", L"    protocol.h"};
        for (int i = 0; i < 6; ++i) paint.Text(files[i], x + 18, y + 90 + i * 30.f, 145, 27, 12, i == 5 ? ACCENT : MUTED);
        paint.Round(x + 181, y + 47, 156, 31, 6, 0x223346);
        paint.Text(L"session.cpp", x + 194, y + 47, 130, 31, 11, WHITE);
        const wchar_t* code[] = {L"// 每一次操作，都更跟手。", L"class RemoteSession {", L"public:", L"    void connect();", L"    void presentLatestFrame();", L"", L"    // 本地光标与视频独立处理", L"    void updateLocalCursor();", L"    void sendInputImmediately();", L"};"};
        for (int i = 0; i < 10; ++i) {
            wchar_t number[16] {}; std::swprintf(number, 16, L"%d", i + 1);
            float lineY = y + 105 + i * 29.f;
            if (lineY > y + h - 43) break;
            paint.Text(number, x + 183, lineY, 35, 26, 11, 0x607288, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
            paint.Text(code[i], x + 233, lineY, w - 255, 26, 14, i == 0 || i == 6 ? 0x74bda4 : (i == 1 || i == 2 ? 0x8fb7e6 : WHITE));
        }
        paint.Rect(x, y + h - 28, w, 28, 0x1b3142);
        paint.Text(L"main    UTF-8    C++    合成界面", x + 18, y + h - 28, w - 36, 28, 10, MUTED);
        float dockW = 310, dockX = (width - dockW) / 2;
        paint.Round(dockX, height - 70, dockW, 52, 15, 0x263745, 0x465763);
        for (int i = 0; i < 6; ++i) {
            paint.Round(dockX + 12 + i * 49.f, height - 60, 33, 33, 8, i == 2 ? 0x305965 : 0x425567);
            paint.Text(i == 2 ? L"\u203a_" : (i % 2 ? L"\u25c6" : L"\u25a1"), dockX + 12 + i * 49.f, height - 60, 33, 33, 18, WHITE, true, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    void Draw() {
        hoverAction = hovered >= 0 && hovered < static_cast<int>(hits.size()) ? static_cast<int>(hits[hovered].action) : -1;
        hits.clear();
        if (connected) DrawConnected(); else DrawIdle();
    }
    void CreateEdits() {
        editBrush = CreateSolidBrush(RGB(13, 21, 30));
        HWND* controls[] = {&address, &token};
        for (int i = 0; i < 2; ++i) {
            *controls[i] = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | (i == 1 ? ES_PASSWORD : 0),
                0, 0, 100, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(101 + i)), instance, nullptr);
            SendMessageW(*controls[i], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
            SetWindowLongPtrW(*controls[i], GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            if (!editProc) editProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(*controls[i], GWLP_WNDPROC));
            SetWindowLongPtrW(*controls[i], GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditWndProc));
        }
        SetWindowTextW(address, Wide(options.address.empty() ? "192.168.1.100:47990" : options.address).c_str());
        SetWindowTextW(token, Wide(options.token).c_str());
        SendMessageW(token, EM_SETPASSWORDCHAR, 0x2022, 0);
        SendMessageW(token, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(remote_i18n::Text(L"输入配对令牌")));
        UpdateFonts();
    }
    void UpdateFonts() {
        if (editFont) DeleteObject(editFont);
        editFont = CreateFontW(-static_cast<int>(14 * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (HWND edit : {address, token}) if (edit) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(editFont), TRUE);
    }
    void Layout() {
        RECT client {}; GetClientRect(window, &client);
        width = (client.right - client.left) / scale;
        height = (client.bottom - client.top) / scale;
        if (paint.target) static_cast<ID2D1HwndRenderTarget*>(paint.target)->Resize(D2D1::SizeU(client.right, client.bottom));
        bool fieldsVisible = !connected && !showSettings;
        float x = kNavWidth + 74.f, w = width - x - 74.f, y = 159.f + 130.f;
        const float fieldY[] = {y + 38, y + 134};
        HWND edits[] = {address, token};
        for (int i = 0; i < 2; ++i) {
            MoveWindow(edits[i], static_cast<int>((x + 14) * scale), static_cast<int>(fieldY[i] * scale),
                std::max(20, static_cast<int>((w - 28) * scale)), static_cast<int>(25 * scale), TRUE);
            ShowWindow(edits[i], fieldsVisible ? SW_SHOW : SW_HIDE);
        }
        if (connected) {
            hovered = -1; hits.clear();
            MoveWindow(canvas, 0, 0, std::max(1L, client.right), std::max(1L, client.bottom), TRUE);
            ShowWindow(canvas, snapshot.hardwareDecode || snapshot.fps > 0 ? SW_SHOW : SW_HIDE);
            UpdateViewport();
        } else ShowWindow(canvas, SW_HIDE);
        PositionOverlays();
        InvalidateRect(window, nullptr, FALSE);
    }
    void UpdateViewport() {
        if (!canvas) return;
        RECT rect {}; GetClientRect(canvas, &rect);
        if (!snapshot.videoWidth || !snapshot.videoHeight) { videoRect = rect; }
        else {
            double fit = std::min(static_cast<double>(rect.right) / snapshot.videoWidth, static_cast<double>(rect.bottom) / snapshot.videoHeight);
            int w = static_cast<int>(snapshot.videoWidth * fit), h = static_cast<int>(snapshot.videoHeight * fit);
            videoRect = {(rect.right - w) / 2, (rect.bottom - h) / 2, (rect.right + w) / 2, (rect.bottom + h) / 2};
        }
        if (session) session->ResizeViewport(videoRect);
    }
    void RenderWindow() {
        if (!paint.target) {
            RECT rect {}; GetClientRect(window, &rect);
            ID2D1HwndRenderTarget* value = nullptr;
            auto properties = D2D1::RenderTargetProperties(); properties.dpiX = 96.f * scale; properties.dpiY = 96.f * scale;
            if (FAILED(paint.factory->CreateHwndRenderTarget(properties,
                D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(rect.right, rect.bottom)), &value))) return;
            if (!paint.SetTarget(value)) return;
        }
        paint.target->BeginDraw();
        Draw();
        if (paint.target->EndDraw() == D2DERR_RECREATE_TARGET) paint.ResetTarget();
    }
    bool CreateOverlays() {
        toolbar.owner = this; metrics.owner = this; metrics.metrics = true;
        captureNotice.owner = this; captureNotice.captureNotice = true;
        // The first mouse message may precede the popup's first WM_PAINT.
        metrics.hits.push_back({D2D1::RectF(kMetricsWidth - 50, 8, kMetricsWidth - 12, 42), Action::Metrics});
        for (Overlay* overlay : {&toolbar, &metrics, &captureNotice}) {
            if (!overlay->paint.Init()) return false;
            overlay->window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (overlay->captureNotice ? WS_EX_LAYERED : 0),
                kOverlayClass, overlay->captureNotice ? remote_i18n::Text(L"光梭 · 采集状态") : overlay->metrics ? remote_i18n::Text(L"光梭 · 解码与传输") : remote_i18n::Text(L"光梭 · 会话工具栏"),
                WS_POPUP, 0, 0, 1, 1, window, nullptr, instance, overlay);
            if (!overlay->window) return false;
            if (overlay->captureNotice && !SetLayeredWindowAttributes(overlay->window, 0, 235, LWA_ALPHA)) return false;
        }
        return true;
    }
    void DestroyOverlays() {
        for (Overlay* overlay : {&toolbar, &metrics, &captureNotice}) {
            HWND hwnd = overlay->window; overlay->window = nullptr;
            if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
            overlay->paint.ResetTarget();
        }
    }
    bool IsOverlayAt(POINT point) const {
        HWND hit = WindowFromPoint(point);
        return hit && (hit == toolbar.window || hit == metrics.window || hit == captureNotice.window ||
            (toolbar.window && IsChild(toolbar.window, hit)) || (metrics.window && IsChild(metrics.window, hit)) ||
            (captureNotice.window && IsChild(captureNotice.window, hit)));
    }
    RECT MetricsBounds(const RECT& client) {
        const int clientWidth = std::max(0L, client.right - client.left);
        const int clientHeight = std::max(0L, client.bottom - client.top);
        const int w = std::min(clientWidth, static_cast<int>(kMetricsWidth * scale));
        const int h = std::min(clientHeight, static_cast<int>(kMetricsHeight * scale));
        int x = metricsPositioned ? static_cast<int>(std::lround(metricsLeft * scale)) : clientWidth - w - static_cast<int>(24 * scale);
        int y = metricsPositioned ? static_cast<int>(std::lround(metricsTop * scale)) : static_cast<int>(94 * scale);
        x = std::clamp(x, 0, clientWidth - w);
        y = std::clamp(y, 0, clientHeight - h);
        if (metricsPositioned) { metricsLeft = x / scale; metricsTop = y / scale; }
        return RECT {x, y, x + w, y + h};
    }
    void DragMetrics(POINT pointer) {
        RECT client {}; GetClientRect(window, &client);
        POINT origin {}; ClientToScreen(window, &origin);
        metricsPositioned = true;
        metricsLeft = (pointer.x - metrics.dragOffset.x - origin.x) / scale;
        metricsTop = (pointer.y - metrics.dragOffset.y - origin.y) / scale;
        const RECT rect = MetricsBounds(client);
        SetWindowPos(metrics.window, nullptr, origin.x + rect.left, origin.y + rect.top, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    }
    void PositionOverlays() {
        if (!window) return;
        RECT client {}; GetClientRect(window, &client); POINT origin {}; ClientToScreen(window, &origin);
        bool visible = connected && appActive && IsWindowVisible(window) && !IsIconic(window);
        for (Overlay* overlay : {&toolbar, &metrics, &captureNotice}) {
            if (!overlay->window) continue;
            bool show = visible && (overlay->captureNotice ? NeedsCaptureNotice() : overlay->metrics ? metricsVisible : toolsVisible);
            if (!show) {
                if (GetCapture() == overlay->window) ReleaseCapture();
                overlay->dragging = false; overlay->pressed = -1;
                ShowWindow(overlay->window, SW_HIDE); continue;
            }
            int w = static_cast<int>((overlay->metrics ? kMetricsWidth : kToolbarWidth) * scale);
            int h = static_cast<int>((overlay->metrics ? kMetricsHeight : kToolbarHeight) * scale);
            int x = overlay->metrics ? client.right - w - static_cast<int>(24 * scale) : (client.right - w) / 2;
            int y = static_cast<int>((overlay->metrics ? 94 : 12) * scale);
            if (overlay->metrics) {
                const RECT rect = MetricsBounds(client);
                x = rect.left; y = rect.top; w = rect.right - rect.left; h = rect.bottom - rect.top;
            }
            if (overlay->captureNotice) {
                w = std::min(client.right, static_cast<LONG>(kCaptureNoticeWidth * scale));
                h = std::min(client.bottom, static_cast<LONG>(kCaptureNoticeHeight * scale));
                x = (client.right - w) / 2; y = (client.bottom - h) / 2;
            }
            SetWindowPos(overlay->window, HWND_TOP, origin.x + std::max(0, x), origin.y + y, w, h,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, static_cast<int>(20 * scale), static_cast<int>(20 * scale));
            if (region && !SetWindowRgn(overlay->window, region, FALSE)) DeleteObject(region);
            InvalidateRect(overlay->window, nullptr, FALSE);
        }
    }
    void RevealTools() {
        if (!connected || !appActive || GetCapture() == canvas) return;
        lastToolActivity = GetTickCount64();
        if (!toolsVisible) { toolsVisible = true; PositionOverlays(); }
    }
    void Activity() { if (toolsVisible) lastToolActivity = GetTickCount64(); }
    bool CanvasActivity(LPARAM point) {
        int x = GET_X_LPARAM(point), y = GET_Y_LPARAM(point);
        int half = static_cast<int>((kToolbarWidth / 2 + 16) * scale);
        int center = static_cast<int>(width * scale / 2);
        if (connected && GetCapture() != canvas && y >= 0 && y <= static_cast<int>(18 * scale) &&
            x >= center - half && x <= center + half) {
            RevealTools();
            POINT screen {x, y}; ClientToScreen(canvas, &screen);
            return IsOverlayAt(screen);
        }
        return false;
    }
    void PollOverlays(const POINT* testPointer = nullptr) {
        if (!connected) return;
        POINT point {};
        if (testPointer) point = *testPointer; else GetCursorPos(&point);
        HWND capture = GetCapture();
        bool interaction = capture == canvas || capture == toolbar.window || capture == metrics.window || IsOverlayAt(point);
        if (interaction) lastToolActivity = GetTickCount64();
        if (toolsVisible && !interaction && GetTickCount64() - lastToolActivity >= 3000) {
            toolsVisible = false; ShowWindow(toolbar.window, SW_HIDE); toolbar.hovered = -1;
        }
    }
    void RenderOverlay(Overlay& overlay) {
        Painter& out = overlay.paint;
        if (!out.target) {
            RECT rect {}; GetClientRect(overlay.window, &rect);
            auto properties = D2D1::RenderTargetProperties(); properties.dpiX = 96.f * scale; properties.dpiY = 96.f * scale;
            ID2D1HwndRenderTarget* target = nullptr;
            if (FAILED(out.factory->CreateHwndRenderTarget(properties,
                D2D1::HwndRenderTargetProperties(overlay.window, D2D1::SizeU(rect.right, rect.bottom)), &target))) return;
            if (!out.SetTarget(target)) return;
        }
        out.target->BeginDraw(); out.target->Clear(D2D1::ColorF(NAV));
        if (overlay.captureNotice) DrawCaptureNotice(out);
        else if (overlay.metrics) DrawMetrics(out, overlay); else DrawToolbar(out, overlay);
        if (out.target->EndDraw() == D2DERR_RECREATE_TARGET) out.ResetTarget();
    }
    static int OverlayHit(Overlay& overlay, LPARAM point) {
        float x = GET_X_LPARAM(point) / overlay.owner->scale, y = GET_Y_LPARAM(point) / overlay.owner->scale;
        for (size_t i = 0; i < overlay.hits.size(); ++i) {
            auto r = overlay.hits[i].rect;
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return static_cast<int>(i);
        }
        return -1;
    }
    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto overlay = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            overlay = static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
        }
        if (!overlay || !overlay->owner) return DefWindowProcW(hwnd, message, wParam, lParam);
        Ui* ui = overlay->owner;
        if (overlay->captureNotice) {
            if (message == WM_LBUTTONUP && !ui->CaptureNoticeActive()) {
                ui->restartNoticeUntil = ui->localNoticeUntil = 0;
                ui->PositionOverlays();
                return 0;
            }
            // This notice offers no remote recovery action. Keep the canvas
            // focused, and consume pointer gestures rather than clicking the
            // frozen desktop beneath it.
            switch (message) {
            case WM_MOUSEMOVE: case WM_MOUSELEAVE:
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: return 0;
            case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
            }
        }
        switch (message) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); ui->RenderOverlay(*overlay); EndPaint(hwnd, &ps); return 0; }
        case WM_SIZE:
            if (overlay->paint.target) static_cast<ID2D1HwndRenderTarget*>(overlay->paint.target)->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            return 0;
        case WM_DPICHANGED: overlay->paint.ResetTarget(); return 0;
        case WM_MOUSEMOVE: {
            if (overlay->dragging && GetCapture() == hwnd) {
                POINT pointer {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(hwnd, &pointer); ui->DragMetrics(pointer); return 0;
            }
            ui->Activity(); int hit = OverlayHit(*overlay, lParam);
            if (hit != overlay->hovered) { overlay->hovered = hit; InvalidateRect(hwnd, nullptr, FALSE); }
            TRACKMOUSEEVENT tracking {sizeof tracking, TME_LEAVE, hwnd, 0}; TrackMouseEvent(&tracking); return 0;
        }
        case WM_MOUSELEAVE: overlay->hovered = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_SETCURSOR: {
            POINT pointer {}; GetCursorPos(&pointer); ScreenToClient(hwnd, &pointer);
            const bool title = overlay->metrics && overlay->hovered < 0 && pointer.y >= 0 && pointer.y < static_cast<int>(44 * ui->scale);
            SetCursor(LoadCursorW(nullptr, overlay->dragging || title ? IDC_SIZEALL : overlay->hovered >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONDOWN: {
            ui->Activity(); overlay->pressed = OverlayHit(*overlay, lParam);
            const int y = GET_Y_LPARAM(lParam);
            overlay->dragging = overlay->metrics && overlay->pressed < 0 && y >= 0 && y < static_cast<int>(44 * ui->scale);
            if (overlay->dragging) overlay->dragOffset = POINT {GET_X_LPARAM(lParam), y};
            SetCapture(hwnd); return 0;
        }
        case WM_LBUTTONUP: {
            if (overlay->dragging) {
                POINT pointer {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(hwnd, &pointer); ui->DragMetrics(pointer);
                overlay->dragging = false; overlay->pressed = -1;
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            int pressed = overlay->pressed, hit = OverlayHit(*overlay, lParam); overlay->pressed = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (pressed >= 0 && pressed == hit && pressed < static_cast<int>(overlay->hits.size())) ui->Activate(overlay->hits[pressed].action);
            return 0;
        }
        case WM_CAPTURECHANGED: overlay->pressed = -1; overlay->dragging = false; return 0;
        case WM_CANCELMODE:
            overlay->pressed = -1; overlay->dragging = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    void Send(uint32_t type, int a = 0, int b = 0, uint32_t flags = 0) {
        if (!session || !connected) return;
        RemoteInputEvent input {type, flags | (windowsKeys ? 2u : 0u), mouseX, mouseY, a, b, ++sequence};
        session->SendInput(reinterpret_cast<const uint8_t*>(&input), sizeof input);
    }
    void TextInput(const std::wstring& value) {
        if (!connected || value.empty()) return;
        pendingPaste = false; exchange.CancelPendingPaste();
        auto text = Utf8(value);
        RemoteInputEvent header {INPUT_TEXT, windowsKeys ? 2u : 0u, mouseX, mouseY, 0, 0, ++sequence};
        std::vector<uint8_t> data(sizeof header + text.size());
        std::memcpy(data.data(), &header, sizeof header);
        std::memcpy(data.data() + sizeof header, text.data(), text.size());
        session->SendInput(data.data(), data.size());
    }
    void ReleaseInputs(bool cancelPaste = true) {
        if (cancelPaste) { pendingPaste = false; exchange.CancelPendingPaste(); }
        suppressPasteV = false; restoreModifiers = false;
        mouseButtons = 0;
        wheelVertical = wheelHorizontal = 0;
        Send(INPUT_RELEASE_ALL);
        if (GetCapture() == canvas) ReleaseCapture();
    }
    void Position(LPARAM lParam) {
        int w = videoRect.right - videoRect.left, h = videoRect.bottom - videoRect.top;
        mouseX = w > 1 ? std::clamp(static_cast<float>(GET_X_LPARAM(lParam) - videoRect.left) / (w - 1), 0.f, 1.f) : .5f;
        mouseY = h > 1 ? std::clamp(static_cast<float>(GET_Y_LPARAM(lParam) - videoRect.top) / (h - 1), 0.f, 1.f) : .5f;
    }
    bool InsideVideo(LPARAM lParam) const {
        POINT p {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        return PtInRect(&videoRect, p) != FALSE;
    }
    void MouseButton(UINT message, LPARAM lParam) {
        bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
                    message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDBLCLK;
        int button = message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK ? 1 :
                     message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_RBUTTONDBLCLK ? 2 : 3;
        if (down && !InsideVideo(lParam)) return;
        if (down) { pendingPaste = false; exchange.CancelPendingPaste(); }
        Position(lParam);
        if (down) { SetFocus(canvas); SetCapture(canvas); mouseButtons |= 1u << button; }
        else mouseButtons &= ~(1u << button);
        Send(INPUT_BUTTON, button, 0, down ? 1u : 0u);
        if (!mouseButtons && GetCapture() == canvas) ReleaseCapture();
    }
    bool Shortcut(WPARAM key, bool repeat = false) {
        if (key == VK_F11) { if (!repeat) ToggleFullscreen(); return true; }
        if (key == 'Q' && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_MENU) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) {
            Disconnect(); return true;
        }
        return false;
    }
    void Connect() {
        if (snapshot.state == SessionState::Connecting) { Disconnect(); return; }
        options.address = Utf8(EditText(address)); options.token = Utf8(EditText(token));
        if (options.address.empty() || options.token.empty()) {
            localError = true;
            snapshot.state = SessionState::Error;
            snapshot.message = "请输入主机地址与 Mac 端的配对令牌。";
            Layout(); SetFocus(options.token.empty() ? token : address); return;
        }
        localError = false;
        hiddenCursor = false; remoteCursor = nullptr;
        snapshot.state = SessionState::Connecting; snapshot.message = "正在建立安全连接...";
        if (!session->Connect(canvas, options)) {
            snapshot = session->Snapshot();
            if (snapshot.state != SessionState::Error) { snapshot.state = SessionState::Error; snapshot.message = "无法启动连接。"; }
        }
        InvalidateRect(window, nullptr, FALSE);
    }
    void Disconnect() {
        ReleaseInputs(); session->Disconnect(); snapshot = session->Snapshot(); connected = false; localError = false;
        toolsVisible = false;
        if (fullscreen) ToggleFullscreen();
        Layout(); SetFocus(address);
    }
    void ToggleFullscreen() {
        if (!connected && !fullscreen) return;
        ReleaseInputs();
        fullscreen = !fullscreen;
        if (fullscreen) {
            savedStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
            savedPlacement.length = sizeof savedPlacement; GetWindowPlacement(window, &savedPlacement);
            MONITORINFO info {}; info.cbSize = sizeof info; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
            SetWindowLongPtrW(window, GWL_STYLE, savedStyle & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
                info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
        } else {
            SetWindowLongPtrW(window, GWL_STYLE, savedStyle);
            SetWindowPlacement(window, &savedPlacement);
            SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        toolsVisible = connected; lastToolActivity = GetTickCount64();
        Layout(); if (connected && GetForegroundWindow() == window) SetFocus(canvas);
    }
    void MoreMenu() {
        ReleaseInputs();
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING | (connected && snapshot.state == SessionState::Connected && snapshot.restartSupported && !snapshot.restartBusy ? 0 : MF_GRAYED), 1, remote_i18n::Text(L"重启 Mac…"));
        if (!snapshot.restartState.empty()) AppendMenuW(menu, MF_STRING, 2, remote_i18n::Text(L"查看重启状态"));
        AppendMenuW(menu, MF_STRING | (connected ? 0 : MF_GRAYED), 4, remote_i18n::Text(snapshot.audio.muted ? L"恢复系统声音" : L"静音系统声音"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        LanguageMenu(menu);
        AppendMenuW(menu, MF_STRING, 3, remote_i18n::Text(L"打开 Windows 日志目录"));
        POINT at {static_cast<LONG>(594 * scale), static_cast<LONG>(kToolbarHeight * scale)};
        ClientToScreen(toolbar.window ? toolbar.window : window, &at);
        const auto choice = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, window, nullptr);
        DestroyMenu(menu);
        if(choice==101) ChangeLanguage(remote_i18n::Language::System);
        else if(choice==102) ChangeLanguage(remote_i18n::Language::SimplifiedChinese);
        else if(choice==103) ChangeLanguage(remote_i18n::Language::English);
        else if (choice == 1) {
            const wchar_t* prompt = snapshot.restartRequiresLocalAuthorization ?
                L"确认重启这台 Mac？未保存的工作可能丢失，远程连接会断开。\n\n如 macOS 要求管理员授权，请在 Mac 系统窗口确认；取消授权即可取消重启。" :
                L"确认重启这台 Mac？未保存的工作可能丢失，远程连接会断开。";
            if (MessageBoxW(window, remote_i18n::Text(prompt), remote_i18n::Text(L"重启 Mac"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                if (!session->RestartMac()) MessageBoxW(window, remote_i18n::Text(L"无法提交重启请求，请检查连接状态或等待当前请求结束。"), remote_i18n::Text(L"重启 Mac"), MB_OK | MB_ICONINFORMATION);
            }
        } else if (choice == 2) {
            restartNoticeUntil = GetTickCount64() + 25000;
            PositionOverlays();
        } else if (choice == 4) {
            session->SetAudioMuted(!snapshot.audio.muted); snapshot=session->Snapshot();
            if(metrics.window)InvalidateRect(metrics.window,nullptr,FALSE);
        } else if (choice == 3) {
            const auto directory = remote_log::Directory();
            std::error_code error; std::filesystem::create_directories(directory, error);
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                MessageBoxW(window, remote_i18n::Text(L"无法打开日志目录。"), remote_i18n::Text(L"诊断日志"), MB_OK | MB_ICONINFORMATION);
        }
        Activity();
    }
    void ChangeLanguage(remote_i18n::Language language, bool persist=true) {
        remote_i18n::SetLanguage(language);
        if(persist&&!remote_windows_language::Save(language)) remote_log::Write(remote_log::Level::Warning,"ui","language preference save failed");
        SetWindowTextW(window,remote_i18n::Text(L"光梭 · GlideBeam"));
        if(canvas)SetWindowTextW(canvas,remote_i18n::Text(L"Mac 桌面"));
        if(token)SendMessageW(token,EM_SETCUEBANNER,FALSE,reinterpret_cast<LPARAM>(remote_i18n::Text(L"输入配对令牌")));
        exchange.RefreshLanguage();
        for(auto overlay:{&toolbar,&metrics,&captureNotice}) if(overlay->window) InvalidateRect(overlay->window,nullptr,FALSE);
        InvalidateRect(window,nullptr,FALSE);
    }
    void LanguageMenu(HMENU menu) {
        const auto language=remote_i18n::GetLanguage();
        HMENU submenu=CreatePopupMenu();
        AppendMenuW(submenu,MF_STRING|(language==remote_i18n::Language::System?MF_CHECKED:0),101,remote_i18n::Text(L"跟随系统"));
        AppendMenuW(submenu,MF_STRING|(language==remote_i18n::Language::SimplifiedChinese?MF_CHECKED:0),102,L"简体中文");
        AppendMenuW(submenu,MF_STRING|(language==remote_i18n::Language::English?MF_CHECKED:0),103,L"English");
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(submenu),remote_i18n::Text(L"语言"));
    }
    void Activate(Action action) {
        switch (action) {
        case Action::LanguageSystem: ChangeLanguage(remote_i18n::Language::System); break;
        case Action::LanguageEnglish: ChangeLanguage(remote_i18n::Language::English); break;
        case Action::LanguageChinese: ChangeLanguage(remote_i18n::Language::SimplifiedChinese); break;
        case Action::Connect: Connect(); break;
        case Action::KeyboardMode: ReleaseInputs(); windowsKeys = !windowsKeys; InvalidateRect(window, nullptr, FALSE); break;
        case Action::Metrics: metricsVisible = !metricsVisible; Activity(); PositionOverlays(); break;
        case Action::Clipboard: exchange.ToggleClipboard(); Activity(); break;
        case Action::Files: ReleaseInputs(); exchange.ShowFiles(); break;
        case Action::More: MoreMenu(); break;
        case Action::Fullscreen: ToggleFullscreen(); break;
        case Action::Disconnect: Disconnect(); break;
        case Action::Sessions: showSettings = false; Layout(); break;
        case Action::Settings: showSettings = true; Layout(); break;
        }
        if (toolbar.window) InvalidateRect(toolbar.window, nullptr, FALSE);
    }
    void CursorPacket(const std::vector<uint8_t>& packet) {
        if (packet.size() < sizeof(RemoteCursorHeader)) return;
        RemoteCursorHeader header {}; std::memcpy(&header, packet.data(), sizeof header);
        if (header.flags & 3) { hiddenCursor = true; remoteCursor = nullptr; return; }
        if (!header.width || !header.height || header.width > 256 || header.height > 256 ||
            header.hotspot_x >= header.width || header.hotspot_y >= header.height ||
            header.rgba_bytes != static_cast<uint64_t>(header.width) * header.height * 4 ||
            packet.size() != sizeof header + header.rgba_bytes) return;
        uint64_t hash = 1469598103934665603ULL;
        for (uint8_t byte : packet) { hash ^= byte; hash *= 1099511628211ULL; }
        auto found = cursorCache.find(hash);
        if (found != cursorCache.end()) { remoteCursor = found->second; hiddenCursor = false; return; }
        auto cursor = RemoteNativeCursorFromRgba(header, packet.data() + sizeof header);
        if (!cursor) return;
        if (cursorCache.size() >= 48) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            for (const auto& item : cursorCache) DestroyCursor(item.second);
            cursorCache.clear();
        }
        cursorCache.emplace(hash, cursor); remoteCursor = cursor; hiddenCursor = false;
    }
    void Poll() {
        remote_ui_watchdog::Beat("clipboard-poll");
        exchange.Tick();
        remote_ui_watchdog::Beat("session-poll");
        session->Tick();
        auto next = session->Snapshot();
        if (localError && next.state == SessionState::Idle) return;
        bool nextConnected = next.state == SessionState::Connected;
        bool sizeChanged = next.videoWidth != snapshot.videoWidth || next.videoHeight != snapshot.videoHeight;
        if (connected && !nextConnected) ReleaseInputs();
        bool changed = connected != nextConnected;
        bool canvasVisibleChanged = (next.hardwareDecode || next.fps > 0) != (snapshot.hardwareDecode || snapshot.fps > 0);
        bool restartChanged = next.restartState != snapshot.restartState || next.restartRequestId != snapshot.restartRequestId;
        bool codecChanged = next.macEncoderMode != snapshot.macEncoderMode || next.hardwareDecode != snapshot.hardwareDecode;
        bool captureChanged = next.captureState != snapshot.captureState || next.message != snapshot.message || restartChanged;
        snapshot = std::move(next); connected = nextConnected;
        if (restartChanged) restartNoticeUntil = GetTickCount64() + (snapshot.restartState == "authorizing" ? 25000 : 10000);
        if (pendingPaste) {
            if (!connected || GetFocus() != canvas || !appActive) {
                pendingPaste = false; exchange.CancelPendingPaste();
            } else {
                auto transaction = exchange.TakePreparedPaste();
                if (transaction.second) {
                    Send(INPUT_RELEASE_ALL);
                    RemoteInputEvent paste {INPUT_PASTE, pendingPasteFlags, pendingPasteX, pendingPasteY, transaction.first, 0, transaction.second};
                    session->SendInput(reinterpret_cast<const uint8_t*>(&paste), sizeof paste);
                    pendingPaste = false; restoreModifiers = true;
                } else if (!exchange.PastePending()) {
                    pendingPaste = false;
                    localNoticeMessage = L"剪贴板未能及时准备好，本次粘贴已取消。请稍后重试。";
                    localNoticeUntil = GetTickCount64() + 8000;
                    PositionOverlays();
                }
            }
        }
        std::vector<uint8_t> cursor;
        while (session->TakeCursor(cursor)) CursorPacket(cursor);
        if (changed) {
            toolsVisible = connected; lastToolActivity = GetTickCount64();
            if (!connected && fullscreen) ToggleFullscreen();
            Layout(); if (connected && GetForegroundWindow() == window) SetFocus(canvas);
        }
        else if (canvasVisibleChanged && connected) Layout();
        else if (sizeChanged && connected) UpdateViewport();
        if (captureChanged) PositionOverlays();
        POINT position {}; GetCursorPos(&position);
        if (WindowFromPoint(position) == canvas) {
            ScreenToClient(canvas, &position);
            bool inVideo = PtInRect(&videoRect, position) != FALSE;
            SetCursor(inVideo ? (hiddenCursor ? nullptr : remoteCursor ? remoteCursor : LoadCursorW(nullptr, IDC_ARROW)) : LoadCursorW(nullptr, IDC_ARROW));
        }
        PollOverlays();
        if (!NeedsCaptureNotice() && captureNotice.window && IsWindowVisible(captureNotice.window)) PositionOverlays();
        static uint32_t ticks = 0;
        if (changed || codecChanged || (++ticks % 15 == 0)) {
            InvalidateRect(window, nullptr, FALSE);
            if (metrics.window && IsWindowVisible(metrics.window)) InvalidateRect(metrics.window, nullptr, FALSE);
        remote_ui_watchdog::Beat();
        }
    }
    static LRESULT CALLBACK EditWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto ui = reinterpret_cast<Ui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!ui) return DefWindowProcW(hwnd, message, wParam, lParam);
        if (message == WM_KEYDOWN && wParam == VK_RETURN) { ui->Connect(); return 0; }
        return CallWindowProcW(ui->editProc, hwnd, message, wParam, lParam);
    }
    static LRESULT CALLBACK CanvasWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto ui = reinterpret_cast<Ui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { ui = static_cast<Ui*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui)); }
        if (!ui) return DefWindowProcW(hwnd, message, wParam, lParam);
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); return 0; }
        case WM_SIZE: ui->UpdateViewport(); return 0;
        case WM_SETCURSOR: {
            POINT position {}; GetCursorPos(&position); ScreenToClient(hwnd, &position);
            bool inVideo = PtInRect(&ui->videoRect, position) != FALSE;
            SetCursor(inVideo ? (ui->hiddenCursor ? nullptr : ui->remoteCursor ? ui->remoteCursor : LoadCursorW(nullptr, IDC_ARROW)) : LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        case WM_MOUSEMOVE:
            if (ui->CanvasActivity(lParam)) return 0;
            if (ui->InsideVideo(lParam) || ui->mouseButtons) { ui->Position(lParam); ui->Send(INPUT_MOVE); }
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK:
            ui->MouseButton(message, lParam); return 0;
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
            POINT p {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (ui->IsOverlayAt(p)) return 0;
            ScreenToClient(hwnd, &p);
            if (!PtInRect(&ui->videoRect, p)) return 0;
            ui->Position(MAKELPARAM(p.x, p.y));
            // Accumulate high-resolution wheel messages; wire values are whole detents.
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int& accumulated = message == WM_MOUSEWHEEL ? ui->wheelVertical : ui->wheelHorizontal;
            accumulated += delta;
            int detents = accumulated / WHEEL_DELTA;
            accumulated -= detents * WHEEL_DELTA;
            if (detents) ui->Send(INPUT_SCROLL, message == WM_MOUSEWHEEL ? detents : 0, message == WM_MOUSEHWHEEL ? detents : 0);
            return 0;
        }
        case WM_KILLFOCUS: ui->ReleaseInputs(); return 0;
        case WM_CAPTURECHANGED:
            if (ui->mouseButtons && reinterpret_cast<HWND>(lParam) != hwnd) ui->ReleaseInputs();
            return 0;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
            bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            bool pasteModifier = ui->windowsKeys ? (GetKeyState(VK_CONTROL)&0x8000)!=0 : (GetKeyState(VK_LWIN)&0x8000)!=0 || (GetKeyState(VK_RWIN)&0x8000)!=0;
            const bool modifier = wParam == VK_CONTROL || wParam == VK_SHIFT || wParam == VK_MENU ||
                wParam == VK_LCONTROL || wParam == VK_RCONTROL || wParam == VK_LSHIFT || wParam == VK_RSHIFT ||
                wParam == VK_LMENU || wParam == VK_RMENU || wParam == VK_LWIN || wParam == VK_RWIN;
            const bool pasteV = wParam == 'V' && (ui->suppressPasteV || (pasteModifier && !(GetKeyState(VK_MENU) & 0x8000)));
            if (down && !pasteV && !modifier && ui->pendingPaste) {
                ui->pendingPaste = false; ui->exchange.CancelPendingPaste();
            }
            if (wParam=='C' && down && pasteModifier && !(lParam&(1L<<30))) ui->exchange.RemoteCopy();
            if (wParam=='V' && ui->suppressPasteV) { if(!down)ui->suppressPasteV=false; return 0; }
            if (wParam=='V' && down && pasteModifier && !(lParam&(1L<<30)) && !(GetKeyState(VK_MENU)&0x8000)) {
                auto transaction=ui->exchange.PreparePaste();
                if (transaction.second || ui->exchange.PastePending()) {
                    ui->ReleaseInputs(false);
                    ui->pendingPasteFlags = (ui->windowsKeys ? 2u : 0u) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 16u : 0u);
                    ui->pendingPasteX = ui->mouseX; ui->pendingPasteY = ui->mouseY;
                    if (transaction.second) {
                        RemoteInputEvent paste {INPUT_PASTE, ui->pendingPasteFlags, ui->mouseX, ui->mouseY, transaction.first, 0, transaction.second};
                        ui->session->SendInput(reinterpret_cast<const uint8_t*>(&paste), sizeof paste);
                    } else ui->pendingPaste = true;
                    ui->suppressPasteV = true; ui->restoreModifiers = true; return 0;
                }
            }
            // The atomic paste releases remote modifiers. Reconcile physical keys
            // before the user's next action, so holding Ctrl across shortcuts works.
            if(down && ui->restoreModifiers){ui->restoreModifiers=false;for(int modifier:{VK_LCONTROL,VK_RCONTROL,VK_LSHIFT,VK_RSHIFT,VK_LMENU,VK_RMENU,VK_LWIN,VK_RWIN})if(modifier!=(int)wParam&&(GetKeyState(modifier)&0x8000)){bool extended=modifier==VK_RCONTROL||modifier==VK_RMENU||modifier==VK_LWIN||modifier==VK_RWIN;ui->Send(INPUT_KEY,modifier,(int)MapVirtualKeyW(modifier,MAPVK_VK_TO_VSC),1u|(extended?4u:0u));}}
            if (down && ui->Shortcut(wParam, (lParam & (1L << 30)) != 0)) return 0;
            if (wParam == VK_F11) return 0;
            if (ui->imeComposing || wParam == VK_PROCESSKEY || wParam == VK_PACKET) return DefWindowProcW(hwnd, message, wParam, lParam);
            int vk = static_cast<int>(wParam), scan = (lParam >> 16) & 0xff;
            uint32_t flags = (down ? 1u : 0u) | ((lParam & (1L << 24)) ? 4u : 0u);
            ui->Send(INPUT_KEY, vk, scan, flags); return 0;
        }
        case WM_IME_STARTCOMPOSITION: ui->imeComposing = true; return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_IME_ENDCOMPOSITION: ui->imeComposing = false; return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_IME_COMPOSITION:
            if (lParam & GCS_RESULTSTR) {
                HIMC context = ImmGetContext(hwnd);
                LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
                if (bytes > 0 && bytes < 65536) {
                    std::wstring text(bytes / sizeof(WCHAR), L'\0');
                    ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), bytes); ui->TextInput(text);
                }
                ImmReleaseContext(hwnd, context); return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_CHAR: case WM_SYSCHAR:
            // Ordinary keyboard events already went to the Mac. IME commits use INPUT_TEXT.
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto ui = reinterpret_cast<Ui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { ui = static_cast<Ui*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams); ui->window = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui)); }
        if (!ui) return DefWindowProcW(hwnd, message, wParam, lParam);
        // The self-test replays own-window input. Real desktop focus changes
        // and taskbar minimize requests must not alter that synthetic state.
        if(ui->syntheticHarness && (message==WM_ACTIVATEAPP ||
            (message==WM_SYSCOMMAND && (wParam&0xfff0)==SC_MINIMIZE)))return 0;
        switch (message) {
        case WM_CREATE:
            ui->scale = GetDpiForWindow(hwnd) / 96.f;
            {
                RECT client {}; GetClientRect(hwnd, &client);
                ui->canvas = CreateWindowExW(0, kCanvasClass, remote_i18n::Text(L"Mac 桌面"), WS_CHILD | WS_TABSTOP | WS_VISIBLE,
                    0, 0, std::max(1L, client.right), std::max(1L, client.bottom), hwnd, nullptr, ui->instance, ui);
            }
            if (!ui->CreateOverlays()) { ui->DestroyOverlays(); return -1; }
            ui->CreateEdits(); ui->Layout(); SetTimer(hwnd, kPollTimer, 16, nullptr);
            ui->exchange.Attach(hwnd,ui->canvas,ui->session);
            if (ui->autoConnect) PostMessageW(hwnd, kConnectMessage, 0, 0);
            return 0;
        case kConnectMessage: ui->Connect(); return 0;
        case WM_CLIPBOARDUPDATE: ui->exchange.ClipboardChanged(); return 0;
        case WM_SIZE: if (ui->address) ui->Layout(); return 0;
        case WM_MOVE: if (ui->address) ui->PositionOverlays(); return 0;
        case WM_DPICHANGED: {
            ui->scale = HIWORD(wParam) / 96.f; ui->paint.ResetTarget(); ui->UpdateFonts();
            ui->toolbar.paint.ResetTarget(); ui->metrics.paint.ResetTarget();
            ui->captureNotice.paint.ResetTarget();
            auto suggested = reinterpret_cast<RECT*>(lParam);
            RECT rect = *suggested;
            if (ui->fullscreen) {
                MONITORINFO monitor {}; monitor.cbSize = sizeof monitor;
                GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &monitor); rect = monitor.rcMonitor;
            }
            SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left,
                rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE); ui->Layout(); return 0;
        }
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lParam); float scale = GetDpiForWindow(hwnd) / 96.f;
            info->ptMinTrackSize.x = static_cast<LONG>((ui->connected ? 820 : 1060) * scale);
            info->ptMinTrackSize.y = static_cast<LONG>((ui->connected ? 420 : 810) * scale); return 0;
        }
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); ui->RenderWindow(); EndPaint(hwnd, &ps); return 0; }
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(235, 242, 250)); SetBkColor(reinterpret_cast<HDC>(wParam), RGB(13, 21, 30));
            return reinterpret_cast<LRESULT>(ui->editBrush);
        case WM_MOUSEMOVE: {
            if (ui->connected) { ui->CanvasActivity(lParam); return 0; }
            float x = GET_X_LPARAM(lParam) / ui->scale, y = GET_Y_LPARAM(lParam) / ui->scale;
            int hover = -1;
            for (size_t i = 0; i < ui->hits.size(); ++i) { auto r = ui->hits[i].rect; if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) { hover = static_cast<int>(i); break; } }
            if (hover != ui->hovered) { ui->hovered = hover; InvalidateRect(hwnd, nullptr, FALSE); }
            TRACKMOUSEEVENT tracking {sizeof tracking, TME_LEAVE, hwnd, 0}; TrackMouseEvent(&tracking); return 0;
        }
        case WM_MOUSELEAVE: ui->hovered = -1; InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_SETCURSOR: if (LOWORD(lParam) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, ui->hovered >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; } break;
        case WM_LBUTTONUP:
            if (ui->hovered >= 0 && ui->hovered < static_cast<int>(ui->hits.size())) ui->Activate(ui->hits[ui->hovered].action);
            return 0;
        case WM_TIMER: if (wParam == kPollTimer) ui->Poll(); return 0;
        case WM_ACTIVATEAPP:
            ui->appActive = wParam != 0;
            if (!wParam) ui->ReleaseInputs(); else if (ui->connected) ui->RevealTools();
            ui->PositionOverlays(); return 0;
        case WM_KEYDOWN: if (wParam == VK_RETURN && !ui->connected) { ui->Connect(); return 0; }
            if (ui->Shortcut(wParam, (lParam & (1L << 30)) != 0)) return 0;
            break;
        case WM_CLOSE:
            remote_log::Write(remote_log::Level::Info, "ui", "main_close_begin");
            remote_ui_watchdog::Beat("main-close");
            ui->ReleaseInputs(); ui->session->Disconnect(); DestroyWindow(hwnd); return 0;
        case WM_DESTROY: KillTimer(hwnd, kPollTimer); ui->exchange.Close(); ui->DestroyOverlays(); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
};
void RegisterUiClasses(HINSTANCE instance) {
    WNDCLASSEXW windowClass {}; windowClass.cbSize = sizeof windowClass; windowClass.hInstance = instance; windowClass.lpfnWndProc = Ui::WndProc;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW); windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_REMOTE_NATIVE), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_REMOTE_NATIVE), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    RegisterClassExW(&windowClass);
    WNDCLASSEXW canvasClass {}; canvasClass.cbSize = sizeof canvasClass; canvasClass.hInstance = instance; canvasClass.lpfnWndProc = Ui::CanvasWndProc;
    canvasClass.hCursor = LoadCursorW(nullptr, IDC_ARROW); canvasClass.lpszClassName = kCanvasClass; canvasClass.style = CS_DBLCLKS;
    RegisterClassExW(&canvasClass);
    WNDCLASSEXW overlayClass {}; overlayClass.cbSize = sizeof overlayClass; overlayClass.hInstance = instance;
    overlayClass.lpfnWndProc = Ui::OverlayWndProc; overlayClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    overlayClass.lpszClassName = kOverlayClass; RegisterClassExW(&overlayClass);
}
} // namespace

int CreateUi(HINSTANCE instance, int showCommand, IClientSession& session,
             const ConnectionOptions& initialOptions, bool autoConnect) {
    ComApartment apartment;
    struct WatchdogLifetime {
        WatchdogLifetime() { remote_ui_watchdog::Start(GetCurrentThreadId()); }
        ~WatchdogLifetime() { remote_ui_watchdog::Stop(); }
    } watchdog;
    Ui ui; ui.instance = instance; ui.session = &session; ui.options = initialOptions; ui.autoConnect = autoConnect;
    if (!ui.paint.Init()) return 1;
    RegisterUiClasses(instance);
    UINT dpi = GetDpiForSystem(); RECT size {0, 0, MulDiv(1280, dpi, 96), MulDiv(820, dpi, 96)};
    AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, 0, dpi);
    HWND window = CreateWindowExW(0, kWindowClass, remote_i18n::Text(L"光梭 · GlideBeam"), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance, &ui);
    if (!window) return 1;
    BOOL dark = TRUE; DwmSetWindowAttribute(window, 20, &dark, sizeof dark);
    COLORREF caption = RGB(16, 23, 32), text = RGB(235, 242, 250), border = RGB(36, 49, 63);
    DwmSetWindowAttribute(window, 35, &caption, sizeof caption); DwmSetWindowAttribute(window, 36, &text, sizeof text);
    DwmSetWindowAttribute(window, 34, &border, sizeof border);
    ShowWindow(window, showCommand); UpdateWindow(window); if (!autoConnect) SetFocus(ui.token);
    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        remote_ui_watchdog::Beat("dispatch");
        // Keep native Tab focus between connection fields; the video canvas receives all remote keystrokes.
        if (!ui.connected && message.message == WM_KEYDOWN && message.wParam == VK_TAB) {
            HWND next = GetNextDlgTabItem(window, GetFocus(), (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            if (next) SetFocus(next);
            continue;
        }
        TranslateMessage(&message); DispatchMessageW(&message);
        remote_ui_watchdog::Beat();
    }
    return static_cast<int>(message.wParam);
}

bool RenderUiPreview(const wchar_t* path, UiPreview mode) {
    ComApartment apartment;
    Ui ui; ui.preview = true; ui.width = 1280; ui.height = 820;
    ui.connected = mode != UiPreview::Landing && mode != UiPreview::Preferences;
    ui.showSettings = mode == UiPreview::Preferences;
    ui.fullscreen = mode == UiPreview::Fullscreen || mode == UiPreview::FullscreenHidden;
    ui.previewOverlays = mode != UiPreview::ConnectedHidden && mode != UiPreview::FullscreenHidden;
    ui.metricsVisible = ui.previewOverlays; ui.options.address = "192.168.1.100:47990";
    if (ui.fullscreen) { ui.width = 1920; ui.height = 1080; }
    ui.snapshot.state = SessionState::Connected; ui.snapshot.fps = 29.8;
    ui.snapshot.bitrateMbps = 8.62; ui.snapshot.networkMs = 1.24; ui.snapshot.decodeMs = 1.70;
    ui.snapshot.hardwareDecode = true; ui.snapshot.actualFrames = 1234;
    ui.snapshot.macEncoderMode = MacVideoEncoderMode::AppleSoftware;
    ui.snapshot.audio.state = AudioPlaybackState::Playing;
    ui.snapshot.audio.queueMs = 14.8;
    ui.snapshot.audio.targetMs = 15.0;
    ui.snapshot.audio.deviceQueuedMs = 5.3;
    ui.snapshot.audio.devicePeriodMs = 2.67;
    UINT pixelWidth = static_cast<UINT>(ui.width), pixelHeight = static_cast<UINT>(ui.height);
    IWICImagingFactory* wic = nullptr; IWICBitmap* bitmap = nullptr; ID2D1RenderTarget* target = nullptr;
    IWICStream* stream = nullptr; IWICBitmapEncoder* encoder = nullptr; IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    if (ui.paint.Init() && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmap(pixelWidth, pixelHeight, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) &&
        SUCCEEDED(ui.paint.factory->CreateWicBitmapRenderTarget(bitmap, D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &target))) {
        bool hasTarget = ui.paint.SetTarget(target); target = nullptr;
        if (hasTarget) {
            ui.paint.target->BeginDraw(); ui.Draw();
            if (SUCCEEDED(ui.paint.target->EndDraw()) && (ui.connected || ui.paint.brandIcon) && SUCCEEDED(wic->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
                SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
                SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
                SUCCEEDED(frame->SetSize(pixelWidth, pixelHeight))) {
                WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
                ok = SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(bitmap, nullptr)) &&
                     SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
            }
        }
    }
    Release(frame); Release(encoder); Release(stream); Release(target); Release(bitmap); Release(wic);
    return ok;
}

bool RunUiSelfTest(HINSTANCE instance, const wchar_t* reportPath) {
    // This session deliberately has no transport, decoder, configuration or token.
    class SyntheticSession final : public IClientSession {
    public:
        SessionSnapshot state;
        uint64_t ordinaryInputs = 0, releaseInputs = 0, f11Inputs = 0, disconnects = 0;
        SyntheticSession() { state.state = SessionState::Connected; state.hardwareDecode = true; }
        bool Connect(HWND, const ConnectionOptions&) override { return false; }
        void Disconnect() override { ++disconnects; state.state = SessionState::Idle; }
        void SendInput(const uint8_t* data, size_t size) override {
            if (size < sizeof(RemoteInputEvent)) return;
            RemoteInputEvent event {}; std::memcpy(&event, data, sizeof event);
            if (event.type == INPUT_RELEASE_ALL) ++releaseInputs; else ++ordinaryInputs;
            if (event.type == INPUT_KEY && event.a == VK_F11) ++f11Inputs;
        }
        void Tick() override {}
        SessionSnapshot Snapshot() const override { return state; }
        void ResizeViewport(const RECT&) override {}
        bool TakeCursor(std::vector<uint8_t>&) override { return false; }
    } session;
    ComApartment apartment;
    Ui ui; ui.instance = instance; ui.session = &session; ui.syntheticHarness = true;
    ui.connected = true; ui.snapshot = session.Snapshot(); ui.options.address = "synthetic.invalid:47990";
    std::vector<std::pair<std::string, bool>> checks;
    auto check = [&](const char* name, bool value) { checks.emplace_back(name, value); };
    auto pump = [] {
        MSG message {};
        for (unsigned n = 0; n < 200 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
            // Synthetic SendMessage input above is delivered directly; these
            // queued mouse messages originate from the physical desktop.
            if(message.message>=WM_MOUSEFIRST && message.message<=WM_MOUSELAST)continue;
            if (message.message != WM_QUIT) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
    };
    auto canvasRect = [&] {
        RECT rect {}; GetWindowRect(ui.canvas, &rect);
        MapWindowPoints(nullptr, ui.window, reinterpret_cast<POINT*>(&rect), 2); return rect;
    };
    auto fullCanvas = [&] {
        RECT client {}; GetClientRect(ui.window, &client); RECT canvas = canvasRect();
        return EqualRect(&client, &canvas) != FALSE;
    };
    auto repaint = [&](Overlay& overlay) { InvalidateRect(overlay.window, nullptr, FALSE); UpdateWindow(overlay.window); };
    auto metricsRect = [&] {
        RECT rect {}; GetWindowRect(ui.metrics.window, &rect);
        MapWindowPoints(nullptr, ui.window, reinterpret_cast<POINT*>(&rect), 2); return rect;
    };
    auto metricsInside = [&] {
        RECT rect = metricsRect(), client {}; GetClientRect(ui.window, &client);
        return rect.left >= client.left && rect.top >= client.top && rect.right <= client.right && rect.bottom <= client.bottom;
    };
    auto captureNoticeCentered = [&] {
        RECT rect {}, client {}; GetWindowRect(ui.captureNotice.window, &rect); GetClientRect(ui.window, &client);
        MapWindowPoints(nullptr, ui.window, reinterpret_cast<POINT*>(&rect), 2);
        return rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom &&
            std::abs((rect.left + rect.right) - client.right) <= 1 && std::abs((rect.top + rect.bottom) - client.bottom) <= 1;
    };
    auto dragMetricsTo = [&](int x, int y) {
        const int grabX = static_cast<int>(40 * ui.scale), grabY = static_cast<int>(20 * ui.scale);
        SendMessageW(ui.metrics.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(grabX, grabY));
        POINT screen {x + grabX, y + grabY}; ClientToScreen(ui.window, &screen);
        POINT point = screen; ScreenToClient(ui.metrics.window, &point);
        SendMessageW(ui.metrics.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        point = screen; ScreenToClient(ui.metrics.window, &point);
        SendMessageW(ui.metrics.window, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
    };
    auto click = [&](Overlay& overlay, Action action) {
        repaint(overlay);
        for (const auto& hit : overlay.hits) {
            if (hit.action != action) continue;
            LPARAM point = MAKELPARAM(static_cast<int>((hit.rect.left + hit.rect.right) * ui.scale / 2),
                static_cast<int>((hit.rect.top + hit.rect.bottom) * ui.scale / 2));
            SendMessageW(overlay.window, WM_LBUTTONDOWN, MK_LBUTTON, point);
            SendMessageW(overlay.window, WM_LBUTTONUP, 0, point); return true;
        }
        return false;
    };
    uint64_t hideDelayMs = 0;
    std::string diagnostics;
    HWND window = nullptr;
    if (ui.paint.Init()) {
        RegisterUiClasses(instance);
        UINT dpi = GetDpiForSystem();
        window = CreateWindowExW(0, kWindowClass, L"Remote · 原生会话界面自检（合成）",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1000, dpi, 96), MulDiv(700, dpi, 96),
            nullptr, nullptr, instance, &ui);
    }
    check("native_window_created", window && ui.canvas && ui.toolbar.window && ui.metrics.window && ui.captureNotice.window);
    if (window) {
        KillTimer(window, kPollTimer);
        // STARTUPINFO (for example a hidden CLI launch) overrides the first ShowWindow call.
        // The second call explicitly shows this own test window without activating another app.
        ShowWindow(window, SW_SHOW); ShowWindow(window, SW_SHOWNOACTIVATE); pump();
        // Isolate physical hit testing from other live clients without taking
        // foreground focus. This applies only to the synthetic test owner.
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        check("selftest_owner_isolated_from_other_live_clients", (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
        ui.appActive = true; ui.toolsVisible = true; ui.lastToolActivity = GetTickCount64(); ui.Layout();
        repaint(ui.toolbar);
        diagnostics = "  \"initial_native_state\": {\"connected\": " + std::string(ui.connected ? "true" : "false") +
            ", \"app_active\": " + (ui.appActive ? "true" : "false") + ", \"owner_visible\": " + (IsWindowVisible(window) ? "true" : "false") +
            ", \"owner_minimized\": " + (IsIconic(window) ? "true" : "false") + ", \"toolbar_owner_matches\": " + (GetWindow(ui.toolbar.window, GW_OWNER) == window ? "true" : "false") +
            ", \"toolbar_style\": " + std::to_string(static_cast<uint32_t>(GetWindowLongPtrW(ui.toolbar.window, GWL_STYLE))) +
            ", \"toolbar_visible\": " + (IsWindowVisible(ui.toolbar.window) ? "true" : "false") + "},\n";
        RECT initialCanvas = canvasRect();
        const auto previousLanguage=remote_i18n::GetLanguage();
        ui.ChangeLanguage(remote_i18n::Language::English,false);
        wchar_t localizedTitle[128]{};GetWindowTextW(window,localizedTitle,128);
        check("english_live_switch_updates_native_title",std::wstring(localizedTitle)==L"GlideBeam");
        check("english_live_switch_keeps_session_and_canvas",ui.connected&&session.disconnects==0&&fullCanvas());
        check("english_live_switch_localizes_capture_and_errors",remote_i18n::Translate("画面共享已停止，请在 Mac 端点「重试画面采集」恢复。").find("Screen sharing stopped")!=std::string::npos);
        ui.ChangeLanguage(remote_i18n::Language::SimplifiedChinese,false);
        GetWindowTextW(window,localizedTitle,128);
        check("chinese_live_switch_updates_native_title",std::wstring(localizedTitle)==L"光梭 · GlideBeam");
        check("chinese_live_switch_keeps_session_and_canvas",ui.connected&&session.disconnects==0&&fullCanvas());
        ui.ChangeLanguage(previousLanguage,false);
        check("canvas_fills_entire_client", fullCanvas());
        check("toolbar_is_native_owned_popup", GetWindow(ui.toolbar.window, GW_OWNER) == window &&
            (GetWindowLongPtrW(ui.toolbar.window, GWL_STYLE) & WS_POPUP) && IsWindowVisible(ui.toolbar.window));
        check("overlay_does_not_activate", SendMessageW(ui.toolbar.window, WM_MOUSEACTIVATE, 0, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)) == MA_NOACTIVATE);

        uint64_t before = session.ordinaryInputs;
        SendMessageW(ui.canvas, WM_KEYDOWN, 'A', 1);
        SendMessageW(ui.canvas, WM_KEYUP, 'A', 1 | (1LL << 30) | (1LL << 31));
        check("ordinary_keyboard_reaches_synthetic_remote_callback", session.ordinaryInputs == before + 2);
        before = session.ordinaryInputs;
        bool clicked = click(ui.toolbar, Action::Metrics);
        RECT after = canvasRect();
        check("metrics_toggle_visible_without_canvas_resize", clicked && ui.metricsVisible && IsWindowVisible(ui.metrics.window) && EqualRect(&initialCanvas, &after));
        POINT screen {static_cast<LONG>(40 * ui.scale), static_cast<LONG>(80 * ui.scale)};
        ClientToScreen(ui.metrics.window, &screen);
        check("metrics_is_above_canvas", ui.IsOverlayAt(screen));
        SendMessageW(ui.toolbar.window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(screen.x, screen.y));
        // Wheel messages can target the focused canvas even while the pointer is over a popup.
        SendMessageW(ui.canvas, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(screen.x, screen.y));
        SendMessageW(ui.canvas, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(screen.x, screen.y));
        check("overlay_click_and_focus_delivered_wheel_are_not_remote_input", session.ordinaryInputs == before);

        SetFocus(ui.canvas); before = session.ordinaryInputs;
        const uint64_t releasesBeforeDrag = session.releaseInputs;
        const HWND focusBeforeDrag = GetFocus();
        const bool toolsBeforeDrag = ui.toolsVisible;
        dragMetricsTo(40, 140);
        RECT dragged = metricsRect(); after = canvasRect();
        check("metrics_title_drag_moves_without_focus_canvas_or_input_changes", dragged.left == 40 && dragged.top == 140 &&
            metricsInside() && GetFocus() == focusBeforeDrag && focusBeforeDrag == ui.canvas && !ui.metrics.dragging &&
            GetCapture() != ui.metrics.window && EqualRect(&initialCanvas, &after) && ui.toolsVisible == toolsBeforeDrag &&
            session.ordinaryInputs == before && session.releaseInputs == releasesBeforeDrag);
        dragMetricsTo(-10000, -10000); dragged = metricsRect();
        check("metrics_drag_clamps_top_left", dragged.left == 0 && dragged.top == 0 && metricsInside());
        dragMetricsTo(10000, 10000); dragged = metricsRect();
        RECT client {}; GetClientRect(window, &client);
        check("metrics_drag_clamps_bottom_right", dragged.right == client.right && dragged.bottom == client.bottom && metricsInside());
        SendMessageW(ui.metrics.window, WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(static_cast<int>(40 * ui.scale), static_cast<int>(100 * ui.scale)));
        bool bodyNotDraggable = !ui.metrics.dragging;
        SendMessageW(ui.metrics.window, WM_CANCELMODE, 0, 0);
        SendMessageW(ui.metrics.window, WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(static_cast<int>((kMetricsWidth - 31) * ui.scale), static_cast<int>(25 * ui.scale)));
        check("metrics_body_and_close_button_do_not_start_drag", bodyNotDraggable && !ui.metrics.dragging && ui.metrics.pressed >= 0);
        SendMessageW(ui.metrics.window, WM_CANCELMODE, 0, 0);
        check("metrics_drag_and_cancel_never_send_remote_input", session.ordinaryInputs == before && session.releaseInputs == releasesBeforeDrag);

        RECT ownerBeforeResize {}; GetWindowRect(window, &ownerBeforeResize);
        SetWindowPos(window, nullptr, ownerBeforeResize.left + 30, ownerBeforeResize.top + 20,
            static_cast<int>(800 * ui.scale), static_cast<int>(500 * ui.scale), SWP_NOZORDER | SWP_NOACTIVATE); pump();
        check("metrics_clamps_after_owner_move_and_resize", metricsInside() && fullCanvas());
        const RECT movedMetrics = metricsRect();
        SetWindowPos(window, nullptr, ownerBeforeResize.left, ownerBeforeResize.top,
            ownerBeforeResize.right - ownerBeforeResize.left, ownerBeforeResize.bottom - ownerBeforeResize.top,
            SWP_NOZORDER | SWP_NOACTIVATE); pump();
        dragged = metricsRect();
        check("metrics_keeps_user_position_after_owner_move", dragged.left == movedMetrics.left && dragged.top == movedMetrics.top && metricsInside());
        RECT dpiSuggested {}; GetWindowRect(window, &dpiSuggested);
        const UINT actualDpi = GetDpiForWindow(window);
        SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&dpiSuggested)); pump();
        check("metrics_clamps_after_synthetic_dpi_transition", metricsInside() && fullCanvas() && ui.scale == 1.5f);
        SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(actualDpi, actualDpi), reinterpret_cast<LPARAM>(&dpiSuggested)); pump();

        const uint64_t beforeCaptureNotice = session.ordinaryInputs;
        const uint64_t releasesBeforeNotice = session.releaseInputs;
        ui.toolsVisible = false; ui.PositionOverlays();
        check("capture_notice_hidden_for_legacy_host", !ui.NeedsCaptureNotice() && !IsWindowVisible(ui.captureNotice.window));
        bool allInactiveVisible = true;
        for (const char* state : {"starting", "recovering", "suspended", "stopped", "user-stopped", "blank", "permission", "failed"}) {
            session.state.captureState = state; session.state.message = "合成状态：请在 Mac 端检查画面采集。";
            ui.Poll(); repaint(ui.captureNotice);
            allInactiveVisible = allInactiveVisible && ui.captureNotice.paint.target && IsWindowVisible(ui.captureNotice.window) &&
                !IsWindowVisible(ui.toolbar.window) && captureNoticeCentered();
        }
        check("all_inactive_capture_states_show_centered_notice_with_toolbar_hidden", allInactiveVisible);
        BYTE noticeAlpha = 0; DWORD noticeFlags = 0;
        const bool alphaRead = GetLayeredWindowAttributes(ui.captureNotice.window, nullptr, &noticeAlpha, &noticeFlags) != FALSE;
        check("capture_notice_is_translucent_and_nonactivating", alphaRead && noticeAlpha == 235 && (noticeFlags & LWA_ALPHA) &&
            SendMessageW(ui.captureNotice.window, WM_MOUSEACTIVATE, 0, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)) == MA_NOACTIVATE);
        const HWND beforeNoticeFocus = GetFocus();
        SendMessageW(ui.captureNotice.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(80, 40));
        SendMessageW(ui.captureNotice.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(90, 45));
        SendMessageW(ui.captureNotice.window, WM_LBUTTONUP, 0, MAKELPARAM(90, 45));
        POINT noticePoint {80, 40}; ClientToScreen(ui.captureNotice.window, &noticePoint);
        SendMessageW(ui.canvas, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(noticePoint.x, noticePoint.y));
        check("capture_notice_gestures_do_not_send_recovery_input_or_change_focus", session.ordinaryInputs == beforeCaptureNotice &&
            session.releaseInputs == releasesBeforeNotice && GetFocus() == beforeNoticeFocus && GetCapture() != ui.captureNotice.window);
        check("capture_notice_keeps_full_video_canvas", fullCanvas());
        session.state.captureState = "running"; session.state.message = "合成采集恢复。"; ui.Poll();
        check("capture_running_immediately_hides_notice", !ui.NeedsCaptureNotice() && !IsWindowVisible(ui.captureNotice.window));
        session.state.captureState.clear(); session.state.message.clear(); ui.Poll();
        bool restartStatusesVisible = true;
        for (const char* state : {"authorizing", "requested", "cancelled", "denied", "error", "busy"}) {
            session.state.restartState = state;
            session.state.restartRequestId = "00000000-0000-4000-8000-000000000001";
            session.state.restartMessage = "合成重启状态；此测试没有执行重启。";
            ui.Poll(); repaint(ui.captureNotice);
            restartStatusesVisible = restartStatusesVisible && ui.RestartNoticeActive() && IsWindowVisible(ui.captureNotice.window);
        }
        check("restart_statuses_visible_without_executing_restart", restartStatusesVisible);
        SendMessageW(ui.captureNotice.window, WM_LBUTTONUP, 0, MAKELPARAM(70, 60));
        check("restart_notice_can_be_dismissed_to_use_native_mac_authorization", !IsWindowVisible(ui.captureNotice.window) && !ui.RestartNoticeActive());
        session.state.restartState = "authorizing"; ui.Poll(); ui.restartNoticeUntil = GetTickCount64() - 1; ui.Poll();
        check("restart_status_notice_expires", !IsWindowVisible(ui.captureNotice.window));
        session.state.restartState.clear(); session.state.restartMessage.clear(); session.state.restartRequestId.clear(); ui.Poll();
        ui.toolsVisible = true; ui.PositionOverlays();
        repaint(ui.toolbar);
        bool morePresent = false, allButtonsFit = true;
        float previousRight = 0;
        for (const auto& hit : ui.toolbar.hits) {
            morePresent = morePresent || hit.action == Action::More;
            allButtonsFit = allButtonsFit && hit.rect.left >= previousRight && hit.rect.right <= kToolbarWidth;
            previousRight = hit.rect.right;
        }
        check("toolbar_more_entry_and_buttons_fit_without_overlap", morePresent && allButtonsFit);
        BYTE previousKeyboard[256] {}, cleanKeyboard[256] {};
        GetKeyboardState(previousKeyboard); SetKeyboardState(cleanKeyboard);
        ui.pendingPaste = true; ui.suppressPasteV = false;
        SendMessageW(ui.canvas, WM_KEYDOWN, 'V', 1);
        SendMessageW(ui.canvas, WM_KEYUP, 'V', 1);
        check("ordinary_v_cancels_pending_paste_after_modifier_release", !ui.pendingPaste);
        ui.pendingPaste = true;
        SendMessageW(ui.canvas, WM_KEYDOWN, 'A', 1);
        SendMessageW(ui.canvas, WM_KEYUP, 'A', 1);
        check("next_typing_cancels_pending_paste", !ui.pendingPaste);
        ui.pendingPaste = true;
        const LPARAM canvasCenter = MAKELPARAM((ui.videoRect.left + ui.videoRect.right) / 2, (ui.videoRect.top + ui.videoRect.bottom) / 2);
        SendMessageW(ui.canvas, WM_LBUTTONDOWN, MK_LBUTTON, canvasCenter);
        SendMessageW(ui.canvas, WM_LBUTTONUP, 0, canvasCenter);
        check("next_mouse_click_cancels_pending_paste", !ui.pendingPaste);
        ui.pendingPaste = true;
        SendMessageW(ui.canvas, WM_KILLFOCUS, 0, 0);
        check("focus_loss_cancels_pending_paste", !ui.pendingPaste);
        SetKeyboardState(previousKeyboard);

        POINT outside {-32000, -32000};
        ui.lastToolActivity = GetTickCount64();
        const uint64_t hideStart = ui.lastToolActivity;
        // Use a synthetic pointer, but the real monotonic clock and native ShowWindow path.
        while (ui.toolsVisible && GetTickCount64() - hideStart < 4200) {
            ui.PollOverlays(&outside); pump();
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        hideDelayMs = GetTickCount64() - hideStart;
        after = canvasRect();
        check("toolbar_hides_after_three_seconds", !ui.toolsVisible && !IsWindowVisible(ui.toolbar.window) && hideDelayMs >= 3000 && hideDelayMs < 4200);
        check("hiding_toolbar_preserves_canvas_and_metrics", EqualRect(&initialCanvas, &after) && IsWindowVisible(ui.metrics.window));
        before = session.ordinaryInputs; dragMetricsTo(80, 140); after = canvasRect();
        check("metrics_drag_never_reveals_hidden_toolbar_or_resizes_canvas", !ui.toolsVisible && !IsWindowVisible(ui.toolbar.window) &&
            EqualRect(&initialCanvas, &after) && session.ordinaryInputs == before && metricsInside());
        ui.CanvasActivity(MAKELPARAM(initialCanvas.right / 2, static_cast<int>(5 * ui.scale)));
        after = canvasRect();
        check("top_hotspot_reveals_without_canvas_resize", ui.toolsVisible && IsWindowVisible(ui.toolbar.window) && EqualRect(&initialCanvas, &after));
        uint64_t activityTime = ui.lastToolActivity;
        ui.CanvasActivity(MAKELPARAM(initialCanvas.right / 2, initialCanvas.bottom / 2));
        check("ordinary_desktop_movement_does_not_extend_toolbar_timer", activityTime == ui.lastToolActivity);
        ui.toolsVisible = false; ui.PositionOverlays(); SetCapture(ui.canvas);
        ui.CanvasActivity(MAKELPARAM(initialCanvas.right / 2, static_cast<int>(5 * ui.scale)));
        check("remote_drag_does_not_reveal_toolbar", GetCapture() == ui.canvas && !ui.toolsVisible);
        ReleaseCapture(); ui.RevealTools();
        SetCapture(ui.toolbar.window); ui.lastToolActivity = GetTickCount64() - 3100; ui.PollOverlays(&outside);
        check("captured_toolbar_does_not_auto_hide", GetCapture() == ui.toolbar.window && ui.toolsVisible && IsWindowVisible(ui.toolbar.window));
        ReleaseCapture();
        before = session.ordinaryInputs;
        clicked = click(ui.metrics, Action::Metrics);
        check("metrics_close_completely_hides_panel", clicked && !ui.metricsVisible && !IsWindowVisible(ui.metrics.window) && session.ordinaryInputs == before);

        WINDOWPLACEMENT normal {}; normal.length = sizeof normal; GetWindowPlacement(window, &normal);
        DWORD originalStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        MONITORINFO monitor {}; monitor.cbSize = sizeof monitor; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        SendMessageW(ui.canvas, WM_KEYDOWN, VK_F11, 1); pump();
        RECT fullscreenRect {}; GetWindowRect(window, &fullscreenRect);
        check("f11_is_borderless_and_covers_monitor", ui.fullscreen && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_OVERLAPPEDWINDOW) && EqualRect(&fullscreenRect, &monitor.rcMonitor) && fullCanvas());
        ui.metricsVisible = true; ui.PositionOverlays(); dragMetricsTo(10000, 10000);
        check("metrics_drag_clamps_in_true_fullscreen", metricsInside());
        session.state.captureState = "user-stopped"; session.state.message = "系统停止了共享。请在 Mac 端点击「重试画面采集」。"; ui.Poll();
        check("capture_notice_centers_in_true_fullscreen_and_requires_mac_retry", IsWindowVisible(ui.captureNotice.window) &&
            captureNoticeCentered() && ui.CaptureNeedsManualRetry());
        SendMessageW(ui.canvas, WM_KEYDOWN, VK_F11, 1 | (1LL << 30));
        check("f11_auto_repeat_does_not_toggle", ui.fullscreen);
        SendMessageW(ui.canvas, WM_KEYUP, VK_F11, 1 | (1LL << 30) | (1LL << 31));
        SendMessageW(ui.canvas, WM_KEYDOWN, VK_F11, 1); pump();
        WINDOWPLACEMENT restored {}; restored.length = sizeof restored; GetWindowPlacement(window, &restored);
        diagnostics += "  \"normal_restore_state\": {\"old_style\": " + std::to_string(originalStyle) + ", \"new_style\": " + std::to_string(static_cast<uint32_t>(GetWindowLongPtrW(window, GWL_STYLE))) +
            ", \"old_show\": " + std::to_string(normal.showCmd) + ", \"new_show\": " + std::to_string(restored.showCmd) +
            ", \"old_rect\": [" + std::to_string(normal.rcNormalPosition.left) + "," + std::to_string(normal.rcNormalPosition.top) + "," + std::to_string(normal.rcNormalPosition.right) + "," + std::to_string(normal.rcNormalPosition.bottom) +
            "], \"new_rect\": [" + std::to_string(restored.rcNormalPosition.left) + "," + std::to_string(restored.rcNormalPosition.top) + "," + std::to_string(restored.rcNormalPosition.right) + "," + std::to_string(restored.rcNormalPosition.bottom) + "]},\n";
        check("normal_window_placement_restores", !ui.fullscreen && static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)) == originalStyle && restored.showCmd == normal.showCmd && EqualRect(&normal.rcNormalPosition, &restored.rcNormalPosition));
        check("metrics_clamps_after_leaving_fullscreen", metricsInside());
        check("capture_notice_recenters_after_fullscreen_exit", IsWindowVisible(ui.captureNotice.window) && captureNoticeCentered());
        session.state.captureState = "running"; session.state.message.clear(); ui.Poll();
        check("f11_is_never_sent_to_remote", session.f11Inputs == 0);
        ShowWindow(window, SW_MAXIMIZE); pump();
        check("metrics_stays_inside_maximized_client", metricsInside());
        WINDOWPLACEMENT maximized {}; maximized.length = sizeof maximized; GetWindowPlacement(window, &maximized);
        SendMessageW(ui.canvas, WM_KEYDOWN, VK_F11, 1); pump();
        GetWindowRect(window, &fullscreenRect);
        check("fullscreen_from_maximized_covers_monitor", ui.fullscreen && EqualRect(&fullscreenRect, &monitor.rcMonitor) && fullCanvas());
        SendMessageW(ui.canvas, WM_KEYDOWN, VK_F11, 1); pump();
        GetWindowPlacement(window, &restored);
        check("maximized_window_placement_restores", !ui.fullscreen && IsZoomed(window) && restored.showCmd == maximized.showCmd && EqualRect(&maximized.rcNormalPosition, &restored.rcNormalPosition));

        HWND toolbar = ui.toolbar.window, metrics = ui.metrics.window, notice = ui.captureNotice.window;
        SendMessageW(window, WM_CLOSE, 0, 0); pump();
        check("owner_close_destroys_all_native_overlays", !IsWindow(window) && !IsWindow(toolbar) && !IsWindow(metrics) && !IsWindow(notice) &&
            !ui.toolbar.window && !ui.metrics.window && !ui.captureNotice.window && session.disconnects == 1);
    }
    bool passed = !checks.empty() && std::all_of(checks.begin(), checks.end(), [](const auto& item) { return item.second; });
    std::string report = "{\n  \"passed\": " + std::string(passed ? "true" : "false") +
        ",\n  \"synthetic_session\": true,\n  \"network_used\": false,\n  \"credentials_read\": false,\n  \"real_gpu_decode_test\": false,\n  \"pointer_source\": \"synthetic; native own-window messages and real clock\",\n  \"toolbar_hide_delay_ms\": " +
        std::to_string(hideDelayMs) + ",\n  \"checks\": {\n";
    report.insert(report.find("  \"checks\""), diagnostics);
    for (size_t i = 0; i < checks.size(); ++i) report += "    \"" + checks[i].first + "\": " + (checks[i].second ? "true" : "false") + (i + 1 == checks.size() ? "\n" : ",\n");
    report += "  },\n  \"remaining_manual_checks\": [\"mixed-DPI monitor transition\", \"physical mouse movement and remote desktop interaction\", \"system-reserved keyboard shortcuts\"]\n}\n";
    FILE* file = _wfopen(reportPath, L"wb");
    if (!file) return false;
    bool written = std::fwrite(report.data(), 1, report.size(), file) == report.size();
    if (std::fclose(file) != 0) written = false;
    return passed && written;
}
