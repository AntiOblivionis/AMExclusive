#include "WasapiExclusiveSink.h"
#include "NativeRenderGateProxy.h"

#include <avrt.h>

#include <algorithm>
#include <cstring>

namespace ammod::audio_v2 {
namespace {

constexpr REFERENCE_TIME kReferenceTimePerSecond = 10000000;
constexpr REFERENCE_TIME kDefaultPeriod = 200000; // 20 ms

void Release(IUnknown*& value) noexcept {
    if (value) value->Release();
    value = nullptr;
}

template <typename T>
void Release(T*& value) noexcept {
    if (value) value->Release();
    value = nullptr;
}

REFERENCE_TIME PeriodForFrames(std::uint32_t frames, std::uint32_t sampleRate) noexcept {
    if (!frames || !sampleRate) return kDefaultPeriod;
    return static_cast<REFERENCE_TIME>(
        (static_cast<std::uint64_t>(frames) * kReferenceTimePerSecond + sampleRate / 2u) /
        sampleRate);
}

} // namespace

WasapiExclusiveSink::~WasapiExclusiveSink() {
    Close();
}

HRESULT WasapiExclusiveSink::Open(const WasapiSinkConfig& config) noexcept {
    Close();
    config_ = config;
    NotifyLifecycle(WasapiSinkLifecycleEvent::OpenEnter, S_OK);
    submittedFrames_.store(0, std::memory_order_release);
    submittedBuffers_.store(0, std::memory_order_release);
    controlledSilenceFrames_.store(0, std::memory_order_release);
    sourceWaits_.store(0, std::memory_order_release);
    underruns_.store(0, std::memory_order_release);
    getBufferFailures_.store(0, std::memory_order_release);
    releaseBufferFailures_.store(0, std::memory_order_release);
    lastError_.store(S_OK, std::memory_order_release);
    waitingForSource_.store(false, std::memory_order_release);
    resumeBlocked_.store(false, std::memory_order_release);
    endOfStreamSubmitted_.store(false, std::memory_order_release);
    if (!MakeWasapiFormat(config_.format, waveFormat_)) {
        RecordError(E_INVALIDARG);
        NotifyLifecycle(WasapiSinkLifecycleEvent::OpenExit, E_INVALIDARG);
        return E_INVALIDARG;
    }

    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(apartment)) comInitialized_ = true;
    else if (apartment != RPC_E_CHANGED_MODE) {
        RecordError(apartment);
        NotifyLifecycle(WasapiSinkLifecycleEvent::OpenExit, apartment);
        return apartment;
    }

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator_));
    if (SUCCEEDED(hr)) {
        if (config_.endpointId.empty()) {
            hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        } else {
            hr = enumerator_->GetDevice(config_.endpointId.c_str(), &device_);
        }
    }
    if (SUCCEEDED(hr)) hr = ActivateClient();
    if (SUCCEEDED(hr)) hr = InitializeClient();
    if (FAILED(hr)) {
        RecordError(hr);
        ReleaseInterfaces();
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        NotifyLifecycle(WasapiSinkLifecycleEvent::OpenExit, hr);
        return hr;
    }

    state_.store(WasapiSinkState::Open, std::memory_order_release);
    NotifyLifecycle(WasapiSinkLifecycleEvent::OpenExit, S_OK);
    return S_OK;
}

HRESULT WasapiExclusiveSink::ActivateClient() noexcept {
    if (!device_) return E_UNEXPECTED;
    Release(render_);
    Release(client_);
    ScopedNativeProxyBypass bypass;
    return device_->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                             reinterpret_cast<void**>(&client_));
}

HRESULT WasapiExclusiveSink::InitializeClient() noexcept {
    NotifyLifecycle(WasapiSinkLifecycleEvent::InitializeEnter, S_OK);
    const auto finish = [this](HRESULT result) noexcept {
        NotifyLifecycle(WasapiSinkLifecycleEvent::InitializeExit, result);
        return result;
    };
    if (!client_) return finish(E_UNEXPECTED);
    // The retry shape follows the pinned Microsoft
    // RenderExclusiveEventDriven/WASAPIRenderer.cpp:42-103 and
    // PlayPcmWin/WasapiIODLL/WasapiUser.cpp:456-480 guidance: use the buffer
    // size returned by AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED, reactivate a fresh
    // client, and retry with a frame-derived periodicity. The format policy
    // and ownership checks around that lifecycle are project-owned.
    const auto requested = config_.periodFrames
        ? PeriodForFrames(config_.periodFrames, config_.format.sampleRate)
        : kDefaultPeriod;
    REFERENCE_TIME period = requested;
    const auto attempts = std::max<std::uint32_t>(1u, config_.alignmentRetries + 1u);

    for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
        HRESULT hr = client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                 &waveFormat_.Format, nullptr);
        if (hr == S_FALSE) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (FAILED(hr)) return finish(hr);

        hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
            period, period, &waveFormat_.Format, nullptr);
        if (hr != AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            if (FAILED(hr)) return finish(hr);
            break;
        }

        UINT32 alignedFrames{};
        hr = client_->GetBufferSize(&alignedFrames);
         if (FAILED(hr) || alignedFrames == 0) return finish(FAILED(hr) ? hr : E_FAIL);
        period = PeriodForFrames(alignedFrames, config_.format.sampleRate);
        if (attempt + 1u >= attempts) return finish(AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED);
        hr = ActivateClient();
        if (FAILED(hr)) return finish(hr);
    }

    HRESULT hr = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(hr) || bufferFrames_ == 0) return finish(FAILED(hr) ? hr : E_FAIL);

    shutdownEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!shutdownEvent_) return finish(HRESULT_FROM_WIN32(GetLastError()));
    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audioEvent_) return finish(HRESULT_FROM_WIN32(GetLastError()));
    hr = client_->SetEventHandle(audioEvent_);
    if (FAILED(hr)) return finish(hr);
    return finish(client_->GetService(__uuidof(IAudioRenderClient),
                                      reinterpret_cast<void**>(&render_)));
}

HRESULT WasapiExclusiveSink::Start(IIntegerPcmSource& source) noexcept {
    NotifyLifecycle(WasapiSinkLifecycleEvent::StartEnter, S_OK);
    const auto finish = [this](HRESULT result) noexcept {
        NotifyLifecycle(WasapiSinkLifecycleEvent::StartExit, result);
        return result;
    };
    if (state_.load(std::memory_order_acquire) != WasapiSinkState::Open || !client_ || !render_) {
        return finish(E_UNEXPECTED);
    }
    if (!shutdownEvent_ || !ResetEvent(shutdownEvent_)) {
        const auto error = HRESULT_FROM_WIN32(GetLastError());
        RecordError(error);
        state_.store(WasapiSinkState::Faulted, std::memory_order_release);
        return finish(error);
    }
    source_ = &source;
    stopRequested_.store(false, std::memory_order_release);
    waitingForSource_.store(false, std::memory_order_release);
    endOfStreamSubmitted_.store(false, std::memory_order_release);

    // Prime one complete device buffer before Start. This keeps the first
    // render deterministic and makes short reads observable before cutover.
    const HRESULT prime = RenderOneBuffer();
    if (FAILED(prime)) {
        source_ = nullptr;
        state_.store(WasapiSinkState::Faulted, std::memory_order_release);
        return finish(prime);
    }

    renderThread_ = CreateThread(nullptr, 0, &WasapiExclusiveSink::RenderThreadThunk, this, 0, nullptr);
    if (!renderThread_) {
        const auto hr = HRESULT_FROM_WIN32(GetLastError());
        RecordError(hr);
        source_ = nullptr;
        state_.store(WasapiSinkState::Faulted, std::memory_order_release);
        return finish(hr);
    }

    HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        RecordError(hr);
        stopRequested_.store(true, std::memory_order_release);
        SetEvent(shutdownEvent_);
        WaitForSingleObject(renderThread_, INFINITE);
        CloseHandle(renderThread_);
        renderThread_ = nullptr;
        source_ = nullptr;
        state_.store(WasapiSinkState::Faulted, std::memory_order_release);
        return finish(hr);
    }
    state_.store(WasapiSinkState::Running, std::memory_order_release);
    return finish(S_OK);
}

HRESULT WasapiExclusiveSink::RenderOneBuffer() noexcept {
    if (!source_) return E_UNEXPECTED;
    if (!source_->CanProvide(bufferFrames_)) {
        // Do not acquire an endpoint buffer for a temporary network wait. The
        // Apple client remains the sole producer and the endpoint thread only
        // resumes after the P0 mirror reports a complete device buffer. No
        // AudioUnitRender call is made from this thread.
        waitingForSource_.store(true, std::memory_order_release);
        sourceWaits_.fetch_add(1, std::memory_order_relaxed);
        if (client_) {
            const HRESULT stop = client_->Stop();
            if (FAILED(stop) && stop != AUDCLNT_E_NOT_INITIALIZED) {
                RecordError(stop);
                state_.store(WasapiSinkState::Faulted, std::memory_order_release);
                return stop;
            }
        }
        return kAudioSourceWouldBlock;
    }
    BYTE* data = nullptr;
    HRESULT hr = render_->GetBuffer(bufferFrames_, &data);
    if (FAILED(hr)) {
        getBufferFailures_.fetch_add(1, std::memory_order_relaxed);
        RecordError(hr);
        return hr;
    }

    std::uint32_t written = 0;
    bool endOfStream = false;
    hr = source_->Fill(reinterpret_cast<std::int32_t*>(data), bufferFrames_, written, endOfStream);
    DWORD flags = 0;
    // CanProvide() is checked before GetBuffer, and the queue is single
    // consumer. A second would-block here therefore indicates a broken source
    // contract rather than a network wait; let the normal fail-closed branch
    // retire the generation instead of fabricating a partial buffer.
    if (FAILED(hr) || written > bufferFrames_ || written != bufferFrames_) {
        if (SUCCEEDED(hr) && written <= bufferFrames_ && endOfStream && written < bufferFrames_) {
            const auto remainder = static_cast<std::size_t>(bufferFrames_ - written) *
                                   waveFormat_.Format.nBlockAlign;
            std::memset(data + static_cast<std::size_t>(written) * waveFormat_.Format.nBlockAlign,
                        0, remainder);
            controlledSilenceFrames_.fetch_add(bufferFrames_ - written, std::memory_order_relaxed);
            written = bufferFrames_;
        } else {
            std::memset(data, 0, static_cast<std::size_t>(bufferFrames_) * waveFormat_.Format.nBlockAlign);
            flags = AUDCLNT_BUFFERFLAGS_SILENT;
            underruns_.fetch_add(1, std::memory_order_relaxed);
            const HRESULT failure = FAILED(hr) ? hr : AUDCLNT_E_BUFFER_ERROR;
            const HRESULT release = render_->ReleaseBuffer(bufferFrames_, flags);
            if (FAILED(release)) releaseBufferFailures_.fetch_add(1, std::memory_order_relaxed);
            RecordError(failure);
            stopRequested_.store(true, std::memory_order_release);
            state_.store(WasapiSinkState::Faulted, std::memory_order_release);
            return failure;
        }
    }

    hr = render_->ReleaseBuffer(bufferFrames_, flags);
    if (FAILED(hr)) {
        releaseBufferFailures_.fetch_add(1, std::memory_order_relaxed);
        RecordError(hr);
        stopRequested_.store(true, std::memory_order_release);
        state_.store(WasapiSinkState::Faulted, std::memory_order_release);
        return hr;
    }
    submittedFrames_.fetch_add(bufferFrames_, std::memory_order_relaxed);
    submittedBuffers_.fetch_add(1, std::memory_order_relaxed);
    if (endOfStream) endOfStreamSubmitted_.store(true, std::memory_order_release);
    // EOS belongs to the buffer just released. Stop requesting another buffer
    // after this full submission; otherwise a source that ends exactly on a
    // device-buffer boundary would make the render thread submit unbounded
    // silent buffers and never converge to a terminal state.
    if (endOfStream) stopRequested_.store(true, std::memory_order_release);
    return S_OK;
}

DWORD WINAPI WasapiExclusiveSink::RenderThreadThunk(void* context) noexcept {
    return static_cast<WasapiExclusiveSink*>(context)->RenderThreadMain();
}

DWORD WasapiExclusiveSink::RenderThreadMain() noexcept {
    // The event/MMCSS/full-buffer loop is intentionally kept at the same
    // boundary as the pinned Microsoft sample/WASAPIRenderer.cpp:467-528 and
    // mpv/audio/out/ao_wasapi.c:87-145. Unlike those general players, this
    // source has already-mapped integer PCM and never calls a converter,
    // mixer, resampler or shared-mode fallback.
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = apartment == S_OK || apartment == S_FALSE;
    DWORD mmcssIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcssIndex);

    if (SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE) {
        HANDLE handles[3] = {shutdownEvent_, audioEvent_, config_.sourceReadyEvent};
        const DWORD handleCount = config_.sourceReadyEvent ? 3u : 2u;
        while (!stopRequested_.load(std::memory_order_acquire)) {
            const DWORD waitTimeout = waitingForSource_.load(std::memory_order_acquire)
                ? 50u
                : (config_.sourceReadyEvent ? INFINITE : 20u);
            const DWORD wait = WaitForMultipleObjects(
                handleCount, handles, FALSE, waitTimeout);
            if (wait == WAIT_OBJECT_0) break;
            if (waitingForSource_.load(std::memory_order_acquire)) {
                if (resumeBlocked_.load(std::memory_order_acquire)) continue;
                const bool sourceSignal = config_.sourceReadyEvent
                    ? wait == WAIT_OBJECT_0 + 2
                    : wait == WAIT_TIMEOUT;
                // A stopped exclusive client no longer produces audio events.
                // Wait for the producer's P0 notification, or poll when a
                // caller did not provide a notification handle.
                if (!sourceSignal) continue;
                if (!source_ || !source_->CanProvide(bufferFrames_)) continue;

                // Fill the stopped client before restarting it. This preserves
                // the exact source stream across a network wait and avoids a
                // restart into an empty endpoint buffer.
                waitingForSource_.store(false, std::memory_order_release);
                const HRESULT fill = RenderOneBuffer();
                if (fill == kAudioSourceWouldBlock) continue;
                if (FAILED(fill)) break;
                if (stopRequested_.load(std::memory_order_acquire)) break;
                const HRESULT resume = client_ ? client_->Start() : E_UNEXPECTED;
                if (FAILED(resume)) {
                    RecordError(resume);
                    state_.store(WasapiSinkState::Faulted, std::memory_order_release);
                    break;
                }
                continue;
            }
            // A producer notification may race with a normal audio-event
            // wake while the sink is already running. It is only meaningful
            // in the waiting state; consume and ignore it here.
            if (config_.sourceReadyEvent && wait == WAIT_OBJECT_0 + 2) continue;
            if (wait == WAIT_TIMEOUT) continue;
            if (wait != WAIT_OBJECT_0 + 1) {
                RecordError(HRESULT_FROM_WIN32(GetLastError()));
                state_.store(WasapiSinkState::Faulted, std::memory_order_release);
                break;
            }
            const HRESULT render = RenderOneBuffer();
            if (render == kAudioSourceWouldBlock) continue;
            if (FAILED(render)) break;
        }
    } else {
        RecordError(apartment);
        stopRequested_.store(true, std::memory_order_release);
        state_.store(WasapiSinkState::Faulted, std::memory_order_release);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (uninitialize) CoUninitialize();
    return 0;
}

void WasapiExclusiveSink::Stop() noexcept {
    const auto current = state_.load(std::memory_order_acquire);
    if (current != WasapiSinkState::Running && !renderThread_) return;
    NotifyLifecycle(WasapiSinkLifecycleEvent::StopEnter, S_OK);
    HRESULT lifecycleResult = S_OK;
    stopRequested_.store(true, std::memory_order_release);
    if (shutdownEvent_) SetEvent(shutdownEvent_);
    if (client_) {
        const HRESULT stop = client_->Stop();
        if (FAILED(stop) && stop != AUDCLNT_E_NOT_INITIALIZED) {
            RecordError(stop);
            lifecycleResult = stop;
        }
    }
    if (renderThread_) {
        WaitForSingleObject(renderThread_, INFINITE);
        CloseHandle(renderThread_);
        renderThread_ = nullptr;
    }
    source_ = nullptr;
    waitingForSource_.store(false, std::memory_order_release);
    resumeBlocked_.store(false, std::memory_order_release);
    if (client_ && current != WasapiSinkState::Faulted) {
        const HRESULT reset = client_->Reset();
        if (FAILED(reset) && reset != AUDCLNT_E_NOT_INITIALIZED) {
            RecordError(reset);
            if (SUCCEEDED(lifecycleResult)) lifecycleResult = reset;
        }
    }
    if (current != WasapiSinkState::Faulted) {
        state_.store(WasapiSinkState::Open, std::memory_order_release);
    }
    NotifyLifecycle(WasapiSinkLifecycleEvent::StopExit, lifecycleResult);
}

void WasapiExclusiveSink::Close() noexcept {
    if (state_.load(std::memory_order_acquire) == WasapiSinkState::Closed &&
        !renderThread_ && !client_ && !render_ && !device_ && !enumerator_ &&
        !shutdownEvent_ && !audioEvent_ && !comInitialized_) {
        return;
    }
    NotifyLifecycle(WasapiSinkLifecycleEvent::CloseEnter, S_OK);
    Stop();
    ReleaseInterfaces();
    source_ = nullptr;
    state_.store(WasapiSinkState::Closed, std::memory_order_release);
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
    NotifyLifecycle(WasapiSinkLifecycleEvent::CloseExit,
                    static_cast<HRESULT>(lastError_.load(std::memory_order_acquire)));
}

void WasapiExclusiveSink::SetResumeBlocked(bool blocked) noexcept {
    resumeBlocked_.store(blocked, std::memory_order_release);
    if (!blocked && config_.sourceReadyEvent) SetEvent(config_.sourceReadyEvent);
}

void WasapiExclusiveSink::ReleaseInterfaces() noexcept {
    Release(render_);
    Release(client_);
    if (shutdownEvent_) {
        CloseHandle(shutdownEvent_);
        shutdownEvent_ = nullptr;
    }
    if (audioEvent_) {
        CloseHandle(audioEvent_);
        audioEvent_ = nullptr;
    }
    Release(device_);
    Release(enumerator_);
    bufferFrames_ = 0;
}

void WasapiExclusiveSink::RecordError(HRESULT error) noexcept {
    if (FAILED(error)) lastError_.store(error, std::memory_order_release);
}

void WasapiExclusiveSink::NotifyLifecycle(WasapiSinkLifecycleEvent event,
                                           HRESULT result) noexcept {
    if (config_.onLifecycle) config_.onLifecycle(config_.lifecycleContext, event, result);
}

WasapiSinkStats WasapiExclusiveSink::Stats() const noexcept {
    return WasapiSinkStats{
        submittedFrames_.load(std::memory_order_acquire),
        submittedBuffers_.load(std::memory_order_acquire),
        controlledSilenceFrames_.load(std::memory_order_acquire),
        sourceWaits_.load(std::memory_order_acquire),
        underruns_.load(std::memory_order_acquire),
        getBufferFailures_.load(std::memory_order_acquire),
        releaseBufferFailures_.load(std::memory_order_acquire),
        static_cast<HRESULT>(lastError_.load(std::memory_order_acquire)),
    };
}

} // namespace ammod::audio_v2
