#include "../exchange.h"
#include <windows.h>
#include <wincrypt.h>
#include <winioctl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef REMOTE_EXCHANGE_TEST_HOOKS
namespace {
std::atomic<bool> sharingCheckArmed{false},sharingCheckHeld{false},sharingCheckRelease{false};
}
void RemoteExchangeTestAfterSharingCheck(bool host,bool enabled){
    if(!host&&!enabled&&sharingCheckArmed.exchange(false)){
        sharingCheckHeld=true;
        while(!sharingCheckRelease.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
#endif

using namespace remote_exchange;
using namespace std::chrono_literals;
namespace {
void Check(bool condition,const char* message){if(!condition){throw std::runtime_error(message);}}
template<class Predicate> void Wait(Predicate predicate,const char* message,int seconds=15){
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    while(!predicate()){Check(std::chrono::steady_clock::now()<end,message);std::this_thread::sleep_for(2ms);}
}
std::vector<uint8_t> Pattern(size_t count,uint32_t seed=17){std::vector<uint8_t> data(count);for(auto& byte:data){seed=seed*1664525+1013904223;byte=uint8_t(seed>>24);}return data;}
void Write(const fs::path& path,const std::vector<uint8_t>& data){fs::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()));Check(bool(file),"fixture write failed");}
std::vector<uint8_t> Read(const fs::path& path){std::ifstream file(path,std::ios::binary);Check(bool(file),"delivered file missing");return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};}
std::string Utf8(const fs::path& path){auto text=path.u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};}
std::string Digest(const std::vector<uint8_t>& data){
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;Check(CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT),"independent SHA provider failed");
    Check(CryptCreateHash(provider,CALG_SHA_256,0,0,&hash),"independent SHA failed");Check(CryptHashData(hash,data.data(),DWORD(data.size()),0),"independent SHA add failed");
    uint8_t bytes[32]{};DWORD count=32;Check(CryptGetHashParam(hash,HP_HASHVAL,bytes,&count,0),"independent SHA end failed");CryptDestroyHash(hash);CryptReleaseContext(provider,0);
    std::string result;const char* hex="0123456789abcdef";for(auto byte:bytes){result+=hex[byte>>4];result+=hex[byte&15];}return result;
}
std::optional<Transfer> Job(Session& session,uint64_t id){for(auto& transfer:session.GetSnapshot().transfers){if(transfer.id==id){return transfer;}}return {};}
Transfer Complete(Session& session,uint64_t id){Transfer result;Wait([&]{auto job=Job(session,id);if(!job){return false;}if(job->state==TransferState::Error){throw std::runtime_error("transfer failed: "+job->error);}if(job->state!=TransferState::Complete){return false;}result=*job;return true;},"transfer completion timeout",30);return result;}
struct Pair {
    fs::path root,aDirectory,bDirectory;std::unique_ptr<Session> a,b;
    std::atomic<bool> link{true},blockA{false},blockB{false};std::atomic<uint64_t> framesA{0},framesB{0};
    std::atomic<bool> holdFile{false};std::mutex heldMutex;std::deque<std::vector<uint8_t>> held;
    std::atomic<bool> holdPauseReturn{false},pauseReturnHeld{false},resumedRequestQueued{false};
    Pair(){root=fs::temp_directory_path()/fs::path("remote-exchange-probe-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+"-"+std::to_string(serial.fetch_add(1)));aDirectory=root/"receive-a";bDirectory=root/"receive-b";fs::create_directories(aDirectory);fs::create_directories(bDirectory);
        a=std::make_unique<Session>(false,aDirectory,[this](const uint8_t* data,size_t size){if(!link){return -1;}if(blockA){return 0;}uint32_t type=0;if(size>=8){std::memcpy(&type,data+4,4);}if(holdFile&&type>=14&&type<=17){std::lock_guard lock(heldMutex);held.emplace_back(data,data+size);return 1;}if(b->Receive(data,size)){++framesA;if(type==19&&holdPauseReturn){pauseReturnHeld=true;while(holdPauseReturn&&link)std::this_thread::sleep_for(1ms);}return 1;}return 0;});
        b=std::make_unique<Session>(true,bDirectory,[this](const uint8_t* data,size_t size){if(!link){return -1;}if(blockB){return 0;}if(a->Receive(data,size)){++framesB;uint32_t type=0;uint64_t epoch=0;if(size>=24){std::memcpy(&type,data+4,4);std::memcpy(&epoch,data+16,8);}if(type==23&&epoch>1)resumedRequestQueued=true;return 1;}return 0;});
        a->SetConnected(true);b->SetConnected(true);
    }
    ~Pair(){link=false;a->SetConnected(false);b->SetConnected(false);a.reset();b.reset();std::error_code ec;fs::remove_all(root,ec);}
    void Disconnect(){link=false;a->SetConnected(false);b->SetConnected(false);}
    void Reconnect(){a->SetConnected(true);b->SetConnected(true);link=true;}
    static std::atomic<uint64_t> serial;
};
std::atomic<uint64_t> Pair::serial{0};
struct Wire {
    std::vector<uint8_t> data;
    Wire(uint32_t type,uint64_t id){U32(0x31455852);U32(type);U64(id);}
    void U32(uint32_t value){for(int n=0;n<4;++n){data.push_back(uint8_t(value>>(8*n)));}}
    void U64(uint64_t value){for(int n=0;n<8;++n){data.push_back(uint8_t(value>>(8*n)));}}
    void String(const std::string& value){U32(uint32_t(value.size()));data.insert(data.end(),value.begin(),value.end());}
    void Deliver(Session& session){Check(session.Receive(data.data(),data.size()),"test packet rejected by bounded inbox");}
};
void RawOffer(Session& session,uint64_t id,const std::vector<FileEntry>& entries,uint32_t count=UINT32_MAX){uint64_t total=0;for(auto& entry:entries){total+=entry.size;}
    Wire start(10,id);start.U64(total);start.U32(count==UINT32_MAX?uint32_t(entries.size()):count);start.U32(1);start.Deliver(session);
    for(auto& entry:entries){Wire item(11,id);item.String(entry.name);item.U64(entry.size);item.U32(entry.directory?1:0);item.Deliver(session);}
    Wire(12,id).Deliver(session);
}
void ClipboardTextAndPng(){Pair pair;std::string text="中文🙂\nline two\tcode";std::vector<uint8_t> bytes(text.begin(),text.end());
    auto id=pair.a->PublishClipboard(ClipboardKind::Text,bytes);ClipboardPacket packet;Wait([&]{return pair.b->TakeClipboard(packet);},"text synchronization timeout");Check(packet.id==id&&packet.kind==ClipboardKind::Text&&packet.data==bytes,"Unicode clipboard altered");pair.b->ClipboardInstalled(id,true);Wait([&]{return pair.a->GetSnapshot().clipboardAcknowledged==id;},"clipboard installed ACK missing");
    auto image=Pattern(1024*1024+31);id=pair.b->PublishClipboard(ClipboardKind::Png,image);Wait([&]{return pair.a->TakeClipboard(packet);},"PNG synchronization timeout");Check(packet.id==id&&packet.kind==ClipboardKind::Png&&packet.data==image,"lossless clipboard bytes altered");pair.a->ClipboardInstalled(id,false);Wait([&]{return pair.b->GetSnapshot().clipboardRejected==id;},"clipboard rejection ACK missing");
}
void ClipboardLargeAndPressure(){Pair pair;pair.blockA=true;auto old=pair.a->PublishClipboard(ClipboardKind::Text,Pattern(8*1024*1024+17));std::this_thread::sleep_for(25ms);auto data=Pattern(9*1024*1024+73,83);auto latest=pair.a->PublishClipboard(ClipboardKind::Png,data);Check(latest>old,"clipboard IDs not monotonic");
    auto before=std::chrono::steady_clock::now();for(int n=0;n<100;++n){(void)pair.a->GetSnapshot();pair.a->SetNetworkRtt(1.5);}Check(std::chrono::steady_clock::now()-before<250ms,"public UI operations blocked behind backpressure");
    pair.blockA=false;ClipboardPacket packet;Wait([&]{return pair.b->TakeClipboard(packet);},"large clipboard timeout");Check(packet.id==latest&&packet.data==data,"superseded/fragmented clipboard corrupted");Check(pair.framesA>100,"large clipboard was not incrementally framed");
    Check(pair.a->PublishClipboard(ClipboardKind::Text,Pattern(32*1024*1024+1))==0,"clipboard resource maximum not enforced");
}
void FolderEmptyClashAndHash(){Pair pair;auto tree=pair.root/"source"/"tree";auto content=Pattern(8*1024*1024+4097);Write(tree/"large.bin",content);Write(tree/fs::path(u8"子目录/中文.txt"),Pattern(411,91));Write(tree/"empty.bin",{});fs::create_directories(tree/"empty-folder");Write(pair.bDirectory/"tree"/"sentinel",{7,8,9});
    auto offer=pair.a->OfferFiles({tree});Wait([&]{return pair.b->GetSnapshot().remoteOffer.id==offer;},"folder offer missing");auto transfer=pair.b->RequestFiles(offer);auto delivered=Complete(*pair.b,transfer);(void)Complete(*pair.a,transfer);Check(delivered.files.size()==1&&delivered.bytes==delivered.total,"folder result missing or size wrong");
    Check(Read(pair.bDirectory/"tree"/"sentinel")==std::vector<uint8_t>({7,8,9}),"same-name existing file overwritten");Check(delivered.files.front().filename()!=tree.filename(),"clash was not renamed");Check(Digest(Read(delivered.files.front()/"large.bin"))==Digest(content),"delivered SHA256 mismatch");Check(fs::file_size(delivered.files.front()/"empty.bin")==0&&fs::is_directory(delivered.files.front()/"empty-folder"),"empty file/folder missing");
    uint8_t sample[37]{};Check(pair.b->ReadTransferFile(transfer,"tree/large.bin",73,sample,sizeof(sample),100)==37,"virtual-file completed read failed");Check(std::equal(std::begin(sample),std::end(sample),content.begin()+73),"virtual-file clash path mapping wrong");Check(pair.b->ReadTransferFile(transfer,"tree/empty.bin",0,sample,sizeof(sample),100)==0,"empty virtual file did not EOF");
    auto download=pair.a->DownloadFiles({Utf8(delivered.files.front()/"large.bin")});auto back=Complete(*pair.a,download);Check(back.files.size()==1&&Digest(Read(back.files.front()))==Digest(content),"reverse selected download corrupted");Check(pair.b->GetSnapshot().remoteOffer.id==offer,"selected download replaced file clipboard");
}
void PauseResumeAndCancel(){Pair pair;auto path=pair.root/"source"/"resume.bin";auto content=Pattern(12*1024*1024+13);Write(path,content);pair.a->SetSpeedLimit(2*1024*1024);
    auto offer=pair.a->OfferFiles({path});Wait([&]{return pair.b->GetSnapshot().remoteOffer.id==offer;},"resume offer missing");auto transfer=pair.b->RequestFiles(offer);Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->bytes>=128*1024;},"resume transfer did not start");pair.Disconnect();Check(Job(*pair.b,transfer)->state==TransferState::Paused,"disconnect did not pause receiver");pair.Reconnect();pair.b->Resume(transfer);auto done=Complete(*pair.b,transfer);Check(done.files.size()==1&&Digest(Read(done.files[0]))==Digest(content),"disconnect/resume SHA mismatch");
    offer=pair.a->OfferFiles({path});Wait([&]{return pair.b->GetSnapshot().remoteOffer.id==offer;},"fresh cancel offer missing");auto second=pair.b->RequestFiles(offer);Wait([&]{auto job=Job(*pair.b,second);return job&&job->bytes>=64*1024;},"cancel transfer did not start");pair.b->Cancel(second);Wait([&]{auto job=Job(*pair.a,second);return job&&job->state==TransferState::Cancelled;},"cancel did not reach source");Check(Job(*pair.b,second)->state==TransferState::Cancelled,"receiver cancel state wrong");Check(!fs::exists(pair.bDirectory/(".remote-part-"+std::to_string(second))),"cancel left staging content");Check(Read(path)==content,"cancel removed/changed source");
}
void SourceMutationRejected(){Pair pair;auto path=pair.root/"source"/"changed.bin";auto data=Pattern(3*1024*1024);Write(path,data);pair.a->SetSpeedLimit(512*1024);auto offer=pair.a->OfferFiles({path});Wait([&]{return pair.b->GetSnapshot().remoteOffer.id==offer;},"mutation offer missing");auto transfer=pair.b->RequestFiles(offer);Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->bytes>=65536;},"mutation transfer did not start");pair.Disconnect();auto timestamp=fs::last_write_time(path);data[0]^=127;Write(path,data);fs::last_write_time(path,timestamp+std::chrono::seconds(5));pair.Reconnect();pair.b->Resume(transfer);Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->state==TransferState::Error;},"changed source resumed without error");Check(!fs::exists(pair.bDirectory/"changed.bin"),"changed source published incomplete final file");}
void MalformedManifestRejected(){Pair pair;uint64_t id=UINT64_C(0xf000000000000000);std::vector<std::vector<FileEntry>> invalid={{{"../escape",0,false}},{{"C:/escape",0,false}},{{"/absolute",0,false}},{{"dir//file",0,false}},{{"CON.txt",0,false}},{{"x",0,false},{"X",0,false}},{{"caf\xc3\xa9",0,false},{"cafe\xcc\x81",0,false}},{{"file",0,false},{"file/child",0,false}},{{"parent",0,true},{"Parent/child",0,false}},{{std::string("bad\xff",4),0,false}}};
    invalid.push_back({{"\xcf\x83",0,false},{"\xcf\x82",0,false}}); // Sigma/final-sigma Windows ordinal alias.
    for(auto& entries:invalid){RawOffer(*pair.b,++id,entries);std::this_thread::sleep_for(20ms);Check(pair.b->GetSnapshot().remoteOffer.id!=id,"malformed/colliding manifest accepted");}
    RawOffer(*pair.b,++id,{{"short",0,false}},2);std::this_thread::sleep_for(20ms);Check(pair.b->GetSnapshot().remoteOffer.id!=id,"declared manifest count not enforced");RawOffer(*pair.b,++id,{},20001);std::this_thread::sleep_for(20ms);Check(pair.b->GetSnapshot().remoteOffer.id!=id,"manifest entry maximum not enforced");Check(!fs::exists(pair.root/"escape"),"malformed path escaped destination");
}
void BrowseAndDirectUpload(){Pair pair;auto file=pair.root/"source"/"selected.txt";Write(file,Pattern(8193));auto clipboardOffer=pair.a->OfferFiles({file});Wait([&]{return pair.b->GetSnapshot().remoteOffer.id==clipboardOffer;},"clipboard offer missing");auto transfer=pair.a->SendFiles({file});auto delivered=Complete(*pair.b,transfer);Check(delivered.files.size()==1&&Read(delivered.files.front())==Read(file),"selected upload corrupted");Check(pair.b->GetSnapshot().remoteOffer.id==clipboardOffer,"direct upload replaced clipboard offer");auto request=pair.a->Browse(Utf8(pair.bDirectory));Directory directory;Wait([&]{return pair.a->TakeDirectory(directory);},"directory browsing timeout");Check(directory.requestId==request&&directory.error.empty()&&!directory.entries.empty(),"directory result wrong");}
void Junction(const fs::path& link,const fs::path& destination){
    fs::create_directory(link);auto target=fs::absolute(destination).native();auto substitute=L"\\??\\"+target;std::vector<uint8_t> buffer(16+(substitute.size()+1+target.size()+1)*sizeof(wchar_t));
    auto u16=[&](size_t offset,uint16_t value){std::memcpy(buffer.data()+offset,&value,sizeof(value));};DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;std::memcpy(buffer.data(),&tag,sizeof(tag));u16(4,uint16_t(buffer.size()-8));u16(8,0);u16(10,uint16_t(substitute.size()*2));u16(12,uint16_t((substitute.size()+1)*2));u16(14,uint16_t(target.size()*2));
    std::memcpy(buffer.data()+16,substitute.c_str(),(substitute.size()+1)*2);std::memcpy(buffer.data()+16+(substitute.size()+1)*2,target.c_str(),(target.size()+1)*2);
    HANDLE handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);Check(handle!=INVALID_HANDLE_VALUE,"junction fixture handle failed");DWORD returned=0;BOOL result=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,buffer.data(),DWORD(buffer.size()),nullptr,0,&returned,nullptr);CloseHandle(handle);Check(result,"junction fixture creation failed");
}
void StagingAndInnerJunctionRejected(){Pair pair;auto outside=pair.root/"outside-stage";Write(outside/"sentinel",{1,3,5});uint64_t offer=UINT64_C(0xf100000000000000),transfer=UINT64_C(0xf200000000000000);RawOffer(*pair.b,offer,{{"escape.txt",1,false}});Wait([&]{return pair.b->GetSnapshot().remoteOffer.id==offer;},"staging attack offer missing");auto foreign=pair.bDirectory/(".remote-part-"+std::to_string(transfer));Junction(foreign,outside);
    Wire upload(35,transfer);upload.U64(offer);upload.String("");upload.Deliver(*pair.b);Wait([&]{return !pair.b->GetSnapshot().message.empty();},"foreign staging junction was adopted");Check(!Job(*pair.b,transfer),"foreign staging produced a receiving job");Check(!fs::exists(outside/"escape.txt")&&Read(outside/"sentinel")==std::vector<uint8_t>({1,3,5}),"foreign staging changed outside content");Check(fs::remove(foreign),"junction fixture cleanup failed");
    offer+=1;RawOffer(*pair.b,offer,{{"dir",0,true},{"dir/file.bin",1,false}});Wait([&]{return pair.b->GetSnapshot().remoteOffer.id==offer;},"inner junction offer missing");pair.blockB=true;transfer=pair.b->RequestFiles(offer);Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->state==TransferState::Running;},"inner junction receiver not running");auto inner=pair.bDirectory/(".remote-part-"+std::to_string(transfer))/"dir";Junction(inner,outside);
    Wire start(14,transfer);start.U64(1);start.String("dir/file.bin");start.U64(1);start.U32(0);start.U64(0);start.Deliver(*pair.b);Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->state==TransferState::Error;},"inner junction FileStart was accepted");Check(!fs::exists(outside/"file.bin")&&Read(outside/"sentinel")==std::vector<uint8_t>({1,3,5}),"FileStart escaped staging");Check(fs::remove(inner),"inner junction cleanup failed");
}
void DisableSharingPausesAndResumes(){Pair pair;auto path=pair.root/"source"/"sharing-toggle.bin";auto data=Pattern(3*1024*1024+17);Write(path,data);pair.a->SetSpeedLimit(512*1024);auto offer=pair.a->OfferFiles({path});Wait([&]{return pair.b->GetSnapshot().remoteOffer.id==offer;},"toggle offer missing");auto transfer=pair.b->RequestFiles(offer);Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->bytes>=65536;},"toggle did not transfer");pair.a->SetEnabled(false);Wait([&]{auto job=Job(*pair.a,transfer);return job&&job->state==TransferState::Paused&&!pair.a->GetSnapshot().enabled;},"disabled sharing did not pause source");Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->state==TransferState::Paused;},"peer did not pause");auto bytes=Job(*pair.a,transfer)->bytes;std::this_thread::sleep_for(std::chrono::milliseconds(100));Check(Job(*pair.a,transfer)->bytes==bytes,"disabled service still read source data");pair.a->SetEnabled(true);Wait([&]{return pair.a->GetSnapshot().enabled;},"sharing reenable missing");pair.b->Resume(transfer);auto complete=Complete(*pair.b,transfer);(void)Complete(*pair.a,transfer);Check(complete.files.size()==1&&Digest(Read(complete.files.front()))==Digest(data),"reenabled sharing corrupt");Check(Read(path)==data,"toggle changed source");}
void EnableBeforeQueuedResume(){
    Pair pair;auto path=pair.root/"source"/"queued-enable.bin";auto data=Pattern(256*1024+17);Write(path,data);
    pair.a->SetSpeedLimit(512*1024);auto offer=pair.a->OfferFiles({path});
    Wait([&]{return pair.b->GetSnapshot().remoteOffer.id==offer;},"queued-enable offer missing");auto transfer=pair.b->RequestFiles(offer);
    Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->bytes>=65536;},"queued-enable transfer did not start");
    // Hold only the source's synthetic transport callback, outside its session
    // mutex. Queue enable and the complete peer resume request before releasing
    // it, so the test covers a specific ordering without sleeps or CPU pressure.
    pair.holdPauseReturn=true;pair.a->SetEnabled(false);
    Wait([&]{return pair.pauseReturnHeld.load();},"disable pause callback not held");
    Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->state==TransferState::Paused;},"queued-enable peer did not pause");
    const uint64_t disabledOffer=UINT64_C(0xf400000000000001);
    RawOffer(*pair.a,disabledOffer,{{"disabled-must-not-reappear.txt",0,false}});
    pair.a->SetEnabled(true);pair.b->Resume(transfer);
    Wait([&]{return pair.resumedRequestQueued.load();},"resumed request was not queued");pair.holdPauseReturn=false;
    Wait([&]{auto job=Job(*pair.a,transfer);return job&&(job->state==TransferState::Running||job->state==TransferState::Queued||job->state==TransferState::Complete);},"queued enable discarded peer resume request",3);
    auto complete=Complete(*pair.b,transfer);(void)Complete(*pair.a,transfer);
    Check(complete.files.size()==1&&Digest(Read(complete.files.front()))==Digest(data),"queued-enable resume corrupted file");
    Check(Read(path)==data,"queued-enable changed source file");
    Check(pair.a->GetSnapshot().remoteOffer.id!=disabledOffer,"offer received while disabled reappeared after enable");
}
#ifdef REMOTE_EXCHANGE_TEST_HOOKS
void EnableAfterWorkerSettingCheck(){
    Pair pair;struct ReleaseHook{~ReleaseHook(){sharingCheckArmed=false;sharingCheckRelease=true;}} releaseHook;
    auto path=pair.root/"source"/"post-check-enable.bin";auto data=Pattern(256*1024+19);Write(path,data);
    pair.a->SetSpeedLimit(512*1024);auto offer=pair.a->OfferFiles({path});
    Wait([&]{return pair.b->GetSnapshot().remoteOffer.id==offer;},"post-check offer missing");auto transfer=pair.b->RequestFiles(offer);
    Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->bytes>=65536;},"post-check transfer did not start");
    pair.a->SetEnabled(false);
    Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->state==TransferState::Paused;},"post-check peer did not pause");
    sharingCheckHeld=false;sharingCheckRelease=false;sharingCheckArmed=true;
    Wait([&]{return sharingCheckHeld.load();},"worker sharing checkpoint was not held");
    // SetEnabled arrives after the loop's initial settings check, before the
    // inbox batch. This is a distinct ordering from the transport callback test.
    pair.a->SetEnabled(true);pair.b->Resume(transfer);
    Wait([&]{return pair.resumedRequestQueued.load();},"post-check resumed request was not queued");sharingCheckRelease=true;
    Wait([&]{auto job=Job(*pair.a,transfer);return job&&(job->state==TransferState::Running||job->state==TransferState::Queued||job->state==TransferState::Complete);},"enable after sharing check discarded resume request",3);
    auto complete=Complete(*pair.b,transfer);(void)Complete(*pair.a,transfer);
    Check(complete.files.size()==1&&Digest(Read(complete.files.front()))==Digest(data),"post-check enable corrupted file");
    Check(Read(path)==data,"post-check enable changed source file");
}
#endif
void BufferedOldEpochIgnored(){Pair pair;auto path=pair.root/"source"/"fast.bin";auto data=Pattern(3*1024*1024+31);Write(path,data);auto offer=pair.a->OfferFiles({path});Wait([&]{return pair.b->GetSnapshot().remoteOffer.id==offer;},"backlog offer missing");auto transfer=pair.b->RequestFiles(offer);Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->bytes>=65536;},"backlog transfer did not start");
    pair.holdFile=true;pair.blockB=true;pair.b->Pause(transfer);Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->state==TransferState::Paused;},"backlog receiver did not pause");
    Wait([&]{std::lock_guard lock(pair.heldMutex);for(auto& frame:pair.held){uint32_t type=0;std::memcpy(&type,frame.data()+4,4);if(type==17){return true;}}return false;},"old file end was not buffered");
    uint64_t oldEpoch=0;std::deque<std::vector<uint8_t>> old;{std::lock_guard lock(pair.heldMutex);old=std::move(pair.held);}Check(!old.empty()&&old.front().size()>=24,"old backlog empty");std::memcpy(&oldEpoch,old.front().data()+16,8);Check(oldEpoch==1,"probe epoch schema inconsistent");
    pair.b->Resume(transfer);Wait([&]{auto job=Job(*pair.b,transfer);return job&&job->state==TransferState::Running;},"receiver did not enter resumed epoch");pair.holdFile=false;pair.blockB=false;
    Wire start(14,transfer);start.U64(oldEpoch);start.String("fast.bin");start.U64(data.size());start.U32(0);start.U64(0);start.Deliver(*pair.b);
    for(auto& frame:old){Wait([&]{return pair.b->Receive(frame.data(),frame.size());},"stale backlog inbox pressure timeout");}
    Wait([&]{auto job=Job(*pair.a,transfer);return job&&job->state==TransferState::Running&&job->bytes<job->total;},"new source epoch did not start");
    for(uint32_t type:{18u,19u,20u,21u}){Wire stale(type,transfer);stale.U64(oldEpoch);if(type==21){stale.String("obsolete transfer error");}stale.Deliver(*pair.a);}
    auto result=Complete(*pair.b,transfer);(void)Complete(*pair.a,transfer);Check(result.files.size()==1&&Digest(Read(result.files.front()))==Digest(data),"old buffered payload corrupted resume");Check(Read(path)==data,"backlog replay changed source");
}
}
int main(int argc,char** argv){std::cout.setf(std::ios::unitbuf);struct Test {const char* name;void(*run)();};Test tests[]={{"text_png_install_ack",ClipboardTextAndPng},{"large_clipboard_backpressure_supersession",ClipboardLargeAndPressure},{"folder_empty_clash_sha_reverse_download",FolderEmptyClashAndHash},{"disconnect_resume_cancel",PauseResumeAndCancel},{"source_mutation_rejected",SourceMutationRejected},{"malformed_case_unicode_manifest",MalformedManifestRejected},{"browse_direct_upload_clipboard_purpose",BrowseAndDirectUpload},{"staging_reparse_and_inner_junction",StagingAndInnerJunctionRejected},{"buffered_old_epoch_pause_resume",BufferedOldEpochIgnored},{"sharing_disable_pause_resume",DisableSharingPausesAndResumes},{"queued_enable_before_peer_resume",EnableBeforeQueuedResume}
#ifdef REMOTE_EXCHANGE_TEST_HOOKS
,{"enable_after_worker_setting_check",EnableAfterWorkerSettingCheck}
#endif
};int failed=0,ran=0;
    for(auto& test:tests){if(argc>1&&std::string(argv[1])!=test.name){continue;}++ran;try{test.run();std::cout<<"PASS "<<test.name<<'\n';}catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<test.name<<": "<<error.what()<<'\n';}}
    std::cout<<"RESULT "<<(ran-failed)<<" passed, "<<failed<<" failed\n";return failed||!ran?1:0;
}
