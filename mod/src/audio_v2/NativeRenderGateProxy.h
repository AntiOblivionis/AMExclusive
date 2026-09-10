#pragma once

#include <audioclient.h>
#include <audiopolicy.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ammod::audio_v2 {

// The exclusive sink activates its own client through the raw MMDevice
// interface. This guard prevents the production client wrapper from wrapping
// that internal client again when the v2 sink opens the endpoint.
inline thread_local std::uint32_t g_nativeProxyBypassDepth{};

class ScopedNativeProxyBypass final {
public:
    ScopedNativeProxyBypass() noexcept { ++g_nativeProxyBypassDepth; }
    ~ScopedNativeProxyBypass() { --g_nativeProxyBypassDepth; }
    ScopedNativeProxyBypass(const ScopedNativeProxyBypass&) = delete;
    ScopedNativeProxyBypass& operator=(const ScopedNativeProxyBypass&) = delete;
};

inline bool NativeProxyBypassed() noexcept {
    return g_nativeProxyBypassDepth != 0;
}

struct NativeAudioCallbacks final {
    void* context{};
    void (*onInitialize)(void* context, void* client, const wchar_t* endpoint,
                         AUDCLNT_SHAREMODE shareMode, DWORD flags,
                         const WAVEFORMATEX* format, HRESULT result) noexcept{};
    void (*onStart)(void* context, void* client, HRESULT result) noexcept{};
    bool (*beforeStop)(void* context, void* client) noexcept{};
    void (*onStop)(void* context, void* client, HRESULT result) noexcept{};
    void (*onReset)(void* context, void* client, HRESULT result) noexcept{};
    bool (*shouldSuppress)(void* context, void* client) noexcept{};
    void (*onGetBuffer)(void* context, void* client, UINT32 frames,
                        HRESULT result, bool shadow) noexcept{};
    void (*onReleaseBuffer)(void* context, void* client, UINT32 frames,
                            HRESULT result, bool shadow) noexcept{};
    void (*onMethod)(void* context, void* client, const wchar_t* method,
                     HRESULT result) noexcept{};
    void (*onService)(void* context, void* client, REFIID serviceIid,
                      HRESULT result) noexcept{};
    std::uint32_t (*pumpPeriodMs)(void* context) noexcept{};
};

class NativeRenderGateProxy;

// The client proxy can outlive its IAudioClient inner object during the
// endpoint handoff. Keep all render-service proxies in a shared registry so
// the last service reference is released together with the client reference;
// otherwise WASAPI may continue reporting AUDCLNT_E_DEVICE_IN_USE.
class NativeRenderGateState final {
public:
    void Add(NativeRenderGateProxy* proxy) noexcept;
    void Remove(NativeRenderGateProxy* proxy) noexcept;
    void DetachAll() noexcept;
    void EnableShadowAll() noexcept;

private:
    std::mutex mutex_;
    std::vector<NativeRenderGateProxy*> proxies_;
};

class NativeRenderGateProxy final : public IAudioRenderClient {
public:
    NativeRenderGateProxy(IAudioRenderClient* inner, NativeAudioCallbacks callbacks,
                          void* clientIdentity, UINT32 bytesPerFrame,
                          std::shared_ptr<NativeRenderGateState> state) noexcept
        : inner_(inner), callbacks_(callbacks), clientIdentity_(clientIdentity),
          bytesPerFrame_(bytesPerFrame), state_(std::move(state)) {
        if (state_) state_->Add(this);
    }

    NativeRenderGateProxy(const NativeRenderGateProxy&) = delete;
    NativeRenderGateProxy& operator=(const NativeRenderGateProxy&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioRenderClient)) {
            *object = static_cast<IAudioRenderClient*>(this);
            AddRef();
            return S_OK;
        }
        std::lock_guard lock(innerMutex_);
        return inner_ ? inner_->QueryInterface(iid, object) : AUDCLNT_E_NOT_INITIALIZED;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames, BYTE** data) override {
        if (data) *data = nullptr;
        // Preserve Apple's actual GetBuffer call and HRESULT.  Returning a
        // synthetic S_OK with a private shadow buffer changes the native
        // scheduler's ownership/padding state and was observed to collapse
        // the render cadence to roughly one call every two seconds.  The
        // native client must see the same success/failure as Apple sees.
        HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
        bool shadow = false;
        {
            std::lock_guard lock(innerMutex_);
            shadow = shadowMode_ || handoffPending_;
            if (shadow) {
                const auto bytesPerFrame = std::max<UINT32>(1u, bytesPerFrame_);
                const bool fits = frames != 0 &&
                    frames <= shadowBuffer_.size() / bytesPerFrame;
                if (!data) {
                    result = E_POINTER;
                } else if (!fits) {
                    result = AUDCLNT_E_BUFFER_SIZE_ERROR;
                } else {
                    std::memset(shadowBuffer_.data(), 0,
                                static_cast<std::size_t>(frames) * bytesPerFrame);
                    *data = shadowBuffer_.data();
                    acquiredData_ = *data;
                    acquiredFrames_ = frames;
                    shadowAcquired_ = true;
                    result = S_OK;
                }
            } else if (inner_) {
                result = inner_->GetBuffer(frames, data);
                if (SUCCEEDED(result) && data && *data) {
                    acquiredData_ = *data;
                    acquiredFrames_ = frames;
                    nativeBufferInFlight_ = true;
                }
            }
        }
        if (callbacks_.onGetBuffer) {
            callbacks_.onGetBuffer(callbacks_.context, clientIdentity_, frames,
                                   result, shadow);
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames, DWORD flags) override {
        DWORD effectiveFlags = flags;
        bool shadow = false;
        const bool suppress = ShouldSuppress();
        HRESULT result = AUDCLNT_E_NOT_INITIALIZED;
        {
            std::lock_guard lock(innerMutex_);
            shadow = shadowMode_;
            if (shadow) {
                result = shadowAcquired_ && acquiredFrames_ == frames
                    ? S_OK : AUDCLNT_E_BUFFER_ERROR;
            } else if (inner_) {
                if (suppress && acquiredData_ && acquiredFrames_ == frames &&
                    bytesPerFrame_ != 0) {
                    // Keep the real native ownership/padding transition, but prevent
                    // the native stream from leaking samples after the exclusive
                    // cutover.  Do not replace the call with a fake ReleaseBuffer.
                    const auto byteCount = static_cast<std::size_t>(frames) * bytesPerFrame_;
                    std::memset(acquiredData_, 0, byteCount);
                }
                result = inner_->ReleaseBuffer(frames, effectiveFlags);
                if (nativeBufferInFlight_ && acquiredFrames_ == frames) {
                    nativeBufferInFlight_ = false;
                    drainCv_.notify_all();
                }
            }
        }
        if (callbacks_.onReleaseBuffer) {
            callbacks_.onReleaseBuffer(callbacks_.context, clientIdentity_, frames,
                                       result, shadow);
        }
        acquiredData_ = nullptr;
        acquiredFrames_ = 0;
        shadowAcquired_ = false;
        return result;
    }

    bool ShouldSuppress() const noexcept {
        return callbacks_.shouldSuppress &&
            callbacks_.shouldSuppress(callbacks_.context, clientIdentity_);
    }

    void DetachInner() noexcept;
    void EnableShadowMode() noexcept;

private:
    ~NativeRenderGateProxy() {
        if (state_) state_->Remove(this);
        std::lock_guard lock(innerMutex_);
        if (inner_) inner_->Release();
    }

    mutable std::mutex innerMutex_;
    std::condition_variable drainCv_;
    IAudioRenderClient* inner_{};
    NativeAudioCallbacks callbacks_{};
    void* clientIdentity_{};
    UINT32 bytesPerFrame_{};
    BYTE* acquiredData_{};
    UINT32 acquiredFrames_{};
    bool shadowMode_{};
    bool handoffPending_{};
    bool shadowAcquired_{};
    bool nativeBufferInFlight_{};
    std::array<BYTE, 256u * 1024u> shadowBuffer_{};
    std::shared_ptr<NativeRenderGateState> state_;
    std::atomic<ULONG> references_{1};
};

class NativeAudioClientProxy final : public IAudioClient3 {
public:
    NativeAudioClientProxy(IAudioClient* inner, NativeAudioCallbacks callbacks,
                           std::wstring endpoint)
        : inner_(inner), callbacks_(callbacks), endpoint_(std::move(endpoint)) {
        if (inner_) {
            IAudioClient2* client2{};
            supports2_ = SUCCEEDED(inner_->QueryInterface(
                __uuidof(IAudioClient2), reinterpret_cast<void**>(&client2)));
            if (client2) client2->Release();
            IAudioClient3* client3{};
            supports3_ = SUCCEEDED(inner_->QueryInterface(
                __uuidof(IAudioClient3), reinterpret_cast<void**>(&client3)));
            if (client3) client3->Release();
        }
    }

    NativeAudioClientProxy(const NativeAudioClientProxy&) = delete;
    NativeAudioClientProxy& operator=(const NativeAudioClientProxy&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE shareMode, DWORD flags,
                                         REFERENCE_TIME duration, REFERENCE_TIME periodicity,
                                         const WAVEFORMATEX* format,
                                         LPCGUID sessionGuid) override;
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override;
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency) override;
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* frames) override;
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE mode,
                                                const WAVEFORMATEX* format,
                                                WAVEFORMATEX** closest) override;
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override;
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* defaultPeriod,
                                               REFERENCE_TIME* minimumPeriod) override;
    HRESULT STDMETHODCALLTYPE Start() override;
    HRESULT STDMETHODCALLTYPE Stop() override;
    HRESULT STDMETHODCALLTYPE Reset() override;
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE eventHandle) override;
    HRESULT STDMETHODCALLTYPE GetService(REFIID iid, void** service) override;
    HRESULT STDMETHODCALLTYPE IsOffloadCapable(AUDIO_STREAM_CATEGORY category,
                                               BOOL* capable) override;
    HRESULT STDMETHODCALLTYPE SetClientProperties(
        const AudioClientProperties* properties) override;
    HRESULT STDMETHODCALLTYPE GetBufferSizeLimits(const WAVEFORMATEX* format,
                                                  BOOL eventDriven,
                                                  REFERENCE_TIME* minimum,
                                                  REFERENCE_TIME* maximum) override;
    HRESULT STDMETHODCALLTYPE GetSharedModeEnginePeriod(const WAVEFORMATEX* format,
                                                        UINT32* defaultFrames,
                                                        UINT32* fundamentalFrames,
                                                        UINT32* minimumFrames,
                                                        UINT32* maximumFrames) override;
    HRESULT STDMETHODCALLTYPE GetCurrentSharedModeEnginePeriod(WAVEFORMATEX** format,
                                                               UINT32* frames) override;
    HRESULT STDMETHODCALLTYPE InitializeSharedAudioStream(DWORD flags, UINT32 periodFrames,
                                                          const WAVEFORMATEX* format,
                                                          LPCGUID sessionGuid) override;

    // Stop/reset the native client before v2 claims the endpoint. The parent
    // AudioUnit remains started so it can continue driving Apple's decoder;
    // after the inner client/service references are detached, its render
    // service is switched to a bounded no-endpoint pump surface.
    HRESULT PrepareForExclusiveHandoff() noexcept;
    void FinalizeExclusiveHandoff() noexcept;

private:
    ~NativeAudioClientProxy();
    void Trace(const wchar_t* method, HRESULT result) noexcept;
    void TraceService(REFIID iid, HRESULT result) noexcept;
    IAudioClient2* Inner2() const noexcept;
    IAudioClient3* Inner3() const noexcept;
    static DWORD WINAPI PumpThreadThunk(void* context) noexcept;
    void StartPumpThread() noexcept;
    void StopPumpThread() noexcept;

    mutable std::mutex innerMutex_;
    IAudioClient* inner_{};
    NativeAudioCallbacks callbacks_{};
    std::wstring endpoint_;
    std::shared_ptr<NativeRenderGateState> renderState_ =
        std::make_shared<NativeRenderGateState>();
    bool supports2_{};
    bool supports3_{};
    std::atomic<UINT32> blockAlign_{};
    UINT32 pumpSampleRate_{};
    UINT32 pumpQuantumFrames_{};
    UINT32 pumpBufferFrames_{};
    bool pumpMode_{};
    bool pumpStarted_{};
    IAudioClock* pumpClock_{};
    IAudioStreamVolume* pumpVolume_{};
    IAudioSessionControl2* pumpSession_{};
    std::atomic<HANDLE> pumpEvent_{};
    HANDLE pumpStopEvent_{};
    HANDLE pumpThread_{};
    std::atomic<ULONG> references_{1};
};

} // namespace ammod::audio_v2
