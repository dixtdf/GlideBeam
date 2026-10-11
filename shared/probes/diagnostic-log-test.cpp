#include "../diagnostic-log.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace remote_log;
static std::string Read(const fs::path& path) { std::ifstream file(path,std::ios::binary); return {std::istreambuf_iterator<char>(file),{}}; }
static bool Check(bool result,const char* name) { std::cout<<(result?"PASS ":"FAIL ")<<name<<'\n'; return result; }
static bool WriteAccepted(Level level, std::string_view component, std::string_view message) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do {
        if(TryWriteForTest(level,component,message)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now()<deadline);
    return Check(false,"required test record admitted before deadline");
}
int main(int argc,char** argv) {
    if(argc==4 && std::string(argv[1])=="--writer") {
        Initialize(Config{fs::absolute(argv[2]),4096,3,512});
        for(int record=0;record<128;++record){Write(Level::Info,"multiprocess","source="+std::string(argv[3])+" record="+std::to_string(record));std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        Write(Level::Info,"multiprocess","END source="+std::string(argv[3]));Shutdown();return 0;
    }
    if(argc!=2) return 2;
    fs::path root=fs::absolute(fs::path(argv[1]));
    if(fs::exists(root)) { std::cerr<<"Use a fresh isolated test directory\n"; return 2; }
    Config config{root,4096,3,8};
    bool passed=Initialize(config);
    passed&=WriteAccepted(Level::Info,"probe","BEGIN appending test");
    passed&=WriteAccepted(Level::Error,"probe","password=synthetic secret phrase; token=fixture-only-token; path=\"C:\\synthetic\\Private File.txt\"; os_path=/Users/synthetic/Private File.txt; clipboard=fixture-only-text");
    passed&=WriteAccepted(Level::Warning,"probe","quoted token=\"fixture \\\" escaped secret\"; status=metadata");
    passed&=WriteAccepted(Level::Info,"probe","line one\nline two\r\tcontrols");
    Shutdown();
    std::string first=Read(root/"remote-native.log");
    passed&=Check(first.find("BEGIN appending")!=std::string::npos,"shutdown drains queued records");
    passed&=Check(first.find("synthetic secret")==std::string::npos && first.find("fixture-only")==std::string::npos && first.find("Private File")==std::string::npos && first.find("escaped secret")==std::string::npos && first.find("[redacted]")!=std::string::npos && first.find("[path]")!=std::string::npos,"secret assignments and paths are redacted");
    passed&=Check(first.find("line one line two  controls")!=std::string::npos,"record line breaks cannot forge new lines");
    passed&=Check(first.find("Z INFO pid=")!=std::string::npos && first.find(" tid=")!=std::string::npos,"UTC timestamps process and thread metadata");
    passed&=Initialize(config); passed&=WriteAccepted(Level::Info,"probe","SECOND process lifecycle"); Shutdown();
    passed&=Check(Read(root/"remote-native.log").find(first)==0 && Read(root/"remote-native.log").find("SECOND process")!=std::string::npos,"reinitialization appends existing log");
    passed&=Initialize(config);
    for(int n=0;n<80;++n) { Write(Level::Debug,"rotation","record="+std::to_string(n)+" "+std::string(400,'x')); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    std::vector<std::thread> workers;
    auto start=std::chrono::steady_clock::now();
    for(int thread=0;thread<8;++thread)workers.emplace_back([thread]{for(int n=0;n<2000;++n)Write(Level::Info,"parallel","thread="+std::to_string(thread)+" record="+std::to_string(n));});
    for(auto& worker:workers)worker.join();
    double producerSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    // Write is best-effort under overload. Confirm admission, then immediately
    // shut down; observing the file first would stop testing shutdown's drain.
    passed&=Check(WriteAccepted(Level::Info,"probe","TAIL shutdown marker"),"tail record admitted before shutdown");
    Shutdown();
    int count=0; bool bounded=true; std::string all;
    for(auto& item:fs::directory_iterator(root)) {
        if(item.path().extension()==".log") { ++count; bounded&=item.file_size()<=4096; all+=Read(item.path()); }
    }
    passed&=Check(count==3 && bounded,"rotation retains three bounded files");
    passed&=Check(all.find("dropped_records=")!=std::string::npos,"queue overflow reports dropped metadata");
    passed&=Check(all.find("TAIL shutdown marker")!=std::string::npos,"shutdown drains after concurrent logging");
    passed&=Check(producerSeconds<2.0,"multithread producers stay independent of disk writes");
    passed&=Check(std::string(ErrorClass("permission denied /Users/test/private"))=="permission" && std::string(ErrorClass("hash mismatch"))=="integrity" && std::string(ErrorClass("timeout"))=="timeout","error classification omits raw details");
    std::cout<<"producer_seconds="<<producerSeconds<<'\n';
    return passed?0:1;
}
