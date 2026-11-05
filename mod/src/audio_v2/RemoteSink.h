#pragma once

// Hook-side sink for "HiFi player" mode: instead of opening the endpoint in
// the Apple Music process, it feeds a cross-process ring that
// am-exclusive-audiohost.exe drains into WASAPI exclusive or ASIO.
//
// If the host is missing or refuses the request, this sink falls back to the
// in-process sink so playback still works (and reports the state honestly).

#include "AudioSink.h"
#include "SharedAudioRing.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace ammod::audio_v2 {

enum class HostMessageType : std::uint32_t {
    Hello = 1,
    HelloAck = 2,
    Open = 3,
    OpenResult = 4,
    Start = 5,
    StartResult = 6,
    Pause = 7,
    Stop = 8,
    Close = 9,
    Shutdown = 10,
};

// Fixed-size control message. Both sides include this header, so the layout is
// identical; PcmFormat is POD and contains no pointers.
struct HostMessage final {
    std::uint32_t type{};
    std::uint32_t size{sizeof(HostMessage)};
    HRESULT result{S_OK};
    PcmFormat format{};
    std::uint32_t periodFrames{};
    std::uint32_t bufferMs{20};
    std::uint32_t ringFrames{};
    std::uint32_t alignmentRetries{3};
    std::uint64_t generation{};
    wchar_t asioClsid[64]{};
    wchar_t asioName[128]{};
    wchar_t endpointId[256]{};
};

inline constexpr wchar_t kHostPipePrefix[] = L"\\\\.\\pipe\\AMEAudioHost_";
inline constexpr wchar_t kHostRingPrefix[] = L"Local\\AMEAudioRing_";
inline constexpr wchar_t kHostDataEventPrefix[] = L"Local\\AMEAudioData_";
inline constexpr wchar_t kHostSpaceEventPrefix[] = L"Local\\AMEAudioSpace_";

inline std::wstring HostObjectName(const wchar_t* prefix, std::uint32_t sessionId) {
    return std::wstring(prefix) + std::to_wstring(sessionId);
}

class RemoteSink final : public IAudioSink {
public:
    RemoteSink() noexcept;
    ~RemoteSink() override;

    RemoteSink(const RemoteSink&) = delete;
    RemoteSink& operator=(const RemoteSink&) = delete;

    AudioSinkKind Kind() const noexcept override { return AudioSinkKind::Remote; }

    HRESULT Open(const AudioSinkOpenParams& params) noexcept override;
    HRESULT Start(IIntegerPcmSource& source) noexcept override;
    void Pause() noexcept override;
    void Stop() noexcept override;
    void Close() noexcept override;

    AudioSinkState State() const noexcept override { return state_.load(std::memory_order_acquire); }
    AudioSinkStats Stats() const noexcept override { return stats_; }
    const PcmFormat& Format() const noexcept override { return params_.format; }
    std::uint32_t BufferFrames() const noexcept override { return params_.periodFrames; }
    std::size_t BufferedFrames() const noexcept override {
        return static_cast<std::size_t>(ring_.BufferedFrames());
    }

    bool WaitingForSource() const noexcept override { return false; }
    bool EndOfStreamSubmitted() const noexcept override { return eosSubmitted_.load(std::memory_order_acquire); }
    bool EndOfStreamComplete() const noexcept override { return eosComplete_.load(std::memory_order_acquire); }
    void SetResumeBlocked(bool blocked) noexcept override {
        resumeBlocked_.store(blocked, std::memory_order_release);
    }

    bool UsingFallback() const noexcept { return fallback_ != nullptr; }

private:
    static DWORD WINAPI PumpThreadThunk(void* context) noexcept;
    DWORD PumpThreadMain() noexcept;

    bool Connect() noexcept;
    bool Send(const HostMessage& message) noexcept;
    bool Receive(HostMessage& message) noexcept;
    bool Request(HostMessage& message) noexcept;
    HRESULT OpenFallback(const AudioSinkOpenParams& params) noexcept;
    void Disconnect() noexcept;

    AudioSinkOpenParams params_{};
    IIntegerPcmSource* source_{};
    std::unique_ptr<IAudioSink> fallback_;

    HANDLE pipe_{INVALID_HANDLE_VALUE};
    HANDLE ringMapping_{};
    void* ringView_{};
    HANDLE dataEvent_{};
    HANDLE spaceEvent_{};
    HANDLE pumpThread_{};
    HANDLE pumpStopEvent_{};

    SharedRingProducer ring_{};
    std::atomic<bool> running_{};
    std::atomic<bool> stopPump_{};
    std::atomic<bool> resumeBlocked_{};
    std::atomic<bool> eosSubmitted_{};
    std::atomic<bool> eosComplete_{};
    std::atomic<AudioSinkState> state_{AudioSinkState::Closed};
    AudioSinkStats stats_{};
    std::uint32_t sessionId_{};
};

} // namespace ammod::audio_v2
