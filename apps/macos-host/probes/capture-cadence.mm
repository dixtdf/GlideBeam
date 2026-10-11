// Metadata-only ScreenCaptureKit cadence diagnostic. No encoding, input,
// pixel locks/copies, retained image surfaces, audio, or screen-content output.
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "../../../shared/latency-stats.h"

namespace {
uint64_t NowUs() {
    static const mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t value{}; mach_timebase_info(&value); return value;
    }();
    return static_cast<uint64_t>(static_cast<long double>(mach_absolute_time()) * base.numer / base.denom / 1000.0L);
}
struct Options {
    unsigned seconds = 20, fps = 60, queueDepth = 3, maxHeight = 0;
    OSType pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
};
bool ParseUnsigned(const char* text, unsigned& value) {
    const char* end = text + std::strlen(text);
    auto result = std::from_chars(text, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}
void Usage(FILE* output) {
    std::fprintf(output, "Usage: capture-cadence [--seconds 1..60] [--fps 0|30|60|90|120] "
                         "[--queue-depth 3|5|8] [--pixel-format nv12|bgra] [--max-height 0|360|720|1080]\n"
                         "FPS 0 requests kCMTimeZero; max-height 0 keeps native capture dimensions.\n");
}
bool ParseOptions(int argc, const char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        if (i + 1 >= argc) return false;
        const char* name = argv[i++]; const char* value = argv[i];
        if (!std::strcmp(name, "--seconds")) {
            if (!ParseUnsigned(value, options.seconds) || !options.seconds || options.seconds > 60) return false;
        } else if (!std::strcmp(name, "--fps")) {
            if (!ParseUnsigned(value, options.fps) ||
                (options.fps != 0 && options.fps != 30 && options.fps != 60 && options.fps != 90 && options.fps != 120)) return false;
        } else if (!std::strcmp(name, "--queue-depth")) {
            if (!ParseUnsigned(value, options.queueDepth) ||
                (options.queueDepth != 3 && options.queueDepth != 5 && options.queueDepth != 8)) return false;
        } else if (!std::strcmp(name, "--max-height")) {
            if (!ParseUnsigned(value, options.maxHeight) ||
                (options.maxHeight != 0 && options.maxHeight != 360 && options.maxHeight != 720 && options.maxHeight != 1080)) return false;
        } else if (!std::strcmp(name, "--pixel-format")) {
            if (!std::strcmp(value, "nv12")) options.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
            else if (!std::strcmp(value, "bgra")) options.pixelFormat = kCVPixelFormatType_32BGRA;
            else return false;
        } else return false;
    }
    return true;
}
NSDictionary* Timing(const RollingLatencyStats& stats) {
    const auto value = stats.Snapshot();
    return @{ @"samples":@(value.totalSamples), @"window_samples":@(value.windowSamples),
              @"last_ms":@(value.lastUs / 1000.0), @"mean_ms":@(value.meanUs / 1000.0),
              @"p95_ms":@(value.p95Us / 1000.0), @"p99_ms":@(value.p99Us / 1000.0),
              @"max_ms":@(value.maxUs / 1000.0) };
}
struct Counters {
    uint64_t callbacks = 0, invalidSamples = 0, complete = 0, idle = 0;
    uint64_t started = 0, blank = 0, suspended = 0, stopped = 0, unknown = 0;
    uint64_t missingStatus = 0, missingImage = 0, invalidPts = 0, ptsRegressions = 0;
    uint64_t duplicatePts = 0, imageChanges = 0, firstCallbackUs = 0, lastCallbackUs = 0;
    uint64_t firstCompleteUs = 0, lastCompleteUs = 0, lastPtsUs = 0, lastCompletePtsUs = 0;
    bool havePts = false, haveCompletePts = false;
    size_t imageWidth = 0, imageHeight = 0;
    OSType pixelFormat = 0;
};
}

@interface CadenceProbe : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    Options _options;
    std::atomic<bool> _accepting;
    std::mutex _statsLock;
    Counters _counts;
    RollingLatencyStats _callbackIntervals, _completeCallbackIntervals, _ptsIntervals, _completePtsIntervals;
    SCStream* _stream;
    dispatch_queue_t _captureQueue;
    NSDictionary* _displayMetadata;
    size_t _configuredWidth, _configuredHeight;
    uint64_t _startedUs, _stopRequestedUs;
    BOOL _stopping, _finished, _startSucceeded, _stopSucceeded;
    NSInteger _errorCode;
    NSString* _reason;
}
- (void)begin;
- (void)requestStop:(NSString*)reason code:(NSInteger)code;
- (void)finish;
@end

@implementation CadenceProbe
- (void)begin {
    _accepting.store(false);
    _captureQueue = dispatch_queue_create("dev.remote-native.capture-cadence", DISPATCH_QUEUE_SERIAL);
    _displayMetadata = @{};
    // Do not request permission or generate UI. Deployment can use an already
    // approved development identity; missing permission is a bounded failure.
    if (!CGPreflightScreenCaptureAccess()) {
        _reason = @"screen-recording-permission-missing"; _errorCode = 1; [self finish]; return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (!self->_finished && !self->_startSucceeded) [self requestStop:@"startup-timeout" code:2];
    });
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO
        completionHandler:^(SCShareableContent* content, NSError* error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self->_finished || self->_stopping) return;
            if (error || !content) { [self requestStop:@"shareable-content-failed" code:error ? error.code : 3]; return; }
            SCDisplay* display = nil;
            const CGDirectDisplayID mainDisplay = CGMainDisplayID();
            for (SCDisplay* candidate in content.displays) {
                if (candidate.displayID == mainDisplay) { display = candidate; break; }
            }
            if (!display) { [self requestStop:@"main-display-unavailable" code:4]; return; }
            CGDisplayModeRef mode = CGDisplayCopyDisplayMode(mainDisplay);
            const double refresh = mode ? CGDisplayModeGetRefreshRate(mode) : 0;
            self->_displayMetadata = @{ @"logical_width":@(display.width), @"logical_height":@(display.height),
                @"mode_width":@(mode ? CGDisplayModeGetWidth(mode) : 0),
                @"mode_height":@(mode ? CGDisplayModeGetHeight(mode) : 0),
                @"mode_pixel_width":@(mode ? CGDisplayModeGetPixelWidth(mode) : 0),
                @"mode_pixel_height":@(mode ? CGDisplayModeGetPixelHeight(mode) : 0),
                @"mode_refresh_hz":@(std::isfinite(refresh) && refresh > 0 ? refresh : 0),
                @"mode_refresh_available":@(std::isfinite(refresh) && refresh > 0) };
            if (mode) CGDisplayModeRelease(mode);
            // Scale only the capture output. The display mode, desktop size and
            // user input coordinates remain untouched. Preserve source aspect,
            // round both dimensions down to even pixels, and never upscale.
            const size_t sourceWidth = display.width, sourceHeight = display.height;
            size_t captureWidth = sourceWidth & ~size_t{1}, captureHeight = sourceHeight & ~size_t{1};
            if (self->_options.maxHeight && captureHeight > self->_options.maxHeight) {
                captureHeight = self->_options.maxHeight;
                captureWidth = static_cast<size_t>(static_cast<long double>(sourceWidth) * captureHeight / sourceHeight) & ~size_t{1};
            }
            if (captureWidth < 2 || captureHeight < 2) { [self requestStop:@"invalid-capture-dimensions" code:10]; return; }
            self->_configuredWidth = captureWidth; self->_configuredHeight = captureHeight;
            SCStreamConfiguration* configuration = [SCStreamConfiguration new];
            configuration.width = captureWidth; configuration.height = captureHeight;
            configuration.pixelFormat = self->_options.pixelFormat;
            configuration.queueDepth = self->_options.queueDepth;
            configuration.minimumFrameInterval = self->_options.fps ? CMTimeMake(1, self->_options.fps) : kCMTimeZero;
            configuration.showsCursor = NO; configuration.capturesAudio = NO;
            // Full main-display capture matches the host's official SCK filter.
            // Only public frame metadata leaves the callback; pixels are discarded.
            SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
            self->_stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:self];
            NSError* attachError = nil;
            if (![self->_stream addStreamOutput:self type:SCStreamOutputTypeScreen
                            sampleHandlerQueue:self->_captureQueue error:&attachError]) {
                [self requestStop:@"capture-attachment-failed" code:attachError ? attachError.code : 5]; return;
            }
            [self->_stream startCaptureWithCompletionHandler:^(NSError* startError) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (self->_finished || self->_stopping) return;
                    if (startError) { [self requestStop:@"capture-start-failed" code:startError.code]; return; }
                    self->_startSucceeded = YES; self->_startedUs = NowUs(); self->_accepting.store(true);
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(self->_options.seconds) * NSEC_PER_SEC),
                                   dispatch_get_main_queue(), ^{
                        if (!self->_finished && !self->_stopping) [self requestStop:@"completed" code:0];
                    });
                });
            }];
        });
    }];
}
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || !_accepting.load()) return;
    (void)stream;
    const uint64_t now = NowUs();
    std::lock_guard lock(_statsLock);
    if (!_accepting.load()) return;
    ++_counts.callbacks;
    if (_counts.lastCallbackUs) _callbackIntervals.ObserveUs(now - _counts.lastCallbackUs);
    else _counts.firstCallbackUs = now;
    _counts.lastCallbackUs = now;
    if (!CMSampleBufferIsValid(sample)) { ++_counts.invalidSamples; return; }
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    NSNumber* frameStatus = nil;
    if (attachments && CFArrayGetCount(attachments)) {
        NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
        id value = info[SCStreamFrameInfoStatus];
        if ([value isKindOfClass:NSNumber.class]) frameStatus = value;
    }
    if (!frameStatus) ++_counts.missingStatus;
    const NSInteger status = frameStatus ? frameStatus.integerValue : -1;
    switch (status) {
        case SCFrameStatusComplete: ++_counts.complete; break;
        case SCFrameStatusIdle: ++_counts.idle; break;
        case SCFrameStatusStarted: ++_counts.started; break;
        case SCFrameStatusBlank: ++_counts.blank; break;
        case SCFrameStatusSuspended: ++_counts.suspended; break;
        case SCFrameStatusStopped: ++_counts.stopped; break;
        default: ++_counts.unknown; break;
    }
    const bool complete = status == SCFrameStatusComplete;
    if (complete) {
        if (_counts.lastCompleteUs) _completeCallbackIntervals.ObserveUs(now - _counts.lastCompleteUs);
        else _counts.firstCompleteUs = now;
        _counts.lastCompleteUs = now;
    }
    const CMTime presentation = CMSampleBufferGetPresentationTimeStamp(sample);
    const double seconds = CMTimeGetSeconds(presentation);
    if (CMTIME_IS_NUMERIC(presentation) && std::isfinite(seconds) && seconds >= 0 && seconds < 1e12) {
        const auto pts = static_cast<uint64_t>(std::llround(seconds * 1000000.0));
        if (_counts.havePts) {
            if (pts >= _counts.lastPtsUs) _ptsIntervals.ObserveUs(pts - _counts.lastPtsUs);
            else ++_counts.ptsRegressions;
            if (pts == _counts.lastPtsUs) ++_counts.duplicatePts;
        }
        _counts.lastPtsUs = pts; _counts.havePts = true;
        if (complete) {
            if (_counts.haveCompletePts && pts >= _counts.lastCompletePtsUs)
                _completePtsIntervals.ObserveUs(pts - _counts.lastCompletePtsUs);
            _counts.lastCompletePtsUs = pts; _counts.haveCompletePts = true;
        }
    } else ++_counts.invalidPts;
    CVPixelBufferRef image = CMSampleBufferGetImageBuffer(sample);
    if (image) {
        const size_t width = CVPixelBufferGetWidth(image), height = CVPixelBufferGetHeight(image);
        const OSType format = CVPixelBufferGetPixelFormatType(image);
        if (_counts.imageWidth && (width != _counts.imageWidth || height != _counts.imageHeight || format != _counts.pixelFormat))
            ++_counts.imageChanges;
        _counts.imageWidth = width; _counts.imageHeight = height; _counts.pixelFormat = format;
    } else if (complete) ++_counts.missingImage;
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    (void)stream;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self->_finished && !self->_stopping) [self requestStop:@"capture-stopped-with-error" code:error ? error.code : 6];
    });
}
- (void)requestStop:(NSString*)reason code:(NSInteger)code {
    if (_finished || _stopping) return;
    _reason = reason; _errorCode = code; _stopping = YES;
    _stopRequestedUs = NowUs(); _accepting.store(false);
    if (!_stream) { [self finish]; return; }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (!self->_finished) {
            self->_reason = @"stop-timeout";
            if (!self->_errorCode) self->_errorCode = 7;
            [self finish];
        }
    });
    [_stream stopCaptureWithCompletionHandler:^(NSError* error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self->_finished) return;
            self->_stopSucceeded = !error;
            if (error && !self->_errorCode) { self->_reason = @"capture-stop-failed"; self->_errorCode = error.code; }
            [self finish];
        });
    }];
}
- (void)finish {
    if (_finished) return;
    _accepting.store(false);
    Counters counts;
    { std::lock_guard lock(_statsLock); counts = _counts; }
    const double captureSeconds = _startedUs && _stopRequestedUs >= _startedUs ? (_stopRequestedUs - _startedUs) / 1000000.0 : 0;
    const double callbackSpan = counts.callbacks > 1 ? (counts.lastCallbackUs - counts.firstCallbackUs) / 1000000.0 : 0;
    const double completeSpan = counts.complete > 1 ? (counts.lastCompleteUs - counts.firstCompleteUs) / 1000000.0 : 0;
    NSDictionary* report = @{ @"probe":@"sck-metadata-cadence", @"reason":_reason ?: @"unknown", @"error_code":@(_errorCode),
        @"start_succeeded":@(_startSucceeded), @"stop_succeeded":@(_stopSucceeded),
        @"requested_seconds":@(_options.seconds), @"capture_seconds":@(captureSeconds),
        @"requested_fps":@(_options.fps), @"queue_depth":@(_options.queueDepth),
        @"max_height":@(_options.maxHeight), @"configured_width":@(_configuredWidth), @"configured_height":@(_configuredHeight),
        @"requested_pixel_format":_options.pixelFormat == kCVPixelFormatType_32BGRA ? @"bgra" : @"nv12",
        @"display":_displayMetadata ?: @{}, @"stream_callbacks":@(counts.callbacks), @"invalid_samples":@(counts.invalidSamples),
        @"complete":@(counts.complete), @"idle":@(counts.idle), @"started":@(counts.started), @"blank":@(counts.blank),
        @"suspended":@(counts.suspended), @"stopped":@(counts.stopped), @"unknown":@(counts.unknown),
        @"missing_status":@(counts.missingStatus), @"complete_without_image":@(counts.missingImage),
        @"invalid_pts":@(counts.invalidPts), @"pts_regressions":@(counts.ptsRegressions), @"duplicate_pts":@(counts.duplicatePts),
        @"image_width":@(counts.imageWidth), @"image_height":@(counts.imageHeight), @"image_format":@(counts.pixelFormat),
        @"image_changes":@(counts.imageChanges), @"callback_span_seconds":@(callbackSpan), @"complete_span_seconds":@(completeSpan),
        @"callbacks_per_capture_second":@(captureSeconds > 0 ? counts.callbacks / captureSeconds : 0),
        @"complete_per_capture_second":@(captureSeconds > 0 ? counts.complete / captureSeconds : 0),
        @"callback_span_fps":@(callbackSpan > 0 ? (counts.callbacks - 1) / callbackSpan : 0),
        @"complete_span_fps":@(completeSpan > 0 ? (counts.complete - 1) / completeSpan : 0),
        @"callback_interval":Timing(_callbackIntervals), @"complete_callback_interval":Timing(_completeCallbackIntervals),
        @"pts_interval":Timing(_ptsIntervals), @"complete_pts_interval":Timing(_completePtsIntervals),
        @"measurement":@"Local callback intervals and stream presentation timestamp intervals. Minimum frame interval is a requested limit, not guaranteed FPS. Static frames can be idle. Percentiles cover at most 256 recent samples. No pixel content, audio, GPU completion or end-to-end latency is recorded." };
    NSData* json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
    if (json) { std::fwrite(json.bytes, 1, json.length, stdout); std::fputc('\n', stdout); std::fflush(stdout); }
    else { std::fputs("{\"probe\":\"sck-metadata-cadence\",\"reason\":\"json-report-failed\",\"error_code\":8}\n", stdout); _errorCode = 8; }
    // Stop callback ownership and release framework resources without waiting
    // indefinitely for its queues. The accepting gate rejects any late sample.
    _stream = nil; _finished = YES;
}
@end

int main(int argc, const char** argv) {
    @autoreleasepool {
        if (argc == 2 && (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h"))) { Usage(stdout); return 0; }
        Options options;
        if (!ParseOptions(argc, argv, options)) { Usage(stderr); return 64; }
        CadenceProbe* probe = [CadenceProbe new]; probe->_options = options;
        [probe begin];
        const uint64_t deadline = NowUs() + static_cast<uint64_t>(options.seconds + 20) * 1000000;
        while (!probe->_finished && NowUs() < deadline) {
            @autoreleasepool { [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]]; }
        }
        if (!probe->_finished) {
            [probe requestStop:@"overall-timeout" code:9];
            probe->_reason = @"overall-timeout"; probe->_errorCode = 9;
            [probe finish]; // Issue stop, but do not wait past the overall bound.
        }
        return probe->_errorCode ? 1 : 0;
    }
}
