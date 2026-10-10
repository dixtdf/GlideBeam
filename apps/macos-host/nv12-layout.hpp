#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Application-side workaround for one known driver build's IOSurface tiling.
// Source and destination must be separate allocations. Dimensions are bytes.
// The final source row is replicated into explicitly allocated coded padding.
namespace remote_nv12 {
inline bool PrepackPlane(const uint8_t *src, size_t srcStride, size_t srcRows,
                         uint8_t *dst, size_t dstStride, size_t dstRows,
                         size_t widthBytes, bool uv) {
    if (!src || !dst || src == dst || !srcRows || srcRows > dstRows ||
        !widthBytes || (widthBytes & 63) || (dstRows & 7) ||
        srcStride < widthBytes || dstStride < widthBytes) return false;
    const size_t groupBytes = uv ? 16 : 8;
    const size_t groups = 64 / groupBytes;
    for (size_t tileY = 0; tileY < dstRows; tileY += 8) {
        for (size_t tileX = 0; tileX < widthBytes; tileX += 64) {
            for (size_t y = 0; y < 8; ++y) {
                const size_t sourceY = tileY + y < srcRows ? tileY + y : srcRows - 1;
                const uint8_t *source = src + sourceY * srcStride + tileX;
                for (size_t g = 0; g < groups; ++g) {
                    const size_t destGroup = uv ? ((g & 2) | ((y & 4) >> 2))
                        : ((g & 4) | ((y & 4) >> 1) | ((g & 2) >> 1));
                    const size_t destRow = uv ? ((y & 3) | ((g & 1) << 2))
                        : (((y & 3) << 1) | (g & 1));
                    uint8_t *target = dst + (tileY + destRow) * dstStride + tileX + destGroup * groupBytes;
                    if (uv) std::memcpy(target, source + g * 16, 16);
                    else std::memcpy(target, source + g * 8, 8);
                }
            }
        }
    }
    return true;
}
}
