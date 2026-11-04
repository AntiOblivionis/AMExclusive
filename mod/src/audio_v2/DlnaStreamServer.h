#pragma once

#include "AudioSink.h"
#include "PcmTypes.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ammod::audio_v2 {

// A bounded byte ring fed by the sink producer and drained by one or more
// HTTP connections at the renderer's pull rate. Append may block the producer
// thread (never the Apple callback) when nobody is draining.
//
// Each connection owns an independent read cursor into the same ring. This is
// deliberate: renderers routinely open a probe connection and then a real
// playback connection (and sometimes keep both alive). Giving every connection
// its own cursor means a probe cannot steal audio from the connection that is
// actually playing, and neither one is terminated when the other appears.
class DlnaStreamSession final {
public:
    DlnaStreamSession(std::wstring token, PcmFormat format, DlnaStreamShape shape,
                      std::uint32_t capacityBytes, std::wstring contentFeatures,
                      std::uint64_t totalBytes) noexcept;
    ~DlnaStreamSession();

    DlnaStreamSession(const DlnaStreamSession&) = delete;
    DlnaStreamSession& operator=(const DlnaStreamSession&) = delete;

    const std::wstring& Token() const noexcept { return token_; }
    const PcmFormat& Format() const noexcept { return format_; }
    DlnaStreamShape Shape() const noexcept { return shape_; }
    // Content-Type actually written on the wire. Empty means "derive it from
    // the shape"; the sink overrides it when the renderer advertises a
    // different spelling (audio/x-wav instead of audio/wav).
    const std::wstring& Mime() const noexcept { return mime_; }
    void SetMime(std::wstring mime) noexcept { mime_ = std::move(mime); }
    // True when the body is a WAV container (file-shaped or unbounded). Both
    // FLAC shapes carry no RIFF header.
    bool Wav() const noexcept { return DlnaStreamShapeUsesWavContainer(shape_); }
    const std::wstring& ContentFeatures() const noexcept { return contentFeatures_; }

    // Declared length of the whole entity, WAV header included. AURALiC-class
    // renderers refuse a body with no total: they probe once, then re-request
    // the payload as "Range: bytes=44-" and stop if the answer carries no
    // Content-Range total. This value is what the response advertises, so it
    // must be fixed before the first response is written.
    std::uint64_t TotalBytes() const noexcept { return totalBytes_; }
    static constexpr std::uint64_t kWavHeaderBytes = 44;

    bool Append(const BYTE* data, std::size_t bytes, bool endOfStream) noexcept;

    // Pin the ring's pages so a reader thread cannot fault on them. Best
    // effort: the stream works unchanged when the lock is refused.
    bool LockMemory() noexcept;

    // An independent reader over this session's ring. Constructed by the HTTP
    // server per accepted GET and released when that connection ends.
    class Reader final {
    public:
        Reader(Reader&&) noexcept;
        Reader& operator=(Reader&&) noexcept;
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;
        ~Reader();

        // Copies up to maxBytes. Returns false on abort or timeout with no data.
        bool Read(BYTE* destination, std::size_t maxBytes, std::size_t& written,
                  bool& endOfStream, std::uint32_t timeoutMs) noexcept;

    private:
        friend class DlnaStreamSession;
        Reader(DlnaStreamSession& session, std::uint64_t readerId,
               std::uint64_t cursor) noexcept;
        void Detach() noexcept;

        DlnaStreamSession* session_{};
        std::uint64_t readerId_{};
        std::uint64_t cursor_{};
    };

    // A reader positioned at an absolute entity offset (WAV header included).
    // Renderers re-request the payload with "Range: bytes=44-" after probing,
    // so the server must be able to attach at an offset that is behind the
    // producer. `reachable` is false when that offset has already been
    // reclaimed; the caller then answers 416 instead of serving wrong bytes.
    Reader NewReaderAt(std::uint64_t entityOffset, bool& reachable) noexcept;
    // Any byte-range request means the renderer has committed to the real
    // pull, so the ring may start discarding the head of the stream. Without
    // this a renderer that asks for "bytes=0-" would stall the producer
    // forever once the ring filled.
    void MarkRangeRequested() noexcept;
    bool Aborted() const noexcept { return aborted_.load(std::memory_order_acquire); }
    void Abort() noexcept;

    std::size_t BufferedBytes() const noexcept;
    std::uint64_t BytesServed() const noexcept {
        return bytesServed_.load(std::memory_order_acquire);
    }
    std::uint64_t BytesWritten() const noexcept {
        return bytesWritten_.load(std::memory_order_acquire);
    }
    // Total readers ever created for this session, including ones that have
    // since detached. The DlnaSink startup gate uses this to tell "the
    // renderer connected" apart from "the renderer is actively draining",
    // which BytesServed conflates and which made WAV-mode attempts fail even
    // though the RIFF header had already been delivered.
    std::uint64_t ReadersCreated() const noexcept {
        return readersCreated_.load(std::memory_order_acquire);
    }
    bool EndOfStream() const noexcept {
        return endOfStream_.load(std::memory_order_acquire);
    }
    bool Finished() const noexcept {
        return endOfStream_.load(std::memory_order_acquire) && BufferedBytes() == 0;
    }
    std::uint32_t BytesPerFrame() const noexcept { return bytesPerFrame_; }
    std::uint32_t CapacityBytes() const noexcept { return capacityBytes_; }

private:
    bool ReadAt(std::uint64_t cursor, BYTE* destination, std::size_t maxBytes,
                std::size_t& written, bool& endOfStream, std::uint32_t timeoutMs) noexcept;
    // mutex_ must be held. Bytes are reclaimed only after every active reader
    // has advanced past them; with no readers, live data need not be retained.
    void ReclaimLocked() noexcept;

    // Per-reader tracking. `lastActiveTick` is refreshed on every successful
    // Read so ReclaimLocked can tell a live playback cursor from a stalled
    // probe. A probe that never drains would otherwise pin `reclaimTo` at its
    // initial cursor forever, the ring would fill, and the producer would
    // block even though the real reader is hungry.
    struct ReaderState {
        std::uint64_t cursor{};
        std::uint64_t lastActiveTick{};
    };
    // Readers idle longer than this are excluded from the reclaim minimum.
    // Two seconds is well above any legitimate renderer read interval and
    // well below the time it takes a stalled probe to fill the ring.
    static constexpr std::uint64_t kReaderStallMs = 2000;

    std::wstring token_;
    PcmFormat format_{};
    DlnaStreamShape shape_{DlnaStreamShape::WavFile};
    std::wstring mime_;
    std::wstring contentFeatures_;
    std::uint64_t totalBytes_{};
    std::uint32_t bytesPerFrame_{};
    std::uint32_t capacityBytes_{};
    std::vector<BYTE> ring_;
    std::size_t head_{};
    std::size_t size_{};
    mutable std::mutex mutex_;
    std::condition_variable dataReady_;
    std::condition_variable spaceReady_;
    std::atomic<bool> aborted_{false};
    std::atomic<bool> endOfStream_{false};
    std::atomic<std::uint64_t> bytesServed_{0};
    std::atomic<std::uint64_t> bytesWritten_{0};
    std::atomic<std::uint64_t> readersCreated_{0};
    // Absolute byte offset one past the newest byte in the ring. Each active
    // reader ID maps to its absolute read cursor; the oldest retained byte is
    // produced_ - size_. A reader's unread data cannot be reclaimed.
    std::uint64_t produced_{0};
    // Set once a renderer issues a byte-range request; until then a
    // file-shaped session must not discard the head of the stream.
    bool rangeRequested_{false};
    std::map<std::uint64_t, ReaderState> readers_;
    std::uint64_t nextReaderId_{1};
};

// Minimal HTTP/1.1 streaming server. It intentionally supports only GET/HEAD
// for /ame/<session-token> and closes the connection at end of stream.
class DlnaStreamServer final {
public:
    DlnaStreamServer() = default;
    ~DlnaStreamServer();

    DlnaStreamServer(const DlnaStreamServer&) = delete;
    DlnaStreamServer& operator=(const DlnaStreamServer&) = delete;

    bool Start(const std::wstring& bindAddress, std::uint16_t& port,
               std::wstring& error) noexcept;
    void Stop() noexcept;

    std::wstring BaseUrl() const;
    bool Running() const noexcept;

    void AddSession(const std::shared_ptr<DlnaStreamSession>& session);
    void RemoveSession(const std::wstring& token);
    std::shared_ptr<DlnaStreamSession> FindSession(const std::wstring& token) const;

    static std::vector<BYTE> WavHeader(const PcmFormat& format,
                                       std::uint64_t totalBytes);
    static std::wstring ContentType(const PcmFormat& format, DlnaStreamShape shape);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace ammod::audio_v2
