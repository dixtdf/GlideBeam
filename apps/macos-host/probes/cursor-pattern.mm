#import <AppKit/AppKit.h>
#include "../cursor-image.h"
#include <cstdio>
#include <vector>

static bool SavePacket(NSData *packet, NSString *directory, NSString *name) {
    if (!packet || packet.length < sizeof(RemoteCursorHeader)) return false;
    RemoteCursorHeader header {}; memcpy(&header, packet.bytes, sizeof(header));
    const uint8_t *rgba = (const uint8_t *)packet.bytes + sizeof(header);
    NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nil
        pixelsWide:header.width pixelsHigh:header.height bitsPerSample:8 samplesPerPixel:4
        hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
        bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:header.width * 4 bitsPerPixel:32];
    if (!bitmap) return false;
    memcpy(bitmap.bitmapData, rgba, header.rgba_bytes);
    NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [packet writeToFile:[directory stringByAppendingPathComponent:[name stringByAppendingString:@".bin"]] atomically:YES] &&
        [png writeToFile:[directory stringByAppendingPathComponent:[name stringByAppendingString:@".png"]] atomically:YES];
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) { fprintf(stderr, "Usage: CursorPattern OUTPUT_DIRECTORY\n"); return 2; }
        [NSApplication sharedApplication];
        NSString *directory = [NSString stringWithUTF8String:argv[1]];
        [[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil];
        NSMutableArray *checks = [NSMutableArray array];
        auto check = [&](NSString *name, bool passed) { [checks addObject:@{@"name":name, @"passed":@(passed)}]; };
        const size_t width = 7, height = 5;
        std::vector<uint8_t> source(width * height * 4, 0);
        // Asymmetric source data with documented top-left origin: red top row,
        // blue bottom row, green bottom-right and a translucent center pixel.
        for (size_t x = 0; x < width; ++x) {
            source[x * 4] = 255; source[x * 4 + 3] = 255;
            size_t bottom = ((height - 1) * width + x) * 4;
            source[bottom + 2] = 255; source[bottom + 3] = 255;
        }
        size_t corner = (width * height - 1) * 4;
        source[corner + 1] = 255; source[corner + 2] = 0;
        size_t center = (2 * width + 3) * 4;
        source[center] = 50; source[center + 1] = 25; source[center + 2] = 100; source[center + 3] = 128;
        CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, source.data(), source.size(), nullptr);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, space,
            static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) | static_cast<uint32_t>(kCGBitmapByteOrder32Big),
            provider, nullptr, false, kCGRenderingIntentDefault);
        NSData *fixture = RemoteCursorImagePacketFromCGImage(image, NSMakeSize(3.5, 2.5), NSMakePoint(1, 0.5));
        check(@"synthetic_packet_created", fixture != nil);
        RemoteCursorHeader fixtureHeader {};
        if (fixture) {
            memcpy(&fixtureHeader, fixture.bytes, sizeof(fixtureHeader));
            const uint8_t *pixels = (const uint8_t *)fixture.bytes + sizeof(fixtureHeader);
            bool rows = true;
            for (size_t x = 0; x < width; ++x)
                rows &= pixels[x * 4] == 255 && pixels[x * 4 + 2] == 0 && pixels[x * 4 + 3] == 255;
            for (size_t x = 0; x < width - 1; ++x) {
                size_t p = ((height - 1) * width + x) * 4;
                rows &= pixels[p] == 0 && pixels[p + 2] == 255 && pixels[p + 3] == 255;
            }
            check(@"top_red_bottom_blue_no_vertical_flip", rows);
            check(@"bottom_right_green_no_horizontal_flip", pixels[corner] == 0 && pixels[corner + 1] == 255 && pixels[corner + 2] == 0);
            check(@"point_to_pixel_hotspot_2x", fixtureHeader.hotspot_x == 2 && fixtureHeader.hotspot_y == 1);
            check(@"straight_alpha_not_premultiplied", std::abs((int)pixels[center] - 100) <= 1 &&
                std::abs((int)pixels[center + 1] - 50) <= 1 && std::abs((int)pixels[center + 2] - 200) <= 1 && pixels[center + 3] == 128);
            check(@"synthetic_exports_saved", SavePacket(fixture, directory, @"cursor-asymmetric"));
        }
        CGImageRelease(image); CGColorSpaceRelease(space); CGDataProviderRelease(provider);
        NSCursor *arrow = NSCursor.arrowCursor;
        NSData *arrowPacket = RemoteCursorImagePacket(arrow);
        check(@"real_mac_arrow_packet_created", arrowPacket != nil);
        RemoteCursorHeader arrowHeader {};
        if (arrowPacket) {
            memcpy(&arrowHeader, arrowPacket.bytes, sizeof(arrowHeader));
            CGRect proposed = CGRectMake(0, 0, arrow.image.size.width, arrow.image.size.height);
            CGImageRef arrowImage = [arrow.image CGImageForProposedRect:&proposed context:nil hints:nil];
            // Independent AppKit decoder, without a CGContext transform, is the
            // reference for row orientation and alpha coverage of the real arrow.
            NSBitmapImageRep *reference = [[NSBitmapImageRep alloc] initWithCGImage:arrowImage];
            const uint8_t *pixels = (const uint8_t *)arrowPacket.bytes + sizeof(arrowHeader);
            NSUInteger mismatch = 0, invertedMismatch = 0;
            bool sameSize = reference.pixelsWide == arrowHeader.width && reference.pixelsHigh == arrowHeader.height;
            if (sameSize) for (uint32_t y = 0; y < arrowHeader.height; ++y) for (uint32_t x = 0; x < arrowHeader.width; ++x) {
                NSUInteger samples[5] {};
                [reference getPixel:samples atX:x y:y];
                NSUInteger alphaIndex = (reference.bitmapFormat & NSBitmapFormatAlphaFirst) ? 0 : reference.samplesPerPixel - 1;
                uint8_t alpha = (uint8_t)samples[alphaIndex];
                mismatch += pixels[(y * arrowHeader.width + x) * 4 + 3] != alpha;
                invertedMismatch += pixels[((arrowHeader.height - 1 - y) * arrowHeader.width + x) * 4 + 3] != alpha;
            }
            check(@"real_arrow_matches_independent_appkit_rows", sameSize && mismatch == 0);
            check(@"real_arrow_differs_from_vertical_mirror", invertedMismatch > 0);
            check(@"real_arrow_top_left_hotspot", arrowHeader.hotspot_x == (uint32_t)lround(arrow.hotSpot.x * arrowHeader.width / arrow.image.size.width) &&
                arrowHeader.hotspot_y == (uint32_t)lround(arrow.hotSpot.y * arrowHeader.height / arrow.image.size.height));
            check(@"real_arrow_exports_saved", SavePacket(arrowPacket, directory, @"cursor-arrow"));
        }
        bool passed = true; for (NSDictionary *item in checks) passed &= [item[@"passed"] boolValue];
        NSDictionary *report = @{@"schema":@"remote.cursor-orientation.mac.v1", @"passed":@(passed), @"checks":checks,
            @"arrow_width":@(arrowHeader.width), @"arrow_height":@(arrowHeader.height),
            @"arrow_hotspot_x":@(arrowHeader.hotspot_x), @"arrow_hotspot_y":@(arrowHeader.hotspot_y)};
        NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
        [json writeToFile:[directory stringByAppendingPathComponent:@"cursor-mac-validation.json"] atomically:YES];
        fprintf(stdout, "Mac cursor orientation: %s (%lu checks), arrow %ux%u hotspot %u,%u\n",
            passed ? "PASS" : "FAIL", (unsigned long)checks.count, arrowHeader.width, arrowHeader.height, arrowHeader.hotspot_x, arrowHeader.hotspot_y);
        return passed ? 0 : 1;
    }
}
