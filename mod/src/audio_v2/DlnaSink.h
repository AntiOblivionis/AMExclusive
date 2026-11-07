#pragma once

#include "AudioSink.h"
#include "DlnaControlClient.h"
#include "DlnaStreamServer.h"
#include "FlacEncoder.h"
#include "IntegerPcmSource.h"
#include "PcmTypes.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
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
                   bool endOfStream, std::vector<BYTE>& output) noexcept;
    bool SendStreamToDevice() noexcept;
    // Emits the encoder's final partial block. A FLAC stream that ends without
    // this loses up to one block (4096 frames) of audio, and a decoder that
    // checks frame continuity may reject the tail outright.
    void FlushEncoderTail() noexcept;
    // Returns false if the producer did not exit within gracefulMs.
    bool JoinProducer(std::uint32_t gracefulMs = 0) noexcept;
    void RecordError(HRESULT error) noexcept;
    // Re-reads the SMTC track length into streamSeconds_ (declared length,
    // headroom included) and trackDurationMsExact_ (the exact boundary used
    // for auto-advance). When previousTitle is non-empty and the snapshot
    // still carries that title, waits briefly for the watcher to publish the
    // next track's metadata.
    void RefreshTrackDurationFromSmtc(const std::wstring& previousTitle,
                                      bool fastTrackChange = false) noexcept;
    // Automatic track rollover for gapless auto-advance. Apple Music keeps
    // feeding PCM through the same converter at a track boundary, so no graph
    // rebuild ever re-opens this sink. Once the exact track length has been
    // produced, the producer calls this to seal the current session, create
    // the next track's session and queue its URL behind the playing one.
    // Runs on the producer thread; returns false on producerStop_ or when the
    // next session could not be established.
    bool AdvanceToNextTrack() noexcept;
    // Queues the freshly created session behind the track the renderer is
    // currently playing (OpenHome Playlist.Insert with AfterId), leaving the
    // current track untouched so the renderer can gaplessly roll into the
    // next URL. Falls back to the full Stop+DeleteAll+Insert+Play sequence
    // for plain AVTransport renderers or when the insert fails. A watchdog
    // (pendingPlayDeadlineTick_) escalates to an explicit Play if the
    // renderer never starts pulling the queued track.
    bool AppendNextTrackToDevice() noexcept;

    AudioSinkOpenParams params_{};
    DlnaTarget target_{};
    PcmFormat format_{};
    DlnaStreamShape shape_{DlnaStreamShape::WavFile};
    // Derived: true for both WAV shapes (file and live), false for FLAC.
    bool wav_{true};
    // FLAC frame encoder; unused unless shape_ is FlacFile.
    FlacEncoder flac_{};
    bool converted_{false};
    std::uint32_t chunkFrames_{1024};
    std::uint32_t bufferMs_{1500};
    // Ring capacity in seconds, independent of buffer_ms: buffer_ms is what the
    // renderer keeps, this is what we can hold while it briefly stops pulling.
    std::uint32_t ringSeconds_{8};
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
    // Exact length of the current track in milliseconds as reported by SMTC,
    // WITHOUT the headroom that streamSeconds_ adds for the declared entity.
    // This is the auto-advance cut point: cutting at the declared (padded)
    // length instead would hand the first seconds of every next track to the
    // previous session, and the offset would grow by the padding on every
    // track. 0 means "no exact value" and falls back to streamSeconds_.
    std::uint32_t trackDurationMsExact_{0};
    // Auto-advance bookkeeping, producer thread only. Frames belonging to the
    // current track (exact length) and how many have been appended so far.
    // Reaching the exact count without a source EOF means Apple Music has
    // gaplessly advanced into the next track.
    std::uint64_t trackFramesExact_{0};
    std::uint64_t sessionFramesProduced_{0};
    // Last title the SMTC watcher reported, used to wait for fresh metadata
    // when rolling over to the next track.
    std::wstring lastTrackTitle_;
    // OpenHome playlist id of the most recently inserted track; auto-advance
    // queues the next track right after it. 0 = unknown (legacy Write path).
    std::uint32_t lastOpenHomeId_{0};
    // Watchdog for the gentle queueing path: when nonzero, the producer
    // checks after this tick whether the renderer started pulling the queued
    // session and forces a Play if it did not.
    std::uint64_t pendingPlayDeadlineTick_{0};
    // Verbatim GetProtocolInfo Sink value, kept only for diagnostics.
    std::wstring sinkProtocolInfoText_;
    // Network byte order for bare LPCM. On by default (RFC 3551/3190); the
    // [dlna] pcm_byte_order=little escape hatch exists for renderers that
    // violate the spec.
    bool bigEndianPcm_{true};
    // Live-mode alias token: the stable URL component the playlist sees. The
    // per-track sessions rotate underneath it (see SetAliasSession).
    std::wstring liveAlias_;
    // Live track changes switch the playlist item via SeekId; the previous
    // session keeps its connection briefly so the renderer can finish moving.
    // Entries are aborted by the producer loop after the deadline.
    std::vector<std::pair<std::shared_ptr<DlnaStreamSession>, std::uint64_t>> retiredSessions_;
    std::vector<std::pair<PcmFormat, bool>> formatCandidates_;
    std::size_t formatIndex_{};

    DlnaControlClient control_{};
    DlnaStreamServer server_{};
    std::shared_ptr<DlnaStreamSession> session_{};

    IIntegerPcmSource* source_{};
    std::thread producer_;
    std::mutex producerExitMutex_;
    std::condition_variable producerExitCv_;
    bool producerExited_{false};
    std::atomic<bool> producerStop_{false};
    std::atomic<bool> producerPaused_{false};
    std::atomic<bool> startedOnce_{false};
    std::atomic<bool> waitingForSource_{false};
    std::atomic<bool> endOfStreamSubmitted_{false};
    // Tick at which the producer sealed the stream. EndOfStreamComplete must
    // not depend forever on the renderer draining the ring: LightningPlayer
    // answers a resume Range with 416 by giving up, and the runtime then waits
    // for a completion that never comes (blocking the next format's rebind).
    std::atomic<std::uint64_t> endOfStreamTick_{0};
    std::atomic<AudioSinkState> state_{AudioSinkState::Closed};
    std::atomic<HRESULT> lastError_{S_OK};
    std::atomic<std::uint64_t> submittedFrames_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> controlFailures_{0};
};

} // namespace ammod::audio_v2
