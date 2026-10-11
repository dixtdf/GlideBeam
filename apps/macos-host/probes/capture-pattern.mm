#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../../shared/latency-stats.h"

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
static NSDictionary *TimingReport(const LatencySnapshot& timing) {
    return @{@"samples":@(timing.totalSamples), @"window_samples":@(timing.windowSamples),
             @"last_ms":@(timing.lastUs / 1000.0), @"mean_ms":@(timing.meanUs / 1000.0),
             @"p95_ms":@(timing.p95Us / 1000.0), @"p99_ms":@(timing.p99Us / 1000.0),
             @"max_ms":@(timing.maxUs / 1000.0)};
}

@interface CapturePatternView : NSView {
@public
    RollingLatencyStats _drawIntervals;
    NSTimeInterval _lastDrawAt;
    RollingLatencyStats _layerIntervals;
    NSTimeInterval _lastLayerUpdateAt;
    RollingLatencyStats _displayCallbackIntervals, _displayTimestampIntervals, _displayTargetIntervals;
    RollingLatencyStats _presentationChangeIntervals;
    NSTimeInterval _firstDisplayCallbackAt, _lastDisplayCallbackAt, _lastDisplayTimestamp;
    NSTimeInterval _lastPresentationChangeAt;
    NSUInteger _displayCallbacks, _invalidDisplayTimestamps;
    NSUInteger _presentationSamples, _presentationChanges, _presentationSame, _presentationMissing;
    CGPoint _lastPresentationPosition;
    BOOL _hasPresentationPosition;
}
@property(nonatomic, strong) NSImage *staticImage;
@property(nonatomic) NSTimeInterval started;
@property(nonatomic) NSTimeInterval elapsed;
@property(nonatomic) NSUInteger drawnFrames;
@property(nonatomic) NSUInteger timerTicks;
@property(nonatomic) NSUInteger layerUpdates;
@property(nonatomic, strong) CALayer *movingLayer;
@property(nonatomic) NSTimeInterval duration;
@property(nonatomic, strong) NSString *pacingDescription;
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
    Text(self.pacingDescription ?: @"Measured draw rate below. This fixture never sends remote input.", NSMakePoint(margin + 160, h - 90), 10, 0x8b9aad);
    [self.staticImage unlockFocus];
}
- (void)drawRect:(NSRect)dirtyRect {
    NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    if (_lastDrawAt > 0 && now >= _lastDrawAt)
        _drawIntervals.ObserveUs((uint64_t)llround((now - _lastDrawAt) * 1000000.0));
    _lastDrawAt = now;
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

// A borderless fixture can become its own key window without invoking macOS
// Spaces fullscreen or changing any display mode.
@interface CapturePatternFullscreenWindow : NSWindow
@end
@implementation CapturePatternFullscreenWindow
- (BOOL)canBecomeKeyWindow { return YES; }
@end

@interface CapturePatternDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) CapturePatternView *pattern;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSTimer *completionTimer;
@property(nonatomic, strong) CADisplayLink *displayLink;
@property(nonatomic, strong) dispatch_source_t deadlineTimer;
@property(nonatomic) BOOL deadlineMode;
@property(nonatomic) BOOL layerMode;
@property(nonatomic) BOOL displayMode;
@property(nonatomic) BOOL animationMode;
@property(nonatomic) BOOL fullscreen;
@property(nonatomic) BOOL pacingUnavailable;
@property(nonatomic) NSUInteger installedAnimations;
@property(nonatomic) NSTimeInterval duration;
@property(nonatomic) BOOL completed;
@property(nonatomic) BOOL reported;
- (void)emitReport;
- (void)advancePattern;
- (void)displayTick:(CADisplayLink *)link;
- (void)stopPacing;
- (void)completePattern;
@end

@implementation CapturePatternDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    NSScreen *screen = NSScreen.mainScreen;
    NSRect available = screen.visibleFrame;
    CGFloat width = std::min<CGFloat>(1000, available.size.width - 60);
    CGFloat height = std::min<CGFloat>(700, available.size.height - 70);
    width = std::max<CGFloat>(640, width); height = std::max<CGFloat>(520, height);
    NSRect contentRect = NSMakeRect(0, 0, width, height);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    Class windowClass = NSWindow.class;
    if (self.fullscreen) {
        contentRect = screen.frame;
        width = contentRect.size.width; height = contentRect.size.height;
        style = NSWindowStyleMaskBorderless;
        windowClass = CapturePatternFullscreenWindow.class;
    }
    self.window = [[windowClass alloc] initWithContentRect:contentRect styleMask:style
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Remote Native \u2022 Capture test";
    self.window.delegate = self; self.window.releasedWhenClosed = NO;
    self.window.backgroundColor = RGB(0x0b1118);
    self.pattern = [[CapturePatternView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    self.pattern.duration = self.duration;
    self.pattern.started = NSProcessInfo.processInfo.systemUptime;
    self.window.contentView = self.pattern;
    if (self.layerMode || self.displayMode) {
        self.pattern.wantsLayer = YES;
        self.pattern.movingLayer = [CALayer layer];
        self.pattern.movingLayer.bounds = CGRectMake(0, 0, 240, 160);
        self.pattern.movingLayer.backgroundColor = RGB(0xffbd4a).CGColor;
        self.pattern.movingLayer.cornerRadius = 20;
        [self.pattern.layer addSublayer:self.pattern.movingLayer];
    }
    if (self.displayMode)
        self.pattern.pacingDescription = self.animationMode ? @"Autonomous layer animation; display samples are not scanned FPS."
                                                          : @"Display-linked layer updates; callbacks are not scanned FPS.";
    if (self.fullscreen) {
        // Cover the menu bar/Dock using only this temporary owned window; no
        // other application's visibility, placement or settings are changed.
        self.window.level = NSMainMenuWindowLevel + 1;
        [self.window setFrame:screen.frame display:YES];
    } else [self.window center];
    [self.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    __weak CapturePatternDelegate *weakSelf = self;
    if (self.displayMode) {
        if (@available(macOS 14.0, *)) {
            // The autonomous mode commits the animation once; its progression
            // does not rely on 60 main-thread timer/property updates per second.
            if (self.animationMode) {
                CGPoint from = CGPointMake(145, 195);
                CGPoint to = CGPointMake(width - 145, height - 195);
                [CATransaction begin]; [CATransaction setDisableActions:YES];
                self.pattern.movingLayer.position = from;
                CABasicAnimation *motion = [CABasicAnimation animationWithKeyPath:@"position"];
                motion.fromValue = [NSValue valueWithPoint:NSPointFromCGPoint(from)];
                motion.toValue = [NSValue valueWithPoint:NSPointFromCGPoint(to)];
                motion.duration = 2.5;
                motion.autoreverses = YES;
                motion.repeatCount = HUGE_VALF;
                motion.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionLinear];
                [self.pattern.movingLayer addAnimation:motion forKey:@"fixture-motion"];
                [CATransaction commit]; self.installedAnimations = 1;
            }
            self.displayLink = [self.pattern displayLinkWithTarget:self selector:@selector(displayTick:)];
            // preferredFramesPerSecond is unavailable on macOS. Request the
            // macOS-supported range; actual callback/schedule rates remain
            // measured below because the system may choose a lower frequency.
            self.displayLink.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
            [self.displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
            // Hidden/off-display views receive no display-link callbacks. An
            // independent one-shot completion timer still bounds fixture life.
            self.completionTimer = [NSTimer timerWithTimeInterval:self.duration repeats:NO block:^(NSTimer *timer) {
                (void)timer; CapturePatternDelegate *owner = weakSelf;
                if (owner) [owner completePattern];
            }];
            [NSRunLoop.mainRunLoop addTimer:self.completionTimer forMode:NSRunLoopCommonModes];
            return;
        }
        self.pacingUnavailable = YES;
        [self.window orderOut:nil]; [self emitReport]; [NSApp terminate:nil];
        return;
    }
    if (self.deadlineMode) {
        self.deadlineTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0,
            DISPATCH_TIMER_STRICT, dispatch_get_main_queue());
        dispatch_source_set_timer(self.deadlineTimer, dispatch_time(DISPATCH_TIME_NOW, 0),
            (uint64_t)llround(1000000000.0 / 60.0), 0);
        dispatch_source_set_event_handler(self.deadlineTimer, ^{
            CapturePatternDelegate *owner = weakSelf;
            if (owner) [owner advancePattern];
        });
        dispatch_resume(self.deadlineTimer);
        return;
    }
    self.timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
        CapturePatternDelegate *owner = weakSelf;
        if (!owner) return;
        [owner advancePattern];
    }];
    self.timer.tolerance = .001;
    [NSRunLoop.mainRunLoop addTimer:self.timer forMode:NSRunLoopCommonModes];
}
- (void)advancePattern {
    self.pattern.elapsed = NSProcessInfo.processInfo.systemUptime - self.pattern.started;
    if (self.pattern.elapsed >= self.duration) {
        [self completePattern];
        return;
    }
    ++self.pattern.timerTicks;
    if (self.layerMode) {
        NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
        if (self.pattern->_lastLayerUpdateAt > 0)
            self.pattern->_layerIntervals.ObserveUs((uint64_t)llround((now - self.pattern->_lastLayerUpdateAt) * 1000000.0));
        self.pattern->_lastLayerUpdateAt = now;
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        CGFloat x = self.pattern.bounds.size.width * .5 + std::sin(self.pattern.elapsed * 2.0) * 300;
        CGFloat y = self.pattern.bounds.size.height * .5 + std::cos(self.pattern.elapsed * 1.7) * 170;
        self.pattern.movingLayer.position = CGPointMake(x, y);
        [CATransaction commit]; ++self.pattern.layerUpdates;
        return;
    }
    [self.pattern setNeedsDisplay:YES];
    // The deadline fixture draws on each timer delivery, instead of allowing
    // AppKit's later invalidation pass to coalesce two benchmark updates.
    if (self.deadlineMode) [self.pattern displayIfNeeded];
}
- (void)displayTick:(CADisplayLink *)link {
    NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    CapturePatternView *pattern = self.pattern;
    pattern.elapsed = now - pattern.started;
    if (pattern.elapsed >= self.duration) { [self completePattern]; return; }
    if (pattern->_lastDisplayCallbackAt > 0 && now >= pattern->_lastDisplayCallbackAt)
        pattern->_displayCallbackIntervals.ObserveUs((uint64_t)llround((now - pattern->_lastDisplayCallbackAt) * 1000000.0));
    if (!pattern->_firstDisplayCallbackAt) pattern->_firstDisplayCallbackAt = now;
    pattern->_lastDisplayCallbackAt = now; ++pattern->_displayCallbacks;
    // These timestamps describe the display-link schedule, not proof that this
    // fixture's pixels were composited, captured, received or optically scanned.
    if (std::isfinite(link.timestamp) && std::isfinite(link.targetTimestamp) && link.timestamp > 0 && link.targetTimestamp > link.timestamp) {
        if (pattern->_lastDisplayTimestamp > 0 && link.timestamp > pattern->_lastDisplayTimestamp)
            pattern->_displayTimestampIntervals.ObserveUs((uint64_t)llround((link.timestamp - pattern->_lastDisplayTimestamp) * 1000000.0));
        else if (pattern->_lastDisplayTimestamp > 0) ++pattern->_invalidDisplayTimestamps;
        pattern->_lastDisplayTimestamp = link.timestamp;
        pattern->_displayTargetIntervals.ObserveUs((uint64_t)llround((link.targetTimestamp - link.timestamp) * 1000000.0));
    } else ++pattern->_invalidDisplayTimestamps;
    // A presentation layer is Apple's close approximation of onscreen layer
    // state. Sampling changes is stronger than counting model updates, but is
    // deliberately named "samples", never "presented frames".
    CALayer *presentation = pattern.movingLayer.presentationLayer;
    if (presentation) {
        ++pattern->_presentationSamples;
        CGPoint position = presentation.position;
        if (pattern->_hasPresentationPosition && !CGPointEqualToPoint(position, pattern->_lastPresentationPosition)) {
            ++pattern->_presentationChanges;
            if (pattern->_lastPresentationChangeAt > 0)
                pattern->_presentationChangeIntervals.ObserveUs((uint64_t)llround((now - pattern->_lastPresentationChangeAt) * 1000000.0));
            pattern->_lastPresentationChangeAt = now;
        } else if (pattern->_hasPresentationPosition) ++pattern->_presentationSame;
        pattern->_lastPresentationPosition = position; pattern->_hasPresentationPosition = YES;
    } else ++pattern->_presentationMissing;
    if (!self.animationMode) {
        if (pattern->_lastLayerUpdateAt > 0)
            pattern->_layerIntervals.ObserveUs((uint64_t)llround((now - pattern->_lastLayerUpdateAt) * 1000000.0));
        pattern->_lastLayerUpdateAt = now;
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        CGFloat x = pattern.bounds.size.width * .5 + std::sin(pattern.elapsed * 2.0) * 300;
        CGFloat y = pattern.bounds.size.height * .5 + std::cos(pattern.elapsed * 1.7) * 170;
        pattern.movingLayer.position = CGPointMake(x, y);
        [CATransaction commit]; ++pattern.layerUpdates;
    }
}
- (void)stopPacing {
    [self.timer invalidate]; self.timer = nil;
    [self.completionTimer invalidate]; self.completionTimer = nil;
    [self.displayLink invalidate]; self.displayLink = nil;
    if (self.deadlineTimer) { dispatch_source_cancel(self.deadlineTimer); self.deadlineTimer = nil; }
    [self.pattern.movingLayer removeAnimationForKey:@"fixture-motion"];
}
- (void)completePattern {
    if (self.reported) return;
    self.completed = YES; [self stopPacing];
    [self.window orderOut:nil]; [self emitReport]; [NSApp terminate:nil];
}
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    [self stopPacing];
    [self emitReport]; [NSApp terminate:nil];
}
- (void)emitReport {
    if (self.reported) return;
    self.reported = YES;
    NSTimeInterval elapsed = self.pattern ? NSProcessInfo.processInfo.systemUptime - self.pattern.started : 0;
    auto intervals = self.pattern ? self.pattern->_drawIntervals.Snapshot() : LatencySnapshot{};
    auto updates = self.pattern ? self.pattern->_layerIntervals.Snapshot() : LatencySnapshot{};
    auto callbacks = self.pattern ? self.pattern->_displayCallbackIntervals.Snapshot() : LatencySnapshot{};
    auto timestamps = self.pattern ? self.pattern->_displayTimestampIntervals.Snapshot() : LatencySnapshot{};
    auto targets = self.pattern ? self.pattern->_displayTargetIntervals.Snapshot() : LatencySnapshot{};
    auto changes = self.pattern ? self.pattern->_presentationChangeIntervals.Snapshot() : LatencySnapshot{};
    NSTimeInterval callbackSpan = self.pattern ? self.pattern->_lastDisplayCallbackAt - self.pattern->_firstDisplayCallbackAt : 0;
    NSDictionary *report = @{
        @"schema":@"remote.capture-pattern.v1", @"completed":@(self.completed),
        @"target_timer_hz":@60, @"requested_seconds":@(self.duration), @"elapsed_seconds":@(elapsed),
        @"timer_ticks":@(self.pattern.timerTicks), @"drawn_frames":@(self.pattern.drawnFrames),
        @"measured_draws_per_second":@(elapsed > 0 ? self.pattern.drawnFrames / elapsed : 0),
        @"pacing":self.displayMode ? (self.animationMode ? @"animation" : @"display") : self.layerMode ? @"layer" : self.deadlineMode ? @"deadline" : @"runloop",
        @"pacing_unavailable":@(self.pacingUnavailable),
        @"layer_updates":@(self.pattern.layerUpdates),
        @"measured_layer_updates_per_second":@(elapsed > 0 ? self.pattern.layerUpdates / elapsed : 0),
        @"layer_update_interval_mean_ms":@(updates.meanUs / 1000.0),
        @"layer_update_interval_p95_ms":@(updates.p95Us / 1000.0),
        @"layer_update_interval_p99_ms":@(updates.p99Us / 1000.0),
        @"layer_update_interval_max_ms":@(updates.maxUs / 1000.0),
        @"draw_interval_mean_ms":@(intervals.meanUs / 1000.0),
        @"draw_interval_p95_ms":@(intervals.p95Us / 1000.0),
        @"draw_interval_p99_ms":@(intervals.p99Us / 1000.0),
        @"draw_interval_max_ms":@(intervals.maxUs / 1000.0),
        @"display_link_callbacks":@(self.pattern->_displayCallbacks),
        @"display_link_requested_min_hz":@(self.displayMode ? 60 : 0),
        @"display_link_requested_max_hz":@(self.displayMode ? 60 : 0),
        @"display_link_requested_preferred_hz":@(self.displayMode ? 60 : 0),
        @"display_callback_span_seconds":@(callbackSpan),
        @"measured_display_callbacks_per_second":@(callbackSpan > 0 ? (self.pattern->_displayCallbacks - 1) / callbackSpan : 0),
        @"display_schedule_hz":@(targets.meanUs > 0 ? 1000000.0 / targets.meanUs : 0),
        @"display_invalid_timestamp_samples":@(self.pattern->_invalidDisplayTimestamps),
        @"display_callback_interval":TimingReport(callbacks),
        @"display_timestamp_interval":TimingReport(timestamps),
        @"display_scheduled_frame_interval":TimingReport(targets),
        @"presentation_samples":@(self.pattern->_presentationSamples),
        @"presentation_changed_samples":@(self.pattern->_presentationChanges),
        @"presentation_unchanged_samples":@(self.pattern->_presentationSame),
        @"presentation_missing_samples":@(self.pattern->_presentationMissing),
        @"measured_presentation_changes_per_second":@(elapsed > 0 ? self.pattern->_presentationChanges / elapsed : 0),
        @"presentation_change_interval":TimingReport(changes),
        @"autonomous_animations_installed":@(self.installedAnimations),
        @"window_width":@(self.pattern.bounds.size.width), @"window_height":@(self.pattern.bounds.size.height),
        @"fullscreen":@(self.fullscreen), @"window_dimensions_unit":@"AppKit points",
        @"measurement":@"Own-window draw callbacks, requested layer updates, display-link schedule and sampled presentation-layer position changes. No counter proves distinct composited images, captured/encoded/received frames or optical scanout FPS. Interval percentiles cover up to the latest 256 samples.",
        @"captures_other_apps":@NO, @"injects_input":@NO
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
    if (json) { fwrite(json.bytes, 1, json.length, stdout); fputc('\n', stdout); fflush(stdout); }
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSTimeInterval duration = 30;
        BOOL deadlineMode = NO;
        BOOL layerMode = NO;
        BOOL displayMode = NO;
        BOOL animationMode = NO;
        BOOL fullscreen = NO;
        for (int arg = 1; arg < argc; ++arg) {
            if (strcmp(argv[arg], "--fullscreen") == 0) {
                fullscreen = YES;
            } else if (strcmp(argv[arg], "--seconds") == 0 && arg + 1 < argc) {
                char *end = nullptr; double requested = strtod(argv[++arg], &end);
                if (!end || *end || !std::isfinite(requested) || requested < 1 || requested > 120) return 2;
                duration = requested;
            } else if (strcmp(argv[arg], "--pacing") == 0 && arg + 1 < argc) {
                const char* pacing = argv[++arg];
                displayMode = NO; animationMode = NO;
                if (strcmp(pacing, "display") == 0) { deadlineMode = NO; layerMode = NO; displayMode = YES; }
                else if (strcmp(pacing, "animation") == 0) { deadlineMode = NO; layerMode = NO; displayMode = YES; animationMode = YES; }
                else if (strcmp(pacing, "layer") == 0) { deadlineMode = YES; layerMode = YES; }
                else if (strcmp(pacing, "deadline") == 0) { deadlineMode = YES; layerMode = NO; }
                else if (strcmp(pacing, "runloop") == 0) { deadlineMode = NO; layerMode = NO; }
                else return 2;
            } else return 2;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        CapturePatternDelegate *delegate = [CapturePatternDelegate new];
        delegate.duration = duration; delegate.deadlineMode = deadlineMode; delegate.layerMode = layerMode;
        delegate.displayMode = displayMode; delegate.animationMode = animationMode; NSApp.delegate = delegate;
        delegate.fullscreen = fullscreen;
        [NSApp run]; [delegate emitReport];
        return delegate.completed ? 0 : 1;
    }
}
