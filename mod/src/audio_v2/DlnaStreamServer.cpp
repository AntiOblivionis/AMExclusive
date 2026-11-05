#include <winsock2.h>
#include <ws2tcpip.h>

#include "DlnaStreamServer.h"

#include "DlnaLog.h"
#include "MemoryLock.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace ammod::audio_v2 {
namespace {

constexpr std::uint32_t kMaxHttpHeaderBytes = 4096;
constexpr std::uint32_t kSendChunkBytes = 65536;
constexpr std::uint32_t kMaxSessionBytes = 32u * 1024u * 1024u;

bool EnsureWinsock() {
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ready;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                          static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        result.data(), bytes, nullptr, nullptr);
    return result;
}

bool SendAll(SOCKET socket, const void* data, std::size_t bytes) {
    const auto* cursor = static_cast<const char*>(data);
    while (bytes != 0) {
        const int sent = send(socket, cursor, static_cast<int>(bytes), 0);
        if (sent <= 0) return false;
        cursor += sent;
        bytes -= static_cast<std::size_t>(sent);
    }
    return true;
}

// One HTTP/1.1 chunk: hex length, CRLF, payload, CRLF.

std::string HttpDate() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    static const char* const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buffer[64]{};
    (void)std::snprintf(buffer, sizeof(buffer), "%s, %02u %s %04u %02u:%02u:%02u GMT",
                        days[st.wDayOfWeek % 7], st.wDay, months[(st.wMonth + 11) % 12],
                        st.wYear, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::wstring CommonHeaders() {
    std::wstring headers;
    headers += L"Date: ";
    const std::string date = HttpDate();
    headers.append(date.begin(), date.end());
    headers += L"\r\n";
    // Some renderers use the Server banner to pick a compatibility profile.
    headers += L"Server: AMExclusive/0.3.0 UPnP/1.0 DLNADOC/1.50\r\n";
    return headers;
}

void WriteLE16(std::vector<BYTE>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<BYTE>(value & 0xFF));
    bytes.push_back(static_cast<BYTE>((value >> 8) & 0xFF));
}

void WriteLE32(std::vector<BYTE>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<BYTE>(value & 0xFF));
    bytes.push_back(static_cast<BYTE>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<BYTE>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<BYTE>((value >> 24) & 0xFF));
}

} // namespace

DlnaStreamSession::DlnaStreamSession(std::wstring token, PcmFormat format,
                                     DlnaStreamShape shape,
                                     std::uint32_t capacityBytes,
                                     std::wstring contentFeatures,
                                     std::uint64_t totalBytes) noexcept
    : token_(std::move(token)), format_(format), shape_(shape),
      contentFeatures_(std::move(contentFeatures)),
      totalBytes_(shape == DlnaStreamShape::WavFile
                      ? std::max<std::uint64_t>(totalBytes, kWavHeaderBytes + 4)
                      : totalBytes),
      bytesPerFrame_(static_cast<std::uint32_t>(format.channels) *
                     (static_cast<std::uint32_t>(format.containerBits) / 8u)),
      capacityBytes_(std::max<std::uint32_t>(256u * 1024u,
                                             std::min<std::uint32_t>(capacityBytes, kMaxSessionBytes))),
      ring_(capacityBytes_) {}

DlnaStreamSession::~DlnaStreamSession() = default;

std::size_t DlnaStreamSession::BufferedBytes() const noexcept {
    std::lock_guard lock(mutex_);
    return size_;
}

bool DlnaStreamSession::LockMemory() noexcept {
    if (ring_.empty()) return false;
    return LockBuffer(ring_.data(), ring_.size());
}

bool DlnaStreamSession::Append(const BYTE* data, std::size_t bytes,
                               bool endOfStream) noexcept {
    try {
        std::unique_lock lock(mutex_);
        std::size_t remaining = bytes;
        std::size_t offset = 0;
        while (remaining != 0 && !aborted_.load(std::memory_order_acquire)) {
            // Check space before waiting, then re-check after waking. Readers
            // notify spaceReady_ after advancing the global retained window;
            // the timed wait is only a fallback for abort and missed wakeups.
            const std::size_t freeBytes = capacityBytes_ - size_;
            if (freeBytes == 0) {
                spaceReady_.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
            const std::size_t take = std::min(remaining, freeBytes);
            const std::size_t tail = (head_ + size_) % capacityBytes_;
            const std::size_t first = std::min(take, capacityBytes_ - tail);
            std::memcpy(ring_.data() + tail, data + offset, first);
            if (take > first) {
                std::memcpy(ring_.data(), data + offset + first, take - first);
            }
            size_ += take;
            produced_ += take;
            offset += take;
            remaining -= take;
            bytesWritten_.fetch_add(take, std::memory_order_acq_rel);
            dataReady_.notify_all();
        }
        if (endOfStream) {
            endOfStream_.store(true, std::memory_order_release);
            dataReady_.notify_all();
        }
        return !aborted_.load(std::memory_order_acquire);
    } catch (...) {
        return false;
    }
}

bool DlnaStreamSession::ReadAt(std::uint64_t cursor, BYTE* destination,
                               std::size_t maxBytes, std::size_t& written,
                               bool& endOfStream, std::uint32_t timeoutMs) noexcept {
    written = 0;
    endOfStream = false;
    if (!destination || maxBytes == 0) return false;
    std::unique_lock lock(mutex_);
    // Only [produced_ - size_, produced_] is retained. capacityBytes_ is the
    // allocated ring size, not the valid-data size: using it here would allow
    // reads from bytes that have already been reclaimed. The bound remains
    // exclusive, so the oldest retained byte is still a valid read position.
    if (cursor > produced_ || produced_ - cursor > size_) {
        // Distinguish an unrecoverable cursor fault from a plain read timeout.
        // The HTTP caller closes this connection instead of retrying forever.
        endOfStream = true;
        return false;
    }
    dataReady_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return aborted_.load(std::memory_order_acquire) || produced_ != cursor ||
               endOfStream_.load(std::memory_order_acquire);
    });
    if (aborted_.load(std::memory_order_acquire)) return false;
    // Re-validate after the wait. The single-thread-per-Reader invariant means
    // no concurrent reclaim can move `oldest` past this cursor today, but the
    // check is cheap and prevents a uint64 underflow in `cursor - oldest` if
    // that invariant is ever relaxed (shared readers, re-attach, etc.).
    if (cursor > produced_ || produced_ - cursor > size_) {
        endOfStream = true;
        return false;
    }
    // A drained ring is not end-of-stream. The producer only raises that flag
    // once the track is genuinely over, so a cursor that has caught up with
    // `produced_` while the flag is clear is reported as a timeout and the
    // caller keeps waiting.
    if (produced_ == cursor) {
        endOfStream = endOfStream_.load(std::memory_order_acquire);
        return endOfStream;
    }
    const auto available = produced_ - cursor;
    const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(maxBytes, available));
    // Translate the absolute cursor into a ring offset. The validity check
    // above guarantees that cursor is inside the retained window.
    const std::uint64_t oldest = produced_ - size_;
    const std::size_t offset = static_cast<std::size_t>(
        (head_ + static_cast<std::size_t>(cursor - oldest)) % capacityBytes_);
    const std::size_t first = std::min(take, capacityBytes_ - offset);
    std::memcpy(destination, ring_.data() + offset, first);
    if (take > first) {
        std::memcpy(destination + first, ring_.data(), take - first);
    }
    written = take;
    bytesServed_.fetch_add(take, std::memory_order_acq_rel);
    return true;
}

void DlnaStreamSession::ReclaimLocked() noexcept {
    if (size_ == 0) return;
    // File-shaped sessions keep byte 0 addressable until a reader actually
    // asks for a byte range; see NewReaderAt.
    if (!rangeRequested_ && shape_ == DlnaStreamShape::WavFile &&
        totalBytes_ > kWavHeaderBytes) {
        return;
    }
    const std::uint64_t oldest = produced_ - size_;
    const std::uint64_t now = GetTickCount64();
    // A reader that has not advanced for kReaderStallMs is a candidate for
    // being an abandoned probe. But real renderers also legitimately read
    // ahead and then pause for seconds while they play from their own buffer,
    // so a stalled reader must only be ignored when at least one OTHER reader
    // is actively draining (the probe-starves-playback case). If every reader
    // is idle we reclaim nothing: their unread data is still owed to them and
    // discarding it would fault their cursors and cut playback.
    bool anyActive = false;
    for (const auto& entry : readers_) {
        if (now - entry.second.lastActiveTick <= kReaderStallMs) {
            anyActive = true;
            break;
        }
    }
    if (!anyActive) return;
    std::uint64_t reclaimTo = produced_;
    for (const auto& entry : readers_) {
        if (now - entry.second.lastActiveTick > kReaderStallMs) continue;
        reclaimTo = std::min(reclaimTo, entry.second.cursor);
    }
    if (reclaimTo <= oldest) return;
    const std::size_t consumed = static_cast<std::size_t>(
        std::min<std::uint64_t>(reclaimTo - oldest, size_));
    head_ = (head_ + consumed) % capacityBytes_;
    size_ -= consumed;
    // Space has actually been released; merely copying bytes for one reader is
    // not sufficient when other readers still need their independent cursors.
    spaceReady_.notify_all();
}

DlnaStreamSession::Reader::Reader(DlnaStreamSession& session, std::uint64_t readerId,
                                  std::uint64_t cursor) noexcept
    : session_(&session), readerId_(readerId), cursor_(cursor) {}

DlnaStreamSession::Reader::Reader(Reader&& other) noexcept
    : session_(other.session_), readerId_(other.readerId_), cursor_(other.cursor_) {
    other.session_ = nullptr;
    other.readerId_ = 0;
}

DlnaStreamSession::Reader& DlnaStreamSession::Reader::operator=(Reader&& other) noexcept {
    if (this != &other) {
        Detach();
        session_ = other.session_;
        readerId_ = other.readerId_;
        cursor_ = other.cursor_;
        other.session_ = nullptr;
        other.readerId_ = 0;
    }
    return *this;
}

DlnaStreamSession::Reader::~Reader() {
    Detach();
}

void DlnaStreamSession::Reader::Detach() noexcept {
    if (!session_) return;
    {
        std::lock_guard lock(session_->mutex_);
        if (readerId_ != 0) session_->readers_.erase(readerId_);
        session_->ReclaimLocked();
    }
    session_ = nullptr;
    readerId_ = 0;
}

bool DlnaStreamSession::Reader::Read(BYTE* destination, std::size_t maxBytes,
                                    std::size_t& written, bool& endOfStream,
                                    std::uint32_t timeoutMs) noexcept {
    if (!session_ || readerId_ == 0) {
        written = 0;
        // Any early-out here is unrecoverable: a moved-from or never-registered
        // Reader will not become valid on retry. Reporting eos=false used to
        // make the HTTP loop `continue` forever on a detached reader.
        endOfStream = true;
        return false;
    }
    // Refresh liveness on every poll, not only when bytes were delivered. The
    // genuinely stalled case is a renderer that stops draining the socket: our
    // HTTP thread then blocks inside SendChunk and never reaches this point, so
    // its lastActiveTick goes stale and ReclaimLocked can route around it. A
    // reader that is still polling but momentarily sees an empty ring (slow
    // producer) must NOT be mistaken for stalled, or its pending bytes would be
    // reclaimed out from under it and fault the connection.
    {
        std::lock_guard lock(session_->mutex_);
        const auto found = session_->readers_.find(readerId_);
        if (found != session_->readers_.end()) {
            found->second.lastActiveTick = GetTickCount64();
        }
    }
    const bool ok = session_->ReadAt(cursor_, destination, maxBytes, written,
                                     endOfStream, timeoutMs);
    if (written != 0) {
        cursor_ += written;
        std::lock_guard lock(session_->mutex_);
        const auto found = session_->readers_.find(readerId_);
        if (found != session_->readers_.end()) {
            found->second.cursor = cursor_;
            found->second.lastActiveTick = GetTickCount64();
        }
        session_->ReclaimLocked();
    }
    return ok;
}


DlnaStreamSession::Reader DlnaStreamSession::NewReaderAt(std::uint64_t entityOffset,
                                                         bool& reachable) noexcept {
    std::lock_guard lock(mutex_);
    // Entity offset -> payload cursor. Bytes [0, 44) are the RIFF header, which
    // the HTTP layer synthesises itself, so every offset below the header maps
    // to the first payload byte.
    // Only a WAV entity carries the synthesised 44-byte header; a FLAC file
    // shape streams its own container bytes, so offsets map one to one.
    const std::uint64_t cursor = shape_ == DlnaStreamShape::WavFile
        ? (entityOffset > kWavHeaderBytes ? entityOffset - kWavHeaderBytes : 0)
        : entityOffset;
    // A renderer that asks for a byte offset has committed to the real pull;
    // only from that moment may the ring discard the head of the stream. Until
    // then the entity must stay addressable from byte 0, because the probe
    // connections that precede it read a few tens of KB and hang up - if those
    // were allowed to reclaim, the subsequent "Range: bytes=44-" would land
    // outside the retained window and be answered with 416.
    const std::uint64_t oldest = produced_ - size_;
    if (cursor > produced_ || cursor < oldest) {
        // Already reclaimed (or beyond the producer). The renderer asked for a
        // window this live ring no longer holds; the caller answers 416 rather
        // than serving bytes from the wrong position.
        reachable = false;
        return Reader(*this, 0, cursor);
    }
    const std::uint64_t readerId = nextReaderId_++;
    try {
        readers_.emplace(readerId, ReaderState{cursor, GetTickCount64()});
    } catch (...) {
        reachable = false;
        return Reader(*this, 0, cursor);
    }
    readersCreated_.fetch_add(1, std::memory_order_release);
    reachable = true;
    ReclaimLocked();
    return Reader(*this, readerId, cursor);
}

void DlnaStreamSession::MarkRangeRequested() noexcept {
    // Flip the flag only. Reclaiming here would run against the cursors of the
    // probe connections that just drained and dropped, which would discard the
    // very head the range request is about to ask for.
    std::lock_guard lock(mutex_);
    rangeRequested_ = true;
}

void DlnaStreamSession::Abort() noexcept {
    aborted_.store(true, std::memory_order_release);
    dataReady_.notify_all();
    spaceReady_.notify_all();
}

struct DlnaStreamServer::State final {
    std::atomic<bool> stopping{false};
    std::atomic<SOCKET> listenSocket{INVALID_SOCKET};
    std::wstring bindIp;
    std::uint16_t port{};
    std::thread acceptThread;
    mutable std::mutex sessionsMutex;
    std::map<std::wstring, std::shared_ptr<DlnaStreamSession>> sessions;

    void ClientThread(SOCKET client) noexcept;
};

std::vector<BYTE> DlnaStreamServer::WavHeader(const PcmFormat& format,
                                             std::uint64_t totalBytes) {
    std::vector<BYTE> header;
    header.reserve(44);
    const std::uint16_t channels = std::max<std::uint16_t>(1, format.channels);
    const std::uint16_t containerBytes =
        static_cast<std::uint16_t>(format.containerBits / 8u);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * containerBytes);
    // The RIFF and data sizes must agree with the Content-Length the response
    // declares. AURALiC-class renderers parse this header and then re-request
    // the payload as "Range: bytes=44-", so 0xFFFFFFFF ("unknown") is rejected
    // as an unsupported format long before any audio is decoded.
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(0, totalBytes > 44 ? totalBytes - 44 : 0));
    header.insert(header.end(), {'R', 'I', 'F', 'F'});
    WriteLE32(header, 36u + dataBytes);
    header.insert(header.end(), {'W', 'A', 'V', 'E'});
    header.insert(header.end(), {'f', 'm', 't', ' '});
    WriteLE32(header, 16);
    WriteLE16(header, 1); // PCM
    WriteLE16(header, channels);
    WriteLE32(header, format.sampleRate);
    WriteLE32(header, format.sampleRate * blockAlign);
    WriteLE16(header, blockAlign);
    WriteLE16(header, format.containerBits);
    header.insert(header.end(), {'d', 'a', 't', 'a'});
    WriteLE32(header, dataBytes);
    return header;
}

std::wstring DlnaStreamServer::ContentType(const PcmFormat& format,
                                           DlnaStreamShape shape) {
    (void)format;
    // Must agree with the protocolInfo in the DIDL. Renderers advertise and
    // accept the "x-" spelling of FLAC; the registered audio/flac is dropped.
    return shape == DlnaStreamShape::FlacFile ? L"audio/x-flac" : L"audio/wav";
}

bool DlnaStreamServer::Start(const std::wstring& bindAddress, std::uint16_t& port,
                            std::wstring& error) noexcept {
    Stop();
    if (!EnsureWinsock()) {
        error = L"Winsock initialization failed";
        return false;
    }
    if (bindAddress.empty()) {
        error = L"bind address is empty";
        return false;
    }
    in_addr address{};
    const std::string narrowAddress = Narrow(bindAddress);
    if (narrowAddress.empty() || inet_pton(AF_INET, narrowAddress.c_str(), &address) != 1) {
        error = L"bind address is not IPv4";
        return false;
    }
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        error = L"socket creation failed";
        return false;
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr = address;
    endpoint.sin_port = htons(port);
    if (bind(socket, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR) {
        closesocket(socket);
        error = L"bind failed on " + bindAddress;
        return false;
    }
    if (listen(socket, 8) == SOCKET_ERROR) {
        closesocket(socket);
        error = L"listen failed";
        return false;
    }
    sockaddr_in actual{};
    int actualSize = sizeof(actual);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&actual), &actualSize) != 0) {
        closesocket(socket);
        error = L"getsockname failed";
        return false;
    }
    auto state = std::make_shared<State>();
    state->listenSocket.store(socket, std::memory_order_release);
    state->bindIp = bindAddress;
    state->port = ntohs(actual.sin_port);
    state_ = state;
    state->acceptThread = std::thread([state] {
        while (!state->stopping.load(std::memory_order_acquire)) {
            const SOCKET listenSocket = state->listenSocket.load(std::memory_order_acquire);
            if (listenSocket == INVALID_SOCKET) break;
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSocket, &readSet);
            timeval timeout{};
            timeout.tv_usec = 200000;
            const int selected = select(
                static_cast<int>(static_cast<std::intptr_t>(listenSocket) + 1),
                &readSet, nullptr, nullptr, &timeout);
            if (selected <= 0) continue;
            const SOCKET client = accept(listenSocket, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            std::thread([state, client] {
                state->ClientThread(client);
            }).detach();
        }
    });
    port = state->port;
    return true;
}

void DlnaStreamServer::Stop() noexcept {
    if (!state_) return;
    auto state = std::move(state_);
    state->stopping.store(true, std::memory_order_release);
    const auto socket = state->listenSocket.exchange(INVALID_SOCKET,
                                                     std::memory_order_acq_rel);
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
    {
        std::lock_guard lock(state->sessionsMutex);
        for (auto& entry : state->sessions) {
            if (entry.second) entry.second->Abort();
        }
        state->sessions.clear();
    }
    if (state->acceptThread.joinable()) state->acceptThread.join();
}

bool DlnaStreamServer::Running() const noexcept {
    return state_ &&
        state_->listenSocket.load(std::memory_order_acquire) != INVALID_SOCKET;
}

std::wstring DlnaStreamServer::BaseUrl() const {
    if (!state_) return {};
    return L"http://" + state_->bindIp + L":" + std::to_wstring(state_->port);
}

void DlnaStreamServer::AddSession(const std::shared_ptr<DlnaStreamSession>& session) {
    if (!state_ || !session) return;
    std::lock_guard lock(state_->sessionsMutex);
    state_->sessions[session->Token()] = session;
}

void DlnaStreamServer::RemoveSession(const std::wstring& token) {
    if (!state_) return;
    std::shared_ptr<DlnaStreamSession> removed;
    {
        std::lock_guard lock(state_->sessionsMutex);
        const auto found = state_->sessions.find(token);
        if (found != state_->sessions.end()) {
            removed = found->second;
            state_->sessions.erase(found);
        }
    }
    if (removed) removed->Abort();
}

std::shared_ptr<DlnaStreamSession> DlnaStreamServer::FindSession(
    const std::wstring& token) const {
    if (!state_) return {};
    std::lock_guard lock(state_->sessionsMutex);
    const auto found = state_->sessions.find(token);
    return found == state_->sessions.end() ? nullptr : found->second;
}

void DlnaStreamServer::State::ClientThread(SOCKET client) noexcept {
    // The producer deliberately runs above normal; the thread that actually
    // pushes bytes to the renderer deserves the same treatment, otherwise a
    // busy neighbour can delay send() even though the ring has data ready.
    (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    // Optional socket tuning, OFF by default: enabling it changed how a VEGA
    // G2.2 consumed the stream (it read ~2 s then reset). Kept behind a bitmask
    // so it can be re-tested: 1 = TCP_NODELAY, 2 = 512 KB send buffer,
    // 4 = DSCP EF.
    // Read per connection (not cached): flipping the bitmask in the ini then
    // only needs a track change, not an Apple Music restart, which is what
    // makes bit-by-bit A/B testing practical.
    int socketTuning = 0;
    {
        wchar_t publicPath[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"PUBLIC", publicPath, MAX_PATH);
        if (length != 0 && length < MAX_PATH) {
            const std::wstring ini = std::wstring(publicPath) +
                L"\\AMExclusive\\am-exclusive.ini";
            socketTuning = GetPrivateProfileIntW(L"dlna", L"socket_tuning", 0, ini.c_str());
        }
    }
    if ((socketTuning & 1) != 0) {
        int enabled = 1;
        (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    }
    // Bit 2 (512 KB SO_SNDBUF) is gone for good: at 44.1 kHz it made a VEGA
    // G2.2 reset the 206 connection after ~500 KB. Sending is left to the
    // TCP window, which is the backpressure the renderer expects.
    if ((socketTuning & 4) != 0) {
        int dscp = 0xB8; // DSCP EF (46) << 2
        (void)setsockopt(client, IPPROTO_IP, IP_TOS,
                         reinterpret_cast<const char*>(&dscp), sizeof(dscp));
    }
    std::string request;
    char buffer[512]{};
    while (request.size() < kMaxHttpHeaderBytes) {
        const int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        request.append(buffer, static_cast<std::size_t>(received));
        if (request.find("\r\n\r\n") != std::string::npos) break;
    }
    if (request.empty()) {
        closesocket(client);
        return;
    }
    const auto lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) {
        closesocket(client);
        return;
    }
    const std::string requestLine = request.substr(0, lineEnd);
    const bool headRequest = requestLine.rfind("HEAD ", 0) == 0;
    const bool getRequest = requestLine.rfind("GET ", 0) == 0;
    {
        // The first line plus the headers that decide compatibility are the
        // whole story when a renderer refuses a stream. Log them verbatim.
        std::wstring text = L"http request " + std::wstring(requestLine.begin(), requestLine.end());
        const auto rangeAt = request.find("Range:");
        const auto uaAt = request.find("User-Agent:");
        const auto ctAt = request.find("Content-Type:");
        if (rangeAt != std::string::npos) {
            const auto end = request.find("\r\n", rangeAt);
            text += L" | " + std::wstring(request.begin() + rangeAt,
                                          request.begin() + (end == std::string::npos ? request.size() : end));
        }
        if (uaAt != std::string::npos) {
            const auto end = request.find("\r\n", uaAt);
            text += L" | " + std::wstring(request.begin() + uaAt,
                                          request.begin() + (end == std::string::npos ? request.size() : end));
        }
        if (ctAt != std::string::npos) {
            const auto end = request.find("\r\n", ctAt);
            text += L" | " + std::wstring(request.begin() + ctAt,
                                          request.begin() + (end == std::string::npos ? request.size() : end));
        }
        LogDlnaLine(text);
    }
    if (!headRequest && !getRequest) {
        const char* response = "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n";
        (void)SendAll(client, response, std::strlen(response));
        closesocket(client);
        return;
    }
    const auto pathStart = requestLine.find('/');
    const auto pathEnd = requestLine.find(' ', pathStart);
    if (pathStart == std::string::npos || pathEnd == std::string::npos) {
        closesocket(client);
        return;
    }
    std::string path = requestLine.substr(pathStart, pathEnd - pathStart);
    const auto marker = path.find("/ame/");
    if (marker == std::string::npos) {
        const char* response = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        (void)SendAll(client, response, std::strlen(response));
        closesocket(client);
        return;
    }
    const auto tokenStart = marker + 5;
    const auto tokenEnd = path.find_first_of("/.?", tokenStart);
    const std::string tokenBytes = path.substr(tokenStart, tokenEnd - tokenStart);
    const std::wstring token(tokenBytes.begin(), tokenBytes.end());

    std::shared_ptr<DlnaStreamSession> session;
    {
        std::lock_guard lock(sessionsMutex);
        const auto found = sessions.find(token);
        if (found != sessions.end()) session = found->second;
    }
    if (!session) {
        // The renderer asked for a token this process no longer owns. Usually a
        // stale URL from a previous stream, or a token truncated by the
        // renderer, so the exact token matters for diagnosis.
        LogDlnaLine(L"http 410 unknown token=" + token +
                    L" path=" + std::wstring(path.begin(), path.end()));
        const char* response = "HTTP/1.1 410 Gone\r\nConnection: close\r\n\r\n";
        (void)SendAll(client, response, std::strlen(response));
        closesocket(client);
        return;
    }

    const std::wstring contentType = session->Mime().empty()
        ? DlnaStreamServer::ContentType(session->Format(), session->Shape())
        : session->Mime();
    // ---------------------------------------------------------------------
    // File-shaped response: WAV with a declared total length and byte ranges.
    //
    // AURALiC-class renderers (LightningPlayer) do not accept an unbounded
    // body. Measured against a VEGA G2.2: a chunked, length-less WAV is read
    // for a few tens of KB and dropped ("unsupported format"), while the same
    // bytes served with Content-Length are fetched once as a probe and then
    // re-requested as "Range: bytes=44-" -- the payload after the RIFF header
    // -- and played in real time as it arrives. The total therefore has to be
    // declared up front even though the producer is still generating it.
    // ---------------------------------------------------------------------
    // Only the file shape declares a length. FLAC and the live WAV shape are
    // deliberately unbounded and answer with a chunked body instead.
    const bool fileShape =
        (session->Shape() == DlnaStreamShape::WavFile ||
         session->Shape() == DlnaStreamShape::FlacFile) &&
        session->TotalBytes() > 0;
    if (fileShape) {
        const std::uint64_t totalBytes = session->TotalBytes();
        std::uint64_t startAbs = 0;
        std::uint64_t endAbs = totalBytes - 1;
        bool hasRange = false;
        {
            const auto rangeAt = request.find("Range:");
            if (rangeAt != std::string::npos) {
                const auto lineEnd = request.find("\r\n", rangeAt);
                std::string line = request.substr(
                    rangeAt + 6,
                    (lineEnd == std::string::npos ? request.size() : lineEnd) - rangeAt - 6);
                const auto bytesAt = line.find("bytes=");
                if (bytesAt != std::string::npos) {
                    line = line.substr(bytesAt + 6);
                    const auto dash = line.find('-');
                    if (dash != std::string::npos) {
                        const std::string first = line.substr(0, dash);
                        const std::string second = line.substr(dash + 1);
                        std::uint64_t parsedStart = 0;
                        std::uint64_t parsedEnd = totalBytes - 1;
                        bool ok = true;
                        if (!first.empty()) {
                            try {
                                parsedStart = std::stoull(first);
                            } catch (...) {
                                ok = false;
                            }
                        } else {
                            // Suffix range ("bytes=-N"): the renderer wants the
                            // tail. A live stream cannot serve that.
                            ok = false;
                        }
                        if (ok && !second.empty()) {
                            try {
                                parsedEnd = std::stoull(second);
                            } catch (...) {
                                ok = false;
                            }
                        }
                        if (ok) {
                            startAbs = parsedStart;
                            endAbs = std::min(parsedEnd, totalBytes - 1);
                            hasRange = true;
                        }
                    }
                }
            }
        }
        if (startAbs >= totalBytes || endAbs < startAbs) {
            const auto response = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                std::to_string(totalBytes) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)SendAll(client, response.data(), response.size());
            closesocket(client);
            return;
        }
        // Attach before writing the response: an offset the ring has already
        // reclaimed has to become a 416, and that is only possible while the
        // status line is still ours to choose.
        if (hasRange) session->MarkRangeRequested();
        bool reachable = false;
        auto reader = session->NewReaderAt(startAbs, reachable);
        if (!reachable) {
            const auto response = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                std::to_string(totalBytes) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)SendAll(client, response.data(), response.size());
            closesocket(client);
            return;
        }
        const std::uint64_t length = endAbs - startAbs + 1;
        std::wstring headerW = hasRange ? L"HTTP/1.1 206 Partial Content\r\n"
                                        : L"HTTP/1.1 200 OK\r\n";
        headerW += L"Content-Type: " + contentType + L"\r\n";
        headerW += L"Content-Length: " + std::to_wstring(length) + L"\r\n";
        if (hasRange) {
            headerW += L"Content-Range: bytes " + std::to_wstring(startAbs) + L"-" +
                std::to_wstring(endAbs) + L"/" + std::to_wstring(totalBytes) + L"\r\n";
        }
        headerW += L"Accept-Ranges: bytes\r\n";
        headerW += CommonHeaders();
        headerW += L"transferMode.dlna.org: Streaming\r\n";
        headerW += L"contentFeatures.dlna.org: " + session->ContentFeatures() + L"\r\n";
        headerW += L"Connection: close\r\n\r\n";
        const std::string header = Narrow(headerW);
        LogDlnaLine(L"http " + std::wstring(hasRange ? L"206" : L"200") + L" token=" + token +
                    L" type=" + contentType + L" range=" + std::to_wstring(startAbs) + L"-" +
                    std::to_wstring(endAbs) + L"/" + std::to_wstring(totalBytes));
        if (!SendAll(client, header.data(), header.size())) {
            closesocket(client);
            return;
        }
        if (headRequest) {
            closesocket(client);
            return;
        }
        std::uint64_t position = startAbs;
        std::uint64_t remaining = length;
        std::uint64_t bytesSent = 0;
        std::uint64_t chunksSent = 0;
        const std::uint64_t streamStartTick = GetTickCount64();
        std::uint64_t firstChunkAt = 0;
        if (session->Shape() == DlnaStreamShape::WavFile &&
            position < DlnaStreamSession::kWavHeaderBytes) {
            const auto wavHeader = DlnaStreamServer::WavHeader(session->Format(), totalBytes);
            const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(
                DlnaStreamSession::kWavHeaderBytes - position, remaining));
            if (!SendAll(client, wavHeader.data() + position, take)) {
                closesocket(client);
                return;
            }
            position += take;
            remaining -= take;
            bytesSent += take;
            ++chunksSent;
            firstChunkAt = GetTickCount64();
        }
        std::vector<BYTE> body(kSendChunkBytes);
        bool endedByStream = false;
        while (!stopping.load(std::memory_order_acquire)) {
            if (remaining == 0) break;
            std::size_t written = 0;
            bool eos = false;
            const auto want = static_cast<std::size_t>(
                std::min<std::uint64_t>(body.size(), remaining));
            if (!reader.Read(body.data(), want, written, eos, 500)) {
                if (session->Aborted()) break;
                if (eos) {
                    // End of track before the declared length. The renderer has
                    // already been told how long the entity is, so the honest
                    // move is to stop feeding it and leave the socket alone:
                    // the next track arrives as a new URL and replaces this
                    // one. Closing here would surface as a network error.
                    endedByStream = true;
                }
                continue;
            }
            if (written != 0) {
                if (!SendAll(client, body.data(), written)) {
                    const int wsa = WSAGetLastError();
                    LogDlnaLine(L"http send failed token=" + token + L" afterBytes=" +
                                std::to_wstring(bytesSent) + L" wsa=" +
                                std::to_wstring(wsa));
                    break;
                }
                if (firstChunkAt == 0) firstChunkAt = GetTickCount64();
                position += written;
                remaining -= written;
                bytesSent += written;
                ++chunksSent;
            }
        }
        LogDlnaLine(L"http stream closed token=" + token + L" bytes=" +
                    std::to_wstring(bytesSent) + L" firstChunkMs=" +
                    std::to_wstring(firstChunkAt ? firstChunkAt - streamStartTick : 0) +
                    L" reason=" + (session->Aborted() ? L"abort" :
                                    endedByStream ? L"eos" :
                                    remaining == 0 ? L"complete" : L"client"));
        closesocket(client);
        return;
    }
    // Only file shapes exist, so a request that matched neither is
    // malformed; close instead of falling through.
    closesocket(client);
}

DlnaStreamServer::~DlnaStreamServer() {
    Stop();
}

} // namespace ammod::audio_v2
