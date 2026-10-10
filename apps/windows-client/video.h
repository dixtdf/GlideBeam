#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include "../../shared/protocol.h"
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
    const std::string& Error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
