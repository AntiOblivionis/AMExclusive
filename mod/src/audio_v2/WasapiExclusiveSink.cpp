#include "WasapiExclusiveSink.h"
#include "NativeRenderGateProxy.h"
#include "ResamplingPolicy.h"
#include "../AudioErrors.h"

#include <avrt.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

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
    endOfStreamComplete_.store(false, std::memory_order_release);
    const auto candidateCount = config_.formatCandidateCount == 0
        ? 1u : config_.formatCandidateCount;
    if (candidateCount > kMaxFormatCandidates) {
        RecordError(E_INVALIDARG);
        NotifyLifecycle(WasapiSinkLifecycleEvent::OpenExit, E_INVALIDARG);
        return E_INVALIDARG;
    }
    for (std::uint32_t index = 0; index < candidateCount; ++index) {
        const auto& candidate = config_.formatCandidateCount == 0
            ? config_.format : config_.formatCandidates[index];
        if (!IsValidFormat(candidate) ||
            candidate.sampleRate != config_.format.sampleRate ||
            candidate.channels != config_.format.channels ||
            EffectiveSourceValidBits(candidate) !=
                EffectiveSourceValidBits(config_.format)) {
            RecordError(E_INVALIDARG);
            NotifyLifecycle(WasapiSinkLifecycleEvent::OpenExit, E_INVALIDARG);
            return E_INVALIDARG;
        }
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
    Release(clock_);
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
    const auto attempts = std::max<std::uint32_t>(1u, config_.alignmentRetries + 1u);
    const auto candidateCount = config_.formatCandidateCount == 0
        ? 1u : config_.formatCandidateCount;
    const auto sourceFormat = config_.format;
    std::uint32_t deviceRateHint = 0;
    if (config_.allowResampling) {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(client_->GetMixFormat(&mix)) && mix) deviceRateHint = mix->nSamplesPerSec;
        if (mix) CoTaskMemFree(mix);
    }
    const auto candidates = SelectEndpointFormats(sourceFormat,
        config_.formatCandidateCount == 0 ? &sourceFormat : config_.formatCandidates.data(),
        candidateCount, config_.allowResampling, deviceRateHint);
    // periodFrames belongs to the source format. Preserve its duration across
    // output rates; alignment retries below use the negotiated endpoint rate.
    const auto requested = config_.periodFrames
        ? PeriodForFrames(config_.periodFrames, sourceFormat.sampleRate)
        : kDefaultPeriod;
    bool initialized = false;
    for (std::size_t candidateIndex = 0;
         candidateIndex < candidates.count && !initialized; ++candidateIndex) {
        config_.format = candidates.values[candidateIndex];
        // Some stereo drivers accept only a legacy WAVEFORMATEX declaration.
        // Probe alternative declarations only in compatibility mode, retaining
        // the exact same PCM contract and exclusive initialization for each.
        const unsigned declarations = config_.allowResampling ? 3u : 1u;
        for (unsigned declaration = 0; declaration < declarations && !initialized; ++declaration) {
            if (declaration == 2 && config_.format.validBits != config_.format.containerBits) continue;
            if (candidateIndex != 0 || declaration != 0) {
                const HRESULT activate = ActivateClient();
                if (FAILED(activate)) return finish(activate);
            }
            if (!MakeWasapiFormat(config_.format, waveFormat_, config_.allowResampling))
                return finish(E_INVALIDARG);
            if (declaration == 1) waveFormat_.dwChannelMask = 0;
            if (declaration == 2) {
                waveFormat_.Format.wFormatTag = config_.format.encoding == PcmEncoding::IeeeFloat
                    ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
                waveFormat_.Format.cbSize = 0;
            }

            REFERENCE_TIME period = requested;
            bool unsupported = false;
            for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
                HRESULT hr = client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                         &waveFormat_.Format, nullptr);
                if (hr == S_FALSE) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
                if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
                    unsupported = true;
                    break;
                }
                if (FAILED(hr)) return finish(hr);

                hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                    period, period, &waveFormat_.Format, nullptr);
                if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
                    unsupported = true;
                    break;
                }
                if (hr != AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                    if (FAILED(hr)) return finish(hr);
                    initialized = true;
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
            if (unsupported) continue;
        }
    }
    if (!initialized) {
        return finish(config_.allowResampling
            ? ammod::audio::kExclusiveFormatUnavailable
            : ammod::audio::kBitPerfectFormatUnavailable);
    }

    HRESULT hr = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(hr) || bufferFrames_ == 0) return finish(FAILED(hr) ? hr : E_FAIL);
    try {
        if (config_.format.containerBits == 32 && config_.format.encoding == PcmEncoding::SignedInteger) {
            canonicalBuffer_.clear();
        } else {
            canonicalBuffer_.resize(static_cast<std::size_t>(bufferFrames_) *
                                    config_.format.channels);
        }
    } catch (const std::bad_alloc&) {
        return finish(E_OUTOFMEMORY);
    }

    shutdownEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!shutdownEvent_) return finish(HRESULT_FROM_WIN32(GetLastError()));
    audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audioEvent_) return finish(HRESULT_FROM_WIN32(GetLastError()));
    hr = client_->SetEventHandle(audioEvent_);
    if (FAILED(hr)) return finish(hr);
    hr = client_->GetService(__uuidof(IAudioRenderClient),
                            reinterpret_cast<void**>(&render_));
    if (FAILED(hr)) return finish(hr);
    // Only transformed PCM uses staging and needs the conversion drain contract.
    // Native lossless output (including the disabled option) keeps its old path.
    if (RequiresPcmConversion(sourceFormat, config_.format)) {
        hr = client_->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&clock_));
        if (FAILED(hr)) return finish(hr);
        UINT64 frequency{};
        hr = clock_->GetFrequency(&frequency);
        if (FAILED(hr) || frequency == 0) return finish(FAILED(hr) ? hr : E_UNEXPECTED);
        REFERENCE_TIME latency{};
        hr = client_->GetStreamLatency(&latency);
        if (FAILED(hr) || latency < 0) return finish(FAILED(hr) ? hr : E_UNEXPECTED);
        // Exclusive event-driven WASAPI has two equal ping-pong buffers.
        // After terminal ReleaseBuffer, advancing the device clock by both
        // buffers' capacity plus the reported stream latency is a conservative
        // drain bound, even after a source wait/underrun. Cumulative frames count silence
        // played during an earlier wait as consumption of the terminal media.
        // https://learn.microsoft.com/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize
        const auto rate = config_.format.sampleRate;
        const auto latencyFrames = static_cast<std::uint64_t>(latency / kReferenceTimePerSecond) * rate +
            (static_cast<std::uint64_t>(latency % kReferenceTimePerSecond) * rate +
             kReferenceTimePerSecond - 1) / kReferenceTimePerSecond;
        const auto frames = std::uint64_t{2} * bufferFrames_ + latencyFrames;
        const auto limit = std::numeric_limits<std::uint64_t>::max();
        if (frequency % rate != 0 && frames > (limit - rate + 1) / (frequency % rate))
            return finish(E_INVALIDARG);
        const auto remainder = (frequency % rate * frames + rate - 1) / rate;
        if (frequency / rate > (limit - remainder) / frames) return finish(E_INVALIDARG);
        drainClockTicks_ = frequency / rate * frames + remainder;
        drainMaxWaitMs_ = 2000u + frames / rate * 1000u +
            (frames % rate * 1000u + rate - 1) / rate;
    }
    return finish(S_OK);
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
    drainWaitStartedTick_ = 0;
    if (!pausedWithBuffer_) {
        waitingForSource_.store(false, std::memory_order_release);
        endOfStreamSubmitted_.store(false, std::memory_order_release);
        endOfStreamComplete_.store(false, std::memory_order_release);
        drainPositionValid_ = false;

        // Prime once per stream. SRC pause preserves the submitted buffer and
        // clock epoch, so resume must not consume or prime another source block.
        const HRESULT prime = RenderOneBuffer();
        if (FAILED(prime)) {
            RecordError(prime);
            source_ = nullptr;
            state_.store(WasapiSinkState::Faulted, std::memory_order_release);
            return finish(prime);
        }
    }
    pausedWithBuffer_ = false;

    if (clock_) {
        // Start before publishing the render worker: otherwise an already-set
        // producer event can make both threads restart the stopped client.
        const HRESULT start = waitingForSource_.load(std::memory_order_acquire)
            ? S_OK : client_->Start();
        if (FAILED(start)) {
            RecordError(start);
            source_ = nullptr;
            state_.store(WasapiSinkState::Faulted, std::memory_order_release);
            return finish(start);
        }
        state_.store(WasapiSinkState::Running, std::memory_order_release);
    }

    renderThread_ = CreateThread(nullptr, 0, &WasapiExclusiveSink::RenderThreadThunk, this, 0, nullptr);
    if (!renderThread_) {
        const auto hr = HRESULT_FROM_WIN32(GetLastError());
        RecordError(hr);
        if (clock_) (void)client_->Stop();
        source_ = nullptr;
        state_.store(WasapiSinkState::Faulted, std::memory_order_release);
        return finish(hr);
    }

    if (clock_) {
        if (waitingForSource_.load(std::memory_order_acquire) && config_.sourceReadyEvent)
            SetEvent(config_.sourceReadyEvent);
        return finish(S_OK);
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
    HRESULT prepared = source_->Prepare(bufferFrames_);
    if (prepared == kAudioSourceWouldBlock) {
        // Publish the waiting state before the second availability check. A
        // producer that races the first check will either be observed here or
        // see waitingForSource_ and signal sourceReadyEvent_, so no wakeup is
        // lost while healthy playback avoids producer-cadence event traffic.
        waitingForSource_.store(true, std::memory_order_release);
        prepared = source_->Prepare(bufferFrames_);
        if (prepared == kAudioSourceWouldBlock) {
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
        waitingForSource_.store(false, std::memory_order_release);
    }
    if (FAILED(prepared)) {
        RecordError(prepared);
        return prepared;
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
    const bool directInteger32 = config_.format.containerBits == 32 &&
        config_.format.encoding == PcmEncoding::SignedInteger;
    auto* canonical = directInteger32
        ? reinterpret_cast<std::int32_t*>(data) : canonicalBuffer_.data();
    hr = source_->Fill(canonical, bufferFrames_, written, endOfStream);
    if (SUCCEEDED(hr) && written <= bufferFrames_ && !directInteger32) {
        const auto sampleCount = static_cast<std::size_t>(written) * config_.format.channels;
        if (!PackEndpointPcm32(canonical, data, sampleCount, config_.format)) {
            hr = E_INVALIDARG;
        }
    }
    DWORD flags = 0;
    // Prepare() is checked before GetBuffer, and the queue is single
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
    if (endOfStream) {
        if (clock_) return CheckEndOfStreamDrain();
        endOfStreamComplete_.store(true, std::memory_order_release);
        stopRequested_.store(true, std::memory_order_release);
    }
    return S_OK;
}

HRESULT WasapiExclusiveSink::CheckEndOfStreamDrain() noexcept {
    const auto now = GetTickCount64();
    if (drainWaitStartedTick_ == 0) drainWaitStartedTick_ = now;
    // An unavailable/stalled clock must not hold a generation forever. This
    // deadline can only fault, never certify playback. Pause/resume restarts
    // the watchdog while retaining the device-clock drain boundary.
    if (now - drainWaitStartedTick_ > drainMaxWaitMs_) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    UINT64 position{};
    const HRESULT hr = clock_->GetPosition(&position, nullptr);
    if (hr != S_OK) return FAILED(hr) ? hr : S_OK; // Retry imprecise readings on a later wake.
    if (!drainPositionValid_) {
        drainStartPosition_ = position;
        drainPositionValid_ = true;
    } else if (position < drainStartPosition_) {
        return E_UNEXPECTED; // A reset/replaced clock cannot complete this stream.
    } else if (position - drainStartPosition_ >= drainClockTicks_) {
        endOfStreamComplete_.store(true, std::memory_order_release);
    }
    return S_OK;
}

DWORD WINAPI WasapiExclusiveSink::RenderThreadThunk(void* context) noexcept {
    return static_cast<WasapiExclusiveSink*>(context)->RenderThreadMain();
}

DWORD WasapiExclusiveSink::RenderThreadMain() noexcept {
    // The event/MMCSS/full-buffer loop is intentionally kept at the same
    // boundary as the pinned Microsoft sample/WASAPIRenderer.cpp:467-528 and
    // mpv/audio/out/ao_wasapi.c:87-145. Unlike those general players, this
    // source provides integer PCM, optionally staged by an application-owned
    // resampler before GetBuffer. There is no shared-mode fallback.
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = apartment == S_OK || apartment == S_FALSE;
    HRESULT threadFailure = S_OK;
    DWORD mmcssIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssIndex);
    if (!mmcss) mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcssIndex);
    if (mmcss) (void)AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);

    if (SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE) {
        HANDLE handles[3] = {shutdownEvent_, audioEvent_, config_.sourceReadyEvent};
        const DWORD handleCount = config_.sourceReadyEvent ? 3u : 2u;
        while (!stopRequested_.load(std::memory_order_acquire)) {
            const bool draining = clock_ && EndOfStreamSubmitted();
            const DWORD waitTimeout = draining ? 20u : waitingForSource_.load(std::memory_order_acquire)
                ? 50u
                : (config_.sourceReadyEvent ? INFINITE : 20u);
            const DWORD wait = WaitForMultipleObjects(
                handleCount, handles, FALSE, waitTimeout);
            if (wait == WAIT_OBJECT_0 || stopRequested_.load(std::memory_order_acquire)) break;
            if (wait == WAIT_FAILED) {
                threadFailure = HRESULT_FROM_WIN32(GetLastError());
                break;
            }
            if (draining) {
                // No more GetBuffer/Fill after the terminal buffer. The timeout
                // only schedules a clock read; wall time never proves drain.
                threadFailure = CheckEndOfStreamDrain();
                if (FAILED(threadFailure) || EndOfStreamComplete()) break;
                continue;
            }
            if (waitingForSource_.load(std::memory_order_acquire)) {
                if (resumeBlocked_.load(std::memory_order_acquire)) continue;
                const bool sourceSignal = config_.sourceReadyEvent
                    ? wait == WAIT_OBJECT_0 + 2
                    : wait == WAIT_TIMEOUT;
                // A stopped exclusive client no longer produces audio events.
                // Wait for the producer's P0 notification, or poll when a
                // caller did not provide a notification handle.
                if (!sourceSignal) continue;
                if (!source_) continue;

                // Fill the stopped client before restarting it. This preserves
                // the exact source stream across a network wait and avoids a
                // restart into an empty endpoint buffer.
                waitingForSource_.store(false, std::memory_order_release);
                const HRESULT fill = RenderOneBuffer();
                if (fill == kAudioSourceWouldBlock) continue;
                if (FAILED(fill)) {
                    threadFailure = fill;
                    break;
                }
                if (stopRequested_.load(std::memory_order_acquire)) break;
                const HRESULT resume = client_ ? client_->Start() : E_UNEXPECTED;
                if (FAILED(resume)) {
                    threadFailure = resume;
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
                threadFailure = wait == WAIT_FAILED
                    ? HRESULT_FROM_WIN32(GetLastError()) : E_UNEXPECTED;
                break;
            }
            const HRESULT render = RenderOneBuffer();
            if (render == kAudioSourceWouldBlock) continue;
            if (FAILED(render)) {
                threadFailure = render;
                break;
            }
        }
    } else {
        threadFailure = apartment;
        stopRequested_.store(true, std::memory_order_release);
    }

    if (FAILED(threadFailure)) {
        RecordError(threadFailure);
        state_.store(WasapiSinkState::Faulted, std::memory_order_release);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (uninitialize) CoUninitialize();
    return 0;
}

void WasapiExclusiveSink::Pause() noexcept {
    StopInternal(clock_ != nullptr);
}

void WasapiExclusiveSink::Stop() noexcept {
    StopInternal(false);
}

void WasapiExclusiveSink::StopInternal(bool preserveBuffer) noexcept {
    const auto current = state_.load(std::memory_order_acquire);
    if (current != WasapiSinkState::Running && !renderThread_ && !pausedWithBuffer_) return;
    NotifyLifecycle(WasapiSinkLifecycleEvent::StopEnter, S_OK);
    HRESULT lifecycleResult = S_OK;
    stopRequested_.store(true, std::memory_order_release);
    if (shutdownEvent_) SetEvent(shutdownEvent_);
    if (clock_ && renderThread_) {
        // Quiesce SRC before Stop/Reset so an in-flight source wake cannot
        // restart the endpoint after the control thread has stopped it.
        WaitForSingleObject(renderThread_, INFINITE);
        CloseHandle(renderThread_);
        renderThread_ = nullptr;
    }
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
    if (!preserveBuffer) waitingForSource_.store(false, std::memory_order_release);
    resumeBlocked_.store(false, std::memory_order_release);
    // The render worker may have faulted while being joined.
    const bool faulted = current == WasapiSinkState::Faulted ||
        (clock_ && state_.load(std::memory_order_acquire) == WasapiSinkState::Faulted);
    if (client_ && !faulted && !preserveBuffer) {
        const HRESULT reset = client_->Reset();
        if (FAILED(reset) && reset != AUDCLNT_E_NOT_INITIALIZED) {
            RecordError(reset);
            if (SUCCEEDED(lifecycleResult)) lifecycleResult = reset;
        }
    }
    pausedWithBuffer_ = preserveBuffer && !faulted && SUCCEEDED(lifecycleResult);
    if (clock_ && (faulted || FAILED(lifecycleResult))) {
        state_.store(WasapiSinkState::Faulted, std::memory_order_release);
    } else if (!faulted) {
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
    Release(clock_);
    drainClockTicks_ = 0;
    drainMaxWaitMs_ = 0;
    drainStartPosition_ = 0;
    drainWaitStartedTick_ = 0;
    drainPositionValid_ = false;
    pausedWithBuffer_ = false;
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
    std::fill(canonicalBuffer_.begin(), canonicalBuffer_.end(), 0);
    canonicalBuffer_.clear();
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
