#include "video.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_3.h>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cstdio>
template<class T> struct Com {
    T* p=nullptr;
    ~Com(){reset();}
    Com()=default;
    Com(const Com&)=delete;
    Com& operator=(const Com&)=delete;
    T* operator->()const{return p;}
    T** put(){reset();return &p;}
    void reset(){if(p){p->Release();p=nullptr;}}
};
struct VideoDecoder::Impl {
    HWND canvas=nullptr;
    uint32_t width=0,height=0,canvasWidth=0,canvasHeight=0;
    RECT viewport{};
    bool mfStarted=false,hardware=false;
    uint64_t presented=0;
    uint64_t lastSequence=0;
    bool seenSequence=false;
    double decodeMs=0;
    RollingLatencyStats decodeInputStats,decodeOutputStats,conversionStats,presentWaitStats,presentCallStats;
    RollingLatencyStats frameProcessStats,pendingWaitStats,presentIntervalStats;
    uint64_t decodedFrames=0,pendingReplaced=0,presentWaitTimeouts=0,presentBusy=0,presentOccluded=0;
    std::chrono::steady_clock::time_point pendingSince{},lastPresentedAt{};
    std::string error;
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<IDXGISwapChain1> swap;
    Com<IDXGISwapChain2> swap2;
    Com<IMFDXGIDeviceManager> manager;
    Com<IMFTransform> decoder;
    Com<IMFSample> pending;
    Com<ID3D11VideoDevice> videoDevice;
    Com<ID3D11VideoContext> videoContext;
    Com<ID3D11VideoProcessorEnumerator> enumerator;
    Com<ID3D11VideoProcessor> processor;
    HANDLE latency=nullptr;
    uint32_t vpInputWidth=0,vpInputHeight=0;
    static uint64_t elapsedUs(std::chrono::steady_clock::time_point start){
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
    }
    bool fail(const char* operation,HRESULT hr){char b[240];snprintf(b,sizeof(b),"%s (0x%08lx)",operation,(unsigned long)hr);error=b;return false;}
    bool outputType(){
        for(DWORD i=0;i<30;i++){
            Com<IMFMediaType> type;
            HRESULT hr=decoder->GetOutputAvailableType(0,i,type.put());
            if(FAILED(hr))break;
            GUID subtype{};
            if(SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE,&subtype)) && subtype==MFVideoFormat_NV12){
                hr=decoder->SetOutputType(0,type.p,0);
                if(SUCCEEDED(hr))return true;
            }
        }
        return fail("NV12 decoder output unavailable",MF_E_INVALIDMEDIATYPE);
    }
    bool sizeSwap(){
        RECT r{};GetClientRect(canvas,&r);
        UINT w=std::max<LONG>(1,r.right-r.left),h=std::max<LONG>(1,r.bottom-r.top);
        if(w==canvasWidth && h==canvasHeight)return true;
        if(swap.p){
            context->ClearState();context->Flush();
            HRESULT hr=swap->ResizeBuffers(2,w,h,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
            if(FAILED(hr))return fail("Resize video swapchain",hr);
        }
        canvasWidth=w;canvasHeight=h;enumerator.reset();processor.reset();
        return true;
    }
    bool present(IMFSample* sample,bool& shown){
        shown=false;
        if(!sizeSwap())return false;
        // Keep decoding reference frames when the compositor is busy, but do not
        // block the video worker behind a background/occluded window's refresh.
        if(latency){
            auto waitStart=std::chrono::steady_clock::now();
            DWORD ready=WaitForSingleObject(latency,2);
            presentWaitStats.ObserveUs(elapsedUs(waitStart));
            if(ready==WAIT_TIMEOUT){++presentWaitTimeouts;return true;}
            if(ready!=WAIT_OBJECT_0)return fail("Wait for video presentation",HRESULT_FROM_WIN32(GetLastError()));
        }
        Com<IMFMediaBuffer> buffer;
        HRESULT hr=sample->GetBufferByIndex(0,buffer.put());
        if(FAILED(hr))return fail("Read decoded video buffer",hr);
        Com<IMFDXGIBuffer> dxgi;
        hr=buffer->QueryInterface(__uuidof(IMFDXGIBuffer),(void**)dxgi.put());
        if(FAILED(hr)){hardware=false;return fail("Decoder returned CPU memory; GPU decode required",hr);}
        Com<ID3D11Texture2D> texture;
        hr=dxgi->GetResource(__uuidof(ID3D11Texture2D),(void**)texture.put());
        if(FAILED(hr))return fail("Read GPU video texture",hr);
        hardware=true;
        UINT subresource=0;dxgi->GetSubresourceIndex(&subresource);
        D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
        if(!enumerator.p || td.Width!=vpInputWidth || td.Height!=vpInputHeight){
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
            desc.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            desc.InputWidth=td.Width;desc.InputHeight=td.Height;
            desc.OutputWidth=canvasWidth;desc.OutputHeight=canvasHeight;
            desc.InputFrameRate={60,1};desc.OutputFrameRate={60,1};
            desc.Usage=D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
            hr=videoDevice->CreateVideoProcessorEnumerator(&desc,enumerator.put());
            if(FAILED(hr))return fail("Create GPU video conversion",hr);
            hr=videoDevice->CreateVideoProcessor(enumerator.p,0,processor.put());
            if(FAILED(hr))return fail("Create GPU video processor",hr);
            vpInputWidth=td.Width;vpInputHeight=td.Height;
        }
        Com<ID3D11Texture2D> back;
        hr=swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)back.put());
        if(FAILED(hr))return fail("Get video backbuffer",hr);
        Com<ID3D11RenderTargetView> target;
        hr=device->CreateRenderTargetView(back.p,nullptr,target.put());
        if(FAILED(hr))return fail("Create render target",hr);
        const float background[]={0.035f,0.055f,0.075f,1.0f};
        context->ClearRenderTargetView(target.p,background);
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
        id.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
        id.Texture2D.MipSlice=subresource%td.MipLevels;
        id.Texture2D.ArraySlice=subresource/td.MipLevels;
        Com<ID3D11VideoProcessorInputView> input;
        hr=videoDevice->CreateVideoProcessorInputView(texture.p,enumerator.p,&id,input.put());
        if(FAILED(hr))return fail("Create video input view",hr);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        od.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
        Com<ID3D11VideoProcessorOutputView> output;
        hr=videoDevice->CreateVideoProcessorOutputView(back.p,enumerator.p,&od,output.put());
        if(FAILED(hr))return fail("Create video output view",hr);
        RECT src={0,0,(LONG)width,(LONG)height};
        RECT dest=viewport;
        if(dest.right<=dest.left || dest.bottom<=dest.top){
            double scale=std::min((double)canvasWidth/width,(double)canvasHeight/height);
            LONG dw=(LONG)(width*scale),dh=(LONG)(height*scale);
            dest={(LONG)(canvasWidth-dw)/2,(LONG)(canvasHeight-dh)/2,(LONG)(canvasWidth-dw)/2+dw,(LONG)(canvasHeight-dh)/2+dh};
        }
        videoContext->VideoProcessorSetStreamSourceRect(processor.p,0,TRUE,&src);
        videoContext->VideoProcessorSetStreamDestRect(processor.p,0,TRUE,&dest);
        videoContext->VideoProcessorSetStreamFrameFormat(processor.p,0,D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE color{};color.YCbCr_Matrix=1;color.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext->VideoProcessorSetStreamColorSpace(processor.p,0,&color);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=input.p;
        auto conversionStart=std::chrono::steady_clock::now();
        hr=videoContext->VideoProcessorBlt(processor.p,output.p,0,1,&stream);
        conversionStats.ObserveUs(elapsedUs(conversionStart));
        if(FAILED(hr))return fail("GPU video conversion",hr);
        auto presentStart=std::chrono::steady_clock::now();
        hr=swap->Present(0,DXGI_PRESENT_DO_NOT_WAIT);
        presentCallStats.ObserveUs(elapsedUs(presentStart));
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING){++presentBusy;return true;}
        if(FAILED(hr))return fail("Present video",hr);
        if(hr==DXGI_STATUS_OCCLUDED){++presentOccluded;return true;}
        auto now=std::chrono::steady_clock::now();
        if(lastPresentedAt!=std::chrono::steady_clock::time_point{})
            presentIntervalStats.ObserveUs(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now-lastPresentedAt).count()));
        lastPresentedAt=now;
        presented++;shown=true;return true;
    }
    bool retryPending(){
        if(!pending.p)return true;
        bool shown=false;
        bool ok=present(pending.p,shown);
        if(shown)pendingWaitStats.ObserveUs(elapsedUs(pendingSince));
        if(shown || !ok)pending.reset();
        return ok;
    }
    bool queueLatest(IMFSample* sample){
        // Retain exactly one DXGI-backed decoder sample. Replacing it releases
        // the old surface, while the decoder continues maintaining references.
        sample->AddRef();
        if(pending.p)++pendingReplaced;
        pending.reset();
        pending.p=sample;
        pendingSince=std::chrono::steady_clock::now();
        ++decodedFrames;
        return retryPending();
    }
    bool drain(){
        for(int i=0;i<16;i++){
            MFT_OUTPUT_STREAM_INFO info{};
            HRESULT hr=decoder->GetOutputStreamInfo(0,&info);
            if(FAILED(hr))return fail("Query decoder output",hr);
            MFT_OUTPUT_DATA_BUFFER out{};Com<IMFSample> supplied;
            if(!(info.dwFlags&(MFT_OUTPUT_STREAM_PROVIDES_SAMPLES|MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))){
                hr=MFCreateSample(supplied.put());if(FAILED(hr))return fail("Create decoder sample",hr);
                Com<IMFMediaBuffer> b;hr=MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize,width*height*2),b.put());
                if(FAILED(hr))return fail("Create decoder buffer",hr);
                supplied->AddBuffer(b.p);out.pSample=supplied.p;
            }
            DWORD status=0;auto outputStart=std::chrono::steady_clock::now();
            hr=decoder->ProcessOutput(0,1,&out,&status);
            decodeOutputStats.ObserveUs(elapsedUs(outputStart));
            if(out.pEvents)out.pEvents->Release();
            if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT)return true;
            if(hr==MF_E_TRANSFORM_STREAM_CHANGE){if(!outputType())return false;continue;}
            if(FAILED(hr)){if(out.pSample && out.pSample!=supplied.p)out.pSample->Release();return fail("H264 GPU decode output",hr);}
            if((out.dwStatus&MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE)==MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE){if(out.pSample&&out.pSample!=supplied.p)out.pSample->Release();continue;}
            if(out.pSample){bool ok=queueLatest(out.pSample);if(out.pSample!=supplied.p)out.pSample->Release();if(!ok)return false;}
        }
        return true;
    }
    ~Impl(){pending.reset();if(latency)CloseHandle(latency);decoder.reset();manager.reset();if(mfStarted)MFShutdown();}
};
VideoDecoder::VideoDecoder():impl_(new Impl){}
VideoDecoder::~VideoDecoder()=default;
bool VideoDecoder::Initialize(HWND canvas,uint32_t width,uint32_t height){
    auto& v=*impl_;v.canvas=canvas;v.width=width;v.height=height;
    HRESULT hr=MFStartup(MF_VERSION,MFSTARTUP_LITE);if(FAILED(hr))return v.fail("Media Foundation startup",hr);v.mfStarted=true;
    D3D_FEATURE_LEVEL level;
    hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_VIDEO_SUPPORT,nullptr,0,D3D11_SDK_VERSION,v.device.put(),&level,v.context.put());
    if(FAILED(hr))return v.fail("D3D11 hardware device",hr);
    Com<ID3D10Multithread> multithread;if(SUCCEEDED(v.context->QueryInterface(__uuidof(ID3D10Multithread),(void**)multithread.put())))multithread->SetMultithreadProtected(TRUE);
    hr=v.device->QueryInterface(__uuidof(ID3D11VideoDevice),(void**)v.videoDevice.put());if(FAILED(hr))return v.fail("D3D11 video device",hr);
    hr=v.context->QueryInterface(__uuidof(ID3D11VideoContext),(void**)v.videoContext.put());if(FAILED(hr))return v.fail("D3D11 video context",hr);
    UINT resetToken=0;hr=MFCreateDXGIDeviceManager(&resetToken,v.manager.put());if(FAILED(hr))return v.fail("DXGI manager",hr);
    hr=v.manager->ResetDevice(v.device.p,resetToken);if(FAILED(hr))return v.fail("DXGI decode device",hr);
    hr=CoCreateInstance(CLSID_CMSH264DecoderMFT,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMFTransform),(void**)v.decoder.put());if(FAILED(hr))return v.fail("H264 decoder",hr);
    Com<IMFAttributes> attributes;
    if(SUCCEEDED(v.decoder->GetAttributes(attributes.put()))){
        attributes->SetUINT32(CODECAPI_AVLowLatencyMode,TRUE);
        attributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264,TRUE);
        attributes->SetUINT32(MF_LOW_LATENCY,TRUE);
    }
    hr=v.decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,(ULONG_PTR)v.manager.p);if(FAILED(hr))return v.fail("D3D11 hardware decode negotiation",hr);
    Com<IMFMediaType> type;hr=MFCreateMediaType(type.put());if(FAILED(hr))return v.fail("H264 media type",hr);
    type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264);
    MFSetAttributeSize(type.p,MF_MT_FRAME_SIZE,width,height);MFSetAttributeRatio(type.p,MF_MT_FRAME_RATE,60,1);
    type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
    hr=v.decoder->SetInputType(0,type.p,0);if(FAILED(hr))return v.fail("H264 decoder input",hr);
    if(!v.outputType())return false;
    v.decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0);v.decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,0);
    Com<IDXGIDevice> dxgi;hr=v.device->QueryInterface(__uuidof(IDXGIDevice),(void**)dxgi.put());if(FAILED(hr))return v.fail("DXGI device",hr);
    Com<IDXGIAdapter> adapter;dxgi->GetAdapter(adapter.put());Com<IDXGIFactory2> factory;
    hr=adapter->GetParent(__uuidof(IDXGIFactory2),(void**)factory.put());if(FAILED(hr))return v.fail("DXGI factory",hr);
    RECT r{};GetClientRect(canvas,&r);v.canvasWidth=std::max<LONG>(1,r.right);v.canvasHeight=std::max<LONG>(1,r.bottom);
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=v.canvasWidth;desc.Height=v.canvasHeight;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;desc.Scaling=DXGI_SCALING_STRETCH;desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    hr=factory->CreateSwapChainForHwnd(v.device.p,canvas,&desc,nullptr,nullptr,v.swap.put());if(FAILED(hr))return v.fail("Video flip swapchain",hr);
    factory->MakeWindowAssociation(canvas,DXGI_MWA_NO_ALT_ENTER);
    if(SUCCEEDED(v.swap->QueryInterface(__uuidof(IDXGISwapChain2),(void**)v.swap2.put()))){v.swap2->SetMaximumFrameLatency(1);v.latency=v.swap2->GetFrameLatencyWaitableObject();}
    return true;
}
bool VideoDecoder::Decode(const RemoteFrameHeader& frame,const uint8_t* bytes,size_t length){
    auto& v=*impl_;if(!v.decoder.p || !length || length>8*1024*1024)return v.fail("Invalid H264 access unit",E_INVALIDARG);
    auto start=std::chrono::steady_clock::now();
    bool discontinuity=v.seenSequence&&frame.sequence!=v.lastSequence+1;
    if(discontinuity&&(frame.flags&1)){
        v.pending.reset();
        HRESULT flushed=v.decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH,0);
        if(FAILED(flushed))return v.fail("Flush H264 discontinuity",flushed);
    }
    Com<IMFSample> sample;Com<IMFMediaBuffer> buffer;
    HRESULT hr=MFCreateSample(sample.put());if(FAILED(hr))return v.fail("Encoded frame sample",hr);
    hr=MFCreateMemoryBuffer((DWORD)length,buffer.put());if(FAILED(hr))return v.fail("Encoded frame memory",hr);
    BYTE* data=nullptr;hr=buffer->Lock(&data,nullptr,nullptr);if(FAILED(hr))return v.fail("Lock H264 sample",hr);
    memcpy(data,bytes,length);buffer->Unlock();buffer->SetCurrentLength((DWORD)length);sample->AddBuffer(buffer.p);
    sample->SetSampleTime((LONGLONG)frame.sequence*166667);sample->SetSampleDuration(166667);sample->SetUINT32(MFSampleExtension_CleanPoint,(frame.flags&1)?TRUE:FALSE);
    if(discontinuity)sample->SetUINT32(MFSampleExtension_Discontinuity,TRUE);
    auto processInput=[&]{auto inputStart=std::chrono::steady_clock::now();
        HRESULT result=v.decoder->ProcessInput(0,sample.p,0);v.decodeInputStats.ObserveUs(Impl::elapsedUs(inputStart));return result;};
    hr=processInput();
    if(hr==MF_E_NOTACCEPTING){if(!v.drain())return false;hr=processInput();}
    if(FAILED(hr))return v.fail("H264 GPU decode input",hr);
    v.lastSequence=frame.sequence;v.seenSequence=true;
    bool ok=v.drain();auto elapsed=Impl::elapsedUs(start);v.decodeMs=elapsed/1000.0;v.frameProcessStats.ObserveUs(elapsed);return ok;
}
bool VideoDecoder::Tick(){
    auto& v=*impl_;
    if(!v.pending.p)return true;
    auto start=std::chrono::steady_clock::now();
    bool ok=v.retryPending();
    v.decodeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    return ok;
}
bool VideoDecoder::Finish(){
    auto& v=*impl_;if(!v.decoder.p)return false;
    HRESULT hr=v.decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM,0);
    if(FAILED(hr))return v.fail("H264 end of stream",hr);
    hr=v.decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN,0);
    if(FAILED(hr))return v.fail("H264 drain",hr);
    if(!v.drain())return false;
    // Offline fixtures may stop immediately after a busy compositor. Give the
    // final retained frame a bounded chance to become visible; live Tick keeps
    // retrying on later worker iterations without holding up input or decode.
    auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
    while(v.pending.p && std::chrono::steady_clock::now()<deadline){
        if(!v.retryPending())return false;
        if(v.pending.p)Sleep(1);
    }
    return true;
}
void VideoDecoder::SetViewport(const RECT& r){impl_->viewport=r;}
bool VideoDecoder::Hardware()const{return impl_->hardware;}
double VideoDecoder::LastDecodeMs()const{return impl_->decodeMs;}
uint64_t VideoDecoder::Presented()const{return impl_->presented;}
VideoTimingSnapshot VideoDecoder::TimingSnapshot()const{
    const auto& v=*impl_;VideoTimingSnapshot result;
    result.decodeInput=v.decodeInputStats.Snapshot();result.decodeOutput=v.decodeOutputStats.Snapshot();
    result.conversionSubmit=v.conversionStats.Snapshot();result.presentWait=v.presentWaitStats.Snapshot();
    result.presentCall=v.presentCallStats.Snapshot();result.frameProcess=v.frameProcessStats.Snapshot();
    result.pendingWait=v.pendingWaitStats.Snapshot();result.presentInterval=v.presentIntervalStats.Snapshot();
    result.decodedFrames=v.decodedFrames;result.pendingReplaced=v.pendingReplaced;
    result.presentWaitTimeouts=v.presentWaitTimeouts;result.presentBusy=v.presentBusy;result.presentOccluded=v.presentOccluded;
    return result;
}
const std::string& VideoDecoder::Error()const{return impl_->error;}
