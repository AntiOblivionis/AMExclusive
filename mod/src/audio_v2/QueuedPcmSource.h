#pragma once

#include "BitPerfectVerifier.h"
#include "ExactSampleMapper.h"
#include "LocalFloatIntegerizer.h"
#include "PcmQueue.h"
#include "WasapiExclusiveSink.h"

#include <cstdint>

namespace ammod::audio_v2 {

enum class PcmMappingPolicy : std::uint8_t {
    ExactSourceInteger = 0,
    LocalFloat32ToPcm32 = 1,
};

struct QueuedPcmSourceStats final {
    std::uint64_t mappedFrames{};
    std::uint64_t queueUnderruns{};
    std::uint64_t mappingFailures{};
    std::uint64_t continuityFailures{};
    std::uint64_t verificationFailures{};
    std::uint64_t integerizedSamples{};
    std::uint64_t clippedSamples{};
    std::uint64_t nonFiniteSamples{};
};

// Bridges the project-owned P0 queue to the endpoint-facing integer source
// contract. It never waits for a producer and never allocates from Fill().
class QueuedPcmSource final : public IIntegerPcmSource {
public:
    QueuedPcmSource(PcmQueue& queue, const PcmFormat& format,
                    std::uint64_t mediaGeneration,
                    BitPerfectVerifier* verifier = nullptr,
                    PcmMappingPolicy mappingPolicy = PcmMappingPolicy::ExactSourceInteger) noexcept
        : queue_(queue), format_(format), mediaGeneration_(mediaGeneration),
          verifier_(verifier), mappingPolicy_(mappingPolicy) {}

    bool CanProvide(std::uint32_t capacityFrames) const noexcept override {
        if (capacityFrames == 0 || ended_) return true;
        const auto queued = queue_.QueuedFrames();
        if (queued >= static_cast<std::size_t>(blockOffsetFrames_) + capacityFrames) {
            return true;
        }
        // A terminal block or a producer-sealed local queue may be shorter than
        // the device period. It is safe to acquire one buffer for the remaining
        // media frames because Fill() will mark that submission terminal; a
        // non-terminal short queue still means recoverable producer wait.
        if (queue_.EndOfStreamSealed() && queued > blockOffsetFrames_) return true;
        const auto* block = queue_.Front();
        return block && block->endOfStream && block->frames > blockOffsetFrames_;
    }

    std::size_t AvailableFrames() const noexcept override {
        if (ended_) return 0;
        const auto queued = queue_.QueuedFrames();
        return queued > blockOffsetFrames_ ? queued - blockOffsetFrames_ : 0;
    }

    HRESULT Fill(std::int32_t* destination, std::uint32_t capacityFrames,
                 std::uint32_t& writtenFrames, bool& endOfStream) noexcept override {
        writtenFrames = 0;
        endOfStream = false;
        if (!destination || capacityFrames == 0) return E_INVALIDARG;
        if (!IsValidFormat(format_)) return E_INVALIDARG;

        if (ended_) {
            endOfStream = true;
            return S_OK;
        }

        while (writtenFrames < capacityFrames) {
            PcmBlock* block = queue_.Front();
            if (!block) {
                if (queue_.EndOfStreamSealed()) {
                    ended_ = true;
                    endOfStream = true;
                    return S_OK;
                }
                ++stats_.queueUnderruns;
                // Preserve the recoverable-wait status. The sink preflights
                // availability before GetBuffer, so this path is only a
                // producer/consumer race or a direct source diagnostic call.
                return kAudioSourceWouldBlock;
            }
            if (!ValidateBlock(*block)) {
                ++stats_.continuityFailures;
                return E_FAIL;
            }

            const std::uint32_t available = block->frames - blockOffsetFrames_;
            if (available == 0) {
                // A malformed block must never make the consumer spin.
                ++stats_.continuityFailures;
                return E_FAIL;
            }

            const std::uint32_t take = (available < (capacityFrames - writtenFrames))
                ? available
                : (capacityFrames - writtenFrames);
            const auto inputOffset = static_cast<std::size_t>(blockOffsetFrames_) *
                                     kSupportedChannels;
            const auto outputOffset = static_cast<std::size_t>(writtenFrames) *
                                      kSupportedChannels;
            if (mappingPolicy_ == PcmMappingPolicy::LocalFloat32ToPcm32) {
                LocalFloatIntegerizerStats integerizer{};
                if (!IntegerizeLocalFloatInterleaved(
                        block->samples.data() + inputOffset,
                        destination + outputOffset,
                        static_cast<std::size_t>(take) * kSupportedChannels,
                        integerizer)) {
                    ++stats_.mappingFailures;
                    stats_.integerizedSamples += integerizer.samples;
                    stats_.clippedSamples += integerizer.clippedSamples;
                    stats_.nonFiniteSamples += integerizer.nonFiniteSamples;
                    return E_FAIL;
                }
                stats_.integerizedSamples += integerizer.samples;
                stats_.clippedSamples += integerizer.clippedSamples;
                stats_.nonFiniteSamples += integerizer.nonFiniteSamples;
            } else {
                if (!MapP0Interleaved(block->samples.data() + inputOffset,
                                      destination + outputOffset,
                                      static_cast<std::size_t>(take) * kSupportedChannels,
                                      EffectiveSourceValidBits(block->format))) {
                    ++stats_.mappingFailures;
                    return E_FAIL;
                }
                if (verifier_ && !verifier_->VerifyBlockSpan(
                        *block, blockOffsetFrames_, destination + outputOffset, take)) {
                    ++stats_.verificationFailures;
                    return E_FAIL;
                }
            }

            blockOffsetFrames_ += take;
            writtenFrames += take;
            stats_.mappedFrames += take;
            if (blockOffsetFrames_ != block->frames) continue;

            const bool blockEnd = block->endOfStream;
            expectedSequence_ = block->sequence + 1;
            expectedFrame_ = block->firstFrame + block->frames;
            queue_.Pop();
            blockOffsetFrames_ = 0;
            firstBlock_ = false;
            if (blockEnd) {
                if (queue_.Size() != 0) {
                    ++stats_.continuityFailures;
                    return E_FAIL;
                }
                ended_ = true;
                endOfStream = true;
                return S_OK;
            }
        }
        // If a producer seal landed exactly on a complete device period, the
        // loop exits at capacity before observing an empty queue. Mark that
        // just-released full buffer terminal as well, avoiding a synthetic
        // all-silence buffer on the next render event.
        if (queue_.EndOfStreamSealed() && queue_.Size() == 0 && blockOffsetFrames_ == 0) {
            ended_ = true;
            endOfStream = true;
        }
        return S_OK;
    }

    const QueuedPcmSourceStats& Stats() const noexcept { return stats_; }
    bool Ended() const noexcept { return ended_; }

private:
    bool ValidateBlock(const PcmBlock& block) const noexcept {
        if (block.frames == 0 || block.frames > kMaxBlockFrames ||
            block.format != format_ || block.mediaGeneration != mediaGeneration_ ||
            blockOffsetFrames_ >= block.frames) {
            return false;
        }
        if (firstBlock_) {
            return true;
        }
        if (block.sequence != expectedSequence_) return false;
        // A converter reset at a natural repeat/gapless boundary preserves
        // queue order but restarts the source-local frame cursor at zero.  The
        // producer marks that first block discontinuous explicitly; accept the
        // cursor restart only on that marker.  Ordinary blocks remain subject
        // to exact firstFrame continuity.
        return block.discontinuity || block.firstFrame == expectedFrame_;
    }

    PcmQueue& queue_;
    PcmFormat format_{};
    std::uint64_t mediaGeneration_{};
    BitPerfectVerifier* verifier_{};
    PcmMappingPolicy mappingPolicy_{PcmMappingPolicy::ExactSourceInteger};
    std::uint64_t expectedSequence_{};
    std::uint64_t expectedFrame_{};
    std::uint32_t blockOffsetFrames_{};
    bool firstBlock_{true};
    bool ended_{};
    QueuedPcmSourceStats stats_{};
};

} // namespace ammod::audio_v2
