#pragma once
#include <windows.h>
#include "../../shared/protocol.h"
#include <cstdint>
#include <vector>

// Convert the top-to-bottom straight RGBA wire image into a native Windows
// cursor. The negative DIB height preserves row order and the top-left hotspot.
inline HCURSOR RemoteNativeCursorFromRgba(const RemoteCursorHeader& header, const uint8_t* rgba) {
    if (!rgba || !header.width || !header.height || header.width > 256 || header.height > 256 ||
        header.hotspot_x >= header.width || header.hotspot_y >= header.height ||
        header.rgba_bytes != static_cast<uint64_t>(header.width) * header.height * 4) return nullptr;
    BITMAPV5HEADER bitmap {}; bitmap.bV5Size = sizeof bitmap;
    bitmap.bV5Width = header.width; bitmap.bV5Height = -static_cast<LONG>(header.height);
    bitmap.bV5Planes = 1; bitmap.bV5BitCount = 32; bitmap.bV5Compression = BI_BITFIELDS;
    bitmap.bV5RedMask = 0x00ff0000; bitmap.bV5GreenMask = 0x0000ff00;
    bitmap.bV5BlueMask = 0x000000ff; bitmap.bV5AlphaMask = 0xff000000;
    void* pixels = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bitmap), DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !pixels) { if (color) DeleteObject(color); return nullptr; }
    auto bgra = static_cast<uint8_t*>(pixels);
    for (size_t i = 0; i < header.rgba_bytes; i += 4) {
        // Preserve straight channels in this V5 bitmap. The native cursor
        // rendering path applies alpha; pre-multiplying here darkens edges twice.
        bgra[i] = rgba[i + 2];
        bgra[i + 1] = rgba[i + 1];
        bgra[i + 2] = rgba[i];
        bgra[i + 3] = rgba[i + 3];
    }
    std::vector<uint8_t> maskPixels(((header.width + 15) / 16) * 2 * header.height, 0);
    HBITMAP mask = CreateBitmap(header.width, header.height, 1, 1, maskPixels.data());
    if (!mask) { DeleteObject(color); return nullptr; }
    ICONINFO info {}; info.fIcon = FALSE; info.xHotspot = header.hotspot_x; info.yHotspot = header.hotspot_y;
    info.hbmMask = mask; info.hbmColor = color;
    HCURSOR cursor = reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
    DeleteObject(mask); DeleteObject(color);
    return cursor;
}
