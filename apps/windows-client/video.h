#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include "../../shared/protocol.h"
#include "../../shared/latency-stats.h"

// Local CPU wall-clock measurements; GPU completion and scanout are not timed.
struct VideoTimingSnapshot {
    LatencySnapshot decodeInput, decodeOutput, conversionSubmit, presentWait, presentCall;
    LatencySnapshot frameProcess, pendingWait, presentInterval, receiveInterval, workerWait;
    uint64_t decodedFrames = 0, pendingReplaced = 0, presentWaitTimeouts = 0;
    uint64_t presentBusy = 0, presentOccluded = 0;
    bool workerHighResolutionWait = false;
    uint64_t workerFallbackWaits = 0, workerTimerFailures = 0;
};
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    bool Initialize(HWND canvas, uint32_t width, uint32_t height);
    bool Decode(const RemoteFrameHeader& frame, const uint8_t* annexb, size_t length);
    // Retry the one latest decoded frame on the same COM/video worker thread.
    bool Tick();
    bool Finish();
    void SetViewport(const RECT& rect);
    bool Hardware() const;
    double LastDecodeMs() const;
    uint64_t Presented() const;
    // Call on the COM/video worker, alongside Decode and Tick.
    VideoTimingSnapshot TimingSnapshot() const;
    const std::string& Error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
