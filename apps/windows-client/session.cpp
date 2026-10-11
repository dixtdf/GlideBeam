#include "session.h"
#include "video.h"
#include <windows.h>
#include <wincrypt.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include "../../shared/exchange.h"
#include "../../shared/diagnostic-log.h"
#include <condition_variable>
#include <atomic>
#include <deque>
#include <chrono>
#include <algorithm>
#include <array>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <climits>
#include <cctype>
#include <optional>
namespace {
std::string TransportErrorText(std::string diagnostic){
    std::transform(diagnostic.begin(),diagnostic.end(),diagnostic.begin(),[](unsigned char c){return (char)std::tolower(c);});
    auto contains=[&](const char* text){return diagnostic.find(text)!=std::string::npos;};
    if(contains("timeout")||contains("timed out"))return "连接超时，请检查 Mac 地址和网络连接";
    if(contains("another paired client")||contains("one paired client"))return "Mac 已有客户端连接，请先断开现有会话";
    if(contains("refused")||contains("blocked"))return "连接被拒绝，请检查 Mac 服务和 IP 黑名单";
    if(contains("pairing")||contains("pake")||contains("proof")||contains("token rejected")||contains("certificate")||contains("fingerprint"))
        return "配对验证失败，请检查地址和令牌；连续 10 次令牌错误后，可在 Mac 端解除 IP 封禁";
    return "连接已中断，请检查 Mac 端状态后重试";
}
std::string JsonString(const std::string& json,const std::string& key){
    auto p=json.find('"'+key+'"');if(p==std::string::npos)return {};
    p=json.find(':',p);if(p==std::string::npos)return {};
    p=json.find_first_not_of(" \r\n\t",p+1);if(p==std::string::npos||json[p]!='"')return {};
    std::string result;bool escape=false;
    for(++p;p<json.size();p++){char c=json[p];if(escape){switch(c){case'n':result+='\n';break;case'r':result+='\r';break;case't':result+='\t';break;case'"':case'\\':case'/':result+=c;break;default:return {};}escape=false;}else if(c=='\\')escape=true;else if(c=='"')return result;else if((unsigned char)c<32)return {};else result+=c;}
    return {};
}
std::optional<bool> JsonBool(const std::string& json,const std::string& key){
    auto p=json.find('"'+key+'"');if(p==std::string::npos)return {};
    p=json.find(':',p);if(p==std::string::npos)return {};
    p=json.find_first_not_of(" \r\n\t",p+1);if(p==std::string::npos)return {};
    bool value=false;size_t length=0;
    if(json.compare(p,4,"true")==0){value=true;length=4;}else if(json.compare(p,5,"false")==0)length=5;else return {};
    size_t end=p+length;if(end<json.size()&&json[end]!=','&&json[end]!='}'&&json[end]!=' '&&json[end]!='\r'&&json[end]!='\n'&&json[end]!='\t')return {};
    return value;
}
bool ValidRequestId(const std::string& id){
    if(id.size()!=36)return false;
    for(size_t n=0;n<id.size();++n){if(n==8||n==13||n==18||n==23){if(id[n]!='-')return false;}else if(!std::isxdigit(static_cast<unsigned char>(id[n])))return false;}
    return true;
}
bool ApplyRestartStatus(SessionSnapshot& snapshot,const std::string& message){
    if(message.size()>16384||JsonString(message,"type")!="host-command-status"||JsonString(message,"kind")!="restart-mac")return false;
    auto id=JsonString(message,"request_id");if(!ValidRequestId(id)||id!=snapshot.restartRequestId)return false;
    auto value=JsonString(message,"state");
    const char* text=nullptr;
    if(value=="authorizing")text="请在 Mac 上确认系统重启授权";
    else if(value=="requested")text="Mac 已接受重启请求，正在重启";
    else if(value=="cancelled")text="已取消 Mac 重启";
    else if(value=="denied")text="Mac 拒绝了重启授权，请在 Mac 上检查";
    else if(value=="error")text="Mac 重启失败，请查看 Mac 日志";
    else if(value=="busy")text="Mac 正在处理其他重启请求";
    else return false;
    snapshot.restartState=value;snapshot.restartMessage=text;
    snapshot.restartBusy=value=="authorizing"||value=="requested"||value=="busy";
    return true;
}
void ApplyRestartCapabilities(SessionSnapshot& snapshot,const std::string& message){
    if(JsonString(message,"type")!="host-status")return;
    if(auto supported=JsonBool(message,"restart_supported"))snapshot.restartSupported=*supported;
    if(auto required=JsonBool(message,"restart_requires_local_authorization"))snapshot.restartRequiresLocalAuthorization=*required;
    if(auto busy=JsonBool(message,"restart_busy")) {
        if(*busy)snapshot.restartBusy=true;
        else if(snapshot.restartState!="queued"&&snapshot.restartState!="authorizing"&&snapshot.restartState!="requested")snapshot.restartBusy=false;
    }
}
void ApplyEncoderStatus(SessionSnapshot& snapshot,const std::string& message){
    if(message.size()>16384||JsonString(message,"type")!="host-status")return;
    // An old host, unknown mode, or unavailable encoder must never inherit a
    // previous hardware/software claim. Host strings are not shown verbatim.
    snapshot.macEncoderMode=MacVideoEncoderMode::Unknown;
    auto value=JsonString(message,"encoder_mode");
    if(value=="hardware")snapshot.macEncoderMode=MacVideoEncoderMode::Hardware;
    else if(value=="apple-software")snapshot.macEncoderMode=MacVideoEncoderMode::AppleSoftware;
    else if(value=="software")snapshot.macEncoderMode=MacVideoEncoderMode::Software;
}
const char* VideoPathMessage(MacVideoEncoderMode mode,bool hardwareDecode){
    if(mode==MacVideoEncoderMode::Hardware)return hardwareDecode?
        "Mac 硬件 H.264 编码 · Windows GPU 硬解":"Mac 硬件 H.264 编码 · 等待 Windows GPU 硬解";
    if(mode==MacVideoEncoderMode::AppleSoftware||mode==MacVideoEncoderMode::Software)return hardwareDecode?
        "Mac 软件 H.264 编码 · Windows GPU 硬解":"Mac 软件 H.264 编码 · 等待 Windows GPU 硬解";
    return hardwareDecode?"H.264 编码 · Windows GPU 硬解":"H.264 编码 · 等待 Windows GPU 硬解";
}
std::string Quote(const std::string& s){std::string r="\"";for(char c:s){if(c=='"'||c=='\\')r+='\\';if((unsigned char)c>=32)r+=c;}return r+'"';}
std::string Hex(const uint8_t* data,size_t length){static const char* h="0123456789abcdef";std::string r;r.reserve(length*2);for(size_t i=0;i<length;i++){r+=h[data[i]>>4];r+=h[data[i]&15];}return r;}
bool Unhex(const std::string& s,std::vector<uint8_t>& r){if(s.size()%2||s.size()>65536)return false;r.clear();auto nib=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return-1;};for(size_t i=0;i<s.size();i+=2){int a=nib(s[i]),b=nib(s[i+1]);if(a<0||b<0)return false;r.push_back((uint8_t)(a*16+b));}return true;}
bool ValidToken(const std::string& token){
    if(token.empty()||token.size()>512)return false;
    std::array<wchar_t,256> wide{};int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,token.data(),(int)token.size(),wide.data(),(int)wide.size());
    if(!length)return false;
    int characters=0;
    for(int i=0;i<length;i++){auto c=(unsigned)wide[i];if(c<32||(c>=127&&c<=159))return false;if(c>=0xDC00&&c<=0xDFFF)continue;characters++;}
    return characters>=6&&characters<=128;
}
std::filesystem::path SettingsPath(){wchar_t p[32768]{};DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",p,32768);if(!n||n>=32768)return {};return std::filesystem::path(p)/L"RemoteNative"/L"connection.json";}
std::string ReadFile(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);if(!f)return {};f.seekg(0,std::ios::end);auto size=f.tellg();if(size<0||size>65536)return {};f.seekg(0);std::string data((size_t)size,'\0');f.read(data.data(),data.size());return data;}
struct Api {
    HMODULE module=nullptr;
    decltype(&remote_client_start) start=nullptr;
    decltype(&remote_status) status=nullptr;
    decltype(&remote_rtt_ms) rtt=nullptr;
    decltype(&remote_pending) pending=nullptr;
    decltype(&remote_send) send=nullptr;
    decltype(&remote_poll) poll=nullptr;
    decltype(&remote_last_error) error=nullptr;
    decltype(&remote_fingerprint) fingerprint=nullptr;
    decltype(&remote_close) close=nullptr;
    decltype(&remote_bulk_status) bulkStatus=nullptr;
    decltype(&remote_bulk_send) bulkSend=nullptr;
    decltype(&remote_bulk_poll) bulkPoll=nullptr;
    ~Api(){if(module)FreeLibrary(module);}
    bool load(){
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
        auto path=std::filesystem::path(exe).parent_path()/L"remote_transport.dll";
        module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module)return false;
#define LOAD(field,name) {auto symbol=GetProcAddress(module,name);static_assert(sizeof(symbol)==sizeof(field));memcpy(&field,&symbol,sizeof(field));if(!field)return false;}
        LOAD(start,"remote_client_start");LOAD(status,"remote_status");LOAD(rtt,"remote_rtt_ms");LOAD(pending,"remote_pending");
        LOAD(send,"remote_send");LOAD(poll,"remote_poll");LOAD(error,"remote_last_error");LOAD(fingerprint,"remote_fingerprint");LOAD(close,"remote_close");
        LOAD(bulkStatus,"remote_bulk_status");LOAD(bulkSend,"remote_bulk_send");LOAD(bulkPoll,"remote_bulk_poll");
#undef LOAD
        return true;
    }
    std::string message(remote_endpoint* ep){
        std::array<char,2048> text{};error(ep,text.data(),text.size());
        auto diagnostic=std::string("RemoteNative transport: ")+text.data()+"\n";OutputDebugStringA(diagnostic.c_str());
        return TransportErrorText(text.data());
    }
};
struct BulkState {
    std::mutex mutex;
    remote_endpoint* endpoint=nullptr;
    decltype(&remote_bulk_send) send=nullptr;
    int Send(const uint8_t* bytes,size_t size){
        std::lock_guard guard(mutex);
        return endpoint&&send?send(endpoint,bytes,size):-1;
    }
    void Reset(){std::lock_guard guard(mutex);endpoint=nullptr;send=nullptr;}
};
class ClientSession final:public IClientSession {
    mutable std::mutex stateMutex_,inputMutex_;
    std::condition_variable inputReady_;
    SessionSnapshot snapshot_;
    std::vector<uint8_t> cursor_;
    std::deque<std::vector<uint8_t>> input_;
    std::deque<std::string> control_;
    std::chrono::steady_clock::time_point restartStarted_{};
    RECT viewport_{};
    HWND canvas_=nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> workerDone_{true};
    std::thread worker_;
    std::shared_ptr<BulkState> bulkState_=std::make_shared<BulkState>();
    std::shared_ptr<remote_exchange::Session> exchange_;
    void state(SessionState s,const std::string& message){
        bool changed=false;
        {std::lock_guard lock(stateMutex_);changed=snapshot_.state!=s;snapshot_.state=s;snapshot_.message=message;
         if(s!=SessionState::Connected){snapshot_.macEncoderMode=MacVideoEncoderMode::Unknown;snapshot_.hardwareDecode=false;}}
        if(changed || s==SessionState::Error)remote_log::Write(s==SessionState::Error?remote_log::Level::Error:remote_log::Level::Info,
            "session","state="+std::to_string(static_cast<int>(s))+" error_class="+remote_log::ErrorClass(s==SessionState::Error?message:std::string_view{}));
    }
    void requestIdr(Api& api,remote_endpoint* ep){RemoteInputEvent e{};e.type=INPUT_KEYFRAME;api.send(ep,REMOTE_INPUT,(const uint8_t*)&e,sizeof(e));}
    void run(ConnectionOptions options){
        struct Apartment {HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);~Apartment(){if(SUCCEEDED(result))CoUninitialize();}} apartment;
        {
            Api api;
            if(!api.load()){remote_log::Write(remote_log::Level::Error,"transport","DLL load failed");state(SessionState::Error,"无法加载传输组件 remote_transport.dll");return;}
            remote_endpoint* endpoint=api.start(options.address.c_str(),options.token.c_str(),"");
            if(!endpoint){state(SessionState::Error,"连接配置无效");return;}
            remote_log::Write(remote_log::Level::Info,"transport","endpoint created");
            std::unique_ptr<remote_endpoint,decltype(api.close)> endpointOwner(endpoint,api.close);
            {std::lock_guard guard(bulkState_->mutex);bulkState_->endpoint=endpoint;bulkState_->send=api.bulkSend;}
            struct BulkCleanup {std::shared_ptr<remote_exchange::Session> exchange;std::shared_ptr<BulkState> state;~BulkCleanup(){exchange->SetConnected(false);state->Reset();}} bulkCleanup{exchange_,bulkState_};
            std::jthread bulkPump([this,&api,endpoint](std::stop_token token){
                std::vector<uint8_t> packet(1024*1024+4096);bool ready=false,blocked=false;int pendingSize=0;
                while(!stop_&&!token.stop_requested()){
                    bool now=api.bulkStatus(endpoint)==1;if(now!=ready){ready=now;pendingSize=0;blocked=false;exchange_->SetConnected(now);remote_log::Write(remote_log::Level::Info,"bulk",ready?"ready":"disconnected");}
                    if(ready){
                        for(int i=0;i<32&&!stop_&&!token.stop_requested();i++){
                            if(!pendingSize){pendingSize=api.bulkPoll(endpoint,packet.data(),packet.size());if(pendingSize<=0){pendingSize=0;break;}}
                            // Poll consumes the transport frame. Keep its bytes until
                            // the bounded engine queue accepts them, preserving order
                            // while the disk worker applies backpressure.
                            if(!exchange_->Receive(packet.data(),(size_t)pendingSize)){
                                if(!blocked)remote_log::Write(remote_log::Level::Warning,"bulk","receive backpressure begin");
                                blocked=true;break;
                            }
                            if(blocked)remote_log::Write(remote_log::Level::Info,"bulk","receive backpressure cleared");
                            blocked=false;
                            pendingSize=0;
                        }
                        exchange_->SetNetworkRtt(api.rtt(endpoint));
                    }else pendingSize=0;
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
                pendingSize=0;
                exchange_->SetConnected(false);
                remote_log::Write(remote_log::Level::Info,"bulk","pump stopped");
            });
            std::jthread inputPump;
            std::unique_ptr<VideoDecoder> video;
            std::vector<uint8_t> packet(8*1024*1024+4096);
            uint64_t bytes=0,frames=0;
            auto lastStats=std::chrono::steady_clock::now();
            bool connected=false,awaitIdr=true,captureInactive=false;
            std::string captureNotice;
            while(!stop_){
                int status=api.status(endpoint);
                if(status<0){state(SessionState::Error,api.message(endpoint));break;}
                if(status==1&&!connected){
                    connected=true;state(SessionState::Connected,"已连接，等待 Mac 屏幕画面");
                    std::array<char,128> fingerprint{};if(api.fingerprint(endpoint,fingerprint.data(),fingerprint.size())>0)options.fingerprint=fingerprint.data();
                    if(options.persistSettings)SaveConnectionOptions(options);
                    requestIdr(api,endpoint);
                    inputPump=std::jthread([this,&api,endpoint](std::stop_token token){
                        while(!stop_&&!token.stop_requested()){
                            std::deque<std::vector<uint8_t>> pending;
                            std::deque<std::string> control;
                            {std::unique_lock lock(inputMutex_);inputReady_.wait_for(lock,std::chrono::milliseconds(20),[&]{return stop_||token.stop_requested()||!input_.empty()||!control_.empty();});pending.swap(input_);control.swap(control_);}
                            for(auto& command:control){
                                if(stop_||token.stop_requested())break;
                                if(api.send(endpoint,REMOTE_CONTROL,reinterpret_cast<const uint8_t*>(command.data()),command.size())<0){
                                    remote_log::Write(remote_log::Level::Error,"restart-mac","control send failed");
                                    std::lock_guard lock(stateMutex_);snapshot_.restartBusy=false;snapshot_.restartState="error";snapshot_.restartMessage="重启请求发送失败，请重试";
                                }else remote_log::Write(remote_log::Level::Info,"restart-mac","authenticated request sent");
                            }
                            for(auto& event:pending){
                                if(stop_||token.stop_requested())break;
                                if(api.send(endpoint,REMOTE_INPUT,event.data(),event.size())<0){state(SessionState::Error,api.message(endpoint));stop_=true;break;}
                            }
                        }
                    });
                }
                if(connected){
                    for(int i=0;i<64&&!stop_;i++){
                        uint32_t channel=0;int count=api.poll(endpoint,&channel,packet.data(),packet.size());
                        if(count==0)break;
                        if(count<0){state(SessionState::Error,api.message(endpoint));stop_=true;break;}
                        if(channel==REMOTE_CURSOR){std::lock_guard lock(stateMutex_);cursor_.assign(packet.begin(),packet.begin()+count);}
                        else if(channel==REMOTE_CONTROL){
                            std::string message((char*)packet.data(),count);
                            {
                                std::lock_guard lock(stateMutex_);
                                ApplyEncoderStatus(snapshot_,message);
                                ApplyRestartCapabilities(snapshot_,message);
                                if(ApplyRestartStatus(snapshot_,message))remote_log::Write(remote_log::Level::Info,"restart-mac","state="+snapshot_.restartState+" request_id="+snapshot_.restartRequestId);
                            }
                            if(JsonString(message,"type")=="host-command-status")continue;
                            auto error=JsonString(message,"error");auto statusText=JsonString(message,"status");
                            if(statusText.empty())statusText=JsonString(message,"message");
                            auto captureState=JsonString(message,"capture_state");
                            if(!captureState.empty()) {
                                bool inactive=captureState!="running";
                                captureNotice=JsonString(message,"capture_message");
                                if(inactive) {
                                    if(!captureInactive) { video.reset(); awaitIdr=true; }
                                    if(captureNotice.empty())captureNotice="等待 Mac 恢复画面采集";
                                } else if(captureInactive) { awaitIdr=true; requestIdr(api,endpoint); }
                                captureInactive=inactive;
                                {std::lock_guard lock(stateMutex_);snapshot_.captureState=captureState;
                                 if(inactive){snapshot_.hardwareDecode=false;snapshot_.macEncoderMode=MacVideoEncoderMode::Unknown;}}
                            }
                            if(captureInactive)state(SessionState::Connected,captureNotice);
                            else if(!error.empty())state(SessionState::Connected,error);
                            else if(!statusText.empty())state(SessionState::Connected,statusText);
                        }
                        else if(channel==REMOTE_VIDEO){
                            if(captureInactive)continue;
                            if((size_t)count<sizeof(RemoteFrameHeader))continue;
                            RemoteFrameHeader header{};memcpy(&header,packet.data(),sizeof(header));
                            if(header.magic!=0x31524652||header.width<64||header.height<64||header.width>7680||header.height>4320||(header.flags&~1u)||header.sequence>(uint64_t)(LLONG_MAX/166667)){state(SessionState::Error,"收到无效视频格式");stop_=true;break;}
                            if(awaitIdr&&!(header.flags&1))continue;
                            if(!video || Snapshot().videoWidth!=header.width || Snapshot().videoHeight!=header.height){
                                if(!(header.flags&1)){awaitIdr=true;requestIdr(api,endpoint);continue;}
                                video.reset();video=std::make_unique<VideoDecoder>();
                                if(!video->Initialize(canvas_,header.width,header.height)){state(SessionState::Error,video->Error());stop_=true;break;}
                                {std::lock_guard lock(stateMutex_);snapshot_.videoWidth=header.width;snapshot_.videoHeight=header.height;}
                            }
                            RECT view;{std::lock_guard lock(stateMutex_);view=viewport_;}video->SetViewport(view);
                            auto before=video->Presented();
                            if(!video->Decode(header,packet.data()+sizeof(header),count-sizeof(header))){state(SessionState::Error,video->Error());stop_=true;break;}
                            awaitIdr=false;bytes+=count;frames+=video->Presented()-before;
                            {std::lock_guard lock(stateMutex_);snapshot_.actualFrames+=video->Presented()-before;snapshot_.hardwareDecode=video->Hardware();snapshot_.decodeMs=video->LastDecodeMs();snapshot_.message=VideoPathMessage(snapshot_.macEncoderMode,video->Hardware());}
                        }
                    }
                    if(video&&!captureInactive){
                        auto before=video->Presented();
                        if(!video->Tick()){state(SessionState::Error,video->Error());stop_=true;break;}
                        auto displayed=video->Presented()-before;frames+=displayed;
                        if(displayed){std::lock_guard lock(stateMutex_);snapshot_.actualFrames+=displayed;snapshot_.hardwareDecode=video->Hardware();snapshot_.decodeMs=video->LastDecodeMs();snapshot_.message=VideoPathMessage(snapshot_.macEncoderMode,video->Hardware());}
                    }
                    auto now=std::chrono::steady_clock::now();double elapsed=std::chrono::duration<double>(now-lastStats).count();
                    if(elapsed>=1.0){
                        std::lock_guard lock(stateMutex_);snapshot_.fps=frames/elapsed;snapshot_.bitrateMbps=bytes*8.0/elapsed/1000000.0;
                        snapshot_.networkMs=api.rtt(endpoint);snapshot_.queuedFrames=api.pending(endpoint,REMOTE_VIDEO);
                        frames=bytes=0;lastStats=now;
                    }
                }
                {
                    std::lock_guard lock(stateMutex_);
                    if(snapshot_.restartBusy&&(snapshot_.restartState=="queued"||snapshot_.restartState=="authorizing")&&
                        std::chrono::steady_clock::now()-restartStarted_>std::chrono::seconds(150)){
                        snapshot_.restartBusy=false;snapshot_.restartState="error";snapshot_.restartMessage="等待 Mac 重启授权超时，请在 Mac 上检查";
                        remote_log::Write(remote_log::Level::Warning,"restart-mac","authorization wait timed out");
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            inputPump.request_stop();inputReady_.notify_all();if(inputPump.joinable())inputPump.join();
            bulkPump.request_stop();if(bulkPump.joinable())bulkPump.join();
            if(connected){RemoteInputEvent release{};release.type=INPUT_RELEASE_ALL;api.send(endpoint,REMOTE_INPUT,(const uint8_t*)&release,sizeof(release));std::this_thread::sleep_for(std::chrono::milliseconds(15));}
            video.reset();exchange_->SetConnected(false);
            bulkState_->Reset();
            endpointOwner.reset();
            remote_log::Write(remote_log::Level::Info,"transport","endpoint closed");
        }
    }
public:
    ClientSession(){wchar_t profile[32768]{};DWORD n=GetEnvironmentVariableW(L"USERPROFILE",profile,32768);auto directory=n&&n<32768?std::filesystem::path(profile)/L"Downloads"/L"Remote Native":std::filesystem::temp_directory_path()/L"Remote Native";exchange_=std::make_shared<remote_exchange::Session>(false,directory,[state=bulkState_](const uint8_t* bytes,size_t size){return state->Send(bytes,size);});}
    ~ClientSession(){Disconnect();exchange_.reset();}
    std::shared_ptr<remote_exchange::Session> Exchange()const override{return exchange_;}
    bool RestartMac()override{
        GUID id{};if(FAILED(CoCreateGuid(&id)))return false;
        char requestId[37]{};
        std::snprintf(requestId,sizeof(requestId),"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            static_cast<unsigned long>(id.Data1),static_cast<unsigned>(id.Data2),static_cast<unsigned>(id.Data3),
            static_cast<unsigned>(id.Data4[0]),static_cast<unsigned>(id.Data4[1]),static_cast<unsigned>(id.Data4[2]),static_cast<unsigned>(id.Data4[3]),
            static_cast<unsigned>(id.Data4[4]),static_cast<unsigned>(id.Data4[5]),static_cast<unsigned>(id.Data4[6]),static_cast<unsigned>(id.Data4[7]));
        {
            std::lock_guard lock(stateMutex_);
            if(stop_||snapshot_.state!=SessionState::Connected||!snapshot_.restartSupported||snapshot_.restartBusy)return false;
            snapshot_.restartBusy=true;snapshot_.restartState="queued";snapshot_.restartRequestId=requestId;snapshot_.restartMessage="正在发送 Mac 重启请求";restartStarted_=std::chrono::steady_clock::now();
        }
        std::string command="{\"type\":\"host-command\",\"kind\":\"restart-mac\",\"request_id\":\""+std::string(requestId)+"\",\"confirmed\":true}";
        {std::lock_guard lock(inputMutex_);control_.push_back(std::move(command));}
        inputReady_.notify_one();remote_log::Write(remote_log::Level::Info,"restart-mac","request queued id="+std::string(requestId));return true;
    }
    bool Connect(HWND canvas,const ConnectionOptions& options)override{
        Disconnect();
        remote_log::Write(remote_log::Level::Info,"session","connect requested");
        if(options.address.empty()||!ValidToken(options.token)){state(SessionState::Error,"请填写地址和 6–128 个字符的配对令牌");return false;}
        canvas_=canvas;stop_=false;
        {std::lock_guard lock(stateMutex_);snapshot_=SessionSnapshot{};snapshot_.state=SessionState::Connecting;snapshot_.message="正在验证加密连接";snapshot_.networkMs=-1;}
        workerDone_=false;
        worker_=std::thread([this,options]{try{run(options);}catch(const std::exception& e){state(SessionState::Error,e.what());}catch(...){state(SessionState::Error,"会话遇到异常");}workerDone_=true;});return true;
    }
    void Disconnect()override{
        const bool hadWorker=worker_.joinable();
        if(hadWorker)remote_log::Write(remote_log::Level::Info,"session","disconnect requested");
        stop_=true;inputReady_.notify_all();
        if(worker_.joinable()){
            if(canvas_&&GetWindowThreadProcessId(canvas_,nullptr)==GetCurrentThreadId()){
                while(!workerDone_){MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);Sleep(2);}
            }
            worker_.join();
        }
        bulkState_->Reset();
        {std::lock_guard lock(inputMutex_);input_.clear();control_.clear();}
        {std::lock_guard lock(stateMutex_);snapshot_=SessionSnapshot{};snapshot_.message="准备连接你的 Mac";cursor_.clear();}
        if(hadWorker)remote_log::Write(remote_log::Level::Info,"session","disconnect complete");
    }
    void SendInput(const uint8_t* bytes,size_t size)override{
        if(size<sizeof(RemoteInputEvent)||size>65536||stop_)return;
        std::vector<uint8_t> event(bytes,bytes+size);uint32_t type=0;memcpy(&type,bytes,4);
        std::lock_guard lock(inputMutex_);
        if(type==INPUT_MOVE&&!input_.empty()){uint32_t previous=0;memcpy(&previous,input_.back().data(),4);if(previous==INPUT_MOVE){input_.back()=std::move(event);inputReady_.notify_one();return;}}
        if(input_.size()>=1024){remote_log::Write(remote_log::Level::Warning,"input","bounded queue overflow; releasing held keys");input_.clear();RemoteInputEvent release{};release.type=INPUT_RELEASE_ALL;input_.emplace_back((uint8_t*)&release,(uint8_t*)&release+sizeof(release));}
        input_.push_back(std::move(event));
        inputReady_.notify_one();
    }
    void Tick()override{}
    SessionSnapshot Snapshot()const override{std::lock_guard lock(stateMutex_);return snapshot_;}
    void ResizeViewport(const RECT& rect)override{std::lock_guard lock(stateMutex_);viewport_=rect;}
    bool TakeCursor(std::vector<uint8_t>& packet)override{std::lock_guard lock(stateMutex_);if(cursor_.empty())return false;packet.swap(cursor_);return true;}
    static bool TestCodecStateReset(){
        // A failed connection and an explicit disconnect must not retain the
        // previous Mac encoder or Windows decoder claim on the next attempt.
        ClientSession client;
        auto seed=[&]{client.snapshot_.state=SessionState::Connected;
            client.snapshot_.macEncoderMode=MacVideoEncoderMode::AppleSoftware;client.snapshot_.hardwareDecode=true;};
        auto cleared=[&]{auto snapshot=client.Snapshot();return snapshot.macEncoderMode==MacVideoEncoderMode::Unknown&&!snapshot.hardwareDecode;};
        seed();client.state(SessionState::Error,"synthetic connection failure");if(!cleared())return false;
        seed();client.state(SessionState::Connecting,"synthetic reconnect");if(!cleared())return false;
        seed();client.Disconnect();return cleared()&&client.Snapshot().state==SessionState::Idle;
    }
    static bool TestRetainedBulkLifetime(){
        // An Explorer/OLE stream can retain this engine after the owning client
        // is gone. Exercise its sender after destruction without any network.
        std::atomic<int> calls{0};
        auto owner=std::make_unique<ClientSession>();
        owner->bulkState_->endpoint=reinterpret_cast<remote_endpoint*>(&calls);
        owner->bulkState_->send=[](remote_endpoint* endpoint,const uint8_t*,size_t){
            reinterpret_cast<std::atomic<int>*>(endpoint)->fetch_add(1);return 1;
        };
        uint8_t byte=1;
        if(owner->bulkState_->Send(&byte,1)!=1||calls!=1)return false;
        std::weak_ptr<BulkState> lifetime=owner->bulkState_;
        auto retained=owner->Exchange();
        owner.reset();
        auto state=lifetime.lock();
        if(!state||state->Send(&byte,1)!=-1||calls!=1)return false;
        state.reset();
        retained->SetConnected(true);
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        while(retained->GetSnapshot().message.empty()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        bool dispatched=!retained->GetSnapshot().message.empty()&&calls==1;
        retained->SetConnected(false);retained.reset();
        return dispatched&&lifetime.expired();
    }
};
}
std::unique_ptr<IClientSession> CreateClientSession(){return std::make_unique<ClientSession>();}
ConnectionOptions LoadConnectionOptions(){
    ConnectionOptions options;options.address="192.168.1.100:47990";
    auto text=ReadFile(SettingsPath());if(text.empty())return options;
    auto address=JsonString(text,"address");if(!address.empty())options.address=address;
    options.fingerprint=JsonString(text,"fingerprint");std::vector<uint8_t> encrypted;
    if(Unhex(JsonString(text,"tokenProtected"),encrypted)&&!encrypted.empty()){
        DATA_BLOB input{(DWORD)encrypted.size(),encrypted.data()},output{};
        if(CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)){options.token.assign((char*)output.pbData,output.cbData);SecureZeroMemory(output.pbData,output.cbData);LocalFree(output.pbData);}
    }
    return options;
}
bool SaveConnectionOptions(const ConnectionOptions& options){
    DATA_BLOB input{(DWORD)options.token.size(),(BYTE*)options.token.data()},output{};
    if(!CryptProtectData(&input,L"RemoteNative pairing",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output))return false;
    auto protectedToken=Hex(output.pbData,output.cbData);LocalFree(output.pbData);
    auto path=SettingsPath();if(path.empty())return false;
    try{std::filesystem::create_directories(path.parent_path());auto temp=path;temp+=L".tmp";std::ofstream f(temp,std::ios::binary);
        f<<"{\n  \"address\": "<<Quote(options.address)<<",\n  \"fingerprint\": "<<Quote(options.fingerprint)<<",\n  \"tokenProtected\": "<<Quote(protectedToken)<<"\n}\n";f.close();if(!f)return false;
        return MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    }catch(...){return false;}
}
bool ImportPairingFile(const wchar_t* path){auto text=ReadFile(path);ConnectionOptions o{JsonString(text,"address"),JsonString(text,"token"),JsonString(text,"fingerprint")};if(o.address.empty()||!ValidToken(o.token))return false;return SaveConnectionOptions(o);}
bool RunSessionSelfTest(){
    std::vector<uint8_t> hex;
    if(!Unhex("00aaff",hex)||hex!=std::vector<uint8_t>({0,170,255})||Unhex("zz",hex))return false;
    if(JsonString("{\"text\":\"hello\\nworld\"}","text")!="hello\nworld")return false;
    if(!ValidToken("123456")||!ValidToken("自定义配对令牌")||ValidToken("12345")||ValidToken("123456\n"))return false;
    if(TransportErrorText("pairing authentication timeout")!="连接超时，请检查 Mac 地址和网络连接")return false;
    if(TransportErrorText("server pairing proof rejected").find("配对验证失败")!=0)return false;
    if(TransportErrorText("QUIC/TLS handshake: connection refused")!="连接被拒绝，请检查 Mac 服务和 IP 黑名单")return false;
    if(TransportErrorText("unexpected socket failure")!="连接已中断，请检查 Mac 端状态后重试")return false;
    if(JsonBool("{\"flag\":\"true\"}","flag")||JsonBool("{\"flag\":truex}","flag")||JsonBool("{\"other\":true}","flag"))return false;
    if(JsonBool("{\"flag\":false}","flag")!=false||JsonBool("{\"flag\": true }","flag")!=true)return false;
    SessionSnapshot encoder;
    auto encoderMessage=[&](bool hardwareDecode){return std::string(VideoPathMessage(encoder.macEncoderMode,hardwareDecode));};
    if(encoderMessage(false)!="H.264 编码 · 等待 Windows GPU 硬解"||
       encoderMessage(true)!="H.264 编码 · Windows GPU 硬解")return false;
    ApplyEncoderStatus(encoder,"{\"type\":\"host-status\",\"encoder_mode\":\"hardware\",\"encoder_id\":\"synthetic-hw\",\"fps\":60}");
    if(encoder.macEncoderMode!=MacVideoEncoderMode::Hardware||encoderMessage(true)!="Mac 硬件 H.264 编码 · Windows GPU 硬解"||
       encoderMessage(false)!="Mac 硬件 H.264 编码 · 等待 Windows GPU 硬解")return false;
    ApplyEncoderStatus(encoder,"{\"type\":\"host-command-status\",\"encoder_mode\":\"apple-software\"}");
    if(encoder.macEncoderMode!=MacVideoEncoderMode::Hardware)return false;
    ApplyEncoderStatus(encoder,"{\"type\":\"host-status\",\"encoder_mode\":\"apple-software\",\"encoder_id\":\"com.apple.videotoolbox.videoencoder.h264\",\"fps\":30}");
    if(encoder.macEncoderMode!=MacVideoEncoderMode::AppleSoftware||encoderMessage(true)!="Mac 软件 H.264 编码 · Windows GPU 硬解"||
       encoderMessage(false)!="Mac 软件 H.264 编码 · 等待 Windows GPU 硬解")return false;
    ApplyEncoderStatus(encoder,"{\"type\":\"host-status\",\"encoder_mode\":\"hardware\",\"padding\":\""+std::string(16384,'x')+"\"}");
    if(encoder.macEncoderMode!=MacVideoEncoderMode::AppleSoftware)return false;
    ApplyEncoderStatus(encoder,"{\"type\":\"host-status\",\"capture_state\":\"running\"}");
    if(encoder.macEncoderMode!=MacVideoEncoderMode::Unknown||encoderMessage(true)!="H.264 编码 · Windows GPU 硬解")return false;
    ApplyEncoderStatus(encoder,"{\"type\":\"host-status\",\"encoder_mode\":\"software\"}");
    if(encoder.macEncoderMode!=MacVideoEncoderMode::Software)return false;
    ApplyEncoderStatus(encoder,"{\"type\":\"host-status\",\"encoder_mode\":\"unavailable\"}");
    if(encoder.macEncoderMode!=MacVideoEncoderMode::Unknown)return false;
    ApplyEncoderStatus(encoder,"{\"type\":\"host-status\",\"encoder_mode\":\"hardware\"}");
    ApplyEncoderStatus(encoder,"{\"type\":\"host-status\",\"encoder_mode\":\"future-codec /private/path token=synthetic\"}");
    if(encoder.macEncoderMode!=MacVideoEncoderMode::Unknown||encoderMessage(true).find("synthetic")!=std::string::npos)return false;
    ApplyEncoderStatus(encoder,"{\"type\":\"host-status\",\"encoder_mode\":\"hardware\"}");
    ApplyEncoderStatus(encoder,"{\"type\":\"host-status\",\"encoder_mode\":true}");
    if(encoder.macEncoderMode!=MacVideoEncoderMode::Unknown)return false;
    if(!ClientSession::TestCodecStateReset())return false;
    SessionSnapshot restart;
    ApplyRestartCapabilities(restart,"{\"type\":\"host-status\",\"restart_supported\":true,\"restart_requires_local_authorization\":true,\"restart_busy\":false}");
    if(!restart.restartSupported||!restart.restartRequiresLocalAuthorization||restart.restartBusy)return false;
    const std::string request="01234567-89ab-cdef-0123-456789abcdef";
    restart.restartRequestId=request;restart.restartState="queued";restart.restartBusy=true;
    auto reply=[&](const std::string& id,const char* state){return "{\"type\":\"host-command-status\",\"kind\":\"restart-mac\",\"request_id\":\""+id+"\",\"state\":\""+state+"\",\"message\":\"untrusted raw error /private/path token=synthetic\"}";};
    if(ApplyRestartStatus(restart,reply("11234567-89ab-cdef-0123-456789abcdef","requested"))||ApplyRestartStatus(restart,reply(request,"unrecognized")))return false;
    if(!ApplyRestartStatus(restart,reply(request,"authorizing"))||restart.restartState!="authorizing"||!restart.restartBusy)return false;
    ApplyRestartCapabilities(restart,"{\"type\":\"host-status\",\"restart_busy\":false}");
    if(!restart.restartBusy)return false;
    if(!ApplyRestartStatus(restart,reply(request,"cancelled"))||restart.restartBusy||restart.restartMessage!="已取消 Mac 重启")return false;
    if(!ApplyRestartStatus(restart,reply(request,"error"))||restart.restartMessage.find("private")!=std::string::npos||restart.restartMessage.find("synthetic")!=std::string::npos)return false;
    if(!ApplyRestartStatus(restart,reply(request,"requested"))||!restart.restartBusy)return false;
    if(ValidRequestId("not-a-uuid")||!ValidRequestId(request))return false;
    if(!ClientSession::TestRetainedBulkLifetime())return false;
    if(sizeof(RemoteFrameHeader)!=32||sizeof(RemoteInputEvent)!=32||sizeof(RemoteCursorHeader)!=24)return false;
    Api api;return api.load();
}
