#pragma once

#include "ApplePcmOutput.h"
#include "ApplePcmTap.h"
#include "AudioCoreConverterRegistry.h"
#include "WasapiExclusiveSink.h"

#include <audioclient.h>
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace ammod::audio_v2 {

enum class AudioCoreRuntimeEvent : std::uint8_t {
    Bound,
    Active,
    Faulted,
    Disabled,
    Telemetry,
};

struct AudioCoreRuntimeCallbacks final {
    void* context{};
    void (*onEvent)(void* context, AudioCoreRuntimeEvent event, HRESULT result) noexcept{};
    void (*onSinkLifecycle)(void* context, WasapiSinkLifecycleEvent event,
                            HRESULT result) noexcept{};
    HRESULT (*prepareNativeHandoff)(void* context, void* client) noexcept{};
    void (*finalizeNativeHandoff)(void* context, void* client) noexcept{};
    HRESULT (*resumeNativeAfterFailedHandoff)(void* context, void* client) noexcept{};
    HRESULT (*prepareNativeGraphHandoff)(void* context, void* unit) noexcept{};
    HRESULT (*resumeNativeGraphAfterFailedHandoff)(void* context, void* unit) noexcept{};
    // Passthrough handoff keeps Apple's AudioUnit and software IAudioClient
    // scheduler running while only the physical endpoint ownership changes.
    bool nativeHandoffPreservesScheduling{};
    // Expensive per-sample verification is diagnostic-only. Production keeps
    // the exact mapper fail-closed but does not remap every sample a second time.
    bool deepDiagnostics{};
};

struct AudioCorePcmFillStats final {
    std::uint64_t rawFillCalls{};
    std::uint64_t fillSuccessWithData{};
    std::uint64_t fillSuccessZeroPackets{};
    std::uint64_t fillErrors{};
    std::uint64_t producedPackets{};
    std::uint64_t rejectUiOff{};
    std::uint64_t rejectNoOutputData{};
    std::uint64_t rejectNoOutputPackets{};
    std::uint64_t rejectZeroPackets{};
    std::uint64_t rejectNoDestinationFormat{};
    std::uint64_t rejectInvalidBufferList{};
    std::uint64_t rejectRegistryMissing{};
    std::uint64_t rejectRegistryUnbound{};
    std::uint64_t rejectRateMismatch{};
    std::uint64_t rejectChannelMismatch{};
    std::uint64_t rejectParse{};
    std::uint64_t rejectClaimFrames{};
    std::uint64_t rejectTap{};
    std::uint64_t acceptedCalls{};
    std::uint64_t acceptedFrames{};
    std::int32_t lastResult{};
    std::uint32_t lastProducedPackets{};
};

enum class AudioCorePcmRejectReason : std::uint8_t {
    NoDestinationFormat,
    InvalidBufferList,
    RegistryMissing,
    RegistryUnbound,
    RateMismatch,
    ChannelMismatch,
    Parse,
    ClaimFrames,
    Tap,
};

struct AudioCoreRuntimeStats final {
    AudioCoreRuntimeEvent event{AudioCoreRuntimeEvent::Disabled};
    AudioCoreGatePhase phase{AudioCoreGatePhase::Off};
    bool uiEnabled{};
    std::size_t bufferedBlocks{};
    std::size_t bufferedFrames{};
    AudioCoreCoordinatorStats coordinator{};
    ApplePcmTapStats tap{};
    WasapiSinkStats sink{};
    AudioCorePcmFillStats pcmFill{};
};

// Production bridge for the v2 core. Hook entry points call the converter and
// AudioUnit methods here; all endpoint ownership transitions happen on the
// private worker thread. The only callback-thread operation is copying the
// returned Apple float32 buffer into the preallocated SPSC queue.
class AudioCoreRuntime final {
public:
    AudioCoreRuntime() noexcept;
    ~AudioCoreRuntime();

    AudioCoreRuntime(const AudioCoreRuntime&) = delete;
    AudioCoreRuntime& operator=(const AudioCoreRuntime&) = delete;

    bool Start(const AudioCoreRuntimeCallbacks& callbacks) noexcept;
    void Stop() noexcept;
    void SetUiEnabled(bool enabled) noexcept;
    void SetHardwareBufferMilliseconds(std::uint32_t milliseconds) noexcept;
    bool UiEnabled() const noexcept { return uiEnabled_.load(std::memory_order_acquire); }

    // UI transport intents are control-plane only. They gate when the exact
    // endpoint stream may run; they never modify or synthesize PCM samples.
    void OnTransportPlayPause() noexcept;
    void OnTransportSelectionArm(std::uint64_t epoch) noexcept;
    void OnTransportSkipArm(std::uint64_t epoch) noexcept;
    void OnTransportSkip(std::uint64_t epoch = 0) noexcept;
    void OnTransportSeekBegin(std::uint64_t epoch) noexcept;
    void OnTransportSeekCommit(std::uint64_t epoch) noexcept;

    void OnConverterCreated(void* converter, const ApplePcmFormatView& source,
                            const ApplePcmFormatView& destination,
                            std::uint64_t observationId) noexcept;
    void OnConverterSourcePrecision(void* converter, std::uint32_t sourceBitDepth,
                                    std::uint32_t sampleRate,
                                    std::uint32_t channels) noexcept;
    void OnConverterReset(void* converter, bool forceTimelineRetire = false) noexcept;
    bool OnLocalEndOfStream(void* converter) noexcept;
    void SetPendingLocalSuccessor(void* converter) noexcept;
    void RequestLocalSuccessorBind(void* converter) noexcept;
    void OnConverterDisposed(void* converter) noexcept;
    bool TryBindLocalSuccessor(void* converter) noexcept;

    void OnAudioUnitInitialized(void* unit, void* candidateConverter,
                                std::uint32_t sampleRate, std::uint32_t channels,
                                std::uint32_t sourceBitDepth,
                                std::uint64_t observationId = 0) noexcept;
    void OnAudioUnitStarted(void* unit) noexcept;
    void OnAudioUnitStopped(void* unit) noexcept;
    void OnAudioUnitUninitialized(void* unit) noexcept;

    void OnNativeClientInitialized(void* client, const wchar_t* endpoint,
                                   AUDCLNT_SHAREMODE shareMode, DWORD flags,
                                   const WAVEFORMATEX* format, HRESULT result) noexcept;
    void OnNativeClientStarted(void* client, HRESULT result) noexcept;
    // Called before NativeRenderGateProxy stops its software pump. For a
    // natural streaming boundary it briefly keeps Apple's existing scheduler
    // alive so a pending decoder tail can reach the normal zero-packet EOF
    // callback. Explicit control stops return immediately.
    bool BeforeNativeClientStop(void* client) noexcept;
    // Returns the bound streaming P0 converter when a natural native-client
    // Stop should synchronously drain the producer to true decoder EOF before
    // Apple tears down the graph. Explicit pause/seek/skip paths return null.
    void* OnNativeClientStopped(void* client, HRESULT result) noexcept;
    bool OnStreamingProducerEndOfStream(void* converter) noexcept;
    void OnNativeClientReset(void* client, HRESULT result) noexcept;

    // Called from the FillComplexBuffer detour after Apple's original call
    // returns. It never waits, allocates, or takes the control mutex.
    void OnPcmFillResult(std::int32_t result, std::uint32_t producedPackets,
                        bool outputDataPresent, bool outputPacketsPresent) noexcept;
    void RecordPcmReject(AudioCorePcmRejectReason reason) noexcept;
    bool OnPcmOutput(void* converter, const ApplePcmFormatView& format,
                     const ApplePcmBufferListView& list,
                     std::uint32_t producedPackets) noexcept;

    bool DestinationFormat(void* converter, ApplePcmFormatView& format) const noexcept;

    // Control-thread graph binder helper.  It exposes the same fixed-slot
    // lookup used internally so the hook can record whether a cross-thread
    // AudioUnit identity resolved to a registered P0 converter.
    bool ResolveCandidate(std::uint32_t sampleRate, std::uint32_t channels,
                          std::uint32_t& sourceBitDepth,
                          std::uint64_t observationId,
                          void*& converter) const noexcept;

    // Diagnostic/control-thread query used by the hook boundary.  It only
    // reads the fixed-slot registry and never touches Apple callback state.
    bool IsConverterRegistered(void* converter) const noexcept;

    // Only compressed-source registrations represent Apple's decoder-side P0
    // converter.  LPCM helper/resampler converters may also be registered for
    // graph binding, but they must never replace the captured P0 callback pair.
    bool IsP0Converter(void* converter) const noexcept;

    // The P0 producer may be taken over only after the graph binder has
    // published the converter into the active generation.  This is a pure
    // fixed-slot lookup and does not touch Apple state.
    bool IsConverterBound(void* converter) const noexcept;
    void* BoundConverter() const noexcept {
        return boundConverter_.load(std::memory_order_acquire);
    }

    // Called from the native ReleaseBuffer detour. It is a pure atomic query.
    bool ShouldSuppressNative(void* client) const noexcept;

    // Native P0 may be taken over only after the exact bound converter's
    // replacement sink has started and the output gate has committed Active.
    // This is deliberately stricter than IsConverterBound(): binding only
    // proves that capture can be staged, not that the native producer can be
    // stopped safely.
    bool IsNativeP0CutoverActive(void* converter) const noexcept;

    // Lock-free feedback for the software native-render pump. The worker
    // publishes queue depth; the pump only reads atomics and never touches the
    // coordinator/SPSC lifetime directly.
    std::uint32_t NativePumpPeriodMs() const noexcept;

    AudioCoreRuntimeEvent LastEvent() const noexcept {
        return lastEvent_.load(std::memory_order_acquire);
    }
    HRESULT LastError() const noexcept {
        return static_cast<HRESULT>(lastError_.load(std::memory_order_acquire));
    }
    AudioCoreRuntimeStats Stats() const noexcept;
    const AudioCoreCoordinator& Coordinator() const noexcept { return coordinator_; }

private:
    static DWORD WINAPI WorkerThunk(void* context) noexcept;
    DWORD WorkerMain() noexcept;

    void Wake() noexcept;
    void Emit(AudioCoreRuntimeEvent event, HRESULT result) noexcept;
    void TryBindGraphLocked() noexcept;
    void TryActivateLocked() noexcept;
    void TryResumePausedSinkLocked() noexcept;
    void ExtendSeekQuietPeriodLocked() noexcept;
    bool BeginEndOfStreamDrainLocked(void* converter, bool requireShortTail) noexcept;
    void FinalizeEndOfStreamDrainLocked() noexcept;
    void RetireCaptureLocked(bool keepGraph) noexcept;
    void RetireGraphLocked(void* unit) noexcept;
    bool BuildFormat(const ConverterRegistrationSnapshot& snapshot,
                     PcmFormat& output) const noexcept;
    std::uint32_t PeriodFrames(std::uint32_t sampleRate) const noexcept;

    AudioCoreRuntimeCallbacks callbacks_{};
    std::atomic<bool> running_{};
    std::atomic<bool> uiEnabled_{};
    std::atomic<bool> workerStop_{};
    std::atomic<std::uint32_t> hardwareBufferMs_{20};
    HANDLE wakeEvent_{};
    HANDLE workerThread_{};

    mutable std::mutex controlMutex_;
    AudioCoreConverterRegistry registry_{};
    AudioCoreCoordinator coordinator_{};
    ApplePcmTap tap_;

    std::atomic<std::uint32_t> callbackDepth_{};
    std::atomic<void*> boundConverter_{};
    std::atomic<void*> nativeClient_{};
    std::atomic<bool> nativeHandoffPending_{};
    std::atomic<bool> nativeGraphHandoffPending_{};
    void* graphUnit_{};
    void* graphOutputUnit_{};
    void* graphConverter_{};
    std::uint32_t graphRate_{};
    std::uint32_t graphChannels_{};
    std::uint32_t graphSourceBits_{};
    std::uint64_t graphObservationId_{};
    bool graphStarted_{};
    bool graphPaused_{};
    bool graphControlPending_{};
    bool userPauseIntent_{};
    bool seekFence_{};
    std::uint64_t latestSeekEpoch_{};
    ULONGLONG seekFenceReleaseTick_{};
    bool nativeInitialized_{};
    bool nativeStarted_{};
    bool graphBound_{};
    // Streaming native Stop is not decoder EOF. Hold the current generation
    // across AudioUnit Stop/Uninitialize while HookDll synchronously flushes
    // the still-live P0 converter to producedPackets==0.
    bool streamingProducerDrainPending_{};
    void* streamingProducerDrainConverter_{};
    // A successful streaming P0 Fill with zero packets is only an EOF
    // candidate until Apple either stops the native scheduler (true natural
    // EOF) or produces more PCM on the same converter (Repeat One / loop).
    // Keep this lock-free so the P0 callback can cancel the candidate and
    // restart its frame cursor without waiting on the control thread.
    std::atomic<void*> streamingEofCandidateConverter_{};
    bool endOfStreamDrainPending_{};
    void* endOfStreamDrainConverter_{};
    std::atomic<std::uint64_t> directSelectionArmEpoch_{};
    std::atomic<std::uint64_t> directSelectionArmGeneration_{};
    std::atomic<std::uint64_t> transportSkipArmEpoch_{};
    std::atomic<std::uint64_t> transportSkipArmGeneration_{};
    std::atomic<std::uint64_t> directSelectionSkipEpoch_{};
    std::atomic<ULONGLONG> directSelectionArmDeadlineTick_{};
    void* pendingLocalSuccessor_{};
    std::atomic<void*> pendingLocalRebindConverter_{};
    std::uint64_t nextMediaGeneration_{1};
    std::atomic<std::uint64_t> boundMediaGeneration_{};
    std::uint32_t requiredPrebufferFrames_{};
    std::wstring nativeEndpoint_;
    ULONGLONG nextTelemetryTick_{};
    // Read-only classifier snapshots continue while the UI switch is Off.
    // This is deliberately separate from the Active telemetry tick so an
    // observer-only run never arms the v2 graph or endpoint.
    ULONGLONG nextDiagnosticTelemetryTick_{};

    std::atomic<AudioCoreRuntimeEvent> lastEvent_{AudioCoreRuntimeEvent::Disabled};
    std::atomic<LONG> lastError_{S_OK};
    // A decoder cookie may arrive before its float32 P0 converter is
    // registered (or on a different converter handle).  Keep the latest
    // validated precision so a later registration can inherit it.
    std::atomic<std::uint32_t> pendingCompressedRate_{};
    std::atomic<std::uint32_t> pendingCompressedChannels_{};
    std::atomic<std::uint32_t> pendingCompressedBits_{};

    std::atomic<std::uint64_t> rawFillCalls_{};
    std::atomic<std::uint64_t> fillSuccessWithData_{};
    std::atomic<std::uint64_t> fillSuccessZeroPackets_{};
    std::atomic<std::uint64_t> fillErrors_{};
    std::atomic<std::uint64_t> producedPackets_{};
    std::atomic<std::uint64_t> rejectUiOff_{};
    std::atomic<std::uint64_t> rejectNoOutputData_{};
    std::atomic<std::uint64_t> rejectNoOutputPackets_{};
    std::atomic<std::uint64_t> rejectZeroPackets_{};
    std::atomic<std::uint64_t> rejectNoDestinationFormat_{};
    std::atomic<std::uint64_t> rejectInvalidBufferList_{};
    std::atomic<std::uint64_t> rejectRegistryMissing_{};
    std::atomic<std::uint64_t> rejectRegistryUnbound_{};
    std::atomic<std::uint64_t> rejectRateMismatch_{};
    std::atomic<std::uint64_t> rejectChannelMismatch_{};
    std::atomic<std::uint64_t> rejectParse_{};
    std::atomic<std::uint64_t> rejectClaimFrames_{};
    std::atomic<std::uint64_t> rejectTap_{};
    std::atomic<std::uint64_t> acceptedCalls_{};
    std::atomic<std::uint64_t> acceptedFrames_{};
    std::atomic<std::int32_t> lastFillResult_{};
    std::atomic<std::uint32_t> lastProducedPackets_{};
    std::atomic<std::uint32_t> nativePumpSampleRate_{};
    std::atomic<std::uint64_t> nativePumpBufferedFrames_{};
};

} // namespace ammod::audio_v2
