#pragma once

#include "AudioSink.h"
#include "DlnaControlClient.h"
#include "DlnaStreamServer.h"
#include "IntegerPcmSource.h"
#include "PcmTypes.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ammod::audio_v2 {

// Network renderer sink. The producer thread pulls canonical integer PCM from
// the coordinator's QueuedPcmSource/ResampledPcmSource and packs it into
// WAV/LPCM bytes; a minimal HTTP server serves those bytes to the renderer on
// its own pull schedule. No decoder callback performs network IO.
class DlnaSink final : public IAudioSink {
public:
    DlnaSink() noexcept = default;
    ~DlnaSink() override;

    DlnaSink(const DlnaSink&) = delete;
    DlnaSink& operator=(const DlnaSink&) = delete;

    AudioSinkKind Kind() const noexcept override { return AudioSinkKind::Dlna; }
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
    std::uint32_t BufferFrames() const noexcept override { return chunkFrames_; }
    bool WaitingForSource() const noexcept override {
        return waitingForSource_.load(std::memory_order_acquire);
    }
    bool EndOfStreamSubmitted() const noexcept override {
        return endOfStreamSubmitted_.load(std::memory_order_acquire);
    }
    bool EndOfStreamComplete() const noexcept override;
    void SetResumeBlocked(bool blocked) noexcept override { (void)blocked; }

private:
    bool BuildFormatCandidates(const std::wstring& sinkProtocolInfo) noexcept;
    bool SetupStream() noexcept;
    void CloseStream() noexcept;
    bool TryNextFormat() noexcept;
    bool WaitForFirstGet(std::uint32_t timeoutMs) noexcept;
    std::wstring AdvertiseAddress(const std::wstring& deviceHost) const noexcept;
    void ProducerMain() noexcept;
    bool PackChunk(const std::int32_t* samples, std::uint32_t frames,
                   bool endOfStream, std::vector<BYTE>& output) const noexcept;
    bool SendStreamToDevice() noexcept;
    void JoinProducer() noexcept;
    void RecordError(HRESULT error) noexcept;

    AudioSinkOpenParams params_{};
    DlnaTarget target_{};
    PcmFormat format_{};
    bool wav_{true};
    bool converted_{false};
    std::uint32_t chunkFrames_{1024};
    std::uint32_t bufferMs_{1500};
    std::wstring advertiseIp_;
    std::wstring streamUrl_;
    std::wstring protocolInfo_;
    std::wstring contentFeatures_;
    // Declared length of the served WAV entity, header included. Renderers of
    // the AURALiC/LightningPlayer class refuse a body without a total, so this
    // is fixed when the session is created and never changes for that track.
    std::uint64_t totalBytes_{};
    // How long the declared entity claims to be. Apple Music streams in real
    // time, so the exact byte count of a track is not known in advance; the
    // value comes from [dlna] stream_seconds (or the track duration once the
    // broker can supply it) and only affects the declared length.
    std::uint32_t streamSeconds_{600};
    // Verbatim GetProtocolInfo Sink value, kept only for diagnostics.
    std::wstring sinkProtocolInfoText_;
    // Network byte order for bare LPCM. On by default (RFC 3551/3190); the
    // [dlna] pcm_byte_order=little escape hatch exists for renderers that
    // violate the spec.
    bool bigEndianPcm_{true};
    std::vector<std::pair<PcmFormat, bool>> formatCandidates_;
    std::size_t formatIndex_{};

    DlnaControlClient control_{};
    DlnaStreamServer server_{};
    std::shared_ptr<DlnaStreamSession> session_{};

    IIntegerPcmSource* source_{};
    std::thread producer_;
    std::atomic<bool> producerStop_{false};
    std::atomic<bool> producerPaused_{false};
    std::atomic<bool> startedOnce_{false};
    std::atomic<bool> waitingForSource_{false};
    std::atomic<bool> endOfStreamSubmitted_{false};
    std::atomic<AudioSinkState> state_{AudioSinkState::Closed};
    std::atomic<HRESULT> lastError_{S_OK};
    std::atomic<std::uint64_t> submittedFrames_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> controlFailures_{0};
};

} // namespace ammod::audio_v2
