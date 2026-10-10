#ifndef UNICODE
#define UNICODE
#endif
#include "../cursor-native.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

struct CursorCheck { std::string name; bool passed; };
static std::vector<CursorCheck> checks;
static void Check(const char* name, bool passed) { checks.push_back({name, passed}); }

static bool TestCursor(const char* path, const char* label, bool fixture) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<uint8_t> packet((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (packet.size() < sizeof(RemoteCursorHeader)) return false;
    RemoteCursorHeader header {}; std::memcpy(&header, packet.data(), sizeof(header));
    if (packet.size() != sizeof(header) + header.rgba_bytes) return false;
    const uint8_t* rgba = packet.data() + sizeof(header);
    HCURSOR cursor = RemoteNativeCursorFromRgba(header, rgba);
    Check((std::string(label) + "_native_cursor_created").c_str(), cursor != nullptr);
    if (!cursor) return false;
    ICONINFO info {};
    bool infoOk = GetIconInfo(cursor, &info) != FALSE;
    Check((std::string(label) + "_native_hotspot_matches").c_str(), infoOk && !info.fIcon &&
        info.xHotspot == header.hotspot_x && info.yHotspot == header.hotspot_y);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    BITMAPINFO bitmap {}; bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = header.width; bitmap.bmiHeader.biHeight = -static_cast<LONG>(header.height);
    bitmap.bmiHeader.biPlanes = 1; bitmap.bmiHeader.biBitCount = 32; bitmap.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr); HDC memory = CreateCompatibleDC(screen);
    HBITMAP canvas = CreateDIBSection(screen, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0); ReleaseDC(nullptr, screen);
    if (!canvas || !memory || !bits) { if (canvas) DeleteObject(canvas); if (memory) DeleteDC(memory); DestroyCursor(cursor); return false; }
    auto old = SelectObject(memory, canvas);
    auto pixels = static_cast<uint8_t*>(bits);
    for (size_t i = 0; i < header.rgba_bytes; i += 4) {
        pixels[i] = pixels[i + 1] = pixels[i + 2] = 40; pixels[i + 3] = 255;
    }
    BOOL drawn = DrawIconEx(memory, 0, 0, cursor, header.width, header.height, 0, nullptr, DI_NORMAL);
    GdiFlush();
    bool rgbMatches = drawn != FALSE; unsigned mismatchCount = 0;
    for (size_t i = 0; i < header.rgba_bytes; i += 4) for (int channel = 0; channel < 3; ++channel) {
        unsigned alpha = rgba[i + 3];
        int expected = ((rgba[i + 2 - channel] * alpha + 127) / 255) + ((40 * (255 - alpha) + 127) / 255);
        bool matches = std::abs((int)pixels[i + channel] - expected) <= 1;
        if (!matches && mismatchCount++ < 4) std::fprintf(stderr, "%s pixel %zu channel %d actual %u expected %d alpha %u\n", label, i / 4, channel, pixels[i + channel], expected, alpha);
        rgbMatches &= matches;
    }
    Check((std::string(label) + "_drawiconex_rgba_rows_match").c_str(), rgbMatches);
    if (fixture) {
        Check("asymmetric_known_top_red_bottom_blue", header.width == 7 && header.height == 5 &&
            pixels[2] == 255 && pixels[0] == 0 && pixels[((4 * 7) * 4)] == 255 && pixels[((4 * 7) * 4) + 2] == 0);
        Check("asymmetric_known_hotspot_2_1", infoOk && info.xHotspot == 2 && info.yHotspot == 1);
    }
    SelectObject(memory, old); DeleteObject(canvas); DeleteDC(memory); DestroyCursor(cursor);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 4) { std::fprintf(stderr, "Usage: CursorRender ASYMMETRIC.bin ARROW.bin REPORT.json\n"); return 2; }
    Check("asymmetric_packet_loaded", TestCursor(argv[1], "asymmetric", true));
    Check("real_mac_arrow_packet_loaded", TestCursor(argv[2], "real_mac_arrow", false));
    RemoteCursorHeader invalid {7, 5, 7, 0, 0, 140}; uint8_t empty[140] {};
    Check("out_of_bounds_hotspot_rejected", RemoteNativeCursorFromRgba(invalid, empty) == nullptr);
    bool passed = std::all_of(checks.begin(), checks.end(), [](const auto& check) { return check.passed; });
    std::ofstream report(argv[3]);
    report << "{\n  \"schema\": \"remote.cursor-orientation.windows.v1\",\n  \"passed\": " << (passed ? "true" : "false") << ",\n  \"checks\": [\n";
    for (size_t i = 0; i < checks.size(); ++i)
        report << "    {\"name\":\"" << checks[i].name << "\",\"passed\":" << (checks[i].passed ? "true" : "false") << "}" << (i + 1 == checks.size() ? "\n" : ",\n");
    report << "  ]\n}\n";
    std::printf("Windows native cursor rendering: %s (%zu checks)\n", passed ? "PASS" : "FAIL", checks.size());
    return passed ? 0 : 1;
}
