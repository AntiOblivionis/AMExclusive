#include "RemoteSink.h"

#include "AsioExclusiveSink.h"
#include "DlnaSink.h"
#include "LocalPlaybackTuning.h"
#include "WasapiExclusiveSink.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ammod::audio_v2 {
namespace {

// The hook cannot use HookDll's logger from this translation unit, and a
// failed host connection must be diagnosable without a debugger.
void LogRemote(const wchar_t* text) noexcept {
    // The hook runs inside AppleMusic/AMPLibraryAgent, whose package directory
    // is read-only. Diagnostics belong next to the main log under Public.
    wchar_t publicPath[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"PUBLIC", publicPath, MAX_PATH);
    std::wstring path;
    if (length != 0 && length < MAX_PATH) {
        path = std::wstring(publicPath) + L"\\AMExclusive\\am-exclusive-remote.log";
    } else {
        wchar_t modulePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, modulePath, MAX_PATH)) return;
        std::wstring base(modulePath);
        const auto slash = base.find_last_of(L"\\/");
        path = (slash == std::wstring::npos ? std::wstring(L".") : base.substr(0, slash)) +
               L"\\am-exclusive-remote.log";
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") == 0 && file) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        fwprintf(file,
                 L"%04u-%02u-%02uT%02u:%02u:%02u.%03u pid=%u %s\n",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                 now.wSecond, now.wMilliseconds, GetCurrentProcessId(), text);
        fclose(file);
    }
}

} // namespace

RemoteSink::RemoteSink() noexcept = default;

RemoteSink::~RemoteSink() {
    Close();
}

bool RemoteSink::Connect() noexcept {
    if (pipe_ != INVALID_HANDLE_VALUE && ring_.Attached()) return true;

    DWORD session = 0;
    (void)ProcessIdToSessionId(GetCurrentProcessId(), &session);
    sessionId_ = session;

    const std::wstring pipeName = HostObjectName(kHostPipePrefix, sessionId_);
    const std::wstring ringName = HostObjectName(kHostRingPrefix, sessionId_);
    const std::wstring dataName = HostObjectName(kHostDataEventPrefix, sessionId_);
    const std::wstring spaceName = HostObjectName(kHostSpaceEventPrefix, sessionId_);

    for (int attempt = 0; attempt < 20 && pipe_ == INVALID_HANDLE_VALUE; ++attempt) {
        pipe_ = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) Sleep(25);
    }
    if (pipe_ == INVALID_HANDLE_VALUE) {
        LogRemote(L"connect: host pipe unavailable");
        return false;
    }

    for (int attempt = 0; attempt < 20 && !ringMapping_; ++attempt) {
        ringMapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, ringName.c_str());
        if (!ringMapping_) Sleep(25);
    }
    if (!ringMapping_) {
        LogRemote(L"connect: ring section unavailable");
        return false;
    }
    ringView_ = MapViewOfFile(ringMapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!ringView_) {
        LogRemote(L"connect: ring map failed");
        return false;
    }

    dataEvent_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, dataName.c_str());
    spaceEvent_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, spaceName.c_str());
    if (!dataEvent_ || !spaceEvent_) {
        LogRemote(L"connect: events unavailable");
        return false;
    }

    if (!ring_.Attach(ringView_, spaceEvent_)) {
        LogRemote(L"connect: ring header rejected");
        return false;
    }

    // Hello handshake is advisory: the host answers with its protocol version
    // so a stale host binary is rejected instead of producing silence.
    HostMessage hello{};
    hello.type = static_cast<std::uint32_t>(HostMessageType::Hello);
    hello.generation = 0;
    // Request() sends this message and overwrites it with the reply, so the
    // reply must start as a copy of the request. Checking a fresh default
    // message here made every handshake fail and silently fell back.
    HostMessage reply = hello;
    if (!Request(reply) ||
        reply.type != static_cast<std::uint32_t>(HostMessageType::HelloAck) ||
        FAILED(reply.result)) {
        LogRemote(L"connect: hello handshake failed");
        return false;
    }
    if (reply.protocol != kHostProtocolVersion) {
        LogRemote(L"connect: host protocol mismatch");
        return false;
    }
    LogRemote(L"connect: handshake ok");
    return true;
}

bool RemoteSink::Send(const HostMessage& message) noexcept {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    return WriteFile(pipe_, &message, sizeof(message), &written, nullptr) &&
           written == sizeof(message);
}

bool RemoteSink::Receive(HostMessage& message) noexcept {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    DWORD read = 0;
    if (!ReadFile(pipe_, &message, sizeof(message), &read, nullptr)) return false;
    return read == sizeof(message) && message.size == sizeof(HostMessage);
}

bool RemoteSink::Request(HostMessage& message) noexcept {
    if (!Send(message)) return false;
    HostMessage reply{};
    if (!Receive(reply)) return false;
    message = reply;
    return true;
}

HRESULT RemoteSink::OpenFallback(const AudioSinkOpenParams& params) noexcept {
    fallback_.reset();
    if (targetKind_ == static_cast<std::uint32_t>(HostTargetKind::Dlna)) {
        fallback_ = std::make_unique<DlnaSink>();
    } else if (!params.asioDriverClsid.empty()) {
        fallback_ = std::make_unique<AsioExclusiveSink>();
    } else {
        fallback_ = std::make_unique<WasapiExclusiveSink>();
    }
    if (!fallback_) return E_OUTOFMEMORY;
    const HRESULT hr = fallback_->Open(params);
    if (FAILED(hr)) {
        fallback_.reset();
        return hr;
    }
    params_ = params;
    state_.store(AudioSinkState::Open, std::memory_order_release);
    return S_OK;
}

HRESULT RemoteSink::Open(const AudioSinkOpenParams& params) noexcept {
    Close();
    params_ = params;
    // DLNA wins when the profile carries a renderer location; ASIO when it
    // carries a driver CLSID; otherwise it is plain WASAPI.
    targetKind_ = !params.dlna.location.empty()
        ? static_cast<std::uint32_t>(HostTargetKind::Dlna)
        : (!params.asioDriverClsid.empty()
               ? static_cast<std::uint32_t>(HostTargetKind::Asio)
               : static_cast<std::uint32_t>(HostTargetKind::Wasapi));
    if (!Connect()) {
        Disconnect();
        return OpenFallback(params);
    }

    HostMessage open{};
    open.type = static_cast<std::uint32_t>(HostMessageType::Open);
    open.protocol = kHostProtocolVersion;
    open.targetKind = targetKind_;
    open.format = params.format;
    open.periodFrames = params.periodFrames;
    open.bufferMs = params.periodFrames ? params.periodFrames : 20;
    open.ringFrames = std::min<std::uint32_t>(
        kSharedRingMaxFrames,
        std::max<std::uint32_t>(params.format.sampleRate * 10u, 48000u));
    open.alignmentRetries = params.alignmentRetries;
    open.streamBufferMs = params.streamBufferMs;
    open.streamSeconds = params.streamSeconds;
    open.generation = 0;
    if (targetKind_ == static_cast<std::uint32_t>(HostTargetKind::Dlna)) {
        wcsncpy_s(open.endpointId, params.dlna.udn.c_str(), _TRUNCATE);
        wcsncpy_s(open.asioName, params.dlna.friendlyName.c_str(), _TRUNCATE);
        wcsncpy_s(open.dlnaLocation, params.dlna.location.c_str(), _TRUNCATE);
    } else if (targetKind_ == static_cast<std::uint32_t>(HostTargetKind::Asio)) {
        wcsncpy_s(open.asioClsid, params.asioDriverClsid.c_str(), _TRUNCATE);
        wcsncpy_s(open.asioName, params.asioDriverName.c_str(), _TRUNCATE);
    } else {
        wcsncpy_s(open.endpointId, params.endpointId.c_str(), _TRUNCATE);
    }

    HostMessage reply = open;
    if (!Request(reply) || reply.type != static_cast<std::uint32_t>(HostMessageType::OpenResult) ||
        FAILED(reply.result)) {
        LogRemote(L"open: host refused the request, falling back in-process");
        Disconnect();
        return OpenFallback(params);
    }

    ring_.MarkHostState(1);
    state_.store(AudioSinkState::Open, std::memory_order_release);
    return S_OK;
}

DWORD WINAPI RemoteSink::PumpThreadThunk(void* context) noexcept {
    return static_cast<RemoteSink*>(context)->PumpThreadMain();
}

DWORD RemoteSink::PumpThreadMain() noexcept {
    LocalPlaybackTuningRuntime::Instance().ApplyToCurrentThread();
    const std::uint32_t period = params_.periodFrames ? params_.periodFrames : 480u;
    std::int32_t buffer[kMaxBlockFrames * kSupportedChannels]{};
    const std::uint32_t chunk = std::min<std::uint32_t>(period, kMaxBlockFrames);

    while (!stopPump_.load(std::memory_order_acquire)) {
        IIntegerPcmSource* source = source_;
        if (!source) break;
        if (ring_.FreeFrames() < chunk) {
            if (pumpStopEvent_) (void)WaitForSingleObject(pumpStopEvent_, 20);
            else Sleep(1);
            continue;
        }
        const HRESULT prepared = source->Prepare(chunk);
        if (prepared == kAudioSourceWouldBlock) {
            Sleep(1);
            continue;
        }
        if (prepared == S_FALSE) {
            ring_.MarkEndOfStream();
            eosSubmitted_.store(true, std::memory_order_release);
            break;
        }
        if (FAILED(prepared)) break;

        std::uint32_t written = 0;
        bool endOfStream = false;
        const HRESULT filled = source->Fill(buffer, chunk, written, endOfStream);
        if (filled == kAudioSourceWouldBlock) {
            Sleep(1);
            continue;
        }
        if (FAILED(filled)) break;
        if (written != 0) {
            const std::uint32_t queued = ring_.Write(buffer, written, 50);
            if (queued != written) {
                stats_.droppedFrames += written - queued;
            }
            if (dataEvent_) SetEvent(dataEvent_);
        }
        if (endOfStream) {
            ring_.MarkEndOfStream();
            eosSubmitted_.store(true, std::memory_order_release);
            break;
        }
    }
    return 0;
}

HRESULT RemoteSink::Start(IIntegerPcmSource& source) noexcept {
    if (fallback_) return fallback_->Start(source);

    if (!ring_.Attached() || pipe_ == INVALID_HANDLE_VALUE) return E_UNEXPECTED;
    source_ = &source;
    stopPump_.store(false, std::memory_order_release);
    eosSubmitted_.store(false, std::memory_order_release);
    eosComplete_.store(false, std::memory_order_release);
    pumpStopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pumpThread_ = CreateThread(nullptr, 0, &RemoteSink::PumpThreadThunk, this, 0, nullptr);
    if (!pumpThread_) {
        source_ = nullptr;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    HostMessage start{};
    start.type = static_cast<std::uint32_t>(HostMessageType::Start);
    HostMessage reply = start;
    const bool ok = Request(reply) &&
        reply.type == static_cast<std::uint32_t>(HostMessageType::StartResult) &&
        SUCCEEDED(reply.result);
    if (!ok) {
        stopPump_.store(true, std::memory_order_release);
        if (pumpStopEvent_) SetEvent(pumpStopEvent_);
        WaitForSingleObject(pumpThread_, 2000);
        CloseHandle(pumpThread_);
        pumpThread_ = nullptr;
        source_ = nullptr;
        return FAILED(reply.result) ? reply.result : E_UNEXPECTED;
    }
    ring_.MarkHostState(2);
    state_.store(AudioSinkState::Running, std::memory_order_release);
    return S_OK;
}

void RemoteSink::Pause() noexcept {
    if (fallback_) {
        fallback_->Pause();
        return;
    }
    HostMessage pause{};
    pause.type = static_cast<std::uint32_t>(HostMessageType::Pause);
    (void)Request(pause);
    state_.store(AudioSinkState::Open, std::memory_order_release);
}

void RemoteSink::Stop() noexcept {
    if (fallback_) {
        fallback_->Stop();
        return;
    }
    stopPump_.store(true, std::memory_order_release);
    if (pumpStopEvent_) SetEvent(pumpStopEvent_);
    if (pumpThread_) {
        WaitForSingleObject(pumpThread_, 2000);
        CloseHandle(pumpThread_);
        pumpThread_ = nullptr;
    }
    if (pumpStopEvent_) {
        CloseHandle(pumpStopEvent_);
        pumpStopEvent_ = nullptr;
    }
    source_ = nullptr;
    if (pipe_ != INVALID_HANDLE_VALUE) {
        HostMessage stop{};
        stop.type = static_cast<std::uint32_t>(HostMessageType::Stop);
        (void)Request(stop);
    }
    state_.store(AudioSinkState::Open, std::memory_order_release);
}

void RemoteSink::Close() noexcept {
    if (fallback_) {
        fallback_->Close();
        fallback_.reset();
        state_.store(AudioSinkState::Closed, std::memory_order_release);
        return;
    }
    if (pumpThread_ || source_) Stop();
    if (pipe_ != INVALID_HANDLE_VALUE) {
        HostMessage close{};
        close.type = static_cast<std::uint32_t>(HostMessageType::Close);
        (void)Request(close);
    }
    Disconnect();
    state_.store(AudioSinkState::Closed, std::memory_order_release);
}

void RemoteSink::Disconnect() noexcept {
    ring_ = SharedRingProducer{};
    if (ringView_) {
        UnmapViewOfFile(ringView_);
        ringView_ = nullptr;
    }
    if (ringMapping_) {
        CloseHandle(ringMapping_);
        ringMapping_ = nullptr;
    }
    if (dataEvent_) {
        CloseHandle(dataEvent_);
        dataEvent_ = nullptr;
    }
    if (spaceEvent_) {
        CloseHandle(spaceEvent_);
        spaceEvent_ = nullptr;
    }
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

} // namespace ammod::audio_v2
