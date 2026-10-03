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
    void RecordError(HRESULT error) noexcept;
    void NotifyLifecycle(WasapiSinkLifecycleEvent event, HRESULT result) noexcept;
    void ReleaseInterfaces() noexcept;

    WasapiSinkConfig config_{};
    WAVEFORMATEXTENSIBLE waveFormat_{};
    std::vector<std::int32_t> canonicalBuffer_;

    IMMDeviceEnumerator* enumerator_{};
    IMMDevice* device_{};
    IAudioClient* client_{};
    IAudioRenderClient* render_{};

    HANDLE shutdownEvent_{};
    HANDLE audioEvent_{};
    HANDLE renderThread_{};
    std::atomic<bool> stopRequested_{};
    std::atomic<bool> waitingForSource_{};
    std::atomic<bool> resumeBlocked_{};
    std::atomic<bool> endOfStreamSubmitted_{};
    IIntegerPcmSource* source_{};
    std::uint32_t bufferFrames_{};
    bool comInitialized_{};

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
