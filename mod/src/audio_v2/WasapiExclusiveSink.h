#pragma once

#include "PcmTypes.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace ammod::audio_v2 {

// A decoder-side queue can be temporarily empty while Apple waits for a
// lossless network segment. This is a recoverable wait, not a format or
// endpoint failure. The sink stops its exclusive client without acquiring
// another endpoint buffer, keeps native output suppressed, and resumes the
// same generation only after a complete P0 buffer is available.
inline constexpr HRESULT kAudioSourceWouldBlock = static_cast<HRESULT>(0x88770001L);

class IIntegerPcmSource {
public:
    virtual ~IIntegerPcmSource() = default;

    // A sink must not acquire an endpoint buffer unless the source can fill
    // the complete period, or has reached an explicit end-of-stream tail.
    // This keeps a network wait from becoming fabricated output samples.
    virtual bool CanProvide(std::uint32_t capacityFrames) const noexcept {
        return capacityFrames != 0;
    }

    // Approximate number of source frames that can be consumed immediately.
    // This is diagnostic only; the sink never drives the Apple graph from the
    // endpoint render thread.
    virtual std::size_t AvailableFrames() const noexcept { return SIZE_MAX; }

    // The sink always asks for a complete endpoint buffer. A source may return
    // fewer frames only when endOfStream is true. It may return
    // kAudioSourceWouldBlock when Apple is waiting for the next lossless
    // segment; the sink keeps the generation alive in that case.
    virtual HRESULT Fill(std::int32_t* destination,
                         std::uint32_t capacityFrames,
                         std::uint32_t& writtenFrames,
                         bool& endOfStream) noexcept = 0;

};

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
    std::wstring endpointId;
    std::uint32_t periodFrames{};
    std::uint32_t alignmentRetries{2};
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
    std::uint32_t BufferFrames() const noexcept { return bufferFrames_; }
    const PcmFormat& Format() const noexcept { return config_.format; }
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
