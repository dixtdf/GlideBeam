#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

enum class AudioPlaybackState { Stopped, Waiting, Playing, Muted, Unavailable };

struct AudioPlaybackSnapshot {
    AudioPlaybackState state = AudioPlaybackState::Stopped;
    std::string error;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    double queueMs = 0;
    double targetMs = 0;
    double devicePeriodMs = 0;
    double deviceQueuedMs = 0;
    double driftPpm = 0;
    // Packet starvation/concealment events reported by the jitter buffer.
    uint64_t underruns = 0;
    // Observed endpoint-buffer depletion during an already primed, recently
    // receiving stream with >1 device period of lead. This is a scheduling
    // diagnostic, not a driver-provided or bit-exact hardware xrun count.
    uint64_t deviceUnderruns = 0;
    uint64_t lost = 0;
    uint64_t late = 0;
    uint64_t discarded = 0;
    uint64_t renderedFrames = 0;
    uint64_t receivedPackets = 0;
    uint64_t deviceRestarts = 0;
    bool muted = false;
    bool lowLatency = false;
};

// Owns its COM/WASAPI and packet polling worker. Poll must be nonblocking and
// remains valid until Stop returns. Stop must precede transport destruction.
class AudioPlayer {
public:
    using Poll = std::function<int(uint8_t*, size_t)>;
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;
    bool Start(Poll poll);
    void Stop();
    void SetMuted(bool muted);
    AudioPlaybackSnapshot Snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
