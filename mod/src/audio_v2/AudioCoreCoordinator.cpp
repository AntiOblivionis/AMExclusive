#include "AudioCoreCoordinator.h"

#include <new>

namespace ammod::audio_v2 {

AudioCoreCoordinator::AudioCoreCoordinator() noexcept
    : outputGate_(gate_) {}

AudioCoreCoordinator::~AudioCoreCoordinator() {
    Disable();
}

HRESULT AudioCoreCoordinator::Enable(const AudioCoreCoordinatorConfig& config) noexcept {
    // Enable may follow a runtime-side retirement that already closed this
    // coordinator. Keep Disable() as the safety boundary, but make the no-op
    // case cheap so one Apple graph transition cannot multiply Stop/Close
    // work through Runtime -> Coordinator -> Sink layers.
    Disable();
    if (!IsValidFormat(config.sink.format) || config.queueCapacity == 0 ||
        config.mediaGeneration == 0) {
        return E_INVALIDARG;
    }

    sourceReadyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!sourceReadyEvent_) return HRESULT_FROM_WIN32(GetLastError());

    gate_.SetUiEnabled(true);
    const auto token = gate_.BeginCapture();
    if (!token) {
        gate_.SetUiEnabled(false);
        CloseHandle(sourceReadyEvent_);
        sourceReadyEvent_ = nullptr;
        return E_UNEXPECTED;
    }

    try {
        queue_ = std::make_unique<PcmQueue>(config.queueCapacity);
    } catch (const std::bad_alloc&) {
        gate_.SetUiEnabled(false);
        CloseHandle(sourceReadyEvent_);
        sourceReadyEvent_ = nullptr;
        return E_OUTOFMEMORY;
    }

    config_ = config;
    config_.sink.sourceReadyEvent = sourceReadyEvent_;
    token_ = *token;
    nextSequence_ = 0;
    verifier_.Reset(config_.sink.format, config_.mediaGeneration);
    BitPerfectVerifier* verifier = config_.mappingPolicy == PcmMappingPolicy::ExactSourceInteger
        ? &verifier_ : nullptr;
    source_.emplace(*queue_, config_.sink.format, config_.mediaGeneration,
                    verifier, config_.mappingPolicy);
    capturedBlocks_.store(0, std::memory_order_release);
    capturedFrames_.store(0, std::memory_order_release);
    droppedBlocks_.store(0, std::memory_order_release);
    droppedFrames_.store(0, std::memory_order_release);
    return S_OK;
}

void AudioCoreCoordinator::Disable() noexcept {
    if (!gate_.UiEnabled() && !queue_ && !source_ && !sourceReadyEvent_ &&
        sink_.State() == WasapiSinkState::Closed) {
        return;
    }

    // Invalidate the callback token first. The hook owner must quiesce the P0
    // callback before this method is called; only then is consumer-side Clear
    // safe under the SPSC contract.
    outputGate_.Reset();
    gate_.SetUiEnabled(false);
    if (sourceReadyEvent_) SetEvent(sourceReadyEvent_);
    sink_.Stop();
    sink_.Close();
    source_.reset();
    if (queue_) queue_->Clear();
    queue_.reset();
    if (sourceReadyEvent_) {
        CloseHandle(sourceReadyEvent_);
        sourceReadyEvent_ = nullptr;
    }
    token_ = {};
    nextSequence_ = 0;
}

bool AudioCoreCoordinator::PushResult(bool pushed, std::uint32_t frames) noexcept {
    if (pushed) {
        capturedBlocks_.fetch_add(
            (frames + kMaxBlockFrames - 1u) / kMaxBlockFrames,
            std::memory_order_relaxed);
        capturedFrames_.fetch_add(frames, std::memory_order_relaxed);
        if (sourceReadyEvent_) SetEvent(sourceReadyEvent_);
    } else {
        droppedBlocks_.fetch_add(
            (frames + kMaxBlockFrames - 1u) / kMaxBlockFrames,
            std::memory_order_relaxed);
        droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        gate_.Fail(token_);
    }
    return pushed;
}

bool AudioCoreCoordinator::PushP0Interleaved(std::uint64_t firstFrame,
                                             const float* interleaved,
                                             std::uint32_t frames,
                                             bool discontinuity,
                                             bool endOfStream) noexcept {
    if (!queue_ || !gate_.CanCapture(token_)) {
        droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
        droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        return false;
    }
    const bool pushed = queue_->TryPush(config_.sink.format, config_.mediaGeneration,
                                        nextSequence_, firstFrame, interleaved, frames,
                                        discontinuity, endOfStream);
    return PushResult(pushed, frames);
}

bool AudioCoreCoordinator::PushP0Planar(std::uint64_t firstFrame,
                                        const float* left,
                                        const float* right,
                                        std::uint32_t frames,
                                        bool discontinuity,
                                        bool endOfStream) noexcept {
    if (!queue_ || !gate_.CanCapture(token_)) {
        droppedBlocks_.fetch_add(1, std::memory_order_relaxed);
        droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        return false;
    }
    const bool pushed = queue_->TryPushPlanar(config_.sink.format, config_.mediaGeneration,
                                              nextSequence_, firstFrame, left, right, frames,
                                              discontinuity, endOfStream);
    return PushResult(pushed, frames);
}

HRESULT AudioCoreCoordinator::OpenEndpoint() noexcept {
    if (!queue_ || !source_ || !gate_.CanOpenEndpoint(token_)) return E_UNEXPECTED;
    if (queue_->Size() == 0) return E_PENDING;

    const HRESULT hr = sink_.Open(config_.sink);
    if (FAILED(hr)) {
        outputGate_.Reset();
        gate_.Fail(token_);
        return hr;
    }
    if (!gate_.MarkPrebuffered(token_)) {
        sink_.Close();
        outputGate_.Reset();
        gate_.Fail(token_);
        return E_UNEXPECTED;
    }
    return S_OK;
}

HRESULT AudioCoreCoordinator::Start() noexcept {
    if (!source_ || !gate_.CanMutateAudio(token_) ||
        gate_.Phase() != AudioCoreGatePhase::Prebuffered) {
        return E_UNEXPECTED;
    }
    const HRESULT hr = sink_.Start(*source_);
    if (FAILED(hr)) {
        outputGate_.Reset();
        sink_.Close();
        gate_.Fail(token_);
        return hr;
    }
    if (!outputGate_.CommitCutover(token_)) {
        sink_.Stop();
        sink_.Close();
        outputGate_.Rollback(token_);
        return E_UNEXPECTED;
    }
    return S_OK;
}

void AudioCoreCoordinator::PauseEndpoint() noexcept {
    if (gate_.Phase() == AudioCoreGatePhase::Active &&
        sink_.State() == WasapiSinkState::Running) {
        sink_.Stop();
    }
}

HRESULT AudioCoreCoordinator::ResumeEndpoint() noexcept {
    if (!source_ || gate_.Phase() != AudioCoreGatePhase::Active) return E_UNEXPECTED;
    if (sink_.State() == WasapiSinkState::Running) return S_OK;
    if (sink_.State() != WasapiSinkState::Open) return E_UNEXPECTED;
    return sink_.Start(*source_);
}

bool AudioCoreCoordinator::ShouldSuppressNative() const noexcept {
    // The exclusive client remains the sole endpoint owner during a recoverable
    // P0/network wait.  It is stopped without acquiring a buffer and resumed
    // only after a complete source buffer is available.  Reopening Apple's
    // native path here would create a second owner while the exclusive client
    // is still initialized, and would advance Apple's timeline past the P0
    // frames waiting in this generation.
    return outputGate_.ShouldSuppress(token_);
}

const AudioCoreCoordinatorStats AudioCoreCoordinator::Stats() const noexcept {
    AudioCoreCoordinatorStats stats{};
    stats.capturedBlocks = capturedBlocks_.load(std::memory_order_acquire);
    stats.capturedFrames = capturedFrames_.load(std::memory_order_acquire);
    stats.droppedBlocks = droppedBlocks_.load(std::memory_order_acquire);
    stats.droppedFrames = droppedFrames_.load(std::memory_order_acquire);
    if (source_) stats.source = source_->Stats();
    return stats;
}

} // namespace ammod::audio_v2
