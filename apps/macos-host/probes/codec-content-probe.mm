#define main RemoteHostMainUnusedByProbe
#include "../main.mm"
#undef main
#include "../nv12-layout.hpp"

// Independent real-machine acceptance probe. The official host remains running.
struct ContentProbeDecoder {
    DecodeTest metrics;
    bool gray=false,padded=false;
    std::atomic<int> denseChromaBad{0};
    std::atomic<uint64_t> denseChromaError{0},denseChromaSamples{0};
    std::vector<std::string> chromaBadRecords;
    std::mutex mutex;
    std::vector<std::pair<int,std::string>> scans;
};
static void DenseDecoded(void *context,void *source,OSStatus status,VTDecodeInfoFlags flags,
                         CVImageBufferRef image,CMTime pts,CMTime duration) {
    auto *test=(ContentProbeDecoder *)context;
    int index=source?*(int *)source:-1;
    if(!status&&image&&CVPixelBufferGetPlaneCount(image)==2&&CVPixelBufferGetWidth(image)>=1720&&CVPixelBufferGetHeight(image)>=57) {
        if(CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly)==kCVReturnSuccess) {
            auto *y=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,0);
            size_t stride=CVPixelBufferGetBytesPerRowOfPlane(image,0);
            std::string scan="frame="+std::to_string(index)+" pts="+std::to_string(CMTimeConvertScale(pts,60,kCMTimeRoundingMethod_RoundHalfAwayFromZero).value);
            for(int row:{40,48,52,56}) {
                scan+=" y"+std::to_string(row)+":";
                int max=0,peak=-1;
                for(int x=1400;x<1720;++x)if(y[row*stride+x]>max){max=y[row*stride+x];peak=x;}
                scan+="peak="+std::to_string(peak)+"/"+std::to_string(max)+" bright=";
                bool any=false;
                for(int x=1400;x<1720;) {
                    if(y[row*stride+x]<=180){++x;continue;}
                    int start=x;while(x<1720&&y[row*stride+x]>180)++x;
                    if(any)scan+=",";scan+=std::to_string(start)+"-"+std::to_string(x-1);any=true;
                }
                if(!any)scan+="none";
            }
            {std::lock_guard<std::mutex> lock(test->mutex);test->scans.emplace_back(index,scan);}
            const uint8_t *uv=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,1);
            size_t uvStride=CVPixelBufferGetBytesPerRowOfPlane(image,1);
            if(index>=0&&index<12&&CVPixelBufferGetHeight(image)>=1080) {
                int marker=1444+index*20;
                for(int row=4;row<540;row+=13)for(int x=6;x<1918;x+=14) {
                    if(row>=12&&row<36&&x>=marker-8&&x<marker+20)continue;
                    int expected[2]={test->gray?128:TestU[x/240],test->gray?128:TestV[x/240]};
                    for(int c=0;c<2;++c){int difference=std::abs((int)uv[row*uvStride+x+c]-expected[c]);
                        test->denseChromaError.fetch_add(difference);test->denseChromaSamples.fetch_add(1);
                        if(difference>12){test->denseChromaBad.fetch_add(1);
                            std::lock_guard<std::mutex> lock(test->mutex);
                            if(test->chromaBadRecords.size()<48)test->chromaBadRecords.push_back("frame="+std::to_string(index)+" uv="+std::to_string(x+c)+","+std::to_string(row)+" actual="+std::to_string(uv[row*uvStride+x+c])+" expected="+std::to_string(expected[c]));}}
                }
            }
            CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
        }
    }
    if(!test->gray&&!test->padded) {SelfDecodeCallback(&test->metrics,source,status,flags,image,pts,duration);return;}
    std::unique_ptr<int> frame((int *)source);
    DecodeTest *metrics=&test->metrics;
    if(status||!image||!frame||CVPixelBufferGetWidth(image)!=1920||CVPixelBufferGetHeight(image)!=(test->padded?1088:1080)||
       CVPixelBufferGetPixelFormatType(image)!=kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange||CVPixelBufferGetPlaneCount(image)!=2) {metrics->errors.fetch_add(1);return;}
    if(CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly)!=kCVReturnSuccess){metrics->errors.fetch_add(1);return;}
    const uint8_t *y=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,0),*uv=(const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image,1);
    size_t ys=CVPixelBufferGetBytesPerRowOfPlane(image,0),uvs=CVPixelBufferGetBytesPerRowOfPlane(image,1);
    for(int row:{192,540,900})for(int bar=0;bar<8;++bar){int x=bar*240+120;
        CheckPixel(metrics,y[row*ys+x],TestY[bar]+(*frame%3)*12,false);
        CheckPixel(metrics,uv[row/2*uvs+x],test->gray?128:TestU[bar],true);CheckPixel(metrics,uv[row/2*uvs+x+1],test->gray?128:TestV[bar],true);}
    int marker=1450+*frame*20;
    if(std::abs((int)y[48*ys+marker]-235)>20)metrics->markerFailures.fetch_add(1);
    CheckPixel(metrics,y[48*ys+marker],235,false,20);
    CheckPixel(metrics,uv[24*uvs+marker],128,true,20);CheckPixel(metrics,uv[24*uvs+marker+1],128,true,20);
    CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);metrics->frames.fetch_add(1);
}
static NSDictionary *ProbeDecode(SelfTest& encoded,bool gray=false,bool padded=false) {
    if(encoded.samples.empty())return @{@"pass":@NO,@"error":@"No encoded samples"};
    std::sort(encoded.samples.begin(),encoded.samples.end(),[](const auto& a,const auto& b){return a.index<b.index;});
    ContentProbeDecoder test;test.gray=gray;test.padded=padded;VTDecompressionSessionRef decoder=nullptr;
    VTDecompressionOutputCallbackRecord callback={DenseDecoded,&test};
    NSDictionary *spec=@{(__bridge NSString *)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder:@NO};
    NSDictionary *attrs=@{(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
    OSStatus create=VTDecompressionSessionCreate(kCFAllocatorDefault,CMSampleBufferGetFormatDescription(encoded.samples.front().sample),
        (__bridge CFDictionaryRef)spec,(__bridge CFDictionaryRef)attrs,&callback,&decoder);
    if(create||!decoder)return @{@"pass":@NO,@"create_status":@(create)};
    for(const auto& sample:encoded.samples) {
        int *frame=new int(sample.index);
        OSStatus result=VTDecompressionSessionDecodeFrame(decoder,sample.sample,kVTDecodeFrame_EnableAsynchronousDecompression,frame,nullptr);
        if(result){delete frame;test.metrics.errors.fetch_add(1);}
    }
    OSStatus finish=VTDecompressionSessionFinishDelayedFrames(decoder),wait=VTDecompressionSessionWaitForAsynchronousFrames(decoder);
    VTDecompressionSessionInvalidate(decoder);CFRelease(decoder);
    double y=test.metrics.lumaSamples.load()?(double)test.metrics.lumaError.load()/test.metrics.lumaSamples.load():999;
    double uv=test.metrics.chromaSamples.load()?(double)test.metrics.chromaError.load()/test.metrics.chromaSamples.load():999;
    bool pass=!finish&&!wait&&test.metrics.frames.load()==12&&!test.metrics.errors.load()&&!test.metrics.badSamples.load()&&!test.metrics.markerFailures.load()&&!test.denseChromaBad.load();
    std::sort(test.scans.begin(),test.scans.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    NSMutableArray *scans=[NSMutableArray array];
    NSMutableArray *chromaBad=[NSMutableArray array];for(const auto& record:test.chromaBadRecords)[chromaBad addObject:[NSString stringWithUTF8String:record.c_str()]];
    for(const auto& row:test.scans)[scans addObject:[NSString stringWithUTF8String:row.second.c_str()]];
    return @{@"pass":@(pass),@"frames":@(test.metrics.frames.load()),@"errors":@(test.metrics.errors.load()),
        @"bad_samples":@(test.metrics.badSamples.load()),@"marker_failures":@(test.metrics.markerFailures.load()),
        @"luma_mae":@(y),@"chroma_mae":@(uv),@"finish_status":@(finish),@"wait_status":@(wait),@"dense_y_scans":scans,
        @"dense_chroma_bad":@(test.denseChromaBad.load()),@"dense_chroma_mae":@(test.denseChromaSamples.load()?(double)test.denseChromaError.load()/test.denseChromaSamples.load():999),
        @"dense_chroma_samples":@(test.denseChromaSamples.load()),@"chroma_bad_records":chromaBad};
}
static bool PrearrangeNV12(CVPixelBufferRef pixel,NSMutableDictionary *report) {
    size_t left=0,right=0,top=0,bottom=0;CVPixelBufferGetExtendedPixels(pixel,&left,&right,&top,&bottom);
    report[@"extended_bottom"]=@(bottom);
    for(size_t plane=0;plane<2;++plane) {
        size_t width=CVPixelBufferGetWidthOfPlane(pixel,plane)*(plane?2:1),rows=CVPixelBufferGetHeightOfPlane(pixel,plane);
        size_t stride=CVPixelBufferGetBytesPerRowOfPlane(pixel,plane),physicalRows=rows+(plane?bottom/2:bottom);
        size_t needed=(rows+7)&~(size_t)7;
        if(width%64||physicalRows<needed){report[@"prearrange_error"]=@"Insufficient physical tile padding";return false;}
        uint8_t *data=(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel,plane);
        std::vector<uint8_t> original(stride*needed,plane?128:16);
        for(size_t y=0;y<rows;++y)memcpy(original.data()+y*stride,data+y*stride,width);
        if(!remote_nv12::PrepackPlane(original.data(),stride,rows,data,stride,needed,width,plane!=0))return false;
    }
    return true;
}
static bool ExportFixture(SelfTest& test,NSString *path) {
    std::sort(test.samples.begin(),test.samples.end(),[](const auto& a,const auto& b){return a.index<b.index;});
    std::ofstream output(path.fileSystemRepresentation,std::ios::binary);
    for(const auto& sample:test.samples) {
        std::vector<uint8_t> payload;bool idr=false;if(!AnnexB(sample.sample,payload,idr))return false;
        RemoteFrameHeader header={0x31524652,1920,1080,idr?1u:0u,(uint64_t)sample.index+1,0};
        uint32_t length=(uint32_t)(sizeof(header)+payload.size());
        output.write((const char *)&length,sizeof(length));output.write((const char *)&header,sizeof(header));
        output.write((const char *)payload.data(),payload.size());
    }
    return (bool)output;
}
int main(int argc,const char **argv) {
    @autoreleasepool {
        NSString *mode=argc>1?[NSString stringWithUTF8String:argv[1]]:@"hw-serial";
        NSString *prefix=argc>2?[NSString stringWithUTF8String:argv[2]]:[@"/Users/Shared/RemoteNative/codec-probe-" stringByAppendingString:mode];
        bool software=[mode isEqual:@"sw-burst"],serial=[mode isEqual:@"hw-serial"],hold=[mode isEqual:@"hw-burst-retain"];
        bool bgra=[mode isEqual:@"hw-bgra-serial"];
        bool padded=[mode isEqual:@"hw-nv12-padded"];
        bool swizzle=[mode isEqual:@"hw-nv12-prearrange"]||padded;
        int codedHeight=padded?1088:1080;
        if(bgra||swizzle)serial=true;
        if(!software&&!serial&&!hold){fprintf(stderr,"Usage: codec-content-probe hw-serial|hw-burst-retain|sw-burst|hw-bgra-serial [output-prefix]\n");return 1;}
        InitializeHardwareVideo();
        NSDictionary *spec=software?@{(__bridge NSString *)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder:@NO,
            (__bridge NSString *)kVTVideoEncoderSpecification_EncoderID:@"com.apple.videotoolbox.videoencoder.h264"}:
            @{(__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder:@YES};
        NSDictionary *attrs=@{(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:@(bgra?kCVPixelFormatType_32BGRA:kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            (__bridge NSString *)kCVPixelBufferWidthKey:@1920,(__bridge NSString *)kCVPixelBufferHeightKey:@(codedHeight),
            (__bridge NSString *)kCVPixelBufferExtendedPixelsBottomKey:@(swizzle?8:0),
            (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey:@{}};
        SelfTest test;VTCompressionSessionRef session=nullptr;
        NSMutableDictionary *report=[@{@"mode":mode,@"width":@1920,@"height":@1080,@"encoded_height":@(codedHeight),@"fps":@60,@"pass":@NO} mutableCopy];
        OSStatus create=VTCompressionSessionCreate(kCFAllocatorDefault,1920,codedHeight,kCMVideoCodecType_H264,(__bridge CFDictionaryRef)spec,
            (__bridge CFDictionaryRef)attrs,nullptr,SelfTestCallback,&test,&session);
        report[@"create_status"]=@(create);
        if(create||!session){PrintJson(report);return 2;}
        CFTypeRef hardware=nullptr;OSStatus hardwareStatus=VTSessionCopyProperty(session,kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,nullptr,&hardware);
        bool actualHardware=hardware==kCFBooleanTrue;if(hardware)CFRelease(hardware);
        report[@"hardware_query_status"]=@(hardwareStatus);report[@"hardware_used"]=@(actualHardware);
        SetVT(session,kVTCompressionPropertyKey_RealTime,kCFBooleanTrue);
        SetVT(session,kVTCompressionPropertyKey_AllowFrameReordering,kCFBooleanFalse);
        SetVT(session,kVTCompressionPropertyKey_ExpectedFrameRate,(__bridge CFNumberRef)@60);
        OSStatus prepare=VTCompressionSessionPrepareToEncodeFrames(session);report[@"prepare_status"]=@(prepare);
        std::vector<CVPixelBufferRef> retained;
        for(int frame=0;!prepare&&frame<12;++frame) {
            CVPixelBufferRef pixel=nullptr;
            OSStatus allocate=CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,VTCompressionSessionGetPixelBufferPool(session),&pixel);
            if(allocate||!pixel){test.errors.fetch_add(1);break;}
            if(CVPixelBufferLockBaseAddress(pixel,0)!=kCVReturnSuccess){CVPixelBufferRelease(pixel);test.errors.fetch_add(1);break;}
            int offset=(frame%3)*12,marker=1444+frame*20;
            if(bgra) {
                if(CVPixelBufferGetPixelFormatType(pixel)!=kCVPixelFormatType_32BGRA){CVPixelBufferUnlockBaseAddress(pixel,0);CVPixelBufferRelease(pixel);test.errors.fetch_add(1);break;}
                uint8_t *pixels=(uint8_t *)CVPixelBufferGetBaseAddress(pixel);size_t stride=CVPixelBufferGetBytesPerRow(pixel);
                for(int row=0;row<1080;++row)for(int col=0;col<1920;++col) {
                    int gray=(int)lround((TestY[col/240]+offset-16)*255.0/219.0);
                    if(row>=32&&row<64&&col>=marker&&col<marker+12)gray=255;
                    uint8_t *p=pixels+row*stride+col*4;p[0]=p[1]=p[2]=(uint8_t)gray;p[3]=255;
                }
            } else {
                uint8_t *y=(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel,0),*uv=(uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel,1);
                size_t ys=CVPixelBufferGetBytesPerRowOfPlane(pixel,0),uvs=CVPixelBufferGetBytesPerRowOfPlane(pixel,1);
                for(int row=0;row<codedHeight;++row)for(int bar=0;bar<8;++bar)memset(y+row*ys+bar*240,TestY[bar]+offset,240);
                for(int row=0;row<codedHeight/2;++row)for(int col=0;col<1920;col+=2){int bar=col/240;uv[row*uvs+col]=TestU[bar];uv[row*uvs+col+1]=TestV[bar];}
                for(int row=32;row<64;++row)memset(y+row*ys+marker,235,12);
                for(int row=16;row<32;++row)memset(uv+row*uvs+marker,128,12);
            }
            if(swizzle&&!PrearrangeNV12(pixel,report)){CVPixelBufferUnlockBaseAddress(pixel,0);CVPixelBufferRelease(pixel);test.errors.fetch_add(1);break;}
            CVPixelBufferUnlockBaseAddress(pixel,0);
            if(hold)retained.push_back(CVPixelBufferRetain(pixel));
            auto source=new SelfFrame{frame,frame==6,pixel};
            NSDictionary *options=frame==6?@{(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame:@YES}:nil;
            OSStatus encode=VTCompressionSessionEncodeFrame(session,pixel,CMTimeMake(frame,60),CMTimeMake(1,60),(__bridge CFDictionaryRef)options,source,nullptr);
            if(encode){delete source;test.errors.fetch_add(1);}
            if(serial&&VTCompressionSessionCompleteFrames(session,kCMTimeInvalid)!=noErr)test.errors.fetch_add(1);
        }
        OSStatus complete=VTCompressionSessionCompleteFrames(session,kCMTimeInvalid);
        for(auto pixel:retained)CVPixelBufferRelease(pixel);
        VTCompressionSessionInvalidate(session);CFRelease(session);
        NSString *fixture=[prefix stringByAppendingString:@".frames"];
        bool fixtureOk=ExportFixture(test,fixture);
        NSDictionary *decoded=ProbeDecode(test,bgra,padded);
        bool pass=!prepare&&!complete&&test.frames.load()==12&&!test.errors.load()&&test.forcedIdr.load()==1&&[decoded[@"pass"] boolValue]&&
            !hardwareStatus&&(software?!actualHardware:actualHardware);
        [report addEntriesFromDictionary:@{@"complete_status":@(complete),@"encoded_frames":@(test.frames.load()),@"encoded_errors":@(test.errors.load()),
            @"encoded_bytes":@(test.bytes.load()),@"forced_idr":@(test.forcedIdr.load()==1),@"fixture_path":fixture,@"fixture_written":@(fixtureOk),@"decoded":decoded,@"pass":@(pass)}];
        NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
        [json writeToFile:[prefix stringByAppendingString:@".json"] atomically:YES];PrintJson(report);return pass?0:3;
    }
}
