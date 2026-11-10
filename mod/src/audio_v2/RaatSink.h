#pragma once

#include "AudioSink.h"
#include "RaatProtocol.h"
#include "RaatScripts.h"
#include "RaatTypes.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ammod::audio_v2 {

// Channel map carried by CMD_CHMAP on the audio channel: one bit per channel,
// so 2 channels -> 0x3 and 6 channels -> 0x3F. The device passes it straight to
// stream:write_chmap; without a CHMAP command the map stays 0.
inline std::uint32_t ChannelMapFor(std::uint32_t channels) noexcept {
    if (channels == 0) return 0;
    if (channels >= 32) return 0xFFFFFFFFu;
    return (1u << channels) - 1u;
}

// Offline check of the audio-channel command sequence. Returns 0 on success.
// Exposed so am_exclusive_raat_selftest can catch a regression that would
// otherwise only show up as silence on real hardware.
int AudioCommandSelfTest();

// Roon RAAT sender sink. It owns the whole sender-side session:
//   SOOD resolve -> 9200 wake (if needed) -> LL control -> Lua injection ->
//   setup -> audio TCP -> stream -> prefill -> start -> UDP clock responder.
// The pump thread converts canonical Q31 PCM into 16/24/32-bit little-endian
// RAAT DATA frames and keeps the device buffer fed.
class RaatSink final : public IAudioSink {
public:
    RaatSink() noexcept = default;
    ~RaatSink() override;

    RaatSink(const RaatSink&) = delete;
    RaatSink& operator=(const RaatSink&) = delete;

    AudioSinkKind Kind() const noexcept override { return AudioSinkKind::Raat; }
    HRESULT Open(const AudioSinkOpenParams& params) noexcept override;
    HRESULT Start(IIntegerPcmSource& source) noexcept override;
    void Pause() noexcept override;
    void Stop() noexcept override;
    void Close() noexcept override;

    AudioSinkState State() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AudioSinkStats Stats() const noexcept override;
    const PcmFormat& Format() const noexcept override { return format_; }
    std::uint32_t BufferFrames() const noexcept override { return blockFrames_; }
    bool WaitingForSource() const noexcept override {
        return waitingForSource_.load(std::memory_order_acquire);
    }
    bool EndOfStreamSubmitted() const noexcept override {
        return endOfStreamSubmitted_.load(std::memory_order_acquire);
    }
    bool EndOfStreamComplete() const noexcept override {
        return endOfStreamComplete_.load(std::memory_order_acquire);
    }
    void SetResumeBlocked(bool blocked) noexcept override { (void)blocked; }

private:
    bool EnsureSession() noexcept;
    bool OpenControl(std::wstring& error) noexcept;
    bool LoadScripts(std::wstring& error) noexcept;
    bool LoadOneScript(const raat::ScriptRecord& record, int variant,
                       std::wstring& error) noexcept;
    bool SendInfo(std::wstring& error) noexcept;
    bool SendSetup(std::wstring& error) noexcept;
    bool OpenAudio(std::wstring& error) noexcept;
    bool SendStream(std::wstring& error) noexcept;
    bool Prefill() noexcept;
    bool SendStart(bool resume) noexcept;
    bool SendSync() noexcept;
    void MaybeSendKeepalive() noexcept;
    void PumpMain() noexcept;
    void ClockMain() noexcept;
    bool SendData(const std::int32_t* samples, std::uint32_t frames) noexcept;
    bool SendSilence(std::uint32_t frames) noexcept;
    bool SendAudioCommands(std::uint64_t startSample) noexcept;
    void FinishEndOfStream() noexcept;
    void StopClock() noexcept;
    void RecordError(HRESULT error) noexcept;
    bool ReadSettingInt(int& value, const wchar_t* key, int fallback) const noexcept;
    bool CheckSupportedFormat(const std::string& supported) const noexcept;

    AudioSinkOpenParams params_{};
    RaatTarget target_{};
    PcmFormat format_{};
    std::uint16_t wireBits_{};
    std::uint32_t blockFrames_{1024};
    std::uint32_t lookaheadMs_{1000};
    std::uint32_t startLeadMs_{1000};
    std::uint32_t streamId_{1};

    IIntegerPcmSource* source_{};
    HANDLE sourceReadyEvent_{};

    raat::ControlChannel control_;
    raat::TcpConnection audio_;
    std::mutex controlMutex_;
    std::mutex audioMutex_;
    std::wstring localAddress_;

    SOCKET clockSocket_{INVALID_SOCKET};
    std::uint16_t clockPort_{};
    std::uint16_t audioPort_{};
    std::uint16_t deviceClockPort_{};   // renderer's own clock port (setup reply)
    std::thread clockThread_;
    std::atomic<bool> clockStop_{false};
    // Clock-exchange counters. `sync` only returns Success after the renderer
    // has collected five good replies to the probes it sends to clockPort_, so
    // these two numbers say whether the failure is "probes never arrived" or
    // "we answered but the renderer did not accept them".
    // Real-time pacing for the audio pump. Apple Music decodes more than a
    // second ahead, so an unpaced pump empties the decoder queue into the
    // renderer in one burst: the renderer's buffer overruns (discarding audio)
    // and then starves when the decode buffer is momentarily empty.
    std::uint64_t paceStartTick_{0};
    std::uint64_t paceStartSample_{0};
    std::uint64_t paceLastLogTick_{0};
    std::uint64_t lastKeepaliveTick_{0};
    std::atomic<std::uint32_t> clockProbesIn_{0};
    std::atomic<std::uint32_t> clockRepliesOut_{0};

    std::thread pumpThread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> startedOnce_{false};
    std::atomic<bool> waitingForSource_{false};
    std::atomic<bool> endOfStreamSubmitted_{false};
    std::atomic<bool> endOfStreamComplete_{false};
    std::atomic<AudioSinkState> state_{AudioSinkState::Closed};
    std::atomic<HRESULT> lastError_{S_OK};

    std::atomic<std::uint64_t> submittedFrames_{0};
    std::atomic<std::uint64_t> droppedFrames_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> controlFailures_{0};
    std::uint64_t sampleIndex_{0};
    std::uint64_t lastSendTick_{0};
    std::vector<std::int32_t> block_;
    std::vector<std::uint8_t> pcm_;
    std::vector<std::uint8_t> frame_;
};

} // namespace ammod::audio_v2
