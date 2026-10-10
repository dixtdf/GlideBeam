#include "../nv12-layout.hpp"
#include <chrono>
#include <cstdio>
#include <vector>

static bool Check(bool uv,size_t width,size_t sourceRows,size_t codedRows) {
    const size_t srcPitch=width+128,dstPitch=width+256;
    std::vector<uint8_t> source(srcPitch*sourceRows),dest(dstPitch*codedRows,0xcd);
    uint32_t random=0x13de7859;
    for(auto& byte:source){random^=random<<13;random^=random>>17;random^=random<<5;byte=(uint8_t)random;}
    if(!remote_nv12::PrepackPlane(source.data(),srcPitch,sourceRows,dest.data(),dstPitch,codedRows,width,uv))return false;
    for(size_t y=0;y<codedRows;++y)for(size_t x=0;x<width;++x) {
        size_t fx=uv?((x&~(size_t)63)|(x&0x2f)|((y&4)<<2)):
            ((x&~(size_t)63)|(x&0x27)|((x&0x10)>>1)|((y&4)<<2));
        size_t fy=uv?((y&~(size_t)7)|(y&3)|((x&16)>>2)):
            ((y&~(size_t)7)|((y&3)<<1)|((x&8)>>3));
        size_t sy=y<sourceRows?y:sourceRows-1;
        if(dest[fy*dstPitch+fx]!=source[sy*srcPitch+x]) {
            std::printf("mapping failed: uv=%d x=%zu y=%zu fx=%zu fy=%zu\n",uv,x,y,fx,fy);return false;
        }
    }
    // Destination row padding is never read or overwritten by the transformation.
    for(size_t y=0;y<codedRows;++y)for(size_t x=width;x<dstPitch;++x)if(dest[y*dstPitch+x]!=0xcd)return false;
    return true;
}
int main() {
    bool valid=Check(false,1920,1080,1088)&&Check(true,1920,540,544)&&
        Check(false,64,8,8)&&Check(true,64,4,8);
    const size_t width=1920;std::vector<uint8_t> sy(width*1080),suv(width*540),dy(width*1088),duv(width*544);
    const int iterations=120;auto begin=std::chrono::steady_clock::now();
    for(int n=0;n<iterations;++n) {
        remote_nv12::PrepackPlane(sy.data(),width,1080,dy.data(),width,1088,width,false);
        remote_nv12::PrepackPlane(suv.data(),width,540,duv.data(),width,544,width,true);
        // External observer barrier: keep every iteration's memory writes measurable.
        asm volatile("" : : "r"(dy.data()), "r"(duv.data()) : "memory");
    }
    double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/iterations;
    std::printf("mapping_pass=%d iterations=%d nv12_1920x1080_to_1088_ms=%.3f\n",valid,iterations,ms);
    return valid?0:1;
}
