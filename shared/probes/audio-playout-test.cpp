#include "../audio-playout.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

using remote_audio::AudioJitterBuffer;
static void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static std::vector<uint8_t> Packet(uint64_t sequence, uint32_t flags = 0, bool constant = false) {
    RemoteAudioHeader header{};
    header.magic=0x31414652; header.sample_rate=48000; header.channels=2; header.frames=240;
    header.flags=flags; header.sequence=sequence; header.capture_time_us=sequence*5000;
    std::vector<uint8_t> bytes(remote_audio::PacketBytes); std::memcpy(bytes.data(), &header, sizeof(header));
    for(size_t frame=0;frame<240;++frame) {
        const double position=static_cast<double>(sequence*240+frame);
        for(size_t channel=0;channel<2;++channel) {
            double signal=constant?0.75:0.6*std::sin(position*6.283185307179586*(channel?500:1000)/48000);
            int16_t sample=static_cast<int16_t>(std::lround(signal*32767));
            std::memcpy(bytes.data()+sizeof(header)+(frame*2+channel)*sizeof(sample),&sample,sizeof(sample));
        }
    }
    return bytes;
}
static bool Push(AudioJitterBuffer& buffer, uint64_t sequence, double arrival, uint32_t flags=0,bool constant=false) {
    auto packet=Packet(sequence,flags,constant); return buffer.Push(packet.data(),packet.size(),arrival);
}
static void Finite(const std::vector<float>& samples) {
    for(float value:samples)Check(std::isfinite(value)&&std::abs(value)<=1,"invalid/clipped output");
}
static void ClockTest(double clockRatio,unsigned batchPackets,uint32_t outputRate,int seconds=60) {
    AudioJitterBuffer buffer;
    uint64_t sequence=0;double maximumQueue=0;
    std::vector<float> output(static_cast<size_t>((outputRate+199)/200)*2);
    uint64_t requestedFrames=0;
    for(int tick=0;tick<seconds*200;++tick) {
        const double now=tick*.005;
        while(sequence*.005/clockRatio<=now) {
            double arrival=sequence*.005/clockRatio;
            for(unsigned packet=0;packet<batchPackets;++packet,++sequence)Push(buffer,sequence,arrival);
        }
        // Integer frame boundaries preserve the exact device clock: at 44.1 kHz
        // successive 5 ms callbacks contain 220/221 frames, rather than 220 each.
        const size_t frames=(static_cast<uint64_t>(tick+1)*outputRate/200)-
            (static_cast<uint64_t>(tick)*outputRate/200);
        output.resize(frames*2);requestedFrames+=frames;
        buffer.Render(output.data(),frames,outputRate);Finite(output);
        maximumQueue=std::max(maximumQueue,buffer.Snapshot().queuedMs);
    }
    auto result=buffer.Snapshot();
    std::cout<<"clock ratio="<<clockRatio<<" batch="<<batchPackets<<" rate="<<outputRate
        <<" max_queue="<<maximumQueue<<" target="<<result.targetMs<<" lost="<<result.lostPackets
        <<" underruns="<<result.underrunEvents<<" ppm="<<result.driftPpm<<'\n';
    Check(maximumQueue<=45.001,"clock drift accumulated latency");
    Check(result.lostPackets==0&&result.underrunEvents==0,"clock drift caused avoidable underrun");
    Check(std::abs(result.driftPpm)<=3000.001,"clock correction unbounded");
    Check(result.targetMs<=20,"batch delivery misclassified as high network jitter");
    Check(requestedFrames==static_cast<uint64_t>(seconds)*outputRate,"device clock lost fractional frames");
}

// ScreenCaptureKit emits 960-frame blocks while a 1024-frame audio device
// quantum advances every 21 1/3 ms. Every fifteenth callback therefore delivers
// two blocks together. This is generated from the two clocks, never a recording
// of a user's stream; equal 20 ms callback spacing would miss this regression.
static void CaptureQuantumTest(bool losePacket) {
    AudioJitterBuffer buffer;
    uint64_t callback=1,render=0,sequence=0;
    std::vector<float> output(128*2);
    double maximumQueue=0;
    constexpr uint64_t LostSequence=1200;
    for(;;) {
        const double captureTime=callback*1024.0/48000+.0003*std::sin(callback*.21);
        const double renderTime=.012+render*128.0/48000+.00035*std::sin(render*.7);
        const double now=std::min(captureTime,renderTime);
        if(now>=30)break;
        if(captureTime<=renderTime) {
            const uint64_t completeBlocks=callback*1024/960;
            const uint64_t endSequence=completeBlocks*4;
            while(sequence<endSequence) {
                if(!losePacket||sequence!=LostSequence)
                    Check(Push(buffer,sequence,captureTime+(sequence%8)*.000004),"quantum packet rejected");
                ++sequence;
                maximumQueue=std::max(maximumQueue,buffer.Snapshot().queuedMs);
            }
            ++callback;
        } else {
            buffer.Render(output.data(),128,48000);Finite(output);++render;
        }
    }
    const auto result=buffer.Snapshot();
    std::cout<<"capture quantum=1024/block=960 loss="<<losePacket<<" max_queue="<<maximumQueue
        <<" target="<<result.targetMs<<" lost="<<result.lostPackets<<" underruns="<<result.underrunEvents
        <<" late="<<result.latePackets<<" discarded="<<result.discardedFrames<<'\n';
    Check(maximumQueue<=60.001,"capture quantum burst exceeded bounded headroom");
    Check(result.targetMs<=30,"capture quantum inflated permanent target");
    Check(result.lostPackets==(losePacket?1u:0u)&&result.underrunEvents==(losePacket?1u:0u),
        "capture quantum caused avoidable/repeated gaps");
    Check(result.latePackets==0&&result.discardedFrames==0,"ordinary capture burst discarded good audio");
    if(!losePacket)Check(result.targetMs<=20,"capture batching classified as sustained high jitter");
}

static std::vector<uint8_t> TonePacket(uint64_t sequence,double frequency) {
    auto packet=Packet(sequence);
    for(size_t frame=0;frame<240;++frame) {
        const double phase=6.283185307179586*frequency*static_cast<double>(sequence*240+frame)/48000;
        const int16_t sample=static_cast<int16_t>(std::lround(.6*32767*std::sin(phase)));
        for(size_t channel=0;channel<2;++channel)
            std::memcpy(packet.data()+sizeof(RemoteAudioHeader)+(frame*2+channel)*sizeof(sample),&sample,sizeof(sample));
    }
    return packet;
}

// Independent spectrum measurement, not an oracle built from the resampler's
// coefficients. A Hann-windowed Goertzel band tolerates phase, FIR delay and
// small continuous clock corrections instead of requiring sample alignment.
static double BandPower(const std::vector<float>& signal,uint32_t rate,double center,double radius) {
    const size_t frames=std::min(signal.size(),static_cast<size_t>(rate/4));
    Check(frames>1,"too few samples for spectrum");
    std::vector<double> windowed(frames);
    for(size_t i=0;i<frames;++i)
        windowed[i]=signal[signal.size()-frames+i]*(.5-.5*std::cos(6.283185307179586*i/(frames-1)));
    const size_t first=static_cast<size_t>(std::ceil((center-radius)*frames/rate));
    const size_t last=static_cast<size_t>(std::floor((center+radius)*frames/rate));
    Check(first<=last&&last<frames/2,"spectrum band out of range");
    double power=0;
    for(size_t bin=first;bin<=last;++bin) {
        const double coefficient=2*std::cos(6.283185307179586*bin/frames);
        double previous=0,previous2=0;
        for(double value:windowed) {
            const double current=value+coefficient*previous-previous2;
            previous2=previous;previous=current;
        }
        power+=std::max(0.0,previous*previous+previous2*previous2-coefficient*previous*previous2);
    }
    return power;
}

static void ResamplerQualityTest(uint32_t outputRate,double frequency,bool stopBand=false) {
    AudioJitterBuffer buffer;
    uint64_t sequence=0;
    std::vector<float> output(static_cast<size_t>((outputRate+199)/200)*2),steady;
    for(unsigned tick=0;tick<600;++tick) {
        while(sequence<=tick) {
            auto packet=TonePacket(sequence,frequency);
            Check(buffer.Push(packet.data(),packet.size(),sequence*.005),"quality tone rejected");
            ++sequence;
        }
        const size_t frames=(static_cast<uint64_t>(tick+1)*outputRate/200)-
            (static_cast<uint64_t>(tick)*outputRate/200);
        output.resize(frames*2);buffer.Render(output.data(),frames,outputRate);Finite(output);
        // Exclude initial priming/fade and let the rate controller settle. No
        // phase matching is used, so the FIR's lookahead is explicitly harmless.
        if(tick>=100)for(size_t frame=0;frame<frames;++frame)steady.push_back(output[frame*2]);
    }
    auto snapshot=buffer.Snapshot();
    Check(snapshot.lostPackets==0&&snapshot.underrunEvents==0,"quality tone starved");
    double square=0;for(float value:steady)square+=static_cast<double>(value)*value;
    const double rmsRatio=std::sqrt(square/steady.size())/(.6/std::sqrt(2.0));
    std::cout<<"resampler rate="<<outputRate<<" tone="<<frequency<<" rms_ratio="<<rmsRatio;
    if(stopBand) {
        // 23.5 kHz is above a 44.1 kHz device's Nyquist frequency. Its folded
        // 20.6 kHz tone must be suppressed, rather than merely shifted in phase.
        Check(rmsRatio<.018,"downsampling alias insufficiently suppressed");
    } else {
        Check(rmsRatio>.90&&rmsRatio<1.08,"audible passband lost energy or gained distortion");
        if(outputRate==96000) {
            // An 18 kHz input's first interpolation image lies at 30 kHz. The
            // broad bands allow +/- 200 Hz, more than the controller's drift.
            const double desired=BandPower(steady,outputRate,frequency,200);
            const double image=BandPower(steady,outputRate,48000-frequency,200);
            const double imageRatio=image/desired;
            Check(desired>0&&imageRatio<.0003,"upsampling image insufficiently suppressed");
            std::cout<<" image_power_ratio="<<imageRatio;
        }
    }
    std::cout<<'\n';
}
int main() {
    try {
        AudioJitterBuffer buffer;
        auto invalid=Packet(0); Check(!buffer.Push(nullptr,invalid.size(),0),"null accepted");
        Check(!buffer.Push(invalid.data(),invalid.size()-1,0),"truncated accepted");
        invalid[0]=0; Check(!buffer.Push(invalid.data(),invalid.size(),0),"invalid magic accepted");
        invalid=Packet(0);invalid[12]=2;Check(!buffer.Push(invalid.data(),invalid.size(),0),"unknown flags accepted");
        invalid=Packet(UINT64_MAX);Check(!buffer.Push(invalid.data(),invalid.size(),0),"overflow sequence accepted");
        Check(Push(buffer,0,0),"first packet rejected");
        std::vector<float> output(240*2);buffer.Render(output.data(),240,48000);
        Check(!buffer.Snapshot().primed,"started before minimum jitter buffer");
        Check(Push(buffer,2,.010)&&Push(buffer,1,.011),"reordered packet rejected");
        Check(!Push(buffer,1,.012),"duplicate packet accepted");
        buffer.Render(output.data(),240,48000);Finite(output);
        Check(buffer.Snapshot().primed&&buffer.Snapshot().receivedPackets==3,"reorder did not prime");
        double energy=0;for(size_t frame=100;frame<240;++frame)energy+=output[frame*2]*output[frame*2];
        Check(energy>10,"tone not played");
        buffer.Render(output.data(),240,48000);
        Check(!Push(buffer,0,.020),"late played packet accepted");
        Check(buffer.Snapshot().latePackets==1,"late count wrong");
        buffer.Reset();Push(buffer,1,.005);Push(buffer,2,.010);
        Check(Push(buffer,0,.011,1),"initial reordered discontinuity rejected");
        Check(buffer.Snapshot().receivedPackets==3&&buffer.Snapshot().discardedFrames==0,"late flag erased future packets");
        Check(!Push(buffer,0,.012,1)&&buffer.Snapshot().queuedMs>=14.99,"duplicate flag reset queue");
        buffer.Render(output.data(),240,48000);buffer.Render(output.data(),240,48000);
        Check(!Push(buffer,0,.015,1)&&buffer.Snapshot().primed,"played flag restarted old audio");

        // One lost packet fades out; a following packet resumes without a hard step.
        buffer.Reset();for(uint64_t sequence=0;sequence<8;++sequence)if(sequence!=4)Push(buffer,sequence,sequence*.005,0,true);
        output.resize(8*240*2);buffer.Render(output.data(),8*240,48000);Finite(output);
        Check(buffer.Snapshot().lostPackets>=1,"loss was not concealed");
        double largestStep=0;for(size_t frame=1;frame<8*240;++frame)
            largestStep=std::max(largestStep,std::abs(static_cast<double>(output[frame*2]-output[(frame-1)*2])));
        Check(largestStep<.02,"hard click at loss/recovery");
        Check(output[6*240*2]>.65f,"audio did not resume after loss");

        // A burst cannot create an unbounded queue or replay seconds of stale audio.
        buffer.Reset();for(uint64_t sequence=0;sequence<500;++sequence)Push(buffer,sequence,.005*sequence);
        Check(buffer.Snapshot().queuedMs<=45.001&&buffer.Snapshot().discardedFrames>0,"backlog was not bounded");
        output.resize(240*2);buffer.Render(output.data(),240,44100);Finite(output);
        Check(buffer.Snapshot().primed,"alternate device rate not rendered");

        // A hard trim whose retained range starts with a hole must wait for a
        // contiguous new tail, rather than starting before the arriving packet.
        buffer.Reset();for(uint64_t sequence=0;sequence<6;++sequence)Push(buffer,sequence,sequence*.005);
        output.resize(128*2);buffer.Render(output.data(),128,48000);
        Push(buffer,10,.050);Check(!buffer.Snapshot().primed,"short trim tail started before priming");
        Push(buffer,11,.055);Push(buffer,12,.060);buffer.Render(output.data(),128,48000);Finite(output);
        Check(buffer.Snapshot().primed&&buffer.Snapshot().discardedFrames>0,"trim did not retain new tail");
        Check(buffer.Snapshot().lostPackets==0&&buffer.Snapshot().underrunEvents==0,
            "trim cursor started in a missing packet");

        // Muting consumes the same stream; silence and reprime stay muted.
        buffer.Reset();for(uint64_t sequence=0;sequence<6;++sequence)Push(buffer,sequence,.005*sequence,0,true);
        output.resize(2400*2);buffer.Render(output.data(),2400,48000,true);Finite(output);
        for(size_t frame=600;frame<2400;++frame)Check(std::abs(output[frame*2])<.0001f,"mute leaked audio");
        Check(buffer.Snapshot().renderedFrames>0&&!buffer.Snapshot().primed,"mute did not consume/reprime");
        Push(buffer,100,.500,1);Check(!buffer.Snapshot().primed,"discontinuity retained old playout");
        Check(buffer.Snapshot().queuedMs<=5.001,"discontinuity retained old queue");

        // Independent clocks, ordinary 10/20ms capture batches and device rates.
        for(double ratio:{.999,1.0,1.001})for(unsigned batch:{1u,2u,4u})ClockTest(ratio,batch,48000);
        ClockTest(.999,2,44100,10);ClockTest(1.001,2,96000,10);
        CaptureQuantumTest(false);CaptureQuantumTest(true);
        ResamplerQualityTest(44100,6000);ResamplerQualityTest(44100,18000);
        ResamplerQualityTest(44100,23500,true);ResamplerQualityTest(96000,18000);
        std::cout<<"PASS audio playout: validation, reorder, loss fades, bounded burst headroom, trim tail, device rates, mute, discontinuity, capture quantum/loss, 60s bidirectional clock drift, passband and alias suppression\n";
    } catch(const std::exception& error) {std::cerr<<"FAIL audio playout: "<<error.what()<<'\n';return 1;}
}
