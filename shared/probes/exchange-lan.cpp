#include "../exchange.h"
#include "../protocol.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <CommonCrypto/CommonDigest.h>
#endif
using namespace remote_exchange;
using Clock=std::chrono::steady_clock;
namespace {
void Check(bool condition,const char* message){if(!condition){throw std::runtime_error(message);}}
std::string Json(const std::string& text){std::string escaped="\"";for(unsigned char byte:text){if(byte=='"'||byte=='\\'){escaped+='\\';escaped+=char(byte);}else if(byte<32){escaped+=' ';}else{escaped+=char(byte);}}return escaped+'"';}
std::string Utf8(const fs::path& path){auto value=path.u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};}
fs::path Path(const std::string& text){return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),text.size()));}
std::vector<uint8_t> Pattern(size_t size,uint32_t seed){std::vector<uint8_t> bytes(size);for(auto& byte:bytes){seed=seed*1664525+1013904223;byte=uint8_t(seed>>24);}return bytes;}
void Write(const fs::path& path,const std::vector<uint8_t>& bytes){fs::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary);output.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));Check(bool(output),"fixture write failed");}
std::vector<uint8_t> Read(const fs::path& path){std::ifstream input(path,std::ios::binary);Check(bool(input),"delivered file missing");return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};}
std::string Digest(const std::vector<uint8_t>& bytes){std::array<uint8_t,32> digest{};
#ifdef _WIN32
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;Check(CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT),"SHA provider failed");Check(CryptCreateHash(provider,CALG_SHA_256,0,0,&hash),"SHA failed");Check(CryptHashData(hash,bytes.data(),DWORD(bytes.size()),0),"SHA add failed");DWORD size=DWORD(digest.size());Check(CryptGetHashParam(hash,HP_HASHVAL,digest.data(),&size,0),"SHA finish failed");CryptDestroyHash(hash);CryptReleaseContext(provider,0);
#else
    CC_SHA256(bytes.data(),CC_LONG(bytes.size()),digest.data());
#endif
    const char* hex="0123456789abcdef";std::string result;for(auto byte:digest){result+=hex[byte>>4];result+=hex[byte&15];}return result;
}
struct Api {
    decltype(&remote_server_start) serverStart{};decltype(&remote_client_start) clientStart{};
    decltype(&remote_status) status{};decltype(&remote_bulk_status) bulkStatus{};decltype(&remote_bulk_send) bulkSend{};
    decltype(&remote_bulk_poll) bulkPoll{};decltype(&remote_send) send{};decltype(&remote_poll) poll{};
    decltype(&remote_rtt_ms) rtt{};decltype(&remote_close) close{};
#ifdef _WIN32
    HMODULE module{};
    template<class Function> void Load(Function& function,const char* name){auto address=GetProcAddress(module,name);Check(address!=nullptr,"transport DLL lacks required export");static_assert(sizeof(function)==sizeof(address));std::memcpy(&function,&address,sizeof(function));}
    Api(){module=LoadLibraryW(L"remote_transport.dll");Check(module!=nullptr,"cannot load remote_transport.dll beside probe");Load(serverStart,"remote_server_start");Load(clientStart,"remote_client_start");Load(status,"remote_status");Load(bulkStatus,"remote_bulk_status");Load(bulkSend,"remote_bulk_send");Load(bulkPoll,"remote_bulk_poll");Load(send,"remote_send");Load(poll,"remote_poll");Load(rtt,"remote_rtt_ms");Load(close,"remote_close");}
    ~Api(){if(module){FreeLibrary(module);}}
#else
    Api():serverStart(remote_server_start),clientStart(remote_client_start),status(remote_status),bulkStatus(remote_bulk_status),bulkSend(remote_bulk_send),bulkPoll(remote_bulk_poll),send(remote_send),poll(remote_poll),rtt(remote_rtt_ms),close(remote_close){}
#endif
};
struct Endpoint {Api& api;remote_endpoint* pointer;~Endpoint(){if(pointer){api.close(pointer);}}};
constexpr size_t pngSize=9*1024*1024+71,windowsFileSize=16*1024*1024+109,macFileSize=8*1024*1024+307;
std::vector<uint8_t> Text(bool server){std::string value=server?"Mac probe 中文🙂\nclipboard":"Windows probe 中文🙂\nclipboard";return {value.begin(),value.end()};}
void Fixture(const fs::path& root,bool server){Write(root/"large.bin",Pattern(server?macFileSize:windowsFileSize,server?37:53));Write(root/fs::path(u8"子目录/中文.txt"),Pattern(419,71));Write(root/"empty.bin",{});fs::create_directories(root/"empty-folder");}
void Validate(const fs::path& root,bool server){Check(Digest(Read(root/"large.bin"))==Digest(Pattern(server?macFileSize:windowsFileSize,server?37:53)),"file SHA256 mismatch");Check(Read(root/fs::path(u8"子目录/中文.txt"))==Pattern(419,71),"Unicode nested file mismatch");Check(fs::file_size(root/"empty.bin")==0&&fs::is_directory(root/"empty-folder"),"empty file/folder missing");}
}
int main(int argc,char** argv){std::cout.setf(std::ios::unitbuf);try{
    Check(argc==4,"usage: exchange-lan server|client numeric_IP:47995 isolated_stage_directory");std::string role=argv[1];bool server=role=="server";Check(server||role=="client","unknown role");
    const char* token=std::getenv("REMOTE_TEST_TOKEN");Check(token&&std::strlen(token)>=6,"REMOTE_TEST_TOKEN required (test-only pairing credential)");
    auto stage=fs::absolute(Path(argv[3]));fs::create_directories(stage);auto receive=stage/"receive";fs::create_directories(receive);auto source=stage/(server?"mac-fixture":"windows-fixture");Fixture(source,server);
    Api api;Endpoint endpoint{api,server?api.serverStart(argv[2],token,Utf8(stage/"identity").c_str()):api.clientStart(argv[2],token,nullptr)};Check(endpoint.pointer!=nullptr,"endpoint startup failed");
    auto session=std::make_unique<Session>(server,receive,[&](const uint8_t* data,size_t size){return api.bulkSend(endpoint.pointer,data,size);});
    std::cout<<"{\"phase\":\"ready_to_connect\",\"role\":"<<Json(role)<<"}\n";
    auto begin=Clock::now(),deadline=begin+std::chrono::seconds(120),nextInput=begin,completeSince=Clock::time_point{},nextStatus=begin,nextSharingStatus=begin;
    bool active=false,textRx=false,pngRx=false,textAck=false,pngAck=false,filesStarted=false,fileRx=false,fileTx=false;
    uint64_t textId=0,pngId=0,fileId=0,offerId=0,inputSequence=0,inputRx=0,bulkFrames=0,bulkBytes=0;
    double maxRtt=0,sumRtt=0;uint64_t rttSamples=0;std::string transferredHash;
    std::vector<uint8_t> bulk(1024*1024),control(8*1024*1024),pending;
    while(Clock::now()<deadline){
        if(Clock::now()>=nextStatus){std::cout<<"{\"phase\":\"progress\",\"transport\":"<<api.status(endpoint.pointer)<<",\"bulk\":"<<api.bulkStatus(endpoint.pointer)<<",\"bulk_frames\":"<<bulkFrames<<",\"text_rx\":"<<(textRx?"true":"false")<<",\"png_rx\":"<<(pngRx?"true":"false")<<",\"file_rx\":"<<(fileRx?"true":"false")<<",\"file_tx\":"<<(fileTx?"true":"false")<<"}\n";nextStatus=Clock::now()+std::chrono::seconds(5);}
        Check(api.status(endpoint.pointer)>=0||completeSince!=Clock::time_point{},"remote transport failed");bool connected=api.status(endpoint.pointer)==1&&api.bulkStatus(endpoint.pointer)==1;
        if(connected!=active){session->SetConnected(connected);active=connected;if(!active){pending.clear();}}
        if(!active&&completeSince==Clock::time_point{}){std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
        if(!textId){textId=session->PublishClipboard(ClipboardKind::Text,Text(server));}
        if(!pending.empty()&&session->Receive(pending.data(),pending.size())){pending.clear();}
        for(int n=0;n<8&&pending.empty();++n){int size=api.bulkPoll(endpoint.pointer,bulk.data(),bulk.size());if(size<=0){break;}++bulkFrames;bulkBytes+=uint64_t(size);if(!session->Receive(bulk.data(),size)){pending.assign(bulk.begin(),bulk.begin()+size);}}
        ClipboardPacket packet;while(session->TakeClipboard(packet)){if(packet.kind==ClipboardKind::Text){Check(packet.data==Text(!server),"cross-platform text changed");textRx=true;}else{Check(packet.data.size()==pngSize&&Digest(packet.data)==Digest(Pattern(pngSize,server?83:97)),"cross-platform PNG payload changed");pngRx=true;}session->ClipboardInstalled(packet.id,true);}
        auto snapshot=session->GetSnapshot();if(snapshot.clipboardAcknowledged==textId){textAck=true;}
        if(Clock::now()>=nextSharingStatus){std::cout<<"{\"phase\":\"sharing\",\"message\":"<<Json(snapshot.message)<<",\"text_ack\":"<<(textAck?"true":"false")<<",\"png_ack\":"<<(pngAck?"true":"false")<<",\"offer\":"<<(snapshot.remoteOffer.id?"true":"false")<<",\"jobs\":[";bool first=true;for(const auto& task:snapshot.transfers){std::cout<<(first?"":",")<<"{\"outgoing\":"<<(task.outgoing?"true":"false")<<",\"state\":"<<Json(StateLabel(task.state))<<",\"bytes\":"<<task.bytes<<",\"total\":"<<task.total<<"}";first=false;}std::cout<<"]}\n";nextSharingStatus=Clock::now()+std::chrono::seconds(5);}
        if(textAck&&!pngId){pngId=session->PublishClipboard(ClipboardKind::Png,Pattern(pngSize,server?97:83));}
        if(pngId&&snapshot.clipboardAcknowledged==pngId){pngAck=true;}
        if(textRx&&pngRx&&pngAck&&!filesStarted){filesStarted=true;if(server){offerId=session->OfferFiles({source});}else{fileId=session->SendFiles({source});}}
        if(!server&&filesStarted&&!offerId&&snapshot.remoteOffer.id){offerId=snapshot.remoteOffer.id;session->RequestFiles(offerId);}
        for(const auto& transfer:snapshot.transfers){if(transfer.state==TransferState::Error){throw std::runtime_error("engine transfer failed: "+transfer.error);}if(transfer.state!=TransferState::Complete){continue;}if(transfer.outgoing){fileTx=true;}else if(!fileRx){Check(transfer.files.size()==1,"completed root output missing");Validate(transfer.files[0],!server);transferredHash=Digest(Read(transfer.files[0]/"large.bin"));fileRx=true;}}
        if(Clock::now()>=nextInput){RemoteInputEvent event{};event.type=INPUT_MOVE;event.x=event.y=0.5f;event.sequence=++inputSequence;(void)api.send(endpoint.pointer,REMOTE_INPUT,reinterpret_cast<const uint8_t*>(&event),sizeof(event));nextInput=Clock::now()+std::chrono::milliseconds(50);}
        for(int n=0;n<16;++n){uint32_t channel=0;int size=api.poll(endpoint.pointer,&channel,control.data(),control.size());if(size<=0){break;}if(channel==REMOTE_INPUT&&size==int(sizeof(RemoteInputEvent))){++inputRx;}}
        double sample=api.rtt(endpoint.pointer);if(sample>0){maxRtt=std::max(maxRtt,sample);sumRtt+=sample;++rttSamples;}
        if(fileRx&&fileTx&&textRx&&textAck&&pngRx&&pngAck&&inputRx>0&&completeSince==Clock::time_point{}){completeSince=Clock::now();}
        // Let the receiver's queued final ACK reach its peer before stopping the
        // engine. Snapshot completion itself does not mean that ACK was sent.
        if(completeSince!=Clock::time_point{}&&Clock::now()-completeSince>std::chrono::milliseconds(750)){Validate(source,server);double seconds=std::chrono::duration<double>(Clock::now()-begin).count();session.reset();
            std::cout<<"{\"status\":\"PASS\",\"role\":"<<Json(role)<<",\"checks\":{\"text_bidirectional\":true,\"png_9MiB_bidirectional\":true,\"clipboard_install_ack\":true,\"file_folder_bidirectional\":true,\"nested_unicode\":true,\"empty_file_folder\":true,\"sha256_verified\":true,\"source_unchanged\":true},\"bulk_frames\":"<<bulkFrames<<",\"bulk_bytes\":"<<bulkBytes<<",\"input_events_received\":"<<inputRx<<",\"input_rtt_mean_ms\":"<<(rttSamples?sumRtt/rttSamples:0)<<",\"input_rtt_max_ms\":"<<maxRtt<<",\"elapsed_seconds\":"<<seconds<<",\"file_sha256\":"<<Json(transferredHash)<<"}\n";return 0;
        }
        (void)fileId;std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("LAN probe timeout: clipboard/file/ACK checks incomplete");
}catch(const std::exception& error){std::cout<<"{\"status\":\"FAIL\",\"error\":"<<Json(error.what())<<"}\n";return 1;}}
