#pragma once

#include "AudioCoreGate.h"
#include "AppleOutputGate.h"
#include "BitPerfectVerifier.h"
#include "PcmQueue.h"
#include "QueuedPcmSource.h"
#include "ResampledPcmSource.h"
#include "WasapiExclusiveSink.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <windows.h>

namespace ammod::audio_v2 {

struct AudioCoreCoordinatorConfig final {
    WasapiSinkConfig sink{};
    std::size_t queueCapacity{8};
    std::uint64_t mediaGeneration{1};
    PcmMappingPolicy mappingPolicy{PcmMappingPolicy::ExactSourceInteger};
    bool verifyBitPerfect{};
};

struct AudioCoreCoordinatorStats final {
    std::uint64_t capturedFrames{};
    std::uint64_t droppedFrames{};
    QueuedPcmSourceStats source{};
};

// Owns the Apple-facing generation boundary around the reusable endpoint
// sink. Enable/Disable/OpenEndpoint/Start run on the coordinator thread. The
// P0 push methods are the only methods intended for Apple's decoder callback;
// they use a preallocated queue and never wait or allocate.
class AudioCoreCoordinator final {
public:
    AudioCoreCoordinator() noexcept;
    ~AudioCoreCoordinator();

    AudioCoreCoordinator(const AudioCoreCoordinator&) = delete;
    AudioCoreCoordinator& operator=(const AudioCoreCoordinator&) = delete;

    HRESULT Enable(const AudioCoreCoordinatorConfig& config) noexcept;
    void Disable() noexcept;

    bool PushP0Interleaved(std::uint64_t firstFrame,
                           const float* interleaved,
                           std::uint32_t frames,
                           bool discontinuity,
                           bool endOfStream) noexcept;
    bool PushP0Planar(std::uint64_t firstFrame,
                      const float* left,
                      const float* right,
                      std::uint32_t frames,
                      bool discontinuity,
                      bool endOfStream) noexcept;

    // Opening is allowed only while the generation is Capturing and at least
    // one P0 block has been prebuffered. Native output is still not suppressed
    // until Start succeeds and CommitCutover runs.
    HRESULT OpenEndpoint() noexcept;
    HRESULT Start() noexcept;
    void PauseEndpoint() noexcept;
    HRESULT ResumeEndpoint() noexcept;

    bool ShouldSuppressNative() const noexcept;
    bool IsEnabled() const noexcept { return gate_.UiEnabled(); }
    AudioCoreGatePhase Phase() const noexcept { return gate_.Phase(); }
    // Requested output until the endpoint opens, then its negotiated format.
    const PcmFormat& OutputFormat() const noexcept {
        return sink_.State() == WasapiSinkState::Closed ? config_.sink.format : sink_.Format();
    }
    const PcmFormat& SourceFormat() const noexcept { return sourceFormat_; }
    bool IsResampling() const noexcept {
        return sink_.State() != WasapiSinkState::Closed &&
               sink_.Format().sampleRate != sourceFormat_.sampleRate;
    }
    std::uint64_t MediaGeneration() const noexcept { return config_.mediaGeneration; }
    const AudioCoreCoordinatorStats Stats() const noexcept;
    const WasapiExclusiveSink& Sink() const noexcept { return sink_; }
    WasapiSinkState SinkState() const noexcept { return sink_.State(); }
    void SetSinkResumeBlocked(bool blocked) noexcept { sink_.SetResumeBlocked(blocked); }
    std::size_t BufferedBlocks() const noexcept { return queue_ ? queue_->Size() : 0; }
    std::size_t BufferedFrames() const noexcept {
        return queue_ ? queue_->QueuedFrames() : 0;
    }
    std::size_t PendingSourceFrames() const noexcept {
        // Control-plane EOS checks are in source frames and must include the
        // resampler's lookahead and staged output, even when the queue is empty.
        const auto queued = source_ ? source_->AvailableFrames() : 0;
        return queued + resampledSource_.PendingSourceFrames();
    }
    bool HasPendingAudio() const noexcept {
        // SRC may be moving the last queue block into its private filter when
        // the worker observes EOS. Always let a live SRC see the producer seal;
        // independent queue/filter snapshots cannot prove that it is empty.
        return PendingSourceFrames() != 0 ||
            (IsResampling() && !sink_.EndOfStreamSubmitted());
    }
    bool SealEndOfStream() noexcept {
        if (!queue_) return false;
        queue_->SealEndOfStream();
        if (sourceReadyEvent_) SetEvent(sourceReadyEvent_);
        return true;
    }
    bool EndOfStreamSubmitted() const noexcept { return sink_.EndOfStreamSubmitted(); }

private:
    bool PushResult(bool pushed, std::uint32_t frames) noexcept;
    IIntegerPcmSource& ActiveSource() noexcept;

    AudioCoreGate gate_{};
    AppleOutputGate outputGate_;
    AudioCoreCoordinatorConfig config_{};
    PcmFormat sourceFormat_{};
    AudioCoreGateToken token_{};
    std::unique_ptr<PcmQueue> queue_;
    std::optional<QueuedPcmSource> source_;
    ResampledPcmSource resampledSource_;
    BitPerfectVerifier verifier_{};
    WasapiExclusiveSink sink_{};
    HANDLE sourceReadyEvent_{};
    std::uint64_t nextSequence_{};
    std::atomic<std::uint64_t> capturedFrames_{};
    std::atomic<std::uint64_t> droppedFrames_{};
};

} // namespace ammod::audio_v2
