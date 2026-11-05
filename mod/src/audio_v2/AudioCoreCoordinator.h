#pragma once

#include "AudioCoreGate.h"
#include "AudioSink.h"
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
    AudioSinkKind sinkKind{AudioSinkKind::Wasapi};
    AudioSinkOpenParams sink{};
    std::size_t queueCapacity{8};
    std::uint64_t mediaGeneration{1};
    PcmMappingPolicy mappingPolicy{PcmMappingPolicy::ExactSourceInteger};
    bool verifyBitPerfect{};
    // Local WASAPI/ASIO deep reservoir: try MEM_LARGE_PAGES for the capture
    // queue. Never set for DLNA.
    bool preferLargePages{};
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
    std::size_t QueueCapacity() const noexcept {
        return queue_ ? queue_->Capacity() : 0u;
    }
    bool QueueUsesLargePages() const noexcept {
        return queue_ ? queue_->UsesLargePages() : false;
    }
    AudioCoreGatePhase Phase() const noexcept { return gate_.Phase(); }
    // Requested output until the endpoint opens, then its negotiated format.
    const PcmFormat& OutputFormat() const noexcept {
        return !sink_ || sink_->State() == AudioSinkState::Closed
            ? config_.sink.format : sink_->Format();
    }
    const PcmFormat& SourceFormat() const noexcept { return sourceFormat_; }
    bool IsSourceMappingExact() const noexcept {
        return config_.mappingPolicy == PcmMappingPolicy::ExactSourceInteger;
    }
    bool IsResampling() const noexcept {
        return sink_ && sink_->State() != AudioSinkState::Closed &&
               sink_->Format().sampleRate != sourceFormat_.sampleRate;
    }
    bool IsReducingBitDepth() const noexcept {
        return sink_ && sink_->State() != AudioSinkState::Closed &&
            sink_->Format().validBits < EffectiveSourceValidBits(sourceFormat_);
    }
    bool IsConverting() const noexcept {
        return sink_ && sink_->State() != AudioSinkState::Closed &&
            RequiresPcmConversion(sourceFormat_, sink_->Format());
    }
    std::uint64_t MediaGeneration() const noexcept { return config_.mediaGeneration; }
    const AudioCoreCoordinatorStats Stats() const noexcept;
    AudioSinkStats SinkStats() const noexcept {
        return sink_ ? sink_->Stats() : AudioSinkStats{};
    }
    AudioSinkState SinkState() const noexcept {
        return sink_ ? sink_->State() : AudioSinkState::Closed;
    }
    AudioSinkKind SinkKind() const noexcept {
        return sink_ ? sink_->Kind() : config_.sinkKind;
    }
    std::uint32_t BufferFrames() const noexcept {
        return sink_ ? sink_->BufferFrames() : 0;
    }
    void SetSinkResumeBlocked(bool blocked) noexcept {
        if (sink_) sink_->SetResumeBlocked(blocked);
    }
    std::size_t BufferedBlocks() const noexcept { return queue_ ? queue_->Size() : 0; }
    std::size_t BufferedFrames() const noexcept {
        // In Remote mode the samples move from this queue into the shared ring,
        // so both halves count as captured-but-not-played. Keeping the sum here
        // is what lets the pump water marks see the real reservoir.
        const auto queued = queue_ ? queue_->QueuedFrames() : 0;
        const auto remote = sink_ ? sink_->BufferedFrames() : 0;
        return queued + remote;
    }
    std::size_t PendingSourceFrames() const noexcept {
        // Control-plane EOS checks are in source frames and must include the
        // resampler's lookahead and staged output, even when the queue is empty.
        const auto queued = source_ ? source_->AvailableFrames() : 0;
        return queued + resampledSource_.PendingSourceFrames();
    }
    bool HasPendingAudio() const noexcept {
        // Conversion may be moving the last queue block into private staging
        // when the worker observes EOS. Always let it see the producer seal;
        // independent queue/staging snapshots cannot prove that it is empty.
        return PendingSourceFrames() != 0 ||
            (sink_ && sink_->State() != AudioSinkState::Closed &&
             (IsConverting() || sink_->Stats().submittedFrames != 0) &&
             !sink_->EndOfStreamComplete());
    }
    bool SealEndOfStream() noexcept {
        if (!queue_) return false;
        queue_->SealEndOfStream();
        if (sourceReadyEvent_) SetEvent(sourceReadyEvent_);
        return true;
    }
    bool EndOfStreamSubmitted() const noexcept {
        return sink_ && sink_->EndOfStreamSubmitted();
    }
    bool EndOfStreamComplete() const noexcept {
        return sink_ && sink_->EndOfStreamComplete();
    }

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
    std::unique_ptr<IAudioSink> sink_;
    HANDLE sourceReadyEvent_{};
    std::uint64_t nextSequence_{};
    std::atomic<std::uint64_t> capturedFrames_{};
    std::atomic<std::uint64_t> droppedFrames_{};
};

} // namespace ammod::audio_v2
