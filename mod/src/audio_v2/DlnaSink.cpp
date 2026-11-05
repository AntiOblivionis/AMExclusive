#include <winsock2.h>
#include <ws2tcpip.h>

#include "DlnaSink.h"

#include "../AudioErrors.h"
#include "../UpnpXml.h"
#include "DlnaLog.h"
#include "DlnaResolve.h"

#include <windows.h>
#include <oleauto.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace ammod::audio_v2 {
namespace {

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

std::wstring GuidToken() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return L"ame-stream";
    wchar_t text[64]{};
    if (!StringFromGUID2(guid, text, static_cast<int>(std::size(text)))) {
        return L"ame-stream";
    }
    std::wstring token(text);
    token.erase(std::remove(token.begin(), token.end(), L'{'), token.end());
    token.erase(std::remove(token.begin(), token.end(), L'}'), token.end());
    token.erase(std::remove(token.begin(), token.end(), L'-'), token.end());
    return token;
}

std::wstring FeaturesOf(const std::wstring& protocolInfo) {
    // ProtocolInfo is <network>:<operation>:<content-type>:<additional-info>.
    // The additional-info field is a ';' separated list of DLNA.ORG_* key=value
    // pairs. contentFeatures.dlna.org carries OP/CI/FLAGS/PS/WPS but NOT PN —
    // PN belongs only in the protocolInfo advertised via the DIDL <res> element.
    // Emitting PN in contentFeatures is tolerated by most renderers but rejected
    // by strict DLNA parsers, so strip it here.
    std::size_t position = 0;
    unsigned colons = 0;
    while (position < protocolInfo.size()) {
        if (protocolInfo[position] == L':') {
            if (++colons == 3) break;
        }
        ++position;
    }
    if (colons < 3) return {};
    const auto additional = protocolInfo.substr(position + 1);
    std::wstring result;
    std::size_t start = 0;
    while (start <= additional.size()) {
        const auto end = additional.find(L';', start);
        const auto token = additional.substr(
            start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!token.empty() && token.rfind(L"DLNA.ORG_PN=", 0) != 0) {
            if (!result.empty()) result += L';';
            result += token;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return result;
}

void DlnaLog(const std::wstring& text) {
    LogDlnaLine(text);
}

// am-exclusive.ini lives next to the installed broker, which is
// %PUBLIC%\AMExclusive. The hook runs inside Apple Music, so it cannot use
// its own module path to find the file the installer wrote.
std::wstring IniPath() {
    wchar_t publicDir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"PUBLIC", publicDir,
                                static_cast<DWORD>(std::size(publicDir))) && publicDir[0]) {
        return (std::filesystem::path(publicDir) / L"AMExclusive" /
                L"am-exclusive.ini").wstring();
    }
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, modulePath, MAX_PATH)) return L"am-exclusive.ini";
    return (std::filesystem::path(modulePath).parent_path() /
            L"am-exclusive.ini").wstring();
}

std::wstring RouteAddress(const std::wstring& host) {
    const std::string narrowHost = Narrow(host);
    if (narrowHost.empty()) return {};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* resolved = nullptr;
    if (getaddrinfo(narrowHost.c_str(), "1900", &hints, &resolved) != 0 || !resolved) {
        return {};
    }
    const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == INVALID_SOCKET) {
        freeaddrinfo(resolved);
        return {};
    }
    std::wstring address;
    if (connect(probe, resolved->ai_addr, static_cast<int>(resolved->ai_addrlen)) == 0) {
        sockaddr_in local{};
        int size = sizeof(local);
        if (getsockname(probe, reinterpret_cast<sockaddr*>(&local), &size) == 0) {
            char text[64]{};
            if (inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text))) {
                address.assign(text, text + std::strlen(text));
            }
        }
    }
    closesocket(probe);
    freeaddrinfo(resolved);
    return address;
}

// Process-wide reader for the "what is Apple Music playing" helper.
//
// The served WAV has to declare its total length before the first byte is
// written, and a live capture has no idea how long the track is. SMTC does,
// but querying it costs a WinRT round trip that must not happen on the audio
// path, so am_exclusive_media_control.exe runs one watcher process per
// Apple Music process and streams "TRACK<TAB>durationMs<TAB>title" lines.
// The sink only ever reads a cached snapshot.
class TrackMonitor final {
public:
    struct Info final {
        bool valid{};
        std::uint32_t durationMs{};
        std::wstring title;
        std::uint64_t stampTick{};
    };

    static TrackMonitor& Instance() noexcept {
        // Intentionally leaked: the reader thread may still be running during
        // process teardown, and a static destructor would race it.
        static TrackMonitor* instance = new TrackMonitor();
        return *instance;
    }

    // Spawns the helper on first use. Safe to call from every Open.
    void EnsureStarted() noexcept {
        std::call_once(started_, [this] { Start(); });
    }

    // Returns the most recent snapshot, waiting up to waitMs for the first one
    // so the very first track of a session still gets its real duration.
    Info Snapshot(std::uint32_t waitMs) const noexcept {
        const auto deadline = GetTickCount64() + waitMs;
        for (;;) {
            {
                std::lock_guard lock(mutex_);
                if (haveLine_) return info_;
            }
            if (GetTickCount64() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::lock_guard lock(mutex_);
        return info_;
    }

private:
    TrackMonitor() = default;

    void Start() noexcept {
        // The helper ships next to the broker (and next to the hook when the
        // hook is loaded from the install directory). Look in the module
        // directory first so a development build finds its own helper, then
        // fall back to the directory of the ini the hook already reads.
        std::vector<std::filesystem::path> candidates;
        wchar_t modulePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH)) {
            candidates.push_back(std::filesystem::path(modulePath).parent_path() /
                                 L"am_exclusive_media_control.exe");
        }
        candidates.push_back(std::filesystem::path(IniPath()).parent_path() /
                             L"am_exclusive_media_control.exe");
        std::filesystem::path helper;
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate)) {
                helper = candidate;
                break;
            }
        }
        if (helper.empty()) return;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &attributes, 0)) return;
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = writeEnd;
        startup.hStdError = writeEnd;
        startup.hStdInput = nullptr;
        PROCESS_INFORMATION process{};
        std::wstring command = L"\"" + helper.wstring() + L"\" watch";
        const BOOL created = CreateProcessW(helper.c_str(), command.data(), nullptr,
                                            nullptr, TRUE, CREATE_NO_WINDOW,
                                            nullptr, nullptr, &startup, &process);
        CloseHandle(writeEnd);
        if (!created) {
            CloseHandle(readEnd);
            return;
        }
        CloseHandle(process.hThread);
        process_ = process.hProcess;
        // Kill the watcher when this process dies, so an Apple Music crash does
        // not leave a helper polling SMTC forever.
        if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                        &limits, sizeof(limits))) {
                AssignProcessToJobObject(job, process.hProcess);
            }
            job_ = job;
        }
        reader_ = std::thread([this, readEnd] { ReaderMain(readEnd); });
    }

    void ReaderMain(HANDLE pipe) noexcept {
        std::string pending;
        char buffer[512]{};
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
            pending.append(buffer, read);
            for (;;) {
                const auto newline = pending.find('\n');
                if (newline == std::string::npos) break;
                ParseLine(pending.substr(0, newline));
                pending.erase(0, newline + 1);
            }
        }
        CloseHandle(pipe);
    }

    void ParseLine(std::string line) noexcept {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto first = line.find('\t');
        if (first == std::string::npos || line.rfind("TRACK\t", 0) != 0) return;
        const auto second = line.find('\t', first + 1);
        const std::string durationText = line.substr(
            first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
        std::uint32_t durationMs = 0;
        try {
            durationMs = static_cast<std::uint32_t>(std::stoul(durationText));
        } catch (...) {
            return;
        }
        std::wstring title;
        if (second != std::string::npos) {
            const std::string tail = line.substr(second + 1);
            const int chars = MultiByteToWideChar(CP_UTF8, 0, tail.data(),
                                                  static_cast<int>(tail.size()), nullptr, 0);
            if (chars > 0) {
                title.resize(static_cast<std::size_t>(chars));
                MultiByteToWideChar(CP_UTF8, 0, tail.data(),
                                    static_cast<int>(tail.size()), title.data(), chars);
            }
        }
        std::lock_guard lock(mutex_);
        info_.durationMs = durationMs;
        info_.title = std::move(title);
        info_.stampTick = GetTickCount64();
        info_.valid = true;
        haveLine_ = true;
    }

    std::once_flag started_;
    mutable std::mutex mutex_;
    Info info_{};
    bool haveLine_{};
    HANDLE process_{};
    HANDLE job_{};
    std::thread reader_;
};

} // namespace

DlnaSink::~DlnaSink() {
    Close();
}

void DlnaSink::RecordError(HRESULT error) noexcept {
    lastError_.store(FAILED(error) ? error : E_FAIL, std::memory_order_release);
    state_.store(AudioSinkState::Faulted, std::memory_order_release);
}

std::wstring DlnaSink::AdvertiseAddress(const std::wstring& deviceHost) const noexcept {
    return RouteAddress(deviceHost);
}

bool DlnaSink::BuildFormatCandidates(const std::wstring& sinkProtocolInfo) noexcept {
    struct Candidate final {
        std::uint32_t rate;
        std::uint16_t bits;
    };
    const auto sourceRate = params_.format.sampleRate;
    const auto sourceBits = EffectiveSourceValidBits(params_.format);
    const auto preferredBits = static_cast<std::uint16_t>(
        sourceBits == 16 ? 16 : (sourceBits >= 24 ? 24 : 16));
    std::vector<Candidate> candidates;
    auto add = [&candidates](std::uint32_t rate, std::uint16_t bits) {
        if (rate < 8000 || rate > 192000 || bits == 0 || bits > 24) return;
        for (const auto& existing : candidates) {
            if (existing.rate == rate && existing.bits == bits) return;
        }
        candidates.push_back({rate, bits});
    };
    // Source rate and source precision first: this is the bit-perfect path.
    add(sourceRate, preferredBits);
    if (preferredBits == 16) add(sourceRate, 24);
    else add(sourceRate, 16);
    if (params_.allowResampling) {
        for (const auto rate : {192000u, 176400u, 96000u, 88200u, 48000u, 44100u}) {
            if (rate == sourceRate) continue;
            add(rate, preferredBits);
            if (preferredBits == 16) add(rate, 24);
            else add(rate, 16);
        }
    }
    formatCandidates_.clear();
    // A renderer that answers GetProtocolInfo with an empty Sink has told us
    // nothing. Treating that as "no raw PCM support" would silently force the
    // WAV path forever, which is exactly what a strict parser rejects. An
    // unknown sink therefore admits both paths, and the LPCM-first ordering
    // below decides which is tried first.
    const bool sinkUnknown = sinkProtocolInfo.empty();
    // Rank every candidate twice: bare LPCM (audio/L16, audio/L24) first, WAV
    // as the fallback. WAV needs a RIFF container whose length is unknown for
    // a live stream, which is exactly the shape many renderers mishandle, so it
    // is only offered when the device declares no raw PCM support.
    for (const bool preferWav : {false, true}) {
        for (const auto& candidate : candidates) {
            const bool wavAllowed = sinkUnknown ||
                DlnaControlClient::ProtocolAllows(sinkProtocolInfo, true, candidate.rate,
                                                  candidate.bits, 2);
            // candidate.bits is already constrained to [1,24] by add() above,
            // so no extra depth guard is needed here.
            const bool pcmAllowed = sinkUnknown ||
                DlnaControlClient::ProtocolAllows(sinkProtocolInfo, false, candidate.rate,
                                                  candidate.bits, 2);
            const bool allowed = preferWav ? wavAllowed : pcmAllowed;
            if (!allowed) continue;
            PcmFormat format = params_.format;
            format.sampleRate = candidate.rate;
            format.validBits = candidate.bits;
            format.containerBits = candidate.bits;
            format.encoding = PcmEncoding::SignedInteger;
            format.interleaved = true;
            if (!IsValidEndpointFormat(format, true)) continue;
            formatCandidates_.emplace_back(format, preferWav);
        }
    }
    if (formatCandidates_.empty()) return false;
    formatIndex_ = 0;
    format_ = formatCandidates_.front().first;
    wav_ = DlnaStreamShapeUsesWavContainer(shape_);
    converted_ = RequiresPcmConversion(params_.format, format_);
    return true;
}

bool DlnaSink::SetupStream() noexcept {
    // Drop any prior session but keep the HTTP server running across format
    // retries. Restarting the server on every retry forced the renderer to
    // re-resolve a new random port and left stale connections behind, which
    // is exactly the shape that made attempt N+1 fail even when attempt N had
    // the renderer connected.
    CloseStream();
    if (!server_.Running()) {
        const auto host = DlnaControlClient::HostFromLocation(target_.location);
        advertiseIp_ = AdvertiseAddress(host);
        if (advertiseIp_.empty()) return false;
        std::wstring error;
        std::uint16_t port = 0;
        if (!server_.Start(advertiseIp_, port, error)) return false;
    }
    chunkFrames_ = std::min<std::uint32_t>(
        std::max<std::uint32_t>(format_.sampleRate / 50u, 256u), 4096u);
    if (shape_ == DlnaStreamShape::FlacFile) {
        // Constant-size frames keep the declared length exact.
        const std::uint64_t samples =
            shape_ == DlnaStreamShape::FlacFile
                ? static_cast<std::uint64_t>(format_.sampleRate) *
                      static_cast<std::uint64_t>(std::max<std::uint32_t>(streamSeconds_, 5))
                : 0;
        flac_.Reset(format_.sampleRate, format_.channels, format_.containerBits,
                    shape_ == DlnaStreamShape::FlacFile, samples);
    }
    // Renderers disagree about the WAV MIME spelling and several only accept
    // the one they advertise. Pick theirs when they name exactly one.
    std::wstring wireMime;
    if (shape_ == DlnaStreamShape::WavFile && !sinkProtocolInfoText_.empty()) {
        const bool plain = sinkProtocolInfoText_.find(L"audio/wav") != std::wstring::npos;
        const bool xWav = sinkProtocolInfoText_.find(L"audio/x-wav") != std::wstring::npos;
        const bool wave = sinkProtocolInfoText_.find(L"audio/wave") != std::wstring::npos;
        if (!plain && xWav) wireMime = L"audio/x-wav";
        else if (!plain && wave) wireMime = L"audio/wave";
    }
    protocolInfo_ = DlnaControlClient::ProtocolInfoFor(shape_, format_, converted_);
    contentFeatures_ = FeaturesOf(protocolInfo_);
    // DLNA's contentFeatures normally opens with a profile name. WAV has no
    // registered profile, but strict renderers still expect the field, so the
    // widely used "WAV" spelling is added when the protocolInfo carries none.
    if (shape_ == DlnaStreamShape::WavFile &&
        contentFeatures_.find(L"DLNA.ORG_PN=") == std::wstring::npos) {
        contentFeatures_ = L"DLNA.ORG_PN=WAV;" + contentFeatures_;
    }
    // Echo the capability decision once per stream so a failing renderer can be
    // diagnosed from the log alone: raw PCM vs WAV fallback, and which
    // protocolInfo entries the device actually advertised.
    DlnaLog(L"format decided wav=" + std::to_wstring(wav_) + L" rate=" +
            std::to_wstring(format_.sampleRate) + L" bits=" +
            std::to_wstring(format_.containerBits) + L" converted=" +
            std::to_wstring(converted_) + L" sinkUnknown=" +
            std::to_wstring(sinkProtocolInfoText_.empty() ? 1 : 0) +
            L" byteOrder=" + std::wstring(wav_ ? L"little" :
                                          bigEndianPcm_ ? L"big" : L"little") +
            L" protocolInfo=" + protocolInfo_ +
            L" sinkProtocolInfo=" + sinkProtocolInfoText_);
    const auto bytesPerFrame = static_cast<std::uint64_t>(format_.channels) *
        (format_.containerBits / 8u);
    const auto bytesPerSecond = static_cast<std::uint64_t>(format_.sampleRate) * bytesPerFrame;
    // The entity length has to be announced before the first byte is written,
    // so it is derived from the configured stream length rather than from what
    // the producer eventually emits. Overshooting is safe: the renderer simply
    // stops at the end of the real audio and waits for the next track's URL.
    // Undershooting would truncate the song, so the value is a ceiling.
    if (shape_ == DlnaStreamShape::FlacFile) {
        // Verbatim frames are fixed size, so the exact entity length is known
        // before a single sample exists. That is what lets a live capture be
        // announced as a normal file with Content-Length and byte ranges.
        const std::uint64_t samples =
            static_cast<std::uint64_t>(format_.sampleRate) *
            static_cast<std::uint64_t>(std::max<std::uint32_t>(streamSeconds_, 5));
        totalBytes_ = FlacEncoder::VerbatimSize(format_.sampleRate, format_.channels,
                                                format_.containerBits, samples);
    } else {
        totalBytes_ = DlnaStreamSession::kWavHeaderBytes +
            bytesPerSecond * static_cast<std::uint64_t>(
                std::max<std::uint32_t>(streamSeconds_, 5));
        // Keep the 32-bit RIFF data size representable.
        totalBytes_ = std::min<std::uint64_t>(totalBytes_, 0x7FFF0000ull);
    }
    const auto capacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        std::max<std::uint64_t>(bytesPerSecond * (bufferMs_ + 3000u) / 1000u,
                                1024u * 1024u),
        32u * 1024u * 1024u));
    const auto token = GuidToken();
    if (!wireMime.empty()) {
        DlnaLog(L"wav mime follows the renderer: " + wireMime);
    }
    session_ = std::make_shared<DlnaStreamSession>(token, format_, shape_, capacity,
                                                   contentFeatures_, totalBytes_);
    if (!wireMime.empty()) session_->SetMime(wireMime);
    server_.AddSession(session_);
    // The HTTP reader thread and the producer both walk this ring; pinning it
    // keeps either of them from taking a page fault mid-transfer.
    DlnaLog(L"stream ring=" + std::to_wstring(capacity / 1024u) + L"KB locked=" +
            std::to_wstring(session_->LockMemory() ? 1 : 0));
    streamUrl_ = server_.BaseUrl() + L"/ame/" + token +
        (shape_ == DlnaStreamShape::FlacFile ? L".flac" : L".wav");
    return true;
}

void DlnaSink::CloseStream() noexcept {
    // Session-only teardown. The HTTP server stays up so format retries can
    // reuse the same bound port; Close() is responsible for the full stop.
    if (session_) {
        server_.RemoveSession(session_->Token());
        session_.reset();
    }
}

bool DlnaSink::TryNextFormat() noexcept {
    if (formatIndex_ + 1 >= formatCandidates_.size()) return false;
    ++formatIndex_;
    format_ = formatCandidates_[formatIndex_].first;
    wav_ = DlnaStreamShapeUsesWavContainer(shape_);
    converted_ = RequiresPcmConversion(params_.format, format_);
    return SetupStream();
}

bool DlnaSink::WaitForFirstGet(std::uint32_t timeoutMs) noexcept {
    if (!session_) return false;
    const auto deadline = GetTickCount64() + timeoutMs;
    // Gate on "a reader was created" rather than "bytes were served". The old
    // BytesServed check required the whole producer -> ring -> reader chain to
    // deliver within the timeout, which made WAV-mode attempts fail even though
    // the renderer had connected and the RIFF header had already been sent
    // (SendChunk does not touch bytesServed_). ReadersCreated is bumped in
    // NewReader the moment an HTTP handler accepts a GET.
    while (GetTickCount64() < deadline) {
        if (session_->ReadersCreated() != 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return session_->ReadersCreated() != 0;
}

HRESULT DlnaSink::Open(const AudioSinkOpenParams& params) noexcept {
    Close();
    if (!IsValidFormat(params.format)) return E_INVALIDARG;
    // [dlna] pcm_byte_order=little is a compatibility escape hatch for
    // renderers that ignore RFC 3551/3190. Anything else keeps big-endian.
    {
        wchar_t value[16]{};
        GetPrivateProfileStringW(L"dlna", L"pcm_byte_order", L"big", value,
                                 static_cast<DWORD>(std::size(value)),
                                 IniPath().c_str());
        bigEndianPcm_ = _wcsicmp(value, L"little") != 0;
    }
    // Declared length of the WAV entity. Apple Music delivers PCM in real time,
    // so the byte count of the track is unknown when the response headers have
    // to be written; this is the ceiling the renderer is told. Too small
    // truncates the song, too large only leaves the progress bar short.
    {
        wchar_t value[16]{};
        GetPrivateProfileStringW(L"dlna", L"stream_seconds", L"600", value,
                                 static_cast<DWORD>(std::size(value)),
                                 IniPath().c_str());
        const int seconds = _wtoi(value);
        streamSeconds_ = static_cast<std::uint32_t>(
            std::min(std::max(seconds, 30), 7200));
    }
    // Prefer the real track length over the configured guess. The declared
    // entity length is what the renderer prints as the track duration, so a
    // guess is either too short (truncates the song) or too long (progress bar
    // never reaches the end). SMTC knows the real value; the watcher process
    // caches it and this read never blocks the audio path.
    {
        wchar_t value[16]{};
        GetPrivateProfileStringW(L"dlna", L"follow_track_duration", L"1", value,
                                 static_cast<DWORD>(std::size(value)),
                                 IniPath().c_str());
        if (_wtoi(value) != 0) {
            auto& monitor = TrackMonitor::Instance();
            monitor.EnsureStarted();
            const auto info = monitor.Snapshot(900);
            const bool fresh = info.valid &&
                GetTickCount64() - info.stampTick < 15000;
            if (fresh && info.durationMs >= 10000 &&
                info.durationMs <= 4ull * 3600ull * 1000ull) {
                // A few seconds of headroom: Apple's timeline is the decoded
                // length, and stopping short would cut the tail off.
                streamSeconds_ = info.durationMs / 1000u + 3u;
                DlnaLog(L"track duration from SMTC ms=" +
                        std::to_wstring(info.durationMs) + L" title=" + info.title);
            } else {
                DlnaLog(L"track duration unavailable, using stream_seconds=" +
                        std::to_wstring(streamSeconds_));
            }
        }
    }
    // An explicit value (self test) overrides both the ini guess and SMTC.
    if (params.streamSeconds != 0) {
        streamSeconds_ = params.streamSeconds;
    }
    if (!params.dlna.avTransportControl.empty() || !params.dlna.openHomePlaylist.empty() ||
        !params.dlna.location.empty()) {
        params_ = params;
        target_ = params.dlna;
    } else {
        return ammod::audio::kDlnaDeviceUnavailable;
    }
    // Up to ten seconds: network jitter absorbs better, at the cost of start /
    // seek latency. The ring itself is capped at 32 MB, which covers 192 kHz.
    bufferMs_ = std::min<std::uint32_t>(std::max<std::uint32_t>(params.streamBufferMs, 300u), 10000u);
    // Wire shape: wav (default, unchanged), flac, or live. Read here so a
    // change takes effect on the next track without restarting Apple Music.
    // params.streamShape wins when the caller sets it explicitly (the self
    // test does); otherwise the persisted ini decides, which is the production
    // path since the broker does not carry the shape over IPC.
    shape_ = params.streamShape;
    if (shape_ == DlnaStreamShape::WavFile) {
        wchar_t value[16]{};
        GetPrivateProfileStringW(L"dlna", L"stream_format", L"wav", value,
                                 static_cast<DWORD>(std::size(value)),
                                 IniPath().c_str());
        shape_ = DlnaStreamShapeFromText(value);
    }
    std::wstring error;
    // A renderer that is booting, waking, or momentarily busy can fail the
    // first description fetch. Retry briefly instead of giving up silently,
    // and log every failure: an unlogged Open failure presents to the user as
    // "no sound at all" with nothing in the DLNA log to explain it.
    bool described = false;
    for (int attempt = 0; attempt < 4 && !described; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(400));
        described = control_.LoadDescription(target_, error);
        if (!described) {
            DlnaLog(L"open LoadDescription failed attempt=" + std::to_wstring(attempt) +
                    L" device=" + target_.friendlyName + L" location=" + target_.location +
                    L" reason=" + error);
        }
    }
    if (!described && !target_.udn.empty()) {
        // The UDN is the only stable identity. Renderers of the AURALiC /
        // LightningPlayer class re-bind their UPnP HTTP port every few minutes,
        // so the location cached in am-exclusive.ini is frequently stale while
        // nothing else about the device changed. Re-resolve it from a live
        // M-SEARCH instead of retrying an address that no longer listens, then
        // retry the description fetch against the current URL.
        std::wstring resolved;
        if (ResolveLocationByUdn(target_.udn, 1500, resolved) &&
            _wcsicmp(resolved.c_str(), target_.location.c_str()) != 0) {
            DlnaLog(L"open re-resolved device=" + target_.friendlyName +
                    L" stale=" + target_.location + L" current=" + resolved);
            target_.location = resolved;
            for (int attempt = 0; attempt < 2 && !described; ++attempt) {
                if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(300));
                described = control_.LoadDescription(target_, error);
                if (!described) {
                    DlnaLog(L"open LoadDescription failed after re-resolve attempt=" +
                            std::to_wstring(attempt) + L" location=" + target_.location +
                            L" reason=" + error);
                }
            }
        } else {
            DlnaLog(L"open re-resolve found no new location for udn=" + target_.udn);
        }
    }
    if (!described) {
        return ammod::audio::kDlnaDeviceUnavailable;
    }
    std::wstring sinkProtocolInfo;
    if (!target_.connectionManager.empty() &&
        !control_.GetSinkProtocolInfo(target_, sinkProtocolInfo, error)) {
        // Non-fatal: an unreadable Sink just means we cannot confirm the
        // renderer's formats, so fall back to the sinkUnknown candidate set
        // rather than refusing to play at all.
        DlnaLog(L"open GetSinkProtocolInfo failed (continuing sinkUnknown) device=" +
                target_.friendlyName + L" reason=" + error);
        sinkProtocolInfo.clear();
    }
    sinkProtocolInfoText_ = sinkProtocolInfo;
    if (!BuildFormatCandidates(sinkProtocolInfo)) {
        DlnaLog(L"open failed: no supported format device=" + target_.friendlyName +
                L" sourceRate=" + std::to_wstring(params_.format.sampleRate) +
                L" sourceBits=" + std::to_wstring(EffectiveSourceValidBits(params_.format)));
        return ammod::audio::kDlnaFormatUnsupported;
    }
    if (!SetupStream()) {
        DlnaLog(L"open failed: stream server unavailable device=" + target_.friendlyName);
        return ammod::audio::kDlnaStreamFailed;
    }
    DlnaLog(L"open device=" + target_.friendlyName + L" openHome=" +
            std::to_wstring(target_.openHome) + L" avTransport=" +
            std::to_wstring(target_.avTransport) + L" rate=" +
            std::to_wstring(format_.sampleRate) + L" bits=" +
            std::to_wstring(format_.validBits) + L" wav=" + std::to_wstring(wav_) +
            L" converted=" + std::to_wstring(converted_) + L" url=" + streamUrl_);
    lastError_.store(S_OK, std::memory_order_release);
    state_.store(AudioSinkState::Open, std::memory_order_release);
    return S_OK;
}

bool DlnaSink::SendStreamToDevice() noexcept {
    std::wstring error;
    // WAV is served file-shaped: a declared length, byte ranges, a musicTrack
    // item that carries both duration and size. That is the shape every
    // renderer already understands for a normal file, and the only one that
    // AURALiC-class devices accept from a live source. Bare LPCM has no
    // container and therefore no length, so it stays a radio broadcast.
    const auto duration = DlnaControlClient::TimeText(streamSeconds_) + L".000";
    // Every shape is a file now: a musicTrack with duration (and size when the
    // container length is known), never an unbounded broadcast item.
    constexpr bool broadcast = false;
    const std::uint64_t didlSize =
        (shape_ == DlnaStreamShape::WavFile || shape_ == DlnaStreamShape::FlacFile)
            ? totalBytes_ : 0;
    const auto didl = upnp::BuildDidlLite(
        L"ame-" + GuidToken(), L"Apple Music", streamUrl_, protocolInfo_,
        broadcast ? std::wstring() : duration, broadcast, didlSize,
        format_.sampleRate, format_.containerBits, format_.channels);
    if (target_.openHome && !target_.openHomePlaylist.empty()) {
        // Verified against foo_upnp (BubbleUPnPServer 0.9-update51) on the LAN:
        // OpenHome Playlist has no SetStreams action. The sequence that actually
        // makes the renderer fetch our URL is
        //   Transport.Stop -> Playlist.DeleteAll ->
        //   Playlist.Insert(AfterId=0, Uri, Metadata) -> Playlist.Play.
        // Metadata MUST be non-empty DIDL-Lite: with empty metadata Insert still
        // succeeds, but Play then fails with UPnP 501 "null or empty input
        // string" and the device never issues a GET.
        if (!target_.openHomeTransport.empty()) {
            std::wstring stopError;
            if (!control_.OpenHomeStop(target_, stopError)) {
                DlnaLog(L"openhome Transport.Stop failed (ignored) device=" +
                        target_.friendlyName + L" reason=" + stopError);
            }
        }
        if (!control_.OpenHomeDeleteAll(target_, error)) {
            // Non-fatal: some renderers reject DeleteAll while idle. Insert with
            // AfterId=0 still prepends the new track.
            DlnaLog(L"openhome DeleteAll failed (ignored) device=" +
                    target_.friendlyName + L" reason=" + error);
            error.clear();
        }
        std::uint32_t newId = 0;
        if (control_.OpenHomeInsert(target_, 0, streamUrl_, didl, newId, error)) {
            DlnaLog(L"openhome Insert ok device=" + target_.friendlyName +
                    L" id=" + std::to_wstring(newId) +
                    L" metadataBytes=" + std::to_wstring(didl.size()));
            error.clear();
            if (control_.OpenHomePlaylistPlay(target_, error)) {
                DlnaLog(L"openhome DeleteAll+Insert+Playlist.Play sent device=" +
                        target_.friendlyName);
                return true;
            }
            DlnaLog(L"openhome Playlist.Play failed device=" + target_.friendlyName +
                    L" reason=" + error);
            if (!target_.openHomeTransport.empty()) {
                std::wstring transportError;
                if (control_.OpenHomePlay(target_, transportError)) {
                    DlnaLog(L"openhome Insert + Transport.Play sent device=" +
                            target_.friendlyName);
                    return true;
                }
                DlnaLog(L"openhome Transport.Play failed device=" +
                        target_.friendlyName + L" reason=" + transportError);
            }
            controlFailures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Legacy OpenHome (v0 draft) has no Insert; hifi-relay drives those
        // devices with Playlist.Write(Id, Uris, Titles) + Playlist.Play(Id).
        // This is a compatibility fallback only - do not use it when Insert works.
        DlnaLog(L"openhome Insert failed, trying legacy Write device=" +
                target_.friendlyName + L" reason=" + error);
        std::wstring writeError;
        if (!control_.OpenHomeWrite(target_, 0, streamUrl_, L"Apple Music", writeError)) {
            controlFailures_.fetch_add(1, std::memory_order_relaxed);
            DlnaLog(L"openhome legacy Write failed device=" + target_.friendlyName +
                    L" reason=" + writeError);
            return false;
        }
        error.clear();
        if (!control_.OpenHomePlaylistPlayId(target_, 0, error)) {
            error.clear();
            if (!control_.OpenHomePlaylistPlay(target_, error) &&
                (target_.openHomeTransport.empty() || !control_.OpenHomePlay(target_, error))) {
                controlFailures_.fetch_add(1, std::memory_order_relaxed);
                DlnaLog(L"openhome legacy Write + Play failed device=" +
                        target_.friendlyName + L" reason=" + error);
                return false;
            }
        }
        DlnaLog(L"openhome legacy Write+Playlist.Play sent device=" +
                target_.friendlyName);
        return true;
    }
    if (target_.avTransportControl.empty()) {
        DlnaLog(L"send failed: no AVTransport and no OpenHome Playlist device=" +
                target_.friendlyName);
        return false;
    }
    if (!control_.SetAvTransportUri(target_, streamUrl_, didl, error)) {
        controlFailures_.fetch_add(1, std::memory_order_relaxed);
        DlnaLog(L"SetAVTransportURI failed device=" + target_.friendlyName +
                L" reason=" + error);
        return false;
    }
    if (!control_.Play(target_, error)) {
        controlFailures_.fetch_add(1, std::memory_order_relaxed);
        DlnaLog(L"AVTransport Play failed device=" + target_.friendlyName +
                L" reason=" + error);
        return false;
    }
    DlnaLog(L"SetAVTransportURI + Play sent device=" + target_.friendlyName);
    return true;
}

HRESULT DlnaSink::Start(IIntegerPcmSource& source) noexcept {
    if (!session_ || state_.load(std::memory_order_acquire) == AudioSinkState::Closed ||
        state_.load(std::memory_order_acquire) == AudioSinkState::Faulted) {
        return E_UNEXPECTED;
    }
    if (state_.load(std::memory_order_acquire) == AudioSinkState::Running) return S_OK;
    if (startedOnce_.load(std::memory_order_acquire)) {
        producerPaused_.store(false, std::memory_order_release);
        if (!producer_.joinable()) {
            producerStop_.store(false, std::memory_order_release);
            producer_ = std::thread(&DlnaSink::ProducerMain, this);
        }
        std::wstring error;
        bool resumed = true;
        if (!target_.avTransportControl.empty()) {
            resumed = control_.Play(target_, error);
        } else if (!target_.openHomeTransport.empty()) {
            resumed = control_.OpenHomePlay(target_, error);
        }
        if (!resumed) {
            controlFailures_.fetch_add(1, std::memory_order_relaxed);
            DlnaLog(L"resume play failed device=" + target_.friendlyName +
                    L" reason=" + error);
            RecordError(ammod::audio::kDlnaControlFailed);
            return ammod::audio::kDlnaControlFailed;
        }
        state_.store(AudioSinkState::Running, std::memory_order_release);
        return S_OK;
    }

    source_ = &source;
    bool started = false;
    for (std::size_t attempt = 0; attempt < formatCandidates_.size(); ++attempt) {
        producerStop_.store(false, std::memory_order_release);
        producerPaused_.store(false, std::memory_order_release);
        producer_ = std::thread(&DlnaSink::ProducerMain, this);
        // The coordinator already prebuffered the configured reservoir into
        // the P0 queue; move it into the HTTP ring before asking for Play.
        const auto targetBytes = static_cast<std::size_t>(
            static_cast<std::uint64_t>(format_.sampleRate) *
            static_cast<std::uint64_t>(format_.channels) *
            (format_.containerBits / 8u) * bufferMs_ / 1000u);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (session_ && session_->BufferedBytes() <
                   std::min<std::size_t>(targetBytes, session_->CapacityBytes() / 2u) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        DlnaLog(L"start attempt=" + std::to_wstring(attempt) + L" rate=" +
                std::to_wstring(format_.sampleRate) + L" bits=" +
                std::to_wstring(format_.validBits) + L" wav=" + std::to_wstring(wav_));
        if (SendStreamToDevice() && WaitForFirstGet(4000)) {
            DlnaLog(L"device started pulling url=" + streamUrl_);
            started = true;
            break;
        }
        DlnaLog(L"attempt failed or device did not GET url=" + streamUrl_);
        // The device rejected the URI or never fetched it. Retire this stream
        // and probe the next lower rate/depth candidate.
        producerStop_.store(true, std::memory_order_release);
        if (session_) session_->Abort();
        JoinProducer();
        std::wstring stopError;
        if (!target_.avTransportControl.empty()) {
            (void)control_.Stop(target_, stopError);
        } else if (!target_.openHomeTransport.empty()) {
            (void)control_.OpenHomeStop(target_, stopError);
        }
        if (attempt + 1 >= formatCandidates_.size()) break;
        if (!TryNextFormat()) break;
    }
    if (!started) {
        RecordError(ammod::audio::kDlnaControlFailed);
        return ammod::audio::kDlnaControlFailed;
    }
    startedOnce_.store(true, std::memory_order_release);
    endOfStreamSubmitted_.store(false, std::memory_order_release);
    state_.store(AudioSinkState::Running, std::memory_order_release);
    return S_OK;
}

void DlnaSink::ProducerMain() noexcept {
    // This thread drains the SPSC queue that the real-time P0 callback writes
    // into. At normal priority a CPU-hungry neighbour can starve it long
    // enough for the queue to back up, at which point P0 pushes fail and audio
    // is dropped even though the network is fine. Above-normal keeps the drain
    // responsive without competing with MMCSS audio threads.
    const int previousPriority = GetThreadPriority(GetCurrentThread());
    (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    lastServedBytes_ = 0;
    lastProgressTick_ = GetTickCount64();
    stallRestarts_ = 0;
    // The sample buffer is sized in int32 slots: one slot per channel per
    // frame. Hard-coding 2 here silently corrupts mono sources (Fill writes
    // half the buffer, PackChunk then reads the uninitialised tail as noise)
    // and overflows anything above stereo.
    const std::uint32_t channels = std::max<std::uint32_t>(1u, format_.channels);
    const auto samplesPerChunk = static_cast<std::size_t>(chunkFrames_) * channels;
    std::vector<std::int32_t> samples(samplesPerChunk);
    std::vector<BYTE> packed;
    for (;;) {
        if (producerStop_.load(std::memory_order_acquire)) break;
        if (producerPaused_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!source_) break;
        // Renderer stall watchdog. The HTTP layer can resume any offset the
        // ring still holds, but a range that has already been reclaimed can
        // only be recovered by re-issuing the URI, so ask the panel to restart
        // the current item. Eight seconds avoids fighting a normal pause.
        if (session_) {
            const auto now = GetTickCount64();
            const auto served = session_->BytesServed();
            if (served != lastServedBytes_) {
                lastServedBytes_ = served;
                lastProgressTick_ = now;
                stallRestarts_ = 0;
            } else if (served > 0 &&
                       !endOfStreamSubmitted_.load(std::memory_order_acquire) &&
                       now - lastProgressTick_ > 8000 && stallRestarts_ < 3) {
                ++stallRestarts_;
                lastProgressTick_ = now;
                LogDlnaLine(L"dlna stalled 8s; requesting restart attempt=" +
                            std::to_wstring(stallRestarts_));
                RequestRestartCurrent();
            }
        }
        const HRESULT prepared = source_->Prepare(chunkFrames_);
        if (prepared == kAudioSourceWouldBlock) {
            waitingForSource_.store(true, std::memory_order_release);
            if (params_.sourceReadyEvent) {
                WaitForSingleObject(params_.sourceReadyEvent, 20);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            continue;
        }
        if (prepared == S_FALSE) {
            waitingForSource_.store(false, std::memory_order_release);
            FlushEncoderTail();
            (void)session_->Append(nullptr, 0, true);
            endOfStreamSubmitted_.store(true, std::memory_order_release);
            break;
        }
        if (FAILED(prepared)) {
            RecordError(prepared);
            break;
        }
        std::uint32_t written = 0;
        bool eos = false;
        const HRESULT filled = source_->Fill(samples.data(), chunkFrames_, written, eos);
        if (FAILED(filled)) {
            RecordError(filled);
            break;
        }
        if (written == 0 && !eos) {
            waitingForSource_.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        waitingForSource_.store(false, std::memory_order_release);
        if (written != 0 && !PackChunk(samples.data(), written, false, packed)) {
            RecordError(E_FAIL);
            break;
        }
        if (written != 0 && !session_->Append(packed.data(), packed.size(), false)) {
            RecordError(ammod::audio::kDlnaStreamFailed);
            break;
        }
        submittedFrames_.fetch_add(written, std::memory_order_relaxed);
        if (eos) {
            FlushEncoderTail();
            (void)session_->Append(nullptr, 0, true);
            endOfStreamSubmitted_.store(true, std::memory_order_release);
            break;
        }
    }
    waitingForSource_.store(false, std::memory_order_release);
    if (previousPriority != THREAD_PRIORITY_ERROR_RETURN) {
        (void)SetThreadPriority(GetCurrentThread(), previousPriority);
    }
}

void DlnaSink::RequestRestartCurrent() noexcept {
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, modulePath, MAX_PATH)) return;
    const auto helper = std::filesystem::path(modulePath).parent_path() /
        L"am_exclusive_media_control.exe";
    if (!std::filesystem::exists(helper)) return;
    std::wstring command = L"\"" + helper.wstring() + L"\" restart-current";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

bool DlnaSink::PackChunk(const std::int32_t* samples, std::uint32_t frames,
                         bool endOfStream, std::vector<BYTE>& output) noexcept {
    if (shape_ == DlnaStreamShape::FlacFile) {
        // FLAC frames are emitted whole, so the encoder buffers up to one block
        // and hands back whatever is ready. Nothing is produced for a partial
        // block, which is why Drain may return an empty vector.
        output.clear();
        if (samples && frames != 0) flac_.Push(samples, frames);
        if (endOfStream) {
            flac_.Finish(output);
        } else {
            flac_.Drain(output);
        }
        return true;
    }
    (void)endOfStream;
    if (!samples || frames == 0) return true;
    // See ProducerMain: the channel count must come from the negotiated format,
    // not a stereo assumption, or mono/multichannel sources pack garbage.
    const std::uint32_t channels = std::max<std::uint32_t>(1u, format_.channels);
    const std::size_t sampleCount = static_cast<std::size_t>(frames) * channels;
    const std::uint32_t bytesPerSample = format_.containerBits / 8u;
    output.resize(sampleCount * bytesPerSample);
    // Bare LPCM (audio/L16, audio/L24) is defined by RFC 3551/3190 as
    // big-endian. WAV is always little-endian. Emitting canonical
    // little-endian bytes for a bare LPCM stream makes every sample byte-
    // swapped, which the renderer decodes as full-scale noise.
    const std::uint16_t bits = static_cast<std::uint16_t>(format_.containerBits);
    if (!wav_ && bigEndianPcm_) {
        return PackLpcmNetworkOrder(samples, output.data(), sampleCount, bits);
    }
    return PackCanonicalPcm32(samples, output.data(), sampleCount, bits);
}

void DlnaSink::FlushEncoderTail() noexcept {
    if (shape_ != DlnaStreamShape::FlacFile || !session_) return;
    std::vector<BYTE> tail;
    if (!PackChunk(nullptr, 0, true, tail) || tail.empty()) return;
    (void)session_->Append(tail.data(), tail.size(), false);
}

void DlnaSink::JoinProducer() noexcept {
    if (producer_.joinable()) producer_.join();
}

void DlnaSink::Pause() noexcept {
    if (state_.load(std::memory_order_acquire) != AudioSinkState::Running) return;
    producerPaused_.store(true, std::memory_order_release);
    std::wstring error;
    if (!target_.avTransportControl.empty()) {
        if (!control_.Pause(target_, error)) {
            controlFailures_.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (!target_.openHomeTransport.empty() &&
               !control_.OpenHomePause(target_, error)) {
        controlFailures_.fetch_add(1, std::memory_order_relaxed);
    }
    state_.store(AudioSinkState::Open, std::memory_order_release);
}

void DlnaSink::Stop() noexcept {
    producerStop_.store(true, std::memory_order_release);
    producerPaused_.store(false, std::memory_order_release);
    // Release a producer blocked on a full ring even when no HTTP client is
    // draining it. The session is not reused after Stop.
    if (session_) session_->Abort();
    JoinProducer();
    std::wstring error;
    if (!target_.avTransportControl.empty()) {
        (void)control_.Stop(target_, error);
    } else if (!target_.openHomeTransport.empty()) {
        (void)control_.OpenHomeStop(target_, error);
    }
    waitingForSource_.store(false, std::memory_order_release);
    if (session_) {
        state_.store(AudioSinkState::Open, std::memory_order_release);
    } else {
        state_.store(AudioSinkState::Closed, std::memory_order_release);
    }
}

void DlnaSink::Close() noexcept {
    Stop();
    CloseStream();
    server_.Stop();
    startedOnce_.store(false, std::memory_order_release);
    endOfStreamSubmitted_.store(false, std::memory_order_release);
    source_ = nullptr;
    state_.store(AudioSinkState::Closed, std::memory_order_release);
}

bool DlnaSink::EndOfStreamComplete() const noexcept {
    return session_ && session_->Finished();
}

AudioSinkStats DlnaSink::Stats() const noexcept {
    AudioSinkStats stats{};
    stats.kind = AudioSinkKind::Dlna;
    stats.state = state_.load(std::memory_order_acquire);
    stats.submittedFrames = submittedFrames_.load(std::memory_order_acquire);
    stats.underruns = underruns_.load(std::memory_order_acquire);
    stats.controlFailures = controlFailures_.load(std::memory_order_acquire);
    stats.lastError = lastError_.load(std::memory_order_acquire);
    if (session_) {
        stats.bytesServed = session_->BytesServed();
        const auto bytesPerFrame = session_->BytesPerFrame();
        stats.bufferedFrames = bytesPerFrame
            ? session_->BufferedBytes() / bytesPerFrame : 0;
    }
    return stats;
}

} // namespace ammod::audio_v2
