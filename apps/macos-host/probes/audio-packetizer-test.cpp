#include "../audio-capture.hpp"
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>

using namespace remote_audio_capture;
static uint64_t Get64(const uint8_t* p) { uint64_t value=0; for(size_t i=0;i<8;++i)value|=uint64_t(p[i])<<(i*8);return value; }
static int16_t Get16(const uint8_t* p) { return static_cast<int16_t>(uint16_t(p[0])|(uint16_t(p[1])<<8)); }
int main() {
    assert(FloatToS16(-2.0f)==-32768 && FloatToS16(2.0f)==32767);
    assert(FloatToS16(std::numeric_limits<float>::infinity())==0);
    assert(FloatToS16(std::numeric_limits<float>::quiet_NaN())==0);
    assert(FloatToS16(0.999999f)==32767 && FloatToS16(-0.5f)==-16384);
    Packetizer p;
    std::vector<std::vector<uint8_t>> packets;
    auto send=[&](const uint8_t* data,size_t size){packets.emplace_back(data,data+size);return 1;};
    std::vector<float> interleaved(480*2);
    for(size_t i=0;i<480;++i){interleaved[i*2]=0.5f;interleaved[i*2+1]=-0.25f;}
    assert(p.PushFloat32(interleaved.data(),120*8,nullptr,0,120,false,1000000,send));
    assert(packets.empty()&&p.PendingFrames()==120);
    assert(p.PushFloat32(interleaved.data()+240,360*8,nullptr,0,360,false,1002500,send));
    assert(packets.size()==2&&p.PendingFrames()==0);
    assert(packets[0].size()==992&&packets[0][12]==1&&packets[1][12]==0);
    assert(Get64(packets[0].data()+24)==1000000&&Get64(packets[1].data()+24)==1005000);
    assert(Get16(packets[0].data()+32)==16384&&Get16(packets[0].data()+34)==-8192);
    assert(Get64(packets[0].data()+16)==0&&Get64(packets[1].data()+16)==1);
    std::vector<float> left(240,-1.0f),right(240,1.0f);
    assert(p.PushFloat32(left.data(),960,right.data(),960,240,true,1010000,send));
    assert(packets.back()[12]==0&&Get16(packets.back().data()+32)==-32768&&Get16(packets.back().data()+34)==32767);
    assert(p.PushFloat32(left.data(),480,right.data(),480,120,true,1015000,send));
    assert(p.PendingFrames()==120);
    assert(p.PushFloat32(left.data(),960,right.data(),960,240,true,2000000,send));
    assert(p.PendingFrames()==0&&packets.back()[12]==1&&Get64(packets.back().data()+24)==2000000);
    assert(!p.PushFloat32(left.data(),959,right.data(),960,240,true,2005000,send));
    assert(p.PendingFrames()==0);
    size_t dropped=0;
    auto drop=[&](const uint8_t*,size_t){++dropped;return 0;};
    assert(p.PushFloat32(left.data(),960,right.data(),960,240,true,2005000,drop));
    assert(dropped==1);
    assert(p.PushFloat32(left.data(),960,right.data(),960,240,true,2010000,send));
    assert(packets.back()[12]==1&&Get64(packets.back().data()+16)==5);
    assert(p.PushFloat32(left.data(),960,right.data(),960,240,true,2015000,send));
    assert(packets.back()[12]==0);
    p.Reset();
    assert(p.PushFloat32(left.data(),960,right.data(),960,240,true,0,send));
    assert(packets.back()[12]==1&&Get64(packets.back().data()+16)==7);
    // Invalid/unbounded sample counts do not touch the source buffers.
    assert(!p.PushFloat32(left.data(),960,right.data(),960,4801,true,0,send));
    assert(!p.PushFloat32(left.data(),960,right.data(),960,240,true,std::numeric_limits<uint64_t>::max(),send));
    std::puts("PASS: PCM saturation/stereo layout, split callbacks, timestamps, gaps, reconnect and backpressure");
}
