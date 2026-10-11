#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio.h"
#include "../../shared/audio-playout.hpp"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double Seconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
constexpr GUID PcmSubtype{WAVE_FORMAT_PCM, 0, 0x0010, {0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
constexpr GUID FloatSubtype{WAVE_FORMAT_IEEE_FLOAT, 0, 0x0010, {0x80,0,0,0xaa,0,0x38,0x9b,0x71}};

template<class T> class ComOwner {
public:
    ComOwner() = default;
    ComOwner(const ComOwner&) = delete;
    ComOwner& operator=(const ComOwner&) = delete;
    ~ComOwner() { Reset(); }
    T* Get() const { return value_; }
    T* operator->() const { return value_; }
    T** Put() { Reset(); return &value_; }
    void Reset() { if (value_) value_->Release(); value_ = nullptr; }
    explicit operator bool() const { return value_ != nullptr; }
private:
    T* value_ = nullptr;
};

// Windows delivers these callbacks outside the playback loop. Callbacks must
// stay nonblocking: they only publish a flag and wake the worker. Registration
// avoids putting routine device enumeration on the low-period audio thread.
class DefaultOutputNotification final : public IMMNotificationClient {
    std::atomic<ULONG> refs_{1};
public:
    std::atomic<bool> changed{false};
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~DefaultOutputNotification() { if (event) CloseHandle(event); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(IMMNotificationClient))) {
            *value = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG remaining = refs_.fetch_sub(1) - 1;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            changed.store(true, std::memory_order_release);
            if (event) SetEvent(event);
        }
        return S_OK;
    }
};

class DefaultOutputWatcher {
    IMMDeviceEnumerator* enumerator_ = nullptr;
    DefaultOutputNotification* callback_ = nullptr;
public:
    ~DefaultOutputWatcher() {
        if (enumerator_) {
            enumerator_->UnregisterEndpointNotificationCallback(callback_);
            enumerator_->Release();
        }
        if (callback_) callback_->Release();
    }
    bool Register(IMMDeviceEnumerator* enumerator) {
        if (callback_) return true;
        auto callback = new (std::nothrow) DefaultOutputNotification;
        if (!callback) return false;
        if (!callback->event || FAILED(enumerator->RegisterEndpointNotificationCallback(callback))) {
            callback->Release();
            return false;
        }
        enumerator_ = enumerator;
        enumerator_->AddRef();
        callback_ = callback;
        return true;
    }
    HANDLE Event() const { return callback_ ? callback_->event : nullptr; }
    bool Changed() { return callback_ && callback_->changed.exchange(false, std::memory_order_acq_rel); }
};

std::string HResult(const char* operation, HRESULT hr) {
    char message[128]{};
    std::snprintf(message, sizeof(message), "%s (0x%08lx)", operation, static_cast<unsigned long>(hr));
    return message;
}

// The device is opened in its own mix format. A stereo network stream maps to
// front L/R (or an averaged mono channel); the remaining surround channels stay
// silent instead of duplicating dialogue/music into every speaker.
struct DeviceFormat {
    uint32_t rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint16_t validBits = 0;
    uint16_t stride = 0;
    uint32_t mask = 0;
    bool floating = false;
    int left = 0;
    int right = 1;
    bool folded = false;

    bool Parse(const WAVEFORMATEX* wave) {
        if (!wave || !wave->nSamplesPerSec || wave->nSamplesPerSec > 384000 ||
            !wave->nChannels || wave->nChannels > 32) return false;
        rate = wave->nSamplesPerSec;
        channels = wave->nChannels;
        bits = validBits = wave->wBitsPerSample;
        stride = wave->nBlockAlign;
        mask = 0;
        folded = false;
        WORD tag = wave->wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE) {
            if (wave->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return false;
            auto ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
            validBits = ext->Samples.wValidBitsPerSample;
            mask = ext->dwChannelMask;
            if (IsEqualGUID(ext->SubFormat, FloatSubtype)) tag = WAVE_FORMAT_IEEE_FLOAT;
            else if (IsEqualGUID(ext->SubFormat, PcmSubtype)) tag = WAVE_FORMAT_PCM;
            else return false;
        }
        floating = tag == WAVE_FORMAT_IEEE_FLOAT;
        if ((!floating && tag != WAVE_FORMAT_PCM) ||
            (floating && (bits != 32 || validBits != 32)) || (!floating && bits != 16 && bits != 24 && bits != 32) ||
            !validBits || validBits > bits || stride != channels * (bits / 8)) return false;
        left = 0;
        right = channels == 1 ? 0 : 1;
        if (mask && channels > 1) {
            left = right = -1;
            int firstSpeaker = -1;
            unsigned index = 0;
            for (unsigned bit = 0; bit < 32; ++bit) {
                if (!(mask & (uint32_t(1) << bit))) continue;
                if (bit == 0) left = static_cast<int>(index);
                if (bit == 1) right = static_cast<int>(index);
                if (bit != 3 && firstSpeaker < 0) firstSpeaker = static_cast<int>(index);
                ++index;
            }
            if (index != channels) return false;
            if (left < 0 || right < 0) {
                // Unusual layouts must not route ordinary audio into LFE.
                if (firstSpeaker < 0) return false;
                left = firstSpeaker;
                right = firstSpeaker;
                folded = true;
            }
        }
        return true;
    }

    void Sample(BYTE* out, float value) const {
        if (!std::isfinite(value)) value = 0;
        value = std::clamp(value, -1.0f, 1.0f);
        if (floating) { std::memcpy(out, &value, sizeof(value)); return; }
        // Extensible PCM places valid bits at the most significant end of the
        // container; quantize before left-aligning to preserve that contract.
        const int64_t scale = int64_t(1) << (validBits - 1);
        int64_t signedValue = std::clamp<int64_t>(std::llround(static_cast<double>(value) * scale), -scale, scale - 1);
        uint32_t packed = static_cast<uint32_t>(signedValue * (int64_t(1) << (bits - validBits)));
        for (unsigned byte = 0; byte < bits / 8; ++byte) out[byte] = static_cast<BYTE>(packed >> (8 * byte));
    }

    void Encode(BYTE* out, const float* stereo, uint32_t frames) const {
        std::memset(out, 0, static_cast<size_t>(frames) * stride);
        for (uint32_t frame = 0; frame < frames; ++frame) {
            BYTE* row = out + static_cast<size_t>(frame) * stride;
            float l = stereo[frame * 2], r = stereo[frame * 2 + 1];
            if (channels == 1) Sample(row, (l + r) * .5f);
            else if (folded) Sample(row + left * (bits / 8), (l + r) * .5f);
            else {
                Sample(row + left * (bits / 8), l);
                Sample(row + right * (bits / 8), r);
            }
        }
    }
};

struct Device {
    ComOwner<IMMDevice> endpoint;
    ComOwner<IAudioClient> client;
    ComOwner<IAudioRenderClient> render;
    DeviceFormat format;
    std::wstring id;
    HANDLE event = nullptr;
    UINT32 bufferFrames = 0;
    UINT32 periodFrames = 0;
    UINT32 leadFrames = 0;
    bool lowLatency = false;
    bool started = false;

    ~Device() { Close(); }
    void Close() {
        if (client && started) client->Stop();
        started = false;
        render.Reset();
        client.Reset();
        endpoint.Reset();
        if (event) CloseHandle(event);
        event = nullptr;
        bufferFrames = periodFrames = leadFrames = 0;
        lowLatency = false;
        id.clear();
    }

    HRESULT Activate() {
        return endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.Put()));
    }

    HRESULT Open(IMMDeviceEnumerator* enumerator) {
        Close();
        HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.Put());
        if (FAILED(hr)) return hr;
        LPWSTR endpointId = nullptr;
        hr = endpoint->GetId(&endpointId);
        if (FAILED(hr)) return hr;
        id = endpointId;
        CoTaskMemFree(endpointId);
        hr = Activate();
        if (FAILED(hr)) return hr;
        WAVEFORMATEX* mix = nullptr;
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr)) return hr;
        if (!format.Parse(mix)) { CoTaskMemFree(mix); return AUDCLNT_E_UNSUPPORTED_FORMAT; }
        ComOwner<IAudioClient3> client3;
        if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3), reinterpret_cast<void**>(client3.Put())))) {
            UINT32 normal = 0, fundamental = 0, minimum = 0, maximum = 0;
            hr = client3->GetSharedModeEnginePeriod(mix, &normal, &fundamental, &minimum, &maximum);
            if (SUCCEEDED(hr) && fundamental && minimum && maximum >= minimum) {
                // Request about 2.5 ms, rounded to a legal device quantum.
                uint64_t requested = std::max<uint64_t>(minimum, (static_cast<uint64_t>(format.rate) + 399) / 400);
                requested = (requested + fundamental - 1) / fundamental * fundamental;
                requested = std::min<uint64_t>(requested, maximum / fundamental * fundamental);
                if (requested >= minimum && requested <= maximum) {
                    hr = client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        static_cast<UINT32>(requested), mix, nullptr);
                    if (SUCCEEDED(hr)) { periodFrames = static_cast<UINT32>(requested); lowLatency = true; }
                }
                if (!lowLatency && normal >= minimum && normal <= maximum && normal % fundamental == 0) {
                    client3.Reset();
                    hr = Activate();
                    if (SUCCEEDED(hr)) hr = client->QueryInterface(__uuidof(IAudioClient3), reinterpret_cast<void**>(client3.Put()));
                    if (SUCCEEDED(hr)) hr = client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, normal, mix, nullptr);
                    if (SUCCEEDED(hr)) { periodFrames = normal; lowLatency = true; }
                }
            }
        }
        if (!lowLatency) {
            client3.Reset();
            hr = Activate();
            if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
            if (SUCCEEDED(hr)) {
                REFERENCE_TIME normal = 0, minimum = 0;
                hr = client->GetDevicePeriod(&normal, &minimum);
                if (SUCCEEDED(hr)) periodFrames = static_cast<UINT32>(std::max<int64_t>(1,
                    (normal * format.rate + 9999999) / 10000000));
            }
        }
        client3.Reset();
        CoTaskMemFree(mix);
        if (FAILED(hr)) return hr;
        hr = client->GetBufferSize(&bufferFrames);
        if (FAILED(hr) || !bufferFrames || bufferFrames > format.rate * 2) return FAILED(hr) ? hr : E_INVALIDARG;
        leadFrames = std::min<UINT32>(bufferFrames, std::max<UINT32>(1, periodFrames * 2));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return HRESULT_FROM_WIN32(GetLastError());
        hr = client->SetEventHandle(event);
        if (FAILED(hr)) return hr;
        hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render.Put()));
        if (FAILED(hr)) return hr;
        BYTE* initial = nullptr;
        hr = render->GetBuffer(leadFrames, &initial);
        if (FAILED(hr)) return hr;
        hr = render->ReleaseBuffer(leadFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(hr)) return hr;
        hr = client->Start();
        if (SUCCEEDED(hr)) started = true;
        return hr;
    }

    bool IsDefault(IMMDeviceEnumerator* enumerator) const {
        ComOwner<IMMDevice> current;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, current.Put()))) return false;
        LPWSTR currentId = nullptr;
        if (FAILED(current->GetId(&currentId))) return false;
        bool same = currentId && id == currentId;
        CoTaskMemFree(currentId);
        return same;
    }
};
}

struct AudioPlayer::Impl {
    std::atomic<bool> muted{false};
    HANDLE stopEvent = nullptr;
    std::thread worker;
    mutable std::mutex snapshotMutex;
    AudioPlaybackSnapshot published;
    Poll poll;

    void Publish(const remote_audio::AudioJitterBuffer& jitter, const Device& device,
                 AudioPlaybackState state, const std::string& error, UINT32 padding,
                 uint64_t restarts, uint64_t deviceUnderruns) {
        auto queue = jitter.Snapshot();
        AudioPlaybackSnapshot snapshot;
        snapshot.state = state;
        snapshot.error = error;
        snapshot.muted = muted.load(std::memory_order_relaxed);
        snapshot.queueMs = queue.queuedMs;
        snapshot.targetMs = queue.targetMs;
        snapshot.driftPpm = queue.driftPpm;
        snapshot.underruns = queue.underrunEvents;
        snapshot.deviceUnderruns = deviceUnderruns;
        snapshot.lost = queue.lostPackets;
        snapshot.late = queue.latePackets;
        snapshot.discarded = queue.discardedFrames;
        snapshot.renderedFrames = queue.renderedFrames;
        snapshot.receivedPackets = queue.receivedPackets;
        snapshot.deviceRestarts = restarts;
        if (device.started) {
            snapshot.sampleRate = device.format.rate;
            snapshot.channels = device.format.channels;
            snapshot.devicePeriodMs = 1000.0 * device.periodFrames / device.format.rate;
            snapshot.deviceQueuedMs = 1000.0 * padding / device.format.rate;
            snapshot.lowLatency = device.lowLatency;
        }
        std::lock_guard lock(snapshotMutex);
        published = std::move(snapshot);
    }

    void Run() noexcept {
        HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized)) {
            std::lock_guard lock(snapshotMutex);
            published.state = AudioPlaybackState::Unavailable;
            published.error = HResult("Audio COM initialization failed", initialized);
            return;
        }
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
        if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
        try {
            RunInitialized();
        } catch (...) {
            std::lock_guard lock(snapshotMutex);
            published.state = AudioPlaybackState::Unavailable;
            published.error = "Audio playback worker failed";
        }
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
    }

    void RunInitialized() {
        ComOwner<IMMDeviceEnumerator> enumerator;
        Device device;
        DefaultOutputWatcher defaultOutput;
        remote_audio::AudioJitterBuffer jitter;
        std::array<uint8_t, 65536> packet{};
        std::vector<float> stereo;
        std::string error;
        uint64_t restarts = 0;
        uint64_t deviceUnderruns = 0;
        UINT32 padding = 0;
        double retryAt = 0, checkDefaultAt = 0, publishAt = 0;
        double lastPacketAt = -1, warmingUntil = 0;
        bool renderedLive = false;
        while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
            double now = Seconds();
            // Poll independently of video, at most one bounded batch per tick.
            // A stalled output device must never retain an old transport queue.
            for (unsigned count = 0; count < 64; ++count) {
                int bytes = poll(packet.data(), packet.size());
                if (bytes <= 0) break;
                if (static_cast<size_t>(bytes) <= packet.size()) {
                    double arrival = Seconds();
                    if (jitter.Push(packet.data(), static_cast<size_t>(bytes), arrival)) lastPacketAt = arrival;
                }
            }
            if (!device.started && defaultOutput.Changed()) retryAt = now;
            if (!device.started && now >= retryAt) {
                HRESULT hr = S_OK;
                if (!enumerator) hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                    __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.Put()));
                if (SUCCEEDED(hr)) {
                    defaultOutput.Register(enumerator.Get());
                    hr = device.Open(enumerator.Get());
                }
                if (FAILED(hr)) {
                    error = HResult("Audio output unavailable; retrying", hr);
                    device.Close();
                    retryAt = now + .5;
                } else {
                    error.clear();
                    jitter.Reset(); // Reopening must resume live audio, not replay stale samples.
                    stereo.resize(static_cast<size_t>(device.leadFrames) * 2);
                    checkDefaultAt = now + 1;
                    padding = 0;
                    lastPacketAt = -1;
                    renderedLive = false;
                    warmingUntil = Seconds() + std::max(.050, 2.0 * device.leadFrames / device.format.rate);
                    ++restarts;
                }
            }
            if (device.started && (defaultOutput.Changed() ||
                (!defaultOutput.Event() && now >= checkDefaultAt))) {
                checkDefaultAt = now + 1;
                if (defaultOutput.Event() || !device.IsDefault(enumerator.Get())) {
                    error = "Default audio output changed; reconnecting";
                    device.Close();
                    jitter.Reset();
                    renderedLive = false;
                    retryAt = now;
                }
            }
            if (device.started) {
                HRESULT hr = device.client->GetCurrentPadding(&padding);
                if (SUCCEEDED(hr) && padding <= device.bufferFrames && padding < device.leadFrames) {
                    auto queue = jitter.Snapshot();
                    // Zero padding is normal for a one-period legacy buffer.
                    // Only count an exhausted >1-period lead after a live feed
                    // and warmup. Waiting/prime/long packet absence cannot
                    // create a false endpoint-depletion event.
                    if (!padding && renderedLive && device.leadFrames > device.periodFrames &&
                        now >= warmingUntil && lastPacketAt >= 0 && now - lastPacketAt < .100 &&
                        queue.primed && queue.receiving) ++deviceUnderruns;
                    UINT32 frames = std::min(device.bufferFrames - padding, device.leadFrames - padding);
                    BYTE* data = nullptr;
                    hr = device.render->GetBuffer(frames, &data);
                    if (SUCCEEDED(hr)) {
                        jitter.Render(stereo.data(), frames, device.format.rate, muted.load(std::memory_order_relaxed));
                        device.format.Encode(data, stereo.data(), frames);
                        hr = device.render->ReleaseBuffer(frames, 0);
                        if (SUCCEEDED(hr)) {
                            padding += frames;
                            auto rendered = jitter.Snapshot();
                            renderedLive = rendered.primed && rendered.receiving &&
                                lastPacketAt >= 0 && now - lastPacketAt < .100;
                        }
                    }
                } else if (SUCCEEDED(hr) && padding > device.bufferFrames) hr = E_UNEXPECTED;
                if (FAILED(hr)) {
                    error = HResult("Audio output reset; reconnecting", hr);
                    device.Close();
                    jitter.Reset();
                    renderedLive = false;
                    retryAt = now + .1;
                }
            }
            if (now >= publishAt) {
                auto queue = jitter.Snapshot();
                AudioPlaybackState state = !device.started ? AudioPlaybackState::Unavailable :
                    muted.load(std::memory_order_relaxed) ? AudioPlaybackState::Muted :
                    queue.receiving && queue.primed ? AudioPlaybackState::Playing : AudioPlaybackState::Waiting;
                Publish(jitter, device, state, error, padding, restarts, deviceUnderruns);
                publishAt = now + .05;
            }
            HANDLE events[3] = {stopEvent};
            DWORD eventCount = 1;
            if (device.event) events[eventCount++] = device.event;
            if (defaultOutput.Event()) events[eventCount++] = defaultOutput.Event();
            DWORD wait = WaitForMultipleObjects(eventCount, events, FALSE, 2);
            if (wait == WAIT_OBJECT_0) break;
        }
        device.Close();
        Publish(jitter, device, AudioPlaybackState::Stopped, {}, 0, restarts, deviceUnderruns);
    }
};

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}
AudioPlayer::~AudioPlayer() { Stop(); }

bool AudioPlayer::Start(Poll poll) {
    Stop();
    if (!poll) return false;
    impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->stopEvent) return false;
    impl_->poll = std::move(poll);
    {
        std::lock_guard lock(impl_->snapshotMutex);
        impl_->published = {};
        impl_->published.state = AudioPlaybackState::Waiting;
        impl_->published.muted = impl_->muted.load(std::memory_order_relaxed);
    }
    try { impl_->worker = std::thread([this] { impl_->Run(); }); }
    catch (...) {
        CloseHandle(impl_->stopEvent);
        impl_->stopEvent = nullptr;
        impl_->poll = {};
        return false;
    }
    return true;
}

void AudioPlayer::Stop() {
    if (impl_->stopEvent) SetEvent(impl_->stopEvent);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->stopEvent) CloseHandle(impl_->stopEvent);
    impl_->stopEvent = nullptr;
    impl_->poll = {};
    std::lock_guard lock(impl_->snapshotMutex);
    impl_->published.state = AudioPlaybackState::Stopped;
}

void AudioPlayer::SetMuted(bool muted) { impl_->muted.store(muted, std::memory_order_relaxed); }

AudioPlaybackSnapshot AudioPlayer::Snapshot() const {
    std::lock_guard lock(impl_->snapshotMutex);
    return impl_->published;
}
