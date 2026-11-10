#include "RaatSink.h"

#include "DlnaLog.h"
#include "PcmTypes.h"
#include "RaatDiscovery.h"
#include "../AudioErrors.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <iomanip>
#include <sstream>
#include <thread>

namespace ammod::audio_v2 {
namespace {

using namespace ammod::raat;

constexpr std::uint32_t kRaatDefaultLookaheadMs = 1000;
constexpr std::uint32_t kRaatDefaultStartLeadMs = 1000;
constexpr std::uint32_t kRaatConnectTimeoutMs = 2500;
constexpr std::uint32_t kRaatRequestTimeoutMs = 5000;

std::wstring IniPath() {
    wchar_t publicDir[MAX_PATH]{};
    GetEnvironmentVariableW(L"PUBLIC", publicDir,
                            static_cast<DWORD>(std::size(publicDir)));
    if (!publicDir[0]) return {};
    return std::filesystem::path(publicDir) / L"AMExclusive" / L"am-exclusive.ini";
}

void LogRaat(const std::wstring& text) {
    LogDlnaLine(L"raat " + text);
}

std::string JsonStatus(const std::string& response) {
    return JsonGetString(response, "status");
}

void AppendBe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24u));
    out.push_back(static_cast<std::uint8_t>(value >> 16u));
    out.push_back(static_cast<std::uint8_t>(value >> 8u));
    out.push_back(static_cast<std::uint8_t>(value));
}

void AppendBe64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::string SourceBitsText(std::uint16_t bits) {
    return std::to_string(bits);
}

bool ContainsFormatToken(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace

RaatSink::~RaatSink() { Close(); }

HRESULT RaatSink::Open(const AudioSinkOpenParams& params) noexcept {
    Close();
    if (!IsValidFormat(params.format)) return E_INVALIDARG;
    if (params.raat.deviceId.empty() && params.raat.address.empty()) {
        return ammod::audio::kRaatDeviceUnavailable;
    }

    params_ = params;
    format_ = params.format;
    target_ = params.raat;
    source_ = nullptr;
    sampleIndex_ = 0;
    lastSendTick_ = 0;

    const std::uint16_t sourceBits = EffectiveSourceValidBits(format_);
    int configuredSoodPort = 0;
    ReadSettingInt(configuredSoodPort, L"sood_port", 0);
    SetSoodPort(configuredSoodPort > 0 && configuredSoodPort <= 65535
                    ? static_cast<std::uint16_t>(configuredSoodPort) : 9003);
    int configuredBits = 0;
    ReadSettingInt(configuredBits, L"wire_bits", 0);
    std::uint16_t wire = sourceBits;
    if (configuredBits == 16 || configuredBits == 24 || configuredBits == 32) {
        if (static_cast<std::uint16_t>(configuredBits) >= sourceBits) {
            wire = static_cast<std::uint16_t>(configuredBits);
        }
    }
    wireBits_ = wire;
    format_.validBits = wire;
    format_.containerBits = wire;

    lookaheadMs_ = params.streamBufferMs;
    if (lookaheadMs_ == 0) {
        int value = 0;
        ReadSettingInt(value, L"buffer_ms",
                       static_cast<int>(kRaatDefaultLookaheadMs));
        lookaheadMs_ = static_cast<std::uint32_t>(value);
    }
    lookaheadMs_ = std::clamp<std::uint32_t>(lookaheadMs_, 100u, 5000u);

    int lead = 0;
    ReadSettingInt(lead, L"start_lead_ms", static_cast<int>(kRaatDefaultStartLeadMs));
    startLeadMs_ = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(std::max(lead, 300)), 300u, 4000u);

    // 2048 frames (42.7 ms at 48 kHz) is the largest block the capture queue
    // hands out and is close to the renderer's own 50 ms output period.
    blockFrames_ = std::min<std::uint32_t>(kMaxBlockFrames, 2048u);
    block_.assign(static_cast<std::size_t>(blockFrames_) * format_.channels, 0);
    pcm_.resize(static_cast<std::size_t>(blockFrames_) * format_.channels * 4u);
    frame_.reserve(pcm_.size() + 16u);

    streamId_ = static_cast<std::uint32_t>((GetTickCount64() & 0x7fffffffu) | 1u);
    lastError_.store(S_OK, std::memory_order_release);
    state_.store(AudioSinkState::Open, std::memory_order_release);
    LogRaat(L"open device=" + target_.friendlyName + L" id=" + target_.deviceId +
            L" addr=" + target_.address + L":" + std::to_wstring(target_.controlPort) +
            L" raat=" + target_.raatVersion + L" rate=" +
            std::to_wstring(format_.sampleRate) + L" bits=" +
            std::to_wstring(wireBits_) + L" ch=" + std::to_wstring(format_.channels) +
            L" lookahead=" + std::to_wstring(lookaheadMs_) + L"ms lead=" +
            std::to_wstring(startLeadMs_) + L"ms");
    return S_OK;
}

HRESULT RaatSink::Start(IIntegerPcmSource& source) noexcept {
    if (state_.load(std::memory_order_acquire) == AudioSinkState::Closed ||
        state_.load(std::memory_order_acquire) == AudioSinkState::Faulted) {
        return E_UNEXPECTED;
    }
    source_ = &source;
    endOfStreamSubmitted_.store(false, std::memory_order_release);
    endOfStreamComplete_.store(false, std::memory_order_release);

    if (startedOnce_.load(std::memory_order_acquire) &&
        control_.IsOpen() && audio_.IsOpen()) {
        paused_.store(false, std::memory_order_release);
        paceStartTick_ = 0;
        lastKeepaliveTick_ = GetTickCount64();
        if (!SendStart(true)) {
            RecordError(ammod::audio::kRaatControlFailed);
            return ammod::audio::kRaatControlFailed;
        }
        state_.store(AudioSinkState::Running, std::memory_order_release);
        LogRaat(L"resume sample=" + std::to_wstring(sampleIndex_));
        return S_OK;
    }

    stop_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    startedOnce_.store(false, std::memory_order_release);
    sampleIndex_ = 0;
    paceStartTick_ = 0;
    paceStartSample_ = 0;
    paceLastLogTick_ = 0;
    lastKeepaliveTick_ = GetTickCount64();
    lastError_.store(S_OK, std::memory_order_release);

    if (!EnsureSession()) {
        const HRESULT error = lastError_.load(std::memory_order_acquire);
        RecordError(error == S_OK ? ammod::audio::kRaatStreamFailed : error);
        StopClock();
        control_.Close();
        audio_.Close();
        state_.store(AudioSinkState::Faulted, std::memory_order_release);
        return error == S_OK ? ammod::audio::kRaatStreamFailed : error;
    }

    try {
        pumpThread_ = std::thread(&RaatSink::PumpMain, this);
    } catch (...) {
        RecordError(ammod::audio::kRaatStreamFailed);
        StopClock();
        control_.Close();
        audio_.Close();
        return ammod::audio::kRaatStreamFailed;
    }
    startedOnce_.store(true, std::memory_order_release);
    state_.store(AudioSinkState::Running, std::memory_order_release);
    LogRaat(L"session live " + target_.address + L":" +
            std::to_wstring(target_.controlPort));
    return S_OK;
}

void RaatSink::Pause() noexcept {
    if (state_.load(std::memory_order_acquire) != AudioSinkState::Running) return;
    paused_.store(true, std::memory_order_release);
    if (control_.IsOpen()) {
        std::lock_guard lock(controlMutex_);
        std::string response;
        bool final = false;
        std::wstring error;
        (void)control_.Request("{\"request\":\"stop\"}", 1500, response, final, error);
    }
    state_.store(AudioSinkState::Open, std::memory_order_release);
    LogRaat(L"pause");
}

void RaatSink::Stop() noexcept {
    stop_.store(true, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(audioMutex_);
        audio_.Close();
    }
    if (pumpThread_.joinable()) pumpThread_.join();

    if (control_.IsOpen()) {
        std::lock_guard lock(controlMutex_);
        for (const char* request : {"{\"request\":\"stop\"}",
                                    "{\"request\":\"end_stream\"}"}) {
            std::string response;
            bool final = false;
            std::wstring error;
            (void)control_.Request(request, 1500, response, final, error);
        }
    }
    StopClock();
    control_.Close();
    audio_.Close();
    startedOnce_.store(false, std::memory_order_release);
    sampleIndex_ = 0;
    if (state_.load(std::memory_order_acquire) != AudioSinkState::Faulted) {
        state_.store(AudioSinkState::Open, std::memory_order_release);
    }
    LogRaat(L"stop");
}

void RaatSink::Close() noexcept {
    stop_.store(true, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(audioMutex_);
        audio_.Close();
    }
    if (pumpThread_.joinable()) pumpThread_.join();
    if (control_.IsOpen()) {
        std::lock_guard lock(controlMutex_);
        std::string response;
        bool final = false;
        std::wstring error;
        (void)control_.Request("{\"request\":\"teardown\"}", 1500, response, final, error);
    }
    StopClock();
    control_.Close();
    audio_.Close();
    startedOnce_.store(false, std::memory_order_release);
    sampleIndex_ = 0;
    source_ = nullptr;
    state_.store(AudioSinkState::Closed, std::memory_order_release);
}

bool RaatSink::EnsureSession() noexcept {
    std::wstring error;
    if (!EnsureWinsock()) {
        error = L"WSAStartup failed";
        lastError_.store(ammod::audio::kRaatControlFailed, std::memory_order_release);
        return false;
    }

    // The control port MOVES whenever the renderer restarts or re-enables its
    // RAAT output (35649 -> 43795 -> 38313 was observed on the same VEGA). A
    // cached ini port is therefore stale far more often than an address is.
    // Always refresh from SOOD before dialling; a failed query keeps the
    // cached value so an offline discovery service cannot block playback.
    {
        RaatDevice fresh;
        std::wstring queryError;
        if (RaatDiscovery::QueryOnce(target_.deviceId, 1500, fresh, queryError,
                                     target_.address) &&
            fresh.controlPort != 0) {
            if (fresh.controlPort != target_.controlPort) {
                LogRaat(L"sood port refresh " + std::to_wstring(target_.controlPort) +
                        L" -> " + std::to_wstring(fresh.controlPort));
            }
            target_.controlPort = fresh.controlPort;
            if (!fresh.address.empty()) target_.address = fresh.address;
            if (!fresh.raatVersion.empty()) target_.raatVersion = fresh.raatVersion;
            if (target_.friendlyName.empty()) target_.friendlyName = fresh.friendlyName;
        }
    }

    if (!OpenControl(error)) {
        LogRaat(L"control open failed: " + error);
        lastError_.store(target_.address.empty() ? ammod::audio::kRaatDeviceUnavailable
                                                 : ammod::audio::kRaatControlFailed,
                         std::memory_order_release);
        return false;
    }
    // Connect() already sent the "hi" frame. The captured working session then
    // repeats echo/info/info before injecting, so do the same. These are
    // Lua-dispatched requests, so before load_script they answer
    // NotImplemented; nothing waits for the replies.
    //
    // The old code also blocked here for the device's type-3 "ready" event
    // (~1.8 s after hi) because injecting earlier appeared to leave the
    // dispatcher uninstalled. Direct testing shows load_script succeeds and
    // the dispatcher runs whether or not that event was awaited, so the wait
    // is gone: it only delayed every session start by up to 3 s.
    {
        std::lock_guard lock(controlMutex_);
        control_.SendProbe("{\"request\":\"echo\"}", 1500);
        control_.SendProbe("{\"request\":\"info\"}", 1500);
        control_.SendProbe("{\"request\":\"info\"}", 1500);
        LogRaat(L"injection session greeted (hi + echo/info/info)");
    }
    if (!LoadScripts(error)) {
        LogRaat(L"scripts failed: " + error);
        lastError_.store(ammod::audio::kRaatScriptRejected, std::memory_order_release);
        return false;
    }

    // Captured working request order is exactly:
    //   echo/info/info -> 4x load_script -> echo -> setup -> stream -> sync -> start
    // There is no set_client_type, no subscribe_* and no second info. Both
    // corpus paths above already run their own echo canary, so go straight to
    // setup here. Sending the extra requests only produced NotImplemented and
    // delayed the audio handshake.
    if (!SendSetup(error)) {
        LogRaat(L"setup failed: " + error);
        return false;
    }
    localAddress_ = control_.LocalAddress();

    // Clock responder must be live before the device is asked to probe it.
    clockPort_ = 0;
    clockProbesIn_.store(0, std::memory_order_relaxed);
    clockRepliesOut_.store(0, std::memory_order_relaxed);
    clockStop_.store(false, std::memory_order_release);
    clockSocket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (clockSocket_ != INVALID_SOCKET) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = 0;
        if (bind(clockSocket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0) {
            sockaddr_in bound{};
            int length = sizeof(bound);
            if (getsockname(clockSocket_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
                clockPort_ = ntohs(bound.sin_port);
            }
        }
        try {
            clockThread_ = std::thread(&RaatSink::ClockMain, this);
        } catch (...) {
            clockStop_.store(true, std::memory_order_release);
        }
    }
    LogRaat(L"clock udp/" + std::to_wstring(clockPort_));

    if (!SendStream(error)) {
        LogRaat(L"stream failed: " + error);
        lastError_.store(ammod::audio::kRaatStreamFailed, std::memory_order_release);
        return false;
    }
    // The device-side audio receiver drops connections that send data before a
    // stream exists, so the audio socket is opened only after the stream
    // request has created sess.stream.
    if (!OpenAudio(error)) {
        LogRaat(L"audio connect failed: " + error);
        lastError_.store(ammod::audio::kRaatStreamFailed, std::memory_order_release);
        return false;
    }
    if (!Prefill()) {
        lastError_.store(ammod::audio::kRaatStreamFailed, std::memory_order_release);
        return false;
    }
    // Clock sync is OFF by default on this firmware (2026-11-09).
    //
    // foobar's captured session does setup -> stream -> sync -> start, and the
    // renderer answers sync with Success 1 ms after collecting five replies to
    // the clock probes it sends to the port named in the request. Replaying
    // that exact sequence from a plain test process works every time, but from
    // inside this hook the renderer closes BOTH the control and the audio
    // connection 1 ms after `sync`, emits no clock probe at all, and restarts
    // its RAAT service ~2.5 s later. The same frames, rid numbering, pacing and
    // real 24-bit PCM sent from the test process never disturb it, so the
    // request is not inherently fatal - something about our session state is.
    //
    // The renderer does not need the sync to start: without it, `start` is
    // answered with Success in ~50 ms and the device logs "Doing ASAP start
    // based on current device clock". Playback therefore works, so sync is
    // skipped (and stays available behind `[roon] sync=1` for further
    // investigation). Waiting for a reply that never comes also used to burn
    // 5 s and leave `start` unanswered, which is what surfaced as error 317.
    int sendSyncSetting = 0;
    ReadSettingInt(sendSyncSetting, L"sync", 0);
    if (sendSyncSetting != 0 && clockPort_ != 0 && !localAddress_.empty()) {
        if (!SendSync()) LogRaat(L"sync did not complete (non-fatal)");
    } else {
        LogRaat(L"sync skipped ([roon] sync=0): starting on the device clock");
    }
    if (!SendStart(false)) {
        lastError_.store(ammod::audio::kRaatControlFailed, std::memory_order_release);
        return false;
    }
    return true;
}

bool RaatSink::OpenControl(std::wstring& error) noexcept {
    std::vector<std::uint16_t> ports;
    if (target_.controlPort != 0) ports.push_back(target_.controlPort);

    const auto tryConnect = [&](std::uint16_t port, std::wstring& failure) -> bool {
        if (port == 0) return false;
        const auto started = std::chrono::steady_clock::now();
        if (!control_.Connect(target_.address, port, kRaatConnectTimeoutMs, failure)) {
            // Name the port: a stale cached port is the most common cause and
            // the log is the only place it can be seen.
            LogRaat(L"control connect " + target_.address + L":" +
                    std::to_wstring(port) + L" failed: " + failure);
            return false;
        }
        std::lock_guard lock(controlMutex_);
        // The hi frame (sent by Connect) is the whole greeting this firmware
        // needs. echo/info sent here are Lua-dispatched requests, so before
        // load_script they can only answer NotImplemented; they are kept
        // because the reference sender sends them and some firmwares use them
        // as a liveness check, but their replies are never waited for.
        control_.SendProbe("{\"request\":\"echo\"}", 1500);
        control_.SendProbe("{\"request\":\"info\"}", 1500);
        control_.SendProbe("{\"request\":\"info\"}", 1500);
        target_.controlPort = port;
        LogRaat(L"control hello sent on :" + std::to_wstring(port) +
                L" in " + std::to_wstring(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count()) + L"ms");
        return true;
    };

    // Historically this opened a throwaway "probe" connection, waited for the
    // device's type-3 ready event, closed it, slept, and injected on a second
    // connection - a shape inferred from a captured foobar2000 session. Direct
    // testing against the VEGA G2.2 showed the shape is irrelevant: a single
    // connection that sends hi and then load_script immediately works, and the
    // "NotImplemented" that motivated the dance was caused by the request
    // frame type, not by connection reuse (see kLlTypeRequest in
    // RaatProtocol.h). The probe/reconnect round trip only added ~4 s to every
    // session start, so it is gone.
    for (std::uint16_t port : ports) {
        std::wstring failure;
        if (tryConnect(port, failure)) {
            LogRaat(L"control port " + std::to_wstring(port) + L" verified");
            return true;
        }
    }

    // Direct connect failed: ask the 9200 signal band to enable the output.
    std::uint16_t wakePort = 0;
    std::wstring wakeError;
    if (!target_.address.empty() &&
        WakeDevice(target_.address, target_.deviceId, wakePort, wakeError)) {
        LogRaat(L"wake reported fresh tcp_port " + std::to_wstring(wakePort));
        std::wstring failure;
        if (tryConnect(wakePort, failure)) return true;
        error = failure;
    } else if (!wakeError.empty()) {
        LogRaat(L"wake failed: " + wakeError);
    }

    // Last resort: re-query SOOD for a moved port/address.
    RaatDevice device;
    std::wstring queryError;
    if (RaatDiscovery::QueryOnce(target_.deviceId, 1500, device, queryError)) {
        target_.address = device.address;
        target_.raatVersion = device.raatVersion;
        target_.friendlyName = device.friendlyName;
        std::wstring failure;
        if (tryConnect(device.controlPort, failure)) {
            LogRaat(L"SOOD re-query rescued port " + std::to_wstring(device.controlPort));
            return true;
        }
        error = failure;
    } else if (!queryError.empty()) {
        LogRaat(L"SOOD re-query failed: " + queryError);
    }
    if (error.empty()) error = wakeError.empty() ? L"no candidate ports" : wakeError;
    LogRaat(L"control open gave up: " + error);
    return false;
}

bool RaatSink::LoadScripts(std::wstring& error) noexcept {
    const bool newGen = RaatVersionIsNewGen(target_.raatVersion);
    if (newGen) LogRaat(L"newgen endpoint: replaying official signed corpus");
    else LogRaat(L"legacy endpoint: signed corpus first, clean-room only as fallback");

    const auto runCorpus = [&](const ScriptRecord* records, std::size_t count,
                               bool official) -> bool {
        (void)official;
        // Inject the whole bundle with ONE wire variant, then verify that the
        // Lua dispatcher really came up. Some firmwares answer every
        // load_script with Success but refuse to install the scripts; echo
        // (and later setup) then return {"status":"NotImplemented"}. Mixing
        // variants across scripts would leave a half-installed dispatcher.
        for (int variant = 0; variant < 3; ++variant) {
            bool allLoaded = true;
            for (std::size_t index = 0; index < count; ++index) {
                const auto& record = records[index];
                std::wstring lastError;
                if (!LoadOneScript(record, variant, lastError)) {
                    error = std::wstring(L"load ") + ToWide(record.name) +
                        L" failed: " + lastError;
                    allLoaded = false;
                    break;
                }
                LogRaat(std::wstring(L"load ") + ToWide(record.name) + L" ok" +
                        (official ? L" (signed)" : L""));
            }
            if (!allLoaded) {
                error.clear();
                continue;
            }
            LogRaat(L"corpus variant " + std::to_wstring(variant) + L" injected");
            // The captured working sender waited ~200 ms between the last
            // load_script and its echo canary while the firmware installed
            // the dispatcher.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::string response;
            bool final = false;
            std::wstring canaryError;
            bool ready = false;
            {
                std::lock_guard lock(controlMutex_);
                if (control_.Request("{\"request\":\"echo\"}", 800,
                                     response, final, canaryError) &&
                    JsonStatus(response) == "Success") {
                    ready = true;
                }
            }
            if (ready) {
                error.clear();
                LogRaat(L"dispatcher ready (echo canary, variant " +
                        std::to_wstring(variant) + L")");
                return true;
            }
            LogRaat(L"dispatcher not ready after variant " +
                    std::to_wstring(variant) + L" (" +
                    (canaryError.empty() ? ToWide(JsonStatus(response)) : canaryError) +
                    L")");
        }
        return false;
    };

    // A firmware that verifies signatures needs the VERBATIM corpus: field
    // order, "keypair":"Roon", the 64-byte signature and "module" all matter.
    // Replay each jsonl record byte-for-byte instead of rebuilding the JSON
    // from our own ScriptRecord table (which omits keypair).
    const auto replaySignedCorpus = [&](const std::wstring& path) -> bool {
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        if (!input) return false;
        int injected = 0;
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.find("\"request\"") == std::string::npos) continue;
            std::string response;
            bool final = false;
            std::wstring requestError;
            {
                std::lock_guard lock(controlMutex_);
                if (!control_.Request(line, 8000, response, final, requestError)) {
                    LogRaat(L"signed corpus replay failed: " + requestError);
                    return false;
                }
            }
            const std::string status = JsonStatus(response);
            if (!status.empty() && status != "Success") {
                LogRaat(L"signed corpus record refused: " + ToWide(status));
                return false;
            }
            ++injected;
        }
        if (injected == 0) return false;
        LogRaat(L"signed corpus replayed " + std::to_wstring(injected) +
                L" records from " + path);
        for (int attempt = 0; attempt < 6; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::string response;
            bool final = false;
            std::wstring canaryError;
            bool ready = false;
            {
                std::lock_guard lock(controlMutex_);
                if (control_.Request("{\"request\":\"echo\"}", 800,
                                     response, final, canaryError) &&
                    JsonStatus(response) == "Success") {
                    ready = true;
                }
            }
            if (ready) {
                error.clear();
                LogRaat(L"dispatcher ready after signed corpus replay");
                return true;
            }
        }
        LogRaat(L"dispatcher not ready after signed corpus replay");
        return false;
    };

    // Preferred path: replay the EXACT load_script requests captured from the
    // working foobar2000 session (tools/raat_foobar_corpus.jsonl, installed as
    // raat_signed_corpus.jsonl). Byte-for-byte identical to what this firmware
    // demonstrably accepts, including the missing module on rroon.lua.
    // Override with the RAAT_CORPUS environment variable when debugging.
    {
        std::wstring corpusPath;
        wchar_t env[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"RAAT_CORPUS", env, MAX_PATH) > 0) {
            corpusPath = env;
        } else {
            corpusPath = (std::filesystem::path(IniPath()).parent_path() /
                          L"raat_signed_corpus.jsonl").wstring();
        }
        if (!corpusPath.empty() && replaySignedCorpus(corpusPath)) return true;
    }

    // Fallback (no jsonl on disk). Prefer the embedded OFFICIAL scripts on
    // every firmware, not just the "new generation" ones: on this VEGA
    // (raat 1.1.41, which the version split calls legacy) the clean-room
    // scripts install and answer echo/setup/stream, but the renderer drops the
    // whole session as soon as it reaches clock/playback, so the clean-room
    // set must not be the first fallback any more.
    std::size_t count = 0;
    const ScriptRecord* records = OfficialScripts(count);
    if (runCorpus(records, count, true)) return true;

    LogRaat(L"official scripts refused; falling back to clean-room scripts");
    records = CleanroomScripts(count);
    if (runCorpus(records, count, false)) {
        target_.raatVersion = L"legacy";
        return true;
    }

    return false;
}

bool RaatSink::LoadOneScript(const ScriptRecord& record, int variant,
                             std::wstring& error) noexcept {
    const std::string script = LoadScriptResource(record.resourceId);
    if (script.empty()) {
        error = L"embedded resource missing";
        return false;
    }
    const std::string name = record.name;
    const std::string module = record.module;
    const std::string signature = record.signature;

    std::string json = "{\"request\":\"load_script\",\"name\":\"" +
        JsonEscape(name) + "\"";
    if (variant == 0) {
        if (!module.empty()) json += ",\"module\":\"" + JsonEscape(module) + "\"";
        json += ",\"script\":\"" + JsonEscape(script) + "\"";
        if (!signature.empty()) {
            json += ",\"signature\":\"" + signature + "\"";
        } else {
            // The reference sender's clean-room template ALWAYS carries a
            // "signature" field ("0" = malformed-hex / no-signature stub).
            // VEGA G2.2 answers load_script with Success but silently does
            // not install the dispatcher when the field is absent, leaving
            // every later request answered with "NotImplemented".
            json += ",\"signature\":\"0\"";
        }
    } else if (variant == 1) {
        json += ",\"script\":\"" + JsonEscape(script) + "\",\"signature\":\"0\"";
    } else {
        json += ",\"script\":\"" + JsonEscape(script) + "\"";
    }
    json += "}";

    std::lock_guard lock(controlMutex_);
    std::string response;
    bool final = false;
    if (!control_.Request(json, kRaatRequestTimeoutMs, response, final, error)) {
        return false;
    }
    const std::string status = JsonStatus(response);
    if (status == "Success" || status.empty()) return true;
    error = L"status " + ToWide(status);
    LogRaat(L"load " + ToWide(name) + L" -> " + ToWide(status) +
            L" (variant " + std::to_wstring(variant) + L")");
    return false;
}

bool RaatSink::SendInfo(std::wstring& error) noexcept {
    std::string response;
    bool final = false;
    {
        std::lock_guard lock(controlMutex_);
        if (!control_.Request("{\"request\":\"info\"}", 800,
                              response, final, error)) {
            // Not fatal: several firmwares answer info only after playback
            // starts. setup is the authoritative format negotiation.
            LogRaat(L"info not answered yet (continuing)");
            error.clear();
            return true;
        }
    }
    const std::string status = JsonStatus(response);
    if (status != "Success") {
        // VEGA G2.2 answers info with "NotImplemented" once the clean-room
        // dispatcher is installed. The captured working foobar2000 session
        // never waits for info at all - setup is the authoritative format
        // negotiation. Do not let this advisory reply kill the session.
        LogRaat(L"info not available (" + ToWide(status) + L"); continuing");
        error.clear();
        return true;
    }
    const std::string supported = JsonGetString(response, "supported_formats");
    if (!CheckSupportedFormat(supported)) {
        error = L"device does not advertise " + std::to_wstring(format_.sampleRate) +
            L"/" + std::to_wstring(wireBits_) + L"/" + std::to_wstring(format_.channels);
        return false;
    }
    LogRaat(L"info formats=\"" + ToWide(supported) + L"\"");
    return true;
}

bool RaatSink::CheckSupportedFormat(const std::string& supported) const noexcept {
    if (supported.empty()) return true;
    if (supported.find('/') == std::string::npos) return true;
    bool sawRateBits = false;
    std::stringstream entries(supported);
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        std::stringstream fields(entry);
        std::string rateText;
        std::string bitsText;
        std::string channelsText;
        if (!std::getline(fields, rateText, '/') ||
            !std::getline(fields, bitsText, '/')) {
            continue;
        }
        std::getline(fields, channelsText);
        const int rate = std::atoi(rateText.c_str());
        const int bits = std::atoi(bitsText.c_str());
        if (rate != static_cast<int>(format_.sampleRate) ||
            bits != static_cast<int>(wireBits_)) {
            continue;
        }
        sawRateBits = true;
        std::stringstream channelList(channelsText);
        std::string channel;
        while (std::getline(channelList, channel, '|')) {
            if (std::atoi(channel.c_str()) == static_cast<int>(format_.channels)) {
                return true;
            }
        }
    }
    return !sawRateBits ? true : false;
}

bool RaatSink::SendSetup(std::wstring& error) noexcept {
    const std::uint16_t sourceBits = EffectiveSourceValidBits(format_);
    std::vector<std::uint16_t> candidates;
    const auto add = [&](std::uint16_t bits) {
        if (bits != 16 && bits != 24 && bits != 32) return;
        if (bits < sourceBits) return;
        if (std::find(candidates.begin(), candidates.end(), bits) == candidates.end()) {
            candidates.push_back(bits);
        }
    };
    add(wireBits_);
    add(24);
    add(32);
    add(16);

    for (std::uint16_t bits : candidates) {
        std::ostringstream json;
        json << "{\"request\":\"setup\",\"format\":{\"sample_type\":\"pcm\","
             << "\"sample_rate\":" << format_.sampleRate
             << ",\"bits_per_sample\":" << bits
             << ",\"channels\":" << format_.channels << "}}";
        std::string response;
        bool final = false;
        std::wstring requestError;
        {
            std::lock_guard lock(controlMutex_);
            if (!control_.Request(json.str(), kRaatRequestTimeoutMs,
                                  response, final, requestError)) {
                error = requestError;
                continue;
            }
            // The signed scripts answer `setup` in TWO frames with the same rid:
            // first {"status":"OutputMessage","message":{"signal_path":…}} and
            // roughly a second later the frame carrying audio_port_tcp /
            // clock_port. Request() returns the first rid match, so the ports
            // would be judged missing and setup would look like a rejection.
            // Keep reading the same rid until the frame with the ports arrives.
            if (JsonStatus(response) == "OutputMessage") {
                const std::uint32_t rid = control_.LastRequestRid();
                LogRaat(L"setup output message received; waiting for the port frame");
                for (int attempt = 0; attempt < 4; ++attempt) {
                    std::string follow;
                    bool followFinal = false;
                    std::wstring followError;
                    if (!control_.ReadFrame(rid, 1500, follow, followFinal, followError)) {
                        LogRaat(L"setup port frame wait failed: " + followError);
                        break;
                    }
                    response = follow;
                    if (JsonGetInt(response, "audio_port_tcp", 0) > 0 ||
                        JsonGetInt(response, "clock_port", 0) > 0) {
                        break;
                    }
                    if (JsonStatus(response) != "OutputMessage") break;
                }
            }
        }
        const std::string status = JsonStatus(response);
        if (status == "Success") {
            const auto clockPort = JsonGetInt(response, "clock_port", 0);
            const auto audioPort = JsonGetInt(response, "audio_port_tcp", 0);
            if (clockPort <= 0 || clockPort > 65535 ||
                audioPort <= 0 || audioPort > 65535) {
                error = L"setup: no clock_port/audio_port_tcp";
                continue;
            }
            clockPort_ = static_cast<std::uint16_t>(clockPort);
            deviceClockPort_ = static_cast<std::uint16_t>(clockPort);
            audioPort_ = static_cast<std::uint16_t>(audioPort);
            wireBits_ = bits;
            format_.validBits = bits;
            format_.containerBits = bits;
            LogRaat(L"setup OK: clock udp/" + std::to_wstring(clockPort_) +
                    L", audio tcp/" + std::to_wstring(audioPort_) +
                    L", wire=" + std::to_wstring(bits) + L"bit");
            return true;
        }
        LogRaat(L"setup " + ToWide(status) + L" at " + std::to_wstring(bits) + L"bit");
        if (!ContainsFormatToken(status, "FORMAT_NOT_SUPPORTED") &&
            !ContainsFormatToken(status, "NotSupported")) {
            error = L"setup status " + ToWide(status);
            return false;
        }
    }
    if (error.empty()) error = L"no supported wire bit depth";
    return false;
}

bool RaatSink::OpenAudio(std::wstring& error) noexcept {
    if (audioPort_ == 0) {
        error = L"audio port not announced";
        return false;
    }
    if (!audio_.Connect(target_.address, audioPort_, kRaatConnectTimeoutMs, error)) {
        return false;
    }
    return SendAudioCommands(sampleIndex_);
}

bool RaatSink::SendAudioCommands(std::uint64_t startSample) noexcept {
    // Order and contents mirror the reference sender exactly: STREAMID, then
    // STREAMSAMPLE, then CHMAP. Nothing else is sent on the audio channel.
    //
    // Deliberately NOT sending CMD_NORMALIZATION (4): the device-side script
    // keeps tcp_gain/tcp_peak at 0.0, and 0.0 is the "no normalisation" value
    // passed straight to stream:write. Sending gain=1.0/peak=0.0 instead makes
    // the device scale every sample by peak, i.e. by zero - silent output with
    // a perfectly healthy control session, which is exactly the symptom this
    // fixed.
    std::vector<std::uint8_t> commands;
    commands.push_back(2); // STREAMID
    AppendBe32(commands, streamId_);
    commands.push_back(3); // STREAMSAMPLE
    AppendBe64(commands, startSample);
    // CHMAP: the device calls stream:write_chmap when has_write_chmap is set,
    // and only a CHMAP command populates tcp_chmap. Without it the device keeps
    // tcp_chmap = 0 and the channel map is left undefined.
    commands.push_back(1); // CHMAP
    AppendBe32(commands, ChannelMapFor(format_.channels));
    std::lock_guard lock(audioMutex_);
    if (!audio_.SendAll(commands.data(), commands.size(), 5000)) {
        return false;
    }
    lastSendTick_ = GetTickCount64();
    return true;
}

bool RaatSink::SendStream(std::wstring& error) noexcept {
    // Captured from the working foobar2000 sender on this very device
    // (raat_version 1.1.41, i.e. "legacy" by our old version split):
    //   {"request":"stream","first_seq":0,"stream_id":1,
    //    "buffer_size":1.0,"ready_threshold":0.25}
    // The legacy variant with first_seq=-1/nak_port=-1/buffer_size=10 was
    // never observed on the wire and is not accepted by this firmware.
    std::ostringstream json;
    json << "{\"request\":\"stream\",\"first_seq\":0,\"stream_id\":" << streamId_
         << ",\"buffer_size\":1.0,\"ready_threshold\":0.25}";
    std::string response;
    bool final = false;
    {
        std::lock_guard lock(controlMutex_);
        if (!control_.Request(json.str(), kRaatRequestTimeoutMs,
                              response, final, error)) {
            return false;
        }
    }
    const std::string status = JsonStatus(response);
    if (status == "Success" || status == "Buffering" || status == "Ready") {
        LogRaat(L"stream id=" + std::to_wstring(streamId_) + L" status=" + ToWide(status));
        return true;
    }
    error = L"stream status " + ToWide(status);
    return false;
}

bool RaatSink::Prefill() noexcept {
    if (!source_) return false;
    // Apple Music decodes roughly 1.3 s ahead; asking for the whole 5 s panel
    // look-ahead would only stall START and then insert silence. Pre-roll is
    // capped at 1 s; the pump keeps the device fed after start.
    const std::uint32_t prefillMs = std::min<std::uint32_t>(lookaheadMs_, 1000u);
    const std::uint64_t targetFrames =
        static_cast<std::uint64_t>(format_.sampleRate) * prefillMs / 1000u;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max<std::uint32_t>(1500u,
                                                                              prefillMs * 2u));
    std::uint64_t produced = 0;
    while (produced < targetFrames && !stop_.load(std::memory_order_acquire)) {
        const std::uint32_t requestFrames = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(blockFrames_, targetFrames - produced));
        if (source_->Prepare(requestFrames) == kAudioSourceWouldBlock) {
            if (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (!SendSilence(requestFrames)) return false;
            produced += requestFrames;
            continue;
        }
        std::uint32_t written = 0;
        bool endOfStream = false;
        const HRESULT fill = source_->Fill(block_.data(), requestFrames, written, endOfStream);
        if (fill == kAudioSourceWouldBlock) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (FAILED(fill) || written == 0) break;
        if (!SendData(block_.data(), written)) return false;
        produced += written;
        sampleIndex_ += written;
        if (endOfStream) {
            endOfStreamSubmitted_.store(true, std::memory_order_release);
            break;
        }
    }
    LogRaat(L"prefill " + std::to_wstring(produced) + L"/" +
            std::to_wstring(targetFrames) + L" frames");
    return true;
}

bool RaatSink::SendStart(bool resume) noexcept {
    // min_offset is in NANOSECONDS. The device computes
    //   time = raat.output.get_local_time(token) + min_offset
    // and echoes that absolute ns timestamp in the start reply. The
    // captured sender's 300000 is therefore 0.3 ms, not 300 ms - its
    // real lead came from the prefill, not from min_offset.
    const std::uint32_t offsetMs =
        std::clamp<std::uint32_t>(startLeadMs_, 300u, 1000u);
    // First start always refers to sample 0: the prefill bytes already queued
    // on the audio channel are the beginning of this stream.
    const std::uint64_t streamSample = resume ? sampleIndex_ : 0;
    std::ostringstream json;
    json << "{\"request\":\"start\",\"stream_sample\":" << streamSample
         << ",\"min_offset\":" << static_cast<std::uint64_t>(offsetMs) * 1000000ull << "}";
    std::string response;
    bool final = false;
    std::wstring error;
    {
        std::lock_guard lock(controlMutex_);
        if (!control_.Request(json.str(), kRaatRequestTimeoutMs,
                              response, final, error)) {
            LogRaat(L"start failed: " + error);
            return false;
        }
    }
    const std::string status = JsonStatus(response);
    if (status == "Success") {
        LogRaat(std::wstring(resume ? L"resume start " : L"start ") +
                L"sample=" + std::to_wstring(sampleIndex_) + L" lead=" +
                std::to_wstring(offsetMs) + L"ms (" +
                std::to_wstring(static_cast<std::uint64_t>(offsetMs) * 1000000ull) +
                L"ns)");
        return true;
    }
    // The device answered but refused. InvalidState means the stream was torn
    // down under us (another controller, or setup never completed); carrying
    // the device's own message is the only way to tell those apart.
    LogRaat(L"start status " + ToWide(status) +
            L" response=" + ToWide(response.substr(0, 400)));
    return false;
}

bool RaatSink::SendSync() noexcept {
    std::ostringstream json;
    json << "{\"request\":\"sync\",\"host\":\"" << JsonEscape(ToUtf8(localAddress_))
         << "\",\"port\":" << clockPort_ << "}";
    std::string response;
    bool final = false;
    std::wstring error;
    // The renderer probes `host:port` and needs five good replies before it
    // answers, so log what it was told and how many probes actually arrived.
    LogRaat(L"sync host=" + localAddress_ + L":" + std::to_wstring(clockPort_) +
            L" device_clock_port=" + std::to_wstring(deviceClockPort_));
    const auto started = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(controlMutex_);
        if (!control_.Request(json.str(), kRaatRequestTimeoutMs,
                              response, final, error)) {
            LogRaat(L"sync request failed: " + error + L" probes=" +
                    std::to_wstring(clockProbesIn_.load(std::memory_order_relaxed)) +
                    L" replies=" +
                    std::to_wstring(clockRepliesOut_.load(std::memory_order_relaxed)) +
                    L" waited=" +
                    std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count()) + L"ms");
            return false;
        }
    }
    const std::string status = JsonStatus(response);
    LogRaat(L"sync status " + ToWide(status) + L" probes=" +
            std::to_wstring(clockProbesIn_.load(std::memory_order_relaxed)) +
            L" replies=" +
            std::to_wstring(clockRepliesOut_.load(std::memory_order_relaxed)) +
            L" in " +
            std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count()) + L"ms");
    // The Lua sync handler answers "Ready" once it has collected enough clock
    // samples, and "Success" when the probe finished in one pass. Both mean the
    // clock domain is established; "Timeout" means the device never reached our
    // UDP responder, which is non-fatal (the sink starts anyway).
    return status == "Success" || status == "Ready";
}

void RaatSink::MaybeSendKeepalive() noexcept {
    const std::uint64_t now = GetTickCount64();
    if (lastKeepaliveTick_ == 0) {
        lastKeepaliveTick_ = now;
        return;
    }
    // RAATServer has a 10000ms inactivity watchdog on the control TCP client.
    // Send a lightweight control request every ~4 seconds to prevent the device
    // from timing out and terminating the session mid-stream.
    if (now - lastKeepaliveTick_ < 4000) {
        return;
    }
    lastKeepaliveTick_ = now;

    if (!control_.IsOpen()) return;

    std::lock_guard lock(controlMutex_);
    std::string response;
    bool final = false;
    std::wstring error;
    (void)control_.Request("{\"request\":\"info\"}", 1000, response, final, error);
}

void RaatSink::PumpMain() noexcept {
    bool reconnected = false;
    while (!stop_.load(std::memory_order_acquire)) {
        if (paused_.load(std::memory_order_acquire)) {
            paceStartTick_ = 0;
            MaybeSendKeepalive();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (!source_) break;

        const HRESULT prepared = source_->Prepare(blockFrames_);
        if (prepared == S_FALSE) {
            FinishEndOfStream();
            return;
        }
        if (prepared == kAudioSourceWouldBlock) {
            waitingForSource_.store(true, std::memory_order_release);
            if (sourceReadyEvent_) {
                WaitForSingleObject(sourceReadyEvent_, 10);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            // Keep the device buffer alive during a decode stall. This is the
            // same trade-off the original sender makes: a short silent gap is
            // better than an underrun that stalls the endpoint clock.
            if (GetTickCount64() - lastSendTick_ > 20) {
                const std::uint32_t silenceFrames = std::min<std::uint32_t>(
                    blockFrames_, static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(format_.sampleRate) / 100u));
                if (silenceFrames && SendSilence(silenceFrames)) {
                    underruns_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
            }
            continue;
        }
        if (FAILED(prepared)) {
            RecordError(ammod::audio::kRaatStreamFailed);
            return;
        }

        waitingForSource_.store(false, std::memory_order_release);
        std::uint32_t written = 0;
        bool endOfStream = false;
        const HRESULT filled = source_->Fill(block_.data(), blockFrames_, written, endOfStream);
        if (filled == kAudioSourceWouldBlock) {
            waitingForSource_.store(true, std::memory_order_release);
            continue;
        }
        if (FAILED(filled) || written == 0) {
            if (endOfStream) {
                endOfStreamSubmitted_.store(true, std::memory_order_release);
                endOfStreamComplete_.store(true, std::memory_order_release);
            }
            return;
        }

        if (!SendData(block_.data(), written)) {
            if (!reconnected) {
                reconnected = true;
                LogRaat(L"audio path lost; reconnecting");
                std::wstring error;
                {
                    std::lock_guard lock(audioMutex_);
                    audio_.Close();
                }
                if (control_.IsOpen()) {
                    std::lock_guard lock(controlMutex_);
                    std::string response;
                    bool final = false;
                    std::wstring stopError;
                    (void)control_.Request("{\"request\":\"stop\"}", 1500,
                                           response, final, stopError);
                }
                if (SendStream(error) && OpenAudio(error) && SendStart(true)) {
                    LogRaat(L"audio path re-established mid-stream");
                    reconnected = false;
                    paceStartTick_ = 0;
                    lastKeepaliveTick_ = GetTickCount64();
                    continue;
                }
                LogRaat(L"audio reconnect failed: " + error);
            }
            RecordError(ammod::audio::kRaatStreamFailed);
            return;
        }
        sampleIndex_ += written;

        // Real-time pacing gate. `sampleIndex_` counts everything handed to the
        // device, including the prefill. To prevent the device ring buffer (~2s)
        // from overrunning and discarding frames, we enforce a strict pacing gate:
        // when `sampleIndex_ > allowed`, loop and wait until the DAC's playback
        // catches up, keeping alive and observing stop/pause signals.
        if (paceStartTick_ == 0) {
            paceStartTick_ = GetTickCount64();
            paceStartSample_ = sampleIndex_;
            paceLastLogTick_ = paceStartTick_;
        }

        while (!stop_.load(std::memory_order_acquire) &&
               !paused_.load(std::memory_order_acquire)) {
            const std::uint64_t nowTick = GetTickCount64();
            const std::uint64_t elapsedMs = nowTick - paceStartTick_;
            const std::uint64_t allowed =
                paceStartSample_ +
                static_cast<std::uint64_t>(format_.sampleRate) * elapsedMs / 1000u;

            if (sampleIndex_ <= allowed) {
                break;
            }

            const std::uint64_t excessFrames = sampleIndex_ - allowed;
            const std::uint64_t waitMs = (excessFrames * 1000u) / format_.sampleRate;
            const std::uint32_t sleepMs = static_cast<std::uint32_t>(
                std::clamp<std::uint64_t>(waitMs, 1u, 10u));

            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            MaybeSendKeepalive();
        }

        MaybeSendKeepalive();

        const std::uint64_t nowLogTick = GetTickCount64();
        if (nowLogTick - paceLastLogTick_ >= 5000) {
            paceLastLogTick_ = nowLogTick;
            const std::uint64_t elapsedMs = nowLogTick - paceStartTick_;
            const std::uint64_t allowed =
                paceStartSample_ +
                static_cast<std::uint64_t>(format_.sampleRate) * elapsedMs / 1000u;
            LogRaat(L"pace sent=" + std::to_wstring(sampleIndex_) +
                    L" allowed=" + std::to_wstring(allowed) +
                    L" lead=" +
                    std::to_wstring(static_cast<std::int64_t>(sampleIndex_) -
                                    static_cast<std::int64_t>(allowed)) + L" frames");
        }

        if (endOfStream) {
            FinishEndOfStream();
            return;
        }
    }
}

void RaatSink::FinishEndOfStream() noexcept {
    endOfStreamSubmitted_.store(true, std::memory_order_release);
    if (control_.IsOpen()) {
        std::lock_guard lock(controlMutex_);
        std::string response;
        bool final = false;
        std::wstring error;
        (void)control_.Request("{\"request\":\"end_stream\"}", 1500,
                               response, final, error);
    }
    // The device still holds roughly one look-ahead of audio. Reporting
    // completion immediately would let the runtime tear the session down and
    // cut the tail; wait for that buffer to play out instead.
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::min<std::uint32_t>(lookaheadMs_, 1000u) + 200u);
    while (!stop_.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (stop_.load(std::memory_order_acquire)) return;
    endOfStreamComplete_.store(true, std::memory_order_release);
    state_.store(AudioSinkState::Open, std::memory_order_release);
    LogRaat(L"end of stream");
}

bool RaatSink::SendData(const std::int32_t* samples, std::uint32_t frames) noexcept {
    if (!samples || frames == 0) return true;
    const std::uint32_t samplesPerFrame = format_.channels;
    const std::size_t sampleCount = static_cast<std::size_t>(frames) * samplesPerFrame;
    const std::uint32_t bytesPerSample = wireBits_ / 8u;
    if (!PackCanonicalPcm32(samples, pcm_.data(), sampleCount, wireBits_)) return false;
    const std::uint32_t bodyBytes = static_cast<std::uint32_t>(
        sampleCount * bytesPerSample);

    frame_.clear();
    frame_.push_back(0); // DATA
    AppendBe32(frame_, frames);
    AppendBe32(frame_, bodyBytes);
    frame_.insert(frame_.end(), pcm_.begin(), pcm_.begin() + bodyBytes);

    std::lock_guard lock(audioMutex_);
    if (!audio_.SendAll(frame_.data(), frame_.size(), 5000)) return false;
    lastSendTick_ = GetTickCount64();
    submittedFrames_.fetch_add(frames, std::memory_order_relaxed);
    return true;
}

bool RaatSink::SendSilence(std::uint32_t frames) noexcept {
    if (frames == 0) return true;
    std::fill(block_.begin(),
              block_.begin() + static_cast<std::size_t>(frames) * format_.channels,
              0);
    if (!SendData(block_.data(), frames)) return false;
    sampleIndex_ += frames;
    return true;
}

void RaatSink::ClockMain() noexcept {
    if (clockSocket_ == INVALID_SOCKET) return;
    while (!clockStop_.load(std::memory_order_acquire)) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(clockSocket_, &set);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        const int ready = select(0, &set, nullptr, nullptr, &tv);
        if (ready <= 0 || !FD_ISSET(clockSocket_, &set)) continue;

        std::uint8_t buffer[256]{};
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int received = recvfrom(clockSocket_, reinterpret_cast<char*>(buffer),
                                      sizeof(buffer), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received < 12) continue;
        if (std::memcmp(buffer, "RAATCLKQ", 8) != 0) continue;
        const std::uint32_t probes =
            clockProbesIn_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (probes <= 8) {
            char peer[INET_ADDRSTRLEN]{};
            (void)inet_ntop(AF_INET, &from.sin_addr, peer, sizeof(peer));
            LogRaat(L"clock probe #" + std::to_wstring(probes) + L" from " +
                    ToWide(peer) + L":" +
                    std::to_wstring(ntohs(from.sin_port)) +
                    L" len=" + std::to_wstring(received));
        }
        const std::uint32_t rid =
            (static_cast<std::uint32_t>(buffer[8]) << 24u) |
            (static_cast<std::uint32_t>(buffer[9]) << 16u) |
            (static_cast<std::uint32_t>(buffer[10]) << 8u) |
            static_cast<std::uint32_t>(buffer[11]);
        std::vector<std::uint8_t> reply(28);
        std::memcpy(reply.data(), "RAATCLKR", 8);
        reply[8] = static_cast<std::uint8_t>(rid >> 24u);
        reply[9] = static_cast<std::uint8_t>(rid >> 16u);
        reply[10] = static_cast<std::uint8_t>(rid >> 8u);
        reply[11] = static_cast<std::uint8_t>(rid);
        // RAATCLKR local_time / output_delay live in the device's
        // nanosecond clock domain: req_start adds min_offset to
        // get_local_time(), and req_sync rejects rtt >= 30000000 (30 ms).
        // Replying with microseconds seeds set_remote_time with a
        // master clock that runs 1000x too slow.
        const std::uint64_t now = NowNanoseconds();
        for (int index = 0; index < 8; ++index) {
            reply[12 + index] = static_cast<std::uint8_t>(now >> (56 - index * 8));
        }
        // output delay = 0 ns
        const int sent = sendto(clockSocket_, reinterpret_cast<const char*>(reply.data()),
                                static_cast<int>(reply.size()), 0,
                                reinterpret_cast<const sockaddr*>(&from), fromLength);
        if (sent > 0) clockRepliesOut_.fetch_add(1, std::memory_order_relaxed);
    }
}

void RaatSink::StopClock() noexcept {
    clockStop_.store(true, std::memory_order_release);
    if (clockThread_.joinable()) clockThread_.join();
    if (clockSocket_ != INVALID_SOCKET) {
        closesocket(clockSocket_);
        clockSocket_ = INVALID_SOCKET;
    }
    clockPort_ = 0;
}

void RaatSink::RecordError(HRESULT error) noexcept {
    lastError_.store(error, std::memory_order_release);
    if (state_.load(std::memory_order_acquire) == AudioSinkState::Running) {
        state_.store(AudioSinkState::Faulted, std::memory_order_release);
    }
    std::wstringstream stream;
    stream << L"error hr=0x" << std::hex << std::uppercase
           << std::setw(8) << std::setfill(L'0')
           << static_cast<std::uint32_t>(error);
    LogRaat(stream.str());
}

bool RaatSink::ReadSettingInt(int& value, const wchar_t* key, int fallback) const noexcept {
    const std::wstring path = IniPath();
    if (path.empty()) {
        value = fallback;
        return false;
    }
    value = GetPrivateProfileIntW(L"roon", key, fallback, path.c_str());
    return true;
}

AudioSinkStats RaatSink::Stats() const noexcept {
    AudioSinkStats stats;
    stats.kind = AudioSinkKind::Raat;
    stats.state = state_.load(std::memory_order_acquire);
    stats.submittedFrames = submittedFrames_.load(std::memory_order_relaxed);
    stats.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
    stats.underruns = underruns_.load(std::memory_order_relaxed);
    stats.controlFailures = controlFailures_.load(std::memory_order_relaxed);
    stats.lastError = lastError_.load(std::memory_order_acquire);
    return stats;
}

int AudioCommandSelfTest() {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* what) {
        if (!condition) {
            std::printf("audio-cmd-check FAIL: %s\n", what);
            ++failures;
        }
    };

    // Channel maps must match what the reference sender computes
    // (~(-1 << channels)) for the layouts a Roon endpoint exposes.
    check(ChannelMapFor(0) == 0x00000000u, "0 channels");
    check(ChannelMapFor(1) == 0x00000001u, "mono");
    check(ChannelMapFor(2) == 0x00000003u, "stereo");
    check(ChannelMapFor(6) == 0x0000003Fu, "5.1");
    check(ChannelMapFor(8) == 0x000000FFu, "7.1");

    // The exact byte sequence OpenAudio must put on the wire: STREAMID,
    // STREAMSAMPLE, CHMAP - and nothing else. A CMD_NORMALIZATION with
    // peak=0.0 here is what silenced playback, so its absence is asserted.
    std::vector<std::uint8_t> commands;
    commands.push_back(2);  // STREAMID
    AppendBe32(commands, 0x12345678u);
    commands.push_back(3);  // STREAMSAMPLE
    AppendBe64(commands, 0);
    commands.push_back(1);  // CHMAP
    AppendBe32(commands, ChannelMapFor(2));

    const std::uint8_t expected[] = {
        0x02, 0x12, 0x34, 0x56, 0x78,
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x03,
    };
    check(commands.size() == sizeof(expected), "command frame length");
    if (commands.size() == sizeof(expected)) {
        check(std::memcmp(commands.data(), expected, sizeof(expected)) == 0,
              "command frame bytes");
    }
    for (const std::uint8_t byte : commands) {
        check(byte != 4, "no CMD_NORMALIZATION in the audio preamble");
    }

    if (failures == 0) std::printf("audio-cmd-check ok\n");
    return failures == 0 ? 0 : 9;
}

} // namespace ammod::audio_v2
