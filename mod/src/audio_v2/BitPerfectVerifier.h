#pragma once

#include "ExactSampleMapper.h"

#include <cstdint>

namespace ammod::audio_v2 {

struct BitPerfectVerifierStats final {
    std::uint64_t comparedFrames{};
    std::uint64_t comparedSamples{};
    std::uint64_t changedFrames{};
    std::uint64_t changedSamples{};
    std::uint64_t missingFrames{};
    std::uint64_t duplicatedFrames{};
    std::uint64_t reorderedBlocks{};
    std::uint64_t metadataFailures{};
    std::uint64_t mappingFailures{};
    bool endOfStream{};
};

// Compares the integer samples submitted by the sink with the exact mapping
// of the same P0 block. It intentionally consumes no side queue: the source
// block remains present while QueuedPcmSource maps it, so partial device
// buffers and block splits can be checked without an allocation or a hash
// collision assumption.
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
            ClassifyMetadataFailure(block, offsetFrames, frames);
            return false;
        }

        if (!haveBlock_) {
            const bool badContinuation = !firstBlock_ &&
                (block.sequence != nextSequence_ ||
                 (!block.discontinuity && block.firstFrame != nextFrame_));
            if (offsetFrames != 0 || badContinuation) {
                ++stats_.metadataFailures;
                ClassifyMetadataFailure(block, offsetFrames, frames);
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
            ClassifyMetadataFailure(block, offsetFrames, frames);
            return false;
        }

        const auto validBits = EffectiveSourceValidBits(block.format);
        bool equal = true;
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
                ++stats_.comparedSamples;
                if (expected != submitted[outputOffset + channel]) {
                    equal = false;
                    ++stats_.changedSamples;
                }
            }
            ++stats_.comparedFrames;
            if (!equal) ++stats_.changedFrames;
            equal = true;
        }

        currentOffsetFrames_ += frames;
        if (currentOffsetFrames_ == currentBlockFrames_) {
            nextSequence_ = currentSequence_ + 1;
            nextFrame_ = currentFirstFrame_ + currentBlockFrames_;
            firstBlock_ = false;
            haveBlock_ = false;
            if (block.endOfStream) {
                ended_ = true;
                stats_.endOfStream = true;
            }
        }
        return stats_.changedSamples == 0 && stats_.metadataFailures == 0 &&
               stats_.mappingFailures == 0;
    }

    const BitPerfectVerifierStats& Stats() const noexcept { return stats_; }

    bool Proven() const noexcept {
        return ended_ && stats_.comparedFrames != 0 &&
               stats_.changedSamples == 0 && stats_.missingFrames == 0 &&
               stats_.duplicatedFrames == 0 && stats_.reorderedBlocks == 0 &&
               stats_.metadataFailures == 0 && stats_.mappingFailures == 0;
    }

private:
    void ClassifyMetadataFailure(const PcmBlock& block,
                                 std::uint32_t offsetFrames,
                                 std::uint32_t frames) noexcept {
        if (!firstBlock_ && block.sequence > nextSequence_) {
            stats_.missingFrames += frames;
            ++stats_.reorderedBlocks;
        } else if (haveBlock_ && block.sequence == currentSequence_ &&
                   offsetFrames < currentOffsetFrames_) {
            stats_.duplicatedFrames += frames;
            ++stats_.reorderedBlocks;
        } else if (!firstBlock_ && block.sequence != nextSequence_) {
            ++stats_.reorderedBlocks;
        }
    }

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
