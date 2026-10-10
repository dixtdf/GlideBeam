#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static NSColor *RGB(unsigned value, CGFloat alpha = 1.0) {
    return [NSColor colorWithSRGBRed:((value >> 16) & 255) / 255.0
                             green:((value >> 8) & 255) / 255.0
                              blue:(value & 255) / 255.0 alpha:alpha];
}
static void Fill(NSRect rect, unsigned color) {
    [RGB(color) setFill]; NSRectFill(rect);
}
static void Rounded(NSRect rect, CGFloat radius, unsigned color) {
    [RGB(color) setFill]; [[NSBezierPath bezierPathWithRoundedRect:rect xRadius:radius yRadius:radius] fill];
}
static void Text(NSString *value, NSPoint origin, CGFloat size, unsigned color, BOOL mono = NO, BOOL bold = NO) {
    NSFont *font = mono ? [NSFont monospacedSystemFontOfSize:size weight:bold ? NSFontWeightSemibold : NSFontWeightRegular]
                        : [NSFont systemFontOfSize:size weight:bold ? NSFontWeightSemibold : NSFontWeightRegular];
    [value drawAtPoint:origin withAttributes:@{NSFontAttributeName:font, NSForegroundColorAttributeName:RGB(color)}];
}

@interface CapturePatternView : NSView
@property(nonatomic, strong) NSImage *staticImage;
@property(nonatomic) NSTimeInterval started;
@property(nonatomic) NSTimeInterval elapsed;
@property(nonatomic) NSUInteger drawnFrames;
@property(nonatomic) NSUInteger timerTicks;
@property(nonatomic) NSTimeInterval duration;
@end

@implementation CapturePatternView
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (void)buildStaticImage {
    NSSize size = self.bounds.size;
    self.staticImage = [[NSImage alloc] initWithSize:size];
    [self.staticImage lockFocusFlipped:YES];
    CGFloat w = size.width, h = size.height;
    Fill(NSMakeRect(0, 0, w, h), 0x0b1118);
    Fill(NSMakeRect(0, 0, w, 92), 0x121c27);
    Rounded(NSMakeRect(28, 25, 38, 38), 9, 0x68e7c1);
    Text(@"R", NSMakePoint(40, 31), 23, 0x0b3028, NO, YES);
    Text(@"Remote Native", NSMakePoint(83, 23), 23, 0xebf2fa, NO, YES);
    Text(@"Capture test  /  own-window animation", NSMakePoint(84, 56), 11, 0x8b9aad);
    CGFloat margin = 28, gap = 18;
    CGFloat leftW = (w - margin * 2 - gap) * 0.57;
    CGFloat rightX = margin + leftW + gap, rightW = w - rightX - margin;
    Rounded(NSMakeRect(margin, 114, leftW, h - 240), 10, 0x131f2b);
    Text(@"01  Fine detail & code", NSMakePoint(margin + 18, 128), 12, 0x68e7c1, NO, YES);
    CGFloat gridY = 163, gridH = std::min<CGFloat>(150, h * .23);
    NSRect checker = NSMakeRect(margin + 18, gridY, leftW - 36, gridH);
    [NSGraphicsContext saveGraphicsState]; NSRectClip(checker);
    for (int row = 0; row < static_cast<int>(gridH / 8) + 1; ++row)
        for (int col = 0; col < static_cast<int>(checker.size.width / 8) + 1; ++col)
            Fill(NSMakeRect(checker.origin.x + col * 8, checker.origin.y + row * 8, 8, 8),
                 (row + col) % 2 ? 0x233444 : 0xd9e8ed);
    [NSGraphicsContext restoreGraphicsState];
    NSArray<NSString *> *lines = @[
        @"// Text should stay sharp while moving",
        @"struct Frame {",
        @"  uint64_t sequence;",
        @"  Texture surface;",
        @"  bool is_keyframe = false;",
        @"};",
        @"capture.on_frame([&](Frame frame) {",
        @"  encoder.submit(frame.surface);",
        @"  transport.send_latest(frame);",
        @"});",
        @"0123456789  Aa Bb Cc  [] {} <>"
    ];
    CGFloat textY = gridY + gridH + 19;
    for (NSUInteger i = 0; i < lines.count; ++i) {
        CGFloat y = textY + i * 19;
        if (y + 16 > h - 142) break;
        Text([NSString stringWithFormat:@"%02lu", (unsigned long)i + 1], NSMakePoint(margin + 18, y), 11, 0x516275, YES);
        Text(lines[i], NSMakePoint(margin + 46, y), 11,
             i == 0 ? 0x8395a8 : i == 7 || i == 8 ? 0x68e7c1 : 0xebf2fa, YES);
    }
    Rounded(NSMakeRect(rightX, 114, rightW, h - 240), 10, 0x131f2b);
    Text(@"02  Color & motion", NSMakePoint(rightX + 18, 128), 12, 0x68e7c1, NO, YES);
    const unsigned bars[] = {0xebf2fa, 0xf5ce65, 0x50dcd4, 0x73dc93, 0xec72c6, 0xef7f83, 0x718aee, 0x101720};
    CGFloat barsW = rightW - 36;
    for (int i = 0; i < 8; ++i) Fill(NSMakeRect(rightX + 18 + i * barsW / 8, 163, barsW / 8 + .5, 66), bars[i]);
    Text(@"Color, contrast, and chroma edges", NSMakePoint(rightX + 18, 240), 10, 0x8b9aad);
    for (int i = 0; i < 28; ++i) {
        CGFloat x = rightX + 18 + i * barsW / 28;
        Fill(NSMakeRect(x, 277, 1, 23), i % 2 ? 0xe5edf4 : 0x68e7c1);
    }
    Text(@"One-pixel lines", NSMakePoint(rightX + 18, 310), 10, 0x8b9aad);
    Rounded(NSMakeRect(margin, h - 108, w - margin * 2, 61), 9, 0x172730);
    Text(@"60 Hz target", NSMakePoint(margin + 18, h - 93), 14, 0xebf2fa, NO, YES);
    Text(@"Measured draw rate below. This fixture never sends remote input.", NSMakePoint(margin + 160, h - 90), 10, 0x8b9aad);
    [self.staticImage unlockFocus];
}
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (!self.staticImage || !NSEqualSizes(self.staticImage.size, self.bounds.size)) [self buildStaticImage];
    [self.staticImage drawInRect:self.bounds fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1
                 respectFlipped:YES hints:nil];
    CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
    CGFloat rightX = 28 + (w - 74) * .57 + 18, rightW = w - rightX - 28;
    CGFloat motionY = 346, motionH = std::max<CGFloat>(44, h - motionY - 150);
    NSRect motion = NSMakeRect(rightX + 18, motionY, rightW - 36, motionH);
    Fill(motion, 0x0b141e);
    double phase = self.elapsed * 2.4;
    CGFloat tile = std::min<CGFloat>(52, motionH * .48);
    CGFloat x = motion.origin.x + (motion.size.width - tile) * (.5 + .5 * std::sin(phase));
    CGFloat y = motion.origin.y + (motion.size.height - tile) * (.5 + .5 * std::cos(phase * 1.31));
    Rounded(NSMakeRect(x, y, tile, tile), 7, 0x68e7c1);
    Rounded(NSMakeRect(motion.origin.x + (motion.size.width - tile * .6) * (.5 + .5 * std::cos(phase * .71)),
                       motion.origin.y + (motion.size.height - tile * .6) * (.5 + .5 * std::sin(phase * .92)),
                       tile * .6, tile * .6), 5, 0x8797fa);
    // A moving scan line creates changed content every timer tick.
    CGFloat scanX = 28 + std::fmod(self.elapsed * 205, w - 56);
    Fill(NSMakeRect(scanX, 114, 2, h - 240), 0x68e7c1);
    ++_drawnFrames;
    double drawRate = self.elapsed > .1 ? self.drawnFrames / self.elapsed : 0;
    Text([NSString stringWithFormat:@"%05lu draws   %.1f draws/s   %.1f / %.0f s",
          (unsigned long)self.drawnFrames, drawRate, self.elapsed, self.duration],
         NSMakePoint(28, h - 30), 11, 0x8b9aad, YES);
    CGFloat progress = std::clamp<CGFloat>(self.elapsed / self.duration, 0, 1);
    Fill(NSMakeRect(w - 210, h - 24, 182, 3), 0x24313f);
    Fill(NSMakeRect(w - 210, h - 24, 182 * progress, 3), 0x68e7c1);
}
@end

@interface CapturePatternDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) CapturePatternView *pattern;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic) NSTimeInterval duration;
@property(nonatomic) BOOL completed;
@property(nonatomic) BOOL reported;
- (void)emitReport;
@end

@implementation CapturePatternDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    NSRect available = NSScreen.mainScreen.visibleFrame;
    CGFloat width = std::min<CGFloat>(1000, available.size.width - 60);
    CGFloat height = std::min<CGFloat>(700, available.size.height - 70);
    width = std::max<CGFloat>(640, width); height = std::max<CGFloat>(520, height);
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Remote Native \u2022 Capture test";
    self.window.delegate = self; self.window.releasedWhenClosed = NO;
    self.window.backgroundColor = RGB(0x0b1118);
    self.pattern = [[CapturePatternView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    self.pattern.duration = self.duration;
    self.pattern.started = NSProcessInfo.processInfo.systemUptime;
    self.window.contentView = self.pattern;
    [self.window center]; [self.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    __weak CapturePatternDelegate *weakSelf = self;
    self.timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        CapturePatternDelegate *owner = weakSelf;
        if (!owner) return;
        owner.pattern.elapsed = NSProcessInfo.processInfo.systemUptime - owner.pattern.started;
        ++owner.pattern.timerTicks;
        [owner.pattern setNeedsDisplay:YES];
        if (owner.pattern.elapsed >= owner.duration) {
            owner.completed = YES; [owner.timer invalidate]; [owner.window orderOut:nil];
            [owner emitReport]; [NSApp terminate:nil];
        }
    }];
    self.timer.tolerance = .001;
    [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
}
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    [self.timer invalidate]; [self emitReport]; [NSApp terminate:nil];
}
- (void)emitReport {
    if (self.reported) return;
    self.reported = YES;
    NSTimeInterval elapsed = self.pattern ? NSProcessInfo.processInfo.systemUptime - self.pattern.started : 0;
    NSDictionary *report = @{
        @"schema":@"remote.capture-pattern.v1", @"completed":@(self.completed),
        @"target_timer_hz":@60, @"requested_seconds":@(self.duration), @"elapsed_seconds":@(elapsed),
        @"timer_ticks":@(self.pattern.timerTicks), @"drawn_frames":@(self.pattern.drawnFrames),
        @"measured_draws_per_second":@(elapsed > 0 ? self.pattern.drawnFrames / elapsed : 0),
        @"window_width":@(self.pattern.bounds.size.width), @"window_height":@(self.pattern.bounds.size.height),
        @"measurement":@"Own-window draw callback count; not encoded or received FPS",
        @"captures_other_apps":@NO, @"injects_input":@NO
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
    if (json) { fwrite(json.bytes, 1, json.length, stdout); fputc('\n', stdout); fflush(stdout); }
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSTimeInterval duration = 30;
        if (argc == 3 && strcmp(argv[1], "--seconds") == 0) {
            char *end = nullptr; double requested = strtod(argv[2], &end);
            if (!end || *end || !std::isfinite(requested) || requested < 1 || requested > 120) return 2;
            duration = requested;
        } else if (argc != 1) return 2;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        CapturePatternDelegate *delegate = [CapturePatternDelegate new];
        delegate.duration = duration; NSApp.delegate = delegate;
        [NSApp run]; [delegate emitReport];
        return delegate.completed ? 0 : 1;
    }
}
