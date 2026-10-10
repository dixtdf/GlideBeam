#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#include <mach-o/dyld.h>
#include <atomic>
#include <cstring>

// Independent bootstrap probe. This intentionally does not modify the official
// bundle and never calls VTCopySupportedPropertyDictionaryForEncoder.
static NSArray *LoadedCodecs() {
    NSMutableArray *paths=[NSMutableArray array];
    for(uint32_t i=0;i<_dyld_image_count();++i) {
        const char *path=_dyld_get_image_name(i);if(!path)continue;
        NSString *name=[NSString stringWithUTF8String:path];
        NSString *lower=name.lowercaseString;
        if([lower containsString:@"nvenc"]||[lower containsString:@"nvdec"]||[lower containsString:@"nvrm"]) [paths addObject:name];
    }
    return paths;
}
struct ProbeState {std::atomic<int> callbacks{0},errors{0};std::atomic<uint64_t> bytes{0};};
static void Encoded(void *context,void *,OSStatus status,VTEncodeInfoFlags flags,CMSampleBufferRef sample) {
    ProbeState *state=(ProbeState *)context;
    if(status||!sample) {state->errors.fetch_add(1);return;}
    state->callbacks.fetch_add(1);
    CMBlockBufferRef block=CMSampleBufferGetDataBuffer(sample);
    if(block)state->bytes.fetch_add(CMBlockBufferGetDataLength(block));
}
int main(int argc,const char **argv) {
    @autoreleasepool {
        NSString *mode=argc>1?[NSString stringWithUTF8String:argv[1]]:@"none";
        NSMutableDictionary *report=[@{@"mode":mode,@"loaded_before":LoadedCodecs()} mutableCopy];
        if([mode isEqual:@"list"]) {
            CFArrayRef list=nullptr;OSStatus result=VTCopyVideoEncoderList(nullptr,&list);
            report[@"encoder_list_status"]=@(result);report[@"encoder_count"]=@(list?CFArrayGetCount(list):0);
            if(list)CFRelease(list);
        } else if([mode isEqual:@"metal"]) {
            id<MTLDevice> device=MTLCreateSystemDefaultDevice();
            report[@"metal_device"]=device.name?:@"unavailable";
        } else if([mode isEqual:@"decode"]) {
            report[@"hardware_h264_decode_supported"]=@(VTIsHardwareDecodeSupported(kCMVideoCodecType_H264));
        } else if(![mode isEqual:@"none"]) {
            fprintf(stderr,"Usage: vt-discovery-probe [none|list|metal|decode]\n");return 1;
        }
        report[@"loaded_after_discovery"]=LoadedCodecs();
        NSDictionary *spec=@{(__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder:@YES};
        NSDictionary *attrs=@{(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            (__bridge NSString *)kCVPixelBufferWidthKey:@1920,(__bridge NSString *)kCVPixelBufferHeightKey:@1080,
            (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey:@{}};
        ProbeState state;VTCompressionSessionRef session=nullptr;
        OSStatus result=VTCompressionSessionCreate(kCFAllocatorDefault,1920,1080,kCMVideoCodecType_H264,
            (__bridge CFDictionaryRef)spec,(__bridge CFDictionaryRef)attrs,nullptr,Encoded,&state,&session);
        report[@"create_status"]=@(result);report[@"loaded_after_create"]=LoadedCodecs();
        if(!result&&session) {
            VTSessionSetProperty(session,kVTCompressionPropertyKey_RealTime,kCFBooleanTrue);
            VTSessionSetProperty(session,kVTCompressionPropertyKey_AllowFrameReordering,kCFBooleanFalse);
            OSStatus prepare=VTCompressionSessionPrepareToEncodeFrames(session);
            report[@"prepare_status"]=@(prepare);
            CVPixelBufferRef pixel=nullptr;
            OSStatus allocate=CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,VTCompressionSessionGetPixelBufferPool(session),&pixel);
            report[@"allocate_status"]=@(allocate);
            if(!prepare&&!allocate&&pixel) {
                CVPixelBufferLockBaseAddress(pixel,0);
                uint8_t *y=(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel,0),*uv=(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel,1);
                size_t ys=CVPixelBufferGetBytesPerRowOfPlane(pixel,0),uvs=CVPixelBufferGetBytesPerRowOfPlane(pixel,1);
                for(int row=0;row<1080;++row)memset(y+row*ys,90,1920);
                for(int row=0;row<540;++row)memset(uv+row*uvs,128,1920);
                CVPixelBufferUnlockBaseAddress(pixel,0);
                report[@"encode_status"]=@(VTCompressionSessionEncodeFrame(session,pixel,CMTimeMake(0,60),CMTimeMake(1,60),nullptr,nullptr,nullptr));
            }
            if(pixel)CVPixelBufferRelease(pixel);
            report[@"complete_status"]=@(VTCompressionSessionCompleteFrames(session,kCMTimeInvalid));
            VTCompressionSessionInvalidate(session);CFRelease(session);
        }
        report[@"callbacks"]=@(state.callbacks.load());report[@"errors"]=@(state.errors.load());report[@"encoded_bytes"]=@(state.bytes.load());
        NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
        fwrite(json.bytes,1,json.length,stdout);fputc('\n',stdout);
        return !result&&state.callbacks.load()==1&&!state.errors.load()?0:2;
    }
}
