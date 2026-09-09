#pragma once

#include "AudioCoreCoordinator.h"

#include <atomic>
#include <cstdint>

namespace ammod::audio_v2 {

struct ApplePcmTapStats final {
    std::uint64_t acceptedFrames{};
    std::uint64_t acceptedBlocks{};
    std::uint64_t ignoredFrames{};
    std::uint64_t droppedFrames{};
    std::uint64_t bindingFailures{};
};

// A deliberately small Apple-facing adapter. The coordinator thread binds a
// converter only after the active AudioUnit graph has been proven. The tap
// then forwards the decoder's already-decoded P0 buffers into the preallocated
// coordinator queue. It never infers identity from creation order and never
// allocates or waits in the callback.
class ApplePcmTap final {
public:
    explicit ApplePcmTap(AudioCoreCoordinator& coordinator) noexcept
        : coordinator_(coordinator) {}

    ApplePcmTap(const ApplePcmTap&) = delete;
    ApplePcmTap& operator=(const ApplePcmTap&) = delete;

    // The caller must have quiesced the previous converter callback before
    // changing the binding. A different media generation always uses a new
    // coordinator queue and cannot reuse a stale binding.
    bool BindActive(void* converter,
                    const PcmFormat& format,
                    std::uint64_t mediaGeneration) noexcept {
        if (!converter || format != coordinator_.Format() ||
            mediaGeneration != coordinator_.MediaGeneration() ||
            coordinator_.Phase() != AudioCoreGatePhase::Capturing) {
            bindingFailures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        endSeen_.store(false, std::memory_order_release);
        mediaGeneration_.store(mediaGeneration, std::memory_order_release);
        converter_.store(converter, std::memory_order_release);
        return true;
    }

    void Retire(void* converter) noexcept {
        void* expected = converter;
        converter_.compare_exchange_strong(expected, nullptr,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire);
        endSeen_.store(false, std::memory_order_release);
    }

    bool OnInterleaved(void* converter,
                       std::uint64_t firstFrame,
                       const float* samples,
                       std::uint32_t frames,
                       bool discontinuity,
                       bool endOfStream) noexcept {
        if (!Bound(converter) || endSeen_.load(std::memory_order_acquire)) {
            ignoredFrames_.fetch_add(frames, std::memory_order_relaxed);
            return false;
        }
        const bool accepted = coordinator_.PushP0Interleaved(
            firstFrame, samples, frames, discontinuity, endOfStream);
        if (accepted) {
            acceptedFrames_.fetch_add(frames, std::memory_order_relaxed);
            acceptedBlocks_.fetch_add(
                (frames + kMaxBlockFrames - 1u) / kMaxBlockFrames,
                std::memory_order_relaxed);
            if (endOfStream) endSeen_.store(true, std::memory_order_release);
        } else {
            droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        }
        return accepted;
    }

    bool OnPlanar(void* converter,
                  std::uint64_t firstFrame,
                  const float* left,
                  const float* right,
                  std::uint32_t frames,
                  bool discontinuity,
                  bool endOfStream) noexcept {
        if (!Bound(converter) || endSeen_.load(std::memory_order_acquire)) {
            ignoredFrames_.fetch_add(frames, std::memory_order_relaxed);
            return false;
        }
        const bool accepted = coordinator_.PushP0Planar(
            firstFrame, left, right, frames, discontinuity, endOfStream);
        if (accepted) {
            acceptedFrames_.fetch_add(frames, std::memory_order_relaxed);
            acceptedBlocks_.fetch_add(
                (frames + kMaxBlockFrames - 1u) / kMaxBlockFrames,
                std::memory_order_relaxed);
            if (endOfStream) endSeen_.store(true, std::memory_order_release);
        } else {
            droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        }
        return accepted;
    }

    bool IsBound(void* converter) const noexcept {
        return Bound(converter);
    }

    ApplePcmTapStats Stats() const noexcept {
        return ApplePcmTapStats{
            acceptedFrames_.load(std::memory_order_acquire),
            acceptedBlocks_.load(std::memory_order_acquire),
            ignoredFrames_.load(std::memory_order_acquire),
            droppedFrames_.load(std::memory_order_acquire),
            bindingFailures_.load(std::memory_order_acquire),
        };
    }

private:
    bool Bound(void* converter) const noexcept {
        return converter && converter_.load(std::memory_order_acquire) == converter &&
               mediaGeneration_.load(std::memory_order_acquire) == coordinator_.MediaGeneration() &&
               coordinator_.IsEnabled();
    }

    AudioCoreCoordinator& coordinator_;
    std::atomic<void*> converter_{};
    std::atomic<std::uint64_t> mediaGeneration_{};
    std::atomic<bool> endSeen_{};
    std::atomic<std::uint64_t> acceptedFrames_{};
    std::atomic<std::uint64_t> acceptedBlocks_{};
    std::atomic<std::uint64_t> ignoredFrames_{};
    std::atomic<std::uint64_t> droppedFrames_{};
    std::atomic<std::uint64_t> bindingFailures_{};
};

} // namespace ammod::audio_v2
