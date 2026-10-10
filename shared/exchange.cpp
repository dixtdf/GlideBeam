#include "exchange.h"
#include "diagnostic-log.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>
#include <stdio.h>
#endif
#ifdef REMOTE_EXCHANGE_TEST_HOOKS
// Defined by the isolated core probe only; absent from production builds.
void RemoteExchangeTestAfterSharingCheck(bool host,bool enabled);
#endif

namespace remote_exchange {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kChunk = 64 * 1024, kMaxFrame = 1024 * 1024;
constexpr size_t kMaxClipboard = 32 * 1024 * 1024, kMaxInbox = 8 * 1024 * 1024;
constexpr size_t kMaxEntries = 20000, kMaxJobs = 64;
constexpr size_t kMaxManifestNames = 4 * 1024 * 1024, kMaxIncomingOffers = 4;
constexpr uint32_t kMagic = 0x31455852;
enum Message : uint32_t { Hello=1, ClipStart, ClipData, ClipEnd, ClipAck,
    OfferStart=10, OfferEntry, OfferEnd, Request, FileStart, FileData, FileEnd,
    TransferEnd, TransferAck, TransferPause, TransferCancel, TransferError,
    RequestOffset=22, RequestReady=23,
    BrowseRequest=30, BrowseStart, BrowseEntry, BrowseEnd, DownloadRequest, UploadRequest };
std::string Utf8(const fs::path& path) { auto s=path.u8string(); return {reinterpret_cast<const char*>(s.data()),s.size()}; }
fs::path Path(const std::string& text) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),text.size())); }
struct Writer {
    std::vector<uint8_t> bytes;
    Writer(uint32_t type,uint64_t id=0) { u32(kMagic);u32(type);u64(id); }
    void u32(uint32_t v) { for(int i=0;i<4;++i)bytes.push_back(uint8_t(v>>(8*i))); }
    void u64(uint64_t v) { for(int i=0;i<8;++i)bytes.push_back(uint8_t(v>>(8*i))); }
    void str(const std::string& s) { u32(uint32_t(s.size()));bytes.insert(bytes.end(),s.begin(),s.end()); }
    void raw(const uint8_t* p,size_t n) { bytes.insert(bytes.end(),p,p+n); }
};
struct Reader {
    const uint8_t* p; size_t left; bool valid=true;
    uint32_t u32() { if(left<4){valid=false;return 0;}uint32_t v=0;for(int i=0;i<4;++i)v|=uint32_t(p[i])<<(8*i);p+=4;left-=4;return v; }
    uint64_t u64() { if(left<8){valid=false;return 0;}uint64_t v=0;for(int i=0;i<8;++i)v|=uint64_t(p[i])<<(8*i);p+=8;left-=8;return v; }
    std::string str(size_t cap=32768) { auto n=u32();if(!valid||n>cap||n>left){valid=false;return {};}std::string v(reinterpret_cast<const char*>(p),n);p+=n;left-=n;if(v.find('\0')!=std::string::npos)valid=false;return v; }
    bool end() const { return valid&&left==0; }
};
class Hash {
#ifdef _WIN32
    HCRYPTPROV provider_=0; HCRYPTHASH hash_=0;
#else
    CC_SHA256_CTX hash_{};
#endif
    bool valid_=false;
public:
    Hash() {
#ifdef _WIN32
        valid_=CryptAcquireContextW(&provider_,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)&&CryptCreateHash(provider_,CALG_SHA_256,0,0,&hash_);
#else
        valid_=CC_SHA256_Init(&hash_)==1;
#endif
    }
    ~Hash() {
#ifdef _WIN32
        if(hash_) { CryptDestroyHash(hash_); }
        if(provider_) { CryptReleaseContext(provider_,0); }
#endif
    }
    bool add(const uint8_t* p,size_t n) {
#ifdef _WIN32
        return valid_&&CryptHashData(hash_,p,DWORD(n),0);
#else
        return valid_&&CC_SHA256_Update(&hash_,p,CC_LONG(n))==1;
#endif
    }
    std::string finish() {
        std::array<uint8_t,32> bytes{};
#ifdef _WIN32
        DWORD n=DWORD(bytes.size());if(!valid_||!CryptGetHashParam(hash_,HP_HASHVAL,bytes.data(),&n,0))return {};
#else
        if(!valid_||CC_SHA256_Final(bytes.data(),&hash_)!=1)return {};
#endif
        constexpr char hex[]="0123456789abcdef";std::string result;for(auto b:bytes){result.push_back(hex[b>>4]);result.push_back(hex[b&15]);}return result;
    }
};
std::string NameKey(const std::string& text) {
#ifdef _WIN32
    if(text.size()>INT_MAX)return {};
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if(count<=0)return {};
    std::wstring wide(size_t(count),L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),wide.data(),count)!=count)return {};
    using NormalizeFn=int(WINAPI*)(NORM_FORM,LPCWSTR,int,LPWSTR,int);
    static NormalizeFn normalize=[] {
        auto module=LoadLibraryExW(L"normaliz.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        NormalizeFn function=nullptr;
        if(module){auto address=GetProcAddress(module,"NormalizeString");static_assert(sizeof(function)==sizeof(address));std::memcpy(&function,&address,sizeof(function));}
        return function;
    }();
    if(!normalize)return {};
    count=normalize(NormalizationC,wide.data(),int(wide.size()),nullptr,0);
    if(count<=0)return {};
    std::wstring normalized(size_t(count),L'\0');
    count=normalize(NormalizationC,wide.data(),int(wide.size()),normalized.data(),int(normalized.size()));
    if(count<=0)return {};
    normalized.resize(size_t(count));
    // Invariant case-insensitive sort keys conservatively reject aliases across
    // Windows/macOS (including sigma/final-sigma), without changing filenames.
    constexpr DWORD keyFlags=LCMAP_SORTKEY|NORM_IGNORECASE;
    count=LCMapStringEx(LOCALE_NAME_INVARIANT,keyFlags,normalized.data(),int(normalized.size()),nullptr,0,nullptr,nullptr,0);
    if(count<=0)return {};
    std::wstring folded((size_t(count)+1)/2,L'\0');
    if(LCMapStringEx(LOCALE_NAME_INVARIANT,keyFlags,normalized.data(),int(normalized.size()),folded.data(),count,nullptr,nullptr,0)!=count)return {};
    return {reinterpret_cast<const char*>(folded.data()),size_t(count)};
#else
    auto source=CFStringCreateWithBytes(kCFAllocatorDefault,reinterpret_cast<const UInt8*>(text.data()),CFIndex(text.size()),kCFStringEncodingUTF8,false);
    if(!source)return {};
    auto folded=CFStringCreateMutableCopy(kCFAllocatorDefault,0,source);CFRelease(source);
    if(!folded)return {};
    CFStringNormalize(folded,kCFStringNormalizationFormC);
    CFStringFold(folded,kCFCompareCaseInsensitive,nullptr);
    auto size=CFStringGetMaximumSizeForEncoding(CFStringGetLength(folded),kCFStringEncodingUTF8);
    std::vector<char> buffer(size_t(size)+1);
    bool valid=CFStringGetCString(folded,buffer.data(),CFIndex(buffer.size()),kCFStringEncodingUTF8);
    CFRelease(folded);return valid?std::string(buffer.data()):std::string{};
#endif
}
bool SafeName(const std::string& name) {
    if(name.empty()||name.size()>16384||name.front()=='/'||name.front()=='\\'||name.find('\\')!=std::string::npos||name.find(':')!=std::string::npos||name.find('\0')!=std::string::npos||name.find("//")!=std::string::npos||NameKey(name).empty())return false;
    auto path=Path(name);if(path.is_absolute()||path.has_root_path())return false;
    for(const auto& component:path){auto part=Utf8(component);if(part.empty()||part=="."||part==".."||part.back()=='.'||part.back()==' '||part.size()>255)return false;
        std::string upper;for(unsigned char c:part){if(c<32||c=='<'||c=='>'||c=='"'||c=='|'||c=='?'||c=='*')return false;upper.push_back(char(c>='a'&&c<='z'?c-32:c));}
        auto dot=upper.find('.');auto stem=upper.substr(0,dot);if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL"||(stem.size()==4&&(stem.substr(0,3)=="COM"||stem.substr(0,3)=="LPT")&&stem[3]>='1'&&stem[3]<='9'))return false;
    }return true;
}
bool Contained(const fs::path& root,const fs::path& path) {
    std::error_code ec;auto canonicalRoot=fs::weakly_canonical(root,ec);if(ec)return false;auto canonicalPath=fs::weakly_canonical(path,ec);if(ec)return false;
    auto rel=canonicalPath.lexically_relative(canonicalRoot);if(rel.empty())return canonicalPath==canonicalRoot;for(const auto& part:rel)if(part=="..")return false;return !rel.is_absolute();
}
bool NoLinksWithin(const fs::path& root,const fs::path& path) {
    if(!Contained(root,path))return false;
    auto check=[](const fs::path& candidate) {
#ifdef _WIN32
        auto attributes=GetFileAttributesW(candidate.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES){auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND;}
        return (attributes&FILE_ATTRIBUTE_REPARSE_POINT)==0;
#else
        std::error_code ec;auto status=fs::symlink_status(candidate,ec);
        if(ec)return ec==std::errc::no_such_file_or_directory;
        return !fs::is_symlink(status);
#endif
    };
    if(!check(root))return false;
    auto relative=path.lexically_relative(root),current=root;
    if(relative.empty()&&path!=root)return false;
    for(const auto& part:relative){if(part=="..")return false;if(part==".")continue;current/=part;if(!check(current))return false;}
    return true;
}
bool ValidManifest(const FileOffer& offer) {
    if(offer.entries.empty()||offer.entries.size()>kMaxEntries)return false;
    size_t names=0;uint64_t bytes=0;
    std::map<std::string,std::pair<std::string,bool>> entries;
    std::map<std::string,std::string> prefixes;
    for(const auto& entry:offer.entries){
        if(!SafeName(entry.name)||entry.name.size()>kMaxManifestNames-names||(entry.directory&&entry.size)||entry.size>UINT64_MAX-bytes)return false;
        names+=entry.name.size();bytes+=entry.size;
        auto key=NameKey(entry.name);
        if(!entries.emplace(key,std::make_pair(entry.name,entry.directory)).second)return false;
        size_t end=0;
        do{end=entry.name.find('/',end);auto prefix=entry.name.substr(0,end);auto folded=NameKey(prefix);
            auto [it,inserted]=prefixes.emplace(folded,prefix);if(!inserted&&it->second!=prefix)return false;
            if(end!=std::string::npos)++end;
        }while(end!=std::string::npos);
    }
    if(bytes!=offer.bytes)return false;
    for(const auto& entry:offer.entries){auto slash=entry.name.find('/');while(slash!=std::string::npos){auto parent=entries.find(NameKey(entry.name.substr(0,slash)));if(parent==entries.end()||!parent->second.second)return false;slash=entry.name.find('/',slash+1);}}
    return true;
}
void PrivateDirectory(const fs::path& path) {
#ifndef _WIN32
    chmod(path.c_str(),0700);
#else
    (void)path;
#endif
}
struct Item { FileEntry entry;fs::path source; fs::file_time_type modified{}; };
struct Offer { FileOffer info;std::vector<Item> items; };
struct IncomingOffer { FileOffer info;uint32_t expected=0;size_t nameBytes=0; };
struct Job {
    Transfer info; Offer source; size_t index=0;uint64_t offset=0,epoch=0;
    std::ifstream input;std::ofstream output;std::unique_ptr<Hash> hash;
    fs::path staging; std::map<std::string,std::string> roots;
    std::map<std::string,uint64_t> received;
    std::vector<FileEntry> manifest;std::map<std::string,std::string> hashes;
    Clock::time_point started=Clock::now(), sample=Clock::now();uint64_t sampleBytes=0;
    bool fileStarted=false,requestPending=false;
};
struct Command { enum Type {Publish,OfferFiles,RequestFiles,SendFiles,Download,Browse,Pause,Resume,Cancel,Installed,Enabled,DirectoryChange} type;uint64_t id=0,other=0;ClipboardKind kind=ClipboardKind::Text;std::vector<uint8_t> data;std::vector<fs::path> paths;std::vector<std::string> names;fs::path destination;std::string text;bool success=true;explicit Command(Type value):type(value){} };
struct StreamTask { uint32_t type=0;uint64_t id=0,afterId=0,epoch=0;size_t offset=0;bool started=false;ClipboardKind kind=ClipboardKind::Text;std::vector<uint8_t> data;FileOffer offer;Directory directory;std::string destination;std::vector<std::pair<std::string,uint64_t>> offsets; };
struct IncomingRequest { uint64_t offerId=0,epoch=0;std::map<std::string,uint64_t> offsets;size_t nameBytes=0; };
struct ClipIncoming { ClipboardPacket packet;size_t total=0; };
bool MoveExclusive(const fs::path& source,const fs::path& destination,std::error_code& error){
#ifdef _WIN32
    if(MoveFileExW(source.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)){error.clear();return true;}
    error=std::error_code(int(GetLastError()),std::system_category());
#else
    if(renamex_np(source.c_str(),destination.c_str(),RENAME_EXCL)==0){error.clear();return true;}
    error=std::error_code(errno,std::generic_category());
#endif
    return false;
}
bool ExistsError(const std::error_code& error){
#ifdef _WIN32
    return error==std::errc::file_exists||error.value()==ERROR_ALREADY_EXISTS||error.value()==ERROR_FILE_EXISTS;
#else
    return error==std::errc::file_exists;
#endif
}
}

struct Session::Impl {
    bool host;Send send;mutable std::mutex mutex,queueMutex,snapshotMutex,eventMutex;std::condition_variable cv;bool stopping=false,connected=false,enabled=true;
    std::atomic<bool> enabledSetting{true};std::atomic<double> rttHint{-1};std::atomic<uint64_t> speedHint{0};
    // Coalesce configuration separately from the bounded work queue. A brief
    // disable followed by enable must still pause active transfers once.
    bool enabledChangePending=false,disablePending=false; // queueMutex
    std::atomic<bool> active{false}; Snapshot published; fs::path publishedDirectory;
    fs::path receiveDirectory;std::thread worker;std::deque<Command> commands;
    std::deque<std::vector<uint8_t>> inbox,outbox;size_t inboxBytes=0,outboxBytes=0,commandBytes=0;
    std::deque<ClipboardPacket> clipboard;size_t clipboardBytes=0;
    std::deque<CompletedFiles> completed;std::deque<Directory> directories;
    std::map<uint64_t,Offer> offers;std::map<uint64_t,std::unique_ptr<Job>> jobs;
    std::map<uint64_t,TransferState> loggedStates;
    std::map<uint64_t,ClipIncoming> receivingClips;std::map<uint64_t,IncomingOffer> receivingOffers;
    std::map<uint64_t,FileOffer> remoteOffers;uint64_t lastClipboardRemote=0,generation=0;
    std::map<uint64_t,IncomingRequest> receivingRequests;
    std::map<uint64_t,Directory> receivingDirectories;Snapshot snapshot;
    std::deque<StreamTask> streams;
    std::atomic<uint64_t> next;double baseline=-1;uint64_t budget=8*1024*1024;Clock::time_point nextData=Clock::now(),lastBudget=Clock::now();
    Impl(bool h,fs::path dir,Send fn):host(h),send(std::move(fn)),receiveDirectory(std::move(dir)) {
        publishedDirectory=receiveDirectory;std::random_device random;next.store((uint64_t(random())<<32)|1);worker=std::thread([this]{run();});
    }
    ~Impl(){ {std::lock_guard lock(mutex);stopping=true;cv.notify_all();}if(worker.joinable())worker.join(); }
    uint64_t id(){return next.fetch_add(1);}
    void enqueue(Command command){std::lock_guard lock(queueMutex);if(commands.size()>=256||commandBytes+command.data.size()>64*1024*1024){remote_log::Write(remote_log::Level::Warning,"files","bounded command queue full");std::lock_guard stateLock(snapshotMutex);published.message="操作队列已满，请稍后重试";return;}commandBytes+=command.data.size();commands.push_back(std::move(command));cv.notify_all();}
    void wire(Writer writer){if(writer.bytes.size()>kMaxFrame||outboxBytes+writer.bytes.size()>kMaxInbox)throw std::runtime_error("传输控制队列已满");outboxBytes+=writer.bytes.size();outbox.push_back(std::move(writer.bytes));}
    void ack(uint64_t id,bool success){Writer w(ClipAck,id);w.u32(success?1:0);wire(std::move(w));}
    void update(){
        snapshot.connected=connected;snapshot.enabled=enabled;snapshot.transfers.clear();
        for(auto& [id,job]:jobs){
            snapshot.transfers.push_back(job->info);
            auto previous=loggedStates.find(id);
            if(previous==loggedStates.end() || previous->second!=job->info.state){
                const auto& info=job->info;
                remote_log::Write(info.state==TransferState::Error?remote_log::Level::Error:remote_log::Level::Info,"files",
                    "role="+std::string(host?"host":"client")+" task="+std::to_string(id)+" state="+std::to_string(static_cast<int>(info.state))+
                    " outgoing="+std::to_string(info.outgoing)+" bytes="+std::to_string(info.bytes)+" total="+std::to_string(info.total)+
                    " error_class="+remote_log::ErrorClass(info.error));
                loggedStates[id]=info.state;
            }
        }
        for(auto it=loggedStates.begin();it!=loggedStates.end();)if(!jobs.contains(it->first))it=loggedStates.erase(it);else ++it;
        {std::lock_guard lock(snapshotMutex);published=snapshot;publishedDirectory=receiveDirectory;}cv.notify_all();
    }
    Offer enumerate(uint64_t offerId,const std::vector<fs::path>& paths) {
        Offer offer;offer.info.id=offerId;std::set<std::string> seen;size_t nameBytes=0;
        for(auto& source:paths){std::error_code ec;auto status=fs::symlink_status(source,ec);if(ec||!NoLinksWithin(source,source)||fs::is_symlink(status)||(!fs::is_directory(status)&&!fs::is_regular_file(status)))throw std::runtime_error("文件不可读取或不支持符号链接");
            auto rootName=Utf8(source.filename());if(!SafeName(rootName)||!seen.insert(rootName).second)throw std::runtime_error("文件名不兼容或重复，请重命名后发送");
            auto add=[&](const fs::path& path,const std::string& name){auto st=fs::symlink_status(path,ec);if(ec||fs::is_symlink(st)||!NoLinksWithin(source,path))throw std::runtime_error("文件夹含符号链接，未发送");if(!fs::is_regular_file(st)&&!fs::is_directory(st))throw std::runtime_error("文件夹含特殊文件，未发送");if(!SafeName(name)||offer.items.size()>=kMaxEntries||name.size()>kMaxManifestNames-nameBytes)throw std::runtime_error("文件名不兼容或文件清单超出限制（最多 20,000 项／4 MiB 名称）");nameBytes+=name.size();Item item;item.entry={name,0,fs::is_directory(st)};item.source=path;item.modified=fs::last_write_time(path,ec);if(ec)throw std::runtime_error("无法读取文件属性");if(!item.entry.directory){item.entry.size=fs::file_size(path,ec);if(ec||item.entry.size>UINT64_MAX-offer.info.bytes)throw std::runtime_error("无法读取文件大小");offer.info.bytes+=item.entry.size;}offer.info.entries.push_back(item.entry);offer.items.push_back(std::move(item));};
            add(source,rootName);if(fs::is_directory(status)){fs::recursive_directory_iterator it(source,fs::directory_options::none,ec),end;if(ec)throw std::runtime_error("无权读取文件夹");while(it!=end){auto relative=it->path().lexically_relative(source).generic_u8string();auto name=rootName+"/"+std::string(reinterpret_cast<const char*>(relative.data()),relative.size());add(it->path(),name);it.increment(ec);if(ec)throw std::runtime_error("无权读取文件夹中的文件");}}
        }if(offer.items.empty())throw std::runtime_error("没有可发送的文件");if(!ValidManifest(offer.info))throw std::runtime_error("文件名大小写或 Unicode 归一化冲突，请重命名后发送");return offer;
    }
    void emitOffer(const Offer& offer,uint64_t afterId=0,std::string destination={}){if(streams.size()>=16)throw std::runtime_error("文件清单队列已满");StreamTask task;task.type=OfferStart;task.id=offer.info.id;task.offer=offer.info;task.afterId=afterId;task.destination=std::move(destination);streams.push_back(std::move(task));}
    bool pumpStream(){if(streams.empty())return false;auto& task=streams.front();if(task.type==ClipStart){if(!task.started){Writer w(ClipStart,task.id);w.u32(uint32_t(task.kind));w.u64(task.data.size());wire(std::move(w));task.started=true;}else if(task.offset<task.data.size()){Writer w(ClipData,task.id);w.u64(task.offset);size_t n=std::min(kChunk,task.data.size()-task.offset);w.raw(task.data.data()+task.offset,n);wire(std::move(w));task.offset+=n;}else{wire(Writer(ClipEnd,task.id));streams.pop_front();}return true;}
        if(task.type==OfferStart){if(!task.started){Writer w(OfferStart,task.id);w.u64(task.offer.bytes);w.u32(uint32_t(task.offer.entries.size()));w.u32(task.offer.clipboard?1:0);wire(std::move(w));task.started=true;}else if(task.offset<task.offer.entries.size()){Writer w(OfferEntry,task.id);while(task.offset<task.offer.entries.size()&&w.bytes.size()<kChunk){auto& item=task.offer.entries[task.offset++];w.str(item.name);w.u64(item.size);w.u32(item.directory?1:0);}wire(std::move(w));}else{wire(Writer(OfferEnd,task.id));if(task.afterId){Writer w(UploadRequest,task.afterId);w.u64(task.id);w.str(task.destination);wire(std::move(w));}streams.pop_front();}return true;}
        if(task.type==Request){if(!task.started){Writer w(Request,task.id);w.u64(task.afterId);w.u64(task.epoch);w.u32(UINT32_MAX);wire(std::move(w));task.started=true;}else if(task.offset<task.offsets.size()){Writer w(RequestOffset,task.id);w.u64(task.epoch);while(task.offset<task.offsets.size()&&w.bytes.size()<kChunk){auto& item=task.offsets[task.offset++];w.str(item.first);w.u64(item.second);}wire(std::move(w));}else{Writer w(RequestReady,task.id);w.u64(task.epoch);wire(std::move(w));streams.pop_front();}return true;}
        if(task.type==BrowseStart){if(!task.started){Writer w(BrowseStart,task.id);w.str(task.directory.path);w.str(task.directory.error);wire(std::move(w));task.started=true;}else if(task.offset<task.directory.entries.size()){auto& item=task.directory.entries[task.offset++];Writer w(BrowseEntry,task.id);w.str(item.name);w.u64(item.size);w.u32(item.directory?1:0);wire(std::move(w));}else{wire(Writer(BrowseEnd,task.id));streams.pop_front();}return true;}streams.pop_front();return false;}
    void rememberOffer(const Offer& offer){
        auto names=[](const Offer& value){size_t bytes=0;for(const auto& entry:value.info.entries)bytes+=entry.name.size();return bytes;};
        size_t total=names(offer);for(const auto& [id,value]:offers){(void)id;total+=names(value);}
        while(offers.size()>=64||total>4*kMaxManifestNames){
            auto disposable=std::find_if(offers.begin(),offers.end(),[&](const auto& value){
                for(const auto& [id,job]:jobs){(void)id;auto state=job->info.state;if(job->info.offerId==value.first&&state!=TransferState::Complete&&state!=TransferState::Cancelled&&state!=TransferState::Error)return false;}
                for(const auto& task:streams)if(task.id==value.first||task.afterId==value.first)return false;
                return true;
            });
            if(disposable==offers.end())throw std::runtime_error("文件清单过多，请先完成或取消任务");
            total-=names(disposable->second);offers.erase(disposable);
        }
        offers[offer.info.id]=offer;
    }
    void prune(){for(auto it=jobs.begin();jobs.size()>=kMaxJobs&&it!=jobs.end();){auto state=it->second->info.state;if(state==TransferState::Complete||state==TransferState::Cancelled||state==TransferState::Error)it=jobs.erase(it);else ++it;}if(jobs.size()>=kMaxJobs)throw std::runtime_error("传输任务过多，请先完成或取消任务");}
    Job& createReceive(uint64_t transferId,uint64_t offerId,fs::path destination,const FileOffer& offer){
        if(!ValidManifest(offer))throw std::runtime_error("收到不兼容或超出限制的文件清单");
        prune();auto job=std::make_unique<Job>();job->info.id=transferId;job->info.offerId=offerId;job->info.total=offer.bytes;job->manifest=offer.entries;job->info.name=offer.entries.front().name;
        job->info.destination=destination.empty()?receiveDirectory:std::move(destination);std::error_code ec;
        fs::create_directories(job->info.destination,ec);if(ec)throw std::runtime_error("无法创建接收目录，请检查权限");
        job->info.destination=fs::canonical(job->info.destination,ec);if(ec)throw std::runtime_error("无法解析接收目录");
        job->staging=job->info.destination/Path(".remote-part-"+std::to_string(transferId));
        // Never adopt a foreign directory or link with a peer-controlled job ID.
        if(!NoLinksWithin(job->info.destination,job->staging)||!fs::create_directory(job->staging,ec)||ec)throw std::runtime_error("临时目录已存在或不安全，请重新发送");
        PrivateDirectory(job->staging);
        if(!NoLinksWithin(job->info.destination,job->staging))throw std::runtime_error("临时目录越界或包含符号链接");
        for(const auto& item:offer.entries){auto name=Utf8(*Path(item.name).begin());job->roots[name]=name;}
        auto& result=*job;jobs[transferId]=std::move(job);update();return result;
    }
    void rejectOffer(uint64_t id,const std::string& reason){
        receivingOffers.erase(id);snapshot.message=reason;
        for(auto& [jobId,job]:jobs){(void)jobId;if(job->requestPending&&job->info.offerId==id)fail(*job,reason);}
        update();
    }
    void request(Job& job){
        for(auto it=streams.begin();it!=streams.end();)if(it->type==Request&&it->id==job.info.id)it=streams.erase(it);else ++it;
        if(streams.size()>=16)throw std::runtime_error("续传队列已满");
        if(job.epoch==UINT64_MAX)throw std::runtime_error("续传次数已超过限制，请重新发送");
        ++job.epoch;job.output.close();job.hash.reset();job.fileStarted=false;
        StreamTask task;task.type=Request;task.id=job.info.id;task.afterId=job.info.offerId;task.epoch=job.epoch;task.offsets.assign(job.received.begin(),job.received.end());streams.push_back(std::move(task));job.info.state=TransferState::Running;job.info.error.clear();job.started=Clock::now();job.requestPending=false;update();
    }
    void fail(Job& job,const std::string& reason,bool inform=true){job.input.close();job.output.close();job.hash.reset();job.info.state=TransferState::Error;job.info.error=reason;if(inform){Writer w(TransferError,job.info.id);w.u64(job.epoch);w.str(reason);wire(std::move(w));}update();}
    void checkSource(const Item& item){std::error_code ec;auto st=fs::symlink_status(item.source,ec);if(ec||fs::is_symlink(st)||(!item.entry.directory&&!fs::is_regular_file(st))||fs::last_write_time(item.source,ec)!=item.modified||ec||(!item.entry.directory&&fs::file_size(item.source,ec)!=item.entry.size)||ec)throw std::runtime_error("源文件已改变，请重新复制或发送");}
    void handle(Command c){
        if(c.type==Command::DirectoryChange){receiveDirectory=std::move(c.destination);update();return;}
        if(c.type==Command::Enabled){enabled=c.success;snapshot.enabled=enabled;if(!enabled){streams.clear();outbox.clear();outboxBytes=0;receivingClips.clear();receivingOffers.clear();receivingRequests.clear();{std::lock_guard eventLock(eventMutex);clipboard.clear();clipboardBytes=0;}for(auto& [id,job]:jobs){(void)id;if(job->info.state==TransferState::Running||job->info.state==TransferState::Queued){job->info.state=TransferState::Paused;Writer w(TransferPause,job->info.id);w.u64(job->epoch);wire(std::move(w));}}}update();return;}
        if(c.type==Command::Installed){ack(c.id,c.success);return;}
        if(c.type==Command::Pause||c.type==Command::Resume||c.type==Command::Cancel){auto it=jobs.find(c.id);if(it==jobs.end())return;auto& job=*it->second;if(job.info.state==TransferState::Complete||job.info.state==TransferState::Cancelled||job.info.state==TransferState::Error)return;if(c.type==Command::Pause){job.info.state=TransferState::Paused;job.output.flush();Writer w(TransferPause,c.id);w.u64(job.epoch);wire(std::move(w));}else if(c.type==Command::Cancel){job.info.state=TransferState::Cancelled;job.input.close();job.output.close();Writer w(TransferCancel,c.id);w.u64(job.epoch);wire(std::move(w));std::error_code ec;if(!job.info.outgoing&&Contained(job.info.destination,job.staging))fs::remove_all(job.staging,ec);}else if(connected&&enabled&&job.info.state==TransferState::Paused){if(job.info.outgoing){Writer w(TransferPause,c.id);w.u64(job.epoch);w.u32(1);wire(std::move(w));}else request(job);}update();return;}
        if(!connected||!enabled){snapshot.message="文件与剪贴板通道未连接";return;}
        if(c.type==Command::Publish){if(c.data.size()>kMaxClipboard)throw std::runtime_error("剪贴板内容超过 32 MiB 限制");for(auto it=streams.begin();it!=streams.end();)if(it->type==ClipStart)it=streams.erase(it);else ++it;StreamTask task;task.type=ClipStart;task.id=c.id;task.kind=c.kind;task.data=std::move(c.data);streams.push_front(std::move(task));return;}
        if(c.type==Command::OfferFiles||c.type==Command::SendFiles){auto offer=enumerate(c.id,c.paths);offer.info.clipboard=c.type==Command::OfferFiles;rememberOffer(offer);emitOffer(offer,c.type==Command::SendFiles?c.other:0,c.text);return;}
        if(c.type==Command::RequestFiles){if(snapshot.remoteOffer.id!=c.other)throw std::runtime_error("文件复制已改变，请重新复制");auto& job=createReceive(c.id,c.other,c.destination,snapshot.remoteOffer);request(job);return;}
        if(c.type==Command::Browse){Writer w(BrowseRequest,c.id);w.str(c.text);wire(std::move(w));return;}
        if(c.type==Command::Download){if(c.names.size()>4096)throw std::runtime_error("一次选择文件过多");Writer w(DownloadRequest,c.id);w.u64(c.other);w.u32(uint32_t(c.names.size()));for(auto& p:c.names)w.str(p);wire(std::move(w));auto job=std::make_unique<Job>();job->info.id=c.id;job->info.offerId=c.other;job->info.destination=c.destination.empty()?receiveDirectory:c.destination;job->info.name="准备下载";job->requestPending=true;prune();jobs[c.id]=std::move(job);update();return;}
    }
    void acceptRequest(uint64_t id,uint64_t offerId,uint64_t epoch,std::map<std::string,uint64_t> received){
        auto current=jobs.find(id);
        if(!epoch||(current!=jobs.end()&&(!current->second->info.outgoing||epoch<=current->second->epoch)))return;
        if(current!=jobs.end()&&current->second->info.state==TransferState::Cancelled){Writer w(TransferError,id);w.u64(epoch);w.str("源端传输已取消，请重新发送");wire(std::move(w));return;}
        auto it=offers.find(offerId);if(it==offers.end()){Writer w(TransferError,id);w.u64(epoch);w.str("源文件清单失效，请重新复制或发送");wire(std::move(w));return;}
        auto job=std::make_unique<Job>();job->epoch=epoch;job->source=it->second;job->info.id=id;job->info.offerId=offerId;job->info.outgoing=true;job->info.total=it->second.info.bytes;job->info.name=it->second.info.entries.front().name;job->info.state=TransferState::Running;job->received=std::move(received);
        for(auto& item:job->source.items){auto found=job->received.find(item.entry.name);if(found!=job->received.end()){if(found->second>item.entry.size){Writer w(TransferError,id);w.u64(epoch);w.str("续传偏移无效");wire(std::move(w));return;}job->info.bytes+=found->second;}}
        if(!jobs.count(id)){prune();}jobs[id]=std::move(job);update();
    }
    void parse(const std::vector<uint8_t>& frame){Reader r{frame.data(),frame.size()};if(r.u32()!=kMagic)return;auto type=r.u32();auto id=r.u64();if(!r.valid)return;
        if(type==Hello){r.str();return;}
        if(!enabled)return;
        if(type==ClipStart){auto kind=r.u32();auto size=r.u64();if(!r.end()||(kind!=1&&kind!=2)||size>kMaxClipboard||receivingClips.size()>2||id<lastClipboardRemote){ack(id,false);return;}lastClipboardRemote=id;receivingClips.clear();ClipIncoming clip;clip.packet.id=id;clip.packet.kind=ClipboardKind(kind);clip.total=size;clip.packet.data.reserve(size);receivingClips[id]=std::move(clip);return;}
        if(type==ClipData){auto offset=r.u64();auto it=receivingClips.find(id);if(it==receivingClips.end())return;auto& clip=it->second;if(!r.valid||offset!=clip.packet.data.size()||r.left>clip.total-clip.packet.data.size()){receivingClips.erase(it);ack(id,false);return;}clip.packet.data.insert(clip.packet.data.end(),r.p,r.p+r.left);return;}
        if(type==ClipEnd){auto it=receivingClips.find(id);if(it==receivingClips.end())return;if(!r.end()||id!=lastClipboardRemote||it->second.packet.data.size()!=it->second.total){ack(id,false);receivingClips.erase(it);return;}{std::lock_guard eventLock(eventMutex);clipboard.clear();clipboardBytes=it->second.packet.data.size();clipboard.push_back(std::move(it->second.packet));}receivingClips.erase(it);snapshot.remoteOffer={};update();return;}
        if(type==ClipAck){auto success=r.u32();if(r.end()){if(success)snapshot.clipboardAcknowledged=id;else snapshot.clipboardRejected=id;update();}return;}
        if(type==OfferStart){auto bytes=r.u64();auto count=r.u32();auto purpose=r.u32();if(!r.end()||count==0||count>kMaxEntries||purpose>1||receivingOffers.size()>=kMaxIncomingOffers||receivingOffers.count(id)){rejectOffer(id,"收到不兼容或超出限制的文件清单");return;}IncomingOffer incoming;incoming.info.id=id;incoming.info.bytes=bytes;incoming.info.clipboard=purpose!=0;incoming.expected=count;incoming.info.entries.reserve(count);receivingOffers[id]=std::move(incoming);return;}
        if(type==OfferEntry){
            auto it=receivingOffers.find(id);if(it==receivingOffers.end())return;
            auto& incoming=it->second;
            // A frame may contain multiple self-delimiting entry records.
            while(r.left&&r.valid){auto name=r.str();auto bytes=r.u64();auto dir=r.u32();
                if(!r.valid||!SafeName(name)||dir>1||(dir&&bytes)||incoming.info.entries.size()>=incoming.expected||name.size()>kMaxManifestNames-incoming.nameBytes){rejectOffer(id,"收到不兼容或超出限制的文件清单");return;}
                incoming.nameBytes+=name.size();incoming.info.entries.push_back({std::move(name),bytes,dir==1});
            }
            if(!r.end()){rejectOffer(id,"收到不完整的文件清单");}return;
        }
        if(type==OfferEnd){
            auto it=receivingOffers.find(id);if(it==receivingOffers.end())return;
            auto& incoming=it->second;
            if(!r.end()||incoming.info.entries.size()!=incoming.expected||!ValidManifest(incoming.info)){rejectOffer(id,"收到不兼容或超出限制的文件清单");return;}
            auto accepted=std::move(incoming.info);receivingOffers.erase(it);
            remoteOffers[id]=std::move(accepted);
            auto retainedNames=[&]{size_t bytes=0;for(const auto& [offerId,offer]:remoteOffers){(void)offerId;for(const auto& entry:offer.entries)bytes+=entry.name.size();}return bytes;};
            while(remoteOffers.size()>16||retainedNames()>16*1024*1024){auto oldest=remoteOffers.begin();if(oldest->first==id)++oldest;if(oldest==remoteOffers.end())break;remoteOffers.erase(oldest);}
            const auto& offer=remoteOffers.at(id);
            if(offer.clipboard&&id>lastClipboardRemote){lastClipboardRemote=id;snapshot.remoteOffer=offer;}
            for(auto& [jobId,job]:jobs){(void)jobId;if(job->requestPending&&job->info.offerId==id){auto destination=job->info.destination;auto tid=job->info.id;
                try{auto& created=createReceive(tid,id,destination,offer);request(created);}catch(const std::exception& e){auto pending=jobs.find(tid);if(pending!=jobs.end())fail(*pending->second,e.what());}
                break;
            }}
            update();return;
        }
        if(type==Request){
            auto offerId=r.u64();auto epoch=r.u64();auto count=r.u32();if(!r.valid||!epoch)return;
            auto current=jobs.find(id);if(current!=jobs.end()&&(!current->second->info.outgoing||epoch<=current->second->epoch))return;
            auto pending=receivingRequests.find(id);if(pending!=receivingRequests.end()&&epoch<=pending->second.epoch)return;
            if(count==UINT32_MAX&&r.end()){if(receivingRequests.size()<16)receivingRequests[id]={offerId,epoch,{}};return;}
            if(count>kMaxEntries)return;
            std::map<std::string,uint64_t> received;size_t nameBytes=0;for(uint32_t i=0;i<count;++i){auto name=r.str();auto n=r.u64();if(!r.valid||!SafeName(name)||name.size()>kMaxManifestNames-nameBytes||!received.emplace(name,n).second)return;nameBytes+=name.size();}
            if(r.end()){acceptRequest(id,offerId,epoch,std::move(received));}return;
        }
        if(type==RequestOffset){auto epoch=r.u64();auto found=receivingRequests.find(id);if(found==receivingRequests.end()||!r.valid||epoch!=found->second.epoch)return;while(r.valid&&r.left){auto name=r.str();auto n=r.u64();if(!r.valid||!SafeName(name)||name.size()>kMaxManifestNames-found->second.nameBytes||found->second.offsets.size()>=kMaxEntries||!found->second.offsets.emplace(name,n).second){receivingRequests.erase(found);return;}found->second.nameBytes+=name.size();}if(!r.end())receivingRequests.erase(found);return;}
        if(type==RequestReady){auto epoch=r.u64();auto found=receivingRequests.find(id);if(found==receivingRequests.end()||epoch!=found->second.epoch||!r.end())return;auto request=std::move(found->second);receivingRequests.erase(found);acceptRequest(id,request.offerId,epoch,std::move(request.offsets));return;}
        if(type==UploadRequest){auto offerId=r.u64();auto dest=r.str();auto offer=remoteOffers.find(offerId);if(!r.end()||offer==remoteOffers.end())return;auto& job=createReceive(id,offerId,dest.empty()?receiveDirectory:Path(dest),offer->second);request(job);return;}
        if(type==DownloadRequest){auto offerId=r.u64();auto count=r.u32();if(count==0||count>4096)return;std::vector<fs::path> paths;for(uint32_t i=0;i<count;++i)paths.push_back(Path(r.str()));if(!r.end())return;try{auto offer=enumerate(offerId,paths);offer.info.clipboard=false;rememberOffer(offer);emitOffer(offer);}catch(const std::exception& e){Writer w(TransferError,id);w.u64(0);w.str(e.what());wire(std::move(w));}return;}
        if(type==BrowseRequest){auto directory=r.str();if(!r.end())return;Directory result;result.requestId=id;result.path=directory.empty()?Utf8(receiveDirectory):directory;std::error_code ec;fs::directory_iterator it(Path(result.path),ec),end;if(ec)result.error="无法读取目录，请检查文件访问权限";for(;!ec&&it!=end&&result.entries.size()<10000;it.increment(ec)){auto st=it->symlink_status(ec);if(ec||fs::is_symlink(st)||(!fs::is_regular_file(st)&&!fs::is_directory(st)))continue;FileEntry item;item.name=Utf8(it->path().filename());item.directory=fs::is_directory(st);if(!item.directory)item.size=it->file_size(ec);if(!ec)result.entries.push_back(std::move(item));}std::sort(result.entries.begin(),result.entries.end(),[](auto& a,auto& b){return a.directory!=b.directory?a.directory>b.directory:a.name<b.name;});if(streams.size()>=16)return;StreamTask task;task.type=BrowseStart;task.id=id;task.directory=std::move(result);streams.push_back(std::move(task));return;}
        if(type==BrowseStart){Directory d;d.requestId=id;d.path=r.str();d.error=r.str();if(r.end()&&receivingDirectories.size()<16)receivingDirectories[id]=std::move(d);return;}
        if(type==BrowseEntry){auto name=r.str();auto size=r.u64();auto dir=r.u32();auto it=receivingDirectories.find(id);if(r.end()&&it!=receivingDirectories.end()&&it->second.entries.size()<10000)it->second.entries.push_back({name,size,dir!=0});return;}
        if(type==BrowseEnd){auto it=receivingDirectories.find(id);if(r.end()&&it!=receivingDirectories.end()){std::lock_guard eventLock(eventMutex);if(directories.size()>16)directories.pop_front();directories.push_back(std::move(it->second));receivingDirectories.erase(it);}return;}
        auto it=jobs.find(id);if(it==jobs.end())return;auto& job=*it->second;
        // Every job frame is bound to the receiver's current request generation.
        // Ordered transports still contain accepted bytes from before a pause;
        // those bytes must never mutate the reopened stream or its hash state.
        if(type>=FileStart&&type<=TransferError){auto epoch=r.u64();if(!r.valid||epoch!=job.epoch)return;}
        if(type==TransferPause){bool resume=r.left==4&&r.u32()==1;if(!r.end())return;if(job.info.state==TransferState::Complete||job.info.state==TransferState::Cancelled||job.info.state==TransferState::Error)return;if(resume&&!job.info.outgoing)request(job);else{job.info.state=TransferState::Paused;job.output.flush();}update();return;}
        if(type==TransferCancel){if(!r.end()||job.info.state==TransferState::Complete)return;job.info.state=TransferState::Cancelled;job.input.close();job.output.close();std::error_code ec;if(!job.info.outgoing&&Contained(job.info.destination,job.staging))fs::remove_all(job.staging,ec);update();return;}
        if(type==TransferError){auto message=r.str();if(r.end()&&job.info.state!=TransferState::Complete&&job.info.state!=TransferState::Cancelled)fail(job,message,false);return;}
        if(type==TransferAck){if(r.end()&&job.info.outgoing&&job.index>=job.source.items.size()&&job.info.state!=TransferState::Cancelled&&job.info.state!=TransferState::Error){job.info.state=TransferState::Complete;job.info.bytes=job.info.total;update();}return;}
        if(job.info.outgoing||job.info.state!=TransferState::Running)return;
        if(type==FileStart){
            auto name=r.str();auto size=r.u64();auto directory=r.u32();auto offset=r.u64();
            if(!r.end()||!SafeName(name)||directory>1)throw std::runtime_error("收到非法文件路径");
            auto item=std::find_if(job.manifest.begin(),job.manifest.end(),[&](const auto& entry){return entry.name==name;});
            if(item==job.manifest.end()||item->size!=size||item->directory!=(directory!=0)||offset>size)throw std::runtime_error("收到不匹配的文件清单");
            auto path=job.staging/Path(name);
            if(!NoLinksWithin(job.info.destination,job.staging)||!NoLinksWithin(job.staging,path))throw std::runtime_error("文件路径越界或含符号链接");
            job.index=size_t(item-job.manifest.begin());std::error_code ec;
            if(directory){fs::create_directories(path,ec);if(ec||!NoLinksWithin(job.staging,path))throw std::runtime_error("接收目录无法安全创建");job.received[name]=0;return;}
            fs::create_directories(path.parent_path(),ec);
            if(ec||!NoLinksWithin(job.staging,path))throw std::runtime_error("接收目录无法安全创建");
            if(job.output.is_open()){job.output.close();}job.hash=std::make_unique<Hash>();
            if(offset){
                if(!fs::is_regular_file(fs::symlink_status(path,ec))||ec||fs::file_size(path,ec)!=offset||ec||job.received[name]!=offset)throw std::runtime_error("临时文件改变，无法续传");
                std::ifstream existing(path,std::ios::binary);std::array<uint8_t,kChunk> block{};
                while(existing){existing.read(reinterpret_cast<char*>(block.data()),block.size());auto n=existing.gcount();if(n>0&&!job.hash->add(block.data(),size_t(n)))throw std::runtime_error("哈希校验初始化失败");}
                if(!existing.eof())throw std::runtime_error("无法读取续传文件");
            }else if(job.received[name])throw std::runtime_error("续传偏移不匹配");
            // Existing ordinary empty files are valid on resume; links and special
            // files must never be opened/truncated even when the offset is zero.
            auto status=fs::symlink_status(path,ec);
            if((ec&&ec!=std::errc::no_such_file_or_directory)||(!ec&&fs::exists(status)&&!fs::is_regular_file(status))||!NoLinksWithin(job.staging,path))throw std::runtime_error("临时文件不是安全的普通文件");
            job.hashes.erase(name);
            // libc++ close() releases the filebuf's buffer. Reopening that same
            // filebuf can fall back to byte-sized writes, so construct a fresh
            // buffered stream for each file and resume generation.
            job.output=std::ofstream(path,std::ios::binary|(offset?std::ios::app:std::ios::trunc));
            if(!job.output){throw std::runtime_error("无法写入文件，请检查磁盘空间");}job.offset=offset;job.fileStarted=true;return;
        }
        if(type==FileData){auto offset=r.u64();if(!r.valid||!job.fileStarted||job.index>=job.manifest.size()||offset!=job.offset||r.left>kChunk||r.left>job.manifest[job.index].size-job.offset)throw std::runtime_error("文件块偏移或长度错误");auto& entry=job.manifest[job.index];job.output.write(reinterpret_cast<const char*>(r.p),r.left);job.output.flush();if(!job.output||!job.hash->add(r.p,r.left))throw std::runtime_error("文件写入失败，请检查磁盘空间");job.offset+=r.left;job.info.bytes+=r.left;job.received[entry.name]=job.offset;updateRate(job);update();return;}
        if(type==FileEnd){auto digest=r.str(64);if(!r.end()||!job.fileStarted||job.index>=job.manifest.size()||job.offset!=job.manifest[job.index].size||digest.empty()||digest!=job.hash->finish())throw std::runtime_error("文件完整性校验失败，请重传");job.output.close();job.hash.reset();job.hashes[job.manifest[job.index].name]=digest;job.fileStarted=false;return;}
        if(type==TransferEnd){if(!r.end())return;for(auto& entry:job.manifest)if(!entry.directory&&(job.received[entry.name]!=entry.size||!job.hashes.count(entry.name)))throw std::runtime_error("文件尚未完整接收");finalize(job);Writer w(TransferAck,id);w.u64(job.epoch);wire(std::move(w));return;}
    }
    void updateRate(Job& job){auto now=Clock::now();double seconds=std::chrono::duration<double>(now-job.sample).count();if(seconds>=0.25){job.info.bytesPerSecond=(job.info.bytes-job.sampleBytes)/seconds;job.sample=now;job.sampleBytes=job.info.bytes;}}
    void finalize(Job& job){
        std::error_code ec;std::set<fs::path> reserved;
        for(auto& [root,unused]:job.roots){
            (void)unused;auto source=job.staging/Path(root);auto original=Path(root);auto target=job.info.destination/original;int suffix=2;
            for(;;){
                if(!Contained(job.info.destination,target))throw std::runtime_error("接收路径越界");
                bool collision=fs::exists(target,ec)||reserved.count(target);
                if(ec)throw std::runtime_error("无法检查同名文件");
                if(!collision&&MoveExclusive(source,target,ec))break;
                if(!collision&&!ExistsError(ec)){
                    throw std::runtime_error(job.info.files.empty()?"文件校验成功，但无法移入接收目录":"部分文件已接收，但无法完成剩余文件，请查看接收目录");
                }
                if(suffix>100000)throw std::runtime_error("同名文件过多");
                target=job.info.destination/Path(Utf8(original.stem())+" ("+std::to_string(suffix++)+")"+Utf8(original.extension()));ec.clear();
            }
            reserved.insert(target);job.info.files.push_back(target);
        }
        fs::remove(job.staging,ec);job.info.state=TransferState::Complete;job.info.bytes=job.info.total;job.info.bytesPerSecond=0;
        {std::lock_guard eventLock(eventMutex);if(completed.size()>64)completed.pop_front();completed.push_back({job.info.id,job.info.offerId,job.info.files});}
        update();
    }
    bool pumpFile(){if(Clock::now()<nextData||outbox.size()>4)return false;for(auto& [id,ptr]:jobs){auto& job=*ptr;if(!job.info.outgoing||job.info.state!=TransferState::Running)continue;try{
        if(job.index>=job.source.items.size()){Writer end(TransferEnd,id);end.u64(job.epoch);wire(std::move(end));job.info.state=TransferState::Queued;update();return true;}
        auto& item=job.source.items[job.index];if(!job.fileStarted){checkSource(item);job.offset=job.received[item.entry.name];job.hash=std::make_unique<Hash>();Writer start(FileStart,id);start.u64(job.epoch);start.str(item.entry.name);start.u64(item.entry.size);start.u32(item.entry.directory?1:0);start.u64(job.offset);wire(std::move(start));if(item.entry.directory){++job.index;return true;}job.input=std::ifstream(item.source,std::ios::binary);if(!job.input)throw std::runtime_error("无权读取源文件");if(job.offset){std::array<uint8_t,kChunk> bytes{};uint64_t remaining=job.offset;while(remaining){size_t n=size_t(std::min<uint64_t>(remaining,bytes.size()));job.input.read(reinterpret_cast<char*>(bytes.data()),n);if(size_t(job.input.gcount())!=n||!job.hash->add(bytes.data(),n))throw std::runtime_error("无法验证续传文件");remaining-=n;}}job.fileStarted=true;return true;}
        if(job.offset<item.entry.size){std::array<uint8_t,kChunk> bytes{};size_t n=size_t(std::min<uint64_t>(bytes.size(),item.entry.size-job.offset));job.input.read(reinterpret_cast<char*>(bytes.data()),n);if(size_t(job.input.gcount())!=n||!job.hash->add(bytes.data(),n))throw std::runtime_error("源文件读取失败或已改变");Writer data(FileData,id);data.u64(job.epoch);data.u64(job.offset);data.raw(bytes.data(),n);wire(std::move(data));job.offset+=n;job.info.bytes+=n;updateRate(job);auto now=Clock::now();if(now-lastBudget>std::chrono::milliseconds(500)){double ms=rttHint.load();if(ms>0){if(baseline<0||ms<baseline)baseline=ms;if(ms>std::max(baseline*2.0,baseline+8.0))budget=std::max<uint64_t>(256*1024,budget*3/4);else budget=std::min<uint64_t>(32*1024*1024,budget+256*1024);}lastBudget=now;}auto configured=speedHint.load();uint64_t speed=configured?configured:budget;nextData=now+std::chrono::microseconds(n*1000000/std::max<uint64_t>(speed,65536));update();return true;}
        checkSource(item);auto digest=job.hash->finish();if(digest.empty())throw std::runtime_error("无法计算文件哈希");Writer end(FileEnd,id);end.u64(job.epoch);end.str(digest);wire(std::move(end));job.input.close();job.hash.reset();job.fileStarted=false;++job.index;return true;
    }catch(const std::exception& e){fail(job,e.what());return true;}}return false;}
    void applySharingSetting(){
        std::optional<bool> setting;bool disable=false;
        {std::lock_guard queueLock(queueMutex);if(enabledChangePending){setting=enabledSetting.load();disable=disablePending;enabledChangePending=false;disablePending=false;}}
        if(!setting)return;
        if(disable){Command command{Command::Enabled};command.success=false;handle(std::move(command));}
        if(*setting){Command command{Command::Enabled};command.success=true;handle(std::move(command));}
    }
    void run(){std::unique_lock lock(mutex);while(!stopping){cv.wait_for(lock,std::chrono::milliseconds(2));if(stopping)break;
        // Apply the acknowledged local sharing setting before consuming peer
        // resume frames. Otherwise an inbox-first iteration can discard the
        // complete request while a queued enable has not reached the worker.
        try{applySharingSetting();}catch(const std::exception& e){snapshot.message=e.what();update();}
#ifdef REMOTE_EXCHANGE_TEST_HOOKS
        ::RemoteExchangeTestAfterSharingCheck(host,enabled);
#endif
        if(!connected){std::optional<Command> config;{std::lock_guard queueLock(queueMutex);for(auto it=commands.begin();it!=commands.end();++it)if(it->type==Command::Enabled||it->type==Command::DirectoryChange){config.emplace(std::move(*it));commands.erase(it);break;}}if(config)handle(std::move(*config));continue;}
        try{bool sharingChanged=false;for(int n=0;n<8;++n){std::vector<uint8_t> frame;{std::lock_guard queueLock(queueMutex);
                // SetEnabled and Receive use this same lock. A setting queued
                // after applySharingSetting must win over dequeuing its peer
                // request as well, not just at the start of the iteration.
                if(enabledChangePending){sharingChanged=true;break;}if(inbox.empty()){break;}frame=std::move(inbox.front());inbox.pop_front();inboxBytes-=frame.size();}uint64_t affected=0;if(frame.size()>=16){Reader r{frame.data()+8,frame.size()-8};affected=r.u64();}try{parse(frame);}catch(const std::exception& e){auto it=jobs.find(affected);if(it!=jobs.end())fail(*it->second,e.what());else{snapshot.message=e.what();update();}}}
            if(sharingChanged)continue;
            Command command{Command::Publish};bool hasCommand=false;{std::lock_guard queueLock(queueMutex);if(!commands.empty()&&outbox.size()<4){command=std::move(commands.front());commands.pop_front();commandBytes-=command.data.size();hasCommand=true;}}if(hasCommand){auto commandId=command.id;try{handle(std::move(command));}catch(const std::exception& e){auto it=jobs.find(commandId);if(it!=jobs.end())fail(*it->second,e.what());else{snapshot.message=e.what();ack(commandId,false);update();}}}
            if(outbox.empty()&&!pumpStream())pumpFile();
            if(!outbox.empty()){auto frame=outbox.front();auto epoch=generation;lock.unlock();int result=-1;try{result=send(frame.data(),frame.size());}catch(...){result=-1;}lock.lock();if(result>0&&connected&&generation==epoch&&!outbox.empty()){outboxBytes-=outbox.front().size();outbox.pop_front();}else if(result<0){snapshot.message="文件通道暂不可用";update();}}
        }catch(const std::exception& e){snapshot.message=e.what();}
    }}
};

Session::Session(bool host,fs::path directory,Send send):impl_(std::make_unique<Impl>(host,std::move(directory),std::move(send))){}
Session::~Session()=default;
void Session::SetConnected(bool connected){
    if(!connected)impl_->active.store(false);
    std::lock_guard lock(impl_->mutex);if(impl_->connected==connected)return;
    impl_->connected=connected;impl_->active.store(connected);++impl_->generation;impl_->enabled=impl_->enabledSetting.load();
    impl_->snapshot.clipboardAcknowledged=impl_->snapshot.clipboardRejected=0;impl_->lastClipboardRemote=0;
    impl_->receivingClips.clear();impl_->receivingOffers.clear();impl_->remoteOffers.clear();impl_->receivingRequests.clear();impl_->receivingDirectories.clear();
    {std::lock_guard eventLock(impl_->eventMutex);impl_->clipboard.clear();impl_->clipboardBytes=0;impl_->directories.clear();impl_->completed.clear();}
    {std::lock_guard queueLock(impl_->queueMutex);for(auto& command:impl_->commands)if(command.type==Command::DirectoryChange)impl_->receiveDirectory=command.destination;impl_->inbox.clear();impl_->inboxBytes=0;impl_->commands.clear();impl_->commandBytes=0;}
    impl_->outbox.clear();impl_->outboxBytes=0;impl_->streams.clear();impl_->snapshot.remoteOffer={};
    for(auto& [id,job]:impl_->jobs){(void)id;if(job->info.state==TransferState::Running||job->info.state==TransferState::Queued){job->info.state=TransferState::Paused;job->input.close();job->output.close();job->hash.reset();job->fileStarted=false;job->info.error="连接已断开，可重新连接后继续";}}
    if(connected){Writer hello(Hello);hello.str(Utf8(impl_->receiveDirectory));impl_->wire(std::move(hello));impl_->snapshot.message.clear();}
    impl_->update();impl_->cv.notify_all();
}
void Session::SetEnabled(bool enabled){
    {std::lock_guard queueLock(impl_->queueMutex);impl_->enabledSetting.store(enabled);impl_->enabledChangePending=true;impl_->disablePending|=!enabled;
        if(!enabled){impl_->inbox.clear();impl_->inboxBytes=0;}}
    {std::lock_guard stateLock(impl_->snapshotMutex);impl_->published.enabled=enabled;}
    impl_->cv.notify_all();
}
void Session::SetReceiveDirectory(fs::path directory){{std::lock_guard lock(impl_->snapshotMutex);impl_->publishedDirectory=directory;}Command c{Command::DirectoryChange};c.destination=std::move(directory);impl_->enqueue(std::move(c));}
fs::path Session::ReceiveDirectory()const{std::lock_guard lock(impl_->snapshotMutex);return impl_->publishedDirectory;}
void Session::SetNetworkRtt(double ms){impl_->rttHint.store(ms);}
void Session::SetSpeedLimit(uint64_t bytes){impl_->speedHint.store(bytes);}
bool Session::Receive(const uint8_t* data,size_t size){if(!data||size<16||size>kMaxFrame||!impl_->active.load())return false;std::lock_guard lock(impl_->queueMutex);
    // Consume and drop while disabled; replaying these frames after enable
    // would turn an explicitly disabled clipboard/offer into an active one.
    if(!impl_->enabledSetting.load())return true;
    if(impl_->inboxBytes+size>kMaxInbox||impl_->inbox.size()>=128)return false;
    impl_->inbox.emplace_back(data,data+size);impl_->inboxBytes+=size;impl_->cv.notify_all();return true;}
uint64_t Session::PublishClipboard(ClipboardKind kind,std::vector<uint8_t> data){if(data.size()>kMaxClipboard)return 0;Command c{Command::Publish};c.id=impl_->id();c.kind=kind;c.data=std::move(data);auto id=c.id;impl_->enqueue(std::move(c));return id;}
bool Session::TakeClipboard(ClipboardPacket& packet){std::lock_guard lock(impl_->eventMutex);if(impl_->clipboard.empty())return false;packet=std::move(impl_->clipboard.front());impl_->clipboard.pop_front();impl_->clipboardBytes=0;return true;}
void Session::ClipboardInstalled(uint64_t id,bool success){Command c{Command::Installed};c.id=id;c.success=success;impl_->enqueue(std::move(c));}
uint64_t Session::OfferFiles(std::vector<fs::path> paths){Command c{Command::OfferFiles};c.id=impl_->id();c.paths=std::move(paths);auto id=c.id;impl_->enqueue(std::move(c));return id;}
uint64_t Session::RequestFiles(uint64_t offerId,fs::path destination){Command c{Command::RequestFiles};c.id=impl_->id();c.other=offerId;c.destination=std::move(destination);auto id=c.id;impl_->enqueue(std::move(c));return id;}
uint64_t Session::SendFiles(std::vector<fs::path> paths,std::string destination){Command c{Command::SendFiles};c.id=impl_->id();c.other=impl_->id();c.paths=std::move(paths);c.text=std::move(destination);auto id=c.other;impl_->enqueue(std::move(c));return id;}
uint64_t Session::DownloadFiles(std::vector<std::string> paths,fs::path destination){Command c{Command::Download};c.id=impl_->id();c.other=impl_->id();c.names=std::move(paths);c.destination=std::move(destination);auto id=c.id;impl_->enqueue(std::move(c));return id;}
uint64_t Session::Browse(std::string directory){Command c{Command::Browse};c.id=impl_->id();c.text=std::move(directory);auto id=c.id;impl_->enqueue(std::move(c));return id;}
bool Session::TakeDirectory(Directory& directory){std::lock_guard lock(impl_->eventMutex);if(impl_->directories.empty())return false;directory=std::move(impl_->directories.front());impl_->directories.pop_front();return true;}
bool Session::TakeCompletedFiles(CompletedFiles& files){std::lock_guard lock(impl_->eventMutex);if(impl_->completed.empty())return false;files=std::move(impl_->completed.front());impl_->completed.pop_front();return true;}
void Session::Pause(uint64_t id){Command c{Command::Pause};c.id=id;impl_->enqueue(std::move(c));}
void Session::Resume(uint64_t id){Command c{Command::Resume};c.id=id;impl_->enqueue(std::move(c));}
void Session::Cancel(uint64_t id){Command c{Command::Cancel};c.id=id;impl_->enqueue(std::move(c));}
Snapshot Session::GetSnapshot()const{std::lock_guard lock(impl_->snapshotMutex);return impl_->published;}
int Session::ReadTransferFile(uint64_t id,const std::string& name,uint64_t offset,uint8_t* data,size_t capacity,uint32_t timeoutMs){
    if(!SafeName(name)||!data||capacity==0)return -1;
    auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);
    std::unique_lock lock(impl_->mutex);
    for(;;){
        auto it=impl_->jobs.find(id);
        if(it!=impl_->jobs.end()){
            auto& job=*it->second;
            if(job.info.state==TransferState::Cancelled||job.info.state==TransferState::Error)return -1;
            if(!job.manifest.empty()){
                auto item=std::find_if(job.manifest.begin(),job.manifest.end(),[&](auto& entry){return entry.name==name&&!entry.directory;});
                if(item==job.manifest.end())return -1;
                if(offset>=item->size)return 0;
                uint64_t available=job.received[name];
                if(available>offset){
                    auto path=job.staging/Path(name);
                    if(job.info.state==TransferState::Complete){
                        auto root=Utf8(*Path(name).begin());auto relative=Path(name).lexically_relative(Path(root));
                        auto position=std::distance(job.roots.begin(),job.roots.find(root));
                        if(position<0||size_t(position)>=job.info.files.size())return -1;
                        auto target=job.info.files[size_t(position)];path=relative.empty()||relative=="."?target:target/relative;
                    }
                    size_t count=size_t(std::min<uint64_t>({capacity,available-offset,kChunk}));
                    std::ifstream input(path,std::ios::binary);if(!input)return -1;
                    input.seekg(std::streamoff(offset));input.read(reinterpret_cast<char*>(data),count);
                    return input.gcount()>0?int(input.gcount()):-1;
                }
            }
        }
        if(impl_->stopping||!impl_->connected)return -1;
        if(impl_->cv.wait_until(lock,deadline)==std::cv_status::timeout)return -2;
    }
}
const char* StateLabel(TransferState state){switch(state){case TransferState::Queued:return "准备中";case TransferState::Running:return "传输中";case TransferState::Paused:return "已暂停";case TransferState::Complete:return "已完成";case TransferState::Cancelled:return "已取消";case TransferState::Error:return "失败";}return "未知";}
}
