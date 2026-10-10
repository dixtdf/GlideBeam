#pragma once
#include <CoreVideo/CoreVideo.h>
#include <cstdint>
#include <mutex>

enum class CaptureFailure { Transient, UserStopped, Permission, Permanent };

// These values are SDK ScreenCaptureKit codes. Unknown domains are never treated
// as a user cancellation; privacy failures always require an explicit retry.
inline CaptureFailure CaptureFailurePolicy(bool screenCaptureDomain, long code) {
    if (!screenCaptureDomain) return CaptureFailure::Transient;
    if (code == -3817) return CaptureFailure::UserStopped;
    if (code == -3801 || code == -3803) return CaptureFailure::Permission;
    if (code == -3812) return CaptureFailure::Permanent;
    return CaptureFailure::Transient;
}

inline uint64_t CaptureRetryDelayUs(unsigned attempt) {
    unsigned shift = attempt > 4 ? 4 : attempt;
    return uint64_t(1000000) << shift;
}

// The exact mailbox used by the live encoder and the recovery probe. Reset
// invalidates both stored pixels and already queued publications from a stream.
class CaptureFrameMailbox {
    std::mutex mutex_;
    CVPixelBufferRef pixel_ = nullptr;
    uint64_t epoch_ = 1, serial_ = 0, consumed_ = 0, capturedUs_ = 0;
public:
    ~CaptureFrameMailbox() { Reset(0); }
    void Reset(uint64_t epoch) {
        std::lock_guard lock(mutex_);
        if (pixel_) CVPixelBufferRelease(pixel_);
        pixel_ = nullptr; epoch_ = epoch; serial_ = consumed_ = capturedUs_ = 0;
    }
    bool Publish(uint64_t epoch, CVPixelBufferRef pixel, uint64_t capturedUs, bool& replaced) {
        if (!pixel) return false;
        std::lock_guard lock(mutex_);
        if (epoch != epoch_) return false;
        replaced = pixel_ && serial_ != consumed_;
        CVPixelBufferRetain(pixel);
        if (pixel_) CVPixelBufferRelease(pixel_);
        pixel_ = pixel; capturedUs_ = capturedUs; ++serial_;
        return true;
    }
    CVPixelBufferRef Take(uint64_t epoch, bool force, uint64_t& capturedUs) {
        std::lock_guard lock(mutex_);
        if (epoch != epoch_ || !pixel_ || (!force && serial_ == consumed_)) return nullptr;
        consumed_ = serial_; capturedUs = capturedUs_;
        return CVPixelBufferRetain(pixel_);
    }
};
