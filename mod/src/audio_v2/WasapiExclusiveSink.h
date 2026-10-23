#pragma once

#include "IntegerPcmSource.h"
#include "PcmTypes.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ammod::audio_v2 {

// Lifecycle notifications are optional and diagnostic-only.  They are invoked
// on the thread that performs the corresponding sink operation and carry the
// HRESULT observed at the operation boundary.  The production hook uses them
// to build a QPC ownership timeline; tests leave the callback unset.
enum class WasapiSinkLifecycleEvent : std::uint8_t {
    OpenEnter,
    OpenExit,
    InitializeEnter,
    InitializeExit,
    StartEnter,
    StartExit,
    StopEnter,
    StopExit,
    CloseEnter,
    CloseExit,
};

using WasapiSinkLifecycleCallback = void (*)(
    void* context, WasapiSinkLifecycleEvent event, HRESULT result) noexcept;

struct WasapiSinkConfig final {
    PcmFormat format{};
    std::array<PcmFormat, kMaxFormatCandidates> formatCandidates{};
    std::uint32_t formatCandidateCount{};
    std::wstring endpointId;
    std::uint32_t periodFrames{};
    std::uint32_t alignmentRetries{2};
    bool allowResampling{};
    // Non-owning producer notification. The coordinator owns this handle and
    // keeps it valid until the sink has stopped and closed.
    HANDLE sourceReadyEvent{};
    void* lifecycleContext{};
    WasapiSinkLifecycleCallback onLifecycle{};
};

struct WasapiSinkStats final {
    std::uint64_t submittedFrames{};
    std::uint64_t submittedBuffers{};
    std::uint64_t controlledSilenceFrames{};
    std::uint64_t sourceWaits{};
    std::uint64_t underruns{};
    std::uint64_t getBufferFailures{};
    std::uint64_t releaseBufferFailures{};
    HRESULT lastError{S_OK};
    std::uint64_t clockPosition{};
    std::uint64_t clockTarget{};
    std::uint64_t clockFrequency{};
    std::uint64_t drainElapsedMs{};
    HRESULT clockReadResult{S_OK};
};

enum class WasapiSinkState : std::uint8_t {
    Closed,
    Open,
    Running,
    Faulted,
};

class WasapiExclusiveSink final {
public:
    WasapiExclusiveSink() noexcept = default;
    ~WasapiExclusiveSink();

    WasapiExclusiveSink(const WasapiExclusiveSink&) = delete;
    WasapiExclusiveSink& operator=(const WasapiExclusiveSink&) = delete;

    HRESULT Open(const WasapiSinkConfig& config) noexcept;
    HRESULT Start(IIntegerPcmSource& source) noexcept;
    void Pause() noexcept;
    void Stop() noexcept;
    void Close() noexcept;

    WasapiSinkState State() const noexcept { return state_.load(std::memory_order_acquire); }
    WasapiSinkStats Stats() const noexcept;
    const PcmFormat& Format() const noexcept { return config_.format; }
    std::uint32_t BufferFrames() const noexcept { return bufferFrames_; }
    bool WaitingForSource() const noexcept {
        return waitingForSource_.load(std::memory_order_acquire);
    }
    bool EndOfStreamSubmitted() const noexcept {
        return endOfStreamSubmitted_.load(std::memory_order_acquire);
    }
    bool EndOfStreamComplete() const noexcept {
        return endOfStreamComplete_.load(std::memory_order_acquire);
    }
    // The coordinator blocks the render thread from consuming a producer
    // notification while it pauses/resumes Apple's graph.  This closes the
    // small race where the source becomes ready between the worker's state
    // check and its graph-control call.
    void SetResumeBlocked(bool blocked) noexcept;

private:
    static DWORD WINAPI RenderThreadThunk(void* context) noexcept;
    DWORD RenderThreadMain() noexcept;

    HRESULT ActivateClient() noexcept;
    HRESULT InitializeClient() noexcept;
    HRESULT RenderOneBuffer() noexcept;
    HRESULT CheckEndOfStreamDrain() noexcept;
    HRESULT ReadClockPosition(std::uint64_t& position) noexcept;
    bool FramesToClockTicks(std::uint64_t frames, std::uint64_t& ticks) const noexcept;
    void ResetClockTimeline() noexcept;
    void StopInternal(bool preserveBuffer) noexcept;
    void RecordError(HRESULT error) noexcept;
    void NotifyLifecycle(WasapiSinkLifecycleEvent event, HRESULT result) noexcept;
    void ReleaseInterfaces() noexcept;
    void ReleaseComInitialization() noexcept;

    WasapiSinkConfig config_{};
    WAVEFORMATEXTENSIBLE waveFormat_{};
    std::vector<std::int32_t> canonicalBuffer_;

    IMMDeviceEnumerator* enumerator_{};
    IMMDevice* device_{};
    IAudioClient* client_{};
    IAudioRenderClient* render_{};
    IAudioClock* clock_{};
    std::uint64_t drainClockTicks_{};
    std::uint64_t drainMaxWaitMs_{};
    std::uint64_t drainStartPosition_{};
    std::uint64_t drainWaitStartedTick_{};
    bool drainPositionValid_{};
    bool pausedWithBuffer_{};
    // Device position and committed PCM share the same stream-relative origin.
    // Keep this ledger across Pause; reset it only with the actual client Reset.
    std::uint64_t clockSubmittedFrames_{};
    std::uint64_t clockGapTicks_{};
    bool clockPositionValid_{};
    std::atomic<std::uint64_t> clockFrequency_{};
    std::atomic<std::uint64_t> clockPosition_{};
    std::atomic<std::uint64_t> clockTarget_{};
    std::atomic<std::uint64_t> drainElapsedMs_{};
    std::atomic<LONG> clockReadResult_{S_OK};
    // Bounded wait for the Running render loop; see Open().
    DWORD renderWatchdogMs_{1000};

    HANDLE shutdownEvent_{};
    HANDLE audioEvent_{};
    HANDLE renderThread_{};
    std::atomic<bool> stopRequested_{};
    std::atomic<bool> waitingForSource_{};
    std::atomic<bool> resumeBlocked_{};
    std::atomic<bool> endOfStreamSubmitted_{};
    std::atomic<bool> endOfStreamComplete_{};
    IIntegerPcmSource* source_{};
    std::uint32_t bufferFrames_{};
    // MTA usage is a process lease, balanced by this exact cookie on any
    // control thread. It never consumes Apple's thread-local apartment ref.
    CO_MTA_USAGE_COOKIE mtaUsageCookie_{};

    std::atomic<WasapiSinkState> state_{WasapiSinkState::Closed};
    std::atomic<std::uint64_t> submittedFrames_{};
    std::atomic<std::uint64_t> submittedBuffers_{};
    std::atomic<std::uint64_t> controlledSilenceFrames_{};
    std::atomic<std::uint64_t> sourceWaits_{};
    std::atomic<std::uint64_t> underruns_{};
    std::atomic<std::uint64_t> getBufferFailures_{};
    std::atomic<std::uint64_t> releaseBufferFailures_{};
    std::atomic<LONG> lastError_{S_OK};
};

} // namespace ammod::audio_v2
