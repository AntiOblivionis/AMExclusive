#include "AudioCoreRuntime.h"
#include "../AudioErrors.h"
#include "../AudioFormatPolicy.h"

#include <algorithm>
#include <cmath>

namespace ammod::audio_v2 {
namespace {

constexpr std::uint32_t kAlac = 0x616C6163u; // 'alac'
constexpr std::uint32_t kQlac = 0x716C6163u; // 'qlac'
constexpr std::uint32_t kReferenceTimePerSecond = 10000000u;
// Apple can return several large P0 fills while WASAPI is still negotiating
// the replacement endpoint. One P0 fill is commonly 19,200 frames at 96 kHz
// (ten queue blocks); 64 slots were therefore exhausted before OpenEndpoint
// could mark the generation prebuffered. Keep enough bounded storage for the
// observed cold-start burst without making the decoder callback wait.
constexpr std::size_t kProductionQueueCapacity = 256;

bool IsCandidateEncodedFormat(std::uint32_t formatId) noexcept {
    return formatId == kAlac || formatId == kQlac || formatId == kAppleLinearPcm;
}

} // namespace

AudioCoreRuntime::AudioCoreRuntime() noexcept
    : tap_(coordinator_) {}

AudioCoreRuntime::~AudioCoreRuntime() {
    Stop();
}

bool AudioCoreRuntime::Start(const AudioCoreRuntimeCallbacks& callbacks) noexcept {
    std::lock_guard lock(controlMutex_);
    if (running_.load(std::memory_order_acquire)) return true;
    callbacks_ = callbacks;
    rawFillCalls_.store(0, std::memory_order_release);
    fillSuccessWithData_.store(0, std::memory_order_release);
    fillSuccessZeroPackets_.store(0, std::memory_order_release);
    fillErrors_.store(0, std::memory_order_release);
    producedPackets_.store(0, std::memory_order_release);
    rejectUiOff_.store(0, std::memory_order_release);
    rejectNoOutputData_.store(0, std::memory_order_release);
    rejectNoOutputPackets_.store(0, std::memory_order_release);
    rejectZeroPackets_.store(0, std::memory_order_release);
    rejectNoDestinationFormat_.store(0, std::memory_order_release);
    rejectInvalidBufferList_.store(0, std::memory_order_release);
    rejectRegistryMissing_.store(0, std::memory_order_release);
    rejectRegistryUnbound_.store(0, std::memory_order_release);
    rejectRateMismatch_.store(0, std::memory_order_release);
    rejectChannelMismatch_.store(0, std::memory_order_release);
    rejectParse_.store(0, std::memory_order_release);
    rejectClaimFrames_.store(0, std::memory_order_release);
    rejectTap_.store(0, std::memory_order_release);
    acceptedCalls_.store(0, std::memory_order_release);
    acceptedFrames_.store(0, std::memory_order_release);
    lastFillResult_.store(0, std::memory_order_release);
    lastProducedPackets_.store(0, std::memory_order_release);
    nativePumpSampleRate_.store(0, std::memory_order_release);
    nativePumpBufferedFrames_.store(0, std::memory_order_release);
    userPauseIntent_ = false;
    seekFence_ = false;
    latestSeekEpoch_ = 0;
    seekFenceReleaseTick_ = 0;
    streamingProducerDrainPending_ = false;
    streamingProducerDrainConverter_ = nullptr;
    streamingEofCandidateConverter_.store(nullptr, std::memory_order_release);
    endOfStreamDrainPending_ = false;
    endOfStreamDrainConverter_ = nullptr;
    directSelectionArmEpoch_.store(0, std::memory_order_release);
    directSelectionArmGeneration_.store(0, std::memory_order_release);
    transportSkipArmEpoch_.store(0, std::memory_order_release);
    transportSkipArmGeneration_.store(0, std::memory_order_release);
    directSelectionSkipEpoch_.store(0, std::memory_order_release);
    directSelectionArmDeadlineTick_.store(0, std::memory_order_release);
    pendingLocalSuccessor_ = nullptr;
    pendingLocalRebindConverter_.store(nullptr, std::memory_order_release);
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wakeEvent_) return false;
    workerStop_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    workerThread_ = CreateThread(nullptr, 0, &AudioCoreRuntime::WorkerThunk, this, 0, nullptr);
    if (!workerThread_) {
        running_.store(false, std::memory_order_release);
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
        return false;
    }
    return true;
}

void AudioCoreRuntime::Stop() noexcept {
    workerStop_.store(true, std::memory_order_release);
    if (wakeEvent_) SetEvent(wakeEvent_);
    if (workerThread_) {
        WaitForSingleObject(workerThread_, INFINITE);
        CloseHandle(workerThread_);
        workerThread_ = nullptr;
    }
    std::lock_guard lock(controlMutex_);
    uiEnabled_.store(false, std::memory_order_release);
    RetireCaptureLocked(false);
    registry_.RetireAll();
    nativeClient_.store(nullptr, std::memory_order_release);
    nativeHandoffPending_.store(false, std::memory_order_release);
    nativeGraphHandoffPending_.store(false, std::memory_order_release);
    if (wakeEvent_) {
        CloseHandle(wakeEvent_);
        wakeEvent_ = nullptr;
    }
    running_.store(false, std::memory_order_release);
}

void AudioCoreRuntime::SetUiEnabled(bool enabled) noexcept {
    const bool wasEnabled = uiEnabled_.exchange(enabled, std::memory_order_acq_rel);
    if (enabled) {
        Wake();
        return;
    }

    // The UI boundary invalidates capture before the coordinator clears its
    // queue. The short wait is on the control/pipe thread; the Apple callback
    // itself never waits for this path.
    std::lock_guard lock(controlMutex_);
    // The Agent receives an initial broker snapshot after connecting.  That
    // snapshot can already be Off while the runtime is still at its default
    // state.  Do not emit a synthetic Disabled event for that no-op path:
    // publishing it would race the broker's persisted intent and create an
    // Agent <-> Broker Off feedback loop.
    const bool hasLiveState = wasEnabled || graphUnit_ || graphConverter_ || graphBound_ ||
        graphStarted_ || nativeInitialized_ || nativeStarted_ ||
        boundConverter_.load(std::memory_order_acquire) != nullptr ||
        coordinator_.Phase() != AudioCoreGatePhase::Off;
    if (!hasLiveState) return;
    RetireCaptureLocked(false);
    registry_.RetireAll();
    graphUnit_ = nullptr;
    graphOutputUnit_ = nullptr;
    graphConverter_ = nullptr;
    graphRate_ = 0;
    graphChannels_ = 0;
    graphSourceBits_ = 0;
    graphObservationId_ = 0;
    graphStarted_ = false;
    userPauseIntent_ = false;
    seekFence_ = false;
    streamingProducerDrainPending_ = false;
    streamingProducerDrainConverter_ = nullptr;
    streamingEofCandidateConverter_.store(nullptr, std::memory_order_release);
    endOfStreamDrainPending_ = false;
    endOfStreamDrainConverter_ = nullptr;
    directSelectionArmEpoch_.store(0, std::memory_order_release);
    directSelectionArmGeneration_.store(0, std::memory_order_release);
    transportSkipArmEpoch_.store(0, std::memory_order_release);
    transportSkipArmGeneration_.store(0, std::memory_order_release);
    directSelectionSkipEpoch_.store(0, std::memory_order_release);
    directSelectionArmDeadlineTick_.store(0, std::memory_order_release);
    pendingLocalSuccessor_ = nullptr;
    pendingLocalRebindConverter_.store(nullptr, std::memory_order_release);
    latestSeekEpoch_ = 0;
    seekFenceReleaseTick_ = 0;
    nativeInitialized_ = false;
    nativeStarted_ = false;
    nativeClient_.store(nullptr, std::memory_order_release);
    nativeHandoffPending_.store(false, std::memory_order_release);
    nativeGraphHandoffPending_.store(false, std::memory_order_release);
    nextTelemetryTick_ = 0;
    Emit(AudioCoreRuntimeEvent::Disabled, S_OK);
}

void AudioCoreRuntime::OnTransportPlayPause() noexcept {
    if (!UiEnabled()) return;
    std::lock_guard lock(controlMutex_);
    if (coordinator_.Phase() == AudioCoreGatePhase::Active &&
        coordinator_.SinkState() == WasapiSinkState::Running) {
        userPauseIntent_ = true;
        coordinator_.PauseEndpoint();
        return;
    }

    userPauseIntent_ = false;
    TryResumePausedSinkLocked();
    Wake();
}

void AudioCoreRuntime::OnTransportSelectionArm(std::uint64_t epoch) noexcept {
    if (!UiEnabled() || epoch == 0) return;
    directSelectionSkipEpoch_.store(0, std::memory_order_release);
    directSelectionArmGeneration_.store(
        boundMediaGeneration_.load(std::memory_order_acquire), std::memory_order_release);
    directSelectionArmEpoch_.store(epoch, std::memory_order_release);
    directSelectionArmDeadlineTick_.store(GetTickCount64() + 750, std::memory_order_release);
}

void AudioCoreRuntime::OnTransportSkipArm(std::uint64_t epoch) noexcept {
    if (!UiEnabled() || epoch == 0) return;
    transportSkipArmGeneration_.store(
        boundMediaGeneration_.load(std::memory_order_acquire), std::memory_order_release);
    transportSkipArmEpoch_.store(epoch, std::memory_order_release);
}

void AudioCoreRuntime::OnTransportSkip(std::uint64_t epoch) noexcept {
    if (!UiEnabled()) return;
    streamingEofCandidateConverter_.store(nullptr, std::memory_order_release);
    const bool directSelection =
        epoch != 0 && directSelectionArmEpoch_.load(std::memory_order_acquire) == epoch;
    const bool transportSkip =
        epoch != 0 && transportSkipArmEpoch_.load(std::memory_order_acquire) == epoch;
    if (directSelection) {
        directSelectionSkipEpoch_.store(epoch, std::memory_order_release);
    }
    std::lock_guard lock(controlMutex_);
    if (transportSkip) {
        const auto armedGeneration =
            transportSkipArmGeneration_.load(std::memory_order_acquire);
        transportSkipArmEpoch_.store(0, std::memory_order_release);
        transportSkipArmGeneration_.store(0, std::memory_order_release);
        const auto currentGeneration = boundMediaGeneration_.load(std::memory_order_acquire);
        if (armedGeneration == 0 || currentGeneration != armedGeneration) {
            TryActivateLocked();
            Wake();
            return;
        }
    }
    if (directSelection) {
        // A double-click selects and starts the target item even when the old
        // item was paused. Pause intent is generation-scoped; carrying the old
        // true value into the new graph leaves the target permanently Bound
        // while Apple's own timeline is already advancing.
        userPauseIntent_ = false;
        // A target graph can bind before the confirming double-click crosses
        // UI -> Broker -> Agent. The skip belongs to the generation that was
        // active on the first press, never to a newly bound target generation.
        const auto armedGeneration =
            directSelectionArmGeneration_.load(std::memory_order_acquire);
        const auto currentGeneration = boundMediaGeneration_.load(std::memory_order_acquire);
        if (armedGeneration == 0 || currentGeneration != armedGeneration) {
            TryActivateLocked();
            Wake();
            return;
        }
    }
    // Previous/Next, or a direct-selection confirmation that still owns the
    // armed generation, is an explicit retirement rather than natural EOS.
    RetireCaptureLocked(true);
    seekFence_ = true;
    seekFenceReleaseTick_ = GetTickCount64() + 500;
    Wake();
}

void AudioCoreRuntime::OnTransportSeekBegin(std::uint64_t epoch) noexcept {
    if (!UiEnabled() || epoch == 0) return;
    streamingEofCandidateConverter_.store(nullptr, std::memory_order_release);
    std::lock_guard lock(controlMutex_);
    if (epoch < latestSeekEpoch_) return;
    latestSeekEpoch_ = epoch;
    seekFence_ = true;
    seekFenceReleaseTick_ = 0;
    // Silence the currently audible generation as soon as the user grabs the
    // scrubber. Apple may rebuild several transient graphs while the pointer
    // moves; those graphs may capture P0, but they are not allowed to own the
    // endpoint until the matching commit settles.
    RetireCaptureLocked(true);
    Wake();
}

void AudioCoreRuntime::OnTransportSeekCommit(std::uint64_t epoch) noexcept {
    if (!UiEnabled() || epoch == 0) return;
    std::lock_guard lock(controlMutex_);
    if (epoch != latestSeekEpoch_) return;
    // Drop any transient generation accumulated during the drag, then require
    // a genuinely quiet control-plane window before endpoint activation. Any
    // subsequent Apple graph/client mutation extends this deadline, and a new
    // seek epoch cancels it entirely until that later epoch commits.
    RetireCaptureLocked(true);
    TryBindGraphLocked();
    seekFence_ = true;
    seekFenceReleaseTick_ = GetTickCount64() + 500;
    Wake();
}

void AudioCoreRuntime::OnConverterCreated(void* converter,
                                           const ApplePcmFormatView& source,
                                           const ApplePcmFormatView& destination,
                                           std::uint64_t observationId) noexcept {
    if (!UiEnabled() || !converter || destination.formatId != kAppleLinearPcm ||
        (destination.formatFlags & kAppleFormatFlagIsFloat) == 0 ||
        destination.bitsPerChannel != 32 || destination.channelsPerFrame != 2 ||
        destination.framesPerPacket != 1 || destination.sampleRate < 8000 ||
        destination.sampleRate > 768000 || !IsCandidateEncodedFormat(source.formatId)) {
        return;
    }

    const auto sourceBits = (source.bitsPerChannel == 16 ||
                             source.bitsPerChannel == 24 || source.bitsPerChannel == 32)
        ? source.bitsPerChannel : 0u;
    const bool localPcm = source.formatId == kAppleLinearPcm;
    // Local PCM is captured before Apple's local-file SRC. Register local
    // signed int32 long enough for the worker to reject it explicitly and
    // surface the dedicated playback error; it must never reach the float
    // canonical mapping path.
    constexpr std::uint32_t kAppleFormatFlagIsSignedInteger = 1u << 2;
    const bool localFloat32 = localPcm &&
        (source.formatFlags & kAppleFormatFlagIsFloat) != 0 &&
        source.bitsPerChannel == 32 && source.bytesPerFrame == 8;
    const bool localExactInteger = localPcm &&
        (source.formatFlags & kAppleFormatFlagIsFloat) == 0 &&
        (source.formatFlags & kAppleFormatFlagIsSignedInteger) != 0 &&
        sourceBits != 0 && sourceBits <= 32;
    if (localPcm && ((!localFloat32 && !localExactInteger) ||
                     source.sampleRate < 8000 || source.sampleRate > 768000 ||
                     source.channelsPerFrame != 2 || source.framesPerPacket != 1 ||
                     source.bytesPerFrame == 0 ||
                     (source.formatFlags & kAppleFormatFlagIsNonInterleaved) != 0)) {
        return;
    }
    const auto endpointBits = localFloat32 || sourceBits == 32
        ? std::uint16_t{32} : std::uint16_t{24};
    const auto captureRate = localPcm ? source.sampleRate : destination.sampleRate;
    const auto captureChannels = localPcm ? source.channelsPerFrame : destination.channelsPerFrame;
    const PcmFormat outputFormat{
        captureRate, static_cast<std::uint16_t>(captureChannels),
        endpointBits, 32, PcmEncoding::SignedInteger, true,
        static_cast<std::uint16_t>(sourceBits)};
    ConverterRegistration registration{};
    registration.converter = converter;
    registration.outputFormat = outputFormat;
    registration.sourceBitDepth = sourceBits;
    registration.encodedFormat = source.formatId;
    registration.observationId = observationId;
    registration.mediaGeneration = 1;
    const auto& capturedAppleFormat = localPcm ? source : destination;
    registration.appleFormatFlags = capturedAppleFormat.formatFlags;
    registration.appleBytesPerPacket = capturedAppleFormat.bytesPerPacket;
    registration.appleFramesPerPacket = capturedAppleFormat.framesPerPacket;
    registration.appleBytesPerFrame = capturedAppleFormat.bytesPerFrame;
    registration.appleBitsPerChannel = capturedAppleFormat.bitsPerChannel;
    registration.appleChannelsPerFrame = capturedAppleFormat.channelsPerFrame;
    registry_.Register(registration);
    const auto pendingRate = pendingCompressedRate_.load(std::memory_order_acquire);
    const auto pendingChannels = pendingCompressedChannels_.load(std::memory_order_acquire);
    const auto pendingBits = pendingCompressedBits_.load(std::memory_order_acquire);
    if (source.formatId == kAlac || source.formatId == kQlac) {
        if (pendingRate == destination.sampleRate && pendingChannels == destination.channelsPerFrame &&
            IsSupportedSourceDepth(static_cast<std::uint16_t>(pendingBits))) {
            registry_.UpdateSourcePrecision(converter, pendingBits, pendingRate, pendingChannels);
        }
    }
}

bool AudioCoreRuntime::IsConverterRegistered(void* converter) const noexcept {
    ConverterRegistrationSnapshot snapshot{};
    return registry_.Snapshot(converter, snapshot);
}

bool AudioCoreRuntime::IsP0Converter(void* converter) const noexcept {
    ConverterRegistrationSnapshot snapshot{};
    if (!converter || !registry_.Snapshot(converter, snapshot)) return false;
    return snapshot.registration.encodedFormat == kAlac ||
           snapshot.registration.encodedFormat == kQlac;
}

bool AudioCoreRuntime::IsConverterBound(void* converter) const noexcept {
    ConverterRegistrationSnapshot snapshot{};
    return converter && boundConverter_.load(std::memory_order_acquire) == converter &&
           registry_.Snapshot(converter, snapshot) && snapshot.bound &&
           snapshot.registration.mediaGeneration == coordinator_.MediaGeneration() &&
           coordinator_.IsEnabled();
}

void AudioCoreRuntime::OnConverterSourcePrecision(void* converter,
                                                  std::uint32_t sourceBitDepth,
                                                  std::uint32_t sampleRate,
                                                  std::uint32_t channels) noexcept {
    if (!UiEnabled()) return;
    pendingCompressedRate_.store(sampleRate, std::memory_order_release);
    pendingCompressedChannels_.store(channels, std::memory_order_release);
    pendingCompressedBits_.store(sourceBitDepth, std::memory_order_release);
    void* matchedConverter = nullptr;
    const bool direct = registry_.UpdateSourcePrecision(
        converter, sourceBitDepth, sampleRate, channels);
    if (!direct) {
        registry_.UpdateCompressedPrecision(sourceBitDepth, sampleRate, channels, 0,
                                             matchedConverter);
    } else {
        matchedConverter = converter;
    }
    std::lock_guard lock(controlMutex_);
    if (!graphConverter_ && graphUnit_ && graphRate_ == sampleRate &&
        graphChannels_ == channels) {
        void* candidate = matchedConverter;
        if (!candidate && registry_.FindCandidate(sampleRate, channels, sourceBitDepth,
                                                  graphObservationId_, candidate)) {
            graphConverter_ = candidate;
        } else if (candidate) {
            graphConverter_ = candidate;
        }
    }
    TryBindGraphLocked();
    Wake();
}

void AudioCoreRuntime::OnConverterReset(void* converter, bool forceTimelineRetire) noexcept {
    if (!UiEnabled()) return;
    registry_.Reset(converter);
    std::lock_guard lock(controlMutex_);
    const bool bound = boundConverter_.load(std::memory_order_acquire) == converter;
    if (forceTimelineRetire) {
        // Ordinary local-file seeking has a version-locked Reset fingerprint.
        // Unlike the streaming natural-repeat Reset, its eager-decoded staging
        // and already-captured ACv2 frames belong to the old media position and
        // must not survive into the new timeline.
        if (bound) RetireCaptureLocked(true);
        if (graphConverter_ == converter) TryBindGraphLocked();
        Wake();
        return;
    }
    if (bound) {
        // A non-seek Reset is not, by itself, permission to discard captured
        // media. Apple issues ordinary Reset during natural repeat/gapless and
        // local EOF handoff, and its control flags can already be changing by
        // the time the Reset callback reaches us. Retiring based on that
        // transient graph state can drop a valid short terminal tail before
        // converter Dispose has a chance to seal/drain it. Preserve the bound
        // generation here; an explicit seek uses forceTimelineRetire above,
        // while subsequent Dispose/Stop lifecycle decides EOS vs retirement.
    }
    Wake();
}

bool AudioCoreRuntime::BeginEndOfStreamDrainLocked(void* converter,
                                                   bool requireShortTail) noexcept {
    if (!converter || endOfStreamDrainPending_ || !graphBound_ ||
        boundConverter_.load(std::memory_order_acquire) != converter ||
        coordinator_.Phase() != AudioCoreGatePhase::Active) {
        return false;
    }
    if (requireShortTail) {
        const auto available = coordinator_.AvailableFrames();
        const auto period = requiredPrebufferFrames_ != 0
            ? requiredPrebufferFrames_ : PeriodFrames(graphRate_);
        if (period == 0 || available == 0 || available > period) return false;
    }
    if (!coordinator_.SealEndOfStream()) return false;
    endOfStreamDrainPending_ = true;
    endOfStreamDrainConverter_ = converter;
    return true;
}

bool AudioCoreRuntime::OnLocalEndOfStream(void* converter) noexcept {
    if (!UiEnabled() || !converter) return false;

    // Direct playlist activation is a double-press gesture. The first press
    // arms a short control-plane epoch; Apple can dispose the local converter
    // only ~3 ms after the second press, faster than UI->Broker->Agent IPC can
    // deliver the confirming skip. If an armed epoch is still live, allow a
    // bounded confirmation window before classifying Dispose as natural EOS.
    const auto armedEpoch = directSelectionArmEpoch_.load(std::memory_order_acquire);
    const auto armedDeadline = directSelectionArmDeadlineTick_.load(std::memory_order_acquire);
    const bool selectionArmLive = armedEpoch != 0 && GetTickCount64() <= armedDeadline;
    if (selectionArmLive) {
        const auto waitDeadline = std::min<ULONGLONG>(armedDeadline, GetTickCount64() + 120);
        while (GetTickCount64() <= waitDeadline &&
               directSelectionSkipEpoch_.load(std::memory_order_acquire) != armedEpoch) {
            Sleep(2);
        }
        if (directSelectionSkipEpoch_.load(std::memory_order_acquire) == armedEpoch) {
            return false;
        }
    } else if (armedEpoch != 0) {
        // A completed direct-selection gesture must not poison later natural
        // EOF classification. Expired arm/skip epochs are cleared before any
        // later converter retirement is classified.
        std::uint64_t expected = armedEpoch;
        if (directSelectionArmEpoch_.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
            directSelectionArmGeneration_.store(0, std::memory_order_release);
            directSelectionSkipEpoch_.store(0, std::memory_order_release);
            directSelectionArmDeadlineTick_.store(0, std::memory_order_release);
        }
    }

    std::lock_guard lock(controlMutex_);
    if (selectionArmLive &&
        directSelectionSkipEpoch_.load(std::memory_order_acquire) == armedEpoch) {
        return false;
    }
    // A non-seek Reset may have already sealed this exact local generation
    // before AudioConverterDispose enters. Treat that state as success so the
    // Dispose hook knows to queue any prefetched successor behind the terminal
    // drain instead of attempting an immediate rebind and losing the successor.
    if (endOfStreamDrainPending_ && endOfStreamDrainConverter_ == converter) {
        return true;
    }
    ConverterRegistrationSnapshot snapshot{};
    if (!registry_.Snapshot(converter, snapshot) ||
        snapshot.registration.encodedFormat != kAppleLinearPcm) {
        return false;
    }
    // If every source frame has already been submitted on an exact endpoint
    // period boundary, there is no terminal buffer left to carry an EOS bit.
    // Sealing an empty queue would set endOfStreamDrainPending_ forever because
    // the sink correctly refuses to acquire another (pure-silence) period.
    // Let the normal converter-dispose retirement close this generation and
    // make room for the successor instead. Only a non-empty queued remainder
    // needs the terminal drain/padding path.
    if (coordinator_.AvailableFrames() == 0) return false;
    const bool sealed = BeginEndOfStreamDrainLocked(converter, false);
    if (sealed) Wake();
    return sealed;
}

void AudioCoreRuntime::SetPendingLocalSuccessor(void* converter) noexcept {
    if (!UiEnabled() || !converter) return;
    std::lock_guard lock(controlMutex_);
    pendingLocalSuccessor_ = converter;
    Wake();
}

void AudioCoreRuntime::RequestLocalSuccessorBind(void* converter) noexcept {
    if (!UiEnabled() || !converter) return;
    if (boundConverter_.load(std::memory_order_acquire) != nullptr) return;
    pendingLocalRebindConverter_.store(converter, std::memory_order_release);
    Wake();
}

void AudioCoreRuntime::OnConverterDisposed(void* converter) noexcept {
    if (!UiEnabled()) return;
    const bool wasBound = boundConverter_.load(std::memory_order_acquire) == converter;
    registry_.Retire(converter);
    if (!wasBound) return;
    std::lock_guard lock(controlMutex_);
    if (boundConverter_.load(std::memory_order_acquire) == converter) {
        if (endOfStreamDrainPending_ && endOfStreamDrainConverter_ == converter) {
            // The producer has already sealed the queue. Keep the sink and its
            // mapped terminal tail alive until ReleaseBuffer confirms that the
            // terminal device period reached the endpoint.
            if (graphConverter_ == converter) graphConverter_ = nullptr;
        } else {
            RetireCaptureLocked(true);
            graphConverter_ = nullptr;
        }
    }
    Wake();
}

bool AudioCoreRuntime::TryBindLocalSuccessor(void* converter) noexcept {
    if (!UiEnabled() || !converter) return false;
    std::lock_guard lock(controlMutex_);
    ConverterRegistrationSnapshot snapshot{};
    if (!registry_.Snapshot(converter, snapshot) ||
        snapshot.registration.encodedFormat != kAppleLinearPcm ||
        !graphUnit_ || !graphStarted_ || graphBound_) {
        return false;
    }
    graphConverter_ = converter;
    graphRate_ = snapshot.registration.outputFormat.sampleRate;
    graphChannels_ = snapshot.registration.outputFormat.channels;
    graphSourceBits_ = snapshot.registration.sourceBitDepth;
    graphObservationId_ = snapshot.registration.observationId;
    TryBindGraphLocked();
    const bool bound = graphBound_ &&
        boundConverter_.load(std::memory_order_acquire) == converter;
    if (bound) Wake();
    return bound;
}

void AudioCoreRuntime::OnAudioUnitInitialized(void* unit, void* candidateConverter,
                                               std::uint32_t sampleRate,
                                               std::uint32_t channels,
                                               std::uint32_t sourceBitDepth,
                                               std::uint64_t observationId) noexcept {
    if (!UiEnabled() || !unit) return;
    std::lock_guard lock(controlMutex_);
    ExtendSeekQuietPeriodLocked();
    if (!candidateConverter && sampleRate != 0 && channels != 0 && sourceBitDepth != 0) {
        // AudioUnitInitialize commonly runs on a render/control thread
        // different from the converter setup thread. Resolve the converter by
        // the cross-thread format observation rather than assuming TLS.
        registry_.FindCandidate(sampleRate, channels, sourceBitDepth, observationId,
                                candidateConverter);
    }
    if (!candidateConverter && sampleRate != 0 && channels != 0 && sourceBitDepth == 0) {
        // Some Apple output units expose only the LPCM tuple.  Once the
        // decoder cookie has supplied precision, resolve the compressed P0
        // registration by rate/channel and carry its source depth forward.
        registry_.FindCandidateAnyPrecision(sampleRate, channels, observationId,
                                             candidateConverter, sourceBitDepth);
    }
    const bool graphIdentityChanged =
        graphUnit_ != unit || graphConverter_ != candidateConverter;
    if (graphIdentityChanged && !endOfStreamDrainPending_ &&
        !streamingProducerDrainPending_) {
        if (graphBound_) RetireCaptureLocked(true);
        // The parent/output AudioUnit is generation-scoped. Forget the old
        // pointer at the graph-identity boundary so a late Stop from the prior
        // graph cannot retire the successor. A Start belonging to the new graph
        // will repopulate graphOutputUnit_.
        graphOutputUnit_ = nullptr;
    }
    graphUnit_ = unit;
    graphConverter_ = candidateConverter;
    graphRate_ = sampleRate;
    graphChannels_ = channels;
    graphSourceBits_ = sourceBitDepth;
    graphObservationId_ = observationId;
    graphStarted_ = false;
    TryBindGraphLocked();
    Wake();
}

void AudioCoreRuntime::OnAudioUnitStarted(void* unit) noexcept {
    if (!UiEnabled()) return;
    std::lock_guard lock(controlMutex_);
    ExtendSeekQuietPeriodLocked();
    graphPaused_ = false;
    graphControlPending_ = false;
    coordinator_.SetSinkResumeBlocked(false);
    if (graphUnit_ == unit) {
        graphStarted_ = true;
        TryBindGraphLocked();
    } else if (graphBound_ && graphUnit_ && unit) {
        // Apple starts the hardware-output parent AudioUnit (for example
        // auou/7820) after initializing the child graph AudioUnit that owns
        // the P0 converter (7823). The child identity is already fixed by
        // the stream-format commit and bound converter, so this first parent
        // start is the graph-start edge for activation purposes.
        graphStarted_ = true;
        graphOutputUnit_ = unit;
    }
    if (!userPauseIntent_) TryResumePausedSinkLocked();
    Wake();
}

void AudioCoreRuntime::OnAudioUnitStopped(void* unit) noexcept {
    if (!UiEnabled()) return;
    if (nativeGraphHandoffPending_.load(std::memory_order_acquire)) return;
    std::lock_guard lock(controlMutex_);
    ExtendSeekQuietPeriodLocked();
    if (graphControlPending_ || graphPaused_ || userPauseIntent_) {
        // A graph stop issued by the runtime worker is an intentional media
        // timeline pause for a P0/network wait.  It must not retire the v2
        // generation the way a user/Apple lifecycle stop does.
        if (graphUnit_ == unit) graphStarted_ = false;
        return;
    }

    const auto converter = boundConverter_.load(std::memory_order_acquire);
    if (!endOfStreamDrainPending_ && !streamingProducerDrainPending_ &&
        !seekFence_ && converter && IsP0Converter(converter)) {
        // Native-client identity can already point at a prefetched successor
        // by the time the old output AudioUnit stops. Do not guess EOS from
        // the current queue depth. Preserve the old streaming generation until
        // HookDll flushes the still-live P0 converter to producedPackets==0.
        streamingProducerDrainPending_ = true;
        streamingProducerDrainConverter_ = converter;
    } else if (!endOfStreamDrainPending_ && !streamingProducerDrainPending_ &&
               !seekFence_) {
        // Local EOF normally seals from Reset/Dispose. Keep the historical
        // short-tail fallback only for non-streaming generations.
        (void)BeginEndOfStreamDrainLocked(converter, true);
    }

    if (graphOutputUnit_ == unit) {
        graphOutputUnit_ = nullptr;
        graphStarted_ = false;
        if (!endOfStreamDrainPending_ && !streamingProducerDrainPending_) {
            RetireCaptureLocked(true);
        }
    } else if (graphUnit_ == unit) {
        graphStarted_ = false;
        if (!endOfStreamDrainPending_ && !streamingProducerDrainPending_) {
            RetireCaptureLocked(true);
        }
    }
    Wake();
}

void AudioCoreRuntime::OnAudioUnitUninitialized(void* unit) noexcept {
    if (!UiEnabled()) return;
    std::lock_guard lock(controlMutex_);
    ExtendSeekQuietPeriodLocked();
    if (graphUnit_ != unit) return;
    graphPaused_ = false;
    graphControlPending_ = false;
    if (!endOfStreamDrainPending_ && !streamingProducerDrainPending_) {
        RetireCaptureLocked(false);
    }
    graphUnit_ = nullptr;
    graphOutputUnit_ = nullptr;
    graphConverter_ = nullptr;
    graphRate_ = 0;
    graphChannels_ = 0;
    graphSourceBits_ = 0;
    graphObservationId_ = 0;
    graphStarted_ = false;
    Wake();
}

void AudioCoreRuntime::OnNativeClientInitialized(void* client, const wchar_t* endpoint,
                                                  AUDCLNT_SHAREMODE, DWORD,
                                                  const WAVEFORMATEX*, HRESULT result) noexcept {
    if (!UiEnabled() || FAILED(result) || !client) return;
    std::lock_guard lock(controlMutex_);
    ExtendSeekQuietPeriodLocked();
    nativeClient_.store(client, std::memory_order_release);
    nativeInitialized_ = true;
    nativeStarted_ = false;
    nativeEndpoint_ = endpoint ? endpoint : L"";
    Wake();
}

void AudioCoreRuntime::OnNativeClientStarted(void* client, HRESULT result) noexcept {
    if (!UiEnabled() || FAILED(result) || !client) return;
    std::lock_guard lock(controlMutex_);
    ExtendSeekQuietPeriodLocked();
    nativeClient_.store(client, std::memory_order_release);
    nativeInitialized_ = true;
    nativeStarted_ = true;
    TryBindGraphLocked();
    if (!userPauseIntent_) TryResumePausedSinkLocked();
    Wake();
}

bool AudioCoreRuntime::BeforeNativeClientStop(void* client) noexcept {
    if (!UiEnabled() || !client) return false;
    if (nativeHandoffPending_.load(std::memory_order_acquire)) return false;

    void* converter{};
    {
        std::lock_guard lock(controlMutex_);
        if (nativeClient_.load(std::memory_order_acquire) != client) return false;
        if (graphControlPending_ || graphPaused_ || userPauseIntent_ || seekFence_ ||
            endOfStreamDrainPending_ || coordinator_.Phase() != AudioCoreGatePhase::Active) {
            return false;
        }
        converter = boundConverter_.load(std::memory_order_acquire);
        if (!converter || !IsP0Converter(converter)) return false;
        if (streamingEofCandidateConverter_.load(std::memory_order_acquire) == converter) {
            return true;
        }
    }

    // Keep the existing software native scheduler alive while waiting. This
    // does not call AudioConverterFillComplexBuffer directly and therefore
    // preserves Apple's queue-head advance/release transaction. The window is
    // intentionally bounded to the same order as the normal 250-400 ms P0
    // reservoir; in the observed failing run the missing producer tail was
    // 12,800 frames / 133.3 ms at 96 kHz.
    constexpr ULONGLONG kProducerEofWaitMs = 500;
    const auto deadline = GetTickCount64() + kProducerEofWaitMs;
    while (GetTickCount64() < deadline) {
        if (!UiEnabled() ||
            boundConverter_.load(std::memory_order_acquire) != converter) {
            return false;
        }
        if (streamingEofCandidateConverter_.load(std::memory_order_acquire) == converter) {
            return true;
        }
        Sleep(2);
    }
    return streamingEofCandidateConverter_.load(std::memory_order_acquire) == converter;
}

void* AudioCoreRuntime::OnNativeClientStopped(void* client, HRESULT) noexcept {
    if (!UiEnabled()) return nullptr;
    if (nativeHandoffPending_.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard lock(controlMutex_);
    ExtendSeekQuietPeriodLocked();
    if (nativeClient_.load(std::memory_order_acquire) != client) return nullptr;
    nativeStarted_ = false;
    if (graphControlPending_ || graphPaused_ || userPauseIntent_) {
        // Stopping the Apple AudioUnit can synchronously stop its native
        // client as well. Keep the v2 generation alive until the matching
        // graph resume or a real lifecycle retirement arrives.
        return nullptr;
    }
    if (endOfStreamDrainPending_) return nullptr;

    const auto converter = boundConverter_.load(std::memory_order_acquire);
    if (!seekFence_ && converter && IsP0Converter(converter) &&
        coordinator_.Phase() == AudioCoreGatePhase::Active) {
        // A successful zero-packet P0 Fill is only an EOF candidate. Repeat One
        // can resume the *same* converter immediately afterwards, in which case
        // sealing here earlier would reject the first ~1 s of the next loop.
        // Native Stop is the second half of the natural-EOF handshake: if the
        // candidate is still armed now, no loop PCM resumed, so all real media
        // has already reached the P0 queue and it is finally safe to seal.
        void* expected = converter;
        if (streamingEofCandidateConverter_.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            streamingProducerDrainPending_ = false;
            streamingProducerDrainConverter_ = nullptr;
            if (coordinator_.AvailableFrames() == 0) {
                RetireCaptureLocked(true);
                Wake();
                return nullptr;
            }
            if (BeginEndOfStreamDrainLocked(converter, false)) {
                Wake();
                return nullptr;
            }
            RetireCaptureLocked(true);
            Wake();
            return nullptr;
        }

        // Stop without a preceding zero-packet Fill is only a scheduler edge.
        // Preserve the generation across AudioUnit teardown; if a late producer
        // EOF arrives, OnStreamingProducerEndOfStream will seal at that point.
        streamingProducerDrainPending_ = true;
        streamingProducerDrainConverter_ = converter;
        Wake();
        return converter;
    }

    // Non-streaming/local generations retain the bounded fallback. Explicit
    // seek/skip/direct-selection paths are already fenced/retired.
    if (!seekFence_ && BeginEndOfStreamDrainLocked(converter, true)) {
        Wake();
        return nullptr;
    }
    RetireCaptureLocked(true);
    Wake();
    return nullptr;
}

bool AudioCoreRuntime::OnStreamingProducerEndOfStream(void* converter) noexcept {
    if (!UiEnabled() || !converter) return false;
    std::lock_guard lock(controlMutex_);
    if (endOfStreamDrainPending_ && endOfStreamDrainConverter_ == converter) {
        streamingProducerDrainPending_ = false;
        streamingProducerDrainConverter_ = nullptr;
        streamingEofCandidateConverter_.store(nullptr, std::memory_order_release);
        return true;
    }
    const bool wasStopPending = streamingProducerDrainPending_ &&
        streamingProducerDrainConverter_ == converter;
    if ((streamingProducerDrainPending_ && !wasStopPending) ||
        boundConverter_.load(std::memory_order_acquire) != converter ||
        !IsP0Converter(converter) || seekFence_ || userPauseIntent_ ||
        graphControlPending_ || graphPaused_ ||
        coordinator_.Phase() != AudioCoreGatePhase::Active) {
        return false;
    }

    // Zero packets alone are not enough to distinguish a natural track end
    // from Repeat One. If the native scheduler already stopped, this is a late
    // producer EOF and can be sealed immediately. Otherwise arm a lock-free
    // candidate and keep the queue open. A subsequent nonzero P0 callback on
    // the same converter cancels the candidate, resets its media-frame cursor
    // to zero and appends the next loop directly after the old tail.
    if (wasStopPending) {
        streamingProducerDrainPending_ = false;
        streamingProducerDrainConverter_ = nullptr;
        streamingEofCandidateConverter_.store(nullptr, std::memory_order_release);
        if (coordinator_.AvailableFrames() == 0) {
            RetireCaptureLocked(true);
            Wake();
            return true;
        }
        const bool sealed = BeginEndOfStreamDrainLocked(converter, false);
        Wake();
        return sealed;
    }

    streamingEofCandidateConverter_.store(converter, std::memory_order_release);
    Wake();
    return true;
}

void AudioCoreRuntime::OnNativeClientReset(void* client, HRESULT) noexcept {
    if (!UiEnabled()) return;
    if (nativeHandoffPending_.load(std::memory_order_acquire)) return;
    std::lock_guard lock(controlMutex_);
    if (nativeClient_.load(std::memory_order_acquire) != client) return;
    ExtendSeekQuietPeriodLocked();
    Wake();
}

void AudioCoreRuntime::OnPcmFillResult(std::int32_t result,
                                       std::uint32_t producedPackets,
                                       bool outputDataPresent,
                                       bool outputPacketsPresent) noexcept {
    if (!callbacks_.deepDiagnostics) return;
    rawFillCalls_.fetch_add(1, std::memory_order_relaxed);
    lastFillResult_.store(result, std::memory_order_relaxed);
    lastProducedPackets_.store(producedPackets, std::memory_order_relaxed);
    producedPackets_.fetch_add(producedPackets, std::memory_order_relaxed);
    if (result != 0) {
        fillErrors_.fetch_add(1, std::memory_order_relaxed);
    } else if (producedPackets != 0) {
        fillSuccessWithData_.fetch_add(1, std::memory_order_relaxed);
    } else {
        fillSuccessZeroPackets_.fetch_add(1, std::memory_order_relaxed);
    }

    if (!UiEnabled()) rejectUiOff_.fetch_add(1, std::memory_order_relaxed);
    if (result == 0) {
        if (!outputDataPresent) {
            rejectNoOutputData_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!outputPacketsPresent) {
            rejectNoOutputPackets_.fetch_add(1, std::memory_order_relaxed);
        }
        if (producedPackets == 0) {
            rejectZeroPackets_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void AudioCoreRuntime::RecordPcmReject(AudioCorePcmRejectReason reason) noexcept {
    if (!callbacks_.deepDiagnostics) return;
    switch (reason) {
    case AudioCorePcmRejectReason::NoDestinationFormat:
        rejectNoDestinationFormat_.fetch_add(1, std::memory_order_relaxed);
        break;
    case AudioCorePcmRejectReason::InvalidBufferList:
        rejectInvalidBufferList_.fetch_add(1, std::memory_order_relaxed);
        break;
    case AudioCorePcmRejectReason::RegistryMissing:
        rejectRegistryMissing_.fetch_add(1, std::memory_order_relaxed);
        break;
    case AudioCorePcmRejectReason::RegistryUnbound:
        rejectRegistryUnbound_.fetch_add(1, std::memory_order_relaxed);
        break;
    case AudioCorePcmRejectReason::RateMismatch:
        rejectRateMismatch_.fetch_add(1, std::memory_order_relaxed);
        break;
    case AudioCorePcmRejectReason::ChannelMismatch:
        rejectChannelMismatch_.fetch_add(1, std::memory_order_relaxed);
        break;
    case AudioCorePcmRejectReason::Parse:
        rejectParse_.fetch_add(1, std::memory_order_relaxed);
        break;
    case AudioCorePcmRejectReason::ClaimFrames:
        rejectClaimFrames_.fetch_add(1, std::memory_order_relaxed);
        break;
    case AudioCorePcmRejectReason::Tap:
        rejectTap_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

bool AudioCoreRuntime::OnPcmOutput(void* converter, const ApplePcmFormatView& format,
                                   const ApplePcmBufferListView& list,
                                   std::uint32_t producedPackets) noexcept {
    if (!UiEnabled()) return false;
    callbackDepth_.fetch_add(1, std::memory_order_acq_rel);
    bool accepted = false;
    if (UiEnabled()) {
        // Repeat One can reuse the exact streaming P0 converter after a
        // successful zero-packet Fill. If PCM resumes before native Stop, the
        // zero-packet event was a loop boundary rather than a natural EOF.
        // Cancel the candidate and restart only the diagnostic/source-frame
        // cursor; the current unsealed coordinator queue deliberately remains
        // intact so the new loop is appended sample-contiguously after the old
        // media tail.
        void* expectedEofConverter = converter;
        if (streamingEofCandidateConverter_.compare_exchange_strong(
                expectedEofConverter, nullptr, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            registry_.Reset(converter);
        }

        ConverterRegistrationSnapshot snapshot{};
        if (!registry_.Snapshot(converter, snapshot)) {
            RecordPcmReject(AudioCorePcmRejectReason::RegistryMissing);
        } else if (!snapshot.bound ||
                   boundConverter_.load(std::memory_order_acquire) != converter ||
                   snapshot.registration.mediaGeneration !=
                       boundMediaGeneration_.load(std::memory_order_acquire)) {
            RecordPcmReject(AudioCorePcmRejectReason::RegistryUnbound);
        } else {
            const bool rateMatches =
                snapshot.registration.outputFormat.sampleRate == format.sampleRate;
            const bool channelMatches =
                snapshot.registration.outputFormat.channels == format.channelsPerFrame;
            if (!rateMatches) RecordPcmReject(AudioCorePcmRejectReason::RateMismatch);
            if (!channelMatches) RecordPcmReject(AudioCorePcmRejectReason::ChannelMismatch);
            if (!rateMatches || !channelMatches) {
                callbackDepth_.fetch_sub(1, std::memory_order_acq_rel);
                return false;
            }
            ApplePcmOutputView output{};
            if (ParseApplePcmOutput(format, list, producedPackets, output)) {
                std::uint64_t firstFrame{};
                bool discontinuity{};
                if (registry_.ClaimFrames(converter, output.frames, firstFrame, discontinuity)) {
                    if (output.layout == ApplePcmLayout::InterleavedFloat32) {
                        accepted = tap_.OnInterleaved(converter, firstFrame,
                                                      output.interleaved, output.frames,
                                                      discontinuity, false);
                    } else if (output.layout == ApplePcmLayout::PlanarFloat32) {
                        accepted = tap_.OnPlanar(converter, firstFrame, output.left,
                                                 output.right, output.frames,
                                                 discontinuity, false);
                    }
                    if (accepted) {
                        if (callbacks_.deepDiagnostics) {
                            acceptedCalls_.fetch_add(1, std::memory_order_relaxed);
                            acceptedFrames_.fetch_add(output.frames, std::memory_order_relaxed);
                        }
                        Wake();
                    } else {
                        RecordPcmReject(AudioCorePcmRejectReason::Tap);
                    }
                } else {
                    RecordPcmReject(AudioCorePcmRejectReason::ClaimFrames);
                }
            } else {
                RecordPcmReject(AudioCorePcmRejectReason::Parse);
            }
        }
    }
    callbackDepth_.fetch_sub(1, std::memory_order_acq_rel);
    return accepted;
}

bool AudioCoreRuntime::DestinationFormat(void* converter,
                                         ApplePcmFormatView& format) const noexcept {
    ConverterRegistrationSnapshot snapshot{};
    if (!registry_.Snapshot(converter, snapshot)) return false;
    const auto& registration = snapshot.registration;
    format.formatId = kAppleLinearPcm;
    format.formatFlags = registration.appleFormatFlags;
    format.bytesPerPacket = registration.appleBytesPerPacket;
    format.framesPerPacket = registration.appleFramesPerPacket;
    format.bytesPerFrame = registration.appleBytesPerFrame;
    format.channelsPerFrame = registration.appleChannelsPerFrame;
    format.bitsPerChannel = registration.appleBitsPerChannel;
    format.sampleRate = registration.outputFormat.sampleRate;
    return format.formatFlags != 0 && format.bytesPerPacket != 0 &&
           format.framesPerPacket != 0 && format.bytesPerFrame != 0 &&
           format.channelsPerFrame != 0 && format.bitsPerChannel != 0;
}

bool AudioCoreRuntime::ResolveCandidate(std::uint32_t sampleRate,
                                        std::uint32_t channels,
                                        std::uint32_t& sourceBitDepth,
                                        std::uint64_t observationId,
                                        void*& converter) const noexcept {
    if (sourceBitDepth != 0) {
        return registry_.FindCandidate(sampleRate, channels, sourceBitDepth,
                                       observationId, converter);
    }
    return registry_.FindCandidateAnyPrecision(sampleRate, channels, observationId,
                                               converter, sourceBitDepth);
}

bool AudioCoreRuntime::ShouldSuppressNative(void* client) const noexcept {
    return UiEnabled() && client && nativeClient_.load(std::memory_order_acquire) == client &&
           coordinator_.ShouldSuppressNative();
}

bool AudioCoreRuntime::IsNativeP0CutoverActive(void* converter) const noexcept {
    if (!UiEnabled() || !converter ||
        boundConverter_.load(std::memory_order_acquire) != converter ||
        boundMediaGeneration_.load(std::memory_order_acquire) == 0 ||
        coordinator_.Phase() != AudioCoreGatePhase::Active ||
        !coordinator_.ShouldSuppressNative()) {
        return false;
    }
    return IsConverterBound(converter);
}

DWORD WINAPI AudioCoreRuntime::WorkerThunk(void* context) noexcept {
    return static_cast<AudioCoreRuntime*>(context)->WorkerMain();
}

DWORD AudioCoreRuntime::WorkerMain() noexcept {
    while (!workerStop_.load(std::memory_order_acquire)) {
        if (wakeEvent_) WaitForSingleObject(wakeEvent_, 50);
        if (workerStop_.load(std::memory_order_acquire)) break;
        if (!UiEnabled()) {
            // Keep the classifier observable in the control group without
            // activating any v2 graph, queue, sink or native-output gate.
            // The lock also keeps the Stats() snapshot consistent with the
            // control-thread retirement path.
            std::lock_guard lock(controlMutex_);
            if (UiEnabled()) continue;
            const auto now = GetTickCount64();
            if (nextDiagnosticTelemetryTick_ == 0 || now >= nextDiagnosticTelemetryTick_) {
                nextDiagnosticTelemetryTick_ = now + 5000;
                Emit(AudioCoreRuntimeEvent::Telemetry, S_OK);
            }
            continue;
        }
        {
            std::lock_guard lock(controlMutex_);
            nextDiagnosticTelemetryTick_ = 0;
            FinalizeEndOfStreamDrainLocked();
            if (!graphBound_) {
                if (void* pendingLocal =
                        pendingLocalRebindConverter_.exchange(nullptr, std::memory_order_acq_rel)) {
                    ConverterRegistrationSnapshot snapshot{};
                    if (registry_.Snapshot(pendingLocal, snapshot) &&
                        snapshot.registration.encodedFormat == kAppleLinearPcm &&
                        graphUnit_ && graphStarted_) {
                        graphConverter_ = pendingLocal;
                        graphRate_ = snapshot.registration.outputFormat.sampleRate;
                        graphChannels_ = snapshot.registration.outputFormat.channels;
                        graphSourceBits_ = snapshot.registration.sourceBitDepth;
                        graphObservationId_ = snapshot.registration.observationId;
                        TryBindGraphLocked();
                    } else if (registry_.Snapshot(pendingLocal, snapshot) &&
                               snapshot.registration.encodedFormat == kAppleLinearPcm) {
                        pendingLocalRebindConverter_.store(pendingLocal,
                                                           std::memory_order_release);
                    }
                }
            }
            TryActivateLocked();
            nativePumpBufferedFrames_.store(
                graphBound_ ? static_cast<std::uint64_t>(coordinator_.BufferedFrames()) : 0,
                std::memory_order_release);
            if (coordinator_.Phase() == AudioCoreGatePhase::Active) {
                const auto now = GetTickCount64();
                if (nextTelemetryTick_ == 0 || now >= nextTelemetryTick_) {
                    nextTelemetryTick_ = now + 5000;
                    Emit(AudioCoreRuntimeEvent::Telemetry, S_OK);
                }
            } else {
                nextTelemetryTick_ = 0;
            }
        }
    }
    return 0;
}

void AudioCoreRuntime::Wake() noexcept {
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void AudioCoreRuntime::Emit(AudioCoreRuntimeEvent event, HRESULT result) noexcept {
    if (event != AudioCoreRuntimeEvent::Telemetry) {
        lastEvent_.store(event, std::memory_order_release);
    }
    lastError_.store(result, std::memory_order_release);
    if (callbacks_.onEvent) callbacks_.onEvent(callbacks_.context, event, result);
}

AudioCoreRuntimeStats AudioCoreRuntime::Stats() const noexcept {
    const auto& coordinator = coordinator_;
    AudioCorePcmFillStats pcmFill{};
    pcmFill.rawFillCalls = rawFillCalls_.load(std::memory_order_acquire);
    pcmFill.fillSuccessWithData = fillSuccessWithData_.load(std::memory_order_acquire);
    pcmFill.fillSuccessZeroPackets = fillSuccessZeroPackets_.load(std::memory_order_acquire);
    pcmFill.fillErrors = fillErrors_.load(std::memory_order_acquire);
    pcmFill.producedPackets = producedPackets_.load(std::memory_order_acquire);
    pcmFill.rejectUiOff = rejectUiOff_.load(std::memory_order_acquire);
    pcmFill.rejectNoOutputData = rejectNoOutputData_.load(std::memory_order_acquire);
    pcmFill.rejectNoOutputPackets = rejectNoOutputPackets_.load(std::memory_order_acquire);
    pcmFill.rejectZeroPackets = rejectZeroPackets_.load(std::memory_order_acquire);
    pcmFill.rejectNoDestinationFormat =
        rejectNoDestinationFormat_.load(std::memory_order_acquire);
    pcmFill.rejectInvalidBufferList = rejectInvalidBufferList_.load(std::memory_order_acquire);
    pcmFill.rejectRegistryMissing = rejectRegistryMissing_.load(std::memory_order_acquire);
    pcmFill.rejectRegistryUnbound = rejectRegistryUnbound_.load(std::memory_order_acquire);
    pcmFill.rejectRateMismatch = rejectRateMismatch_.load(std::memory_order_acquire);
    pcmFill.rejectChannelMismatch = rejectChannelMismatch_.load(std::memory_order_acquire);
    pcmFill.rejectParse = rejectParse_.load(std::memory_order_acquire);
    pcmFill.rejectClaimFrames = rejectClaimFrames_.load(std::memory_order_acquire);
    pcmFill.rejectTap = rejectTap_.load(std::memory_order_acquire);
    pcmFill.acceptedCalls = acceptedCalls_.load(std::memory_order_acquire);
    pcmFill.acceptedFrames = acceptedFrames_.load(std::memory_order_acquire);
    pcmFill.lastResult = lastFillResult_.load(std::memory_order_acquire);
    pcmFill.lastProducedPackets = lastProducedPackets_.load(std::memory_order_acquire);
    return AudioCoreRuntimeStats{
        LastEvent(), coordinator.Phase(), UiEnabled(), coordinator.BufferedBlocks(),
        coordinator.BufferedFrames(), coordinator.Stats(), tap_.Stats(), coordinator.Sink().Stats(),
        pcmFill,
    };
}

void AudioCoreRuntime::TryBindGraphLocked() noexcept {
    if (!UiEnabled() || !graphUnit_ || graphRate_ < 8000 || graphRate_ > 768000 ||
        graphChannels_ != 2) return;
    // A successor graph may initialize/start a few milliseconds before the
    // previous track's final short endpoint period is released. Preserve the
    // old queue/sink until that terminal ReleaseBuffer completes; the current
    // graph identity is retained and rebound by FinalizeEndOfStreamDrainLocked.
    if ((endOfStreamDrainPending_ || streamingProducerDrainPending_) && graphBound_) return;

    if (!graphConverter_ && graphSourceBits_ != 0) {
        registry_.FindCandidate(graphRate_, graphChannels_, graphSourceBits_,
                                graphObservationId_, graphConverter_);
    }
    if (!graphConverter_) return;

    ConverterRegistrationSnapshot snapshot{};
    if (!registry_.Snapshot(graphConverter_, snapshot)) return;
    if (snapshot.registration.outputFormat.sampleRate != graphRate_ ||
        snapshot.registration.outputFormat.channels != graphChannels_ ||
        snapshot.registration.sourceBitDepth == 0 ||
        !IsSupportedSourceDepth(static_cast<std::uint16_t>(
            snapshot.registration.sourceBitDepth)) ||
        (graphSourceBits_ != 0 && graphSourceBits_ != snapshot.registration.sourceBitDepth)) {
        return;
    }

    if (graphBound_ && boundConverter_.load(std::memory_order_acquire) == graphConverter_) {
        return;
    }

    if (graphBound_) RetireCaptureLocked(true);

    const bool localSignedInt32 = ammod::audio::IsUnsupportedLocalInt32(
        snapshot.registration.encodedFormat, snapshot.registration.appleFormatFlags,
        snapshot.registration.sourceBitDepth);
    if (localSignedInt32) {
        // Reject this media item, not the user's AME intent. Keeping the runtime
        // armed guarantees that replaying the same int32 item is rejected again
        // instead of falling through to Apple's shared renderer.
        RetireCaptureLocked(true);
        lastError_.store(ammod::audio::kUnsupportedLocalInt32, std::memory_order_release);
        Emit(AudioCoreRuntimeEvent::Faulted, ammod::audio::kUnsupportedLocalInt32);
        return;
    }

    const auto generation = ++nextMediaGeneration_;
    registry_.UpdateMediaGeneration(graphConverter_, generation);
    if (!registry_.Snapshot(graphConverter_, snapshot)) return;

    PcmFormat format{};
    if (!BuildFormat(snapshot, format)) return;
    AudioCoreCoordinatorConfig config{};
    config.sink.format = format;
    const bool localFloat32 = snapshot.registration.encodedFormat == kAppleLinearPcm &&
        (snapshot.registration.appleFormatFlags & kAppleFormatFlagIsFloat) != 0 &&
        snapshot.registration.sourceBitDepth == 32;
    const auto candidates = ammod::audio::SelectBitPerfectFormatCandidates(
        snapshot.registration.sourceBitDepth, localFloat32);
    config.sink.formatCandidateCount = static_cast<std::uint32_t>(candidates.count);
    for (std::size_t index = 0; index < candidates.count; ++index) {
        config.sink.formatCandidates[index] = {
            format.sampleRate, format.channels,
            candidates.values[index].validBits, candidates.values[index].containerBits,
            PcmEncoding::SignedInteger, true,
            static_cast<std::uint16_t>(snapshot.registration.sourceBitDepth)};
    }
    config.sink.endpointId = nativeEndpoint_;
    config.sink.periodFrames = PeriodFrames(format.sampleRate);
    config.sink.alignmentRetries = 3;
    config.sink.lifecycleContext = callbacks_.context;
    config.sink.onLifecycle = callbacks_.onSinkLifecycle;
    config.queueCapacity = kProductionQueueCapacity;
    config.mediaGeneration = generation;
    config.mappingPolicy = localFloat32
        ? PcmMappingPolicy::LocalFloat32ToPcm32
        : PcmMappingPolicy::ExactSourceInteger;
    config.verifyBitPerfect = callbacks_.deepDiagnostics;
    const HRESULT enable = coordinator_.Enable(config);
    if (FAILED(enable) || !tap_.BindActive(graphConverter_, format, generation) ||
        !registry_.Bind(graphConverter_, generation)) {
        coordinator_.Disable();
        const auto error = FAILED(enable) ? enable : E_UNEXPECTED;
        lastError_.store(error, std::memory_order_release);
        uiEnabled_.store(false, std::memory_order_release);
        Emit(AudioCoreRuntimeEvent::Faulted, error);
        registry_.RetireAll();
        return;
    }
    boundConverter_.store(graphConverter_, std::memory_order_release);
    boundMediaGeneration_.store(generation, std::memory_order_release);
    graphBound_ = true;
    // Cut over only after roughly 100 ms is already captured. A single 20 ms
    // device period was too easy to drain during transient CPU scheduling load;
    // this remains far below the old second-scale reservoir that hurt controls.
    requiredPrebufferFrames_ = std::max<std::uint32_t>(
        config.sink.periodFrames ? config.sink.periodFrames * 5u : 0u,
        std::max<std::uint32_t>(1u, format.sampleRate / 10u));
    nativePumpSampleRate_.store(format.sampleRate, std::memory_order_release);
    nativePumpBufferedFrames_.store(
        static_cast<std::uint64_t>(coordinator_.BufferedFrames()),
        std::memory_order_release);
    Emit(AudioCoreRuntimeEvent::Bound, S_OK);
}

void AudioCoreRuntime::ExtendSeekQuietPeriodLocked() noexcept {
    if (!seekFence_ || seekFenceReleaseTick_ == 0) return;
    seekFenceReleaseTick_ = GetTickCount64() + 500;
}

void AudioCoreRuntime::TryResumePausedSinkLocked() noexcept {
    if (userPauseIntent_ || seekFence_ || !graphStarted_ || !nativeStarted_ ||
        coordinator_.Phase() != AudioCoreGatePhase::Active ||
        coordinator_.SinkState() != WasapiSinkState::Open) {
        return;
    }
    const HRESULT resume = coordinator_.ResumeEndpoint();
    if (FAILED(resume)) {
        lastError_.store(resume, std::memory_order_release);
        Emit(AudioCoreRuntimeEvent::Faulted, resume);
        RetireCaptureLocked(false);
    }
}

void AudioCoreRuntime::TryActivateLocked() noexcept {
    if (userPauseIntent_) return;
    if (seekFence_) {
        if (seekFenceReleaseTick_ == 0 || GetTickCount64() < seekFenceReleaseTick_) return;
        seekFence_ = false;
        seekFenceReleaseTick_ = 0;
    }
    if (!UiEnabled() || !graphBound_ || !graphStarted_ || !nativeInitialized_ ||
        !nativeStarted_ || coordinator_.Phase() != AudioCoreGatePhase::Capturing ||
        coordinator_.BufferedFrames() < requiredPrebufferFrames_) {
        return;
    }

    void* nativeClient = nativeClient_.load(std::memory_order_acquire);
    if (!nativeClient || !callbacks_.prepareNativeHandoff) return;
    void* nativeOutputUnit = graphOutputUnit_;

    // Apple has already initialized and started its scheduler by the time the
    // P0 queue reaches a complete device period. Historical handoffs could
    // stop/reset that scheduler before v2 opened the endpoint; the current
    // passthrough handoff preserves the AudioUnit/software-client scheduling
    // state and only removes physical endpoint ownership. Keep the bookkeeping
    // aligned with whichever handoff contract the callback declares.
    nativeHandoffPending_.store(true, std::memory_order_release);
    HRESULT graphHandoff = S_OK;
    if (nativeOutputUnit && callbacks_.prepareNativeGraphHandoff) {
        nativeGraphHandoffPending_.store(true, std::memory_order_release);
        graphHandoff = callbacks_.prepareNativeGraphHandoff(callbacks_.context,
                                                             nativeOutputUnit);
        nativeGraphHandoffPending_.store(false, std::memory_order_release);
        if (SUCCEEDED(graphHandoff) && !callbacks_.nativeHandoffPreservesScheduling) {
            graphStarted_ = false;
        }
    }
    if (FAILED(graphHandoff)) {
        if (!callbacks_.nativeHandoffPreservesScheduling &&
            callbacks_.resumeNativeGraphAfterFailedHandoff) {
            nativeGraphHandoffPending_.store(true, std::memory_order_release);
            (void)callbacks_.resumeNativeGraphAfterFailedHandoff(
                callbacks_.context, nativeOutputUnit);
            nativeGraphHandoffPending_.store(false, std::memory_order_release);
        }
        nativeHandoffPending_.store(false, std::memory_order_release);
        lastError_.store(graphHandoff, std::memory_order_release);
        uiEnabled_.store(false, std::memory_order_release);
        Emit(AudioCoreRuntimeEvent::Faulted, graphHandoff);
        RetireCaptureLocked(false);
        registry_.RetireAll();
        return;
    }
    const HRESULT handoff = callbacks_.prepareNativeHandoff(callbacks_.context,
                                                              nativeClient);
    nativeHandoffPending_.store(false, std::memory_order_release);
    if (FAILED(handoff)) {
        if (!callbacks_.nativeHandoffPreservesScheduling && nativeOutputUnit &&
            callbacks_.resumeNativeGraphAfterFailedHandoff) {
            nativeGraphHandoffPending_.store(true, std::memory_order_release);
            const HRESULT resume = callbacks_.resumeNativeGraphAfterFailedHandoff(
                callbacks_.context, nativeOutputUnit);
            nativeGraphHandoffPending_.store(false, std::memory_order_release);
            if (SUCCEEDED(resume)) graphStarted_ = true;
        }
        lastError_.store(handoff, std::memory_order_release);
        uiEnabled_.store(false, std::memory_order_release);
        Emit(AudioCoreRuntimeEvent::Faulted, handoff);
        RetireCaptureLocked(false);
        registry_.RetireAll();
        return;
    }
    if (!callbacks_.nativeHandoffPreservesScheduling) nativeStarted_ = false;

    const HRESULT open = coordinator_.OpenEndpoint();
    if (FAILED(open)) {
        if (!callbacks_.nativeHandoffPreservesScheduling &&
            callbacks_.resumeNativeAfterFailedHandoff) {
            nativeHandoffPending_.store(true, std::memory_order_release);
            const HRESULT resume = callbacks_.resumeNativeAfterFailedHandoff(
                callbacks_.context, nativeClient);
            nativeHandoffPending_.store(false, std::memory_order_release);
            if (SUCCEEDED(resume)) nativeStarted_ = true;
        }
        if (!callbacks_.nativeHandoffPreservesScheduling && nativeOutputUnit &&
            callbacks_.resumeNativeGraphAfterFailedHandoff) {
            nativeGraphHandoffPending_.store(true, std::memory_order_release);
            const HRESULT resume = callbacks_.resumeNativeGraphAfterFailedHandoff(
                callbacks_.context, nativeOutputUnit);
            nativeGraphHandoffPending_.store(false, std::memory_order_release);
            if (SUCCEEDED(resume)) graphStarted_ = true;
        }
        lastError_.store(open, std::memory_order_release);
        const bool playbackRejected = ammod::audio::IsPlaybackRejection(open);
        if (!playbackRejected) uiEnabled_.store(false, std::memory_order_release);
        Emit(AudioCoreRuntimeEvent::Faulted, open);
        RetireCaptureLocked(playbackRejected);
        if (!playbackRejected) registry_.RetireAll();
        return;
    }
    const HRESULT start = coordinator_.Start();
    if (FAILED(start)) {
        if (!callbacks_.nativeHandoffPreservesScheduling &&
            callbacks_.resumeNativeAfterFailedHandoff) {
            nativeHandoffPending_.store(true, std::memory_order_release);
            const HRESULT resume = callbacks_.resumeNativeAfterFailedHandoff(
                callbacks_.context, nativeClient);
            nativeHandoffPending_.store(false, std::memory_order_release);
            if (SUCCEEDED(resume)) nativeStarted_ = true;
        }
        if (!callbacks_.nativeHandoffPreservesScheduling && nativeOutputUnit &&
            callbacks_.resumeNativeGraphAfterFailedHandoff) {
            nativeGraphHandoffPending_.store(true, std::memory_order_release);
            const HRESULT resume = callbacks_.resumeNativeGraphAfterFailedHandoff(
                callbacks_.context, nativeOutputUnit);
            nativeGraphHandoffPending_.store(false, std::memory_order_release);
            if (SUCCEEDED(resume)) graphStarted_ = true;
        }
        lastError_.store(start, std::memory_order_release);
        uiEnabled_.store(false, std::memory_order_release);
        Emit(AudioCoreRuntimeEvent::Faulted, start);
        RetireCaptureLocked(false);
        registry_.RetireAll();
        return;
    }
    if (callbacks_.finalizeNativeHandoff) {
        callbacks_.finalizeNativeHandoff(callbacks_.context, nativeClient);
    }
    Emit(AudioCoreRuntimeEvent::Active, S_OK);
}

void AudioCoreRuntime::FinalizeEndOfStreamDrainLocked() noexcept {
    if (!endOfStreamDrainPending_ || !coordinator_.EndOfStreamSubmitted()) return;

    const auto drainedConverter = endOfStreamDrainConverter_;
    const auto localSuccessor = pendingLocalSuccessor_;
    endOfStreamDrainPending_ = false;
    endOfStreamDrainConverter_ = nullptr;
    pendingLocalSuccessor_ = nullptr;

    // The terminal endpoint buffer has been released successfully. Only now is
    // it safe to tear down the old generation. This covers both imported local
    // media and the bounded final-period remainder of a naturally ending
    // streaming track.
    RetireCaptureLocked(true);
    if (graphConverter_ == drainedConverter) graphConverter_ = nullptr;

    if (localSuccessor) {
        ConverterRegistrationSnapshot snapshot{};
        if (registry_.Snapshot(localSuccessor, snapshot) &&
            snapshot.registration.encodedFormat == kAppleLinearPcm &&
            graphUnit_ && graphStarted_) {
            graphConverter_ = localSuccessor;
            graphRate_ = snapshot.registration.outputFormat.sampleRate;
            graphChannels_ = snapshot.registration.outputFormat.channels;
            graphSourceBits_ = snapshot.registration.sourceBitDepth;
            graphObservationId_ = snapshot.registration.observationId;
        }
    }
    // A streaming successor can initialize/start while the old terminal period
    // is still draining. OnAudioUnitInitialized retained that graph identity;
    // bind it now that the old sink has been closed.
    TryBindGraphLocked();
}

void AudioCoreRuntime::RetireCaptureLocked(bool keepGraph) noexcept {
    graphPaused_ = false;
    graphControlPending_ = false;
    streamingProducerDrainPending_ = false;
    streamingProducerDrainConverter_ = nullptr;
    streamingEofCandidateConverter_.store(nullptr, std::memory_order_release);
    endOfStreamDrainPending_ = false;
    endOfStreamDrainConverter_ = nullptr;
    pendingLocalSuccessor_ = nullptr;
    pendingLocalRebindConverter_.store(nullptr, std::memory_order_release);
    const bool hasCapture = graphBound_ ||
        boundConverter_.load(std::memory_order_acquire) != nullptr ||
        coordinator_.Phase() != AudioCoreGatePhase::Off;
    if (hasCapture) {
        coordinator_.SetSinkResumeBlocked(false);
        const auto converter = boundConverter_.exchange(nullptr, std::memory_order_acq_rel);
        if (converter) {
            tap_.Retire(converter);
            registry_.Unbind(converter);
        }
        while (callbackDepth_.load(std::memory_order_acquire) != 0) SwitchToThread();
        coordinator_.Disable();
        graphBound_ = false;
        boundMediaGeneration_.store(0, std::memory_order_release);
        nativePumpBufferedFrames_.store(0, std::memory_order_release);
        nativePumpSampleRate_.store(0, std::memory_order_release);
        requiredPrebufferFrames_ = 0;
        nextTelemetryTick_ = 0;
    }
    if (!keepGraph) {
        graphUnit_ = nullptr;
        graphConverter_ = nullptr;
        graphRate_ = 0;
        graphChannels_ = 0;
        graphSourceBits_ = 0;
        graphStarted_ = false;
    }
}

void AudioCoreRuntime::RetireGraphLocked(void* unit) noexcept {
    if (graphUnit_ == unit) RetireCaptureLocked(false);
}

bool AudioCoreRuntime::BuildFormat(const ConverterRegistrationSnapshot& snapshot,
                                   PcmFormat& output) const noexcept {
    const auto sourceBits = snapshot.registration.sourceBitDepth;
    if (sourceBits == 0 || sourceBits > UINT16_MAX ||
        !IsSupportedSourceDepth(static_cast<std::uint16_t>(sourceBits))) return false;
    const auto candidates = ammod::audio::SelectBitPerfectFormatCandidates(
        sourceBits,
        snapshot.registration.encodedFormat == kAppleLinearPcm &&
            (snapshot.registration.appleFormatFlags & kAppleFormatFlagIsFloat) != 0);
    if (candidates.count == 0) return false;
    output = {snapshot.registration.outputFormat.sampleRate, 2,
              candidates.values[0].validBits, candidates.values[0].containerBits,
              PcmEncoding::SignedInteger, true, static_cast<std::uint16_t>(sourceBits)};
    return IsValidFormat(output);
}

std::uint32_t AudioCoreRuntime::NativePumpPeriodMs() const noexcept {
    constexpr std::uint32_t kNominalMs = 10;
    constexpr std::uint32_t kCatchUpMs = 9;
    constexpr std::uint32_t kBackOffMs = 11;

    const auto sampleRate = nativePumpSampleRate_.load(std::memory_order_acquire);
    const auto bufferedFrames = nativePumpBufferedFrames_.load(std::memory_order_acquire);
    if (!UiEnabled() || sampleRate < 8000 || sampleRate > 768000) return kNominalMs;

    // Keep a modest load-jitter reservoir without returning to the old 1+ s
    // pause-latency buffer. 300-500 ms gives the producer substantially more
    // scheduling headroom while the 9/10/11 ms loop still corrects clock drift.
    const std::uint64_t lowWaterFrames =
        (static_cast<std::uint64_t>(sampleRate) * 3u) / 10u;
    const std::uint64_t highWaterFrames = sampleRate / 2u;
    if (bufferedFrames < lowWaterFrames) return kCatchUpMs;
    if (bufferedFrames > highWaterFrames) return kBackOffMs;
    return kNominalMs;
}

std::uint32_t AudioCoreRuntime::PeriodFrames(std::uint32_t sampleRate) const noexcept {
    if (!sampleRate) return 0;
    constexpr std::uint32_t period100ns = 200000; // 20 ms, fixed production default
    const auto frames = (static_cast<std::uint64_t>(sampleRate) * period100ns +
                         kReferenceTimePerSecond / 2u) / kReferenceTimePerSecond;
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(1u, frames));
}

} // namespace ammod::audio_v2
