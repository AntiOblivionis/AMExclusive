#pragma once

#include "PcmTypes.h"

#include <rigtorp/SPSCQueue.h>

#include <cstddef>
#include <cstdint>

namespace ammod::audio_v2 {

class PcmQueue final {
public:
    explicit PcmQueue(std::size_t capacity)
        : queue_(capacity) {}

    PcmQueue(const PcmQueue&) = delete;
    PcmQueue& operator=(const PcmQueue&) = delete;

    bool TryPush(const PcmFormat& format,
                 std::uint64_t mediaGeneration,
                 std::uint64_t& sequence,
                 std::uint64_t firstFrame,
                 const float* interleaved,
                 std::uint32_t frames,
                 bool discontinuity,
                 bool endOfStream) noexcept {
        if (!IsValidFormat(format) || !interleaved || frames == 0 ||
            endOfStreamSealed_.load(std::memory_order_acquire)) return false;
        const auto blocks = (frames + kMaxBlockFrames - 1u) / kMaxBlockFrames;
        if (queue_.size() + blocks > queue_.capacity()) return false;

        std::uint32_t remaining = frames;
        std::uint32_t offset = 0;
        while (remaining != 0) {
            const auto count = remaining > kMaxBlockFrames ? kMaxBlockFrames : remaining;
            const bool first = offset == 0;
            const bool last = count == remaining;
            if (!queue_.try_emplace(format, mediaGeneration, sequence++, firstFrame + offset,
                                    count, discontinuity && first, endOfStream && last,
                                    interleaved + static_cast<std::size_t>(offset) * kSupportedChannels)) {
                return false;
            }
            queuedFrames_.fetch_add(count, std::memory_order_release);
            offset += count;
            remaining -= count;
        }
        return true;
    }

    // Apple may expose the decoder's float32 P0 as one buffer per channel.
    // Canonicalize the two-channel planar form into the queue's interleaved
    // storage without allocating or waiting on the callback thread.
    bool TryPushPlanar(const PcmFormat& format,
                       std::uint64_t mediaGeneration,
                       std::uint64_t& sequence,
                       std::uint64_t firstFrame,
                       const float* left,
                       const float* right,
                       std::uint32_t frames,
                       bool discontinuity,
                       bool endOfStream) noexcept {
        if (!IsValidFormat(format) || !left || !right || frames == 0 ||
            endOfStreamSealed_.load(std::memory_order_acquire)) return false;
        const auto blocks = (frames + kMaxBlockFrames - 1u) / kMaxBlockFrames;
        if (queue_.size() + blocks > queue_.capacity()) return false;

        std::uint32_t remaining = frames;
        std::uint32_t offset = 0;
        while (remaining != 0) {
            const auto count = remaining > kMaxBlockFrames ? kMaxBlockFrames : remaining;
            const bool first = offset == 0;
            const bool last = count == remaining;
            if (!queue_.try_emplace(format, mediaGeneration, sequence++, firstFrame + offset,
                                    count, discontinuity && first, endOfStream && last,
                                    left + offset, right + offset)) {
                return false;
            }
            queuedFrames_.fetch_add(count, std::memory_order_release);
            offset += count;
            remaining -= count;
        }
        return true;
    }

    PcmBlock* Front() noexcept { return queue_.front(); }
    const PcmBlock* Front() const noexcept {
        // rigtorp::SPSCQueue exposes only a non-const front() because the
        // consumer owns the read cursor; this wrapper is a read-only view and
        // does not advance or mutate that cursor.
        return const_cast<PcmQueue*>(this)->queue_.front();
    }
    void Pop() noexcept {
        if (auto* block = queue_.front()) {
            queuedFrames_.fetch_sub(block->frames, std::memory_order_release);
            queue_.pop();
        }
    }
    // Producer-side terminal seal. No sample is added or modified: this only
    // declares that the frames already queued are the final frames of the
    // current media item. The consumer may therefore legally submit a short
    // terminal device period instead of waiting forever for more producer data.
    void SealEndOfStream() noexcept {
        endOfStreamSealed_.store(true, std::memory_order_release);
    }
    bool EndOfStreamSealed() const noexcept {
        return endOfStreamSealed_.load(std::memory_order_acquire);
    }

    // Consumer-side generation retirement. The producer must be quiesced by
    // the coordinator before calling this method.
    void Clear() noexcept {
        while (queue_.front()) Pop();
        endOfStreamSealed_.store(false, std::memory_order_release);
    }
    std::size_t Size() const noexcept { return queue_.size(); }
    std::size_t Capacity() const noexcept { return queue_.capacity(); }
    std::size_t QueuedFrames() const noexcept {
        return queuedFrames_.load(std::memory_order_acquire);
    }

private:
    rigtorp::SPSCQueue<PcmBlock> queue_;
    std::atomic<std::size_t> queuedFrames_{};
    std::atomic<bool> endOfStreamSealed_{};
};

} // namespace ammod::audio_v2
