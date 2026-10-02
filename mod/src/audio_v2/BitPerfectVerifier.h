#pragma once

#include "ExactSampleMapper.h"

#include <cstdint>

namespace ammod::audio_v2 {

struct BitPerfectVerifierStats final {
    std::uint64_t changedSamples{};
    std::uint64_t metadataFailures{};
    std::uint64_t mappingFailures{};
};

// Compares canonical source PCM with the exact mapping of the same P0 block,
// before any optional resampling. Passing this check validates source mapping;
// it does not make resampled endpoint output bit-perfect. It intentionally
// consumes no side queue: the source block remains present while QueuedPcmSource
// maps it, so partial reads and block splits can be checked without an
// allocation or a hash collision assumption.
class BitPerfectVerifier final {
public:
    BitPerfectVerifier() noexcept = default;
    BitPerfectVerifier(const PcmFormat& format, std::uint64_t generation) noexcept {
        Reset(format, generation);
    }

    void Reset(const PcmFormat& format, std::uint64_t generation) noexcept {
        format_ = format;
        generation_ = generation;
        stats_ = {};
        firstBlock_ = true;
        haveBlock_ = false;
        ended_ = false;
        currentSequence_ = 0;
        currentFirstFrame_ = 0;
        currentBlockFrames_ = 0;
        currentOffsetFrames_ = 0;
        nextSequence_ = 0;
        nextFrame_ = 0;
    }

    bool VerifyBlockSpan(const PcmBlock& block,
                         std::uint32_t offsetFrames,
                         const std::int32_t* submitted,
                         std::uint32_t frames) noexcept {
        if (!submitted || frames == 0 || offsetFrames > block.frames ||
            frames > block.frames - offsetFrames || ended_ ||
            !IsValidFormat(format_) || block.format != format_ ||
            block.mediaGeneration != generation_) {
            ++stats_.metadataFailures;
            return false;
        }

        if (!haveBlock_) {
            const bool badContinuation = !firstBlock_ &&
                (block.sequence != nextSequence_ ||
                 (!block.discontinuity && block.firstFrame != nextFrame_));
            if (offsetFrames != 0 || badContinuation) {
                ++stats_.metadataFailures;
                return false;
            }
            currentSequence_ = block.sequence;
            currentFirstFrame_ = block.firstFrame;
            currentBlockFrames_ = block.frames;
            currentOffsetFrames_ = 0;
            haveBlock_ = true;
        } else if (block.sequence != currentSequence_ ||
                   block.firstFrame != currentFirstFrame_ ||
                   block.frames != currentBlockFrames_ ||
                   offsetFrames != currentOffsetFrames_) {
            ++stats_.metadataFailures;
            return false;
        }

        const auto validBits = EffectiveSourceValidBits(block.format);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const auto sourceOffset = static_cast<std::size_t>(offsetFrames + frame) *
                                      kSupportedChannels;
            const auto outputOffset = static_cast<std::size_t>(frame) * kSupportedChannels;
            for (std::uint32_t channel = 0; channel < kSupportedChannels; ++channel) {
                std::int32_t expected{};
                if (!MapP0Sample(block.samples[sourceOffset + channel], validBits, expected)) {
                    ++stats_.mappingFailures;
                    return false;
                }
                if (expected != submitted[outputOffset + channel]) {
                    ++stats_.changedSamples;
                }
            }
        }

        currentOffsetFrames_ += frames;
        if (currentOffsetFrames_ == currentBlockFrames_) {
            nextSequence_ = currentSequence_ + 1;
            nextFrame_ = currentFirstFrame_ + currentBlockFrames_;
            firstBlock_ = false;
            haveBlock_ = false;
            if (block.endOfStream) {
                ended_ = true;
            }
        }
        return stats_.changedSamples == 0 && stats_.metadataFailures == 0 &&
               stats_.mappingFailures == 0;
    }

private:
    PcmFormat format_{};
    std::uint64_t generation_{};
    BitPerfectVerifierStats stats_{};
    bool firstBlock_{true};
    bool haveBlock_{};
    bool ended_{};
    std::uint64_t currentSequence_{};
    std::uint64_t currentFirstFrame_{};
    std::uint32_t currentBlockFrames_{};
    std::uint32_t currentOffsetFrames_{};
    std::uint64_t nextSequence_{};
    std::uint64_t nextFrame_{};
};

} // namespace ammod::audio_v2
