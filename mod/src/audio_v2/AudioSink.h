#pragma once

#include "IntegerPcmSource.h"
#include "PcmTypes.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>

namespace ammod::audio_v2 {

enum class AudioSinkKind : std::uint8_t {
    Wasapi = 0,
    Dlna = 1,
};

enum class AudioSinkState : std::uint8_t {
    Closed,
    Open,
    Running,
    Faulted,
};

// How the DLNA stream is shaped on the wire. Both shapes are *file* shaped:
// a declared Content-Length plus byte ranges, because that is the only thing
// every renderer tested here accepts. They differ only in the container.
enum class DlnaStreamShape : std::uint8_t {
    // RIFF/WAVE with a declared length, Accept-Ranges and 206 replies.
    WavFile = 0,
    // FLAC with a declared length. Verbatim subframes give every frame an
    // identical size, so the total byte count is computable before the first
    // sample exists. No compression (bandwidth within 1% of WAV), but it plays
    // on renderers that refuse WAV while still demanding a known length.
    FlacFile = 1,
};

// True when the wire body is a RIFF/WAVE container.
inline bool DlnaStreamShapeUsesWavContainer(DlnaStreamShape shape) noexcept {
    return shape == DlnaStreamShape::WavFile;
}

inline const wchar_t* DlnaStreamShapeName(DlnaStreamShape shape) noexcept {
    return shape == DlnaStreamShape::FlacFile ? L"flac-file" : L"wav";
}

inline DlnaStreamShape DlnaStreamShapeFromText(const wchar_t* text) noexcept {
    if (text && (_wcsicmp(text, L"flac-file") == 0 || _wcsicmp(text, L"flac") == 0)) {
        return DlnaStreamShape::FlacFile;
    }
    return DlnaStreamShape::WavFile;
}

// Lifecycle notifications are diagnostic-only. They are invoked on the thread
// that performs the corresponding sink operation.
enum class AudioSinkLifecycleEvent : std::uint8_t {
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

using AudioSinkLifecycleCallback = void (*)(
    void* context, AudioSinkLifecycleEvent event, HRESULT result) noexcept;

// The focused DLNA/OpenHome target selected by the user. Service URLs are
// filled by the broker (SSDP + description) or fetched from Location by the
// control client when a field is empty.
struct DlnaTarget final {
    std::wstring udn;
    std::wstring friendlyName;
    std::wstring location;
    std::wstring avTransportControl;
    std::wstring renderingControl;
    std::wstring connectionManager;
    std::wstring openHomePlaylist;
    std::wstring openHomeTransport;
    std::wstring openHomeTime;
    std::wstring openHomeVolume;
    std::wstring openHomeInfo;
    bool openHome{};
    bool avTransport{};
    bool nextUri{};
    bool seek{};
    bool volume{};
};

// Sink-facing open parameters. WasapiExclusiveSink maps these onto its
// existing WasapiSinkConfig without changing any of its behavior.
struct AudioSinkOpenParams final {
    PcmFormat format{};
    std::array<PcmFormat, kMaxFormatCandidates> formatCandidates{};
    std::uint32_t formatCandidateCount{};
    std::wstring endpointId;
    std::uint32_t periodFrames{};
    std::uint32_t alignmentRetries{2};
    bool allowResampling{};
    bool primeWithSilence{};
    HANDLE sourceReadyEvent{};
    void* lifecycleContext{};
    AudioSinkLifecycleCallback onLifecycle{};
    std::uint32_t streamBufferMs{1500};
    // Declared entity length for the file shapes, in seconds. 0 means "derive
    // it from the ini / SMTC the way production does".
    std::uint32_t streamSeconds{};
    DlnaStreamShape streamShape{DlnaStreamShape::WavFile};
    DlnaTarget dlna{};
};

// Generic sink diagnostics. The first block intentionally preserves the names
// used by the existing WASAPI diagnostics so hook logging keeps its shape.
struct AudioSinkStats final {
    AudioSinkKind kind{AudioSinkKind::Wasapi};
    AudioSinkState state{AudioSinkState::Closed};
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
    std::uint64_t clientStarts{};
    std::uint64_t startsBeforeFirstBuffer{};
    // DLNA-specific counters.
    std::uint64_t bytesServed{};
    std::uint64_t bufferedFrames{};
    std::uint64_t droppedFrames{};
    std::uint64_t controlFailures{};
    std::uint64_t transportRestarts{};
};

// Existing code and the hardware drain probe refer to the WASAPI names.
using WasapiSinkState = AudioSinkState;
using WasapiSinkStats = AudioSinkStats;
using WasapiSinkLifecycleEvent = AudioSinkLifecycleEvent;
using WasapiSinkLifecycleCallback = AudioSinkLifecycleCallback;

// A playback endpoint. Implementations must never block the decoder callback:
// Start/Pause/Stop/Close run on the coordinator worker; the render/network
// threads they own are strictly internal.
class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    virtual AudioSinkKind Kind() const noexcept = 0;
    virtual HRESULT Open(const AudioSinkOpenParams& params) noexcept = 0;
    virtual HRESULT Start(IIntegerPcmSource& source) noexcept = 0;
    virtual void Pause() noexcept = 0;
    virtual void Stop() noexcept = 0;
    virtual void Close() noexcept = 0;

    virtual AudioSinkState State() const noexcept = 0;
    virtual AudioSinkStats Stats() const noexcept = 0;
    virtual const PcmFormat& Format() const noexcept = 0;
    virtual std::uint32_t BufferFrames() const noexcept = 0;

    virtual bool WaitingForSource() const noexcept = 0;
    virtual bool EndOfStreamSubmitted() const noexcept = 0;
    virtual bool EndOfStreamComplete() const noexcept = 0;
    virtual void SetResumeBlocked(bool blocked) noexcept = 0;
};

} // namespace ammod::audio_v2
