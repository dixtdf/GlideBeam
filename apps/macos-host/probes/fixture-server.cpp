#include "../../../shared/protocol.h"
#include <chrono>
#include <thread>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <memory>
int main(int argc,char** argv){
    if(argc!=5)return 2;
    std::ifstream input(argv[1],std::ios::binary|std::ios::ate);
    if(!input||input.tellg()>64*1024*1024)return 2;
    input.seekg(0);std::vector<std::vector<uint8_t>> frames;
    for(;;){uint32_t length=0;input.read((char*)&length,4);if(input.eof()&&!input.gcount())break;
        if(!input||length<sizeof(RemoteFrameHeader)+1||length>8*1024*1024)return 2;
        std::vector<uint8_t> bytes(length);input.read((char*)bytes.data(),length);if(!input)return 2;
        RemoteFrameHeader h{};memcpy(&h,bytes.data(),sizeof(h));if(h.magic!=0x31524652)return 2;
        frames.push_back(std::move(bytes));
    }
    if(frames.empty())return 2;
    std::ifstream secret(argv[4]);std::string token;std::getline(secret,token);if(token.size()<32)return 2;
    auto raw=remote_server_start(argv[2],token.c_str(),argv[3]);if(!raw)return 3;
    std::unique_ptr<remote_endpoint,decltype(&remote_close)> endpoint(raw,remote_close);
    uint64_t sequence=0,sent=0;size_t index=0;bool connected=false;
    auto start=std::chrono::steady_clock::now(),next=start;
    std::vector<uint8_t> poll(65536);
    while(std::chrono::steady_clock::now()-start<std::chrono::seconds(45)){
        int state=remote_status(raw);if(state<0){char error[512]{};remote_last_error(raw,error,sizeof(error));fprintf(stderr,"%s\n",error);return 3;}
        if(state==1){
            if(!connected){connected=true;index=0;next=std::chrono::steady_clock::now();const char* message="{\"message\":\"Diagnostic fixture playback; not desktop capture\"}";remote_send(raw,REMOTE_CONTROL,(const uint8_t*)message,strlen(message));}
            for(int i=0;i<32;++i){uint32_t channel=0;int n=remote_poll(raw,&channel,poll.data(),poll.size());if(n<=0)break;
                RemoteInputEvent event{};if(channel==REMOTE_INPUT&&n>=(int)sizeof(event)){memcpy(&event,poll.data(),sizeof(event));if(event.type==INPUT_KEYFRAME)index=0;}
            }
            auto now=std::chrono::steady_clock::now();
            if(now>=next){auto bytes=frames[index];RemoteFrameHeader header{};memcpy(&header,bytes.data(),sizeof(header));header.sequence=++sequence;header.capture_time_us=0;memcpy(bytes.data(),&header,sizeof(header));
                if(remote_send(raw,REMOTE_VIDEO,bytes.data(),bytes.size())<0)return 3;
                ++sent;index=(index+1)%frames.size();next=now+std::chrono::microseconds(16667);
            }
        }else connected=false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    printf("{\"scenario\":\"preencoded_fixture_LAN_60fps_target\",\"sent_frames\":%llu,\"captures_desktop\":false,\"injects_input\":false}\n",(unsigned long long)sent);
    return 0;
}
