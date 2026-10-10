// Runs the production lifecycle methods with owned synthetic SCStream samples.
// It neither starts a desktop capture nor reads a pairing token/identity.
#define main RemoteAgentMain
#include "../main.mm"
#undef main
#include <condition_variable>

static void DrainRecoveryCallbacks() {
    NSDate *until=[NSDate dateWithTimeIntervalSinceNow:0.06];
    while(until.timeIntervalSinceNow>0) [NSRunLoop.mainRunLoop runMode:NSDefaultRunLoopMode beforeDate:until];
}
static CMSampleBufferRef RecoverySample(CVPixelBufferRef pixel,SCFrameStatus state) {
    CMVideoFormatDescriptionRef format=nullptr;
    if(CMVideoFormatDescriptionCreateForImageBuffer(nullptr,pixel,&format)!=noErr)return nullptr;
    CMSampleTimingInfo timing={CMTimeMake(1,60),CMTimeMake(1,60),kCMTimeInvalid};
    CMSampleBufferRef sample=nullptr;
    OSStatus result=CMSampleBufferCreateReadyWithImageBuffer(nullptr,pixel,format,&timing,&sample);
    CFRelease(format);
    if(result!=noErr)return nullptr;
    CFArrayRef attachments=CMSampleBufferGetSampleAttachmentsArray(sample,true);
    CFMutableDictionaryRef attachment=(CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments,0);
    CFDictionarySetValue(attachment,(__bridge const void *)SCStreamFrameInfoStatus,(__bridge const void *)@(state));
    return sample;
}
int main(int argc,const char **argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSMutableArray *checks=[NSMutableArray new];
        auto check=[&](NSString *name,bool pass){[checks addObject:@{@"name":name,@"passed":@(pass)}];};
        check(@"user_stop_is_explicit",CaptureFailurePolicy(true,SCStreamErrorUserStopped)==CaptureFailure::UserStopped);
        check(@"permission_denial_requires_user",CaptureFailurePolicy(true,SCStreamErrorUserDeclined)==CaptureFailure::Permission);
        check(@"missing_entitlement_requires_user",CaptureFailurePolicy(true,SCStreamErrorMissingEntitlements)==CaptureFailure::Permission);
        check(@"system_stop_is_transient",CaptureFailurePolicy(true,SCStreamErrorSystemStoppedStream)==CaptureFailure::Transient);
        check(@"wrong_domain_is_not_user_stop",CaptureFailurePolicy(false,SCStreamErrorUserStopped)==CaptureFailure::Transient);
        check(@"bounded_retry_backoff",CaptureRetryDelayUs(0)==1000000 && CaptureRetryDelayUs(1)==2000000 && CaptureRetryDelayUs(90)==16000000);
        RemoteHost *host=[[RemoteHost alloc] initWithBind:@"127.0.0.1:0" token:@"fixture-token" identity:@"" videoCursor:NO requestPermissions:NO background:YES];
        NSObject *source=[NSObject new], *replacement=[NSObject new];
        CVPixelBufferRef pixel=nullptr;
        CVReturn allocation=CVPixelBufferCreate(nullptr,64,64,kCVPixelFormatType_32BGRA,nullptr,&pixel);
        check(@"owned_pixel_buffer_allocated",allocation==kCVReturnSuccess && pixel);
        if(!pixel)return 2;
        host->_captureSource.store((__bridge void *)source);
        host->_capturing=YES;
        CMSampleBufferRef complete=RecoverySample(pixel,SCFrameStatusComplete);
        check(@"owned_sample_created",complete && CMSampleBufferIsValid(complete));
        if(!complete){CVPixelBufferRelease(pixel);return 2;}
        [host stream:(SCStream *)source didOutputSampleBuffer:complete ofType:SCStreamOutputTypeScreen];
        DrainRecoveryCallbacks();
        uint64_t capturedUs=0;
        CVPixelBufferRef taken=host->_captureFrames.Take(host->_frameEpoch.load(),false,capturedUs);
        check(@"complete_frame_published",taken==pixel && capturedUs>0 && host->_captureActive.load());
        if(taken)CVPixelBufferRelease(taken);
        taken=host->_captureFrames.Take(host->_frameEpoch.load(),false,capturedUs);
        check(@"unchanged_frame_not_reencoded",taken==nullptr);
        if(taken)CVPixelBufferRelease(taken);
        taken=host->_captureFrames.Take(host->_frameEpoch.load(),true,capturedUs);
        check(@"active_static_frame_can_supply_idr",taken==pixel);
        if(taken)CVPixelBufferRelease(taken);
        uint64_t oldEpoch=host->_frameEpoch.load();
        CMSampleBufferRef suspended=RecoverySample(pixel,SCFrameStatusSuspended);
        [host stream:(SCStream *)source didOutputSampleBuffer:suspended ofType:SCStreamOutputTypeScreen];
        DrainRecoveryCallbacks();
        taken=host->_captureFrames.Take(host->_frameEpoch.load(),true,capturedUs);
        check(@"suspension_discards_old_idr_source",!taken && !host->_captureActive.load() && [host->_captureState isEqualToString:@"suspended"]);
        if(taken)CVPixelBufferRelease(taken);
        bool replaced=false;
        check(@"queued_old_publication_rejected",!host->_captureFrames.Publish(oldEpoch,pixel,1,replaced));
        [host stream:(SCStream *)source didOutputSampleBuffer:complete ofType:SCStreamOutputTypeScreen];
        DrainRecoveryCallbacks();
        check(@"same_stream_resume_recovers",host->_captureActive.load() && [host->_captureState isEqualToString:@"running"]);
        uint64_t lifecycle=host->_captureEpoch.load();
        [host stopCapture];
        taken=host->_captureFrames.Take(host->_frameEpoch.load(),true,capturedUs);
        check(@"stop_invalidates_lifecycle_and_pixels",!taken && host->_captureEpoch.load()>lifecycle && !host->_captureActive.load() && !host->_capturing && !host->_starting);
        if(taken)CVPixelBufferRelease(taken);
        [host stream:(SCStream *)source didOutputSampleBuffer:complete ofType:SCStreamOutputTypeScreen];
        taken=host->_captureFrames.Take(host->_frameEpoch.load(),true,capturedUs);
        check(@"late_frame_from_old_stream_ignored",!taken);
        if(taken)CVPixelBufferRelease(taken);
        // Hold the production lifecycle gate while an old Complete callback
        // attempts entry. Perform the exact production invalidation under the
        // gate and install a suspended replacement before releasing it.
        std::mutex rendezvous;
        std::condition_variable ready;
        bool entered=false;
        std::unique_lock gate(host->_captureLock);
        host->_captureSource.store((__bridge void *)source);
        host->_captureActive.store(true);
        host->_captureFrameState.store(SCFrameStatusComplete);
        std::thread delayed([&]{
            @autoreleasepool {
                {std::lock_guard lock(rendezvous);entered=true;} ready.notify_one();
                [host stream:(SCStream *)source didOutputSampleBuffer:complete ofType:SCStreamOutputTypeScreen];
            }
        });
        {std::unique_lock lock(rendezvous);ready.wait(lock,[&]{return entered;});}
        [host invalidateCaptureSourceLocked];
        host->_captureSource.store((__bridge void *)replacement);
        host->_captureFrameState.store(SCFrameStatusSuspended);
        gate.unlock();
        delayed.join();
        DrainRecoveryCallbacks();
        check(@"contended_old_complete_cannot_reactivate",!host->_captureActive.load());
        check(@"contended_old_complete_cannot_poison_state",host->_captureFrameState.load()==SCFrameStatusSuspended && host->_captureSource.load()==(__bridge void *)replacement);
        taken=host->_captureFrames.Take(host->_frameEpoch.load(),true,capturedUs);
        check(@"contended_old_complete_cannot_publish_pixels",!taken);
        if(taken)CVPixelBufferRelease(taken);
        host->_captureSource.store((__bridge void *)replacement);
        host->_capturing=YES;
        [host stream:(SCStream *)replacement didOutputSampleBuffer:complete ofType:SCStreamOutputTypeScreen];
        DrainRecoveryCallbacks();
        check(@"replacement_stream_accepts_new_frame",host->_captureActive.load());
        [host stream:(SCStream *)source didStopWithError:[NSError errorWithDomain:SCStreamErrorDomain code:SCStreamErrorUserStopped userInfo:nil]];
        DrainRecoveryCallbacks();
        check(@"late_stop_from_old_stream_ignored",host->_captureActive.load() && !host->_captureBlocked);
        [host captureFailed:[NSError errorWithDomain:SCStreamErrorDomain code:SCStreamErrorSystemStoppedStream userInfo:nil] stage:@"Synthetic system stop"];
        check(@"transient_failure_has_automatic_retry",!host->_captureBlocked && host->_captureRetryAt>NowUs() && [host->_captureState isEqualToString:@"recovering"] && !host->_captureActive.load());
        [host captureFailed:[NSError errorWithDomain:SCStreamErrorDomain code:SCStreamErrorUserStopped userInfo:nil] stage:@"Synthetic user stop"];
        check(@"user_stop_does_not_auto_restart",host->_captureBlocked && [host->_captureState isEqualToString:@"user-stopped"]);
        taken=host->_captureFrames.Take(host->_frameEpoch.load(),true,capturedUs);
        check(@"reconnect_cannot_reuse_stopped_frame",!taken);
        if(taken)CVPixelBufferRelease(taken);
        // The retry path is production code. A missing endpoint prevents starting
        // any real stream while still proving a blocked/stuck lifecycle is reset.
        host->_starting=YES; lifecycle=host->_captureEpoch.load();
        [host restartCapture];
        check(@"authorized_restart_resets_stuck_start",!host->_captureBlocked && !host->_starting && host->_captureEpoch.load()>lifecycle);
        host->_captureBlocked=YES;host->_stopping=YES;host->_captureStopDeadline=NowUs()-1;
        lifecycle=host->_captureEpoch.load();
        [host tick:nil];
        check(@"hung_stop_completion_is_bounded",!host->_stopping && host->_captureEpoch.load()>lifecycle);
        host->_running.store(false);
        CFRelease(suspended);CFRelease(complete);CVPixelBufferRelease(pixel);
        bool passed=true;unsigned passedCount=0,skippedCount=0;
        for(NSDictionary *c in checks) {if([c[@"skipped"] boolValue]){++skippedCount;continue;}if([c[@"passed"] boolValue])++passedCount;else passed=false;}
        NSDictionary *report=@{@"passed":@(passed),@"passed_count":@(passedCount),@"skipped_count":@(skippedCount),@"scope":@"Production capture lifecycle with owned synthetic CMSampleBuffer; no desktop capture or system UI clicks",@"checks":checks};
        NSData *data=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
        if(argc>1) [data writeToFile:[NSString stringWithUTF8String:argv[1]] atomically:YES];
        printf("%s\n",[[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] UTF8String]);
        return passed?0:1;
    }
}
