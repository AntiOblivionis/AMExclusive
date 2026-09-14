#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <MinHook.h>
#include <intrin.h>

#include "BoundedLog.h"
#include "IpcProtocol.h"
#include "IpcTransport.h"
#include "AudioFormatPolicy.h"
#include "ApplePrivateOffsets.h"
#include "LosslessQualityLock.h"
#include "LosslessQualityPolicy.h"
#include "audio_v2/AudioCoreGate.h"
#include "audio_v2/AudioCoreRuntime.h"
#include "audio_v2/NativeRenderGateProxy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <deque>
#include <iomanip>
#include <mutex>
#include <memory>
#include <new>
#include <numbers>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

HMODULE g_module{};
std::filesystem::path g_iniPath;
std::filesystem::path g_logPath;
std::mutex g_logMutex;

using CoCreateInstanceFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
using EnumAudioEndpointsFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, DWORD, IMMDeviceCollection**);
using GetDefaultEndpointFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, EDataFlow, ERole, IMMDevice**);
using GetDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceEnumerator*, LPCWSTR, IMMDevice**);
using CollectionItemFn = HRESULT(STDMETHODCALLTYPE*)(IMMDeviceCollection*, UINT, IMMDevice**);
using DeviceActivateFn = HRESULT(STDMETHODCALLTYPE*)(IMMDevice*, REFIID, DWORD, PROPVARIANT*, void**);

// Apple CoreAudioToolbox ABI. Format hooks observe descriptors; the production
// v2 path also performs bounded, read-only P1 signature sampling after Apple's
// AudioUnitRender returns and copies the validated P0 output into its queue.
using AppleOSStatus = std::int32_t;
struct AudioStreamBasicDescription {
    double mSampleRate;
    std::uint32_t mFormatID;
    std::uint32_t mFormatFlags;
    std::uint32_t mBytesPerPacket;
    std::uint32_t mFramesPerPacket;
    std::uint32_t mBytesPerFrame;
    std::uint32_t mChannelsPerFrame;
    std::uint32_t mBitsPerChannel;
    std::uint32_t mReserved;
};
static_assert(sizeof(AudioStreamBasicDescription) == 40);

struct AppleCMTime {
    std::int64_t value;
    std::int32_t timescale;
    std::uint32_t flags;
    std::int64_t epoch;
};
static_assert(sizeof(AppleCMTime) == 24);

struct AppleSmpteTime {
    std::int16_t mSubframes;
    std::int16_t mSubframeDivisor;
    std::uint32_t mCounter;
    std::uint32_t mType;
    std::uint32_t mFlags;
    std::int16_t mHours;
    std::int16_t mMinutes;
    std::int16_t mSeconds;
    std::int16_t mFrames;
};
struct AppleAudioTimeStamp {
    double mSampleTime;
    std::uint64_t mHostTime;
    double mRateScalar;
    std::uint64_t mWordClockTime;
    AppleSmpteTime mSmpteTime;
    std::uint32_t mFlags;
    std::uint32_t mReserved;
};
struct AppleAudioBuffer {
    std::uint32_t mNumberChannels;
    std::uint32_t mDataByteSize;
    void* mData;
};
struct AppleAudioBufferList {
    std::uint32_t mNumberBuffers;
    std::uint32_t mReserved;
    AppleAudioBuffer mBuffers[1];
};
struct AppleAudioStreamPacketDescription {
    std::int64_t mStartOffset;
    std::uint32_t mVariableFramesInPacket;
    std::uint32_t mDataByteSize;
};
static_assert(sizeof(AppleAudioTimeStamp) == 64);
static_assert(sizeof(AppleAudioBufferList) == 24);

using AudioConverterNewFn = AppleOSStatus(__cdecl*)(
    const AudioStreamBasicDescription*, const AudioStreamBasicDescription*, void**);
using AudioConverterNewSpecificFn = AppleOSStatus(__cdecl*)(
    const AudioStreamBasicDescription*, const AudioStreamBasicDescription*, std::uint32_t,
    const void*, void**);
using AudioConverterSetPropertyFn = AppleOSStatus(__cdecl*)(
    void*, std::uint32_t, std::uint32_t, const void*);
using AudioConverterGetPropertyFn = AppleOSStatus(__cdecl*)(
    void*, std::uint32_t, std::uint32_t*, void*);
using AudioConverterComplexInputDataProcFn = AppleOSStatus(__cdecl*)(
    void*, std::uint32_t*, void*, void**, void*);
using AudioConverterFillComplexBufferFn = AppleOSStatus(__cdecl*)(
    void*, AudioConverterComplexInputDataProcFn, void*, std::uint32_t*, void*, void*);
using AudioConverterResetFn = AppleOSStatus(__cdecl*)(void*);
using AudioConverterDisposeFn = AppleOSStatus(__cdecl*)(void*);
using AudioUnitSetPropertyFn = AppleOSStatus(__cdecl*)(
    void*, std::uint32_t, std::uint32_t, std::uint32_t, const void*, std::uint32_t);
using AudioOutputUnitStartFn = AppleOSStatus(__cdecl*)(void*);
using AudioOutputUnitStopFn = AppleOSStatus(__cdecl*)(void*);
using AudioUnitInitializeFn = AppleOSStatus(__cdecl*)(void*);
using AudioUnitUninitializeFn = AppleOSStatus(__cdecl*)(void*);
using AudioUnitRenderFn = AppleOSStatus(__cdecl*)(
    void*, std::uint32_t*, const void*, std::uint32_t, std::uint32_t, void*);
using AVCFSetPreferredMaximumSampleRateFn = void(__cdecl*)(void*, double);
using AVCFSetVariantPreferencesFn = void(__cdecl*)(void*, std::uint32_t);
using FigAlternateEligibleLosslessFilterCreateFn = AppleOSStatus(__cdecl*)(
    void*, void*, void*);
using FigAlternateAllowableMediaSubtypeFilterCreateFn = AppleOSStatus(__cdecl*)(
    void*, void*, void*, void**);
using CFNumberCreateFn = void*(__cdecl*)(void*, std::int32_t, const void*);
using CFArrayCreateFn = void*(__cdecl*)(void*, const void**, std::int64_t, const void*);
using CFReleaseFn = void(__cdecl*)(const void*);
using CFStringCreateWithCStringFn = void*(__cdecl*)(void*, const char*, std::uint32_t);
using CFPreferencesGetAppIntegerValueFn = std::int64_t(__cdecl*)(
    const void*, const void*, unsigned char*);
using CFPreferencesGetAppBooleanValueFn = unsigned char(__cdecl*)(
    const void*, const void*, unsigned char*);

AVCFSetPreferredMaximumSampleRateFn g_originalAVCFSetPreferredMaximumSampleRate{};
AVCFSetVariantPreferencesFn g_originalAVCFSetVariantPreferences{};
FigAlternateEligibleLosslessFilterCreateFn
    g_originalFigAlternateEligibleLosslessFilterCreate{};
FigAlternateAllowableMediaSubtypeFilterCreateFn
    g_figAlternateAllowableMediaSubtypeFilterCreate{};
CFNumberCreateFn g_cfNumberCreate{};
CFArrayCreateFn g_cfArrayCreate{};
CFReleaseFn g_cfRelease{};
CFStringCreateWithCStringFn g_cfStringCreateWithCString{};
CFPreferencesGetAppIntegerValueFn g_cfPreferencesGetAppIntegerValue{};
CFPreferencesGetAppBooleanValueFn g_cfPreferencesGetAppBooleanValue{};
void* g_cfTypeArrayCallbacks{};
void* g_cfPreferencesCurrentApplication{};
void* g_streamQualityPreferenceKey{};
void* g_losslessEnabledPreferenceKey{};
std::atomic<std::uint32_t> g_lastVariantPreferences{};
std::atomic<std::uint16_t> g_configuredStreamQuality{};
std::atomic<bool> g_configuredLosslessEnabled{};
std::atomic<bool> g_configuredQualityKnown{};

std::mutex g_hookMutex;
std::mutex g_pipeMutex;
std::mutex g_outboundMutex;
std::deque<ammod::ipc::Message> g_outbound;
HANDLE g_pipe{INVALID_HANDLE_VALUE};
std::atomic<bool> g_ipcConnected{};
std::atomic<bool> g_ipcEnabled{};
std::atomic<bool> g_qualityHooksInstalled{};
ammod::audio_v2::AudioCoreGate g_audioCoreGate;
ammod::audio_v2::AudioCoreRuntime g_audioCoreRuntime;
CoCreateInstanceFn g_v2OriginalCoCreateInstance{};
EnumAudioEndpointsFn g_v2OriginalEnumAudioEndpoints{};
GetDefaultEndpointFn g_v2OriginalGetDefaultEndpoint{};
GetDeviceFn g_v2OriginalGetDevice{};
CollectionItemFn g_v2OriginalCollectionItem{};
DeviceActivateFn g_v2OriginalDeviceActivate{};
AudioConverterNewFn g_v2OriginalAudioConverterNew{};
AudioConverterNewSpecificFn g_v2OriginalAudioConverterNewSpecific{};
AudioConverterSetPropertyFn g_v2OriginalAudioConverterSetProperty{};
AudioConverterGetPropertyFn g_v2OriginalAudioConverterGetProperty{};
AudioConverterFillComplexBufferFn g_v2OriginalAudioConverterFillComplexBuffer{};
AudioConverterResetFn g_v2OriginalAudioConverterReset{};
AudioConverterDisposeFn g_v2OriginalAudioConverterDispose{};
AudioUnitSetPropertyFn g_v2OriginalAudioUnitSetProperty{};
AudioUnitRenderFn g_v2OriginalAudioUnitRender{};
AudioOutputUnitStartFn g_v2OriginalAudioOutputUnitStart{};
AudioOutputUnitStopFn g_v2OriginalAudioOutputUnitStop{};
AudioUnitInitializeFn g_v2OriginalAudioUnitInitialize{};
AudioUnitUninitializeFn g_v2OriginalAudioUnitUninitialize{};
std::atomic<bool> g_audioCoreV2HooksInstalled{};
// Diagnostic-only native acquisition trace. It is disabled by default and is
// intentionally independent of the UI/audio-core gate. When enabled, the v2
// detour wraps Apple's input callback only to record its request/return shape;
// it never changes the callback result or any converter output.
std::atomic<bool> g_nativeTraceEnabled{};
std::atomic<std::uint64_t> g_nativeTraceFillCalls{};
std::atomic<std::uint64_t> g_nativeTracePropertyCalls{};
struct GraphFormatCandidate {
    std::uint32_t sampleRate{};
    std::uint32_t channels{};
    std::uint32_t sourceBitDepth{};
    ULONGLONG observedTick{};
    std::uint32_t encodedFormat{};
    std::uint64_t observationId{};
    DWORD threadId{};
    std::int64_t qpc{};
    void* converter{};
    void* caller{};
};
thread_local GraphFormatCandidate g_threadGraphFormat;
std::atomic<void*> g_v2PendingLocalConverter{};
struct LocalPcmQueue {
    std::mutex mutex;
    std::condition_variable spaceAvailable;
    std::vector<BYTE> storage;
    std::size_t readOffset{};
    std::size_t writeOffset{};
    std::size_t sizeBytes{};
    std::uint32_t sampleRate{};
    std::uint32_t channels{};
    std::uint32_t bitsPerChannel{};
    std::uint32_t bytesPerFrame{};
    std::uint32_t formatFlags{};
    std::size_t maximumBytes{};
    std::size_t hardMaximumBytes{512ULL * 1024ULL * 1024ULL};
    bool active{};
    bool abandoned{};
    std::uint64_t writeEpoch{};
    std::uint64_t capturedFrames{};
    std::uint64_t consumedFrames{};
    std::uint64_t droppedFrames{};
    std::uint64_t underrunFrames{};

    void Push(const void* data, std::size_t byteCount) {
        if (!data || !byteCount || !bytesPerFrame || byteCount % bytesPerFrame != 0) return;
        std::unique_lock lock(mutex);
        if (storage.empty() && maximumBytes) storage.resize(maximumBytes);
        if (abandoned) return;
        if (storage.empty() || byteCount > hardMaximumBytes) {
            droppedFrames += byteCount / bytesPerFrame;
            return;
        }
        const auto requiredBytes = sizeBytes + byteCount;
        if (requiredBytes > storage.size() && requiredBytes <= hardMaximumBytes) {
            const auto doubled = storage.size() <= hardMaximumBytes / 2
                ? storage.size() * 2 : hardMaximumBytes;
            const auto expandedSize = std::min(hardMaximumBytes,
                std::max(requiredBytes, doubled));
            try {
                std::vector<BYTE> expanded(expandedSize);
                if (sizeBytes) {
                    const auto firstPart = std::min(sizeBytes, storage.size() - readOffset);
                    std::memcpy(expanded.data(), storage.data() + readOffset, firstPart);
                    if (firstPart < sizeBytes) {
                        std::memcpy(expanded.data() + firstPart, storage.data(),
                                    sizeBytes - firstPart);
                    }
                }
                storage.swap(expanded);
                readOffset = 0;
                writeOffset = sizeBytes;
            } catch (const std::bad_alloc&) {
                droppedFrames += byteCount / bytesPerFrame;
                return;
            }
        }
        if (abandoned || sizeBytes + byteCount > storage.size()) {
            droppedFrames += byteCount / bytesPerFrame;
            return;
        }
        const auto* first = static_cast<const BYTE*>(data);
        const auto firstPart = std::min(byteCount, storage.size() - writeOffset);
        std::memcpy(storage.data() + writeOffset, first, firstPart);
        if (firstPart < byteCount) {
            std::memcpy(storage.data(), first + firstPart, byteCount - firstPart);
        }
        writeOffset = (writeOffset + byteCount) % storage.size();
        sizeBytes += byteCount;
        capturedFrames += byteCount / bytesPerFrame;
    }

    bool Pop(void* destination, std::uint32_t frames,
             std::uint32_t* copiedFrameCount = nullptr) {
        if (!destination || !frames || !bytesPerFrame) return false;
        const auto requestedBytes = static_cast<std::size_t>(frames) * bytesPerFrame;
        std::unique_lock lock(mutex);
        const auto copiedBytes = std::min(requestedBytes, sizeBytes);
        auto* output = static_cast<BYTE*>(destination);
        if (copiedBytes) {
            const auto firstPart = std::min(copiedBytes, storage.size() - readOffset);
            std::memcpy(output, storage.data() + readOffset, firstPart);
            if (firstPart < copiedBytes) {
                std::memcpy(output + firstPart, storage.data(), copiedBytes - firstPart);
            }
            readOffset = (readOffset + copiedBytes) % storage.size();
            sizeBytes -= copiedBytes;
        }
        const auto copiedFrames = static_cast<std::uint32_t>(copiedBytes / bytesPerFrame);
        if (copiedBytes < requestedBytes) {
            std::memset(output + copiedBytes, 0, requestedBytes - copiedBytes);
            underrunFrames += (requestedBytes - copiedBytes) / bytesPerFrame;
        }
        consumedFrames += copiedFrames;
        if (copiedFrameCount) *copiedFrameCount = copiedFrames;
        lock.unlock();
        spaceAvailable.notify_all();
        return copiedBytes == requestedBytes;
    }

    void FlushForSeek() {
        std::lock_guard lock(mutex);
        ++writeEpoch;
        readOffset = 0;
        writeOffset = 0;
        sizeBytes = 0;
        active = true;
        abandoned = false;
        spaceAvailable.notify_all();
    }
    void Abandon() {
        std::lock_guard lock(mutex);
        ++writeEpoch;
        active = false;
        abandoned = true;
        readOffset = 0;
        writeOffset = 0;
        sizeBytes = 0;
        spaceAvailable.notify_all();
    }
};
struct ConverterProbeState {
    std::shared_ptr<LocalPcmQueue> localQueue;
};
std::mutex g_converterProbeMutex;
std::unordered_map<void*, ConverterProbeState> g_converterProbes;
void Log(const std::wstring& message);
std::wstring AppleFormatText(const AudioStreamBasicDescription* format);
bool PromoteV2LocalQueue(void* converter);

// Compile-time native-acquisition A/B. The passthrough arm leaves Apple's
// AudioConverter/AudioQueue scheduler in control and mirrors its post-Fill P0;
// false restores the historical clone/Mod-owned Fill handoff below.
constexpr bool kNativeAcquisitionPassthrough = true;

// The native graph owns the decoder, but it does not own the only legal way
// to pull it. Once a native Fill call has exposed Apple's real input callback
// and user-data pair, this worker can repeat the exact
// AudioConverterFillComplexBuffer ABI on a Mod-owned thread. The native hook
// is cut over only after the exact converter is bound to the current
// generation and the replacement sink has actually started (AudioCore gate
// Active). A single mutex serializes native and Mod fills, Reset, and the
// private cursor property; the decoder is never entered concurrently.
std::mutex g_p0FillMutex;
// The runtime publishes Active from its control thread.  Do not take the
// NativeP0Puller mutex from that callback: on Apple's handoff path the
// callback can run while the decoder is tearing down its own worker state.
// Queue the transition and let the already-running Mod P0 worker apply it at
// its normal serialized boundary instead.
std::atomic<bool> g_nativeP0CutoverResumeRequested{};

struct NativeP0OutputList final {
    std::uint32_t mNumberBuffers{};
    std::uint32_t mReserved{};
    AppleAudioBuffer mBuffers[2]{};
};

class NativeP0Puller final {
public:
    bool Start() noexcept {
        std::lock_guard lock(stateMutex_);
        if (thread_) return true;
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake_) return false;
        stop_.store(false, std::memory_order_release);
        thread_ = CreateThread(nullptr, 0, &ThreadThunk, this, 0, nullptr);
        if (!thread_) {
            CloseHandle(wake_);
            wake_ = nullptr;
            return false;
        }
        return true;
    }

    void DisableForUiOff() noexcept {
        // Quiesce any in-flight Fill before clearing the private Apple
        // callback pointers.  This is also the hard UI authority boundary:
        // after this returns the worker cannot call into CoreAudioToolbox.
        g_nativeP0CutoverResumeRequested.store(false, std::memory_order_release);
        std::lock_guard fillLock(g_p0FillMutex);
        DisposeIndependentDecoder();
        std::lock_guard stateLock(stateMutex_);
        armed_ = false;
        paused_ = true;
        cutover_ = false;
        converter_ = nullptr;
        inputProc_ = nullptr;
        inputUserData_ = nullptr;
        SetEventNoLock();
    }

    void ObserveNativeFill(void* converter, AudioConverterComplexInputDataProcFn inputProc,
                           void* inputUserData, std::uint32_t requestedPackets) noexcept {
        if (!converter || !inputProc || !g_audioCoreRuntime.UiEnabled()) return;
        std::lock_guard lock(stateMutex_);
        if (converter_ != converter) {
            if (cloneConverter_ && g_v2OriginalAudioConverterDispose) {
                (void)g_v2OriginalAudioConverterDispose(cloneConverter_);
                cloneConverter_ = nullptr;
                cloneForConverter_ = nullptr;
            }
            converter_ = converter;
            inputProc_ = inputProc;
            inputUserData_ = inputUserData;
            requestedPackets_ = requestedPackets ? requestedPackets : kDefaultPullFrames;
            armed_ = true;
            paused_ = false;
            cutover_ = false;
        } else {
            // Apple normally keeps this pair stable across all fills.  Keep
            // the latest values so a graph successor can replace it without
            // relying on creation order.
            inputProc_ = inputProc;
            inputUserData_ = inputUserData;
            if (requestedPackets) requestedPackets_ = requestedPackets;
            armed_ = true;
            // A successful native Reset pauses the Mod worker until Apple's
            // cursor property arrives. Some decoder builds omit that private
            // property and proceed directly to the next Fill; that Fill is
            // already serialized here, so it is a safe recovery boundary.
            if (paused_ && !cutover_) paused_ = false;
        }
        SetEventNoLock();
    }

    void ObserveNativeCookie(void* converter, const void* data, std::uint32_t dataSize,
                             const AudioStreamBasicDescription& source,
                             const AudioStreamBasicDescription& destination) noexcept {
        constexpr std::uint32_t kAlac = 0x616C6163u; // 'alac'
        constexpr std::uint32_t kQlac = 0x716C6163u; // 'qlac'
        constexpr std::uint32_t kLpcm = 0x6C70636Du; // 'lpcm'
        if (!converter || !data || dataSize == 0 || dataSize > cookie_.size() ||
            (source.mFormatID != kAlac && source.mFormatID != kQlac) ||
            destination.mFormatID != kLpcm) {
            return;
        }
        std::lock_guard lock(stateMutex_);
        cookieConverter_ = converter;
        sourceFormat_ = source;
        destinationFormat_ = destination;
        cookieSize_ = dataSize;
        std::memcpy(cookie_.data(), data, dataSize);
        if (converter_ == converter) SetEventNoLock();
    }

    bool PrepareIndependentDecoder() noexcept {
        std::lock_guard lock(stateMutex_);
        if (cloneConverter_ && cloneForConverter_ == converter_) return true;
        if (!converter_ || !inputProc_ || cookieConverter_ != converter_ || cookieSize_ == 0 ||
            !g_v2OriginalAudioConverterNew || !g_v2OriginalAudioConverterSetProperty ||
            !g_v2OriginalAudioConverterDispose) {
            Log(L"audio core v2 independent P0 decoder unavailable: converter/callback/cookie incomplete");
            return false;
        }

        if (cloneConverter_) {
            const auto retired = cloneConverter_;
            cloneConverter_ = nullptr;
            cloneForConverter_ = nullptr;
            (void)g_v2OriginalAudioConverterDispose(retired);
        }

        void* clone{};
        const auto create = g_v2OriginalAudioConverterNew(
            &sourceFormat_, &destinationFormat_, &clone);
        if (create != 0 || !clone) {
            Log(L"audio core v2 independent P0 decoder create failed result=" +
                std::to_wstring(create));
            return false;
        }
        constexpr std::uint32_t decompressionMagicCookie = 0x646D6763u; // 'dmgc'
        const auto cookieResult = g_v2OriginalAudioConverterSetProperty(
            clone, decompressionMagicCookie, cookieSize_, cookie_.data());
        if (cookieResult != 0) {
            Log(L"audio core v2 independent P0 decoder cookie failed result=" +
                std::to_wstring(cookieResult));
            (void)g_v2OriginalAudioConverterDispose(clone);
            return false;
        }
        cloneConverter_ = clone;
        cloneForConverter_ = converter_;
        Log(L"audio core v2 independent P0 decoder ready original=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter_)) +
            L" clone=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(clone)) +
            L" source=" + AppleFormatText(&sourceFormat_) +
            L" destination=" + AppleFormatText(&destinationFormat_) +
            L" cookieBytes=" + std::to_wstring(cookieSize_));
        return true;
    }

    void DisposeIndependentDecoder() noexcept {
        std::lock_guard lock(stateMutex_);
        if (cloneConverter_ && g_v2OriginalAudioConverterDispose) {
            const auto clone = cloneConverter_;
            cloneConverter_ = nullptr;
            cloneForConverter_ = nullptr;
            const auto result = g_v2OriginalAudioConverterDispose(clone);
            Log(L"audio core v2 independent P0 decoder disposed clone=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(clone)) +
                L" result=" + std::to_wstring(result));
        } else {
            cloneConverter_ = nullptr;
            cloneForConverter_ = nullptr;
        }
    }

    bool ShouldSuppressNativeFill(void* converter) const noexcept {
        if (!g_audioCoreRuntime.UiEnabled()) return false;
        std::lock_guard lock(stateMutex_);
        // Being bound is not enough: suppressing Apple's line before the
        // replacement sink has committed Active strands both producers. The
        // runtime query also checks the same converter/generation and the
        // post-Start output gate.
        return armed_ && converter_ == converter &&
               g_audioCoreRuntime.IsNativeP0CutoverActive(converter);
    }

    bool Tracks(void* converter) const noexcept {
        std::lock_guard lock(stateMutex_);
        return converter && converter_ == converter && armed_;
    }

    void PauseForReset(void* converter) noexcept {
        std::lock_guard lock(stateMutex_);
        if (converter_ != converter || !armed_) return;
        paused_ = true;
        SetEventNoLock();
    }

    void ResumeAfterCursorProperty(void* converter) noexcept {
        std::lock_guard lock(stateMutex_);
        if (converter_ != converter || !armed_) return;
        paused_ = false;
        SetEventNoLock();
    }

    void ResetAfterNativeReset(void* converter) noexcept {
        std::lock_guard lock(stateMutex_);
        if (converter_ != converter || !armed_) return;
        // During an already-active generation, Apple's decoder may reset for
        // a timeline/cursor transition after the private cursor property has
        // been omitted.  The reset is serialized by g_p0FillMutex, so once it
        // returns the Mod worker can continue without waiting for a property
        // callback that this decoder generation never emits.  Before Active,
        // retain the conservative pause until the handoff is committed.
        const bool cutoverActive =
            g_audioCoreRuntime.IsNativeP0CutoverActive(converter);
        cutover_ = cutoverActive;
        paused_ = !cutoverActive;
        SetEventNoLock();
    }

    // A successful native Reset normally reaches the private cursor-property
    // update before Apple's next Fill.  Some Apple decoder generations omit
    // that property, however.  Once the runtime has completed the native
    // handoff and committed the replacement sink, the Mod worker is the only
    // legal P0 caller; leaving it paused here would make v2 drain only the
    // prebuffer and then go permanently silent.  The request is consumed by
    // Run(), never by the runtime Active callback, so the callback does not
    // enter this object's mutex during Apple's handoff.
    bool ApplyCutoverResumeRequest() noexcept {
        if (!g_nativeP0CutoverResumeRequested.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }
        std::lock_guard lock(stateMutex_);
        if (!armed_ || !converter_) {
            g_nativeP0CutoverResumeRequested.store(true, std::memory_order_release);
            return false;
        }
        paused_ = false;
        cutover_ = true;
        SetEventNoLock();
        return true;
    }

    bool BeginDispose(void* converter) noexcept {
        std::lock_guard lock(stateMutex_);
        if (converter_ != converter) return false;
        armed_ = false;
        paused_ = true;
        cutover_ = false;
        converter_ = nullptr;
        inputProc_ = nullptr;
        inputUserData_ = nullptr;
        return true;
    }

private:
    static constexpr std::uint32_t kDefaultPullFrames = 19200;
    static constexpr std::uint32_t kMaxPullFrames = 65536;

    struct Snapshot final {
        void* converter{};
        void* fillConverter{};
        AudioConverterComplexInputDataProcFn inputProc{};
        void* inputUserData{};
        std::uint32_t requestedPackets{};
        bool armed{};
        bool paused{};
        bool cutover{};
        bool independent{};
    };

    static DWORD WINAPI ThreadThunk(void* context) noexcept {
        static_cast<NativeP0Puller*>(context)->Run();
        return 0;
    }

    void SetEventNoLock() noexcept {
        if (wake_) SetEvent(wake_);
    }

    Snapshot SnapshotState() const noexcept {
        std::lock_guard lock(stateMutex_);
        const bool independent = cloneConverter_ && cloneForConverter_ == converter_;
        return Snapshot{converter_, independent ? cloneConverter_ : converter_, inputProc_,
                        inputUserData_, requestedPackets_, armed_, paused_, cutover_, independent};
    }

    static AppleOSStatus __cdecl IndependentInputThunk(
        void*, std::uint32_t* packets, void* data, void** descriptions,
        void* context) noexcept {
        auto* self = static_cast<NativeP0Puller*>(context);
        if (!self || !self->inputProc_ || !self->converter_) return -50;
        // The callback/user-data pair belongs to Apple's live converter. Keep
        // that handle as the callback's first argument even though the Fill
        // operation itself is performed by the independent decoder instance.
        return self->inputProc_(self->converter_, packets, data, descriptions,
                                self->inputUserData_);
    }

    void MarkCutover(void* converter) noexcept {
        std::lock_guard lock(stateMutex_);
        if (converter_ == converter && armed_) {
            cutover_ = true;
            SetEventNoLock();
        }
    }

    void Run() noexcept {
        std::vector<float> left(kMaxPullFrames);
        std::vector<float> right(kMaxPullFrames);
        std::vector<float> interleaved(static_cast<std::size_t>(kMaxPullFrames) * 2u);

        while (!stop_.load(std::memory_order_acquire)) {
            if (wake_) WaitForSingleObject(wake_, 20);
            if (stop_.load(std::memory_order_acquire)) break;
            if (!g_audioCoreRuntime.UiEnabled()) continue;

            if (ApplyCutoverResumeRequest()) {
                Log(L"audio core v2 Mod-owned P0 puller applied queued Active resume");
            }
            auto state = SnapshotState();
            if (!state.armed || state.paused || !state.converter || !state.inputProc ||
                !g_audioCoreRuntime.IsConverterBound(state.converter) ||
                !g_audioCoreRuntime.IsNativeP0CutoverActive(state.converter) ||
                !g_v2OriginalAudioConverterFillComplexBuffer) {
                continue;
            }

            const auto buffered = g_audioCoreRuntime.Coordinator().BufferedFrames();
            const auto format = g_audioCoreRuntime.Coordinator().Format();
            const auto target = std::max<std::size_t>(
                format.sampleRate / 2u, 4096u);
            if (buffered >= target) continue;

            // Re-check the UI and callback state while holding the same lock
            // used by the native hook.  This makes the Off boundary and the
            // no-concurrent-Fill invariant explicit.
            std::unique_lock fillLock(g_p0FillMutex);
            state = SnapshotState();
            if (!g_audioCoreRuntime.UiEnabled() || !state.armed || state.paused ||
                !state.converter || !state.inputProc ||
                !g_audioCoreRuntime.IsConverterBound(state.converter) ||
                !g_audioCoreRuntime.IsNativeP0CutoverActive(state.converter)) {
                continue;
            }
            ammod::audio_v2::ApplePcmFormatView appleFormat{};
            if (!g_audioCoreRuntime.DestinationFormat(state.converter, appleFormat)) continue;

            const bool planar = (appleFormat.formatFlags &
                                 ammod::audio_v2::kAppleFormatFlagIsNonInterleaved) != 0;
            const auto requested = std::clamp<std::uint32_t>(
                state.requestedPackets ? state.requestedPackets : kDefaultPullFrames,
                1u, kMaxPullFrames);
            NativeP0OutputList output{};
            output.mNumberBuffers = planar ? 2u : 1u;
            if (planar) {
                output.mBuffers[0] = {1u, requested * static_cast<std::uint32_t>(sizeof(float)),
                                      left.data()};
                output.mBuffers[1] = {1u, requested * static_cast<std::uint32_t>(sizeof(float)),
                                      right.data()};
            } else {
                output.mBuffers[0] = {2u,
                                      requested * 2u * static_cast<std::uint32_t>(sizeof(float)),
                                      interleaved.data()};
            }
            std::uint32_t produced = requested;
            const auto result = g_v2OriginalAudioConverterFillComplexBuffer(
                state.fillConverter,
                state.independent ? &IndependentInputThunk : state.inputProc,
                state.independent ? static_cast<void*>(this) : state.inputUserData, &produced,
                &output, nullptr);
            if (!g_audioCoreRuntime.UiEnabled()) continue;

            g_audioCoreRuntime.OnPcmFillResult(result, produced, true, true);
            bool accepted = false;
            if (ammod::audio_v2::ShouldForwardPcmOutput(true, result, produced, true, true)) {
                std::array<ammod::audio_v2::ApplePcmBufferView, 2> buffers{};
                const auto count = output.mNumberBuffers;
                for (std::uint32_t index = 0; index < count && index < buffers.size(); ++index) {
                    buffers[index] = {output.mBuffers[index].mNumberChannels,
                                      output.mBuffers[index].mDataByteSize,
                                      output.mBuffers[index].mData};
                }
                const ammod::audio_v2::ApplePcmBufferListView view{count, buffers.data()};
                accepted = g_audioCoreRuntime.OnPcmOutput(
                    state.converter, appleFormat, view, produced);
            }
            if (accepted) {
                const auto firstCutover = !state.cutover;
                MarkCutover(state.converter);
                if (firstCutover) {
                    Log(L"audio core v2 native P0 line replaced by Mod-owned Fill converter=" +
                        std::to_wstring(reinterpret_cast<std::uintptr_t>(state.converter)) +
                        L" requestedPackets=" + std::to_wstring(requested) +
                        L" producedPackets=" + std::to_wstring(produced) +
                        L" result=" + std::to_wstring(result));
                }
            } else if (result != 0 || produced == 0) {
                // A status-3 empty callback is normal while the converter
                // drains its internal decoded frames.  Avoid a tight loop,
                // but never translate it into silence or reset the decoder.
                Sleep(2);
            }
        }
    }

    mutable std::mutex stateMutex_;
    HANDLE wake_{};
    HANDLE thread_{};
    std::atomic<bool> stop_{};
    void* converter_{};
    AudioConverterComplexInputDataProcFn inputProc_{};
    void* inputUserData_{};
    std::uint32_t requestedPackets_{kDefaultPullFrames};
    bool armed_{};
    bool paused_{};
    bool cutover_{};
    void* cloneConverter_{};
    void* cloneForConverter_{};
    void* cookieConverter_{};
    AudioStreamBasicDescription sourceFormat_{};
    AudioStreamBasicDescription destinationFormat_{};
    std::array<std::uint8_t, 256> cookie_{};
    std::uint32_t cookieSize_{};
};

NativeP0Puller g_nativeP0Puller;

struct QlacBitDepthEvidence {
    std::uint32_t sampleRate{};
    std::uint32_t channels{};
    std::uint32_t sourceBitDepth{};
    ULONGLONG observedTick{};
    std::uint64_t observationId{};
};
std::mutex g_qlacBitDepthEvidenceMutex;
std::deque<QlacBitDepthEvidence> g_qlacBitDepthEvidence;
std::atomic<std::uint64_t> g_formatObservationSequence{};
std::mutex g_audioUnitEpochMutex;
struct AudioUnitInputFormatState {
    AudioStreamBasicDescription format{};
    std::uint32_t sourceBitDepth{};
    std::uint64_t observationId{};
};
std::unordered_map<void*, AudioUnitInputFormatState> g_audioUnitInputFormats;
std::atomic<std::uint64_t> g_v2ObservedRenderCalls{};
std::atomic<std::uint64_t> g_v2P1SignatureValidCalls{};
std::atomic<std::uint64_t> g_v2P1SignatureInvalidCalls{};
std::atomic<std::uint64_t> g_v2P1SignatureNonZeroCalls{};
std::atomic<std::uint64_t> g_v2P1SignatureAllZeroCalls{};
std::atomic<std::uint64_t> g_v2P1RepeatedSignatureCalls{};
std::atomic<std::uint64_t> g_v2P1TimestampMissingCalls{};
std::atomic<std::uint64_t> g_v2P1TimestampRepeatedCalls{};
std::atomic<std::uint64_t> g_v2P1TimestampDiscontinuities{};
std::atomic<std::uint64_t> g_v2P1RenderErrors{};
std::atomic<std::uint64_t> g_v2P1LastSignature{};
std::atomic<std::uint64_t> g_v2P1LastSignatureBytes{};
std::atomic<std::uint64_t> g_v2P1LastDeclaredBytes{};
std::atomic<std::uint64_t> g_v2P1LastSampleTimeBits{};
std::atomic<std::uint64_t> g_v2P1LastHostTime{};
std::atomic<std::uint64_t> g_v2P1LastTimestampFlags{};
std::atomic<std::uint32_t> g_v2P1LastFrames{};
std::atomic<std::uint32_t> g_v2P1LastBufferCount{};
std::atomic<std::uint32_t> g_v2P1LastZeroBytes{};
std::atomic<std::int64_t> g_v2P1LastQpc{};
struct V2P1SignatureSlot {
    std::atomic<std::uint64_t> key{};
    std::atomic<std::uint64_t> sequence{};
    std::atomic<std::uint64_t> lastSignature{};
    std::atomic<std::uint64_t> lastSampleTimeBits{};
    std::atomic<std::uint64_t> lastHostTime{};
    std::atomic<std::uint32_t> lastFrames{};
};
std::array<V2P1SignatureSlot, 32> g_v2P1SignatureSlots{};
std::atomic<std::uint64_t> g_v2NativeGetBufferCalls{};
std::atomic<std::uint64_t> g_v2NativeReleaseBufferCalls{};

std::wstring AddressLocation(void* address);

std::wstring CaptureCurrentStackText(DWORD framesToSkip = 1, DWORD maximumFrames = 8) {
    std::array<void*, 16> frames{};
    const auto frameCount = CaptureStackBackTrace(
        framesToSkip + 1, std::min<DWORD>(maximumFrames, static_cast<DWORD>(frames.size())),
        frames.data(), nullptr);
    std::wstring text;
    for (USHORT index = 0; index < frameCount; ++index) {
        if (!text.empty()) text += L" <- ";
        text += AddressLocation(frames[index]);
    }
    return text;
}


std::wstring GuidText(REFGUID guid) {
    wchar_t text[64]{};
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    return text;
}

std::wstring HResultText(HRESULT hr) {
    std::wostringstream out;
    out << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0')
        << static_cast<unsigned long>(hr);
    return out.str();
}

std::wstring AddressLocation(void* address) {
    if (!address) return L"null";
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) || !module) {
        return std::to_wstring(reinterpret_cast<std::uintptr_t>(address));
    }
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(module, modulePath, static_cast<DWORD>(std::size(modulePath)));
    const auto rva = reinterpret_cast<std::uintptr_t>(address) -
        reinterpret_cast<std::uintptr_t>(module);
    std::wostringstream out;
    out << std::filesystem::path(modulePath).filename().wstring() << L"+0x"
        << std::hex << std::uppercase << rva;
    return out.str();
}

void Log(const std::wstring& message);

void Log(const std::wstring& message) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wostringstream line;
    line << std::setfill(L'0') << std::setw(4) << now.wYear << L'-' << std::setw(2) << now.wMonth
         << L'-' << std::setw(2) << now.wDay << L'T' << std::setw(2) << now.wHour << L':'
         << std::setw(2) << now.wMinute << L':' << std::setw(2) << now.wSecond << L'.'
         << std::setw(3) << now.wMilliseconds << L" pid=" << GetCurrentProcessId()
         << L" tid=" << GetCurrentThreadId() << L" " << message;
    ammod::logging::AppendWideLine(g_logPath, g_logMutex, line.str(), 8ull * 1024ull * 1024ull, 2);
}

std::wstring ReadSetting(const wchar_t* key, const wchar_t* fallback) {
    wchar_t value[1024]{};
    GetPrivateProfileStringW(L"mod", key, fallback, value, static_cast<DWORD>(std::size(value)), g_iniPath.c_str());
    return value;
}

bool ReadBool(const wchar_t* key, bool fallback) {
    std::wstring value = ReadSetting(key, fallback ? L"true" : L"false");
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value == L"true" || value == L"1" || value == L"yes";
}

std::wstring FormatText(const WAVEFORMATEX* format) {
    if (!format) return L"format=null";
    std::wostringstream out;
    out << L"tag=" << format->wFormatTag << L" rate=" << format->nSamplesPerSec
        << L" channels=" << format->nChannels << L" bits=" << format->wBitsPerSample
        << L" blockAlign=" << format->nBlockAlign << L" avgBps=" << format->nAvgBytesPerSec
        << L" cbSize=" << format->cbSize;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        out << L" validBits=" << ext->Samples.wValidBitsPerSample << L" channelMask=0x"
            << std::hex << ext->dwChannelMask << L" subFormat=" << GuidText(ext->SubFormat);
    }
    return out.str();
}

std::wstring AppleFormatIdText(std::uint32_t formatId) {
    wchar_t printable[5]{};
    bool allPrintable = true;
    for (unsigned index = 0; index < 4; ++index) {
        const auto byte = static_cast<unsigned char>((formatId >> (24 - index * 8)) & 0xff);
        printable[index] = static_cast<wchar_t>(byte);
        allPrintable = allPrintable && byte >= 0x20 && byte <= 0x7e;
    }
    std::wostringstream out;
    out << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0') << formatId;
    if (allPrintable) out << L" ('" << printable << L"')";
    return out.str();
}

std::wstring AppleFormatText(const AudioStreamBasicDescription* format) {
    if (!format) return L"asbd=null";
    std::wostringstream out;
    out << std::setprecision(12) << L"rate=" << format->mSampleRate
        << L" formatId=" << AppleFormatIdText(format->mFormatID)
        << L" flags=0x" << std::hex << format->mFormatFlags << std::dec
        << L" bytesPerPacket=" << format->mBytesPerPacket
        << L" framesPerPacket=" << format->mFramesPerPacket
        << L" bytesPerFrame=" << format->mBytesPerFrame
        << L" channels=" << format->mChannelsPerFrame
        << L" bits=" << format->mBitsPerChannel;
    return out.str();
}

bool IsAppleLinearPcm(const AudioStreamBasicDescription* format) {
    return format && format->mFormatID == ammod::audio::kLinearPcm;
}

ammod::audio::ApplePcmFormat PcmFormatView(const AudioStreamBasicDescription& format) {
    return {format.mSampleRate, format.mFormatID, format.mFormatFlags,
            format.mBytesPerPacket, format.mFramesPerPacket, format.mBytesPerFrame,
            format.mChannelsPerFrame, format.mBitsPerChannel};
}

std::int64_t CurrentQpc() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

struct NativeTraceBufferShape final {
    std::uint32_t bufferCount{};
    std::uintptr_t firstData{};
    std::uint32_t firstBytes{};
    std::uint32_t firstChannels{};
};

struct NativeTracePacketShape final {
    std::uintptr_t descriptions{};
    std::int64_t firstOffset{};
    std::uint32_t firstFrames{};
    std::uint32_t firstBytes{};
    bool firstValid{};
};

NativeTraceBufferShape InspectNativeTraceBuffers(const void* rawList) noexcept {
    NativeTraceBufferShape shape{};
    if (!rawList) return shape;
    __try {
        const auto* list = static_cast<const AppleAudioBufferList*>(rawList);
        shape.bufferCount = list->mNumberBuffers;
        if (shape.bufferCount != 0 && shape.bufferCount <= 8) {
            shape.firstData = reinterpret_cast<std::uintptr_t>(list->mBuffers[0].mData);
            shape.firstBytes = list->mBuffers[0].mDataByteSize;
            shape.firstChannels = list->mBuffers[0].mNumberChannels;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        shape = {};
    }
    return shape;
}

NativeTracePacketShape InspectNativeTraceInputPackets(void** descriptions,
                                                        std::uint32_t packetCount) noexcept {
    NativeTracePacketShape shape{};
    if (!descriptions) return shape;
    __try {
        const auto* packets = static_cast<const AppleAudioStreamPacketDescription*>(*descriptions);
        shape.descriptions = reinterpret_cast<std::uintptr_t>(packets);
        if (packets && packetCount != 0) {
            shape.firstOffset = packets[0].mStartOffset;
            shape.firstFrames = packets[0].mVariableFramesInPacket;
            shape.firstBytes = packets[0].mDataByteSize;
            shape.firstValid = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        shape = {};
    }
    return shape;
}

bool CopyNativeTraceScalar(const void* data, std::uint32_t dataSize,
                           std::uint64_t& value) noexcept {
    __try {
        std::memcpy(&value, data, dataSize);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::wstring NativeTraceScalarText(const void* data, std::uint32_t dataSize) {
    if (!data || (dataSize != sizeof(std::uint32_t) && dataSize != sizeof(std::uint64_t))) {
        return {};
    }
    std::uint64_t value{};
    if (!CopyNativeTraceScalar(data, dataSize, value)) return {};
    std::wostringstream text;
    text << L" scalar=0x" << std::hex << value << std::dec;
    return text.str();
}

struct V2P1SignatureResult final {
    std::uint64_t signature{};
    std::uint64_t declaredBytes{};
    std::uint32_t hashedBytes{};
    std::uint32_t zeroBytes{};
    bool valid{};
    bool allZero{};
};

constexpr std::uint64_t kFnv1aOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnv1aPrime = 1099511628211ULL;

void AppendP1Hash(std::uint64_t& hash, const void* data, std::size_t bytes) noexcept {
    const auto* input = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < bytes; ++index) {
        hash ^= input[index];
        hash *= kFnv1aPrime;
    }
}

template <typename T>
void AppendP1Scalar(std::uint64_t& hash, const T& value) noexcept {
    AppendP1Hash(hash, &value, sizeof(value));
}

std::uint64_t DoubleBits(double value) noexcept {
    std::uint64_t bits{};
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double BitsDouble(std::uint64_t bits) noexcept {
    double value{};
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

V2P1SignatureResult ComputeV2P1Signature(const AppleAudioBufferList* list) noexcept {
    V2P1SignatureResult result{};
    result.signature = kFnv1aOffset;
    if (!list || list->mNumberBuffers == 0 || list->mNumberBuffers > 8) return result;

    result.valid = true;
    AppendP1Scalar(result.signature, list->mNumberBuffers);
    constexpr std::size_t kWindowBytes = 64;
    for (std::uint32_t index = 0; index < list->mNumberBuffers; ++index) {
        const auto& buffer = list->mBuffers[index];
        AppendP1Scalar(result.signature, index);
        AppendP1Scalar(result.signature, buffer.mNumberChannels);
        AppendP1Scalar(result.signature, buffer.mDataByteSize);
        result.declaredBytes += buffer.mDataByteSize;
        if (buffer.mDataByteSize == 0) continue;
        if (!buffer.mData) {
            result.valid = false;
            continue;
        }

        const auto* bytes = static_cast<const unsigned char*>(buffer.mData);
        const auto firstBytes = std::min<std::size_t>(kWindowBytes, buffer.mDataByteSize);
        const auto tailBytes = buffer.mDataByteSize > firstBytes
            ? std::min<std::size_t>(kWindowBytes, buffer.mDataByteSize - firstBytes) : 0;
        AppendP1Hash(result.signature, bytes, firstBytes);
        result.hashedBytes += static_cast<std::uint32_t>(firstBytes);
        for (std::size_t offset = 0; offset < firstBytes; ++offset) {
            if (bytes[offset] == 0) ++result.zeroBytes;
        }
        if (tailBytes != 0) {
            const auto* tail = bytes + buffer.mDataByteSize - tailBytes;
            AppendP1Hash(result.signature, tail, tailBytes);
            result.hashedBytes += static_cast<std::uint32_t>(tailBytes);
            for (std::size_t offset = 0; offset < tailBytes; ++offset) {
                if (tail[offset] == 0) ++result.zeroBytes;
            }
        }
    }
    result.allZero = result.valid && result.hashedBytes != 0 &&
        result.zeroBytes == result.hashedBytes;
    return result;
}

V2P1SignatureSlot* FindV2P1SignatureSlot(void* unit, std::uint32_t bus) noexcept {
    if (!unit) return nullptr;
    auto key = (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(unit)) >> 4) ^
        (static_cast<std::uint64_t>(bus) + 0x9E3779B97F4A7C15ULL);
    if (key == 0) key = 1;
    const auto start = static_cast<std::size_t>(key % g_v2P1SignatureSlots.size());
    for (std::size_t probe = 0; probe < g_v2P1SignatureSlots.size(); ++probe) {
        auto& slot = g_v2P1SignatureSlots[(start + probe) % g_v2P1SignatureSlots.size()];
        auto observed = slot.key.load(std::memory_order_acquire);
        if (observed == key) return &slot;
        if (observed == 0) {
            if (slot.key.compare_exchange_strong(observed, key,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                slot.sequence.store(0, std::memory_order_release);
                slot.lastSignature.store(0, std::memory_order_release);
                slot.lastSampleTimeBits.store(0, std::memory_order_release);
                slot.lastHostTime.store(0, std::memory_order_release);
                slot.lastFrames.store(0, std::memory_order_release);
                return &slot;
            }
        }
    }
    return nullptr;
}

void RetireAudioUnitEpoch(void* unit) {
    std::lock_guard lock(g_audioUnitEpochMutex);
    g_audioUnitInputFormats.erase(unit);
}

void RememberAudioUnitInputFormat(void* unit, const AudioStreamBasicDescription& format,
                                  std::uint32_t sourceBitDepth,
                                  std::uint64_t observationId) {
    std::lock_guard lock(g_audioUnitEpochMutex);
    g_audioUnitInputFormats[unit] = {format, sourceBitDepth, observationId};
}

bool FindAudioUnitInputFormat(void* unit, AudioUnitInputFormatState& state) {
    std::lock_guard lock(g_audioUnitEpochMutex);
    const auto existing = g_audioUnitInputFormats.find(unit);
    if (existing == g_audioUnitInputFormats.end()) return false;
    state = existing->second;
    return true;
}

void ObserveDecodedSourceFormat(const AudioStreamBasicDescription* source,
                                const AudioStreamBasicDescription* destination,
                                void* converter, void* caller) {
    if (!source || !destination || !IsAppleLinearPcm(destination)) return;
    if (IsAppleLinearPcm(source)) {
        ammod::audio::LocalPcmCandidate local{};
        if (!ammod::audio::TryClassifyLocalPcmCandidate(
                PcmFormatView(*source), PcmFormatView(*destination), local)) return;
        const auto now = GetTickCount64();
        const auto existingAge = g_threadGraphFormat.observedTick != 0
            ? now - g_threadGraphFormat.observedTick : UINT64_MAX;
        if (ammod::audio::PreferExistingCompressedCandidate(
                g_threadGraphFormat.encodedFormat, g_threadGraphFormat.sampleRate,
                g_threadGraphFormat.channels, existingAge, local.sampleRate, local.channels)) {
            Log(L"Local LPCM candidate suppressed by fresh compressed predecessor rate=" +
                std::to_wstring(local.sampleRate) + L" predecessor=" +
                AppleFormatIdText(g_threadGraphFormat.encodedFormat) + L" observation=" +
                std::to_wstring(g_threadGraphFormat.observationId));
            return;
        }
        const auto observationId = g_formatObservationSequence.fetch_add(1) + 1;
        g_threadGraphFormat = {local.sampleRate, local.channels, local.sourceBitDepth,
                               now, source->mFormatID, observationId,
                               GetCurrentThreadId(), CurrentQpc(), converter, caller};
        Log(L"Local LPCM source format observed rate=" +
            std::to_wstring(local.sampleRate) + L" channels=" +
            std::to_wstring(local.channels) + L" sourceBitDepth=" +
            std::to_wstring(local.sourceBitDepth) + L" numeric=" +
            (local.sourceIsFloat ? std::wstring(L"float") : std::wstring(L"integer")) +
            L" observation=" + std::to_wstring(observationId) + L" tid=" +
            std::to_wstring(GetCurrentThreadId()) + L" qpc=" +
            std::to_wstring(g_threadGraphFormat.qpc) + L" converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) + L" caller=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(caller)));
        return;
    }
    if (destination->mSampleRate < 8000.0 || destination->mSampleRate > 768000.0 ||
        destination->mChannelsPerFrame == 0 || destination->mChannelsPerFrame > 8) return;
    const auto rate = static_cast<std::uint32_t>(destination->mSampleRate + 0.5);
    const auto observationId = g_formatObservationSequence.fetch_add(1) + 1;
    g_threadGraphFormat = {rate, destination->mChannelsPerFrame, 0, GetTickCount64(),
                           source->mFormatID, observationId, GetCurrentThreadId(),
                           CurrentQpc(), converter, caller};
    Log(L"Decoded source format observed rate=" + std::to_wstring(rate) +
        L" channels=" + std::to_wstring(destination->mChannelsPerFrame) +
        L" encodedFormat=" + AppleFormatIdText(source->mFormatID) +
        L" observation=" + std::to_wstring(observationId) +
        L" tid=" + std::to_wstring(GetCurrentThreadId()) +
        L" qpc=" + std::to_wstring(g_threadGraphFormat.qpc) +
        L" converter=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
        L" caller=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(caller)));
}

std::wstring DeviceId(IMMDevice* device) {
    LPWSTR id{};
    if (!device || FAILED(device->GetId(&id)) || !id) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

template<typename Fn>
bool HookAddress(void* target, void* detour, Fn& original, const wchar_t* label) {
    std::lock_guard lock(g_hookMutex);
    if (original) return true;
    void* trampoline{};
    const MH_STATUS create = MH_CreateHook(target, detour, &trampoline);
    if (create != MH_OK) {
        Log(std::wstring(L"hook create failed ") + label + L" status=" + std::to_wstring(create));
        return false;
    }
    original = reinterpret_cast<Fn>(trampoline);
    const MH_STATUS enable = MH_EnableHook(target);
    if (enable != MH_OK && enable != MH_ERROR_ENABLED) {
        Log(std::wstring(L"hook enable failed ") + label + L" status=" + std::to_wstring(enable));
        return false;
    }
    Log(std::wstring(L"hooked ") + label);
    return true;
}

template<typename T>
void** Vtable(T* object) {
    return object ? *reinterpret_cast<void***>(object) : nullptr;
}

bool ExclusiveRequested();

void RefreshConfiguredAudioQuality() {
    if (!g_cfPreferencesGetAppIntegerValue || !g_cfPreferencesGetAppBooleanValue ||
        !g_cfPreferencesCurrentApplication || !g_streamQualityPreferenceKey ||
        !g_losslessEnabledPreferenceKey) {
        g_configuredQualityKnown.store(false, std::memory_order_release);
        return;
    }
    unsigned char qualityExists{};
    unsigned char losslessExists{};
    const auto quality = g_cfPreferencesGetAppIntegerValue(
        g_streamQualityPreferenceKey, g_cfPreferencesCurrentApplication, &qualityExists);
    const unsigned char lossless = g_cfPreferencesGetAppBooleanValue(
        g_losslessEnabledPreferenceKey, g_cfPreferencesCurrentApplication, &losslessExists);
    const bool known = qualityExists && losslessExists && quality >= 0 && quality <= 0xFFFF;
    if (known) {
        g_configuredStreamQuality.store(static_cast<std::uint16_t>(quality),
                                        std::memory_order_release);
        g_configuredLosslessEnabled.store(lossless != 0, std::memory_order_release);
    }
    g_configuredQualityKnown.store(known, std::memory_order_release);
    Log(L"Apple preferences audio quality=" + std::to_wstring(quality) +
        L" qualityExists=" + std::to_wstring(qualityExists) +
        L" losslessEnabled=" + std::to_wstring(static_cast<unsigned int>(lossless)) +
        L" losslessExists=" + std::to_wstring(losslessExists));
}

bool StrictLosslessRequested() {
    const auto quality = g_configuredStreamQuality.load(std::memory_order_acquire);
    return g_configuredQualityKnown.load(std::memory_order_acquire) &&
        g_configuredLosslessEnabled.load(std::memory_order_acquire) &&
        (quality == 15 || quality == 20);
}

void __cdecl HookAVCFSetPreferredMaximumSampleRate(void* playerItem, double sampleRate) {
    RefreshConfiguredAudioQuality();
    const auto ceiling = ammod::quality::ProfileCeiling(true,
        g_configuredQualityKnown.load(), g_configuredLosslessEnabled.load(),
        g_configuredStreamQuality.load());
    // Only highest-tier locking is changed here; ordinary lossless/AAC retain
    // Apple's existing selection policy. This ceiling alone is not the lock.
    const double effectiveRate = ceiling == 192000 ? 192000.0 : sampleRate;
    Log(L"AVCFPlayerItemSetPreferredMaximumAudioSampleRate playerItem=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(playerItem)) +
        L" requestedRate=" + std::to_wstring(sampleRate) +
        L" effectiveRate=" + std::to_wstring(effectiveRate));
    g_originalAVCFSetPreferredMaximumSampleRate(playerItem, effectiveRate);
}

void __cdecl HookAVCFSetVariantPreferences(void* playerItem, std::uint32_t preferences) {
    RefreshConfiguredAudioQuality();
    // Apple Music 1.6.4.90 can persist streaming quality 20/losslessEnabled=true yet
    // construct a player item without AVVariantPreferenceScalabilityToLosslessAudio
    // (0x8). When the quality lock is active this Mod is fail-closed for source quality: keep all
    // of Apple's flags and restore that single missing preference before selection.
    // Bits 0/1 are the immersive/spatial preference pair manipulated by the
    // Agent's second policy pass. Leaving them set can immediately replace the
    // lossless preference with the AAC compatibility branch. Strict lossless
    // stereo therefore keeps bit 2, forces lossless bit 3, and clears only that
    // competing pair.
    const std::uint32_t effective = StrictLosslessRequested()
        ? ((preferences | 0x8u) & ~0x3u) : preferences;
    g_lastVariantPreferences.store(effective, std::memory_order_release);
    std::wostringstream requestedValue;
    requestedValue << std::hex << preferences;
    std::wostringstream effectiveValue;
    effectiveValue << std::hex << effective;
    Log(L"AVCFPlayerItemSetVariantPreferences playerItem=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(playerItem)) +
        L" requested=0x" + requestedValue.str() + L" effective=0x" +
        effectiveValue.str());
    g_originalAVCFSetVariantPreferences(playerItem, effective);
}

AppleOSStatus __cdecl HookFigAlternateEligibleLosslessFilterCreate(
    void* allocator, void* options, void* outFilter) {
    RefreshConfiguredAudioQuality();
    AppleOSStatus result{-1};
    bool strictSubtypeFilter{};
    const bool strictRequested = StrictLosslessRequested() &&
        (g_lastVariantPreferences.load(std::memory_order_acquire) & 0x8u);
    if (strictRequested && outFilter) {
        *static_cast<void**>(outFilter) = nullptr;
    }
    if (strictRequested && outFilter && g_figAlternateAllowableMediaSubtypeFilterCreate &&
        g_cfNumberCreate && g_cfArrayCreate && g_cfRelease && g_cfTypeArrayCallbacks) {
        constexpr std::int32_t cfNumberSInt32Type = 3;
        const std::int32_t alac = 0x616C6163; // 'alac' in HLS/CMAudioFormatDescription
        const std::int32_t qlac = 0x716C6163; // protected ALAC ASBD used by Apple Music
        void* alacNumber = g_cfNumberCreate(nullptr, cfNumberSInt32Type, &alac);
        void* qlacNumber = g_cfNumberCreate(nullptr, cfNumberSInt32Type, &qlac);
        const void* values[]{alacNumber, qlacNumber};
        void* allowed = alacNumber && qlacNumber
            ? g_cfArrayCreate(nullptr, values, 2, g_cfTypeArrayCallbacks) : nullptr;
        if (allowed) {
            if (g_configuredStreamQuality.load(std::memory_order_acquire) == 20) {
                result = ammod::quality::CreateHighestLosslessFilter(
                    g_figAlternateAllowableMediaSubtypeFilterCreate, allocator, allowed,
                    static_cast<void**>(outFilter));
            } else {
                result = g_figAlternateAllowableMediaSubtypeFilterCreate(
                    allocator, allowed, nullptr, static_cast<void**>(outFilter));
            }
            strictSubtypeFilter = result == 0 && *static_cast<void**>(outFilter);
            g_cfRelease(allowed);
        } else {
            result = -1;
        }
        if (alacNumber) g_cfRelease(alacNumber);
        if (qlacNumber) g_cfRelease(qlacNumber);
    }
    // Never fall back to Apple's adaptive eligible-lossless filter for a strict
    // request: that path may emit an AAC prefix. Failure is intentionally silent.
    if (!strictRequested) {
        result = g_originalFigAlternateEligibleLosslessFilterCreate(
            allocator, options, outFilter);
    }
    void* filter = outFilter ? *static_cast<void**>(outFilter) : nullptr;
    Log(L"FigAlternateEligibleLosslessAudioFilterCreate options=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(options)) +
        L" strictRequested=" + std::to_wstring(strictRequested) +
        L" strictSubtypeFilter=" + std::to_wstring(strictSubtypeFilter) + L" result=" +
        std::to_wstring(result) + L" filter=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(filter)));
    return result;
}

std::uint32_t ReadBigEndianU32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

bool ParseAlacSpecificConfig(const void* data, std::uint32_t dataSize,
                             std::uint32_t& bitDepth, std::uint32_t& channels,
                             std::uint32_t& sampleRate, std::uint32_t& configOffset) {
    namespace offsets = ammod::apple_private::alac_cookie;
    if (!data || dataSize < offsets::kConfigBytes) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (const auto offset : offsets::kCandidateOffsets) {
        if (offset + offsets::kConfigBytes > dataSize) continue;
        const auto candidateBits = static_cast<std::uint32_t>(bytes[offset + offsets::kBitDepth]);
        const auto candidateChannels = static_cast<std::uint32_t>(bytes[offset + offsets::kChannels]);
        const auto candidateRate = ReadBigEndianU32(bytes + offset + offsets::kSampleRate);
        if ((candidateBits == 16 || candidateBits == 24 ||
             candidateBits == 32) && candidateChannels > 0 && candidateChannels <= 8 &&
            candidateRate >= 8000 && candidateRate <= 768000) {
            bitDepth = candidateBits;
            channels = candidateChannels;
            sampleRate = candidateRate;
            configOffset = offset;
            return true;
        }
    }
    return false;
}

bool IsSupportedSourceBitDepth(std::uint32_t bitDepth) {
    return ammod::audio::IsSupportedSourceBitDepth(bitDepth);
}

void RememberQlacBitDepthEvidence(std::uint32_t sampleRate, std::uint32_t channels,
                                  std::uint32_t sourceBitDepth,
                                  std::uint64_t observationId) {
    std::lock_guard lock(g_qlacBitDepthEvidenceMutex);
    g_qlacBitDepthEvidence.push_back(
        {sampleRate, channels, sourceBitDepth, GetTickCount64(), observationId});
    while (g_qlacBitDepthEvidence.size() > 64) g_qlacBitDepthEvidence.pop_front();
}

bool ResolveUnambiguousQlacBitDepth(std::uint32_t sampleRate, std::uint32_t channels,
                                    std::uint32_t& sourceBitDepth,
                                    std::uint64_t& observationId) {
    constexpr ULONGLONG evidenceLifetimeMs = 10 * 60 * 1000;
    const auto now = GetTickCount64();
    std::lock_guard lock(g_qlacBitDepthEvidenceMutex);
    while (!g_qlacBitDepthEvidence.empty() &&
           now - g_qlacBitDepthEvidence.front().observedTick > evidenceLifetimeMs) {
        g_qlacBitDepthEvidence.pop_front();
    }
    std::uint32_t resolvedDepth{};
    std::uint64_t newestObservation{};
    for (const auto& evidence : g_qlacBitDepthEvidence) {
        if (evidence.sampleRate != sampleRate || evidence.channels != channels) continue;
        if (resolvedDepth != 0 && resolvedDepth != evidence.sourceBitDepth) return false;
        resolvedDepth = evidence.sourceBitDepth;
        newestObservation = evidence.observationId;
    }
    if (!IsSupportedSourceBitDepth(resolvedDepth)) return false;
    sourceBitDepth = resolvedDepth;
    observationId = newestObservation;
    return true;
}

void ObserveAudioConverterProperty(const wchar_t* operation, void* converter,
                                   std::uint32_t propertyId, const void* data,
                                   std::uint32_t dataSize, AppleOSStatus result) {
    constexpr std::uint32_t decompressionMagicCookie = 0x646D6763; // 'dmgc'
    if (propertyId != decompressionMagicCookie) return;
    std::wstring message = std::wstring(L"AudioConverter") + operation +
        L" property=" + AppleFormatIdText(propertyId) +
        L" converter=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
        L" size=" + std::to_wstring(dataSize) + L" result=" + std::to_wstring(result) +
        L" tid=" + std::to_wstring(GetCurrentThreadId()) +
        L" qpc=" + std::to_wstring(CurrentQpc());
    if (result == 0 && propertyId == decompressionMagicCookie && data && dataSize) {
        std::uint32_t bitDepth{}, channels{}, sampleRate{}, offset{};
        if (ParseAlacSpecificConfig(data, dataSize, bitDepth, channels, sampleRate, offset)) {
            message += L" alacBitDepth=" + std::to_wstring(bitDepth) +
                L" alacChannels=" + std::to_wstring(channels) +
                L" alacSampleRate=" + std::to_wstring(sampleRate) +
                L" configOffset=" + std::to_wstring(offset);
            constexpr std::uint32_t qlac = 0x716C6163; // 'qlac'
            const auto now = GetTickCount64();
            const bool candidateMatches = g_threadGraphFormat.encodedFormat == qlac &&
                g_threadGraphFormat.observedTick != 0 &&
                now - g_threadGraphFormat.observedTick <= 2000 &&
                g_threadGraphFormat.sampleRate == sampleRate &&
                g_threadGraphFormat.channels == channels &&
                IsSupportedSourceBitDepth(bitDepth);
            if (candidateMatches) {
                g_threadGraphFormat.sourceBitDepth = bitDepth;
                RememberQlacBitDepthEvidence(sampleRate, channels, bitDepth,
                                             g_threadGraphFormat.observationId);
                message += L" boundObservation=" +
                    std::to_wstring(g_threadGraphFormat.observationId) +
                    L" decoderConverter=" + std::to_wstring(
                        reinterpret_cast<std::uintptr_t>(g_threadGraphFormat.converter));
            } else {
                message += L" binding=rejected";
            }
        } else {
            message += L" alacConfig=unrecognized";
            std::wostringstream hex;
            hex << L" cookieHex=" << std::hex << std::setfill(L'0');
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            const auto captured = std::min<std::uint32_t>(dataSize, 64);
            for (std::uint32_t index = 0; index < captured; ++index) {
                hex << std::setw(2) << static_cast<unsigned>(bytes[index]);
            }
            message += hex.str();
        }
    }
    Log(message);
}

bool InstallQualityLockHooks() {
    // Quality locking is installation-scoped. Do not gate this path on the UI intent;
    // that intent is reserved for the removed audio-core hook set.
    if (g_qualityHooksInstalled.load(std::memory_order_acquire)) return true;
    HMODULE avFoundation = GetModuleHandleW(L"AVFoundationCF.dll");
    HMODULE coreFoundation = GetModuleHandleW(L"CoreFoundation.dll");
    HMODULE coreMedia = GetModuleHandleW(L"CoreMedia.dll");
    if (!avFoundation || !coreFoundation || !coreMedia) return false;

    auto installAVCF = [avFoundation](const char* exportName, void* detour, auto& original,
                                      const wchar_t* label) {
        void* target = reinterpret_cast<void*>(GetProcAddress(avFoundation, exportName));
        return target && HookAddress(target, detour, original, label);
    };
    auto installCoreMedia = [coreMedia](const char* exportName, void* detour, auto& original,
                                        const wchar_t* label) {
        void* target = reinterpret_cast<void*>(GetProcAddress(coreMedia, exportName));
        return target && HookAddress(target, detour, original, label);
    };

    g_cfNumberCreate = reinterpret_cast<CFNumberCreateFn>(
        GetProcAddress(coreFoundation, "CFNumberCreate"));
    g_cfArrayCreate = reinterpret_cast<CFArrayCreateFn>(
        GetProcAddress(coreFoundation, "CFArrayCreate"));
    g_cfRelease = reinterpret_cast<CFReleaseFn>(
        GetProcAddress(coreFoundation, "CFRelease"));
    g_cfStringCreateWithCString = reinterpret_cast<CFStringCreateWithCStringFn>(
        GetProcAddress(coreFoundation, "CFStringCreateWithCString"));
    g_cfPreferencesGetAppIntegerValue = reinterpret_cast<CFPreferencesGetAppIntegerValueFn>(
        GetProcAddress(coreFoundation, "CFPreferencesGetAppIntegerValue"));
    g_cfPreferencesGetAppBooleanValue = reinterpret_cast<CFPreferencesGetAppBooleanValueFn>(
        GetProcAddress(coreFoundation, "CFPreferencesGetAppBooleanValue"));
    g_cfTypeArrayCallbacks = reinterpret_cast<void*>(
        GetProcAddress(coreFoundation, "kCFTypeArrayCallBacks"));
    void* currentApplicationExport = reinterpret_cast<void*>(
        GetProcAddress(coreFoundation, "kCFPreferencesCurrentApplication"));
    g_cfPreferencesCurrentApplication = currentApplicationExport
        ? *reinterpret_cast<void**>(currentApplicationExport) : nullptr;
    constexpr std::uint32_t cfStringEncodingUtf8 = 0x08000100;
    if (!g_streamQualityPreferenceKey && g_cfStringCreateWithCString) {
        g_streamQualityPreferenceKey = g_cfStringCreateWithCString(
            nullptr, "preferredStreamPlaybackAudioQuality", cfStringEncodingUtf8);
    }
    if (!g_losslessEnabledPreferenceKey && g_cfStringCreateWithCString) {
        g_losslessEnabledPreferenceKey = g_cfStringCreateWithCString(
            nullptr, "losslessEnabled", cfStringEncodingUtf8);
    }
    g_figAlternateAllowableMediaSubtypeFilterCreate =
        reinterpret_cast<FigAlternateAllowableMediaSubtypeFilterCreateFn>(
            GetProcAddress(coreMedia, "FigAlternateAllowableMediaSubtypeFilterCreate"));

    const bool metadataReady = g_cfNumberCreate && g_cfArrayCreate && g_cfRelease &&
        g_cfTypeArrayCallbacks && g_cfPreferencesGetAppIntegerValue &&
        g_cfPreferencesGetAppBooleanValue && g_cfPreferencesCurrentApplication &&
        g_streamQualityPreferenceKey && g_losslessEnabledPreferenceKey &&
        g_figAlternateAllowableMediaSubtypeFilterCreate;
    if (!metadataReady) return false;

    const bool highestLossless = ammod::quality::Install(coreMedia, coreFoundation, Log);
    if (!highestLossless) {
        Log(L"Highest ALAC quality filter unavailable; quality hooks remain disabled");
        return false;
    }
    const bool preferredRate = installAVCF(
        "AVCFPlayerItemSetPreferredMaximumAudioSampleRate",
        reinterpret_cast<void*>(&HookAVCFSetPreferredMaximumSampleRate),
        g_originalAVCFSetPreferredMaximumSampleRate,
        L"AVCFPlayerItemSetPreferredMaximumAudioSampleRate [quality]");
    const bool variantPreferences = installAVCF(
        "AVCFPlayerItemSetVariantPreferences",
        reinterpret_cast<void*>(&HookAVCFSetVariantPreferences),
        g_originalAVCFSetVariantPreferences,
        L"AVCFPlayerItemSetVariantPreferences [quality]");
    const bool eligibleLossless = installCoreMedia(
        "FigAlternateEligibleLosslessAudioFilterCreate",
        reinterpret_cast<void*>(&HookFigAlternateEligibleLosslessFilterCreate),
        g_originalFigAlternateEligibleLosslessFilterCreate,
        L"FigAlternateEligibleLosslessAudioFilterCreate [quality]");
    const bool ready = preferredRate && variantPreferences && eligibleLossless && highestLossless;
    if (ready) {
        g_qualityHooksInstalled.store(true, std::memory_order_release);
        Log(L"Quality-lock-only hooks ready; audio renderer/WASAPI/PCM hooks are disabled");
    }
    return ready;
}

void DisableExclusiveIntent(const std::wstring& reason, HRESULT hr,
                            const std::wstring& endpoint,
                            const WAVEFORMATEX* format,
                            ammod::ipc::ErrorCategory category);

DWORD WINAPI QualityLockHookLoop(void*) {
    // The quality lock is enabled once its verified native hooks are installed;
    // UI/Broker intent belongs only to the removed audio-core path.
    while (!InstallQualityLockHooks()) {
        ammod::quality::SetEnabled(false);
        Sleep(250);
    }
    ammod::quality::SetEnabled(true);
    Log(L"Quality lock enabled after Agent installation; UI intent does not gate quality policy");
    return 0;
}

std::wstring DeviceFriendlyName(IMMDevice* device) {
    if (!device) return {};
    IPropertyStore* store{};
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store) return {};
    PROPVARIANT value{};
    PropVariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        result = value.pwszVal;
    }
    PropVariantClear(&value);
    store->Release();
    return result;
}

void LogDefaultEndpointEnvironment(ERole role, const wchar_t* roleName) {
    IMMDeviceEnumerator* enumerator{};
    IMMDevice* endpoint{};
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr) && enumerator) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, role, &endpoint);
    }
    if (FAILED(hr) || !endpoint) {
        Log(L"environment defaultEndpoint role=" + std::wstring(roleName) +
            L" unavailable hr=" + HResultText(hr));
        if (endpoint) endpoint->Release();
        if (enumerator) enumerator->Release();
        return;
    }

    DWORD state{};
    endpoint->GetState(&state);
    const auto id = DeviceId(endpoint);
    const auto name = DeviceFriendlyName(endpoint);
    IAudioClient* client{};
    WAVEFORMATEX* mix{};
    REFERENCE_TIME defaultPeriod{};
    REFERENCE_TIME minimumPeriod{};
    const HRESULT activateHr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                   reinterpret_cast<void**>(&client));
    HRESULT mixHr = E_UNEXPECTED;
    HRESULT periodHr = E_UNEXPECTED;
    if (SUCCEEDED(activateHr) && client) {
        mixHr = client->GetMixFormat(&mix);
        periodHr = client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    }

    std::wostringstream snapshot;
    snapshot << L"environment defaultEndpoint role=" << roleName
             << L" state=0x" << std::hex << state << std::dec
             << L" id=" << id << L" name=" << name
             << L" activate=" << HResultText(activateHr)
             << L" mixResult=" << HResultText(mixHr)
             << L" mix{" << FormatText(mix) << L"}"
             << L" periodResult=" << HResultText(periodHr)
             << L" defaultPeriod100ns=" << defaultPeriod
             << L" minimumPeriod100ns=" << minimumPeriod;
    Log(snapshot.str());

    if (mix) CoTaskMemFree(mix);
    if (client) client->Release();
    endpoint->Release();
    enumerator->Release();
}

void LogEnvironmentSnapshot() {
    wchar_t build[64]{};
    DWORD buildBytes = sizeof(build);
    DWORD ubr{};
    DWORD ubrBytes = sizeof(ubr);
    const LSTATUS buildStatus = RegGetValueW(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, build, &buildBytes);
    const LSTATUS ubrStatus = RegGetValueW(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &ubrBytes);
    wchar_t imagePath[32768]{};
    const DWORD imageLength = GetModuleFileNameW(nullptr, imagePath,
                                                 static_cast<DWORD>(std::size(imagePath)));
    Log(L"environment windowsBuild=" +
        std::wstring(buildStatus == ERROR_SUCCESS ? build : L"unknown") +
        L" ubr=" + (ubrStatus == ERROR_SUCCESS ? std::to_wstring(ubr) : std::wstring(L"unknown")) +
        L" processImage=" +
        std::wstring(imageLength != 0 && imageLength < std::size(imagePath) ? imagePath : L"unknown"));

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LogDefaultEndpointEnvironment(eConsole, L"console");
    LogDefaultEndpointEnvironment(eMultimedia, L"multimedia");
    if (coHr == S_OK || coHr == S_FALSE) CoUninitialize();
}

template<std::size_t N>
void CopyText(wchar_t (&destination)[N], std::wstring_view source) {
    const auto count = std::min(source.size(), N - 1);
    wmemcpy(destination, source.data(), count);
    destination[count] = L'\0';
}

void SendAgentEvent(ammod::ipc::MessageType type, ammod::ipc::RuntimeState state,
                    bool enabled, const std::wstring& endpoint = {},
                    const WAVEFORMATEX* format = nullptr, HRESULT hr = S_OK,
                    ammod::ipc::ErrorCategory category = ammod::ipc::ErrorCategory::None,
                    const std::wstring& detail = {}) {
    using namespace ammod::ipc;
    if (!g_ipcConnected.load()) return;
    auto message = NewMessage(type, Role::Agent);
    message.agentPid = GetCurrentProcessId();
    message.state = state;
    message.enabledIntent = enabled ? 1 : 0;
    message.hresult = hr;
    message.error = category;
    CopyText(message.endpointId, endpoint);
    CopyText(message.format, FormatText(format));
    CopyText(message.detail, detail);
    std::lock_guard lock(g_outboundMutex);
    g_outbound.push_back(message);
}

bool ExclusiveRequested() {
    // This predicate is reserved for the removed audio-core path. A stale INI value,
    // a manual injection, or the short IPC-connect window must never enable an audio-core hook.
    return g_ipcConnected.load(std::memory_order_acquire) &&
        g_ipcEnabled.load(std::memory_order_acquire) &&
        g_audioCoreGate.UiEnabled();
}

void AudioCoreRuntimeEventCallback(void* context,
                                   ammod::audio_v2::AudioCoreRuntimeEvent event,
                                   HRESULT result) noexcept {
    auto* runtime = static_cast<ammod::audio_v2::AudioCoreRuntime*>(context);
    if (!runtime) return;
    switch (event) {
    case ammod::audio_v2::AudioCoreRuntimeEvent::Bound: {
        const auto& format = runtime->Coordinator().Format();
        const auto sourceBits = format.sourceValidBits == 0
            ? format.validBits : format.sourceValidBits;
        Log(L"audio core v2 bound active AudioUnit/P0 converter outputFormat{rate=" +
            std::to_wstring(format.sampleRate) + L" channels=" +
            std::to_wstring(format.channels) + L" validBits=" +
            std::to_wstring(format.validBits) + L" containerBits=" +
            std::to_wstring(format.containerBits) + L" sourceBits=" +
            std::to_wstring(sourceBits) + L" encoding=signed-int interleaved=1}");
        Log(L"audio core v2 bound qpc=" + std::to_wstring(CurrentQpc()));
        if (void* pendingLocal = g_v2PendingLocalConverter.load(std::memory_order_acquire);
            pendingLocal && runtime->BoundConverter() == pendingLocal) {
            (void)PromoteV2LocalQueue(pendingLocal);
        }
        SendAgentEvent(ammod::ipc::MessageType::StatusChanged,
                       ammod::ipc::RuntimeState::Probing, true);
        break;
    }
    case ammod::audio_v2::AudioCoreRuntimeEvent::Active: {
        const auto& format = runtime->Coordinator().Format();
        const auto sourceBits = format.sourceValidBits == 0
            ? format.validBits : format.sourceValidBits;
        if (!kNativeAcquisitionPassthrough) {
            g_nativeP0CutoverResumeRequested.store(true, std::memory_order_release);
            Log(L"audio core v2 Mod-owned P0 puller Active resume queued");
        } else {
            Log(L"audio core v2 native acquisition passthrough Active; original Fill/scheduler remains producer");
        }
        Log(L"audio core v2 exclusive sink active; native output gate committed outputFormat{rate=" +
            std::to_wstring(format.sampleRate) + L" channels=" +
            std::to_wstring(format.channels) + L" validBits=" +
            std::to_wstring(format.validBits) + L" containerBits=" +
            std::to_wstring(format.containerBits) + L" sourceBits=" +
            std::to_wstring(sourceBits) + L" encoding=signed-int interleaved=1}");
        Log(L"audio core v2 active qpc=" + std::to_wstring(CurrentQpc()));
        SendAgentEvent(ammod::ipc::MessageType::StatusChanged,
                       ammod::ipc::RuntimeState::Active, true);
        break;
    }
    case ammod::audio_v2::AudioCoreRuntimeEvent::Faulted: {
        const bool playbackRejected = ammod::audio::IsPlaybackRejection(result);
        if (!playbackRejected) {
            // Stop this failed generation locally. The Broker owns the user's
            // persisted intent and immediately re-arms the Agent from the
            // WaitingForStream status sent below.
            g_nativeP0Puller.DisableForUiOff();
            g_ipcEnabled.store(false, std::memory_order_release);
            g_audioCoreGate.SetUiEnabled(false);
            Log(L"audio core v2 faulted; current attempt retired, exclusive intent remains armed result=" +
                HResultText(result));
        } else {
            Log(L"audio core v2 rejected current playback; AME remains armed result=" +
                HResultText(result));
        }
        SendAgentEvent(ammod::ipc::MessageType::AttemptFailed,
                       ammod::ipc::RuntimeState::WaitingForStream,
                       true, {}, nullptr, result,
                       ammod::ipc::CategorizeAudioError(result),
                       playbackRejected ? L"audio_core_v2_playback_rejected"
                                        : L"audio_core_v2_faulted");
        break;
    }
    case ammod::audio_v2::AudioCoreRuntimeEvent::Disabled:
        SendAgentEvent(ammod::ipc::MessageType::StatusChanged,
                       ammod::ipc::RuntimeState::Off, false);
        break;
    case ammod::audio_v2::AudioCoreRuntimeEvent::Telemetry: {
        const auto stats = runtime->Stats();
        const auto& sink = stats.sink;
        const auto& tap = stats.tap;
        const auto& fill = stats.pcmFill;
        Log(L"audio core v2 telemetry capturedFrames=" +
            std::to_wstring(stats.coordinator.capturedFrames) +
            L" acceptedFrames=" + std::to_wstring(tap.acceptedFrames) +
            L" bufferedFrames=" + std::to_wstring(stats.bufferedFrames) +
            L" submittedFrames=" + std::to_wstring(sink.submittedFrames) +
            L" submittedBuffers=" + std::to_wstring(sink.submittedBuffers) +
            L" controlledSilenceFrames=" + std::to_wstring(sink.controlledSilenceFrames) +
            L" sourceWaits=" + std::to_wstring(sink.sourceWaits) +
            L" droppedFrames=" + std::to_wstring(stats.coordinator.droppedFrames) +
            L" sourceMappingFailures=" + std::to_wstring(stats.coordinator.source.mappingFailures) +
            L" integerizedSamples=" + std::to_wstring(stats.coordinator.source.integerizedSamples) +
            L" clippedSamples=" + std::to_wstring(stats.coordinator.source.clippedSamples) +
            L" nonFiniteSamples=" + std::to_wstring(stats.coordinator.source.nonFiniteSamples) +
            L" underruns=" + std::to_wstring(sink.underruns) +
            L" getFailures=" + std::to_wstring(sink.getBufferFailures) +
            L" releaseFailures=" + std::to_wstring(sink.releaseBufferFailures) +
            L" rawFillCalls=" + std::to_wstring(fill.rawFillCalls) +
            L" fillSuccessWithData=" + std::to_wstring(fill.fillSuccessWithData) +
            L" fillSuccessZeroPackets=" + std::to_wstring(fill.fillSuccessZeroPackets) +
            L" fillErrors=" + std::to_wstring(fill.fillErrors) +
            L" producedPackets=" + std::to_wstring(fill.producedPackets) +
            L" rejectUiOff=" + std::to_wstring(fill.rejectUiOff) +
            L" rejectNoOutputData=" + std::to_wstring(fill.rejectNoOutputData) +
            L" rejectNoOutputPackets=" + std::to_wstring(fill.rejectNoOutputPackets) +
            L" rejectZeroPackets=" + std::to_wstring(fill.rejectZeroPackets) +
            L" rejectNoDestinationFormat=" + std::to_wstring(fill.rejectNoDestinationFormat) +
            L" rejectInvalidBufferList=" + std::to_wstring(fill.rejectInvalidBufferList) +
            L" rejectRegistryMissing=" + std::to_wstring(fill.rejectRegistryMissing) +
            L" rejectRegistryUnbound=" + std::to_wstring(fill.rejectRegistryUnbound) +
            L" rejectRateMismatch=" + std::to_wstring(fill.rejectRateMismatch) +
            L" rejectChannelMismatch=" + std::to_wstring(fill.rejectChannelMismatch) +
            L" rejectParse=" + std::to_wstring(fill.rejectParse) +
            L" rejectClaimFrames=" + std::to_wstring(fill.rejectClaimFrames) +
            L" rejectTap=" + std::to_wstring(fill.rejectTap) +
            L" acceptedCalls=" + std::to_wstring(fill.acceptedCalls) +
            L" acceptedFrames=" + std::to_wstring(fill.acceptedFrames) +
            L" lastFillResult=" + std::to_wstring(fill.lastResult) +
            L" lastProducedPackets=" + std::to_wstring(fill.lastProducedPackets));
        double lastSampleTime{};
        const auto lastSampleBits = g_v2P1LastSampleTimeBits.load(std::memory_order_acquire);
        std::memcpy(&lastSampleTime, &lastSampleBits, sizeof(lastSampleTime));
        Log(L"audio core v2 p1 telemetry observedRenderCalls=" +
            std::to_wstring(g_v2ObservedRenderCalls.load(std::memory_order_acquire)) +
            L" signatureValid=" +
            std::to_wstring(g_v2P1SignatureValidCalls.load(std::memory_order_acquire)) +
            L" signatureInvalid=" +
            std::to_wstring(g_v2P1SignatureInvalidCalls.load(std::memory_order_acquire)) +
            L" nonZero=" +
            std::to_wstring(g_v2P1SignatureNonZeroCalls.load(std::memory_order_acquire)) +
            L" allZero=" +
            std::to_wstring(g_v2P1SignatureAllZeroCalls.load(std::memory_order_acquire)) +
            L" repeatedSignature=" +
            std::to_wstring(g_v2P1RepeatedSignatureCalls.load(std::memory_order_acquire)) +
            L" timestampMissing=" +
            std::to_wstring(g_v2P1TimestampMissingCalls.load(std::memory_order_acquire)) +
            L" timestampRepeated=" +
            std::to_wstring(g_v2P1TimestampRepeatedCalls.load(std::memory_order_acquire)) +
            L" timestampDiscontinuities=" +
            std::to_wstring(g_v2P1TimestampDiscontinuities.load(std::memory_order_acquire)) +
            L" renderErrors=" +
            std::to_wstring(g_v2P1RenderErrors.load(std::memory_order_acquire)) +
            L" lastSignature=" +
            std::to_wstring(g_v2P1LastSignature.load(std::memory_order_acquire)) +
            L" lastSignatureBytes=" +
            std::to_wstring(g_v2P1LastSignatureBytes.load(std::memory_order_acquire)) +
            L" lastDeclaredBytes=" +
            std::to_wstring(g_v2P1LastDeclaredBytes.load(std::memory_order_acquire)) +
            L" lastZeroBytes=" +
            std::to_wstring(g_v2P1LastZeroBytes.load(std::memory_order_acquire)) +
            L" lastFrames=" +
            std::to_wstring(g_v2P1LastFrames.load(std::memory_order_acquire)) +
            L" lastBufferCount=" +
            std::to_wstring(g_v2P1LastBufferCount.load(std::memory_order_acquire)) +
            L" lastSampleTime=" + std::to_wstring(lastSampleTime) +
            L" lastHostTime=" +
            std::to_wstring(g_v2P1LastHostTime.load(std::memory_order_acquire)) +
            L" lastTimestampFlags=" +
            std::to_wstring(g_v2P1LastTimestampFlags.load(std::memory_order_acquire)) +
            L" lastQpc=" +
            std::to_wstring(g_v2P1LastQpc.load(std::memory_order_acquire)));
        break;
    }
    }
}

ammod::audio_v2::ApplePcmFormatView ApplePcmFormatViewFromAsbd(
    const AudioStreamBasicDescription* format) noexcept {
    if (!format || !std::isfinite(format->mSampleRate) || format->mSampleRate < 0.0 ||
        format->mSampleRate > static_cast<double>(UINT32_MAX)) return {};
    return {
        format->mFormatID,
        format->mFormatFlags,
        format->mBytesPerPacket,
        format->mFramesPerPacket,
        format->mBytesPerFrame,
        format->mChannelsPerFrame,
        format->mBitsPerChannel,
        static_cast<std::uint32_t>(format->mSampleRate + 0.5),
    };
}

bool IsV2SupportedLocalSource(const AudioStreamBasicDescription* source,
                              const AudioStreamBasicDescription* destination,
                              ammod::audio::LocalPcmCandidate* candidate = nullptr) noexcept {
    if (!source || !destination) return false;
    ammod::audio::LocalPcmCandidate local{};
    if (!ammod::audio::TryClassifyLocalPcmCandidate(
            PcmFormatView(*source), PcmFormatView(*destination), local)) {
        return false;
    }
    constexpr std::uint32_t kBigEndian = 1u << 1;
    if ((source->mFormatFlags & ammod::audio::kFormatFlagIsNonInterleaved) != 0 ||
        (source->mFormatFlags & kBigEndian) != 0 || source->mBytesPerFrame == 0 ||
        source->mBytesPerFrame > 8 || source->mBytesPerFrame % 2 != 0) {
        return false;
    }
    const auto bytesPerSample = source->mBytesPerFrame / 2u;
    if (local.sourceIsFloat) {
        if (local.sourceBitDepth != 32 || source->mBytesPerFrame != 8 ||
            bytesPerSample != sizeof(float)) {
            return false;
        }
    } else {
        if (local.sourceBitDepth > 24 ||
            (source->mFormatFlags & ammod::audio::kFormatFlagIsSignedInteger) == 0 ||
            bytesPerSample == 0 || bytesPerSample > 4 ||
            bytesPerSample * 8u < local.sourceBitDepth) {
            return false;
        }
    }
    if (candidate) *candidate = local;
    return true;
}

std::shared_ptr<LocalPcmQueue> CreateV2LocalQueue(
    void* converter, const AudioStreamBasicDescription* source,
    const AudioStreamBasicDescription* destination) {
    if (!converter) return {};
    ammod::audio::LocalPcmCandidate local{};
    if (!IsV2SupportedLocalSource(source, destination, &local)) return {};
    auto queue = std::make_shared<LocalPcmQueue>();
    queue->sampleRate = local.sampleRate;
    queue->channels = local.channels;
    queue->bitsPerChannel = local.sourceBitDepth;
    queue->bytesPerFrame = source->mBytesPerFrame;
    queue->formatFlags = source->mFormatFlags;
    // Historical local-file traces showed roughly 6.36 s of eager decode.
    // Pre-size ten seconds outside the callback and retain the old bounded
    // growth policy for unusual files. Never block Apple's producer thread.
    queue->maximumBytes = static_cast<std::size_t>(local.sampleRate) *
        source->mBytesPerFrame * 10u;
    try {
        queue->storage.resize(queue->maximumBytes);
    } catch (const std::bad_alloc&) {
        return {};
    }
    {
        std::lock_guard lock(g_converterProbeMutex);
        g_converterProbes[converter] = {queue};
    }
    g_v2PendingLocalConverter.store(converter, std::memory_order_release);
    Log(L"v2 local PCM staging queue created converter=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) + L" rate=" +
        std::to_wstring(local.sampleRate) + L" bits=" +
        std::to_wstring(local.sourceBitDepth) + L" numeric=" +
        (local.sourceIsFloat ? std::wstring(L"float") : std::wstring(L"integer")) +
        L" capacityFrames=" + std::to_wstring(queue->maximumBytes / queue->bytesPerFrame));
    return queue;
}

std::shared_ptr<LocalPcmQueue> FindV2LocalQueue(void* converter) {
    std::lock_guard lock(g_converterProbeMutex);
    const auto found = g_converterProbes.find(converter);
    return found != g_converterProbes.end() ? found->second.localQueue : nullptr;
}

std::uint32_t V2LocalQueueAvailableFrames(const std::shared_ptr<LocalPcmQueue>& queue) noexcept {
    if (!queue) return 0;
    std::lock_guard lock(queue->mutex);
    if (!queue->bytesPerFrame) return 0;
    const auto frames = queue->sizeBytes / queue->bytesPerFrame;
    return frames > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(frames);
}

bool ConvertV2LocalToCanonical(const LocalPcmQueue& queue, const BYTE* raw,
                               std::uint32_t frames, float* canonical) noexcept {
    if (!raw || !canonical || frames == 0 || queue.channels != 2 ||
        queue.bytesPerFrame == 0 || queue.bytesPerFrame % 2 != 0) {
        return false;
    }
    constexpr std::uint32_t kBigEndian = 1u << 1;
    constexpr std::uint32_t kAlignedHigh = 1u << 4;
    if ((queue.formatFlags & ammod::audio::kFormatFlagIsNonInterleaved) != 0 ||
        (queue.formatFlags & kBigEndian) != 0) {
        return false;
    }

    const auto sampleCount = static_cast<std::size_t>(frames) * 2u;
    if ((queue.formatFlags & ammod::audio::kFormatFlagIsFloat) != 0) {
        if (queue.bitsPerChannel != 32 || queue.bytesPerFrame != sizeof(float) * 2u) {
            return false;
        }
        std::memcpy(canonical, raw, sampleCount * sizeof(float));
        return true;
    }

    if ((queue.formatFlags & ammod::audio::kFormatFlagIsSignedInteger) == 0 ||
        queue.bitsPerChannel == 0 || queue.bitsPerChannel > 24) {
        return false;
    }
    const auto bytesPerSample = queue.bytesPerFrame / 2u;
    if (bytesPerSample == 0 || bytesPerSample > 4 ||
        bytesPerSample * 8u < queue.bitsPerChannel) {
        return false;
    }
    const auto containerBits = bytesPerSample * 8u;
    const auto validBits = queue.bitsPerChannel;
    const auto validMask = (1u << validBits) - 1u;
    const auto signBit = 1u << (validBits - 1u);
    const bool alignedHigh = (queue.formatFlags & kAlignedHigh) != 0;
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const BYTE* p = raw + index * bytesPerSample;
        std::uint32_t packed{};
        for (std::uint32_t byte = 0; byte < bytesPerSample; ++byte) {
            packed |= static_cast<std::uint32_t>(p[byte]) << (byte * 8u);
        }
        if (alignedHigh && containerBits > validBits) packed >>= (containerBits - validBits);
        packed &= validMask;
        const std::int32_t code = (packed & signBit)
            ? static_cast<std::int32_t>(packed | ~validMask)
            : static_cast<std::int32_t>(packed);
        // <=24-bit signed integer codes are exactly representable in float32;
        // division by a power of two is exact. ExactSampleMapper later proves
        // that the endpoint integer code round-trips unchanged.
        canonical[index] = std::ldexp(static_cast<float>(code),
                                      -static_cast<int>(validBits - 1u));
    }
    return true;
}

bool DrainV2LocalQueue(void* converter, const std::shared_ptr<LocalPcmQueue>& queue) {
    if (!converter || !queue || !queue->active || !g_audioCoreRuntime.IsConverterBound(converter)) {
        return false;
    }
    thread_local std::array<BYTE, ammod::audio_v2::kMaxBlockFrames * 8u> raw{};
    thread_local std::array<float, ammod::audio_v2::kMaxBlockFrames * 2u> canonical{};
    bool forwarded{};
    for (;;) {
        // Leave headroom in the proven fixed-capacity ACv2 queue. The local
        // staging queue remains the lossless back-pressure reservoir.
        if (g_audioCoreRuntime.Coordinator().BufferedBlocks() >= 240) break;
        const auto available = V2LocalQueueAvailableFrames(queue);
        if (available == 0) break;
        const auto frames = std::min<std::uint32_t>(
            available, ammod::audio_v2::kMaxBlockFrames);
        std::uint32_t copied{};
        if (!queue->Pop(raw.data(), frames, &copied) || copied != frames) break;
        if (!ConvertV2LocalToCanonical(*queue, raw.data(), frames, canonical.data())) {
            queue->droppedFrames += frames;
            Log(L"v2 local PCM canonicalization failed converter=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)));
            break;
        }
        const ammod::audio_v2::ApplePcmFormatView canonicalFormat{
            ammod::audio_v2::kAppleLinearPcm,
            ammod::audio_v2::kAppleFormatFlagIsFloat,
            static_cast<std::uint32_t>(sizeof(float) * 2u), 1u,
            static_cast<std::uint32_t>(sizeof(float) * 2u), 2u, 32u,
            queue->sampleRate};
        const ammod::audio_v2::ApplePcmBufferView buffer{
            2u, frames * static_cast<std::uint32_t>(sizeof(float) * 2u), canonical.data()};
        const ammod::audio_v2::ApplePcmBufferListView list{1u, &buffer};
        if (!g_audioCoreRuntime.OnPcmOutput(converter, canonicalFormat, list, frames)) {
            queue->droppedFrames += frames;
            Log(L"v2 local PCM ACv2 intake rejected after staging dequeue converter=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                L" frames=" + std::to_wstring(frames));
            break;
        }
        forwarded = true;
    }
    return forwarded;
}

bool PromoteV2LocalQueue(void* converter) {
    auto queue = FindV2LocalQueue(converter);
    if (!queue) return false;
    {
        std::lock_guard lock(queue->mutex);
        if (queue->droppedFrames != 0) {
            Log(L"v2 local PCM promotion refused because staging already dropped frames converter=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                L" droppedFrames=" + std::to_wstring(queue->droppedFrames));
            return false;
        }
        queue->active = true;
        queue->abandoned = false;
    }
    (void)DrainV2LocalQueue(converter, queue);
    void* expected = converter;
    g_v2PendingLocalConverter.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    Log(L"v2 local PCM staging promoted converter=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) + L" queuedFrames=" +
        std::to_wstring(V2LocalQueueAvailableFrames(queue)));
    return true;
}

AppleOSStatus __cdecl HookV2AudioConverterNew(const AudioStreamBasicDescription* source,
                                              const AudioStreamBasicDescription* destination,
                                              void** converter) {
    const auto result = g_v2OriginalAudioConverterNew
        ? g_v2OriginalAudioConverterNew(source, destination, converter) : -1;
    if (result == 0 && converter && *converter &&
        g_nativeTraceEnabled.load(std::memory_order_acquire)) {
        Log(L"native trace AudioConverterNew converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(*converter)) +
            L" caller=" + CaptureCurrentStackText(2, 8) +
            L" source{" + AppleFormatText(source) + L"} destination{" +
            AppleFormatText(destination) + L"}");
    }
    if (result == 0 && converter && *converter && g_audioCoreRuntime.UiEnabled()) {
        ObserveDecodedSourceFormat(source, destination, *converter, _ReturnAddress());
        (void)CreateV2LocalQueue(*converter, source, destination);
        const auto sourceView = ApplePcmFormatViewFromAsbd(source);
        const auto destinationView = ApplePcmFormatViewFromAsbd(destination);
        g_audioCoreRuntime.OnConverterCreated(
            *converter, sourceView, destinationView, g_threadGraphFormat.observationId);
        Log(L"v2 AudioConverterNew observed converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(*converter)) +
            L" source=" + AppleFormatIdText(source ? source->mFormatID : 0) +
            L" destination=" + AppleFormatIdText(destination ? destination->mFormatID : 0) +
            L" observation=" + std::to_wstring(g_threadGraphFormat.observationId) +
            L" rate=" + std::to_wstring(destinationView.sampleRate) +
            L" channels=" + std::to_wstring(destinationView.channelsPerFrame) +
            L" flags=0x" + HResultText(static_cast<HRESULT>(destinationView.formatFlags)).substr(2) +
            L" bits=" + std::to_wstring(destinationView.bitsPerChannel) +
            L" registered=" + std::to_wstring(g_audioCoreRuntime.IsConverterRegistered(*converter)));
    }
    return result;
}

AppleOSStatus __cdecl HookV2AudioConverterNewSpecific(
    const AudioStreamBasicDescription* source,
    const AudioStreamBasicDescription* destination,
    std::uint32_t classDescriptionCount, const void* classDescriptions,
    void** converter) {
    const auto result = g_v2OriginalAudioConverterNewSpecific
        ? g_v2OriginalAudioConverterNewSpecific(source, destination, classDescriptionCount,
                                                 classDescriptions, converter) : -1;
    if (result == 0 && converter && *converter &&
        g_nativeTraceEnabled.load(std::memory_order_acquire)) {
        Log(L"native trace AudioConverterNewSpecific converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(*converter)) +
            L" classDescriptions=" + std::to_wstring(classDescriptionCount) +
            L" caller=" + CaptureCurrentStackText(2, 8) +
            L" source{" + AppleFormatText(source) + L"} destination{" +
            AppleFormatText(destination) + L"}");
    }
    if (result == 0 && converter && *converter && g_audioCoreRuntime.UiEnabled()) {
        ObserveDecodedSourceFormat(source, destination, *converter, _ReturnAddress());
        (void)CreateV2LocalQueue(*converter, source, destination);
        const auto sourceView = ApplePcmFormatViewFromAsbd(source);
        const auto destinationView = ApplePcmFormatViewFromAsbd(destination);
        g_audioCoreRuntime.OnConverterCreated(
            *converter, sourceView, destinationView, g_threadGraphFormat.observationId);
        Log(L"v2 AudioConverterNewSpecific observed converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(*converter)) +
            L" source=" + AppleFormatIdText(source ? source->mFormatID : 0) +
            L" destination=" + AppleFormatIdText(destination ? destination->mFormatID : 0) +
            L" observation=" + std::to_wstring(g_threadGraphFormat.observationId) +
            L" rate=" + std::to_wstring(destinationView.sampleRate) +
            L" channels=" + std::to_wstring(destinationView.channelsPerFrame) +
            L" flags=0x" + HResultText(static_cast<HRESULT>(destinationView.formatFlags)).substr(2) +
            L" bits=" + std::to_wstring(destinationView.bitsPerChannel) +
            L" registered=" + std::to_wstring(g_audioCoreRuntime.IsConverterRegistered(*converter)));
    }
    return result;
}

AppleOSStatus __cdecl HookV2AudioConverterSetProperty(void* converter,
                                                      std::uint32_t propertyId,
                                                      std::uint32_t dataSize,
                                                      const void* data) {
    const bool serializedCursorUpdate = propertyId ==
        ammod::apple_private::audio_converter_property::kCursor &&
        g_nativeP0Puller.Tracks(converter);
    std::unique_lock<std::mutex> p0PropertyLock;
    if (serializedCursorUpdate) p0PropertyLock = std::unique_lock<std::mutex>(g_p0FillMutex);
    const auto result = g_v2OriginalAudioConverterSetProperty
        ? g_v2OriginalAudioConverterSetProperty(converter, propertyId, dataSize, data) : -1;
    if (serializedCursorUpdate && result == 0) {
        g_nativeP0Puller.ResumeAfterCursorProperty(converter);
    }
    if (g_nativeTraceEnabled.load(std::memory_order_acquire) &&
        g_nativeTracePropertyCalls.fetch_add(1, std::memory_order_relaxed) < 256) {
        Log(L"native trace AudioConverterSetProperty converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
            L" property=0x" + HResultText(static_cast<HRESULT>(propertyId)).substr(2) +
            L" size=" + std::to_wstring(dataSize) + L" data=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(data)) +
            L" result=" + std::to_wstring(result) + NativeTraceScalarText(data, dataSize) +
            L" stack=" +
            CaptureCurrentStackText(2, 6));
    }
    if (result == 0 && g_audioCoreRuntime.UiEnabled()) {
        ObserveAudioConverterProperty(L"SetProperty", converter, propertyId, data, dataSize,
                                      result);
        constexpr std::uint32_t decompressionMagicCookie = 0x646D6763; // 'dmgc'
        if (propertyId == decompressionMagicCookie && data && dataSize) {
            std::uint32_t bits{}, channels{}, rate{}, offset{};
            if (ParseAlacSpecificConfig(data, dataSize, bits, channels, rate, offset)) {
                Log(L"v2 ALAC precision observed converter=" +
                    std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                    L" rate=" + std::to_wstring(rate) + L" channels=" +
                    std::to_wstring(channels) + L" bits=" + std::to_wstring(bits));
                g_audioCoreRuntime.OnConverterSourcePrecision(converter, bits, rate, channels);
                // Apple can create the long-lived decoder through an internal
                // component path that never calls the exported
                // AudioConverterNew/NewSpecific functions. Refresh both ASBDs
                // through the original GetProperty trampoline so that this
                // cookie-bearing handle becomes a registered P0 candidate.
                Log(L"v2 ALAC cookie refresh enter converter=" +
                    std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                    L" getProperty=" + std::to_wstring(
                        reinterpret_cast<std::uintptr_t>(g_v2OriginalAudioConverterGetProperty)));
                if (g_v2OriginalAudioConverterGetProperty) {
                    AudioStreamBasicDescription refreshedSource{};
                    AudioStreamBasicDescription refreshedDestination{};
                    std::uint32_t sourceSize = sizeof(refreshedSource);
                    std::uint32_t destinationSize = sizeof(refreshedDestination);
                    const auto sourceResult = g_v2OriginalAudioConverterGetProperty(
                        converter, ammod::apple_private::audio_converter_property::kCurrentInputDescription, &sourceSize, &refreshedSource);
                    const auto destinationResult = g_v2OriginalAudioConverterGetProperty(
                        converter, ammod::apple_private::audio_converter_property::kCurrentOutputDescription, &destinationSize,
                        &refreshedDestination);
                    Log(L"v2 ALAC cookie refresh query converter=" +
                        std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                        L" sourceResult=" + std::to_wstring(sourceResult) +
                        L" sourceSize=" + std::to_wstring(sourceSize) +
                        L" destinationResult=" + std::to_wstring(destinationResult) +
                        L" destinationSize=" + std::to_wstring(destinationSize) +
                        L" source{" + AppleFormatText(&refreshedSource) + L"} destination{" +
                        AppleFormatText(&refreshedDestination) + L"}");
                    if (sourceResult == 0 && destinationResult == 0 &&
                        sourceSize >= sizeof(refreshedSource) &&
                        destinationSize >= sizeof(refreshedDestination)) {
                        const auto sourceView = ApplePcmFormatViewFromAsbd(&refreshedSource);
                        const auto destinationView = ApplePcmFormatViewFromAsbd(
                            &refreshedDestination);
                        if ((refreshedSource.mFormatID == 0x616C6163 ||
                             refreshedSource.mFormatID == 0x716C6163) &&
                            refreshedDestination.mFormatID == 0x6C70636D) {
                            g_nativeP0Puller.ObserveNativeCookie(
                                converter, data, dataSize, refreshedSource, refreshedDestination);
                            ObserveDecodedSourceFormat(&refreshedSource, &refreshedDestination,
                                                       converter, _ReturnAddress());
                            g_audioCoreRuntime.OnConverterCreated(
                                converter, sourceView, destinationView,
                                g_threadGraphFormat.observationId);
                            Log(L"v2 converter ASBD refreshed after ALAC cookie converter=" +
                                std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                                L" sourceRate=" +
                                std::to_wstring(static_cast<std::uint32_t>(
                                    refreshedSource.mSampleRate + 0.5)) +
                                L" destinationRate=" +
                                std::to_wstring(static_cast<std::uint32_t>(
                                    refreshedDestination.mSampleRate + 0.5)) +
                                L" destinationFlags=0x" +
                                HResultText(static_cast<HRESULT>(
                                    refreshedDestination.mFormatFlags)).substr(2) +
                                L" destinationBits=" +
                                std::to_wstring(refreshedDestination.mBitsPerChannel) +
                                L" registered=" + std::to_wstring(
                                    g_audioCoreRuntime.IsConverterRegistered(converter)));
                        }
                    } else {
                        Log(L"v2 converter ASBD refresh failed after ALAC cookie converter=" +
                            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                            L" sourceResult=" + std::to_wstring(sourceResult) +
                            L" destinationResult=" + std::to_wstring(destinationResult));
                    }
                }
            }
        }
    }
    return result;
}

AppleOSStatus __cdecl HookV2AudioConverterGetProperty(void* converter,
                                                       std::uint32_t propertyId,
                                                       std::uint32_t* dataSize,
                                                       void* data) {
    const auto result = g_v2OriginalAudioConverterGetProperty
        ? g_v2OriginalAudioConverterGetProperty(converter, propertyId, dataSize, data) : -1;
    if (g_nativeTraceEnabled.load(std::memory_order_acquire) &&
        g_nativeTracePropertyCalls.fetch_add(1, std::memory_order_relaxed) < 256) {
        Log(L"native trace AudioConverterGetProperty converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
            L" property=0x" + HResultText(static_cast<HRESULT>(propertyId)).substr(2) +
            L" size=" + std::to_wstring(dataSize ? *dataSize : 0) +
            L" data=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(data)) +
            L" result=" + std::to_wstring(result) + NativeTraceScalarText(
                data, dataSize ? *dataSize : 0) + L" stack=" +
            CaptureCurrentStackText(2, 6));
    }
    if (result == 0 && g_audioCoreRuntime.UiEnabled() && data && dataSize &&
        *dataSize >= sizeof(AudioStreamBasicDescription) &&
        (propertyId == ammod::apple_private::audio_converter_property::kCurrentInputDescription ||
         propertyId == ammod::apple_private::audio_converter_property::kCurrentOutputDescription)) {
        const auto* format = static_cast<const AudioStreamBasicDescription*>(data);
        Log(L"v2 AudioConverterGetProperty converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
            L" property=" + AppleFormatIdText(propertyId) + L" " +
            AppleFormatText(format));
        if (propertyId == ammod::apple_private::audio_converter_property::kCurrentOutputDescription && format->mFormatID == 0x6C70636D &&
            g_v2OriginalAudioConverterGetProperty) {
            AudioStreamBasicDescription source{};
            std::uint32_t sourceSize = sizeof(source);
            if (g_v2OriginalAudioConverterGetProperty(
                    converter, ammod::apple_private::audio_converter_property::kCurrentInputDescription, &sourceSize, &source) == 0 &&
                sourceSize >= sizeof(source) && source.mFormatID == 0x716C6163) {
                const auto sourceView = ApplePcmFormatViewFromAsbd(&source);
                const auto destinationView = ApplePcmFormatViewFromAsbd(format);
                ObserveDecodedSourceFormat(&source, format, converter, _ReturnAddress());
                g_audioCoreRuntime.OnConverterCreated(
                    converter, sourceView, destinationView,
                    g_threadGraphFormat.observationId);
                Log(L"v2 converter registered from output ASBD converter=" +
                    std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                    L" registered=" + std::to_wstring(
                        g_audioCoreRuntime.IsConverterRegistered(converter)));
            }
        }
    }
    return result;
}

AppleOSStatus __cdecl HookV2AudioConverterFillComplexBuffer(
    void* converter, AudioConverterComplexInputDataProcFn inputProc, void* inputUserData,
    std::uint32_t* outputPackets, void* outputData, void* packetDescriptions) {
    const bool p0Candidate = g_audioCoreRuntime.IsP0Converter(converter);
    std::unique_lock<std::mutex> p0FillLock;
    if (p0Candidate) {
        p0FillLock = std::unique_lock<std::mutex>(g_p0FillMutex);
        // After the Mod-owned worker has successfully reached the bound P0
        // tap, the Apple caller is kept alive with the same normal empty-read
        // status used by Apple's callback.  It never enters the decoder again;
        // only the worker below calls the original Fill ABI.
        if (!kNativeAcquisitionPassthrough &&
            g_nativeP0Puller.ShouldSuppressNativeFill(converter)) {
            if (outputPackets) *outputPackets = 0;
            if (outputData) {
                auto* list = static_cast<AppleAudioBufferList*>(outputData);
                if (list->mNumberBuffers <= 8) {
                    for (std::uint32_t index = 0; index < list->mNumberBuffers; ++index) {
                        list->mBuffers[index].mDataByteSize = 0;
                    }
                }
            }
            static std::atomic<std::uint64_t> suppressedCalls{};
            const auto suppressed = suppressedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
            if (suppressed <= 4 || suppressed % 256u == 0) {
                Log(L"audio core v2 native P0 Fill suppressed; Mod-owned producer active converter=" +
                    std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                    L" call=" + std::to_wstring(suppressed));
            }
            return 3;
        }
    }
    const bool nativeTrace = g_nativeTraceEnabled.load(std::memory_order_acquire);
    const auto localQueue = FindV2LocalQueue(converter);
    const auto traceCall = nativeTrace
        ? g_nativeTraceFillCalls.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    const auto requestedPackets = outputPackets ? *outputPackets : 0u;
    const auto enterQpc = nativeTrace ? CurrentQpc() : 0;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    struct NativeInputTraceContext final {
        AudioConverterComplexInputDataProcFn original{};
        void* userData{};
        void* converter{};
        std::uint64_t fillCall{};
        std::uint32_t inputCalls{};
        NativeTraceBufferShape lastBuffers{};
        NativeTracePacketShape lastPackets{};
        std::uint32_t lastRequested{};
        std::uint32_t lastReturned{};
        AppleOSStatus lastResult{};
        std::int64_t lastEnterQpc{};
        std::int64_t lastReturnQpc{};
        std::shared_ptr<LocalPcmQueue> localQueue;
    } inputContext{inputProc, inputUserData, converter, traceCall, 0, {}, {}, 0, 0, 0, 0, 0,
                   localQueue};
    const auto proxyInput = +[](void* inputConverter, std::uint32_t* packets, void* data,
                                void** descriptions, void* context) -> AppleOSStatus {
        auto* trace = static_cast<NativeInputTraceContext*>(context);
        const auto requested = packets ? *packets : 0u;
        const auto enter = CurrentQpc();
        const auto result = trace->original(inputConverter, packets, data, descriptions,
                                            trace->userData);
        const auto returned = packets ? *packets : 0u;
        const auto exit = CurrentQpc();
        ++trace->inputCalls;
        trace->lastRequested = requested;
        trace->lastReturned = returned;
        trace->lastResult = result;
        trace->lastEnterQpc = enter;
        trace->lastReturnQpc = exit;
        trace->lastBuffers = InspectNativeTraceBuffers(data);
        trace->lastPackets = InspectNativeTraceInputPackets(descriptions, returned);
        if (result == 0 && trace->localQueue && data) {
            const auto* list = static_cast<const AppleAudioBufferList*>(data);
            if (list->mNumberBuffers == 1 && list->mBuffers[0].mData &&
                list->mBuffers[0].mDataByteSize != 0 && returned != 0 &&
                list->mBuffers[0].mDataByteSize ==
                    static_cast<std::uint64_t>(returned) * trace->localQueue->bytesPerFrame) {
                trace->localQueue->Push(list->mBuffers[0].mData,
                                        list->mBuffers[0].mDataByteSize);
                if (!trace->localQueue->active) {
                    g_audioCoreRuntime.RequestLocalSuccessorBind(trace->converter);
                }
                if (trace->localQueue->active) {
                    (void)DrainV2LocalQueue(trace->converter, trace->localQueue);
                }
            }
        }
        if (g_nativeTraceEnabled.load(std::memory_order_acquire) &&
            trace->inputCalls <= 16 &&
            (trace->fillCall <= 64 || result != 0)) {
            Log(L"native trace input callback converter=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(trace->converter)) +
                L" fillCall=" + std::to_wstring(trace->fillCall) +
                L" inputCall=" + std::to_wstring(trace->inputCalls) +
                L" inputConverter=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(inputConverter)) +
                L" requestedPackets=" + std::to_wstring(requested) +
                L" returnedPackets=" + std::to_wstring(returned) +
                L" data=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(data)) +
                L" buffers=" + std::to_wstring(trace->lastBuffers.bufferCount) +
                L" firstData=" + std::to_wstring(trace->lastBuffers.firstData) +
                L" firstBytes=" + std::to_wstring(trace->lastBuffers.firstBytes) +
                L" firstChannels=" + std::to_wstring(trace->lastBuffers.firstChannels) +
                L" packetDescriptions=" + std::to_wstring(trace->lastPackets.descriptions) +
                L" firstOffset=" + std::to_wstring(trace->lastPackets.firstOffset) +
                L" firstPacketFrames=" + std::to_wstring(trace->lastPackets.firstFrames) +
                L" firstPacketBytes=" + std::to_wstring(trace->lastPackets.firstBytes) +
                L" result=" + std::to_wstring(result) +
                L" enterQpc=" + std::to_wstring(enter) +
                L" returnQpc=" + std::to_wstring(exit));
        }
        return result;
    };
    const bool wrapInput = inputProc && (nativeTrace || localQueue);
    const auto result = g_v2OriginalAudioConverterFillComplexBuffer
        ? g_v2OriginalAudioConverterFillComplexBuffer(
            converter, wrapInput ? proxyInput : inputProc,
            wrapInput ? static_cast<void*>(&inputContext) : inputUserData,
            outputPackets, outputData, packetDescriptions) : -1;
    const auto producedPackets = outputPackets ? *outputPackets : 0u;
    if (p0Candidate) {
        // Capture the real callback/user-data pair even when this particular
        // Fill has no decoded frames.  Binding can complete on a later graph
        // event, while the pair remains valid for the same converter.
        g_nativeP0Puller.ObserveNativeFill(converter, inputProc, inputUserData, requestedPackets);
    }
    if (nativeTrace && (traceCall <= 64 || result != 0 || traceCall % 256u == 0)) {
        const auto outputShape = InspectNativeTraceBuffers(outputData);
        Log(L"native trace fill converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
            L" call=" + std::to_wstring(traceCall) +
            L" inputProc=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(inputProc)) +
            L" inputUserData=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(inputUserData)) +
            L" requestedPackets=" + std::to_wstring(requestedPackets) +
            L" producedPackets=" + std::to_wstring(producedPackets) +
            L" outputData=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(outputData)) +
            L" buffers=" + std::to_wstring(outputShape.bufferCount) +
            L" firstData=" + std::to_wstring(outputShape.firstData) +
            L" firstBytes=" + std::to_wstring(outputShape.firstBytes) +
            L" firstChannels=" + std::to_wstring(outputShape.firstChannels) +
            L" packetDescriptions=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(packetDescriptions)) +
            L" inputCalls=" + std::to_wstring(inputContext.inputCalls) +
            L" inputLastRequested=" + std::to_wstring(inputContext.lastRequested) +
            L" inputLastReturned=" + std::to_wstring(inputContext.lastReturned) +
            L" inputLastResult=" + std::to_wstring(inputContext.lastResult) +
            L" enterQpc=" + std::to_wstring(enterQpc) +
            L" returnQpc=" + std::to_wstring(CurrentQpc()) +
            L" caller=" + std::to_wstring(caller) +
            L" stack=" + CaptureCurrentStackText(2, 8) +
            L" result=" + std::to_wstring(result));
    }
    g_audioCoreRuntime.OnPcmFillResult(result, producedPackets, outputData != nullptr,
                                       outputPackets != nullptr);
    if (p0Candidate && result == 0 && producedPackets == 0 &&
        g_audioCoreRuntime.IsConverterBound(converter)) {
        const bool accepted = g_audioCoreRuntime.OnStreamingProducerEndOfStream(converter);
        Log(L"v2 streaming producer EOF candidate converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
            L" accepted=" + std::to_wstring(accepted) +
            L" qpc=" + std::to_wstring(CurrentQpc()));
    }
    // For local PCM, the source-side callback is Local P0. The converter
    // output is Apple's already-resampled work-format PCM and must never be
    // forwarded into ACv2.
    if (localQueue) return result;
    // Apple can return a nonzero status when its input callback has no new
    // encoded packet while the converter still flushes decoded PCM already
    // buffered internally. Keep the native status untouched, but do not
    // discard a P0 block from the v2 observer/queue for that reason.
    if (!ammod::audio_v2::ShouldForwardPcmOutput(
            g_audioCoreRuntime.UiEnabled(), result, producedPackets,
            outputData != nullptr, outputPackets != nullptr)) {
        return result;
    }
    ammod::audio_v2::ApplePcmFormatView format{};
    if (!g_audioCoreRuntime.DestinationFormat(converter, format)) {
        g_audioCoreRuntime.RecordPcmReject(
            ammod::audio_v2::AudioCorePcmRejectReason::NoDestinationFormat);
        return result;
    }
    const auto* list = static_cast<const AppleAudioBufferList*>(outputData);
    const auto count = list->mNumberBuffers;
    if (count == 0 || count > 8) {
        g_audioCoreRuntime.RecordPcmReject(
            ammod::audio_v2::AudioCorePcmRejectReason::InvalidBufferList);
        return result;
    }
    std::array<ammod::audio_v2::ApplePcmBufferView, 8> buffers{};
    for (std::uint32_t index = 0; index < count; ++index) {
        buffers[index] = {
            list->mBuffers[index].mNumberChannels,
            list->mBuffers[index].mDataByteSize,
            list->mBuffers[index].mData,
        };
    }
    const ammod::audio_v2::ApplePcmBufferListView view{count, buffers.data()};
    g_audioCoreRuntime.OnPcmOutput(converter, format, view, producedPackets);
    return result;
}

AppleOSStatus __cdecl HookV2AudioConverterReset(void* converter) {
    const auto localQueue = FindV2LocalQueue(converter);
    bool localSeekReset{};
    if (localQueue) {
        std::array<void*, ammod::apple_private::agent_stack::kLocalSeekResetFrames> resetStack{};
        const auto count = CaptureStackBackTrace(
            1, static_cast<DWORD>(resetStack.size()), resetStack.data(), nullptr);
        const auto agentBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto rva = [agentBase](void* address) -> std::uintptr_t {
            const auto value = reinterpret_cast<std::uintptr_t>(address);
            return agentBase && value >= agentBase ? value - agentBase : 0;
        };
        localSeekReset = count >= 3 &&
            rva(resetStack[0]) == ammod::apple_private::agent_stack::kLocalSeekResetFrame0 &&
            rva(resetStack[1]) == ammod::apple_private::agent_stack::kLocalSeekResetFrame1 &&
            (rva(resetStack[2]) == ammod::apple_private::agent_stack::kLocalSeekResetFrame2[0] ||
             rva(resetStack[2]) == ammod::apple_private::agent_stack::kLocalSeekResetFrame2[1]);
    }
    const bool trackedP0 = g_nativeP0Puller.Tracks(converter);
    if (trackedP0) g_nativeP0Puller.PauseForReset(converter);
    std::unique_lock<std::mutex> p0ResetLock;
    if (trackedP0) p0ResetLock = std::unique_lock<std::mutex>(g_p0FillMutex);
    const auto result = g_v2OriginalAudioConverterReset
        ? g_v2OriginalAudioConverterReset(converter) : -1;
    if (trackedP0 && result != 0) g_nativeP0Puller.ResumeAfterCursorProperty(converter);
    if (g_nativeTraceEnabled.load(std::memory_order_acquire)) {
        Log(L"native trace AudioConverterReset converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
            L" result=" + std::to_wstring(result) + L" stack=" +
            CaptureCurrentStackText(2, 8));
    }
    if (result == 0 && g_audioCoreRuntime.UiEnabled()) {
        if (trackedP0) g_nativeP0Puller.ResetAfterNativeReset(converter);
        if (localQueue && localSeekReset) localQueue->FlushForSeek();
        g_audioCoreRuntime.OnConverterReset(converter, localSeekReset);
        bool localEndSealed{};
        std::uint32_t stagingBefore{};
        std::uint32_t stagingAfter{};
        if (localQueue && !localSeekReset) {
            // Local natural EOF resets its converter before Apple enters the
            // subsequent Dispose/rebuild path. Drain any already-captured local
            // staging remainder into ACv2 before asking the coordinator to seal;
            // otherwise a final short block can still be waiting one layer
            // upstream and appear as AvailableFrames()==0 to the terminal gate.
            stagingBefore = V2LocalQueueAvailableFrames(localQueue);
            (void)DrainV2LocalQueue(converter, localQueue);
            stagingAfter = V2LocalQueueAvailableFrames(localQueue);
            // Direct-selection arm/skip confirmation is handled inside
            // OnLocalEndOfStream; exact-period EOF simply returns false.
            localEndSealed = g_audioCoreRuntime.OnLocalEndOfStream(converter);
        }
        if (localQueue) {
            Log(L"v2 local PCM reset converter=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                L" seekReset=" + std::to_wstring(localSeekReset) +
                L" flushed=" + std::to_wstring(localSeekReset) +
                L" stagingBefore=" + std::to_wstring(stagingBefore) +
                L" stagingAfter=" + std::to_wstring(stagingAfter) +
                L" acv2Available=" + std::to_wstring(
                    g_audioCoreRuntime.Coordinator().AvailableFrames()) +
                L" eosSealed=" + std::to_wstring(localEndSealed));
        }
    }
    return result;
}

AppleOSStatus __cdecl HookV2AudioConverterDispose(void* converter) {
    const auto localQueue = FindV2LocalQueue(converter);
    const bool uiEnabled = g_audioCoreRuntime.UiEnabled();
    const bool registeredBefore = uiEnabled && g_audioCoreRuntime.IsConverterRegistered(converter);

    // Natural local EOF can synchronously rebuild the successor AudioUnit from
    // inside Apple's original AudioConverterDispose. Seal the captured tail
    // before entering Dispose so nested graph-lifecycle callbacks cannot retire
    // the old sink before its terminal frames are submitted.
    // Explicit seek/skip/direct-selection control fences make this return false
    // for non-natural retirement paths.
    if (uiEnabled && localQueue) {
        (void)DrainV2LocalQueue(converter, localQueue);
    }
    const bool localEndSealed = uiEnabled && localQueue &&
        g_audioCoreRuntime.OnLocalEndOfStream(converter);
    if (localEndSealed) {
        Log(L"v2 local PCM end-of-stream sealed before dispose converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
            L" queuedFrames=" + std::to_wstring(V2LocalQueueAvailableFrames(localQueue)));
    }

    const bool trackedP0 = g_nativeP0Puller.BeginDispose(converter);
    std::unique_lock<std::mutex> p0DisposeLock;
    if (trackedP0) p0DisposeLock = std::unique_lock<std::mutex>(g_p0FillMutex);
    const auto result = g_v2OriginalAudioConverterDispose
        ? g_v2OriginalAudioConverterDispose(converter) : -1;
    if (g_nativeTraceEnabled.load(std::memory_order_acquire)) {
        Log(L"native trace AudioConverterDispose converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
            L" result=" + std::to_wstring(result) + L" stack="+
            CaptureCurrentStackText(2, 8));
    }
    if (uiEnabled) g_audioCoreRuntime.OnConverterDisposed(converter);
    if (localQueue) {
        {
            std::lock_guard lock(g_converterProbeMutex);
            g_converterProbes.erase(converter);
        }
        localQueue->Abandon();
        void* expected = converter;
        g_v2PendingLocalConverter.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);

        void* successor = g_v2PendingLocalConverter.load(std::memory_order_acquire);
        auto successorQueue = successor ? FindV2LocalQueue(successor) : nullptr;
        if (successor && successor != converter && successorQueue &&
            V2LocalQueueAvailableFrames(successorQueue) != 0) {
            if (localEndSealed) {
                // Do not let the prefetched successor steal the endpoint until
                // the old generation's terminal buffer is actually released.
                g_audioCoreRuntime.SetPendingLocalSuccessor(successor);
                Log(L"v2 local PCM successor queued behind terminal tail old=" +
                    std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                    L" new=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(successor)));
            } else if (g_audioCoreRuntime.TryBindLocalSuccessor(successor)) {
                (void)PromoteV2LocalQueue(successor);
                Log(L"v2 local PCM successor rebound after natural dispose old=" +
                    std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
                    L" new=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(successor)));
            }
        }
    }
    if (registeredBefore) {
        Log(L"v2 AudioConverterDispose retired converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
            L" result=" + std::to_wstring(result));
    }
    return result;
}

AppleOSStatus __cdecl HookV2AudioUnitSetProperty(void* unit, std::uint32_t propertyId,
                                                  std::uint32_t scope, std::uint32_t element,
                                                  const void* data, std::uint32_t dataSize) {
    const auto result = g_v2OriginalAudioUnitSetProperty
        ? g_v2OriginalAudioUnitSetProperty(unit, propertyId, scope, element, data, dataSize)
        : -1;
    if (result != 0 || !g_audioCoreRuntime.UiEnabled() || propertyId != 8 || scope != 1 ||
        !data || dataSize < sizeof(AudioStreamBasicDescription)) {
        return result;
    }

    const auto* streamFormat = static_cast<const AudioStreamBasicDescription*>(data);
    if (!IsAppleLinearPcm(streamFormat) || !std::isfinite(streamFormat->mSampleRate) ||
        streamFormat->mSampleRate < 8000.0 || streamFormat->mSampleRate > 768000.0 ||
        streamFormat->mChannelsPerFrame != 2) {
        return result;
    }

    const auto rate = static_cast<std::uint32_t>(streamFormat->mSampleRate + 0.5);
    const auto now = GetTickCount64();
    const bool hasFreshCandidate = g_threadGraphFormat.observedTick != 0 &&
        now - g_threadGraphFormat.observedTick <= 2000 &&
        g_threadGraphFormat.sampleRate == rate &&
        g_threadGraphFormat.channels == streamFormat->mChannelsPerFrame;
    constexpr std::uint32_t qlac = 0x716C6163; // 'qlac'
    const bool qlacCandidateMatches = hasFreshCandidate &&
        g_threadGraphFormat.encodedFormat == qlac &&
        IsSupportedSourceBitDepth(g_threadGraphFormat.sourceBitDepth);
    const bool localPcmCandidateMatches = hasFreshCandidate &&
        g_threadGraphFormat.encodedFormat == ammod::audio::kLinearPcm &&
        IsSupportedSourceBitDepth(g_threadGraphFormat.sourceBitDepth);
    std::uint32_t sourceBitDepth = (qlacCandidateMatches || localPcmCandidateMatches)
        ? g_threadGraphFormat.sourceBitDepth : 0;
    std::uint64_t observationId = (qlacCandidateMatches || localPcmCandidateMatches)
        ? g_threadGraphFormat.observationId : 0;
    std::wstring binding = qlacCandidateMatches ? L"same-thread-qlac" :
        (localPcmCandidateMatches ? L"same-thread-local-lpcm" :
         (hasFreshCandidate ? L"fresh-non-qlac" : L"unknown"));
    if (!sourceBitDepth && ResolveUnambiguousQlacBitDepth(
            rate, streamFormat->mChannelsPerFrame, sourceBitDepth, observationId)) {
        binding = L"cross-thread-unambiguous";
    }
    RememberAudioUnitInputFormat(unit, *streamFormat, sourceBitDepth, observationId);
    Log(L"v2 AudioUnit input format remembered unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) + L" rate=" +
        std::to_wstring(rate) + L" sourceBitDepth=" + std::to_wstring(sourceBitDepth) +
        L" observation=" + std::to_wstring(observationId) + L" binding=" + binding +
        L" tid=" + std::to_wstring(GetCurrentThreadId()));
    return result;
}

AppleOSStatus __cdecl HookV2AudioUnitRender(void* unit, std::uint32_t* actionFlags,
                                              const void* timestamp, std::uint32_t bus,
                                              std::uint32_t frames, void* buffers) {
    // Production is observation-only and must stay effectively transparent on
    // Apple's real-time render thread. Deep PCM signatures/timestamp diagnostics
    // are opt-in through native_trace; they are never needed for playback.
    const bool deepDiagnostics = g_nativeTraceEnabled.load(std::memory_order_relaxed);
    const auto enterQpc = deepDiagnostics ? CurrentQpc() : 0;
    AppleAudioTimeStamp observed{};
    const bool hasTimestamp = timestamp != nullptr;
    if (hasTimestamp && (!kNativeAcquisitionPassthrough || deepDiagnostics)) {
        std::memcpy(&observed, timestamp, sizeof(observed));
    }
    const auto result = g_v2OriginalAudioUnitRender
        ? g_v2OriginalAudioUnitRender(unit, actionFlags, timestamp, bus, frames, buffers) : -1;
    const auto call = g_v2ObservedRenderCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (result != 0) g_v2P1RenderErrors.fetch_add(1, std::memory_order_relaxed);
    if (!deepDiagnostics) return result;

    const auto returnQpc = CurrentQpc();
    if (!hasTimestamp) g_v2P1TimestampMissingCalls.fetch_add(1, std::memory_order_relaxed);

    std::uint32_t bufferCount{};
    std::uint32_t bufferChannels{};
    std::uint32_t bufferBytes{};
    V2P1SignatureResult signature{};
    bool signatureAttempted = false;
    if (buffers) {
        const auto* list = static_cast<const AppleAudioBufferList*>(buffers);
        bufferCount = list->mNumberBuffers;
        if (bufferCount != 0 && bufferCount <= 8) {
            bufferChannels = list->mBuffers[0].mNumberChannels;
            bufferBytes = list->mBuffers[0].mDataByteSize;
        }
        if (result == 0) {
            signature = ComputeV2P1Signature(list);
            signatureAttempted = true;
        }
    } else if (result == 0) {
        g_v2P1SignatureInvalidCalls.fetch_add(1, std::memory_order_relaxed);
    }

    bool repeatedSignature = false;
    bool repeatedTimestamp = false;
    bool timestampDiscontinuity = false;
    auto* slot = FindV2P1SignatureSlot(unit, bus);
    if (slot) {
        const auto previousSequence = slot->sequence.fetch_add(1, std::memory_order_relaxed);
        const auto previousSignature = slot->lastSignature.load(std::memory_order_acquire);
        const auto previousSampleTimeBits =
            slot->lastSampleTimeBits.load(std::memory_order_acquire);
        const auto previousFrames = slot->lastFrames.load(std::memory_order_acquire);
        if (previousSequence != 0 && signatureAttempted && signature.valid &&
            previousSignature != 0 && previousSignature == signature.signature) {
            repeatedSignature = true;
            g_v2P1RepeatedSignatureCalls.fetch_add(1, std::memory_order_relaxed);
        }
        const auto sampleTimeBits = hasTimestamp ? DoubleBits(observed.mSampleTime) : 0;
        if (previousSequence != 0 && hasTimestamp && previousSampleTimeBits != 0 &&
            std::isfinite(observed.mSampleTime)) {
            const auto previousSampleTime = BitsDouble(previousSampleTimeBits);
            if (std::isfinite(previousSampleTime)) {
                repeatedTimestamp = previousSampleTimeBits == sampleTimeBits;
                if (repeatedTimestamp) {
                    g_v2P1TimestampRepeatedCalls.fetch_add(1, std::memory_order_relaxed);
                }
                if (previousFrames != 0 &&
                    std::abs((observed.mSampleTime - previousSampleTime) -
                             static_cast<double>(previousFrames)) > 0.5) {
                    timestampDiscontinuity = true;
                    g_v2P1TimestampDiscontinuities.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        slot->lastSignature.store(
            signatureAttempted && signature.valid ? signature.signature : 0,
            std::memory_order_release);
        slot->lastSampleTimeBits.store(
            hasTimestamp && std::isfinite(observed.mSampleTime) ? sampleTimeBits : 0,
            std::memory_order_release);
        slot->lastHostTime.store(hasTimestamp ? observed.mHostTime : 0,
                                 std::memory_order_release);
        slot->lastFrames.store(frames, std::memory_order_release);
    }

    if (signatureAttempted) {
        if (!signature.valid) {
            g_v2P1SignatureInvalidCalls.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_v2P1SignatureValidCalls.fetch_add(1, std::memory_order_relaxed);
            if (signature.allZero) {
                g_v2P1SignatureAllZeroCalls.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_v2P1SignatureNonZeroCalls.fetch_add(1, std::memory_order_relaxed);
            }
            g_v2P1LastSignature.store(signature.signature, std::memory_order_release);
            g_v2P1LastSignatureBytes.store(signature.hashedBytes, std::memory_order_release);
            g_v2P1LastDeclaredBytes.store(signature.declaredBytes, std::memory_order_release);
            g_v2P1LastZeroBytes.store(signature.zeroBytes, std::memory_order_release);
        }
    }
    if (hasTimestamp) {
        g_v2P1LastSampleTimeBits.store(DoubleBits(observed.mSampleTime),
                                       std::memory_order_release);
        g_v2P1LastHostTime.store(observed.mHostTime, std::memory_order_release);
        g_v2P1LastTimestampFlags.store(observed.mFlags, std::memory_order_release);
    }
    g_v2P1LastFrames.store(frames, std::memory_order_release);
    g_v2P1LastBufferCount.store(bufferCount, std::memory_order_release);
    g_v2P1LastQpc.store(returnQpc, std::memory_order_release);

    const bool sampledRepeated = repeatedSignature && (call <= 12 || call % 256 == 0);
    const bool signatureAnomaly = signatureAttempted && (!signature.valid || sampledRepeated);
    const bool sampledAllZero = signatureAttempted && signature.allZero &&
        (call <= 12 || call % 256 == 0);
    const bool logRecord = call <= 12 || result != 0 || repeatedTimestamp ||
        timestampDiscontinuity || signatureAnomaly || sampledAllZero || (call % 256 == 0);
    if (logRecord) {
        Log(L"v2 AudioUnitRender observed call=" + std::to_wstring(call) +
            L" unit=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
            L" bus=" + std::to_wstring(bus) + L" frames=" + std::to_wstring(frames) +
            L" buffers=" + std::to_wstring(bufferCount) +
            L" channels=" + std::to_wstring(bufferChannels) +
            L" bytes=" + std::to_wstring(bufferBytes) +
            L" result=" + std::to_wstring(result) +
            L" qpcEnter=" + std::to_wstring(enterQpc) +
            L" qpcReturn=" + std::to_wstring(returnQpc) +
            L" sampleTime=" + std::to_wstring(hasTimestamp ? observed.mSampleTime : 0.0) +
            L" hostTime=" + std::to_wstring(hasTimestamp ? observed.mHostTime : 0) +
            L" rateScalar=" + std::to_wstring(hasTimestamp ? observed.mRateScalar : 0.0) +
            L" timestampFlags=" + std::to_wstring(hasTimestamp ? observed.mFlags : 0) +
            L" contentSig=" + std::to_wstring(signatureAttempted && signature.valid
                ? signature.signature : 0) +
            L" signatureValid=" + std::to_wstring(signatureAttempted && signature.valid) +
            L" signatureBytes=" + std::to_wstring(signature.hashedBytes) +
            L" declaredBytes=" + std::to_wstring(signature.declaredBytes) +
            L" zeroBytes=" + std::to_wstring(signature.zeroBytes) +
            L" allZero=" + std::to_wstring(signatureAttempted && signature.allZero) +
            L" repeatedSignature=" + std::to_wstring(repeatedSignature) +
            L" repeatedTimestamp=" + std::to_wstring(repeatedTimestamp) +
            L" timestampDiscontinuity=" + std::to_wstring(timestampDiscontinuity));
    }
    return result;
}

AppleOSStatus __cdecl HookV2AudioUnitInitialize(void* unit) {
    const auto result = g_v2OriginalAudioUnitInitialize
        ? g_v2OriginalAudioUnitInitialize(unit) : -1;
    if (result == 0 && g_audioCoreRuntime.UiEnabled()) {
        AudioUnitInputFormatState remembered{};
        const bool hasRemembered = FindAudioUnitInputFormat(unit, remembered) &&
            IsAppleLinearPcm(&remembered.format) &&
            std::isfinite(remembered.format.mSampleRate) &&
            remembered.format.mSampleRate >= 8000.0 &&
            remembered.format.mSampleRate <= 768000.0 &&
            remembered.format.mChannelsPerFrame == 2;
        const auto now = GetTickCount64();
        const bool fresh = g_threadGraphFormat.converter &&
            g_threadGraphFormat.observedTick != 0 && now - g_threadGraphFormat.observedTick <= 2000 &&
            g_threadGraphFormat.sampleRate >= 8000 && g_threadGraphFormat.sampleRate <= 768000 &&
            g_threadGraphFormat.channels == 2;
        const auto rate = hasRemembered
            ? static_cast<std::uint32_t>(remembered.format.mSampleRate + 0.5)
            : (fresh ? g_threadGraphFormat.sampleRate : 0u);
        const auto channels = hasRemembered ? remembered.format.mChannelsPerFrame
                                             : (fresh ? g_threadGraphFormat.channels : 0u);
        auto sourceBitDepth = hasRemembered ? remembered.sourceBitDepth
                                            : (fresh ? g_threadGraphFormat.sourceBitDepth : 0u);
        const auto observationId = hasRemembered ? remembered.observationId
                                                 : (fresh ? g_threadGraphFormat.observationId : 0u);
        const bool sameObservedCandidate = fresh &&
            (!hasRemembered || remembered.observationId == 0 ||
             remembered.observationId == g_threadGraphFormat.observationId);
        void* candidateConverter = sameObservedCandidate ? g_threadGraphFormat.converter : nullptr;
        std::uint32_t resolvedSourceBitDepth = sourceBitDepth;
        if (!candidateConverter && rate != 0 && channels == 2) {
            const bool resolved = g_audioCoreRuntime.ResolveCandidate(
                rate, channels, resolvedSourceBitDepth, observationId, candidateConverter);
            Log(L"v2 graph candidate resolution rate=" + std::to_wstring(rate) +
                L" channels=" + std::to_wstring(channels) + L" sourceBitDepth=" +
                std::to_wstring(resolvedSourceBitDepth) + L" observation=" +
                std::to_wstring(observationId) + L" resolved=" + std::to_wstring(resolved) +
                L" converter=" + std::to_wstring(
                    reinterpret_cast<std::uintptr_t>(candidateConverter)));
            sourceBitDepth = resolvedSourceBitDepth;
        }
        Log(L"v2 AudioUnitInitialize unit=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) + L" rate=" +
            std::to_wstring(rate) + L" sourceBitDepth=" +
            std::to_wstring(sourceBitDepth) + L" observation=" +
            std::to_wstring(observationId) + L" crossThread=" +
            std::to_wstring(hasRemembered && !sameObservedCandidate) + L" qpc=" +
            std::to_wstring(CurrentQpc()));
        g_audioCoreRuntime.OnAudioUnitInitialized(
            unit, candidateConverter, rate, channels, sourceBitDepth, observationId);
    }
    return result;
}

AppleOSStatus __cdecl HookV2AudioOutputUnitStart(void* unit) {
    const auto result = g_v2OriginalAudioOutputUnitStart
        ? g_v2OriginalAudioOutputUnitStart(unit) : -1;
    Log(L"v2 AudioOutputUnitStart unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
        L" result=" + std::to_wstring(result) + L" qpc=" +
        std::to_wstring(CurrentQpc()));
    if (result == 0) {
        // Local-file graphs can create the LPCM->LPCM converter and immediately
        // start the output unit without a separately observed AudioUnitInitialize
        // edge.  In that case the same setup thread still carries the fresh
        // Local-P0 converter identity; synthesize only the runtime graph-bind
        // notification (never an Apple API call) before recording Start.
        const auto now = GetTickCount64();
        const bool freshLocalGraph = g_threadGraphFormat.converter &&
            g_threadGraphFormat.encodedFormat == ammod::audio_v2::kAppleLinearPcm &&
            g_threadGraphFormat.observedTick != 0 &&
            now >= g_threadGraphFormat.observedTick &&
            now - g_threadGraphFormat.observedTick <= 500 &&
            g_audioCoreRuntime.IsConverterRegistered(g_threadGraphFormat.converter);
        if (freshLocalGraph) {
            g_audioCoreRuntime.OnAudioUnitInitialized(
                unit, g_threadGraphFormat.converter, g_threadGraphFormat.sampleRate,
                g_threadGraphFormat.channels, g_threadGraphFormat.sourceBitDepth,
                g_threadGraphFormat.observationId);
            Log(L"v2 local PCM graph bound from fresh AudioOutputUnitStart converter=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(g_threadGraphFormat.converter)) +
                L" rate=" + std::to_wstring(g_threadGraphFormat.sampleRate) +
                L" bits=" + std::to_wstring(g_threadGraphFormat.sourceBitDepth));
        }
        g_audioCoreRuntime.OnAudioUnitStarted(unit);
        if (void* bound = g_audioCoreRuntime.BoundConverter()) {
            (void)PromoteV2LocalQueue(bound);
        }
    }
    return result;
}

AppleOSStatus __cdecl HookV2AudioOutputUnitStop(void* unit) {
    const auto result = g_v2OriginalAudioOutputUnitStop
        ? g_v2OriginalAudioOutputUnitStop(unit) : -1;
    Log(L"v2 AudioOutputUnitStop unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
        L" result=" + std::to_wstring(result) + L" qpc=" +
        std::to_wstring(CurrentQpc()));
    g_audioCoreRuntime.OnAudioUnitStopped(unit);
    return result;
}

AppleOSStatus __cdecl HookV2AudioUnitUninitialize(void* unit) {
    const auto result = g_v2OriginalAudioUnitUninitialize
        ? g_v2OriginalAudioUnitUninitialize(unit) : -1;
    Log(L"v2 AudioUnitUninitialize unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
        L" result=" + std::to_wstring(result) + L" qpc=" +
        std::to_wstring(CurrentQpc()));
    g_audioCoreRuntime.OnAudioUnitUninitialized(unit);
    RetireAudioUnitEpoch(unit);
    return result;
}

const wchar_t* WasapiSinkLifecycleText(
    ammod::audio_v2::WasapiSinkLifecycleEvent event) noexcept {
    using Event = ammod::audio_v2::WasapiSinkLifecycleEvent;
    switch (event) {
    case Event::OpenEnter: return L"OpenEnter";
    case Event::OpenExit: return L"OpenExit";
    case Event::InitializeEnter: return L"InitializeEnter";
    case Event::InitializeExit: return L"InitializeExit";
    case Event::StartEnter: return L"StartEnter";
    case Event::StartExit: return L"StartExit";
    case Event::StopEnter: return L"StopEnter";
    case Event::StopExit: return L"StopExit";
    case Event::CloseEnter: return L"CloseEnter";
    case Event::CloseExit: return L"CloseExit";
    }
    return L"Unknown";
}

void V2OnSinkLifecycle(void*, ammod::audio_v2::WasapiSinkLifecycleEvent event,
                       HRESULT result) noexcept {
    Log(L"v2 sink lifecycle event=" + std::wstring(WasapiSinkLifecycleText(event)) +
        L" qpc=" + std::to_wstring(CurrentQpc()) + L" result=" + HResultText(result));
}

void V2OnNativeInitialize(void* context, void* client, const wchar_t* endpoint,
                          AUDCLNT_SHAREMODE shareMode, DWORD flags,
                          const WAVEFORMATEX* format, HRESULT result) noexcept {
    Log(L"v2 native client Initialize client=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
        L" shareMode=" + std::to_wstring(static_cast<unsigned>(shareMode)) +
        L" flags=0x" + HResultText(static_cast<HRESULT>(flags)).substr(2) +
        L" result=" + HResultText(result) + L" qpc=" + std::to_wstring(CurrentQpc()) +
        L" endpoint=" + (endpoint ? std::wstring(endpoint) : std::wstring()) +
        L" format{" + FormatText(format) + L"}");
    static_cast<ammod::audio_v2::AudioCoreRuntime*>(context)->OnNativeClientInitialized(
        client, endpoint, shareMode, flags, format, result);
}

void V2OnNativeStart(void* context, void* client, HRESULT result) noexcept {
    Log(L"v2 native client Start client=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
        L" result=" + HResultText(result) + L" qpc=" + std::to_wstring(CurrentQpc()));
    static_cast<ammod::audio_v2::AudioCoreRuntime*>(context)->OnNativeClientStarted(client, result);
}

bool V2BeforeNativeStop(void* context, void* client) noexcept {
    const auto enter = CurrentQpc();
    const bool eofReady = static_cast<ammod::audio_v2::AudioCoreRuntime*>(context)
        ->BeforeNativeClientStop(client);
    Log(L"v2 native client pre-stop client=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
        L" eofReady=" + std::to_wstring(eofReady) +
        L" enterQpc=" + std::to_wstring(enter) +
        L" returnQpc=" + std::to_wstring(CurrentQpc()));
    return eofReady;
}

void V2OnNativeStop(void* context, void* client, HRESULT result) noexcept {
    Log(L"v2 native client Stop client=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
        L" result=" + HResultText(result) + L" qpc=" + std::to_wstring(CurrentQpc()));
    auto* runtime = static_cast<ammod::audio_v2::AudioCoreRuntime*>(context);
    void* streamingConverter = runtime->OnNativeClientStopped(client, result);
    if (streamingConverter) {
        Log(L"v2 streaming native-stop awaiting producer EOF converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(streamingConverter)));
    }
}

void V2OnNativeReset(void* context, void* client, HRESULT result) noexcept {
    Log(L"v2 native client Reset client=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
        L" result=" + HResultText(result) + L" qpc=" + std::to_wstring(CurrentQpc()));
    static_cast<ammod::audio_v2::AudioCoreRuntime*>(context)->OnNativeClientReset(client, result);
}

void V2OnNativeGetBuffer(void*, void* client, std::uint32_t frames,
                         HRESULT result, bool shadow) noexcept {
    const auto call = g_v2NativeGetBufferCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool trace = g_nativeTraceEnabled.load(std::memory_order_relaxed);
    if (FAILED(result) || (!shadow && call <= 4) ||
        (trace && (call <= 12 || call % 256u == 0))) {
        Log(L"v2 native GetBuffer client=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
            L" frames=" + std::to_wstring(frames) + L" result=" +
            HResultText(result) + L" shadow=" + std::to_wstring(shadow) + L" qpc=" +
            std::to_wstring(CurrentQpc()));
    }
}

void V2OnNativeReleaseBuffer(void*, void* client, std::uint32_t frames,
                             HRESULT result, bool shadow) noexcept {
    const auto call = g_v2NativeReleaseBufferCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool trace = g_nativeTraceEnabled.load(std::memory_order_relaxed);
    if (FAILED(result) || (!shadow && call <= 4) ||
        (trace && (call <= 12 || call % 256u == 0))) {
        Log(L"v2 native ReleaseBuffer client=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
            L" frames=" + std::to_wstring(frames) + L" result=" +
            HResultText(result) + L" shadow=" + std::to_wstring(shadow) + L" qpc=" +
            std::to_wstring(CurrentQpc()));
    }
}

void V2OnNativeMethod(void*, void* client, const wchar_t* method, HRESULT result) noexcept {
    // GetCurrentPadding is called at render cadence. Successful proxy-method
    // tracing is therefore opt-in; failures remain visible in production.
    if (SUCCEEDED(result) && !g_nativeTraceEnabled.load(std::memory_order_relaxed)) return;
    Log(L"v2 native client method=" + (method ? std::wstring(method) : std::wstring()) +
        L" client=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
        L" result=" + HResultText(result) + L" qpc=" +
        std::to_wstring(CurrentQpc()));
}

void V2OnNativeService(void*, void* client, REFIID serviceIid, HRESULT result) noexcept {
    Log(L"v2 native client service client=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
        L" iid=" + GuidText(serviceIid) + L" result=" + HResultText(result) +
        L" qpc=" + std::to_wstring(CurrentQpc()));
}

std::uint32_t V2NativePumpPeriodMs(void* context) noexcept {
    auto* runtime = static_cast<ammod::audio_v2::AudioCoreRuntime*>(context);
    return runtime ? runtime->NativePumpPeriodMs() : 10u;
}

bool V2ShouldSuppress(void* context, void* client) noexcept {
    return static_cast<ammod::audio_v2::AudioCoreRuntime*>(context)->ShouldSuppressNative(client);
}

HRESULT V2PrepareNativeHandoff(void*, void* client) noexcept {
    auto* proxy = static_cast<ammod::audio_v2::NativeAudioClientProxy*>(client);
    const auto result = proxy ? proxy->PrepareForExclusiveHandoff()
                              : AUDCLNT_E_NOT_INITIALIZED;
    Log(L"v2 native client prepare exclusive handoff client=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
        L" result=" + HResultText(result) + L" qpc=" +
        std::to_wstring(CurrentQpc()));
    return result;
}

void V2FinalizeNativeHandoff(void*, void* client) noexcept {
    auto* proxy = static_cast<ammod::audio_v2::NativeAudioClientProxy*>(client);
    if (proxy) proxy->FinalizeExclusiveHandoff();
    Log(L"v2 native client finalized exclusive handoff client=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
        L" qpc=" + std::to_wstring(CurrentQpc()));
}

HRESULT V2ResumeNativeAfterFailedHandoff(void*, void* client) noexcept {
    auto* proxy = static_cast<ammod::audio_v2::NativeAudioClientProxy*>(client);
    const auto result = proxy ? proxy->Start() : AUDCLNT_E_NOT_INITIALIZED;
    Log(L"v2 native client resume after failed handoff client=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(client)) +
        L" result=" + HResultText(result) + L" qpc=" +
        std::to_wstring(CurrentQpc()));
    return result;
}

HRESULT V2PrepareNativeGraphHandoff(void*, void* unit) noexcept {
    if (!unit) return E_INVALIDARG;
    if (kNativeAcquisitionPassthrough) {
        Log(L"v2 native graph prepare exclusive handoff unit=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
            L" stop=not-requested uninitialize=not-requested native-acquisition=passthrough independent-decoder=disabled qpc=" +
            std::to_wstring(CurrentQpc()));
        return S_OK;
    }
    std::unique_lock p0FillLock(g_p0FillMutex);
    if (!g_nativeP0Puller.PrepareIndependentDecoder()) {
        Log(L"v2 native graph prepare exclusive handoff independent P0 decoder unavailable");
        return E_FAIL;
    }
    Log(L"v2 native graph prepare exclusive handoff unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
        L" stop=not-requested uninitialize=not-requested independent-decoder=ready qpc=" +
        std::to_wstring(CurrentQpc()));
    return S_OK;
}

HRESULT V2ResumeNativeGraphAfterFailedHandoff(void*, void* unit) noexcept {
    if (!unit) return E_INVALIDARG;
    // The successful pump handoff leaves the parent running. This path is only
    // for a failed v2 Open/Start, so resume the original render clock directly.
    const auto result = g_v2OriginalAudioOutputUnitStart
        ? static_cast<HRESULT>(g_v2OriginalAudioOutputUnitStart(unit))
        : E_NOTIMPL;
    Log(L"v2 native graph resume after failed handoff unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
        L" initialize=not-requested start=" + HResultText(result) + L" qpc=" +
        std::to_wstring(CurrentQpc()));
    return result;
}

ammod::audio_v2::NativeAudioCallbacks V2NativeCallbacks() noexcept {
    return {&g_audioCoreRuntime, &V2OnNativeInitialize, &V2OnNativeStart,
            &V2BeforeNativeStop, &V2OnNativeStop, &V2OnNativeReset, &V2ShouldSuppress,
            &V2OnNativeGetBuffer, &V2OnNativeReleaseBuffer, &V2OnNativeMethod,
            &V2OnNativeService, &V2NativePumpPeriodMs};
}

void HookV2Device(IMMDevice* device);

HRESULT STDMETHODCALLTYPE HookV2DeviceActivate(IMMDevice* self, REFIID iid, DWORD context,
                                               PROPVARIANT* params, void** object) {
    const auto result = g_v2OriginalDeviceActivate
        ? g_v2OriginalDeviceActivate(self, iid, context, params, object) : E_UNEXPECTED;
    if (FAILED(result) || !object || !*object || ammod::audio_v2::NativeProxyBypassed() ||
        !g_audioCoreRuntime.UiEnabled() ||
        (iid != __uuidof(IAudioClient) && iid != __uuidof(IAudioClient2) &&
         iid != __uuidof(IAudioClient3))) {
        return result;
    }

    auto* returned = reinterpret_cast<IUnknown*>(*object);
    IAudioClient* client{};
    const auto query = returned->QueryInterface(
        __uuidof(IAudioClient), reinterpret_cast<void**>(&client));
    if (FAILED(query) || !client) return result;
    auto* proxy = new (std::nothrow) ammod::audio_v2::NativeAudioClientProxy(
        client, V2NativeCallbacks(), DeviceId(self));
    if (!proxy) {
        client->Release();
        return result;
    }
    void* replacement{};
    const auto wrapped = proxy->QueryInterface(iid, &replacement);
    proxy->Release();
    if (FAILED(wrapped) || !replacement) return result;
    returned->Release();
    *object = replacement;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE HookV2CollectionItem(IMMDeviceCollection* self, UINT index,
                                                IMMDevice** device) {
    const auto result = g_v2OriginalCollectionItem
        ? g_v2OriginalCollectionItem(self, index, device) : E_UNEXPECTED;
    if (SUCCEEDED(result) && device && *device) HookV2Device(*device);
    return result;
}

HRESULT STDMETHODCALLTYPE HookV2EnumAudioEndpoints(IMMDeviceEnumerator* self, EDataFlow flow,
                                                   DWORD state,
                                                   IMMDeviceCollection** collection) {
    const auto result = g_v2OriginalEnumAudioEndpoints
        ? g_v2OriginalEnumAudioEndpoints(self, flow, state, collection) : E_UNEXPECTED;
    if (SUCCEEDED(result) && collection && *collection) {
        auto** table = Vtable(*collection);
        if (table) HookAddress(table[4], reinterpret_cast<void*>(&HookV2CollectionItem),
                               g_v2OriginalCollectionItem, L"IMMDeviceCollection::Item [v2]");
    }
    return result;
}

HRESULT STDMETHODCALLTYPE HookV2GetDefaultEndpoint(IMMDeviceEnumerator* self, EDataFlow flow,
                                                    ERole role, IMMDevice** device) {
    const auto result = g_v2OriginalGetDefaultEndpoint
        ? g_v2OriginalGetDefaultEndpoint(self, flow, role, device) : E_UNEXPECTED;
    if (SUCCEEDED(result) && device && *device) HookV2Device(*device);
    return result;
}

HRESULT STDMETHODCALLTYPE HookV2GetDevice(IMMDeviceEnumerator* self, LPCWSTR id,
                                          IMMDevice** device) {
    const auto result = g_v2OriginalGetDevice
        ? g_v2OriginalGetDevice(self, id, device) : E_UNEXPECTED;
    if (SUCCEEDED(result) && device && *device) HookV2Device(*device);
    return result;
}

void HookV2Device(IMMDevice* device) {
    auto** table = Vtable(device);
    if (table) HookAddress(table[3], reinterpret_cast<void*>(&HookV2DeviceActivate),
                           g_v2OriginalDeviceActivate, L"IMMDevice::Activate [v2]");
}

void HookV2Enumerator(IMMDeviceEnumerator* enumerator) {
    auto** table = Vtable(enumerator);
    if (!table) return;
    HookAddress(table[3], reinterpret_cast<void*>(&HookV2EnumAudioEndpoints),
                g_v2OriginalEnumAudioEndpoints, L"IMMDeviceEnumerator::EnumAudioEndpoints [v2]");
    HookAddress(table[4], reinterpret_cast<void*>(&HookV2GetDefaultEndpoint),
                g_v2OriginalGetDefaultEndpoint, L"IMMDeviceEnumerator::GetDefaultAudioEndpoint [v2]");
    HookAddress(table[5], reinterpret_cast<void*>(&HookV2GetDevice),
                g_v2OriginalGetDevice, L"IMMDeviceEnumerator::GetDevice [v2]");
}

HRESULT WINAPI HookV2CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context,
                                      REFIID iid, LPVOID* object) {
    const auto result = g_v2OriginalCoCreateInstance
        ? g_v2OriginalCoCreateInstance(clsid, outer, context, iid, object) : E_UNEXPECTED;
    if (SUCCEEDED(result) && object && *object && iid == __uuidof(IMMDeviceEnumerator)) {
        HookV2Enumerator(static_cast<IMMDeviceEnumerator*>(*object));
    }
    return result;
}

bool InstallAudioCoreV2Hooks() {
    if (g_audioCoreV2HooksInstalled.load(std::memory_order_acquire)) return true;
    HMODULE toolbox = GetModuleHandleW(L"CoreAudioToolbox.dll");
    if (!toolbox) return false;
    auto installToolbox = [toolbox](const char* name, void* detour, auto& original,
                                    const wchar_t* label) {
        auto* target = GetProcAddress(toolbox, name);
        return target && HookAddress(reinterpret_cast<void*>(target), detour, original, label);
    };
    const bool converterNew = installToolbox(
        "AudioConverterNew", reinterpret_cast<void*>(&HookV2AudioConverterNew),
        g_v2OriginalAudioConverterNew, L"AudioConverterNew [v2]");
    const bool converterSpecific = installToolbox(
        "AudioConverterNewSpecific", reinterpret_cast<void*>(&HookV2AudioConverterNewSpecific),
        g_v2OriginalAudioConverterNewSpecific, L"AudioConverterNewSpecific [v2]");
    const bool converterProperty = installToolbox(
        "AudioConverterSetProperty", reinterpret_cast<void*>(&HookV2AudioConverterSetProperty),
        g_v2OriginalAudioConverterSetProperty, L"AudioConverterSetProperty [v2]");
    const bool converterGetProperty = installToolbox(
        "AudioConverterGetProperty", reinterpret_cast<void*>(&HookV2AudioConverterGetProperty),
        g_v2OriginalAudioConverterGetProperty, L"AudioConverterGetProperty [v2]");
    const bool converterFill = installToolbox(
        "AudioConverterFillComplexBuffer", reinterpret_cast<void*>(&HookV2AudioConverterFillComplexBuffer),
        g_v2OriginalAudioConverterFillComplexBuffer, L"AudioConverterFillComplexBuffer [v2]");
    const bool converterReset = installToolbox(
        "AudioConverterReset", reinterpret_cast<void*>(&HookV2AudioConverterReset),
        g_v2OriginalAudioConverterReset, L"AudioConverterReset [v2]");
    const bool converterDispose = installToolbox(
        "AudioConverterDispose", reinterpret_cast<void*>(&HookV2AudioConverterDispose),
        g_v2OriginalAudioConverterDispose, L"AudioConverterDispose [v2]");

    const bool unitSetProperty = installToolbox(
        "AudioUnitSetProperty", reinterpret_cast<void*>(&HookV2AudioUnitSetProperty),
        g_v2OriginalAudioUnitSetProperty, L"AudioUnitSetProperty [v2 graph identity]");
    const bool unitRender = installToolbox(
        "AudioUnitRender", reinterpret_cast<void*>(&HookV2AudioUnitRender),
        g_v2OriginalAudioUnitRender, L"AudioUnitRender [v2 observer]");
    const bool unitStart = installToolbox(
        "AudioOutputUnitStart", reinterpret_cast<void*>(&HookV2AudioOutputUnitStart),
        g_v2OriginalAudioOutputUnitStart, L"AudioOutputUnitStart [v2]");
    const bool unitStop = installToolbox(
        "AudioOutputUnitStop", reinterpret_cast<void*>(&HookV2AudioOutputUnitStop),
        g_v2OriginalAudioOutputUnitStop, L"AudioOutputUnitStop [v2]");
    const bool unitInitialize = installToolbox(
        "AudioUnitInitialize", reinterpret_cast<void*>(&HookV2AudioUnitInitialize),
        g_v2OriginalAudioUnitInitialize, L"AudioUnitInitialize [v2]");
    const bool unitUninitialize = installToolbox(
        "AudioUnitUninitialize", reinterpret_cast<void*>(&HookV2AudioUnitUninitialize),
        g_v2OriginalAudioUnitUninitialize, L"AudioUnitUninitialize [v2]");

    auto* ole32 = GetModuleHandleW(L"ole32.dll");
    void* coCreateTarget = ole32
        ? reinterpret_cast<void*>(GetProcAddress(ole32, "CoCreateInstance")) : nullptr;
    const bool coCreate = coCreateTarget && HookAddress(
        coCreateTarget,
        reinterpret_cast<void*>(&HookV2CoCreateInstance), g_v2OriginalCoCreateInstance,
        L"CoCreateInstance [v2 audio-client gate]");
    const bool ready = converterNew && converterSpecific && converterProperty &&
                       converterGetProperty && converterFill &&
                       converterReset && converterDispose && unitSetProperty && unitRender && unitStart &&
                       unitStop && unitInitialize && unitUninitialize && coCreate;
    if (ready) {
        g_audioCoreV2HooksInstalled.store(true, std::memory_order_release);
        Log(L"audio core v2 production hooks ready: native P0 callback handoff + Mod-owned Fill + native render gate");
    }
    return ready;
}

DWORD WINAPI AudioCoreV2HookLoop(void*) {
    while (!InstallAudioCoreV2Hooks()) Sleep(250);
    return 0;
}

void DisableExclusiveIntent(const std::wstring& reason, HRESULT hr,
                            const std::wstring& endpoint = {},
                            const WAVEFORMATEX* format = nullptr,
                            ammod::ipc::ErrorCategory category = ammod::ipc::ErrorCategory::None) {
    // Retire only the current attempt. The Broker remains the owner of the
    // persisted exclusive preference and re-arms this Agent for the next song.
    g_ipcEnabled.store(false);
    Log(L"exclusive attempt retired; intent remains armed reason=" + reason +
        L" result=" + HResultText(hr));
    if (category == ammod::ipc::ErrorCategory::None) category = ammod::ipc::CategorizeAudioError(hr);
    SendAgentEvent(ammod::ipc::MessageType::AttemptFailed,
                   ammod::ipc::RuntimeState::WaitingForStream,
                   true, endpoint, format, hr, category, reason);
}

DWORD WINAPI PipeClientLoop(void*) {
    using namespace ammod::ipc;
    const auto name = PipeName();
    if (name.empty()) return 1;
    for (;;) {
        if (!WaitNamedPipeW(name.c_str(), 2000)) {
            Sleep(1000);
            continue;
        }
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
            CloseHandle(pipe);
            Sleep(1000);
            continue;
        }
        {
            std::lock_guard lock(g_pipeMutex);
            g_pipe = pipe;
            g_ipcConnected.store(true);
            g_ipcEnabled.store(false, std::memory_order_release);
            g_audioCoreGate.SetUiEnabled(false);
            g_nativeP0Puller.DisableForUiOff();
            g_audioCoreRuntime.SetUiEnabled(false);
            auto hello = NewMessage(MessageType::Hello, Role::Agent);
            hello.agentPid = GetCurrentProcessId();
            if (!WriteMessage(pipe, hello)) g_ipcConnected.store(false);
        }
        Log(L"broker IPC connected");

        Message incoming{};
        while (g_ipcConnected.load()) {
            Message outbound{};
            bool hasOutbound{};
            {
                std::lock_guard lock(g_outboundMutex);
                if (!g_outbound.empty()) {
                    outbound = g_outbound.front();
                    g_outbound.pop_front();
                    hasOutbound = true;
                }
            }
            if (hasOutbound && !WriteMessage(pipe, outbound)) {
                std::lock_guard lock(g_outboundMutex);
                g_outbound.push_front(outbound);
                g_ipcConnected.store(false);
                break;
            }
            if (!ReadMessageTimeout(pipe, incoming, 50)) {
                if (GetLastError() == ERROR_TIMEOUT) continue;
                break;
            }
            if (incoming.type == MessageType::StatusChanged) {
                const bool enabled = incoming.enabledIntent != 0;
                const auto hardwareBufferMs = incoming.hardwareBufferMs
                    ? incoming.hardwareBufferMs : 20u;
                g_audioCoreRuntime.SetHardwareBufferMilliseconds(hardwareBufferMs);
                g_ipcEnabled.store(enabled, std::memory_order_release);
                if (!enabled) g_nativeP0Puller.DisableForUiOff();
                g_audioCoreGate.SetUiEnabled(enabled);
                g_audioCoreRuntime.SetUiEnabled(enabled);
                Log(L"broker status received enabled=" +
                    std::to_wstring(incoming.enabledIntent) + L" state=" +
                    std::to_wstring(static_cast<unsigned>(incoming.state)) +
                    L" hardwareBufferMs=" + std::to_wstring(hardwareBufferMs));
            } else if (incoming.type == MessageType::TransportIntent) {
                if (wcscmp(incoming.detail, L"play_pause") == 0) {
                    g_audioCoreRuntime.OnTransportPlayPause();
                    Log(L"transport intent play_pause");
                } else if (wcscmp(incoming.detail, L"selection_arm") == 0) {
                    g_audioCoreRuntime.OnTransportSelectionArm(incoming.generation);
                    Log(L"transport intent selection_arm epoch=" +
                        std::to_wstring(incoming.generation));
                } else if (wcscmp(incoming.detail, L"skip_arm") == 0) {
                    g_audioCoreRuntime.OnTransportSkipArm(incoming.generation);
                    Log(L"transport intent skip_arm epoch=" +
                        std::to_wstring(incoming.generation));
                } else if (wcscmp(incoming.detail, L"skip") == 0) {
                    g_audioCoreRuntime.OnTransportSkip(incoming.generation);
                    Log(L"transport intent skip epoch=" +
                        std::to_wstring(incoming.generation));
                } else if (wcscmp(incoming.detail, L"seek_begin") == 0) {
                    g_audioCoreRuntime.OnTransportSeekBegin(incoming.generation);
                    Log(L"transport intent seek_begin epoch=" +
                        std::to_wstring(incoming.generation));
                } else if (wcscmp(incoming.detail, L"seek_commit") == 0) {
                    g_audioCoreRuntime.OnTransportSeekCommit(incoming.generation);
                    Log(L"transport intent seek_commit epoch=" +
                        std::to_wstring(incoming.generation));
                }
            } else if (incoming.type == MessageType::Goodbye) {
                break;
            }
        }
        {
            std::lock_guard lock(g_pipeMutex);
            g_ipcConnected.store(false);
            g_ipcEnabled.store(false);
            g_audioCoreGate.SetUiEnabled(false);
            g_nativeP0Puller.DisableForUiOff();
            g_audioCoreRuntime.SetUiEnabled(false);
            if (g_pipe == pipe) g_pipe = INVALID_HANDLE_VALUE;
        }
        CloseHandle(pipe);
        Log(L"broker IPC disconnected; local runtime retired while broker-owned intent is preserved");
        Sleep(1000);
    }
}

DWORD WINAPI InitializeHooks(void*) {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(g_module, modulePath, static_cast<DWORD>(std::size(modulePath)));
    const auto directory = std::filesystem::path(modulePath).parent_path();
    g_iniPath = directory / L"am-exclusive.ini";
    g_logPath = directory / L"am-exclusive.log";
    LogEnvironmentSnapshot();
    g_nativeTraceEnabled.store(ReadBool(L"native_trace", false), std::memory_order_release);
    ammod::quality::SetEnabled(false);
    g_audioCoreGate.SetUiEnabled(false);
    // Current production keeps Apple's acquisition scheduler alive on the
    // software render pump while ACv2 owns only the P0 mirror and physical
    // exclusive endpoint. The runtime must therefore preserve native/graph
    // started bookkeeping across the endpoint handoff.
    const ammod::audio_v2::AudioCoreRuntimeCallbacks runtimeCallbacks{
        &g_audioCoreRuntime, &AudioCoreRuntimeEventCallback, &V2OnSinkLifecycle,
        &V2PrepareNativeHandoff, &V2FinalizeNativeHandoff,
        &V2ResumeNativeAfterFailedHandoff, &V2PrepareNativeGraphHandoff,
        &V2ResumeNativeGraphAfterFailedHandoff, kNativeAcquisitionPassthrough,
        g_nativeTraceEnabled.load(std::memory_order_acquire)};
    if (!g_audioCoreRuntime.Start(runtimeCallbacks)) {
        Log(L"audio core v2 runtime worker failed to start; core remains fail-closed");
    }
    if (!kNativeAcquisitionPassthrough) {
        if (!g_nativeP0Puller.Start()) {
            Log(L"audio core v2 Mod-owned P0 pull worker failed to start; native line remains fail-closed");
        }
    } else {
        Log(L"audio core v2 Mod-owned P0 pull worker intentionally disabled; native acquisition passthrough enabled");
    }
    g_audioCoreRuntime.SetUiEnabled(false);
    Log(L"hook DLL loaded; audio core v2 runtime staged with native acquisition passthrough A/B; quality lock installs independently of UI intent");
    Log(L"native acquisition trace=" +
        std::wstring(g_nativeTraceEnabled.load(std::memory_order_acquire) ? L"enabled" : L"disabled"));
    if (MH_Initialize() != MH_OK) {
        Log(L"MH_Initialize failed");
        return 1;
    }
    if (HANDLE qualityThread = CreateThread(nullptr, 0, QualityLockHookLoop, nullptr, 0, nullptr)) {
        CloseHandle(qualityThread);
    } else {
        Log(L"failed to start quality-lock hook thread");
    }
    if (HANDLE audioCoreThread = CreateThread(nullptr, 0, AudioCoreV2HookLoop, nullptr, 0, nullptr)) {
        CloseHandle(audioCoreThread);
    } else {
        Log(L"failed to start audio core v2 hook thread");
    }
    if (HANDLE pipeThread = CreateThread(nullptr, 0, PipeClientLoop, nullptr, 0, nullptr)) {
        CloseHandle(pipeThread);
    } else {
        Log(L"failed to start broker IPC thread");
    }
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, InitializeHooks, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
