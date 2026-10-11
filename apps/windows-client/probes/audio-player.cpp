// Standalone probe: compile this file alone, with -lole32 -luuid -lavrt.
// Including the implementation also exercises private device-format conversion
// without opening an output endpoint or playing sound in the default test mode.
#include "../audio.cpp"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

WAVEFORMATEX Wave(WORD tag, WORD bits, WORD channels = 2, DWORD rate = 48000) {
    WAVEFORMATEX wave{};
    wave.wFormatTag = tag;
    wave.nChannels = channels;
    wave.nSamplesPerSec = rate;
    wave.wBitsPerSample = bits;
    wave.nBlockAlign = channels * (bits / 8);
    wave.nAvgBytesPerSec = rate * wave.nBlockAlign;
    return wave;
}

WAVEFORMATEXTENSIBLE Extensible(WORD bits, WORD valid, WORD channels, DWORD mask) {
    WAVEFORMATEXTENSIBLE wave{};
    wave.Format = Wave(WAVE_FORMAT_EXTENSIBLE, bits, channels);
    wave.Format.cbSize = sizeof(wave) - sizeof(WAVEFORMATEX);
    wave.Samples.wValidBitsPerSample = valid;
    wave.dwChannelMask = mask;
    wave.SubFormat = PcmSubtype;
    return wave;
}

void FormatTests() {
    DeviceFormat format;
    std::array<BYTE, 64> bytes{};
    float stereo[] = { .5f, -.5f, 2.0f, -2.0f, std::numeric_limits<float>::quiet_NaN(), 0 };
    auto wave = Wave(WAVE_FORMAT_IEEE_FLOAT, 32, 2, 44100);
    Check(format.Parse(&wave) && format.rate == 44100, "device sample rate hardcoded");
    format.Encode(bytes.data(), stereo, 3);
    float converted[6]{};
    std::memcpy(converted, bytes.data(), sizeof(converted));
    Check(converted[0] == .5f && converted[1] == -.5f && converted[2] == 1 &&
        converted[3] == -1 && converted[4] == 0, "float clamp/finite conversion");
    wave = Wave(WAVE_FORMAT_PCM, 16);
    Check(format.Parse(&wave), "PCM16 rejected");
    format.Encode(bytes.data(), stereo, 2);
    int16_t pcm16[4]{};
    std::memcpy(pcm16, bytes.data(), sizeof(pcm16));
    Check(pcm16[0] == 16384 && pcm16[1] == -16384 && pcm16[2] == 32767 &&
        pcm16[3] == -32768, "PCM16 signs/clamp");
    wave = Wave(WAVE_FORMAT_PCM, 24);
    Check(format.Parse(&wave), "PCM24 rejected");
    format.Encode(bytes.data(), stereo, 1);
    Check(bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0x40 &&
        bytes[3] == 0 && bytes[4] == 0 && bytes[5] == 0xc0, "packed PCM24 signs");
    auto ext = Extensible(32, 24, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    Check(format.Parse(&ext.Format), "PCM24 in 32 container rejected");
    format.Encode(bytes.data(), stereo, 1);
    int32_t pcm32[2]{};
    std::memcpy(pcm32, bytes.data(), sizeof(pcm32));
    Check(pcm32[0] == 1073741824 && pcm32[1] == -1073741824, "valid bits not left aligned");
    wave = Wave(WAVE_FORMAT_IEEE_FLOAT, 32, 1, 96000);
    Check(format.Parse(&wave) && format.mask == 0, "regular format inherited previous mask");
    format.Encode(bytes.data(), stereo, 1);
    float mono = 1;
    std::memcpy(&mono, bytes.data(), sizeof(mono));
    Check(mono == 0, "mono stereo average wrong");
    ext = Extensible(16, 16, 6, KSAUDIO_SPEAKER_5POINT1);
    Check(format.Parse(&ext.Format), "5.1 layout rejected");
    format.Encode(bytes.data(), stereo, 1);
    Check(bytes[1] == 0x40 && bytes[3] == 0xc0, "front stereo mapping wrong");
    for (size_t i = 4; i < 12; ++i) Check(bytes[i] == 0, "surround/LFE duplicated stereo");
    ext = Extensible(16, 16, 2, SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY);
    Check(format.Parse(&ext.Format), "unusual speaker layout rejected");
    float same[] = { .5f, .5f };
    format.Encode(bytes.data(), same, 1);
    Check(bytes[1] == 0x40 && bytes[2] == 0 && bytes[3] == 0, "LFE received ordinary stereo");
    ext.dwChannelMask = SPEAKER_FRONT_LEFT;
    Check(!format.Parse(&ext.Format), "invalid channel mask accepted");
    wave = Wave(WAVE_FORMAT_PCM, 8);
    Check(!format.Parse(&wave), "unsupported PCM accepted");
    wave = Wave(WAVE_FORMAT_PCM, 16);
    wave.nBlockAlign = 1;
    Check(!format.Parse(&wave), "invalid alignment accepted");
}

int DeviceTest() {
    AudioPlayer player;
    player.SetMuted(true);
    std::atomic<unsigned> polls{0};
    uint64_t sequence = 0;
    double due = Seconds();
    Check(player.Start([&](uint8_t* output, size_t capacity) -> int {
        ++polls;
        double now = Seconds();
        if (now < due) return 0;
        due += .005;
        if (now - due > .1) due = now;
        Check(capacity >= remote_audio::PacketBytes, "packet capacity too small");
        RemoteAudioHeader header{};
        header.magic = 0x31414652;
        header.sample_rate = 48000;
        header.channels = 2;
        header.frames = 240;
        header.sequence = sequence++;
        header.capture_time_us = static_cast<uint64_t>(now * 1000000);
        std::memcpy(output, &header, sizeof(header));
        std::memset(output + sizeof(header), 0, remote_audio::PacketBytes - sizeof(header));
        return static_cast<int>(remote_audio::PacketBytes);
    }), "audio worker start failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    auto snapshot = player.Snapshot();
    double beforeStop = Seconds();
    player.Stop();
    double stopMs = (Seconds() - beforeStop) * 1000;
    unsigned stoppedPolls = polls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(polls.load() == stoppedPolls, "callback called after Stop joined");
    Check(stopMs < 1000, "Stop did not interrupt worker wait");
    if (!snapshot.sampleRate) {
        std::cout << "{\"device_test\":\"unavailable\",\"muted\":true,\"stop_ms\":" << stopMs
            << ",\"error\":\"" << snapshot.error << "\"}\n";
        return 77;
    }
    Check(snapshot.state == AudioPlaybackState::Muted && snapshot.muted, "muted state missing");
    Check(snapshot.receivedPackets > 50 && snapshot.renderedFrames > 0, "muting stopped live consumption");
    Check(snapshot.devicePeriodMs > 0 && snapshot.deviceQueuedMs <= snapshot.devicePeriodMs * 2.1,
        "local device queue exceeded bounded lead");
    std::cout << "{\"device_test\":\"pass\",\"muted\":true,\"sample_rate\":" << snapshot.sampleRate
        << ",\"channels\":" << snapshot.channels << ",\"device_period_ms\":" << snapshot.devicePeriodMs
        << ",\"device_queue_ms\":" << snapshot.deviceQueuedMs << ",\"received_packets\":" << snapshot.receivedPackets
        << ",\"rendered_frames\":" << snapshot.renderedFrames << ",\"underruns\":" << snapshot.underruns
        << ",\"device_underruns\":" << snapshot.deviceUnderruns
        << ",\"lost_packets\":" << snapshot.lost << ",\"stop_ms\":" << stopMs << "}\n";
    return 0;
}
}

int main(int argc, char** argv) {
    try {
        FormatTests();
        std::cout << "{\"format_conversion\":\"pass\",\"float_pcm16_pcm24_pcm32\":true,\"mono_surround\":true,\"audible_test\":false}\n";
        if (argc == 2 && std::string(argv[1]) == "--device") return DeviceTest();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio player probe failed: " << error.what() << '\n';
        return 1;
    }
}
