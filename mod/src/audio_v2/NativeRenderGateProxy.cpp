#include "NativeRenderGateProxy.h"

#include <algorithm>
#include <new>

namespace ammod::audio_v2 {

namespace {

bool IsAppleSharedGraphClient(AUDCLNT_SHAREMODE shareMode,
                              const WAVEFORMATEX* format) noexcept {
    // Apple Music 1.6.4.90 uses a 384 kHz, 32-bit float shared client as the
    // clock/render leg for the compressed decoder. This narrow shape keeps
    // ordinary application clients on the real WASAPI path.
    return shareMode == AUDCLNT_SHAREMODE_SHARED && format &&
        format->nChannels == 2 && format->nSamplesPerSec >= 192000u &&
        format->wBitsPerSample == 32u && format->nBlockAlign == 8u;
}

class PumpAudioClock final : public IAudioClock {
public:
    PumpAudioClock() noexcept {
        QueryPerformanceFrequency(&frequency_);
        QueryPerformanceCounter(&origin_);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioClock)) {
            *object = static_cast<IAudioClock*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* frequency) override {
        if (!frequency) return E_POINTER;
        *frequency = 384000u;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPosition(UINT64* position, UINT64* qpcPosition) override {
        if (!position) return E_POINTER;
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now) || frequency_.QuadPart <= 0) {
            *position = 0;
            if (qpcPosition) *qpcPosition = 0;
            return S_OK;
        }
        const auto elapsed = std::max<LONGLONG>(0, now.QuadPart - origin_.QuadPart);
        *position = static_cast<UINT64>(
            (static_cast<unsigned long long>(elapsed) * 384000u) /
            static_cast<unsigned long long>(frequency_.QuadPart));
        if (qpcPosition) {
            *qpcPosition = static_cast<UINT64>(
                (static_cast<unsigned long long>(now.QuadPart) * 10000000ULL) /
                static_cast<unsigned long long>(frequency_.QuadPart));
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* characteristics) override {
        if (!characteristics) return E_POINTER;
        // The SDK used by the Apple Music package does not expose a named
        // fixed-clock characteristic; zero is the documented neutral value
        // and the graph only consumes the position/frequency pair here.
        *characteristics = 0;
        return S_OK;
    }

private:
    ~PumpAudioClock() = default;

    LARGE_INTEGER frequency_{};
    LARGE_INTEGER origin_{};
    std::atomic<ULONG> references_{1};
};

class PumpStreamVolume final : public IAudioStreamVolume {
public:
    PumpStreamVolume() noexcept { volumes_.fill(1.0f); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioStreamVolume)) {
            *object = static_cast<IAudioStreamVolume*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetChannelCount(UINT32* channelCount) override {
        if (!channelCount) return E_POINTER;
        *channelCount = 2;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetChannelVolume(UINT32 channel, const float volume) override {
        if (channel >= volumes_.size() || volume < 0.0f || volume > 1.0f) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        volumes_[channel] = volume;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetChannelVolume(UINT32 channel, float* volume) override {
        if (!volume) return E_POINTER;
        if (channel >= volumes_.size()) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        *volume = volumes_[channel];
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetAllVolumes(UINT32 channelCount,
                                             const float* volumes) override {
        if (!volumes) return E_POINTER;
        if (channelCount != volumes_.size()) return E_INVALIDARG;
        for (UINT32 index = 0; index < channelCount; ++index) {
            if (volumes[index] < 0.0f || volumes[index] > 1.0f) return E_INVALIDARG;
        }
        std::lock_guard lock(mutex_);
        std::copy_n(volumes, volumes_.size(), volumes_.begin());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAllVolumes(UINT32 channelCount, float* volumes) override {
        if (!volumes) return E_POINTER;
        if (channelCount != volumes_.size()) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        std::copy_n(volumes_.begin(), volumes_.size(), volumes);
        return S_OK;
    }

private:
    ~PumpStreamVolume() = default;

    std::mutex mutex_;
    std::array<float, 2> volumes_{};
    std::atomic<ULONG> references_{1};
};

class PumpSessionControl final : public IAudioSessionControl2 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioSessionControl2)) {
            *object = static_cast<IAudioSessionControl2*>(this);
            AddRef();
            return S_OK;
        }
        if (iid == __uuidof(IAudioSessionControl)) {
            *object = static_cast<IAudioSessionControl*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetState(AudioSessionState* state) override {
        if (!state) return E_POINTER;
        *state = AudioSessionStateActive;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDisplayName(LPWSTR* name) override {
        return CopyString(name, L"Apple Music");
    }

    HRESULT STDMETHODCALLTYPE SetDisplayName(LPCWSTR, LPCGUID) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetIconPath(LPWSTR* path) override {
        return CopyString(path, L"");
    }

    HRESULT STDMETHODCALLTYPE SetIconPath(LPCWSTR, LPCGUID) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetGroupingParam(GUID* grouping) override {
        if (!grouping) return E_POINTER;
        *grouping = GUID_NULL;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetGroupingParam(LPCGUID, LPCGUID) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE RegisterAudioSessionNotification(
        IAudioSessionEvents*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE UnregisterAudioSessionNotification(
        IAudioSessionEvents*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE GetSessionIdentifier(LPWSTR* identifier) override {
        return CopyString(identifier, L"AMExclusive/pump-session");
    }

    HRESULT STDMETHODCALLTYPE GetSessionInstanceIdentifier(LPWSTR* identifier) override {
        return CopyString(identifier, L"AMExclusive/pump-session-instance");
    }

    HRESULT STDMETHODCALLTYPE GetProcessId(DWORD* processId) override {
        if (!processId) return E_POINTER;
        *processId = GetCurrentProcessId();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE IsSystemSoundsSession() override { return S_FALSE; }

    HRESULT STDMETHODCALLTYPE SetDuckingPreference(BOOL) override { return S_OK; }

private:
    static HRESULT CopyString(LPWSTR* destination, LPCWSTR value) noexcept {
        if (!destination) return E_POINTER;
        *destination = nullptr;
        const auto length = value ? wcslen(value) : 0u;
        auto* copy = static_cast<LPWSTR>(CoTaskMemAlloc((length + 1u) * sizeof(wchar_t)));
        if (!copy) return E_OUTOFMEMORY;
        if (length) std::memcpy(copy, value, length * sizeof(wchar_t));
        copy[length] = L'\0';
        *destination = copy;
        return S_OK;
    }

    ~PumpSessionControl() = default;

    std::atomic<ULONG> references_{1};
};

} // namespace

void NativeRenderGateState::Add(NativeRenderGateProxy* proxy) noexcept {
    if (!proxy) return;
    std::lock_guard lock(mutex_);
    proxies_.push_back(proxy);
}

void NativeRenderGateState::Remove(NativeRenderGateProxy* proxy) noexcept {
    if (!proxy) return;
    std::lock_guard lock(mutex_);
    const auto it = std::find(proxies_.begin(), proxies_.end(), proxy);
    if (it != proxies_.end()) proxies_.erase(it);
}

void NativeRenderGateState::DetachAll() noexcept {
    std::lock_guard lock(mutex_);
    for (auto* proxy : proxies_) {
        if (proxy) proxy->DetachInner();
    }
}

void NativeRenderGateState::EnableShadowAll() noexcept {
    std::lock_guard lock(mutex_);
    for (auto* proxy : proxies_) {
        if (proxy) proxy->EnableShadowMode();
    }
}

void NativeRenderGateProxy::DetachInner() noexcept {
    std::unique_lock lock(innerMutex_);
    if (!inner_) {
        acquiredData_ = nullptr;
        acquiredFrames_ = 0;
        shadowAcquired_ = false;
        return;
    }
    // The handoff callback can run on a control thread while Apple's render
    // thread is between GetBuffer and ReleaseBuffer. Divert new calls to the
    // private shadow surface, wait for the one real endpoint packet to return,
    // and only then release the inner render service. Releasing it earlier
    // makes the next ReleaseBuffer land on a dead ownership state and can make
    // AMPLibraryAgent fast-fail with STATUS_STACK_BUFFER_OVERRUN.
    handoffPending_ = true;
    drainCv_.wait(lock, [this] { return !nativeBufferInFlight_; });
    if (acquiredData_ && acquiredFrames_ != 0) {
        // The native render thread can be between GetBuffer and ReleaseBuffer
        // while the handoff is requested. Close that outstanding packet before
        // dropping the service reference so the endpoint has no live owner.
        (void)inner_->ReleaseBuffer(acquiredFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    acquiredData_ = nullptr;
    acquiredFrames_ = 0;
    shadowAcquired_ = false;
    inner_->Release();
    inner_ = nullptr;
    shadowMode_ = true;
    handoffPending_ = false;
}

void NativeRenderGateProxy::EnableShadowMode() noexcept {
    std::lock_guard lock(innerMutex_);
    shadowMode_ = true;
    handoffPending_ = false;
    shadowAcquired_ = false;
    acquiredData_ = nullptr;
    acquiredFrames_ = 0;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::QueryInterface(REFIID iid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioClient)) {
        *object = static_cast<IAudioClient*>(this);
    } else if (iid == __uuidof(IAudioClient2) && supports2_) {
        *object = static_cast<IAudioClient2*>(this);
    } else if (iid == __uuidof(IAudioClient3) && supports3_) {
        *object = static_cast<IAudioClient3*>(this);
    } else {
        std::lock_guard lock(innerMutex_);
        return inner_ ? inner_->QueryInterface(iid, object) : E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE NativeAudioClientProxy::Release() {
    const ULONG remaining = --references_;
    if (!remaining) delete this;
    return remaining;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::Initialize(
    AUDCLNT_SHAREMODE shareMode, DWORD flags, REFERENCE_TIME duration,
    REFERENCE_TIME periodicity, const WAVEFORMATEX* format, LPCGUID sessionGuid) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (IsAppleSharedGraphClient(shareMode, format)) {
            // Do not initialize Apple's shared endpoint owner. The decoder
            // still receives a valid client lifecycle below, while its render
            // service is a private software surface driven by pumpEvent_.
            pumpMode_ = true;
            pumpBufferFrames_ = 8448u;
            if (format) blockAlign_.store(format->nBlockAlign, std::memory_order_release);
            result = S_OK;
        } else if (inner_) {
            result = inner_->Initialize(shareMode, flags, duration, periodicity,
                                        format, sessionGuid);
        } else if (pumpMode_) {
            result = S_OK;
        }
    }
    if (SUCCEEDED(result) && format) {
        blockAlign_.store(format->nBlockAlign, std::memory_order_release);
    }
    if (callbacks_.onInitialize) {
        callbacks_.onInitialize(callbacks_.context, this, endpoint_.c_str(), shareMode,
                                 flags, format, result);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetBufferSize(UINT32* frames) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            if (!frames) result = E_POINTER;
            else {
                *frames = pumpBufferFrames_;
                result = pumpBufferFrames_ != 0 ? S_OK : AUDCLNT_E_NOT_INITIALIZED;
            }
        } else if (inner_) {
            result = inner_->GetBufferSize(frames);
        }
    }
    Trace(L"GetBufferSize", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetStreamLatency(REFERENCE_TIME* latency) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            if (!latency) result = E_POINTER;
            else {
                // The Apple graph asks for this immediately after the
                // software-backed Initialize.  The real inner client is
                // deliberately not initialized in pump mode, so forwarding
                // here would return AUDCLNT_E_NOT_INITIALIZED and Apple
                // tears the graph down before requesting its render service.
                const auto frames = pumpBufferFrames_ ? pumpBufferFrames_ : 8448u;
                const auto rate = 384000u;
                *latency = static_cast<REFERENCE_TIME>(
                    (10000000LL * frames + rate - 1u) / rate);
                result = S_OK;
            }
        } else {
            result = inner_ ? inner_->GetStreamLatency(latency) : AUDCLNT_E_NOT_INITIALIZED;
        }
    }
    Trace(L"GetStreamLatency", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetCurrentPadding(UINT32* frames) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            if (!frames) result = E_POINTER;
            else {
                // Apple's event-driven shared client reports the part of the
                // 8448-frame ring that is already occupied.  The observed
                // post-Start render quantum is 3840 frames, so exposing the
                // remaining 4608 frames makes its GetBuffer request match
                // the native graph instead of asking AudioUnitRender for the
                // whole ring (which returns -10874).
                *frames = pumpStarted_ && pumpBufferFrames_ > 3840u
                    ? pumpBufferFrames_ - 3840u
                    : 0u;
                result = S_OK;
            }
        } else if (inner_) {
            result = inner_->GetCurrentPadding(frames);
        }
    }
    Trace(L"GetCurrentPadding", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::IsFormatSupported(
    AUDCLNT_SHAREMODE mode, const WAVEFORMATEX* format, WAVEFORMATEX** closest) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (IsAppleSharedGraphClient(mode, format)) result = S_OK;
        else result = inner_ ? inner_->IsFormatSupported(mode, format, closest)
                             : AUDCLNT_E_NOT_INITIALIZED;
    }
    Trace(L"IsFormatSupported", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetMixFormat(WAVEFORMATEX** format) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        result = inner_ ? inner_->GetMixFormat(format) : AUDCLNT_E_NOT_INITIALIZED;
    }
    Trace(L"GetMixFormat", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetDevicePeriod(
    REFERENCE_TIME* defaultPeriod, REFERENCE_TIME* minimumPeriod) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        result = inner_ ? inner_->GetDevicePeriod(defaultPeriod, minimumPeriod)
                        : AUDCLNT_E_NOT_INITIALIZED;
    }
    Trace(L"GetDevicePeriod", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::Start() {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            pumpStarted_ = true;
            result = S_OK;
        }
        else if (inner_) result = inner_->Start();
    }
    if (SUCCEEDED(result)) StartPumpThread();
    if (callbacks_.onStart) callbacks_.onStart(callbacks_.context, this, result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::Stop() {
    // Natural streaming EOF is not guaranteed to produce a final zero-packet
    // Fill before Apple asks the software-backed client to Stop. Give the
    // existing native scheduler a brief chance to finish its producer tail
    // while the pump is still alive. Explicit pause/seek/skip paths are
    // rejected by the runtime callback and return immediately.
    if (callbacks_.beforeStop) {
        (void)callbacks_.beforeStop(callbacks_.context, this);
    }

    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            pumpStarted_ = false;
            result = S_OK;
        }
        else if (inner_) result = inner_->Stop();
    }
    if (pumpMode_) StopPumpThread();
    if (callbacks_.onStop) callbacks_.onStop(callbacks_.context, this, result);
    Trace(L"Stop", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::Reset() {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            pumpStarted_ = false;
            result = S_OK;
        }
        else if (inner_) result = inner_->Reset();
    }
    if (callbacks_.onReset) callbacks_.onReset(callbacks_.context, this, result);
    Trace(L"Reset", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::SetEventHandle(HANDLE eventHandle) {
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_) {
            pumpEvent_.store(eventHandle, std::memory_order_release);
            result = eventHandle ? S_OK : E_INVALIDARG;
        } else if (inner_) {
            result = inner_->SetEventHandle(eventHandle);
        }
    }
    Trace(L"SetEventHandle", result);
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetService(REFIID iid, void** service) {
    if (!service) {
        TraceService(iid, E_POINTER);
        return E_POINTER;
    }
    *service = nullptr;
    const wchar_t* serviceName =
        iid == __uuidof(IAudioRenderClient) ? L"GetService[IAudioRenderClient]" :
        iid == __uuidof(IAudioClock) ? L"GetService[IAudioClock]" :
        iid == __uuidof(IAudioStreamVolume) ? L"GetService[IAudioStreamVolume]" :
        iid == __uuidof(IAudioSessionControl) ? L"GetService[IAudioSessionControl]" :
        iid == __uuidof(IAudioSessionControl2) ? L"GetService[IAudioSessionControl2]" :
        L"GetService[other]";
    HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
    bool softwareRender = false;
    bool softwareClock = false;
    bool softwareSession = false;
    {
        std::lock_guard lock(innerMutex_);
        if (pumpMode_ && iid == __uuidof(IAudioRenderClient)) {
            result = S_OK;
            softwareRender = true;
        } else if (pumpMode_ && iid == __uuidof(IAudioClock)) {
            if (!pumpClock_) pumpClock_ = new (std::nothrow) PumpAudioClock();
            if (pumpClock_) {
                pumpClock_->AddRef();
                *service = pumpClock_;
                result = S_OK;
                softwareClock = true;
            } else {
                result = E_OUTOFMEMORY;
            }
        } else if (pumpMode_ && iid == __uuidof(IAudioStreamVolume)) {
            if (!pumpVolume_) pumpVolume_ = new (std::nothrow) PumpStreamVolume();
            if (pumpVolume_) {
                pumpVolume_->AddRef();
                *service = pumpVolume_;
                result = S_OK;
            } else {
                result = E_OUTOFMEMORY;
            }
        } else if (pumpMode_ && (iid == __uuidof(IAudioSessionControl) ||
                                 iid == __uuidof(IAudioSessionControl2))) {
            if (!pumpSession_) pumpSession_ = new (std::nothrow) PumpSessionControl();
            if (pumpSession_) {
                pumpSession_->AddRef();
                *service = iid == __uuidof(IAudioSessionControl2)
                    ? static_cast<IAudioSessionControl2*>(pumpSession_)
                    : static_cast<IAudioSessionControl*>(pumpSession_);
                result = S_OK;
                softwareSession = true;
            } else {
                result = E_OUTOFMEMORY;
            }
        } else if (inner_) {
            result = inner_->GetService(iid, service);
        }
    }
    if (softwareClock) {
        TraceService(iid, S_OK);
        Trace(serviceName, S_OK);
        return S_OK;
    }
    if (softwareSession) {
        TraceService(iid, S_OK);
        Trace(serviceName, S_OK);
        return S_OK;
    }
    if (softwareRender) {
        auto* proxy = new (std::nothrow) NativeRenderGateProxy(
            nullptr, callbacks_, this, blockAlign_.load(std::memory_order_acquire), renderState_);
        if (!proxy) {
            TraceService(iid, E_OUTOFMEMORY);
            Trace(serviceName, E_OUTOFMEMORY);
            return E_OUTOFMEMORY;
        }
        proxy->EnableShadowMode();
        *service = static_cast<IAudioRenderClient*>(proxy);
        TraceService(iid, S_OK);
        Trace(serviceName, S_OK);
        return S_OK;
    }
    if (FAILED(result) || !*service || iid != __uuidof(IAudioRenderClient)) {
        TraceService(iid, result);
        Trace(serviceName, result);
        return result;
    }

    auto* render = static_cast<IAudioRenderClient*>(*service);
    auto* proxy = new (std::nothrow) NativeRenderGateProxy(
        render, callbacks_, this, blockAlign_.load(std::memory_order_acquire), renderState_);
    if (!proxy) {
        render->Release();
        *service = nullptr;
        TraceService(iid, E_OUTOFMEMORY);
        Trace(serviceName, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    *service = static_cast<IAudioRenderClient*>(proxy);
    TraceService(iid, S_OK);
    Trace(serviceName, S_OK);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::IsOffloadCapable(
    AUDIO_STREAM_CATEGORY category, BOOL* capable) {
    auto* client = Inner2();
    if (!client) return E_NOINTERFACE;
    const HRESULT result = client->IsOffloadCapable(category, capable);
    client->Release();
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::SetClientProperties(
    const AudioClientProperties* properties) {
    auto* client = Inner2();
    if (!client) return E_NOINTERFACE;
    const HRESULT result = client->SetClientProperties(properties);
    client->Release();
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetBufferSizeLimits(
    const WAVEFORMATEX* format, BOOL eventDriven, REFERENCE_TIME* minimum,
    REFERENCE_TIME* maximum) {
    auto* client = Inner2();
    if (!client) return E_NOINTERFACE;
    const HRESULT result = client->GetBufferSizeLimits(format, eventDriven, minimum, maximum);
    client->Release();
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetSharedModeEnginePeriod(
    const WAVEFORMATEX* format, UINT32* defaultFrames, UINT32* fundamentalFrames,
    UINT32* minimumFrames, UINT32* maximumFrames) {
    auto* client = Inner3();
    if (!client) return E_NOINTERFACE;
    const HRESULT result = client->GetSharedModeEnginePeriod(
        format, defaultFrames, fundamentalFrames, minimumFrames, maximumFrames);
    client->Release();
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::GetCurrentSharedModeEnginePeriod(
    WAVEFORMATEX** format, UINT32* frames) {
    auto* client = Inner3();
    if (!client) return E_NOINTERFACE;
    const HRESULT result = client->GetCurrentSharedModeEnginePeriod(format, frames);
    client->Release();
    return result;
}

HRESULT STDMETHODCALLTYPE NativeAudioClientProxy::InitializeSharedAudioStream(
    DWORD flags, UINT32 periodFrames, const WAVEFORMATEX* format, LPCGUID sessionGuid) {
    auto* client = Inner3();
    if (!client) return E_NOINTERFACE;
    const HRESULT result = client->InitializeSharedAudioStream(
        flags, periodFrames, format, sessionGuid);
    client->Release();
    if (SUCCEEDED(result) && format) {
        blockAlign_.store(format->nBlockAlign, std::memory_order_release);
    }
    if (callbacks_.onInitialize) {
        // The callback receives the ordinary Initialize ABI. A shared stream
        // still has a concrete WAVEFORMATEX and must participate in the same
        // native-client lifecycle bookkeeping.
        callbacks_.onInitialize(callbacks_.context, this, endpoint_.c_str(),
                                 AUDCLNT_SHAREMODE_SHARED, flags, format, result);
    }
    return result;
}

IAudioClient2* NativeAudioClientProxy::Inner2() const noexcept {
    std::lock_guard lock(innerMutex_);
    if (!inner_ || !supports2_) return nullptr;
    IAudioClient2* client{};
    return SUCCEEDED(inner_->QueryInterface(
        __uuidof(IAudioClient2), reinterpret_cast<void**>(&client))) ? client : nullptr;
}

IAudioClient3* NativeAudioClientProxy::Inner3() const noexcept {
    std::lock_guard lock(innerMutex_);
    if (!inner_ || !supports3_) return nullptr;
    IAudioClient3* client{};
    return SUCCEEDED(inner_->QueryInterface(
        __uuidof(IAudioClient3), reinterpret_cast<void**>(&client))) ? client : nullptr;
}

void NativeAudioClientProxy::Trace(const wchar_t* method, HRESULT result) noexcept {
    if (callbacks_.onMethod) callbacks_.onMethod(callbacks_.context, this, method, result);
}

void NativeAudioClientProxy::TraceService(REFIID iid, HRESULT result) noexcept {
    if (callbacks_.onService) callbacks_.onService(callbacks_.context, this, iid, result);
}

NativeAudioClientProxy::~NativeAudioClientProxy() {
    StopPumpThread();
    IAudioClient* inner{};
    {
        std::lock_guard lock(innerMutex_);
        inner = inner_;
        inner_ = nullptr;
    }
    if (inner) inner->Release();
    if (pumpClock_) {
        pumpClock_->Release();
        pumpClock_ = nullptr;
    }
    if (pumpVolume_) {
        pumpVolume_->Release();
        pumpVolume_ = nullptr;
    }
    if (pumpSession_) {
        pumpSession_->Release();
        pumpSession_ = nullptr;
    }
}

DWORD WINAPI NativeAudioClientProxy::PumpThreadThunk(void* context) noexcept {
    auto* self = static_cast<NativeAudioClientProxy*>(context);
    if (!self) return 0;

    const HANDLE stop = self->pumpStopEvent_;
    if (!stop) return 0;

    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!timer) {
        WaitForSingleObject(stop, INFINITE);
        return 0;
    }

    const HANDLE waitHandles[] = {stop, timer};
    for (;;) {
        const auto requestedMs = self->callbacks_.pumpPeriodMs
            ? self->callbacks_.pumpPeriodMs(self->callbacks_.context)
            : 10u;
        const auto periodMs = std::clamp<std::uint32_t>(requestedMs, 8u, 12u);
        LARGE_INTEGER dueTime{};
        dueTime.QuadPart = -static_cast<LONGLONG>(periodMs) * 10'000;
        if (!SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE)) break;

        const DWORD waitResult = WaitForMultipleObjects(
            ARRAYSIZE(waitHandles), waitHandles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult != WAIT_OBJECT_0 + 1) break;
        const auto eventHandle = self->pumpEvent_.load(std::memory_order_acquire);
        if (eventHandle) SetEvent(eventHandle);
    }

    CancelWaitableTimer(timer);
    CloseHandle(timer);
    return 0;
}

void NativeAudioClientProxy::StartPumpThread() noexcept {
    std::lock_guard lock(innerMutex_);
    if (!pumpMode_ || pumpThread_ || !pumpEvent_.load(std::memory_order_acquire)) return;
    pumpStopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!pumpStopEvent_) return;
    pumpThread_ = CreateThread(nullptr, 0, &NativeAudioClientProxy::PumpThreadThunk,
                               this, 0, nullptr);
    if (!pumpThread_) {
        CloseHandle(pumpStopEvent_);
        pumpStopEvent_ = nullptr;
    }
}

void NativeAudioClientProxy::StopPumpThread() noexcept {
    HANDLE stop{};
    HANDLE thread{};
    {
        std::lock_guard lock(innerMutex_);
        stop = pumpStopEvent_;
        thread = pumpThread_;
    }
    if (stop) SetEvent(stop);
    if (thread) WaitForSingleObject(thread, INFINITE);
    {
        std::lock_guard lock(innerMutex_);
        if (pumpThread_ == thread) pumpThread_ = nullptr;
        if (pumpStopEvent_ == stop) pumpStopEvent_ = nullptr;
    }
    if (thread) CloseHandle(thread);
    if (stop) CloseHandle(stop);
}

HRESULT NativeAudioClientProxy::PrepareForExclusiveHandoff() noexcept {
    {
        std::lock_guard lock(innerMutex_);
        if (inner_) {
            (void)inner_->GetBufferSize(&pumpBufferFrames_);
        }
    }

    // The parent AudioUnit remains Started and the native client is not
    // Stop/Reset-ed here: those calls tear down the decoder's source clock.
    // Drop the inner client/service references while the proxy stays alive,
    // then expose a bounded no-endpoint render surface so the parent can keep
    // driving Apple's decoder without retaining the physical endpoint.
    renderState_->DetachAll();
    FinalizeExclusiveHandoff();
    {
        std::lock_guard lock(innerMutex_);
        pumpMode_ = true;
    }
    renderState_->EnableShadowAll();
    return S_OK;
}

void NativeAudioClientProxy::FinalizeExclusiveHandoff() noexcept {
    renderState_->DetachAll();
    IAudioClient* inner{};
    {
        std::lock_guard lock(innerMutex_);
        inner = inner_;
        inner_ = nullptr;
    }
    if (inner) inner->Release();
}

} // namespace ammod::audio_v2
