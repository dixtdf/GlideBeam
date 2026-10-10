#pragma once
#import <AppKit/AppKit.h>
#include "../../shared/protocol.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// The wire image is top-to-bottom, straight-alpha RGBA. NSCursor hotspots
// already use the same top-left origin, expressed in image points.
static inline NSData *RemoteCursorImagePacketFromCGImage(CGImageRef image, NSSize points, NSPoint hotspot) {
    if (!image || !std::isfinite(points.width) || !std::isfinite(points.height) ||
        points.width <= 0 || points.height <= 0 || !std::isfinite(hotspot.x) || !std::isfinite(hotspot.y)) return nil;
    size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
    if (!width || !height || width > 256 || height > 256) return nil;
    NSMutableData *packet = [NSMutableData dataWithLength:sizeof(RemoteCursorHeader) + width * height * 4];
    uint8_t *rgba = (uint8_t *)packet.mutableBytes + sizeof(RemoteCursorHeader);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(rgba, width, height, 8, width * 4, space,
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) | static_cast<uint32_t>(kCGBitmapByteOrder32Big));
    CGColorSpaceRelease(space);
    if (!context) return nil;
    // A fresh bitmap context preserves CGImage row order. Applying the flip
    // used for drawing into an AppKit view would invert these transmitted rows.
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    for (size_t p = 0; p < width * height * 4; p += 4) {
        if (rgba[p + 3]) {
            for (int c = 0; c < 3; ++c)
                rgba[p + c] = (uint8_t)std::min(255u, ((unsigned)rgba[p + c] * 255u + rgba[p + 3] / 2) / rgba[p + 3]);
        } else rgba[p] = rgba[p + 1] = rgba[p + 2] = 0;
    }
    RemoteCursorHeader header = {(uint32_t)width, (uint32_t)height,
        (uint32_t)std::clamp((long)lround(hotspot.x * width / points.width), 0L, (long)width - 1),
        (uint32_t)std::clamp((long)lround(hotspot.y * height / points.height), 0L, (long)height - 1),
        0, (uint32_t)(width * height * 4)};
    memcpy(packet.mutableBytes, &header, sizeof(header));
    return packet;
}

static inline NSData *RemoteCursorImagePacket(NSCursor *cursor) {
    NSImage *image = cursor.image;
    if (!image) return nil;
    CGRect proposed = CGRectMake(0, 0, image.size.width, image.size.height);
    CGImageRef cg = [image CGImageForProposedRect:&proposed context:nil hints:nil];
    return RemoteCursorImagePacketFromCGImage(cg, image.size, cursor.hotSpot);
}
