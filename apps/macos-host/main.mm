#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <VideoToolbox/VideoToolbox.h>
#import <Security/Security.h>
#import <Carbon/Carbon.h>
#import "host-ui.h"
#import "localization.h"
#import "login-startup.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include "../../shared/protocol.h"
#include "nv12-layout.hpp"
#include "cursor-image.h"
#include "capture-recovery.h"
#include "mac-exchange.h"
#include "restart-mac.h"
#include "../../shared/diagnostic-log.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>
#include <mach/mach_time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>

static uint64_t NowUs() {
    static mach_timebase_info_data_t tb = [] { mach_timebase_info_data_t x; mach_timebase_info(&x); return x; }();
    return (uint64_t)((long double)mach_absolute_time() * tb.numer / tb.denom / 1000.0L);
}
static void Log(NSString *format, ...) NS_FORMAT_FUNCTION(1,2);
static void Log(NSString *format, ...) {
    va_list ap; va_start(ap, format);
    NSString *s = [[NSString alloc] initWithFormat:format arguments:ap];
    va_end(ap); fprintf(stderr, "%s\n", s.UTF8String); fflush(stderr);
    remote_log::Write(remote_log::Level::Info,"mac-host",s.UTF8String ? s.UTF8String : "empty message");
}
static BOOL ValidPairingToken(NSString *token) {
    NSData *utf32 = [token dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    if (!utf32 || ![token dataUsingEncoding:NSUTF8StringEncoding]) return NO;
    NSUInteger count = utf32.length / sizeof(uint32_t);
    if (count < 6 || count > 128) return NO;
    const uint32_t *characters = (const uint32_t *)utf32.bytes;
    for (NSUInteger i = 0; i < count; ++i)
        if (characters[i] <= 0x1f || (characters[i] >= 0x7f && characters[i] <= 0x9f)) return NO;
    return YES;
}
static NSString *PairingTokenPath(void) {
    NSString *support = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject];
    return [[support stringByAppendingPathComponent:@"RemoteNative"] stringByAppendingPathComponent:@"pairing-token"];
}
static BOOL WritePrivateData(NSString *path, NSData *data) {
    // The temporary file is private from creation through the atomic replacement.
    NSString *temporary = [path stringByAppendingFormat:@".%@.tmp", NSUUID.UUID.UUIDString];
    int file = open(temporary.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) return NO;
    const uint8_t *bytes = (const uint8_t *)data.bytes;
    size_t offset = 0;
    BOOL ok = YES;
    while (offset < data.length) {
        ssize_t written = write(file, bytes + offset, data.length - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = NO; break; }
        offset += (size_t)written;
    }
    if (ok && fsync(file) != 0) ok = NO;
    if (close(file) != 0) ok = NO;
    if (ok && rename(temporary.fileSystemRepresentation, path.fileSystemRepresentation) != 0) ok = NO;
    if (!ok) unlink(temporary.fileSystemRepresentation);
    return ok;
}
static OSStatus SetVT(VTCompressionSessionRef session, CFStringRef key, CFTypeRef value) {
    OSStatus result = VTSessionSetProperty(session, key, value);
    Log(@"VT property %@ = %@, status=%d", (__bridge NSString *)key, (__bridge id)value, (int)result);
    return result;
}
enum class H264EncoderMode { Unavailable, Hardware, AppleSoftware };
struct H264EncoderConfiguration {
    uint32_t width, height, codedHeight = 0, fps = 60;
    bool layoutWorkaround = false;
    H264EncoderMode mode = H264EncoderMode::Unavailable;
};
static NSString *EncoderModeName(H264EncoderMode mode) {
    return mode == H264EncoderMode::Hardware ? @"hardware" :
        mode == H264EncoderMode::AppleSoftware ? @"apple-software" : @"unavailable";
}
static NSString *EncoderID(VTCompressionSessionRef session) {
    CFTypeRef value = nullptr;
    OSStatus status = VTSessionCopyProperty(session, kVTCompressionPropertyKey_EncoderID, nullptr, &value);
    NSString *result = status == noErr && value && CFGetTypeID(value) == CFStringGetTypeID() ?
        [(__bridge NSString *)value copy] : @"";
    if (value) CFRelease(value);
    return result;
}
static bool ForceSoftwareH264() {
    const char *value = getenv("GLIDEBEAM_FORCE_SOFTWARE_H264");
    return value && strcmp(value, "1") == 0;
}
static OSStatus ConfigureEncoder(VTCompressionSessionRef session, bool requireHardware, uint32_t fps) {
    OSStatus realtime = SetVT(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    OSStatus reordering = SetVT(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    if (!requireHardware && (realtime != noErr || reordering != noErr)) return realtime != noErr ? realtime : reordering;
    SetVT(session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    SetVT(session, kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFNumberRef)@(fps));
    SetVT(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFNumberRef)@(fps * 2));
    SetVT(session, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, (__bridge CFNumberRef)@2);
    SetVT(session, kVTCompressionPropertyKey_MaxFrameDelayCount, (__bridge CFNumberRef)@0);
    // Experimental NVIDIA VideoToolbox implementations may reject rate-control properties.
    // The fallback is explicit; both the UI and diagnostics identify software encoding.
    SetVT(session, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFNumberRef)@(requireHardware ? 25000000 : 8000000));
    OSStatus prepared = VTCompressionSessionPrepareToEncodeFrames(session);
    Log(@"VT prepare mode=%@ status=%d", requireHardware ? @"hardware" : @"apple-software", (int)prepared);
    if (prepared != noErr) return prepared;
    CFTypeRef hardware = nullptr;
    OSStatus hwStatus = VTSessionCopyProperty(session,
        kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
        kCFAllocatorDefault, &hardware);
    Log(@"VT hardware query status=%d value=%@", (int)hwStatus, hardware ? (__bridge id)hardware : @"unavailable");
    bool hardwareUsed = hwStatus == noErr && hardware == kCFBooleanTrue;
    if (hardware) CFRelease(hardware);
    if (requireHardware && !hardwareUsed) {
        Log(@"Hardware H264 session did not confirm hardware use; refusing candidate"); return paramErr;
    }
    // Apple software sessions may omit the hardware-use property. The explicit
    // software specification plus the selected Apple encoder ID establish the mode.
    if (!requireHardware && (hardwareUsed || ![EncoderID(session) isEqualToString:@"com.apple.videotoolbox.videoencoder.h264"])) {
        Log(@"Software H264 session did not select the requested Apple encoder; refusing candidate"); return paramErr;
    }
    return noErr;
}
static void InitializeHardwareVideo() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // On this experimental driver, the public decoder discovery initializes
        // the GPU user-mode module and its encoder registration for this process.
        // This is not a supported-property dictionary query or an encoder probe.
        bool decode=VTIsHardwareDecodeSupported(kCMVideoCodecType_H264);
        Log(@"Hardware video discovery initialized; H264 decode support=%d",(int)decode);
    });
}
static bool KnownBrokenNVENCBuild() {
    // Fingerprint the actual module mapped into this process, never a guessed path.
    static std::mutex mutex;
    static std::map<std::string,bool> seen;
    std::lock_guard<std::mutex> lock(mutex);
    static const char expected[]="164f6dfa409352e90b079659578a012dc37d1fba364067b99cb8c7bf337b1b5d";
    for(uint32_t i=0;i<_dyld_image_count();++i) {
        const char *name=_dyld_get_image_name(i);if(!name)continue;
        NSString *path=[NSString stringWithUTF8String:name];
        if(![path.lastPathComponent isEqualToString:@"libnvenc_h264.dylib"])continue;
        auto cached=seen.find(name);if(cached!=seen.end()){if(cached->second)return true;continue;}
        std::ifstream file(name,std::ios::binary);CC_SHA256_CTX sha;CC_SHA256_Init(&sha);
        char block[65536];while(file){file.read(block,sizeof(block));auto count=file.gcount();if(count>0)CC_SHA256_Update(&sha,block,(CC_LONG)count);}
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(digest,&sha);
        char hex[CC_SHA256_DIGEST_LENGTH*2+1]={};for(size_t n=0;n<sizeof(digest);++n)snprintf(hex+n*2,3,"%02x",digest[n]);
        bool match=file.eof()&&!file.bad()&&strcmp(hex,expected)==0;
        seen[name]=match;Log(@"Loaded NVENC module SHA256=%s known-layout-workaround=%d",hex,(int)match);
        if(match)return true;
    }
    return false;
}
static bool SelectedKnownNVENC(VTCompressionSessionRef session) {
    CFTypeRef encoder=nullptr;OSStatus status=VTSessionCopyProperty(session,kVTCompressionPropertyKey_EncoderID,nullptr,&encoder);
    bool selected=status==noErr&&encoder&&CFGetTypeID(encoder)==CFStringGetTypeID()&&
        CFEqual(encoder,CFSTR("com.nvmtl.videoencoder.avc.nvenc"));
    Log(@"Selected encoder ID=%@ status=%d",encoder?(__bridge id)encoder:@"unavailable",(int)status);
    if(encoder)CFRelease(encoder);return selected&&KnownBrokenNVENCBuild();
}
static OSStatus CreateHardwareEncoder(uint32_t width,uint32_t visibleHeight,
    VTCompressionOutputCallback callback,void *context,VTCompressionSessionRef *session,
    uint32_t *codedHeight,bool *layoutWorkaround) {
    *session=nullptr;*codedHeight=visibleHeight;*layoutWorkaround=false;
    auto create=[&](uint32_t height,bool pinKnownEncoder)->OSStatus {
        NSMutableDictionary *spec=[@{(__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder:@YES} mutableCopy];
        if(pinKnownEncoder)spec[(__bridge NSString *)kVTVideoEncoderSpecification_EncoderID]=@"com.nvmtl.videoencoder.avc.nvenc";
        NSDictionary *attrs=@{(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            (__bridge NSString *)kCVPixelBufferWidthKey:@(width),(__bridge NSString *)kCVPixelBufferHeightKey:@(height),
            (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey:@{}};
        return VTCompressionSessionCreate(kCFAllocatorDefault,width,height,kCMVideoCodecType_H264,
            (__bridge CFDictionaryRef)spec,(__bridge CFDictionaryRef)attrs,nullptr,callback,context,session);
    };
    OSStatus status=create(visibleHeight,false);if(status||!*session)return status?status:-1;
    if(!SelectedKnownNVENC(*session))return noErr;
    if(width%64||visibleHeight%2){VTCompressionSessionInvalidate(*session);CFRelease(*session);*session=nullptr;Log(@"Known NVENC layout requires width multiple of 64 and even height");return paramErr;}
    const uint32_t padded=(visibleHeight+15)&~15u;
    if(padded!=visibleHeight) {
        // Destroy the unprepared session before creating the final coded size.
        // No encoding engine has been prepared or submitted at this point.
        VTCompressionSessionInvalidate(*session);CFRelease(*session);*session=nullptr;
        status=create(padded,true);if(status||!*session)return status?status:-1;
        if(!SelectedKnownNVENC(*session)){VTCompressionSessionInvalidate(*session);CFRelease(*session);*session=nullptr;return paramErr;}
    }
    *codedHeight=padded;*layoutWorkaround=true;
    Log(@"Known NVENC layout correction enabled, visible=%ux%u coded=%ux%u",width,visibleHeight,width,padded);
    return noErr;
}
static OSStatus CreateStreamingEncoder(H264EncoderConfiguration& configuration,
    VTCompressionOutputCallback callback, void *context, VTCompressionSessionRef *session, bool forceSoftware) {
    *session = nullptr;
    configuration.codedHeight = configuration.height;
    configuration.layoutWorkaround = false;
    configuration.mode = H264EncoderMode::Unavailable;
    configuration.fps = 60;
    auto discard = [&] {
        if (*session) { VTCompressionSessionInvalidate(*session); CFRelease(*session); *session = nullptr; }
    };
    if (!forceSoftware) {
        OSStatus hardware = CreateHardwareEncoder(configuration.width, configuration.height, callback, context,
            session, &configuration.codedHeight, &configuration.layoutWorkaround);
        if (hardware == noErr && *session) hardware = ConfigureEncoder(*session, true, configuration.fps);
        if (hardware == noErr && *session) { configuration.mode = H264EncoderMode::Hardware; return noErr; }
        Log(@"Hardware H264 unavailable create/configure status=%d; trying Apple software H264", (int)hardware);
        discard();
    } else Log(@"Apple software H264 explicitly selected for this capture");

    // Bound CPU work while preserving aspect ratio. ScreenCaptureKit supplies
    // NV12 at this size; software never receives the NVENC-only row/tile packing.
    double scale = std::min(1.0, std::min(1920.0 / configuration.width, 1080.0 / configuration.height));
    configuration.width = std::max(2u, (uint32_t)(configuration.width * scale) & ~1u);
    configuration.height = std::max(2u, (uint32_t)(configuration.height * scale) & ~1u);
    configuration.codedHeight = configuration.height;
    configuration.layoutWorkaround = false;
    configuration.fps = 30;
    NSDictionary *spec = @{
        (__bridge NSString *)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO,
        (__bridge NSString *)kVTVideoEncoderSpecification_EncoderID: @"com.apple.videotoolbox.videoencoder.h264" };
    NSDictionary *attrs = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (__bridge NSString *)kCVPixelBufferWidthKey: @(configuration.width),
        (__bridge NSString *)kCVPixelBufferHeightKey: @(configuration.height),
        (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{} };
    OSStatus software = VTCompressionSessionCreate(kCFAllocatorDefault, configuration.width, configuration.height,
        kCMVideoCodecType_H264, (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)attrs,
        nullptr, callback, context, session);
    if (software == noErr && *session) software = ConfigureEncoder(*session, false, configuration.fps);
    if (software != noErr || !*session) {
        Log(@"Apple software H264 create/configure failed status=%d", (int)software);
        discard(); return software != noErr ? software : paramErr;
    }
    configuration.mode = H264EncoderMode::AppleSoftware;
    Log(@"Apple software H264 ready %ux%u %u fps; NV12 source layout preserved",
        configuration.width, configuration.height, configuration.fps);
    return noErr;
}
static OSStatus SubmitH264Frame(VTCompressionSessionRef session, H264EncoderMode mode,
    CVPixelBufferRef frame, CMTime pts, CMTime duration, CFDictionaryRef options,
    void *frameContext, OSStatus *drainStatus) {
    *drainStatus = noErr;
    OSStatus encoded = VTCompressionSessionEncodeFrame(session, frame, pts, duration, options, frameContext, nullptr);
    // Apple's software encoder rejects MaxFrameDelayCount and can buffer the
    // first frame despite RealTime/NoBFrames. Complete the submitted timestamp
    // before waiting on our single-inflight gate. The session remains reusable.
    if (encoded == noErr && mode == H264EncoderMode::AppleSoftware)
        *drainStatus = VTCompressionSessionCompleteFrames(session, pts);
    return encoded;
}
static bool PrepackNV12(CVPixelBufferRef source,CVPixelBufferRef target,uint32_t width,uint32_t visibleHeight,uint32_t codedHeight) {
    if(source==target||CVPixelBufferGetPixelFormatType(source)!=kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange||
       CVPixelBufferGetPixelFormatType(target)!=kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange||
       CVPixelBufferGetPlaneCount(source)!=2||CVPixelBufferGetPlaneCount(target)!=2||
       CVPixelBufferGetWidth(source)!=width||CVPixelBufferGetHeight(source)<visibleHeight||
       CVPixelBufferGetWidth(target)!=width||CVPixelBufferGetHeight(target)!=codedHeight)return false;
    if(CVPixelBufferLockBaseAddress(source,kCVPixelBufferLock_ReadOnly)!=kCVReturnSuccess)return false;
    if(CVPixelBufferLockBaseAddress(target,0)!=kCVReturnSuccess){CVPixelBufferUnlockBaseAddress(source,kCVPixelBufferLock_ReadOnly);return false;}
    bool ok=true;
    for(size_t plane=0;plane<2;++plane) {
        const size_t visibleRows=plane?visibleHeight/2:visibleHeight,codedRows=plane?codedHeight/2:codedHeight;
        if(CVPixelBufferGetWidthOfPlane(source,plane)*(plane?2:1)!=width||CVPixelBufferGetWidthOfPlane(target,plane)*(plane?2:1)!=width||
           CVPixelBufferGetHeightOfPlane(source,plane)<visibleRows||CVPixelBufferGetHeightOfPlane(target,plane)!=codedRows){ok=false;break;}
        ok=remote_nv12::PrepackPlane((const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(source,plane),CVPixelBufferGetBytesPerRowOfPlane(source,plane),visibleRows,
            (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(target,plane),CVPixelBufferGetBytesPerRowOfPlane(target,plane),codedRows,width,plane!=0);
        if(!ok)break;
    }
    CVPixelBufferUnlockBaseAddress(target,0);CVPixelBufferUnlockBaseAddress(source,kCVPixelBufferLock_ReadOnly);return ok;
}
static bool AnnexB(CMSampleBufferRef sample, std::vector<uint8_t>& bytes, bool& idr) {
    if (!sample || !CMSampleBufferDataIsReady(sample)) return false;
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    idr = true;
    if (attachments && CFArrayGetCount(attachments)) {
        CFDictionaryRef a = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        idr = !CFDictionaryContainsKey(a, kCMSampleAttachmentKey_NotSync) ||
              CFDictionaryGetValue(a, kCMSampleAttachmentKey_NotSync) == kCFBooleanFalse;
    }
    const uint8_t start[] = {0,0,0,1};
    int nalLengthBytes = 4;
    size_t count = 0, length = 0;
    const uint8_t *set = nullptr;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, &set, &length, &count, &nalLengthBytes) != noErr) return false;
    if (nalLengthBytes < 1 || nalLengthBytes > 4) return false;
    if (idr) {
        for (size_t i = 0; i < count; ++i) {
            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &set, &length, nullptr, nullptr) != noErr) return false;
            bytes.insert(bytes.end(), start, start + 4); bytes.insert(bytes.end(), set, set + length);
        }
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block) return false;
    size_t total = CMBlockBufferGetDataLength(block);
    if (total == 0 || total > 8 * 1024 * 1024 - sizeof(RemoteFrameHeader)) return false;
    std::vector<uint8_t> avcc(total);
    if (CMBlockBufferCopyDataBytes(block, 0, total, avcc.data()) != noErr) return false;
    size_t p = 0;
    while (p + (size_t)nalLengthBytes <= total) {
        uint32_t size = 0;
        for (int i = 0; i < nalLengthBytes; ++i) size = (size << 8) | avcc[p++];
        if (!size || size > total - p) return false;
        bytes.insert(bytes.end(), start, start + 4);
        bytes.insert(bytes.end(), avcc.begin() + p, avcc.begin() + p + size);
        p += size;
    }
    return p == total;
}

struct FrameMeta {
    uint64_t sequence, capturedUs;
    CVPixelBufferRef pixel;
    uint64_t captureEpoch;
    uint32_t width, height;
    ~FrameMeta() {if(pixel)CVPixelBufferRelease(pixel);}
};
// One state per VT session. A callback from a retiring session must never
// decrement the replacement session's single-inflight gate.
struct EncoderFlightState : std::enable_shared_from_this<EncoderFlightState> {
    explicit EncoderFlightState(void *value=nullptr):owner(value) {}
    void *owner;
    std::atomic<int> inflight{0};
    std::mutex pendingLock;
    std::map<FrameMeta *,std::unique_ptr<FrameMeta>> pending;
    bool retired=false;
    FrameMeta *Track(std::unique_ptr<FrameMeta> meta) {
        std::lock_guard lock(pendingLock);
        if(retired)return nullptr;
        auto key=meta.get();pending.emplace(key,std::move(meta));inflight.fetch_add(1);
        return key;
    }
    std::unique_ptr<FrameMeta> Take(FrameMeta *key) {
        std::lock_guard lock(pendingLock);
        auto found=pending.find(key);if(found==pending.end())return {};
        auto meta=std::move(found->second);pending.erase(found);return meta;
    }
    void Reject(FrameMeta *key) {
        auto meta=Take(key);
        if(meta)inflight.fetch_sub(1); // A synchronous callback may already own it.
    }
    void Retire() {
        std::map<FrameMeta *,std::unique_ptr<FrameMeta>> abandoned;
        { std::lock_guard lock(pendingLock);
          retired=true;abandoned.swap(pending);inflight.fetch_sub((int)abandoned.size()); }
        // Release retained source buffers outside the ownership lock.
    }
};
static void RetireEncoder(VTCompressionSessionRef& session,std::shared_ptr<EncoderFlightState>& flight) {
    if(session) {
        VTCompressionSessionCompleteFrames(session,kCMTimeInvalid);
        // Invalidation provides VT's orderly teardown before reclaiming frame
        // contexts for which a failed drain did not deliver a callback.
        VTCompressionSessionInvalidate(session);CFRelease(session);session=nullptr;
    }
    if(flight) { flight->Retire();flight.reset(); }
}
static bool EncoderFlightSelfTest() {
    auto old=std::make_shared<EncoderFlightState>(),replacement=std::make_shared<EncoderFlightState>();
    auto make=[] {return std::unique_ptr<FrameMeta>(new FrameMeta{0,0,nullptr,0,64,64});};
    auto activeKey=old->Track(make());auto abandonedKey=old->Track(make());
    if(old->inflight.load()!=2)return false;
    auto active=old->Take(activeKey);if(!active)return false;
    // Simulate an unsuccessful drain: one callback is active, one never arrived.
    old->Retire();
    if(old->inflight.load()!=1||old->Take(abandonedKey)||old->Track(make()))return false;
    auto replacementKey=replacement->Track(make());
    old->inflight.fetch_sub(1);active.reset();
    if(old->inflight.load()!=0||replacement->inflight.load()!=1)return false;
    // A callback that already took ownership makes a rejected submission a no-op.
    auto completed=replacement->Take(replacementKey);
    if(!completed)return false;
    replacement->Reject(replacementKey);
    if(replacement->inflight.load()!=1)return false;
    replacement->inflight.fetch_sub(1);completed.reset();replacement->Retire();
    return replacement->inflight.load()==0;
}
struct QueuedInput { RemoteInputEvent event; std::vector<uint8_t> text; };

@interface RemoteHost : NSObject <NSApplicationDelegate, SCStreamOutput, SCStreamDelegate, NSTableViewDataSource, NSTableViewDelegate> {
@public
    remote_endpoint *_endpoint;
    VTCompressionSessionRef _encoder;
    std::atomic<bool> _running, _forceKeyframe;
    std::shared_ptr<EncoderFlightState> _encoderFlight;
    std::atomic<uint64_t> _captured, _encoded, _dropped, _encodedBytes, _encodeFailures;
    std::atomic<int> _connection;
    std::mutex _encoderLock;
    std::mutex _captureLock;
    CaptureFrameMailbox _captureFrames;
    std::atomic<uint64_t> _captureEpoch, _frameEpoch;
    std::atomic<void *> _captureSource;
    std::atomic<bool> _captureActive;
    std::atomic<int> _captureFrameState;
    BOOL _stopping, _restartAfterStop;
    uint64_t _captureRetryAt;
    uint64_t _captureStopDeadline;
    unsigned _captureRetryAttempt;
    NSString *_captureState, *_captureStatus;
    std::thread _networkThread, _encodeThread, _bulkThread;
    std::unique_ptr<remote_exchange::Session> _exchange;
    std::unique_ptr<RemoteMacExchange> _nativeExchange;
    std::unique_ptr<RemoteMacRestart> _restart;
    std::map<int,int> _keys;
    bool _buttons[3];
    CGPoint _lastPoint;
    uint64_t _lastSequence;
    bool _hasSequence;
    CGEventFlags _modifiers;
    BOOL _capturing, _starting, _captureBlocked, _cursorBaked, _cursorAvailable, _cursorKnown;
    BOOL _forceVideoCursor;
    BOOL _postAllowed;
    NSData *_previousCursor;
    SCStream *_stream;
    SCDisplay *_display;
    SCStreamConfiguration *_configuration;
    dispatch_queue_t _captureQueue;
    NSStatusItem *_statusItem;
    NSMenuItem *_statusMenu, *_permissionsMenu, *_streamMenu;
    NSTimer *_timer;
    NSString *_token, *_identity, *_bind;
    uint32_t _width, _height, _codedHeight;
    bool _layoutWorkaround;
    uint32_t _encoderFps;
    NSString *_encoderID;
    std::atomic<H264EncoderMode> _encoderMode;
    std::atomic<unsigned> _consecutiveEncodeFailures;
    std::atomic<bool> _encoderRecoveryScheduled;
    BOOL _forceSoftwareCapture;
    CGRect _displayBounds;
    NSWindow *_window;
    RemoteHostPanel *_hostPanel;
    NSString *_displayStatus;
    BOOL _requestPermissionsOnLaunch;
    BOOL _backgroundLaunch;
    id _showWindowObserver;
    NSWindow *_blacklistWindow;
    NSTableView *_blacklistTable;
    NSTextField *_blacklistSummary;
    NSButton *_unblockButton;
    NSArray<NSDictionary *> *_blockedEntries;
    BOOL _policyAvailable;
}
- (instancetype)initWithBind:(NSString *)bind token:(NSString *)token identity:(NSString *)identity
                 videoCursor:(BOOL)videoCursor requestPermissions:(BOOL)request background:(BOOL)background;
- (void)sendStatus:(NSString *)message;
- (void)handleInputs:(std::vector<QueuedInput>)inputs;
- (void)releaseAll;
- (void)probeCursor;
- (void)startCapture;
- (void)stopCapture;
- (NSString *)encoderStreamingStatus;
- (void)encoderFailed:(OSStatus)status epoch:(uint64_t)epoch immediately:(BOOL)immediately;
- (uint64_t)invalidateCaptureSourceLocked;
- (void)captureFailed:(NSError *)error stage:(NSString *)stage;
- (void)captureFrameStatus:(SCFrameStatus)status stream:(SCStream *)stream;
- (void)showWindow:(id)sender;
- (void)requestPermissions:(id)sender;
- (void)retryCapture:(id)sender;
- (void)restartCapture;
- (void)tick:(NSTimer *)timer;
- (void)editToken:(id)sender;
- (void)showBlacklist:(id)sender;
- (void)refreshBlacklist:(id)sender;
- (void)removeBlockedIP:(id)sender;
- (void)updateAuthPolicy;
- (void)toggleLoginStartup:(id)sender;
- (void)toggleExchange:(id)sender;
- (void)showTransfers:(id)sender;
- (void)performPaste:(uint32_t)flags;
@end

static void EncodedCallback(void *context, void *frameContext, OSStatus status,
                            VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    auto flight=((EncoderFlightState *)context)->shared_from_this();
    auto meta=flight->Take((FrameMeta *)frameContext);
    if(!meta)return; // Retired/rejected contexts cannot be completed twice.
    struct CallbackDone {std::shared_ptr<EncoderFlightState> flight;~CallbackDone(){flight->inflight.fetch_sub(1);}} done{flight};
    RemoteHost *host = (__bridge RemoteHost *)flight->owner;
    if (meta->captureEpoch != host->_frameEpoch.load() || !host->_captureActive.load()) return;
    if (status != noErr || !sample) {
        host->_encodeFailures.fetch_add(1);
        Log(@"VT callback status=%d dropped=%d", (int)status, !!(flags & kVTEncodeInfo_FrameDropped));
        [host encoderFailed:status != noErr ? status : paramErr epoch:meta->captureEpoch immediately:NO];
        return;
    }
    std::vector<uint8_t> packet(sizeof(RemoteFrameHeader)); bool idr = false;
    if (!AnnexB(sample, packet, idr)) { host->_encodeFailures.fetch_add(1); [host encoderFailed:paramErr epoch:meta->captureEpoch immediately:NO]; return; }
    if(packet.size()>8*1024*1024) { host->_encodeFailures.fetch_add(1); host->_forceKeyframe.store(true); return; }
    RemoteFrameHeader header = {0x31524652, meta->width, meta->height, idr ? 1u : 0u,
        meta ? meta->sequence : 0, meta ? meta->capturedUs : 0};
    memcpy(packet.data(), &header, sizeof(header));
    // AnnexB construction may overlap a pause/stop/resume. Linearize publication
    // with lifecycle invalidation, rather than relying on the entry check.
    std::lock_guard captureLock(host->_captureLock);
    if(meta->captureEpoch!=host->_frameEpoch.load() || !host->_captureActive.load() || !host->_captureSource.load()) return;
    host->_consecutiveEncodeFailures.store(0);
    host->_encoded.fetch_add(1); host->_encodedBytes.fetch_add(packet.size());
    int result = remote_send(host->_endpoint, REMOTE_VIDEO, packet.data(), packet.size());
    if (result < 0) host->_forceKeyframe.store(true);
}

static CGKeyCode MacKey(int vk, int scan, bool extended, bool shortcuts) {
    // Left/right modifier VKs are preserved; Win becomes Command and Alt becomes Option.
    switch (vk) {
        case 0xA0: return kVK_Shift; case 0xA1: return kVK_RightShift;
        case 0xA2: return shortcuts ? kVK_Command : kVK_Control;
        case 0xA3: return shortcuts ? kVK_RightCommand : kVK_RightControl;
        case 0xA4: return kVK_Option; case 0xA5: return kVK_RightOption;
        case 0x10: return scan == 0x36 ? kVK_RightShift : kVK_Shift;
        case 0x11: return shortcuts ? (extended ? kVK_RightCommand : kVK_Command) : (extended ? kVK_RightControl : kVK_Control);
        case 0x12: return extended ? kVK_RightOption : kVK_Option;
        case 0x5B: return kVK_Command; case 0x5C: return kVK_RightCommand;
        case 0x08: return kVK_Delete; case 0x09: return kVK_Tab;
        case 0x0D: return extended ? static_cast<CGKeyCode>(kVK_ANSI_KeypadEnter) : static_cast<CGKeyCode>(kVK_Return);
        case 0x1B: return kVK_Escape; case 0x20: return kVK_Space;
        case 0x14: return kVK_CapsLock; case 0x2E: return kVK_ForwardDelete;
        case 0x24: return kVK_Home; case 0x23: return kVK_End;
        case 0x21: return kVK_PageUp; case 0x22: return kVK_PageDown;
        case 0x25: return kVK_LeftArrow; case 0x26: return kVK_UpArrow;
        case 0x27: return kVK_RightArrow; case 0x28: return kVK_DownArrow;
        case 0x6A: return kVK_ANSI_KeypadMultiply; case 0x6B: return kVK_ANSI_KeypadPlus;
        case 0x6D: return kVK_ANSI_KeypadMinus; case 0x6E: return kVK_ANSI_KeypadDecimal;
        case 0x6F: return kVK_ANSI_KeypadDivide; case 0x90: return kVK_ANSI_KeypadClear;
        case 0xBA: return kVK_ANSI_Semicolon; case 0xBB: return kVK_ANSI_Equal;
        case 0xBC: return kVK_ANSI_Comma; case 0xBD: return kVK_ANSI_Minus;
        case 0xBE: return kVK_ANSI_Period; case 0xBF: return kVK_ANSI_Slash;
        case 0xC0: return kVK_ANSI_Grave; case 0xDB: return kVK_ANSI_LeftBracket;
        case 0xDC: return kVK_ANSI_Backslash; case 0xDD: return kVK_ANSI_RightBracket;
        case 0xDE: return kVK_ANSI_Quote;
    }
    static const CGKeyCode letters[] = {kVK_ANSI_A,kVK_ANSI_B,kVK_ANSI_C,kVK_ANSI_D,kVK_ANSI_E,kVK_ANSI_F,
        kVK_ANSI_G,kVK_ANSI_H,kVK_ANSI_I,kVK_ANSI_J,kVK_ANSI_K,kVK_ANSI_L,kVK_ANSI_M,kVK_ANSI_N,
        kVK_ANSI_O,kVK_ANSI_P,kVK_ANSI_Q,kVK_ANSI_R,kVK_ANSI_S,kVK_ANSI_T,kVK_ANSI_U,kVK_ANSI_V,
        kVK_ANSI_W,kVK_ANSI_X,kVK_ANSI_Y,kVK_ANSI_Z};
    static const CGKeyCode digits[] = {kVK_ANSI_0,kVK_ANSI_1,kVK_ANSI_2,kVK_ANSI_3,kVK_ANSI_4,
        kVK_ANSI_5,kVK_ANSI_6,kVK_ANSI_7,kVK_ANSI_8,kVK_ANSI_9};
    static const CGKeyCode keypad[] = {kVK_ANSI_Keypad0,kVK_ANSI_Keypad1,kVK_ANSI_Keypad2,kVK_ANSI_Keypad3,
        kVK_ANSI_Keypad4,kVK_ANSI_Keypad5,kVK_ANSI_Keypad6,kVK_ANSI_Keypad7,kVK_ANSI_Keypad8,kVK_ANSI_Keypad9};
    static const CGKeyCode function[] = {kVK_F1,kVK_F2,kVK_F3,kVK_F4,kVK_F5,kVK_F6,kVK_F7,kVK_F8,kVK_F9,kVK_F10,kVK_F11,kVK_F12};
    if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
    if (vk >= '0' && vk <= '9') return digits[vk - '0'];
    if (vk >= 0x60 && vk <= 0x69) return keypad[vk - 0x60];
    if (vk >= 0x70 && vk <= 0x7B) return function[vk - 0x70];
    return UINT16_MAX;
}
static CGEventFlags ModifierFlag(int key) {
    switch (key) {
        case kVK_Shift: case kVK_RightShift: return kCGEventFlagMaskShift;
        case kVK_Control: case kVK_RightControl: return kCGEventFlagMaskControl;
        case kVK_Option: case kVK_RightOption: return kCGEventFlagMaskAlternate;
        case kVK_Command: case kVK_RightCommand: return kCGEventFlagMaskCommand;
        default: return 0;
    }
}

@implementation RemoteHost
- (instancetype)initWithBind:(NSString *)bind token:(NSString *)token identity:(NSString *)identity
                 videoCursor:(BOOL)videoCursor requestPermissions:(BOOL)request background:(BOOL)background {
    self = [super init]; if (!self) return nil;
    _bind = bind; _token = token; _identity = identity;
    _forceVideoCursor = videoCursor; _cursorBaked = YES; _cursorAvailable = NO; _cursorKnown = NO;
    _requestPermissionsOnLaunch = request;
    _backgroundLaunch = background;
    _postAllowed=CGPreflightPostEventAccess();
    _running.store(true); _forceKeyframe.store(true); _connection.store(0);
    _captured.store(0); _encoded.store(0); _dropped.store(0); _encodedBytes.store(0); _encodeFailures.store(0);
    _encoder = nullptr; _endpoint = nullptr;
    _captureEpoch.store(1); _frameEpoch.store(1); _captureSource.store(nullptr);
    _captureActive.store(false); _captureFrameState.store(SCFrameStatusStopped);
    _captureState = @"idle";
    _width = 1920; _height = 1080; _displayBounds = CGDisplayBounds(CGMainDisplayID());
    _encoderFps = 60; _encoderID = @""; _encoderMode.store(H264EncoderMode::Unavailable);
    _consecutiveEncodeFailures.store(0); _encoderRecoveryScheduled.store(false);
    _forceSoftwareCapture = ForceSoftwareH264();
    _lastPoint = CGPointMake(CGRectGetMidX(_displayBounds), CGRectGetMidY(_displayBounds));
    _modifiers = 0; _hasSequence = false; _lastSequence = 0;
    for (int i=0;i<3;++i) _buttons[i] = false;
    _captureQueue = dispatch_queue_create("remote.capture", DISPATCH_QUEUE_SERIAL);
    return self;
}
- (void)installLocalizedMenus {
    RemoteHostInstallApplicationMenu();
    _statusItem.button.title = RemoteHostBrand();
    NSMenu *menu = [NSMenu new];
    _statusMenu = [[NSMenuItem alloc] initWithTitle:RemoteHostText(_displayStatus ?: @"正在启动…") action:nil keyEquivalent:@""];
    [menu addItem:_statusMenu];
    NSMenuItem *show = [[NSMenuItem alloc] initWithTitle:RemoteHostText(@"连接信息与权限状态…") action:@selector(showWindow:) keyEquivalent:@""];
    show.target = self; [menu addItem:show];
    _permissionsMenu = [[NSMenuItem alloc] initWithTitle:RemoteHostText(@"设置屏幕与输入权限…") action:@selector(requestPermissions:) keyEquivalent:@""];
    _permissionsMenu.target = self; [menu addItem:_permissionsMenu];
    _streamMenu = [[NSMenuItem alloc] initWithTitle:RemoteHostText(@"重试画面采集") action:@selector(retryCapture:) keyEquivalent:@""];
    _streamMenu.target = self; [menu addItem:_streamMenu];
    NSMenuItem *tokenItem = [[NSMenuItem alloc] initWithTitle:RemoteHostText(@"修改配对令牌…") action:@selector(editToken:) keyEquivalent:@""];
    tokenItem.target = self; [menu addItem:tokenItem];
    NSMenuItem *blacklistItem = [[NSMenuItem alloc] initWithTitle:RemoteHostText(@"黑名单管理…") action:@selector(showBlacklist:) keyEquivalent:@""];
    blacklistItem.target = self; [menu addItem:blacklistItem];
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *language = [[NSMenuItem alloc] initWithTitle:RemoteHostText(@"语言") action:nil keyEquivalent:@""];
    language.submenu = RemoteHostMakeLanguageMenu(self, @selector(changeLanguage:));
    [menu addItem:language];
    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:RemoteHostText(@"退出 Mac 端") action:@selector(terminate:) keyEquivalent:@"q"];
    [menu addItem:quit]; _statusItem.menu = menu;
}
- (void)changeLanguage:(NSMenuItem *)sender {
    RemoteHostSetLanguagePreference(static_cast<remote_i18n::Language>(sender.tag));
}
- (void)languageChanged:(NSNotification *)note {
    (void)note;
    [self installLocalizedMenus];
    if (_window) _window.title = RemoteHostText(@"光梭 · Mac 端");
    [_hostPanel applyLocalization];
    if (_blacklistWindow) {
        _blacklistWindow.title = RemoteHostText(@"光梭 · 黑名单管理");
        for (NSView *view in _blacklistWindow.contentView.subviews) {
            if ([view isKindOfClass:NSTextField.class]) {
                NSTextField *label = (NSTextField *)view;
                if (label != _blacklistSummary) label.stringValue = RemoteHostText(label.stringValue);
            } else if ([view isKindOfClass:NSButton.class]) {
                NSButton *button = (NSButton *)view;
                button.title = RemoteHostText(button.title); button.needsDisplay = YES;
            }
        }
        for (NSTableColumn *column in _blacklistTable.tableColumns)
            column.title = RemoteHostText([column.identifier isEqualToString:@"ip"] ? @"IP 地址" : @"失败次数");
        [self updateAuthPolicy];
    }
    [self tick:nil];
}
- (void)applicationDidFinishLaunching:(NSNotification *)note {
    __weak RemoteHost *weakSelf = self;
    _showWindowObserver = [NSDistributedNotificationCenter.defaultCenter addObserverForName:@"dev.remote-native.host.show-window"
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification) { [weakSelf showWindow:nil]; }];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(languageChanged:) name:RemoteHostLanguageChangedNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(languageChanged:) name:NSCurrentLocaleDidChangeNotification object:nil];
    _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    [self installLocalizedMenus];
    _endpoint = remote_server_start(_bind.UTF8String, _token.UTF8String, _identity.fileSystemRepresentation);
    if (!_endpoint) { [self sendStatus:@"Transport startup failed"]; if (!_backgroundLaunch) [self showWindow:nil]; return; }
    _restart=std::make_unique<RemoteMacRestart>([endpoint=_endpoint](const uint8_t* data,size_t size) {
        if(remote_status(endpoint)==1) remote_send(endpoint,REMOTE_CONTROL,data,size);
    });
    NSString *receive=[NSUserDefaults.standardUserDefaults stringForKey:@"RemoteReceiveDirectory"];
    if(!receive.length)receive=[NSHomeDirectory() stringByAppendingPathComponent:@"Downloads/Remote Native"];
    _exchange=std::make_unique<remote_exchange::Session>(true,remote_exchange::fs::path(receive.fileSystemRepresentation),
        [endpoint=_endpoint](const uint8_t *data,size_t size){ return remote_bulk_send(endpoint,data,size); });
    _nativeExchange=std::make_unique<RemoteMacExchange>(*_exchange,[self](uint32_t flags){ [self performPaste:flags]; });
    _nativeExchange->SetEnabled(![NSUserDefaults.standardUserDefaults boolForKey:@"RemoteExchangeDisabled"]);
    _bulkThread=std::thread([self] {
        std::vector<uint8_t> buffer(2*1024*1024);
        size_t pendingBytes=0;
        bool previous=false;
        while(self->_running.load()) {
            bool connected=remote_status(self->_endpoint)==1 && remote_bulk_status(self->_endpoint)==1;
            if(connected!=previous) { previous=connected; remote_log::Write(remote_log::Level::Info,"bulk",connected ? "ready" : "disconnected"); }
            self->_exchange->SetConnected(connected);
            if(!connected)pendingBytes=0;
            if(connected) {
                self->_exchange->SetNetworkRtt(remote_rtt_ms(self->_endpoint));
                for(int n=0;n<32;++n) {
                    // Retain a dequeued frame until the bounded engine inbox
                    // accepts it. Polling another frame would overwrite it and
                    // silently corrupt a transfer under receiver backpressure.
                    if(!pendingBytes) {
                        int bytes=remote_bulk_poll(self->_endpoint,buffer.data(),buffer.size());
                        if(bytes<=0)break;
                        pendingBytes=(size_t)bytes;
                    }
                    if(remote_status(self->_endpoint)!=1 || remote_bulk_status(self->_endpoint)!=1) {
                        pendingBytes=0;
                        self->_exchange->SetConnected(false);
                        break;
                    }
                    if(!self->_exchange->Receive(buffer.data(),pendingBytes))break;
                    pendingBytes=0;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        self->_exchange->SetConnected(false);
    });
    _networkThread = std::thread([self] {
        std::vector<uint8_t> buffer(8 * 1024 * 1024);
        int previous = -99;
        while (self->_running.load()) {
            int state = remote_status(self->_endpoint);
            self->_connection.store(state);
            if(self->_restart)self->_restart->SetConnected(state==1);
            if (state != previous) {
                bool disconnected = previous == 1 && state != 1;
                previous = state;
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (disconnected) [self releaseAll];
                    if (state == 1) { self->_previousCursor = nil; self->_forceKeyframe.store(true); [self sendStatus:@"Connected"]; }
                    else if (state < 0) {
                        char error[512] = {}; remote_last_error(self->_endpoint,error,sizeof(error));
                        [self sendStatus:[NSString stringWithFormat:@"Transport error: %s",error]];
                    } else [self sendStatus:@"Listening; waiting for paired Windows client"];
                });
            }
            std::vector<QueuedInput> batch;
            for (int n = 0; n < 256; ++n) {
                uint32_t channel = 0;
                int bytes = remote_poll(self->_endpoint,&channel,buffer.data(),buffer.size());
                if (bytes <= 0) break;
                if(channel==REMOTE_CONTROL) {
                    if(self->_restart && remote_status(self->_endpoint)==1)self->_restart->Handle(buffer.data(),(size_t)bytes);
                    continue;
                }
                if (channel != REMOTE_INPUT || (size_t)bytes < sizeof(RemoteInputEvent)) continue;
                QueuedInput item; memcpy(&item.event,buffer.data(),sizeof(item.event));
                if (item.event.type < INPUT_MOVE || item.event.type > INPUT_PASTE) continue;
                if (item.event.type == INPUT_TEXT) {
                    if ((size_t)bytes > sizeof(RemoteInputEvent) + 65536) continue;
                    item.text.assign(buffer.begin()+sizeof(RemoteInputEvent),buffer.begin()+bytes);
                } else if ((size_t)bytes != sizeof(RemoteInputEvent)) continue;
                // Collapse consecutive move updates without crossing a button/key boundary.
                if (item.event.type == INPUT_MOVE && !batch.empty() && batch.back().event.type == INPUT_MOVE) batch.back() = std::move(item);
                else batch.push_back(std::move(item));
            }
            if (!batch.empty()) dispatch_async(dispatch_get_main_queue(), ^{ [self handleInputs:batch]; });
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    _encodeThread = std::thread([self] {
        while (self->_running.load()) {
            if (self->_connection.load() != 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue;
            }
            std::unique_lock<std::mutex> encoderLock(self->_encoderLock);
            auto flight=self->_encoderFlight;
            if (!self->_encoder || !flight || flight->inflight.load()>=1) { encoderLock.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            CVPixelBufferRef frame = nullptr; CMTime pts = kCMTimeInvalid; uint64_t captured = 0;
            uint64_t epoch = 0;
            { std::lock_guard captureLock(self->_captureLock);
              epoch = self->_frameEpoch.load();
              if(self->_captureActive.load() && self->_captureSource.load()) frame = self->_captureFrames.Take(epoch,self->_forceKeyframe.load(),captured); }
            // Static desktops can supply a new IDR, but only from the current,
            // actively shared stream. A paused/stopped stream never supplies it.
            if(frame) pts=CMTimeMake((int64_t)NowUs(),1000000);
            if (!frame) { encoderLock.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            if(self->_layoutWorkaround) {
                CVPixelBufferRef target=nullptr;
                CVReturn allocated=CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,VTCompressionSessionGetPixelBufferPool(self->_encoder),&target);
                bool packed=allocated==kCVReturnSuccess&&target&&PrepackNV12(frame,target,self->_width,self->_height,self->_codedHeight);
                CVPixelBufferRelease(frame);frame=target;
                if(!packed){if(frame)CVPixelBufferRelease(frame);self->_encodeFailures.fetch_add(1);self->_forceKeyframe.store(true);Log(@"NV12 layout correction failed allocation=%d",(int)allocated);continue;}
            }
            // Keep the input surface alive until its encoder callback finishes.
            // Experimental NVENC does not reliably retain pooled source buffers.
            auto meta = flight->Track(std::unique_ptr<FrameMeta>(new FrameMeta{
                self->_captured.fetch_add(1)+1, captured, frame, epoch, self->_width, self->_height}));
            if(!meta)continue;
            CFDictionaryRef options = nullptr;
            bool force = self->_forceKeyframe.exchange(false);
            if (force) options = (__bridge CFDictionaryRef)@{(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame:@YES};
            OSStatus drainStatus = noErr;
            OSStatus result = SubmitH264Frame(self->_encoder,self->_encoderMode.load(),frame,pts,
                CMTimeMake(1,self->_encoderFps),options,meta,&drainStatus);
            encoderLock.unlock();
            if (result != noErr) {
                flight->Reject(meta); self->_encodeFailures.fetch_add(1);
                self->_forceKeyframe.store(true); Log(@"VT encode status=%d",(int)result);
                [self encoderFailed:result epoch:epoch immediately:NO];
            } else if (drainStatus != noErr) {
                // A failed drain may still own the frame context. Stop/invalidate
                // through the lifecycle gate; do not free callback-owned memory.
                self->_encodeFailures.fetch_add(1);self->_forceKeyframe.store(true);
                Log(@"Apple software H264 frame completion failed status=%d",(int)drainStatus);
                [self encoderFailed:drainStatus epoch:epoch immediately:YES];
            }
        }
    });
    char fingerprint[256] = {};
    if (remote_fingerprint(_endpoint,fingerprint,sizeof(fingerprint)) > 0) Log(@"Server certificate SHA256: %s",fingerprint);
    [self probeCursor];
    _timer = [NSTimer timerWithTimeInterval:1.0/60.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
    if (!_backgroundLaunch) [self showWindow:nil];
    if (_requestPermissionsOnLaunch) [self requestPermissions:nil];
    else if (CGPreflightScreenCaptureAccess()) [self startCapture];
    else [self sendStatus:@"Screen Recording permission required; click Request permissions"];
}
- (void)sendStatus:(NSString *)message {
    if(!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(),^{ [self sendStatus:message]; });
        return;
    }
    Log(@"%@",message);
    _displayStatus = _captureStatus.length ? _captureStatus : RemoteHostLocalizedStatus(message);
    _statusMenu.title = RemoteHostText(_displayStatus);
    if (_endpoint && _connection.load() == 1) {
        NSDictionary *status = @{ @"type":@"host-status", @"message":message,
            @"capture_state":_captureState ?: @"idle", @"capture_message":_captureStatus ?: @"",
            @"cursor":_cursorBaked ? @"video-compatibility" : @"local",
            @"width":@(_width), @"height":@(_height), @"fps":@(_encoderFps),
            @"encoder_mode":EncoderModeName(_encoderMode.load()), @"encoder_id":_encoderID ?: @"",
            @"coded_height":@(_codedHeight), @"nvenc_layout_workaround":@(_layoutWorkaround),
            @"captured":@(_captured.load()), @"encoded":@(_encoded.load()),
            @"dropped":@(_dropped.load()), @"encoded_bytes":@(_encodedBytes.load()),
            @"encode_failures":@(_encodeFailures.load()),
            @"restart_supported":@YES, @"restart_requires_local_authorization":@YES,
            @"restart_busy":@(_restart && _restart->Busy()),
            @"accessibility":@(CGPreflightPostEventAccess()), @"screen_recording":@(CGPreflightScreenCaptureAccess()) };
        NSData *data = [NSJSONSerialization dataWithJSONObject:status options:0 error:nil];
        remote_send(_endpoint,REMOTE_CONTROL,(const uint8_t *)data.bytes,data.length);
    }
}
- (void)showWindow:(id)sender {
    if (!_window) {
        _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,840,860)
            styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        _window.title = RemoteHostText(@"光梭 · Mac 端");
        _window.contentMinSize = NSMakeSize(720,840);
        _window.releasedWhenClosed = NO;
        _window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        _window.titlebarAppearsTransparent = YES;
        _window.backgroundColor = [NSColor colorWithSRGBRed:11/255.0 green:17/255.0 blue:24/255.0 alpha:1];
        _hostPanel = [[RemoteHostPanel alloc] initWithTarget:self permissions:@selector(requestPermissions:) retry:@selector(retryCapture:) editToken:@selector(editToken:) blacklist:@selector(showBlacklist:) loginStartup:@selector(toggleLoginStartup:)];
        [_hostPanel setExchangeTarget:self toggle:@selector(toggleExchange:) files:@selector(showTransfers:)];
        _window.contentView = _hostPanel;
        [_window center];
    }
    [self tick:nil]; [NSApp activateIgnoringOtherApps:YES]; [_window makeKeyAndOrderFront:nil];
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)visible {
    [self showWindow:nil];
    return NO;
}
- (void)requestPermissions:(id)sender {
    [NSApp activateIgnoringOtherApps:YES];
    if (!CGPreflightScreenCaptureAccess()) CGRequestScreenCaptureAccess();
    NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt:@YES};
    AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
    if (!CGPreflightPostEventAccess()) CGRequestPostEventAccess();
    [self sendStatus:@"Grant Screen Recording and Accessibility in System Settings; then retry capture"];
}
- (void)toggleLoginStartup:(id)sender {
    BOOL enabled = [sender state] == NSControlStateValueOn;
    NSString *message = nil;
    if (!RemoteLoginSetEnabled(enabled, &message)) [self showError:message ?: RemoteHostText(@"无法修改登录自启状态。")];
    [self tick:nil];
}
- (void)showError:(NSString *)message {
    NSAlert *alert = [NSAlert new];
    alert.messageText = RemoteHostText(@"操作未完成"); alert.informativeText = message;
    [alert addButtonWithTitle:RemoteHostText(@"知道了")];
    alert.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    [alert beginSheetModalForWindow:_window completionHandler:nil];
}
- (void)editToken:(id)sender {
    [self showWindow:nil];
    if (!_endpoint) { [self showError:RemoteHostText(@"连接服务不可用，请先重新启动 Mac 端。")]; return; }
    NSAlert *alert = [NSAlert new];
    alert.messageText = RemoteHostText(@"修改配对令牌");
    alert.informativeText = RemoteHostText(@"输入 6–128 个字符，不包含换行或控制字符。令牌会保存到这台 Mac。保存后当前连接会断开，请使用新令牌重新连接。");
    [alert addButtonWithTitle:RemoteHostText(@"保存令牌")]; [alert addButtonWithTitle:RemoteHostText(@"取消")];
    NSTextField *input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 440, 36)];
    input.stringValue = _token ?: @"";
    input.font = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightRegular];
    input.accessibilityLabel = RemoteHostText(@"自定义配对令牌");
    alert.accessoryView = input;
    alert.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    [alert.window setInitialFirstResponder:input];
    [alert beginSheetModalForWindow:_window completionHandler:^(NSModalResponse result) {
        if (result != NSAlertFirstButtonReturn) return;
        NSString *updated = input.stringValue;
        if (!ValidPairingToken(updated)) { [self showError:RemoteHostText(@"令牌需要 6–128 个字符，且不能包含换行或控制字符。")]; return; }
        if ([updated isEqualToString:self->_token]) return;
        NSString *path = PairingTokenPath();
        BOOL existed = [[NSFileManager defaultManager] fileExistsAtPath:path];
        NSData *previous = [NSData dataWithContentsOfFile:path];
        if (existed && !previous) { [self showError:RemoteHostText(@"无法读取原令牌，未做修改。请检查本机文件权限后重试。")]; return; }
        if (!WritePrivateData(path, [updated dataUsingEncoding:NSUTF8StringEncoding])) {
            [self showError:RemoteHostText(@"无法保存令牌，请检查本机文件权限后重试。")]; return;
        }
        if (remote_set_token(self->_endpoint, updated.UTF8String) != 1) {
            BOOL restored = previous ? WritePrivateData(path, previous) : unlink(path.fileSystemRepresentation) == 0;
            [self showError:restored ? RemoteHostText(@"连接服务未接受新令牌，已恢复原令牌。") : RemoteHostText(@"连接服务未接受新令牌，恢复文件失败，请重新设置令牌后再启动 Mac 端。")]; return;
        }
        self->_token = [updated copy];
        [self sendStatus:@"Pairing token updated"];
        [self tick:nil];
    }];
}
- (void)updateAuthPolicy {
    NSString *selected = nil;
    NSInteger row = _blacklistTable.selectedRow;
    if (row >= 0 && row < (NSInteger)_blockedEntries.count) selected = _blockedEntries[(NSUInteger)row][@"ip"];
    NSMutableArray<NSDictionary *> *entries = [NSMutableArray new];
    _policyAvailable = NO;
    if (_endpoint) {
        std::vector<char> json(1024 * 1024);
        int length = remote_auth_policy_json(_endpoint, json.data(), json.size());
        if (length >= 0) {
            NSData *data = [NSData dataWithBytes:json.data() length:(NSUInteger)length];
            id policy = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            if ([policy isKindOfClass:NSDictionary.class] && [policy[@"entries"] isKindOfClass:NSArray.class]) {
                _policyAvailable = YES;
                for (id entry in policy[@"entries"])
                    if ([entry isKindOfClass:NSDictionary.class] && [entry[@"blocked"] boolValue] &&
                        [entry[@"ip"] isKindOfClass:NSString.class] && [entry[@"failures"] isKindOfClass:NSNumber.class]) [entries addObject:entry];
            }
        }
    }
    [entries sortUsingComparator:^NSComparisonResult(NSDictionary *left, NSDictionary *right) {
        return [left[@"ip"] compare:right[@"ip"] options:NSNumericSearch];
    }];
    if (![_blockedEntries isEqualToArray:entries]) {
        _blockedEntries = entries;
        [_blacklistTable reloadData];
        [_blacklistTable deselectAll:nil];
        if (selected) for (NSUInteger i = 0; i < entries.count; ++i)
            if ([entries[i][@"ip"] isEqualToString:selected]) { [_blacklistTable selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO]; break; }
    }
    _blacklistSummary.stringValue = !_policyAvailable ? RemoteHostText(@"暂时无法读取黑名单，请检查连接服务。") :
        (_blockedEntries.count ? [NSString stringWithFormat:RemoteHostText(@"已封禁 %lu 个 IP · 选中记录可解除封禁"), (unsigned long)_blockedEntries.count] : RemoteHostText(@"暂无封禁记录"));
    _unblockButton.enabled = _policyAvailable && _blacklistTable.selectedRow >= 0;
    _unblockButton.needsDisplay = YES;
}
- (void)showBlacklist:(id)sender {
    if (!_blacklistWindow) {
        _blacklistWindow = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 720, 460)
            styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        _blacklistWindow.title = RemoteHostText(@"光梭 · 黑名单管理"); _blacklistWindow.releasedWhenClosed = NO;
        _blacklistWindow.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        _blacklistWindow.titlebarAppearsTransparent = YES;
        _blacklistWindow.backgroundColor = [NSColor colorWithSRGBRed:11/255.0 green:17/255.0 blue:24/255.0 alpha:1];
        NSView *content = _blacklistWindow.contentView;
        NSTextField *title = [NSTextField labelWithString:RemoteHostText(@"连接黑名单")];
        title.frame = NSMakeRect(28, 401, 660, 30); title.font = [NSFont systemFontOfSize:23 weight:NSFontWeightSemibold];
        [content addSubview:title];
        NSTextField *hint = [NSTextField labelWithString:RemoteHostText(@"同一 IP 连续 10 次令牌验证失败将封禁；断网和视频错误不计入。")];
        hint.frame = NSMakeRect(28, 366, 664, 25); hint.font = [NSFont systemFontOfSize:12]; hint.textColor = NSColor.secondaryLabelColor;
        [content addSubview:hint];
        NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(28, 100, 664, 250)];
        scroll.hasVerticalScroller = YES; scroll.autohidesScrollers = YES; scroll.borderType = NSLineBorder;
        _blacklistTable = [[NSTableView alloc] initWithFrame:scroll.bounds];
        _blacklistTable.dataSource = self; _blacklistTable.delegate = self;
        _blacklistTable.rowHeight = 38; _blacklistTable.allowsMultipleSelection = NO;
        _blacklistTable.backgroundColor = [NSColor colorWithSRGBRed:13/255.0 green:21/255.0 blue:30/255.0 alpha:1];
        NSTableColumn *ip = [[NSTableColumn alloc] initWithIdentifier:@"ip"]; ip.title = RemoteHostText(@"IP 地址"); ip.width = 480;
        NSTableColumn *failures = [[NSTableColumn alloc] initWithIdentifier:@"failures"]; failures.title = RemoteHostText(@"失败次数"); failures.width = 150;
        [_blacklistTable addTableColumn:ip]; [_blacklistTable addTableColumn:failures];
        scroll.documentView = _blacklistTable; [content addSubview:scroll];
        _blacklistSummary = [NSTextField labelWithString:RemoteHostText(@"正在读取…")];
        _blacklistSummary.frame = NSMakeRect(28, 67, 664, 22); _blacklistSummary.font = [NSFont systemFontOfSize:12];
        _blacklistSummary.textColor = NSColor.secondaryLabelColor; [content addSubview:_blacklistSummary];
        _unblockButton = RemoteHostMakeButton(RemoteHostText(@"解除封禁"), self, @selector(removeBlockedIP:), YES);
        _unblockButton.frame = NSMakeRect(28, 18, 140, 38); [content addSubview:_unblockButton];
        NSButton *refresh = RemoteHostMakeButton(RemoteHostText(@"刷新列表"), self, @selector(refreshBlacklist:), NO);
        refresh.frame = NSMakeRect(180, 18, 124, 38); [content addSubview:refresh];
        [_blacklistWindow center];
    }
    [self updateAuthPolicy]; [_blacklistTable reloadData];
    [NSApp activateIgnoringOtherApps:YES]; [_blacklistWindow makeKeyAndOrderFront:nil];
}
- (void)refreshBlacklist:(id)sender { [self updateAuthPolicy]; [self tick:nil]; }
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView { return (NSInteger)_blockedEntries.count; }
- (id)tableView:(NSTableView *)tableView objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_blockedEntries.count) return @"";
    return _blockedEntries[(NSUInteger)row][column.identifier] ?: @"";
}
- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    _unblockButton.enabled = _policyAvailable && _blacklistTable.selectedRow >= 0; _unblockButton.needsDisplay = YES;
}
- (void)removeBlockedIP:(id)sender {
    NSInteger row = _blacklistTable.selectedRow;
    if (row < 0 || row >= (NSInteger)_blockedEntries.count || !_endpoint) return;
    NSString *ip = _blockedEntries[(NSUInteger)row][@"ip"];
    if (remote_unblock_ip(_endpoint, ip.UTF8String) != 1) {
        NSAlert *alert = [NSAlert new]; alert.messageText = RemoteHostText(@"无法解除封禁");
        alert.informativeText = RemoteHostText(@"保存黑名单失败，请检查本机文件权限后重试。"); [alert addButtonWithTitle:RemoteHostText(@"知道了")];
        [alert beginSheetModalForWindow:_blacklistWindow completionHandler:nil]; return;
    }
    [self refreshBlacklist:nil];
}
- (void)retryCapture:(id)sender {
    if (!CGPreflightScreenCaptureAccess()) { [self requestPermissions:nil]; return; }
    [self restartCapture];
}
- (void)restartCapture {
    _captureBlocked=NO; _captureRetryAt=0; _captureRetryAttempt=0;
    _forceSoftwareCapture=ForceSoftwareH264();
    _captureState=@"recovering"; _captureStatus=@"正在重新建立画面采集…";
    _restartAfterStop=YES;
    [self stopCapture];
    [self sendStatus:_captureStatus];
    if (!_stopping) { _restartAfterStop=NO; [self startCapture]; }
}
- (void)tick:(NSTimer *)timer {
    if(_nativeExchange)_nativeExchange->Tick(_endpoint && remote_status(_endpoint)==1 && remote_bulk_status(_endpoint)==1);
    [self probeCursor];
    static uint64_t lastStatus = 0;
    uint64_t now = NowUs();
    // A hung system stop completion must not make the explicit retry button
    // ineffective forever. Old callbacks remain invalidated by their epoch.
    if(_stopping && now>=_captureStopDeadline) {
        _stopping=NO;
        { std::lock_guard captureLock(_captureLock); _captureEpoch.fetch_add(1); }
        Log(@"Capture stop completion timed out; abandoned old stream");
        if(_restartAfterStop) { _restartAfterStop=NO; [self startCapture]; }
    }
    if (timer && now-lastStatus<=2000000) return;
    if (!timer || now-lastStatus > 2000000) {
        lastStatus = now;
        BOOL allowed=CGPreflightPostEventAccess();
        if(_postAllowed && !allowed)[self releaseAll];
        _postAllowed=allowed;
        if (!_capturing && !_starting && !_stopping && !_captureBlocked &&
            now>=_captureRetryAt && CGPreflightScreenCaptureAccess()) [self startCapture];
        if (_connection.load() == 1) [self sendStatus:_captureStatus ?: (_capturing ? [self encoderStreamingStatus] : @"Waiting for screen permission/capture")];
    }
    char fingerprint[256] = {};
    if (_endpoint) remote_fingerprint(_endpoint,fingerprint,sizeof(fingerprint));
    RemoteHostPanelState *state = [RemoteHostPanelState new];
    state.address = RemoteHostConnectionAddress(_bind);
    state.token = _token;
    state.fingerprint = [NSString stringWithUTF8String:fingerprint] ?: @"";
    state.status = _displayStatus ?: RemoteHostText(@"连接服务正在启动…");
    state.screenGranted = CGPreflightScreenCaptureAccess();
    state.inputGranted = CGPreflightPostEventAccess();
    state.capturing = _capturing && _captureActive.load(); state.starting = _starting || _stopping;
    state.connected = _connection.load() == 1;
    state.cursorBaked = _cursorBaked;
    state.width = _width; state.height = _height;
    state.encoded = _encoded.load(); state.replaced = _dropped.load();
    [self updateAuthPolicy]; state.blockedCount = _blockedEntries.count;
    state.loginStartup = RemoteLoginEnabled();
    state.exchangeEnabled=_nativeExchange && _nativeExchange->Enabled();
    state.exchangeStatus=_nativeExchange && _nativeExchange->Status().length ? _nativeExchange->Status() : RemoteHostText(@"复制后跨端粘贴 · 文件按需传输");
    [_hostPanel updateState:state];
}
- (void)probeCursor {
    if (_forceVideoCursor) { _cursorAvailable = NO; }
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSCursor *cursor = NSCursor.currentSystemCursor;
#pragma clang diagnostic pop
        NSData *data = RemoteCursorImagePacket(cursor);
        _cursorAvailable = data != nil;
        if (_cursorAvailable && (!_previousCursor || ![_previousCursor isEqualToData:data])) {
            _previousCursor = data;
            if (_endpoint && _connection.load()==1) remote_send(_endpoint,REMOTE_CURSOR,(const uint8_t *)data.bytes,data.length);
        }
    }
    BOOL baked = !_cursorAvailable;
    if (!_cursorKnown || baked != _cursorBaked) {
        _cursorKnown = YES; _cursorBaked = baked;
        if (baked) {
            RemoteCursorHeader header={0,0,0,0,3,0};
            _previousCursor=[NSData dataWithBytes:&header length:sizeof(header)];
            if (_endpoint && _connection.load()==1) remote_send(_endpoint,REMOTE_CURSOR,(const uint8_t *)&header,sizeof(header));
            [self sendStatus:@"Global cursor shape unavailable; using video cursor compatibility mode"];
        } else [self sendStatus:@"Global cursor shape available; using local Windows cursor"];
        if (_configuration && _stream) {
            _configuration.showsCursor = baked;
            [_stream updateConfiguration:_configuration completionHandler:^(NSError *error){ if(error)Log(@"Cursor configuration error domain=%@ code=%ld",error.domain,(long)error.code); }];
        }
    } else if (baked && !_previousCursor) {
        RemoteCursorHeader header={0,0,0,0,3,0}; _previousCursor=[NSData dataWithBytes:&header length:sizeof(header)];
        if (_endpoint && _connection.load()==1)remote_send(_endpoint,REMOTE_CURSOR,(const uint8_t *)&header,sizeof(header));
    }
}
- (void)startCapture {
    if (_starting || _capturing || _stopping || !_running.load()) return;
    if (!_endpoint) { [self sendStatus:@"Transport unavailable; restart the host after fixing startup"]; return; }
    if (!CGPreflightScreenCaptureAccess()) { [self sendStatus:@"Screen Recording permission missing"]; return; }
    _starting=YES; _captureState=@"starting"; _captureStatus=@"正在启动画面采集…";
    uint64_t epoch = _captureEpoch.load();
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO completionHandler:^(SCShareableContent *content,NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if(epoch!=self->_captureEpoch.load() || !self->_running.load()) return;
            if(error || !content.displays.count) {
                [self captureFailed:error ?: [NSError errorWithDomain:SCStreamErrorDomain code:SCStreamErrorNoDisplayList userInfo:nil] stage:@"Screen source unavailable"]; return;
            }
            self->_display=content.displays.firstObject;
            for(SCDisplay *d in content.displays) if(d.displayID==CGMainDisplayID())self->_display=d;
            self->_width=(uint32_t)self->_display.width & ~1u; self->_height=(uint32_t)self->_display.height & ~1u;
            self->_displayBounds=CGDisplayBounds(self->_display.displayID);
            InitializeHardwareVideo();
            std::unique_lock<std::mutex> encoderLock(self->_encoderLock);
            RetireEncoder(self->_encoder,self->_encoderFlight);
            self->_encoderFlight=std::make_shared<EncoderFlightState>((__bridge void *)self);
            H264EncoderConfiguration encoder{self->_width,self->_height};
            OSStatus created=CreateStreamingEncoder(encoder,EncodedCallback,self->_encoderFlight.get(),
                &self->_encoder,self->_forceSoftwareCapture);
            self->_width=encoder.width;self->_height=encoder.height;self->_codedHeight=encoder.codedHeight;
            self->_layoutWorkaround=encoder.layoutWorkaround;self->_encoderFps=encoder.fps;
            self->_encoderMode.store(encoder.mode);self->_encoderID=self->_encoder?EncoderID(self->_encoder):@"";
            self->_consecutiveEncodeFailures.store(0);self->_encoderRecoveryScheduled.store(false);
            Log(@"VT H264 mode=%@ encoder=%@ visible=%ux%u coded-height=%u fps=%u status=%d",
                EncoderModeName(encoder.mode),self->_encoderID,self->_width,self->_height,self->_codedHeight,self->_encoderFps,(int)created);
            if(created!=noErr || !self->_encoder) { encoderLock.unlock(); self->_captureState=@"failed"; self->_captureStatus=@"H.264 硬件及 Apple 软件编码器均不可用，请查看诊断日志并重试画面采集。"; self->_captureBlocked=YES; [self stopCapture]; [self sendStatus:self->_captureStatus]; return; }
            encoderLock.unlock();
            self->_configuration=[SCStreamConfiguration new];
            self->_configuration.width=self->_width; self->_configuration.height=self->_height;
            self->_configuration.pixelFormat=kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
            self->_configuration.queueDepth=3; self->_configuration.minimumFrameInterval=CMTimeMake(1,self->_encoderFps);
            self->_configuration.showsCursor=self->_cursorBaked;
            self->_configuration.capturesAudio=NO;
            SCContentFilter *filter=[[SCContentFilter alloc] initWithDisplay:self->_display excludingWindows:@[]];
            self->_stream=[[SCStream alloc] initWithFilter:filter configuration:self->_configuration delegate:self];
            SCStream *newStream=self->_stream;
            { std::lock_guard captureLock(self->_captureLock);
              self->_captureSource.store((__bridge void *)newStream);
              self->_captureActive.store(false);
              self->_captureFrameState.store(SCFrameStatusStarted); }
            NSError *attachError=nil;
            if(![self->_stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:self->_captureQueue error:&attachError]) {
                [self captureFailed:attachError stage:@"Capture attachment failed"]; return;
            }
            [self->_stream startCaptureWithCompletionHandler:^(NSError *startError){
                dispatch_async(dispatch_get_main_queue(),^{
                    if(epoch!=self->_captureEpoch.load() || self->_captureSource.load()!=(__bridge void *)newStream || !self->_running.load()) return;
                    if(startError) { [self captureFailed:startError stage:@"Capture start error"]; return; }
                    self->_starting=NO; self->_capturing=YES; self->_captureBlocked=NO;
                    self->_captureRetryAt=0; self->_forceKeyframe.store(true);
                    [self captureFrameStatus:(SCFrameStatus)self->_captureFrameState.load() stream:newStream];
                    [self sendStatus:self->_captureStatus ?: [self encoderStreamingStatus]];
                });
            }];
        });
    }];
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if(type!=SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample))return;
    std::lock_guard captureLock(_captureLock);
    if(_captureSource.load()!=(__bridge void *)stream)return;
    SCFrameStatus status=SCFrameStatusComplete;
    CFArrayRef all=CMSampleBufferGetSampleAttachmentsArray(sample,false);
    if(all && CFArrayGetCount(all)) {
        NSDictionary *attachment=(__bridge NSDictionary *)CFArrayGetValueAtIndex(all,0);
        NSNumber *state=attachment[SCStreamFrameInfoStatus];
        if(state) status=(SCFrameStatus)state.integerValue;
    }
    int previous=_captureFrameState.exchange(status);
    if(status==SCFrameStatusSuspended || status==SCFrameStatusStopped || status==SCFrameStatusBlank) {
        _captureActive.store(false);
        if(previous!=status) _captureFrames.Reset(_frameEpoch.fetch_add(1)+1);
    } else if(status==SCFrameStatusComplete) _captureActive.store(true);
    if(previous!=status) dispatch_async(dispatch_get_main_queue(),^{ [self captureFrameStatus:status stream:stream]; });
    if(status!=SCFrameStatusComplete)return;
    CVPixelBufferRef pixel=CMSampleBufferGetImageBuffer(sample); if(!pixel)return;
    uint64_t epoch=_frameEpoch.load();
    double capturedSeconds=CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
    uint64_t capturedUs=std::isfinite(capturedSeconds)&&capturedSeconds>=0 ?
        (uint64_t)llround(capturedSeconds*1000000.0) : NowUs();
    bool replaced=false;
    if(_captureSource.load()==(__bridge void *)stream && _captureFrames.Publish(epoch,pixel,capturedUs,replaced) && replaced) _dropped.fetch_add(1);
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    dispatch_async(dispatch_get_main_queue(),^{
        if(self->_captureSource.load()!=(__bridge void *)stream) return;
        [self captureFailed:error stage:@"Capture stopped"];
    });
}
- (NSString *)encoderStreamingStatus {
    return _encoderMode.load() == H264EncoderMode::AppleSoftware ?
        @"正在通过 Apple 软件 H.264 编码传送画面（最高 1080p / 30 fps），CPU 负载较高。" :
        @"正在通过 H.264 硬件编码传送画面。";
}
- (void)encoderFailed:(OSStatus)status epoch:(uint64_t)epoch immediately:(BOOL)immediately {
    // One dropped frame is not an encoder failure. Repeated production failures
    // trigger a single lifecycle transition on the main queue, never in a VT callback.
    unsigned failures=_consecutiveEncodeFailures.fetch_add(1)+1;
    if ((!immediately && failures < 3) || _encoderRecoveryScheduled.exchange(true)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if(epoch!=self->_frameEpoch.load() || !self->_running.load()) { self->_encoderRecoveryScheduled.store(false); return; }
        H264EncoderMode mode=self->_encoderMode.load();
        Log(@"Repeated H264 encode failures mode=%@ status=%d",EncoderModeName(mode),(int)status);
        if(mode==H264EncoderMode::Hardware) {
            self->_forceSoftwareCapture=YES;self->_captureBlocked=NO;
            self->_captureState=@"recovering";self->_captureStatus=@"H.264 硬件编码中断，正在切换到 Apple 软件编码…";
            self->_restartAfterStop=YES;
            [self stopCapture];[self sendStatus:self->_captureStatus];
            if(!self->_stopping){self->_restartAfterStop=NO;[self startCapture];}
        } else if(mode==H264EncoderMode::AppleSoftware) {
            self->_captureState=@"failed";self->_captureStatus=@"Apple 软件 H.264 编码连续失败，请查看诊断日志并重试画面采集。";
            self->_captureBlocked=YES;self->_restartAfterStop=NO;
            [self stopCapture];[self sendStatus:self->_captureStatus];
        }
    });
}
- (void)stopCapture {
    if(_stopping) return;
    uint64_t epoch;
    { std::lock_guard captureLock(_captureLock); epoch=[self invalidateCaptureSourceLocked]; }
    // VT drain can synchronously invoke EncodedCallback. Never hold captureLock
    // while taking/draining the encoder; that callback needs the lifecycle gate.
    { std::lock_guard lock(_encoderLock);
      RetireEncoder(_encoder,_encoderFlight);
      _encoderMode.store(H264EncoderMode::Unavailable); _encoderID=@""; }
    SCStream *oldStream=_stream; _stream=nil; _configuration=nil;
    _stopping=oldStream!=nil;
    _captureStopDeadline=NowUs()+5000000;
    if(!oldStream)return;
    [oldStream stopCaptureWithCompletionHandler:^(NSError *error){
        if(error && !([error.domain isEqualToString:SCStreamErrorDomain] && error.code==SCStreamErrorAttemptToStopStreamState)) Log(@"Capture stop error domain=%@ code=%ld",error.domain,(long)error.code);
        dispatch_async(dispatch_get_main_queue(),^{
            if(epoch!=self->_captureEpoch.load())return;
            self->_stopping=NO;
            if(self->_restartAfterStop && self->_running.load()) { self->_restartAfterStop=NO; [self startCapture]; }
        });
    }];
}
- (uint64_t)invalidateCaptureSourceLocked {
    _capturing=NO; _starting=NO; _captureActive.store(false);
    uint64_t epoch=_captureEpoch.fetch_add(1)+1;
    _captureSource.store(nullptr);
    _captureFrameState.store(SCFrameStatusStopped);
    _captureFrames.Reset(_frameEpoch.fetch_add(1)+1);
    return epoch;
}
- (void)captureFailed:(NSError *)error stage:(NSString *)stage {
    Log(@"%@ error domain=%@ code=%ld",stage,error.domain,(long)error.code);
    CaptureFailure failure=CaptureFailurePolicy([error.domain isEqualToString:SCStreamErrorDomain],error.code);
    _restartAfterStop=NO;
    [self releaseAll]; [self stopCapture];
    _captureBlocked=failure!=CaptureFailure::Transient;
    if(failure==CaptureFailure::UserStopped) {
        _captureState=@"user-stopped";
        _captureStatus=@"共享已通过系统提示停止，请在 Mac 端点「重试画面采集」恢复。";
    } else if(failure==CaptureFailure::Permission) {
        _captureState=@"permission"; _captureStatus=@"屏幕录制权限不可用，请授权后重试画面采集。";
    } else if(failure==CaptureFailure::Permanent) {
        _captureState=@"failed"; _captureStatus=@"画面采集配置失败，请点重试画面采集。";
    } else {
        _captureState=@"recovering"; _captureStatus=@"画面采集暂时中断，正在自动恢复…";
        _captureRetryAt=NowUs()+CaptureRetryDelayUs(_captureRetryAttempt++);
    }
    [self sendStatus:_captureStatus];
}
- (void)captureFrameStatus:(SCFrameStatus)status stream:(SCStream *)stream {
    { std::lock_guard captureLock(_captureLock);
      if(_captureSource.load()!=(__bridge void *)stream)return;
      // Deferred main-queue notifications must describe the newest state of the
      // same stream, rather than restoring an older suspended/complete state.
      status=(SCFrameStatus)_captureFrameState.load(); }
    NSString *before=_captureState;
    if(status==SCFrameStatusSuspended) { _captureState=@"suspended"; _captureStatus=@"画面共享已暂停，可通过系统共享提示继续，或在 Mac 端点「重试画面采集」。"; }
    else if(status==SCFrameStatusStopped) { _captureState=@"stopped"; _captureStatus=@"画面共享已停止，请在 Mac 端点「重试画面采集」恢复。"; }
    else if(status==SCFrameStatusBlank) { _captureState=@"blank"; _captureStatus=@"Mac 显示器暂时没有画面，等待屏幕恢复。"; }
    else if(status==SCFrameStatusComplete || (status==SCFrameStatusIdle && _captureActive.load())) {
        _captureState=@"running"; _captureStatus=nil; _captureRetryAttempt=0;
        if(![before isEqualToString:@"running"]) _forceKeyframe.store(true);
    }
    if(![before isEqualToString:_captureState]) {
        if(!_captureActive.load()) [self releaseAll];
        [self sendStatus:_captureStatus ?: [self encoderStreamingStatus]];
    }
}
- (void)handleInputs:(std::vector<QueuedInput>)inputs {
    for(const QueuedInput& item:inputs) {
        const RemoteInputEvent& e=item.event;
        if(e.type==INPUT_PASTE) {
            // A clipboard ID is not a keyboard sequence. The sender released
            // its chord; native paste is one transaction after content readiness.
            if(_nativeExchange && _postAllowed && _captureActive.load())_nativeExchange->RequestPaste(e.sequence,e.flags);
            continue;
        }
        // Idempotent control requests are not ordered user input; sequence zero is valid.
        if(e.type==INPUT_KEYFRAME) { _forceKeyframe.store(true); continue; }
        if(e.type==INPUT_RELEASE_ALL) {
            // releaseAll also resets sequence tracking on disconnect. Preserve it for
            // a control-only release so stale ordinary input still stays discarded.
            bool hadSequence = _hasSequence;
            [self releaseAll]; _hasSequence = hadSequence; continue;
        }
        if(_hasSequence && e.sequence<=_lastSequence)continue;
        _hasSequence=true; _lastSequence=e.sequence;
        if(!_postAllowed || !_captureActive.load())continue;
        if(_nativeExchange && ((e.type==INPUT_BUTTON && (e.flags&1)) || (e.type==INPUT_KEY && (e.flags&1)) || e.type==INPUT_TEXT))
            _nativeExchange->CancelPaste(RemoteHostText(@"粘贴已取消：收到新的输入操作"));
        if(e.type==INPUT_MOVE || e.type==INPUT_BUTTON || e.type==INPUT_SCROLL) {
            if(!std::isfinite(e.x)||!std::isfinite(e.y)||e.x<0||e.x>1||e.y<0||e.y>1)continue;
            _lastPoint=CGPointMake(_displayBounds.origin.x+e.x*std::max(0.0,_displayBounds.size.width-1),
                                  _displayBounds.origin.y+e.y*std::max(0.0,_displayBounds.size.height-1));
        }
        CGEventRef event=nullptr;
        if(e.type==INPUT_MOVE) {
            CGEventType kind=_buttons[0]?kCGEventLeftMouseDragged:_buttons[1]?kCGEventRightMouseDragged:_buttons[2]?kCGEventOtherMouseDragged:kCGEventMouseMoved;
            CGMouseButton button=_buttons[1]?kCGMouseButtonRight:_buttons[2]?kCGMouseButtonCenter:kCGMouseButtonLeft;
            event=CGEventCreateMouseEvent(nullptr,kind,_lastPoint,button);
        } else if(e.type==INPUT_BUTTON && e.a>=1 && e.a<=3) {
            int button=e.a-1; bool down=e.flags&1;
            _buttons[button]=down;
            CGEventType kind=button==0?(down?kCGEventLeftMouseDown:kCGEventLeftMouseUp):button==1?(down?kCGEventRightMouseDown:kCGEventRightMouseUp):(down?kCGEventOtherMouseDown:kCGEventOtherMouseUp);
            event=CGEventCreateMouseEvent(nullptr,kind,_lastPoint,(CGMouseButton)button);
            // Windows sends a single down/up; derive double-click count from local event timing/position.
            static uint64_t clickTime=0; static CGPoint clickPoint={0,0}; static int clicks=0, previousButton=-1;
            if(down) { uint64_t now=NowUs(); double dx=_lastPoint.x-clickPoint.x,dy=_lastPoint.y-clickPoint.y;
                clicks=(previousButton==button && now-clickTime<(uint64_t)(NSEvent.doubleClickInterval*1000000.0) && dx*dx+dy*dy<25)?clicks+1:1;
                clicks=std::min(clicks,3); clickTime=now; clickPoint=_lastPoint; previousButton=button; }
            if(event)CGEventSetIntegerValueField(event,kCGMouseEventClickState,clicks);
        } else if(e.type==INPUT_SCROLL) {
            if(std::abs((int64_t)e.a)>1000 || std::abs((int64_t)e.b)>1000)continue;
            event=CGEventCreateScrollWheelEvent(nullptr,kCGScrollEventUnitLine,2,e.a,e.b);
            if(event)CGEventSetLocation(event,_lastPoint);
        } else if(e.type==INPUT_KEY) {
            int mac=MacKey(e.a,e.b,!!(e.flags&4),!!(e.flags&2)); if(mac==UINT16_MAX)continue;
            int identity=(e.a&0xffff)|((e.b&0xff)<<16)|((e.flags&4)?(1<<24):0);
            bool down=e.flags&1;
            if(down)_keys[identity]=mac; else { auto old=_keys.find(identity); if(old!=_keys.end()) { mac=old->second; _keys.erase(old); } }
            _modifiers=0; for(const auto& key:_keys)_modifiers|=ModifierFlag(key.second);
            event=CGEventCreateKeyboardEvent(nullptr,(CGKeyCode)mac,down);
            if(event && ModifierFlag(mac))CGEventSetType(event,kCGEventFlagsChanged);
        } else if(e.type==INPUT_TEXT) {
            NSString *text=[[NSString alloc] initWithBytes:item.text.data() length:item.text.size() encoding:NSUTF8StringEncoding];
            if(!text.length)continue;
            std::vector<UniChar> unicode(text.length); [text getCharacters:unicode.data() range:NSMakeRange(0,text.length)];
            for(bool down:{true,false}) { CGEventRef key=CGEventCreateKeyboardEvent(nullptr,0,down);
                if(key) { CGEventKeyboardSetUnicodeString(key,unicode.size(),unicode.data()); CGEventSetFlags(key,0); CGEventPost(kCGHIDEventTap,key); CFRelease(key); } }
            continue;
        }
        if(event) { CGEventSetFlags(event,_modifiers); CGEventPost(kCGHIDEventTap,event); CFRelease(event); }
    }
}
- (void)releaseAll {
    for(const auto& key:_keys) { CGEventRef event=CGEventCreateKeyboardEvent(nullptr,key.second,false);
        if(event) { if(ModifierFlag(key.second))CGEventSetType(event,kCGEventFlagsChanged);
            CGEventSetFlags(event,0); CGEventPost(kCGHIDEventTap,event); CFRelease(event); } }
    _keys.clear(); _modifiers=0;
    for(int i=0;i<3;++i)if(_buttons[i]) { CGEventType kind=i==0?kCGEventLeftMouseUp:i==1?kCGEventRightMouseUp:kCGEventOtherMouseUp;
        CGEventRef event=CGEventCreateMouseEvent(nullptr,kind,_lastPoint,(CGMouseButton)i);
        if(event) { CGEventSetFlags(event,0); CGEventPost(kCGHIDEventTap,event); CFRelease(event); } _buttons[i]=false; }
    _hasSequence=false;
}
- (void)toggleExchange:(id)sender {
    if(_nativeExchange) _nativeExchange->SetEnabled([sender state]==NSControlStateValueOn);
    [self tick:nil];
}
- (void)showTransfers:(id)sender { if(_nativeExchange)_nativeExchange->ShowTransfers(); }
- (void)performPaste:(uint32_t)flags {
    if(!_postAllowed || !_captureActive.load() || _connection.load()!=1)return;
    bool sequence=_hasSequence; [self releaseAll]; _hasSequence=sequence;
    CGEventFlags modifiers=kCGEventFlagMaskCommand | ((flags&16)?kCGEventFlagMaskShift:0);
    for(int index=0;index<4;++index) {
        CGKeyCode code=(index==0||index==3)?(CGKeyCode)kVK_Command:(CGKeyCode)kVK_ANSI_V;
        bool down=index<2;
        CGEventRef event=CGEventCreateKeyboardEvent(nullptr,code,down);
        if(!event)continue;
        if(code==kVK_Command)CGEventSetType(event,kCGEventFlagsChanged);
        CGEventSetFlags(event,index==3?0:modifiers); CGEventPost(kCGHIDEventTap,event); CFRelease(event);
    }
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    if (_showWindowObserver) [NSDistributedNotificationCenter.defaultCenter removeObserver:_showWindowObserver];
    [self releaseAll]; [_timer invalidate]; [self stopCapture]; _running.store(false);
    if(_networkThread.joinable())_networkThread.join();
    _restart.reset(); // Cancel any unapproved authorization before closing its transport.
    if(_bulkThread.joinable())_bulkThread.join();
    if(_encodeThread.joinable())_encodeThread.join();
    RetireEncoder(_encoder,_encoderFlight);
    _captureFrames.Reset(0);
    _nativeExchange.reset(); _exchange.reset();
    if(_endpoint) {remote_close(_endpoint);_endpoint=nullptr;}
    return NSTerminateNow;
}
@end

struct SelfFrame {
    int index;bool force;CVPixelBufferRef pixel;
    ~SelfFrame(){if(pixel)CVPixelBufferRelease(pixel);}
};
struct SelfSample { CMSampleBufferRef sample; int index; };
struct SelfTest {
    std::atomic<int> frames{0}, errors{0}, keyframes{0}, forcedIdr{0};
    std::atomic<uint64_t> bytes{0};
    std::mutex mutex;
    std::vector<SelfSample> samples;
    ~SelfTest() { for(const auto& sample:samples)CFRelease(sample.sample); }
};
static void PrintJson(NSDictionary *report) {
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
    fwrite(json.bytes,1,json.length,stdout);fputc('\n',stdout);fflush(stdout);
}
static void SelfTestCallback(void *context,void *source,OSStatus result,VTEncodeInfoFlags flags,CMSampleBufferRef sample) {
    SelfTest *test=(SelfTest *)context;
    std::unique_ptr<SelfFrame> frame((SelfFrame *)source);
    std::vector<uint8_t> bytes; bool idr=false;
    if(result!=noErr || !frame || !AnnexB(sample,bytes,idr)) {test->errors.fetch_add(1);return;}
    bool hasIdr=false,hasSps=false,hasPps=false;
    for(size_t i=0;i+4<bytes.size();++i) if(!bytes[i]&&!bytes[i+1]&&!bytes[i+2]&&bytes[i+3]==1) {
        int nal=bytes[i+4]&31;hasIdr|=nal==5;hasSps|=nal==7;hasPps|=nal==8;
    }
    if(idr!=hasIdr || (idr&&(!hasSps||!hasPps)))test->errors.fetch_add(1);
    if(frame->force) {
        if(idr&&hasIdr&&hasSps&&hasPps)test->forcedIdr.fetch_add(1);
        else test->errors.fetch_add(1);
    }
    test->frames.fetch_add(1);test->bytes.fetch_add(bytes.size());if(idr)test->keyframes.fetch_add(1);
    CFRetain(sample);
    std::lock_guard<std::mutex> lock(test->mutex);test->samples.push_back({sample,frame->index});
}
static const int TestY[8]={210,180,160,130,110,80,40,24};
static const int TestU[8]={128,40,160,60,196,90,224,128};
static const int TestV[8]={128,140,40,60,196,224,110,128};
static constexpr int CodecSelfTestFrames=30, CodecSelfTestMarkerStart=1244;
struct DecodeTest {
    std::atomic<int> frames{0},errors{0},badSamples{0},markerFailures{0};
    std::atomic<uint64_t> lumaError{0},chromaError{0},lumaSamples{0},chromaSamples{0};
    uint32_t expectedHeight=1080;
    uint32_t fps=60;
    std::atomic<int> denseChromaBad{0},denseChromaMax{0};
    std::atomic<uint64_t> denseChromaError{0},denseChromaSamples{0};
    struct Record { int sourceIndex,ptsIndex,markerLuma,strongestMarker,strongestLuma,bar0Luma,bar6Luma; };
    std::mutex mutex;
    std::vector<Record> records;
    std::vector<std::string> denseBadRecords;
};
static void CheckPixel(DecodeTest *test,uint8_t actual,int expected,bool chroma,int tolerance=12) {
    int error=std::abs((int)actual-expected);
    if(chroma) {test->chromaError.fetch_add(error);test->chromaSamples.fetch_add(1);}
    else {test->lumaError.fetch_add(error);test->lumaSamples.fetch_add(1);}
    if(error>tolerance)test->badSamples.fetch_add(1);
}
static void SelfDecodeCallback(void *context,void *source,OSStatus status,VTDecodeInfoFlags flags,
                               CVImageBufferRef image,CMTime pts,CMTime duration) {
    DecodeTest *test=(DecodeTest *)context;std::unique_ptr<int> frame((int *)source);
    if(status!=noErr || !image || !frame || CVPixelBufferGetWidth(image)!=1920 || CVPixelBufferGetHeight(image)!=test->expectedHeight ||
       CVPixelBufferGetPixelFormatType(image)!=kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || CVPixelBufferGetPlaneCount(image)!=2) {
        test->errors.fetch_add(1);return;
    }
    if(CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly)!=kCVReturnSuccess) {test->errors.fetch_add(1);return;}
    const uint8_t *y=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,0),*uv=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,1);
    size_t ys=CVPixelBufferGetBytesPerRowOfPlane(image,0),uvs=CVPixelBufferGetBytesPerRowOfPlane(image,1);
    int offset=(*frame%3)*12;
    for(int row:{192,540,900})for(int bar=0;bar<8;++bar) {
        int x=bar*240+120;
        CheckPixel(test,y[row*ys+x],TestY[bar]+offset,false);
        CheckPixel(test,uv[(row/2)*uvs+(x&~1)],TestU[bar],true);
        CheckPixel(test,uv[(row/2)*uvs+(x&~1)+1],TestV[bar],true);
    }
    // A white marker changes location every frame within the right-hand bars. This catches
    // repeated/stale pictures that a smooth average-luma comparison would miss.
    int marker=CodecSelfTestMarkerStart+6+*frame*20;
    int markerError=std::abs((int)y[48*ys+marker]-235);
    int strongest=-1,strongestLuma=-1;
    for(int i=0;i<CodecSelfTestFrames;++i) {int luma=y[48*ys+CodecSelfTestMarkerStart+6+i*20];if(luma>strongestLuma) {strongestLuma=luma;strongest=i;}}
    int ptsIndex=CMTIME_IS_NUMERIC(pts)?(int)CMTimeConvertScale(pts,test->fps,kCMTimeRoundingMethod_RoundHalfAwayFromZero).value:-1;
    {std::lock_guard<std::mutex> lock(test->mutex);test->records.push_back({*frame,ptsIndex,
        (int)y[48*ys+marker],strongest,strongestLuma,(int)y[540*ys+120],(int)y[540*ys+1560]});}
    if(markerError>20)test->markerFailures.fetch_add(1);
    CheckPixel(test,y[48*ys+marker],235,false,20);
    CheckPixel(test,uv[24*uvs+(marker&~1)],128,true,20);
    CheckPixel(test,uv[24*uvs+(marker&~1)+1],128,true,20);
    // Dense chroma grid and every final visible UV row catch partial last-tile
    // uploads. Preserve the original color and moving-marker checks above.
    auto chromaRow=[&](int row) {
        const int start=CodecSelfTestMarkerStart+*frame*20;
        for(int x=6;x<1918;x+=14) {
            if(row>=12&&row<36&&x>=start-8&&x<start+20)continue;
            const int expected[2]={TestU[x/240],TestV[x/240]};
            for(int c=0;c<2;++c) {
                int error=std::abs((int)uv[row*uvs+x+c]-expected[c]);
                test->denseChromaError.fetch_add(error);test->denseChromaSamples.fetch_add(1);
                int maximum=test->denseChromaMax.load();while(error>maximum&&!test->denseChromaMax.compare_exchange_weak(maximum,error)){}
                if(error>12){test->denseChromaBad.fetch_add(1);std::lock_guard<std::mutex> lock(test->mutex);
                    if(test->denseBadRecords.size()<16)test->denseBadRecords.push_back("frame="+std::to_string(*frame)+" uv="+std::to_string(x+c)+","+std::to_string(row)+" error="+std::to_string(error));}
            }
        }
    };
    for(int row=4;row<540;row+=13)chromaRow(row);
    for(int row=536;row<540;++row)chromaRow(row);
    CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);test->frames.fetch_add(1);
}
static NSDictionary *DecodeSelfSamples(SelfTest& encoded,uint32_t codedHeight=1080,uint32_t fps=60) {
    if(encoded.samples.empty())return @{@"pass":@NO,@"error":@"No encoded samples"};
    std::sort(encoded.samples.begin(),encoded.samples.end(),[](const SelfSample& a,const SelfSample& b){return a.index<b.index;});
    DecodeTest test;test.expectedHeight=codedHeight;test.fps=fps;VTDecompressionSessionRef decoder=nullptr;
    VTDecompressionOutputCallbackRecord callback={SelfDecodeCallback,&test};
    NSDictionary *spec=@{(__bridge NSString *)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder:@NO};
    NSDictionary *attrs=@{(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
    OSStatus created=VTDecompressionSessionCreate(kCFAllocatorDefault,CMSampleBufferGetFormatDescription(encoded.samples.front().sample),
        (__bridge CFDictionaryRef)spec,(__bridge CFDictionaryRef)attrs,&callback,&decoder);
    if(created!=noErr || !decoder)return @{@"pass":@NO,@"create_status":@(created)};
    CFTypeRef hardware=nullptr;
    OSStatus hardwareStatus=VTSessionCopyProperty(decoder,kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,nullptr,&hardware);
    bool hardwareUsed=hardware==kCFBooleanTrue;if(hardware)CFRelease(hardware);
    for(const SelfSample& sample:encoded.samples) {
        int *index=new int(sample.index);
        OSStatus result=VTDecompressionSessionDecodeFrame(decoder,sample.sample,kVTDecodeFrame_EnableAsynchronousDecompression,index,nullptr);
        if(result!=noErr) {delete index;test.errors.fetch_add(1);}
    }
    OSStatus finish=VTDecompressionSessionFinishDelayedFrames(decoder);
    OSStatus waited=VTDecompressionSessionWaitForAsynchronousFrames(decoder);
    VTDecompressionSessionInvalidate(decoder);CFRelease(decoder);
    double luma=test.lumaSamples.load()?(double)test.lumaError.load()/test.lumaSamples.load():999;
    double chroma=test.chromaSamples.load()?(double)test.chromaError.load()/test.chromaSamples.load():999;
    bool pass=test.frames.load()==CodecSelfTestFrames&&!test.errors.load()&&!test.badSamples.load()&&!test.markerFailures.load()&&!test.denseChromaBad.load()&&
        finish==noErr&&waited==noErr&&luma<12&&chroma<12&&!(hardwareStatus==noErr&&hardwareUsed);
    NSMutableArray *records=[NSMutableArray array];
    std::sort(test.records.begin(),test.records.end(),[](const auto& a,const auto& b){return a.sourceIndex<b.sourceIndex;});
    for(const auto& record:test.records)[records addObject:@{@"source_index":@(record.sourceIndex),@"pts_index":@(record.ptsIndex),
        @"expected_marker_luma":@235,@"marker_luma":@(record.markerLuma),@"observed_marker_index":@(record.strongestMarker),
        @"observed_marker_luma":@(record.strongestLuma),@"bar0_luma":@(record.bar0Luma),@"bar6_luma":@(record.bar6Luma)}];
    NSMutableArray *bad=[NSMutableArray array];for(const auto& s:test.denseBadRecords)[bad addObject:[NSString stringWithUTF8String:s.c_str()]];
    return @{@"pass":@(pass),@"decoder":@"Apple software H264",@"create_status":@(created),@"finish_status":@(finish),@"wait_status":@(waited),
        @"hardware_query_status":@(hardwareStatus),@"hardware_used":@(hardwareUsed),@"frames":@(test.frames.load()),@"errors":@(test.errors.load()),
        @"bad_samples":@(test.badSamples.load()),@"moving_marker_failures":@(test.markerFailures.load()),@"luma_mean_error":@(luma),@"chroma_mean_error":@(chroma),@"frame_samples":records,
        @"dense_chroma_bad":@(test.denseChromaBad.load()),@"dense_chroma_max_error":@(test.denseChromaMax.load()),
        @"dense_chroma_mean_error":@(test.denseChromaSamples.load()?(double)test.denseChromaError.load()/test.denseChromaSamples.load():999),
        @"dense_chroma_samples":@(test.denseChromaSamples.load()),@"dense_chroma_bad_coordinates":bad};
}
static bool ExportCodecTestFrames(SelfTest& test, NSString *path, uint32_t width, uint32_t height,
                                 uint32_t fps, uint64_t *bytesWritten, NSString **error) {
    static_assert(sizeof(RemoteFrameHeader) == 32);
    *bytesWritten = 0;
    if (test.samples.empty()) { *error = @"no_encoded_samples"; return false; }
    std::sort(test.samples.begin(), test.samples.end(), [](const SelfSample& a, const SelfSample& b) { return a.index < b.index; });
    NSMutableData *records = [NSMutableData data];
    for (const SelfSample& sample : test.samples) {
        std::vector<uint8_t> payload; bool idr = false;
        if (!AnnexB(sample.sample, payload, idr)) { *error = @"annex_b_conversion_failed"; return false; }
        const size_t recordSize = sizeof(RemoteFrameHeader) + payload.size();
        if (payload.empty() || recordSize > 8U * 1024 * 1024) { *error = @"record_length_out_of_range"; return false; }
        if (records.length + 4 + recordSize > 64ULL * 1024 * 1024) { *error = @"fixture_size_out_of_range"; return false; }
        const uint32_t length = (uint32_t)recordSize;
        const uint8_t lengthBytes[4] = {(uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16), (uint8_t)(length >> 24)};
        RemoteFrameHeader header = {0x31524652, width, height, idr ? 1u : 0u,
            (uint64_t)sample.index + 1, (uint64_t)sample.index * 1000000 / fps};
        // decode-smoke reads a little-endian record length, then the exact
        // production 32-byte header and Annex B payload. There is no file preamble.
        [records appendBytes:lengthBytes length:sizeof(lengthBytes)];
        [records appendBytes:&header length:sizeof(header)];
        [records appendBytes:payload.data() length:payload.size()];
    }
    // The existing private-file writer uses a 0600 temporary file, fsync and
    // atomic rename. Callers provide an existing private test directory.
    if (!WritePrivateData(path, records)) { *error = @"fixture_write_failed"; return false; }
    *bytesWritten = records.length;
    return true;
}
static int CodecSelfTest() {
    InitializeHardwareVideo();
    SelfTest test;VTCompressionSessionRef session=nullptr;
    bool ownershipPass=EncoderFlightSelfTest();
    NSMutableDictionary *report=[@{@"codec":@"h264",@"width":@1920,@"height":@1080,
        @"encoder_flight_ownership_pass":@(ownershipPass),
        @"pass":@NO,@"scope":@"30-frame production encoder selection/configuration, forced IDR, software-decoded NV12 color/position test; sustained streaming and latency require separate verification"} mutableCopy];
    const char *fixtureEnvironment = getenv("GLIDEBEAM_CODEC_TEST_FRAMES");
    NSString *fixturePath = fixtureEnvironment && *fixtureEnvironment ? [NSString stringWithUTF8String:fixtureEnvironment] : nil;
    if (fixtureEnvironment && *fixtureEnvironment) {
        report[@"fixture_written"] = @NO;
        report[@"fixture_path"] = fixturePath ?: @"";
        if (!fixturePath.length) { report[@"fixture_error"] = @"invalid_utf8_fixture_path"; PrintJson(report); return 5; }
    }
    NSDictionary *attrs=@{(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (__bridge NSString *)kCVPixelBufferWidthKey:@1920,(__bridge NSString *)kCVPixelBufferHeightKey:@1080,
        (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey:@{}};
    H264EncoderConfiguration encoder{1920,1080};
    OSStatus created=CreateStreamingEncoder(encoder,SelfTestCallback,&test,&session,ForceSoftwareH264());
    uint32_t codedHeight=encoder.codedHeight;bool layoutWorkaround=encoder.layoutWorkaround;
    report[@"coded_height"]=@(codedHeight);report[@"nvenc_layout_workaround"]=@(layoutWorkaround);
    report[@"encoder_mode"]=EncoderModeName(encoder.mode);report[@"fps"]=@(encoder.fps);
    report[@"require_hardware"]=@(encoder.mode==H264EncoderMode::Hardware);
    report[@"encoder_id"]=session?EncoderID(session):@"";
    report[@"create_status"]=@(created);Log(@"Self-test production H264 mode=%@ status=%d",EncoderModeName(encoder.mode),(int)created);
    if(created!=noErr || !session) {PrintJson(report);return 2;}
    report[@"prepare_status"]=@0; // The production creation helper already prepared and verified this session.
    CFTypeRef hardware=nullptr;
    OSStatus hardwareStatus=VTSessionCopyProperty(session,kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,nullptr,&hardware);
    bool hardwareUsed=hardware==kCFBooleanTrue;if(hardware)CFRelease(hardware);
    uint64_t encodeStarted=NowUs();
    bool singleInflightSoftwarePass=true;
    for(int frame=0;frame<CodecSelfTestFrames;++frame) {
        CVPixelBufferRef pixel=nullptr;
        OSStatus allocated=layoutWorkaround?CVPixelBufferCreate(kCFAllocatorDefault,1920,1080,kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,(__bridge CFDictionaryRef)attrs,&pixel):
            CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,VTCompressionSessionGetPixelBufferPool(session),&pixel);
        if(allocated!=noErr || !pixel) {test.errors.fetch_add(1);break;}
        CVPixelBufferLockBaseAddress(pixel,0);
        uint8_t *y=(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel,0),*uv=(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel,1);
        size_t ys=CVPixelBufferGetBytesPerRowOfPlane(pixel,0),uvs=CVPixelBufferGetBytesPerRowOfPlane(pixel,1);
        int offset=(frame%3)*12,marker=CodecSelfTestMarkerStart+frame*20;
        for(int row=0;row<1080;++row)for(int bar=0;bar<8;++bar)memset(y+row*ys+bar*240,TestY[bar]+offset,240);
        for(int row=0;row<540;++row)for(int col=0;col<1920;col+=2) {int bar=col/240;uv[row*uvs+col]=TestU[bar];uv[row*uvs+col+1]=TestV[bar];}
        for(int row=32;row<64;++row)memset(y+row*ys+marker,235,12);
        for(int row=16;row<32;++row)memset(uv+row*uvs+marker,128,12);
        CVPixelBufferUnlockBaseAddress(pixel,0);
        if(layoutWorkaround) {
            CVPixelBufferRef target=nullptr;CVReturn result=CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,VTCompressionSessionGetPixelBufferPool(session),&target);
            bool ok=result==kCVReturnSuccess&&target&&PrepackNV12(pixel,target,1920,1080,codedHeight);CVPixelBufferRelease(pixel);pixel=target;
            if(!ok){if(pixel)CVPixelBufferRelease(pixel);test.errors.fetch_add(1);break;}
        }
        bool force=frame==6;
        NSDictionary *options=force?@{(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame:@YES}:nil;
        auto meta=new SelfFrame{frame,force,pixel};
        OSStatus drainStatus=noErr;
        OSStatus result=SubmitH264Frame(session,encoder.mode,pixel,CMTimeMake(frame,encoder.fps),
            CMTimeMake(1,encoder.fps),(__bridge CFDictionaryRef)options,meta,&drainStatus);
        if(result!=noErr) {delete meta;test.errors.fetch_add(1);}
        if(encoder.mode==H264EncoderMode::AppleSoftware) {
            // This assertion precedes any final drain: a buffered first frame
            // would deadlock production's single-inflight gate and must fail here.
            if(result!=noErr || drainStatus!=noErr || test.frames.load()!=frame+1) {
                singleInflightSoftwarePass=false;test.errors.fetch_add(1);
                Log(@"Software single-inflight test failed frame=%d callbacks=%d drain-status=%d",
                    frame,test.frames.load(),(int)drainStatus);break;
            }
        } else if(VTCompressionSessionCompleteFrames(session,kCMTimeInvalid)!=noErr)test.errors.fetch_add(1);
    }
    OSStatus completed=VTCompressionSessionCompleteFrames(session,kCMTimeInvalid);
    double encodeElapsedMs=(NowUs()-encodeStarted)/1000.0;
    VTCompressionSessionInvalidate(session);CFRelease(session);
    NSDictionary *decoded=DecodeSelfSamples(test,codedHeight,encoder.fps);
    bool pass=ownershipPass&&test.frames.load()==CodecSelfTestFrames&&test.errors.load()==0&&test.keyframes.load()>=2&&test.forcedIdr.load()==1&&completed==noErr&&[decoded[@"pass"] boolValue];
    if (fixturePath.length) {
        uint64_t fixtureBytes = 0; NSString *fixtureError = nil;
        bool written = ExportCodecTestFrames(test, fixturePath, encoder.width, encoder.height, encoder.fps, &fixtureBytes, &fixtureError);
        report[@"fixture_written"] = @(written); report[@"fixture_bytes"] = @(fixtureBytes);
        report[@"fixture_records"] = @(written ? test.samples.size() : 0);
        if (!written) { report[@"fixture_error"] = fixtureError ?: @"fixture_export_failed"; pass = false; }
    }
    [report addEntriesFromDictionary:@{@"hardware_query_status":@(hardwareStatus),@"hardware_used":@(hardwareUsed),@"complete_status":@(completed),
        @"frames":@(test.frames.load()),@"errors":@(test.errors.load()),@"encode_elapsed_ms":@(encodeElapsedMs),@"keyframes":@(test.keyframes.load()),@"forced_idr_verified":@(test.forcedIdr.load()==1),
        @"software_single_inflight_pass":encoder.mode==H264EncoderMode::AppleSoftware?@(singleInflightSoftwarePass):[NSNull null],
        @"annex_b_bytes":@(test.bytes.load()),@"decoded_content":decoded,@"pass":@(pass)}];
    PrintJson(report);return pass?0:4;
}
static NSString *LoadToken(NSString *directory,NSString *provided) {
    if(provided.length) return ValidPairingToken(provided) ? provided : nil;
    const char *environment=getenv("REMOTE_PAIRING_TOKEN");
    if(environment&&*environment) { NSString *value=[NSString stringWithUTF8String:environment]; return ValidPairingToken(value) ? value : nil; }
    NSString *path=[directory stringByAppendingPathComponent:@"pairing-token"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
        NSString *existing=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
        if (!ValidPairingToken(existing)) { Log(@"Stored pairing token is invalid (contents withheld)"); return nil; }
        [[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:path error:nil];
        return existing;
    }
    uint8_t random[32];if(SecRandomCopyBytes(kSecRandomDefault,sizeof(random),random)!=errSecSuccess)return nil;
    NSMutableString *token=[NSMutableString stringWithCapacity:64];for(uint8_t byte:random)[token appendFormat:@"%02x",byte];
    if(!WritePrivateData(path,[token dataUsingEncoding:NSUTF8StringEncoding])) {Log(@"Private token file write failed");return nil;}
    Log(@"New pairing token stored (contents withheld)");return token;
}
static int AuthSettingsSelfTest(void) {
    NSString *directory = [NSTemporaryDirectory() stringByAppendingPathComponent:[@"remote-native-settings-test-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    BOOL created = [[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:nil];
    NSString *path = [directory stringByAppendingPathComponent:@"pairing-token"];
    NSString *custom = @" 自定义令牌🔒 ";
    NSString *maximum = [@"🔒" stringByPaddingToLength:256 withString:@"🔒" startingAtIndex:0];
    BOOL unicode = ValidPairingToken(@"ab🔒cde") && !ValidPairingToken(@"a🔒bcd") && ValidPairingToken(maximum) && !ValidPairingToken([maximum stringByAppendingString:@"a"]);
    BOOL controls = !ValidPairingToken(@"123456\n") && !ValidPairingToken(@"123456\u0085");
    BOOL saved = created && WritePrivateData(path, [custom dataUsingEncoding:NSUTF8StringEncoding]);
    NSString *loaded = LoadToken(directory, nil);
    BOOL preserved = saved && [custom isEqualToString:loaded] && ValidPairingToken(loaded);
    struct stat attributes = {};
    BOOL privateMode = stat(path.fileSystemRepresentation, &attributes) == 0 && (attributes.st_mode & 0777) == 0600;
    BOOL replacement = WritePrivateData(path, [@"replacement-token" dataUsingEncoding:NSUTF8StringEncoding]) && WritePrivateData(path, [custom dataUsingEncoding:NSUTF8StringEncoding]);
    BOOL rollback = replacement && [[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] isEqualToString:custom];
    BOOL cleanup = created && [[NSFileManager defaultManager] removeItemAtPath:directory error:nil];
    BOOL pass = unicode && controls && preserved && privateMode && rollback && cleanup;
    PrintJson(@{@"schema":@"remote.auth-settings-test.v1", @"unicode_scalar_limits":@(unicode), @"control_characters_rejected":@(controls),
        @"custom_token_exact_roundtrip":@(preserved), @"private_file_mode":@(privateMode), @"atomic_replace_and_rollback":@(rollback), @"temporary_cleanup":@(cleanup), @"pass":@(pass)});
    return pass ? 0 : 5;
}
int main(int argc,const char **argv) {
    remote_log::Initialize();
    struct LogLifetime { ~LogLifetime() { remote_log::Shutdown(); } } logLifetime;
    @autoreleasepool {
        RemoteHostLoadLanguagePreference();
        NSString *bind=@"0.0.0.0:47990",*token=nil,*identity=nil,*previewPath=nil;
        BOOL selfTest=NO,settingsTest=NO,permissions=NO,videoCursor=NO,background=NO,loginTest=NO,restartTest=NO,localizationTest=NO;
        NSInteger loginChange=-1;
        for(int i=1;i<argc;++i) {
            NSString *arg=[NSString stringWithUTF8String:argv[i]];
            if([arg isEqual:@"--self-test-codec"])selfTest=YES;
            else if([arg isEqual:@"--request-permissions"])permissions=YES;
            else if([arg isEqual:@"--video-cursor"])videoCursor=YES;
            else if([arg isEqual:@"--background"])background=YES;
            else if([arg isEqual:@"--enable-login-startup"])loginChange=1;
            else if([arg isEqual:@"--disable-login-startup"])loginChange=0;
            else if([arg isEqual:@"--self-test-login-startup"])loginTest=YES;
            else if([arg isEqual:@"--self-test-restart"])restartTest=YES;
            else if([arg isEqual:@"--self-test-localization"])localizationTest=YES;
            else if([arg isEqual:@"--language"] && i+1<argc) {
                NSString *tag=[NSString stringWithUTF8String:argv[++i]];
                if(![@[@"system",@"en",@"zh-Hans",@"zh-CN"] containsObject:tag]) { fprintf(stderr,"Unsupported UI language\n"); return 1; }
                remote_i18n::SetLanguage(remote_i18n::ParseLanguageTag(tag.UTF8String));
            }
            else if([arg isEqual:@"--bind"]&&i+1<argc)bind=[NSString stringWithUTF8String:argv[++i]];
            else if([arg isEqual:@"--token"]&&i+1<argc)token=[NSString stringWithUTF8String:argv[++i]];
            else if([arg isEqual:@"--identity-dir"]&&i+1<argc)identity=[NSString stringWithUTF8String:argv[++i]];
            else if([arg isEqual:@"--preview-ui"]&&i+1<argc)previewPath=[NSString stringWithUTF8String:argv[++i]];
            else if([arg isEqual:@"--self-test-auth-settings"])settingsTest=YES;
            else if([arg isEqual:@"--help"]) {
                puts("GlideBeam Mac host (RemoteHost) [--language system|en|zh-Hans] [--self-test-localization] [--bind 0.0.0.0:47990] [--identity-dir PATH] [--request-permissions] [--video-cursor] [--background] [--enable-login-startup|--disable-login-startup] [--self-test-codec] [--self-test-login-startup] [--self-test-restart] [--preview-ui PNG]\nPairing token: REMOTE_PAIRING_TOKEN environment or private Application Support/pairing-token. --token is also supported, but environment/file avoids process-list disclosure.");return 0;
            } else {fprintf(stderr,"Unknown/missing argument: %s\n",argv[i]);return 1;}
        }
        if(previewPath) {
            if(RemoteHostRenderPreview(previewPath)) return 0;
            fprintf(stderr,"UI preview rendering/write failed\n"); return 1;
        }
        if(localizationTest)return RemoteHostLocalizationSelfTest() ? 0 : 1;
        if(selfTest)return CodecSelfTest();
        if(restartTest)return RemoteMacRestart::SelfTest();
        if(settingsTest)return AuthSettingsSelfTest();
        if(loginTest)return RemoteLoginSelfTest();
        NSString *support=[NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,NSUserDomainMask,YES).firstObject stringByAppendingPathComponent:@"RemoteNative"];
        NSError *error=nil;
        if(![[NSFileManager defaultManager] createDirectoryAtPath:support withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:&error]) {Log(@"Application Support failed domain=%@ code=%ld",error.domain,(long)error.code);return 1;}
        if(loginChange!=-1) {
            NSString *message=nil;
            if(!RemoteLoginSetEnabled(loginChange==1,&message)) { Log(@"%@",message);return 1; }
            puts(loginChange==1 ? "Login startup enabled" : "Login startup disabled");return 0;
        }
        NSString *lockPath=[support stringByAppendingPathComponent:@"host-instance.lock"];
        int instanceFd=open(lockPath.fileSystemRepresentation,O_CREAT|O_RDWR|O_CLOEXEC,0600);
        if(instanceFd<0) { Log(@"Cannot open host instance lock: %s",strerror(errno));return 1; }
        if(flock(instanceFd,LOCK_EX|LOCK_NB)!=0) {
            int failure=errno;close(instanceFd);
            if(failure==EWOULDBLOCK||failure==EAGAIN) {
                if(!background) [NSDistributedNotificationCenter.defaultCenter postNotificationName:@"dev.remote-native.host.show-window" object:nil userInfo:nil deliverImmediately:YES];
                puts("Existing Agent instance reused");return 0;
            }
            Log(@"Cannot acquire host instance lock: %s",strerror(failure));return 1;
        }
        if(!identity)identity=[support stringByAppendingPathComponent:@"identity"];
        if(![[NSFileManager defaultManager] createDirectoryAtPath:identity withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:&error]) {Log(@"Identity directory failed domain=%@ code=%ld",error.domain,(long)error.code);return 1;}
        token=LoadToken(support,token);if(!token){close(instanceFd);return 1;}
        NSApplication *app=[NSApplication sharedApplication];[app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        RemoteHost *host=[[RemoteHost alloc] initWithBind:bind token:token identity:identity videoCursor:videoCursor requestPermissions:permissions background:background];
        app.delegate=host;[app run];close(instanceFd);return 0;
    }
}
