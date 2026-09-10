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
#include "SampleConversion.h"
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
using WaitForSingleObjectFn = DWORD(WINAPI*)(HANDLE, DWORD);

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
struct AppleAudioComponentDescription {
    std::uint32_t componentType;
    std::uint32_t componentSubType;
    std::uint32_t componentManufacturer;
    std::uint32_t componentFlags;
    std::uint32_t componentFlagsMask;
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
using AudioUnitGetPropertyFn = AppleOSStatus(__cdecl*)(
    void*, std::uint32_t, std::uint32_t, std::uint32_t, void*, std::uint32_t*);
using AudioOutputUnitStartFn = AppleOSStatus(__cdecl*)(void*);
using AudioOutputUnitStopFn = AppleOSStatus(__cdecl*)(void*);
using AudioUnitInitializeFn = AppleOSStatus(__cdecl*)(void*);
using AudioUnitUninitializeFn = AppleOSStatus(__cdecl*)(void*);
using AudioUnitRenderFn = AppleOSStatus(__cdecl*)(
    void*, std::uint32_t*, const void*, std::uint32_t, std::uint32_t, void*);
using AudioUnitAddRenderNotifyFn = AppleOSStatus(__cdecl*)(void*, void*, void*);
using AudioComponentInstanceNewFn = AppleOSStatus(__cdecl*)(void*, void**);
using AudioComponentInstanceDisposeFn = AppleOSStatus(__cdecl*)(void*);
using AudioComponentGetDescriptionFn = AppleOSStatus(__cdecl*)(
    void*, AppleAudioComponentDescription*);
using CoreMediaInternalSetCurrentTimeFn = AppleOSStatus(__cdecl*)(
    void*, const AppleCMTime*, std::uint32_t);
using AVCFSetAudioTapProcessorFn = void(__cdecl*)(void*, void*);
using AVCFSetPreferredMaximumSampleRateFn = void(__cdecl*)(void*, double);
using AVCFSetVariantPreferencesFn = void(__cdecl*)(void*, std::uint32_t);
using AVCFSetExclusiveOptimizationFn = void(__cdecl*)(void*, unsigned char);
using AVCFGetExclusiveOptimizationFn = unsigned char(__cdecl*)(void*);
using AVCFAssetTrackCopyFormatDescriptionsFn = void*(__cdecl*)(void*);
using FigAlternateLosslessPreferenceFilterCreateFn = AppleOSStatus(__cdecl*)(
    void*, unsigned char, void*);
using FigAlternateEligibleLosslessFilterCreateFn = AppleOSStatus(__cdecl*)(
    void*, void*, void*);
using FigAlternateLossyPreferenceFilterCreateFn = AppleOSStatus(__cdecl*)(void*, void*);
using FigAlternateHasLosslessAudioFn = unsigned char(__cdecl*)(void*);
using FigAlternateAllowableMediaSubtypeFilterCreateFn = AppleOSStatus(__cdecl*)(
    void*, void*, void*, void**);
// CoreMedia 1.1540.23042.0 x64 ABI. The tenth argument is copied into the
// playback-bitrate monitor's startsOnFirstEligibleVariant byte at the field
// inventoried by ApplePrivateOffsets.h.
using FigAlternatePlaybackBitrateMonitorCreateFn = AppleOSStatus(__cdecl*)(
    void*, void*, void*, void*, void*, void*, void*, void*, void*, unsigned char, void**);
using FigAlternateFilterTreeSetFallbackBranchFn = AppleOSStatus(__cdecl*)(void*, void*);
using CFGetTypeIDFn = std::uintptr_t(__cdecl*)(const void*);
using CFArrayGetCountFn = std::int64_t(__cdecl*)(const void*);
using CFArrayGetValueAtIndexFn = const void*(__cdecl*)(const void*, std::int64_t);
using CFDictionaryGetValueFn = const void*(__cdecl*)(const void*, const void*);
using CFNumberGetValueFn = unsigned char(__cdecl*)(const void*, std::int32_t, void*);
using CFTypeIdFn = std::uintptr_t(__cdecl*)();
using CFNumberCreateFn = void*(__cdecl*)(void*, std::int32_t, const void*);
using CFArrayCreateFn = void*(__cdecl*)(void*, const void**, std::int64_t, const void*);
using CFReleaseFn = void(__cdecl*)(const void*);
using CFStringCreateWithCStringFn = void*(__cdecl*)(void*, const char*, std::uint32_t);
using CFPreferencesGetAppIntegerValueFn = std::int64_t(__cdecl*)(
    const void*, const void*, unsigned char*);
using CFPreferencesGetAppBooleanValueFn = unsigned char(__cdecl*)(
    const void*, const void*, unsigned char*);

CoCreateInstanceFn g_originalCoCreateInstance{};
EnumAudioEndpointsFn g_originalEnumAudioEndpoints{};
GetDefaultEndpointFn g_originalGetDefaultEndpoint{};
GetDeviceFn g_originalGetDevice{};
CollectionItemFn g_originalCollectionItem{};
DeviceActivateFn g_originalDeviceActivate{};
WaitForSingleObjectFn g_originalWaitForSingleObject{};
AudioConverterNewFn g_originalAudioConverterNew{};
AudioConverterNewSpecificFn g_originalAudioConverterNewSpecific{};
AudioConverterSetPropertyFn g_originalAudioConverterSetProperty{};
AudioConverterGetPropertyFn g_originalAudioConverterGetProperty{};
AudioConverterFillComplexBufferFn g_originalAudioConverterFillComplexBuffer{};
AudioConverterResetFn g_originalAudioConverterReset{};
AudioConverterDisposeFn g_originalAudioConverterDispose{};
AudioUnitSetPropertyFn g_originalAudioUnitSetProperty{};
AudioUnitGetPropertyFn g_originalAudioUnitGetProperty{};
AudioOutputUnitStartFn g_originalAudioOutputUnitStart{};
AudioOutputUnitStopFn g_originalAudioOutputUnitStop{};
AudioUnitInitializeFn g_originalAudioUnitInitialize{};
AudioUnitUninitializeFn g_originalAudioUnitUninitialize{};
AudioUnitRenderFn g_originalAudioUnitRender{};
AudioUnitAddRenderNotifyFn g_originalAudioUnitAddRenderNotify{};
AudioComponentInstanceNewFn g_originalAudioComponentInstanceNew{};
AudioComponentInstanceDisposeFn g_originalAudioComponentInstanceDispose{};
AudioComponentGetDescriptionFn g_audioComponentGetDescription{};
CoreMediaInternalSetCurrentTimeFn g_originalCoreMediaInternalSetCurrentTime{};
AVCFSetAudioTapProcessorFn g_originalAVCFSetAudioTapProcessor{};
AVCFSetPreferredMaximumSampleRateFn g_originalAVCFSetPreferredMaximumSampleRate{};
AVCFSetVariantPreferencesFn g_originalAVCFSetVariantPreferences{};
AVCFSetExclusiveOptimizationFn g_setAVCFExclusiveOptimization{};
AVCFGetExclusiveOptimizationFn g_getAVCFExclusiveOptimization{};
AVCFAssetTrackCopyFormatDescriptionsFn g_originalAVCFAssetTrackCopyFormatDescriptions{};
FigAlternateLosslessPreferenceFilterCreateFn
    g_originalFigAlternateLosslessPreferenceFilterCreate{};
FigAlternateEligibleLosslessFilterCreateFn
    g_originalFigAlternateEligibleLosslessFilterCreate{};
FigAlternateLossyPreferenceFilterCreateFn
    g_originalFigAlternateLossyPreferenceFilterCreate{};
FigAlternateHasLosslessAudioFn g_originalFigAlternateHasLosslessAudio{};
FigAlternateAllowableMediaSubtypeFilterCreateFn
    g_figAlternateAllowableMediaSubtypeFilterCreate{};
FigAlternatePlaybackBitrateMonitorCreateFn
    g_originalFigAlternatePlaybackBitrateMonitorCreate{};
FigAlternateFilterTreeSetFallbackBranchFn g_originalFigAlternateFilterTreeSetFallbackBranch{};
CFGetTypeIDFn g_cfGetTypeID{};
CFArrayGetCountFn g_cfArrayGetCount{};
CFArrayGetValueAtIndexFn g_cfArrayGetValueAtIndex{};
CFDictionaryGetValueFn g_cfDictionaryGetValue{};
CFNumberGetValueFn g_cfNumberGetValue{};
CFTypeIdFn g_cfArrayGetTypeID{};
CFTypeIdFn g_cfDictionaryGetTypeID{};
CFTypeIdFn g_cfNumberGetTypeID{};
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
void* g_avcfSampleRateKey{};
void* g_avcfLinearPcmBitDepthKey{};
void* g_avcfNumberOfChannelsKey{};
void* g_avcfFormatIdKey{};
std::atomic<std::uint64_t> g_figAlternateHasLosslessCalls{};
std::atomic<std::uint64_t> g_scheduledNaturalRecoveryCommitSequence{};
std::atomic<std::uint32_t> g_lastVariantPreferences{};
std::atomic<std::uint16_t> g_configuredStreamQuality{};
std::atomic<bool> g_configuredLosslessEnabled{};
std::atomic<bool> g_configuredQualityKnown{};
thread_local ULONGLONG g_threadEligibleLosslessFilterTick{};

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
std::atomic<std::uint64_t> g_nativeTraceInputCalls{};
std::atomic<std::uint64_t> g_nativeTracePropertyCalls{};
std::atomic<std::uint32_t> g_sourceSampleRate{};
std::atomic<std::uint32_t> g_sourceChannels{};
std::atomic<std::uint32_t> g_lastQlacSampleRate{};
std::atomic<std::uint32_t> g_lastQlacChannels{};
std::atomic<std::uint32_t> g_startupLockedSampleRate{};
std::atomic<std::uint32_t> g_startupLockedChannels{};
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
thread_local void* g_threadLocalPcmSourceConverter{};
struct HardwareOutputReplacementFormatOverride {
    std::uint32_t sampleRate{};
    std::uint32_t channels{};
    std::uint32_t sourceBitDepth{};
    std::uint64_t commitSequence{};
    bool active{};
};
thread_local HardwareOutputReplacementFormatOverride g_hardwareOutputReplacementFormatOverride;
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
    std::atomic<bool> announced{};

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

    void Activate() {
        std::lock_guard lock(mutex);
        active = true;
        abandoned = false;
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
    AudioStreamBasicDescription source{};
    AudioStreamBasicDescription destination{};
    std::uint64_t fillCalls{};
    bool correctedStreamingRate{};
    std::shared_ptr<LocalPcmQueue> localQueue;
};
std::mutex g_converterProbeMutex;
std::unordered_map<void*, ConverterProbeState> g_converterProbes;
std::mutex g_activeLocalQueueMutex;
std::shared_ptr<LocalPcmQueue> g_activeLocalQueue;
std::mutex g_pendingLocalQueueMutex;
std::shared_ptr<LocalPcmQueue> g_pendingLocalQueue;
std::uint64_t g_pendingLocalQueueGeneration{};
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

void AnnounceLocalPcmQueue(const std::shared_ptr<LocalPcmQueue>& queue) {
    if (!queue || queue->announced.exchange(true)) return;
    // The first packet proves only that Apple is prefetching this decoder. It does not prove that
    // the item has been promoted to the active output graph, so this queue must not take over live
    // output until a real AudioUnit lifecycle boundary binds it.
    std::lock_guard lock(g_pendingLocalQueueMutex);
    g_pendingLocalQueue = queue;
    ++g_pendingLocalQueueGeneration;
    Log(L"Local PCM queue retained as inactive prefetch generation=" +
        std::to_wstring(g_pendingLocalQueueGeneration) + L" rate=" +
        std::to_wstring(queue->sampleRate) + L" bits=" +
        std::to_wstring(queue->bitsPerChannel));
}
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
std::atomic<std::uint64_t> g_audioUnitEpochSequence{};
std::atomic<std::uint64_t> g_audioClientSequence{};
std::mutex g_audioUnitEpochMutex;
std::unordered_map<void*, std::uint64_t> g_audioUnitEpochs;
std::unordered_map<void*, AppleAudioComponentDescription> g_audioUnitDescriptions;
struct HardwareOutputPropertyRecord {
    std::uint32_t propertyId{};
    std::uint32_t scope{};
    std::uint32_t element{};
    std::vector<std::uint8_t> data;
    bool afterInitialize{};
};
struct HardwareOutputReplayState {
    void* component{};
    AppleAudioComponentDescription description{};
    void* replacement{};
    bool initialized{};
    std::vector<HardwareOutputPropertyRecord> properties;
};
std::unordered_map<void*, HardwareOutputReplayState> g_hardwareOutputReplayStates;
std::unordered_map<void*, void*> g_hardwareOutputLogicalByPhysical;
struct AudioUnitInputFormatState {
    AudioStreamBasicDescription format{};
    std::uint32_t sourceBitDepth{};
    std::uint64_t observationId{};
};
std::unordered_map<void*, AudioUnitInputFormatState> g_audioUnitInputFormats;
struct ActiveGraphFormatCommit {
    std::uint32_t sampleRate{};
    std::uint32_t channels{};
    std::uint32_t sourceBitDepth{};
    ULONGLONG committedTick{};
    std::uint64_t sequence{};
    void* unit{};
    std::uint64_t unitEpoch{};
    bool localPcm{};
};
std::mutex g_activeGraphFormatMutex;
ActiveGraphFormatCommit g_activeGraphFormat;
std::atomic<std::uint64_t> g_activeGraphFormatSequence{};
std::atomic<std::uint64_t> g_refreshedOutputCommitSequence{};
std::atomic<std::uint64_t> g_lastAudioUnitGetFormatSignature{};
std::atomic<std::uint64_t> g_renderGetCalls{};
std::atomic<std::uint64_t> g_renderReleaseCalls{};
std::atomic<std::uint64_t> g_renderRequestedFrames{};
std::atomic<std::uint64_t> g_renderWrittenFrames{};
std::atomic<std::uint64_t> g_renderBridgeSilentFlags{};
std::atomic<std::uint32_t> g_renderLastRequestedFrames{};
std::atomic<std::uint32_t> g_renderLastWrittenFrames{};
std::atomic<HRESULT> g_renderLastGetResult{S_OK};
std::atomic<HRESULT> g_renderLastReleaseResult{S_OK};
std::atomic<bool> g_renderGetFailureLogged{};
std::atomic<bool> g_renderReleaseFailureLogged{};
std::atomic<std::uint64_t> g_deviceEventSignals{};
std::atomic<std::uint64_t> g_forwardedAppleEventSignals{};
std::atomic<std::uint64_t> g_failedAppleEventSignals{};
std::atomic<std::uint64_t> g_audioUnitRenderCalls{};
std::atomic<std::uint64_t> g_modPumpCalls{};
std::atomic<bool> g_exclusiveAwaitingFirstRender{};
std::atomic<ULONGLONG> g_exclusiveStartTick{};
std::mutex g_activeEndpointMutex;
std::wstring g_activeEndpoint;
std::atomic<void*> g_currentAudioUnit{};
class AudioClientProxy;
std::mutex g_activeExclusiveProxyMutex;
AudioClientProxy* g_activeExclusiveProxy{};
bool PromotePendingLocalQueueAtDrain(
    const std::shared_ptr<LocalPcmQueue>& drainedQueue,
    std::shared_ptr<LocalPcmQueue>& promotedQueue,
    std::uint64_t& promotedGeneration,
    bool& requiresEndpointRebuild);
bool RebuildPendingLocalQueueAtDrain(
    const std::shared_ptr<LocalPcmQueue>& drainedQueue);
std::mutex g_audioUnitRenderTemplateMutex;
AppleAudioTimeStamp g_audioUnitRenderTimestamp{};
std::uint32_t g_audioUnitRenderBus{};
bool g_hasAudioUnitRenderTemplate{};
std::int64_t g_audioUnitRenderTemplateQpc{};
// A lock-free snapshot used by the v2 source-pull bridge. The ordinary
// AudioUnitRender hook records the last Apple-provided timestamp before the
// exclusive sink starts; the sink thread then advances that timestamp while
// it drives the same graph itself.
std::atomic<std::uint64_t> g_v2RenderTemplateSequence{};
AppleAudioTimeStamp g_v2RenderTemplate{};
void* g_v2RenderTemplateUnit{};
std::uint32_t g_v2RenderTemplateBus{};
std::uint32_t g_v2RenderTemplateFrames{};
std::int64_t g_v2RenderTemplateQpc{};
std::atomic<std::uint64_t> g_v2PumpCalls{};
std::atomic<std::uint64_t> g_v2PumpFailures{};
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
std::atomic<std::uint64_t> g_nonzeroRenderActionFlags{};
std::atomic<std::uint32_t> g_lastRenderActionFlags{};
std::atomic<std::uint64_t> g_maxDeviceEventGapUs{};
std::atomic<std::uint64_t> g_maxPumpDurationUs{};
std::atomic<std::uint64_t> g_lateDeviceEvents{};
std::atomic<std::int64_t> g_qpcFrequency{};
std::atomic<std::uint64_t> g_expectedPeriodUs{};
std::atomic<std::uint64_t> g_paddingCalls{};
std::atomic<std::uintptr_t> g_toolboxBase{};
std::atomic<std::uint32_t> g_toolboxSize{};
std::array<std::atomic<std::uint64_t>, 64> g_waitSignatures{};

std::wstring AddressLocation(void* address);

bool IsHardwareOutputDescription(const AppleAudioComponentDescription& description) {
    constexpr std::uint32_t outputType = 0x61756F75; // 'auou'
    constexpr std::uint32_t hardwareOutputSubtype = 0x61686C77; // 'ahlw'
    constexpr std::uint32_t appleManufacturer = 0x6170706C; // 'appl'
    return description.componentType == outputType &&
        description.componentSubType == hardwareOutputSubtype &&
        description.componentManufacturer == appleManufacturer;
}

bool IsHardwareOutputUnit(void* unit) {
    std::lock_guard lock(g_audioUnitEpochMutex);
    if (g_hardwareOutputReplayStates.contains(unit) ||
        g_hardwareOutputLogicalByPhysical.contains(unit)) return true;
    const auto description = g_audioUnitDescriptions.find(unit);
    return description != g_audioUnitDescriptions.end() &&
        IsHardwareOutputDescription(description->second);
}

void* LogicalHardwareOutputUnit(void* unit) {
    std::lock_guard lock(g_audioUnitEpochMutex);
    if (g_hardwareOutputReplayStates.contains(unit)) return unit;
    const auto owner = g_hardwareOutputLogicalByPhysical.find(unit);
    return owner == g_hardwareOutputLogicalByPhysical.end() ? nullptr : owner->second;
}

void* ResolveHardwareOutputUnit(void* unit) {
    std::lock_guard lock(g_audioUnitEpochMutex);
    auto logical = unit;
    const auto owner = g_hardwareOutputLogicalByPhysical.find(unit);
    if (owner != g_hardwareOutputLogicalByPhysical.end()) logical = owner->second;
    const auto state = g_hardwareOutputReplayStates.find(logical);
    return state != g_hardwareOutputReplayStates.end() && state->second.replacement
        ? state->second.replacement : unit;
}

void RememberHardwareOutputProperty(void* unit, std::uint32_t propertyId,
                                    std::uint32_t scope, std::uint32_t element,
                                    const void* data, std::uint32_t dataSize) {
    constexpr std::uint32_t maximumReplayPropertyBytes = 4096;
    if (!data || dataSize == 0 || dataSize > maximumReplayPropertyBytes) return;
    std::lock_guard lock(g_audioUnitEpochMutex);
    auto logical = unit;
    const auto owner = g_hardwareOutputLogicalByPhysical.find(unit);
    if (owner != g_hardwareOutputLogicalByPhysical.end()) logical = owner->second;
    const auto state = g_hardwareOutputReplayStates.find(logical);
    if (state == g_hardwareOutputReplayStates.end()) return;
    const bool afterInitialize = state->second.initialized;
    auto record = std::find_if(state->second.properties.begin(), state->second.properties.end(),
        [&](const HardwareOutputPropertyRecord& candidate) {
            return candidate.propertyId == propertyId && candidate.scope == scope &&
                candidate.element == element &&
                candidate.afterInitialize == afterInitialize;
        });
    if (record == state->second.properties.end()) {
        state->second.properties.push_back({propertyId, scope, element, {}, afterInitialize});
        record = std::prev(state->second.properties.end());
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    record->data.assign(bytes, bytes + dataSize);
}

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

void UpdateMaximum(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    auto previous = target.load();
    while (previous < value && !target.compare_exchange_weak(previous, value)) {}
}

void Log(const std::wstring& message);

DWORD WINAPI HookWaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto base = g_toolboxBase.load();
    const auto size = g_toolboxSize.load();
    bool firstObservation{};
    std::uint64_t signature{};
    if (base && caller >= base && caller < base + size) {
        const auto rva = caller - base;
        signature = (static_cast<std::uint64_t>(rva) << 32) ^
                    static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(handle));
        for (auto& slot : g_waitSignatures) {
            auto value = slot.load();
            if (value == signature) break;
            if (value == 0 && slot.compare_exchange_strong(value, signature)) {
                firstObservation = true;
                Log(L"CoreAudioToolbox WaitForSingleObject callerRva=0x" +
                    HResultText(static_cast<HRESULT>(rva)).substr(2) +
                    L" handle=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(handle)) +
                    L" timeoutMs=" + std::to_wstring(milliseconds));
                break;
            }
        }
    }
    const DWORD result = g_originalWaitForSingleObject(handle, milliseconds);
    if (firstObservation) {
        Log(L"CoreAudioToolbox WaitForSingleObject first return signature=" +
            std::to_wstring(signature) + L" result=" + std::to_wstring(result));
    }
    return result;
}

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

REFERENCE_TIME ReadPeriod() {
    const auto text = ReadSetting(L"period_100ns", L"0");
    wchar_t* end{};
    const long long value = wcstoll(text.c_str(), &end, 10);
    return end != text.c_str() && value > 0 ? value : 0;
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

std::wstring ComponentDescriptionText(const AppleAudioComponentDescription& description) {
    return L"type=" + AppleFormatIdText(description.componentType) +
        L" subtype=" + AppleFormatIdText(description.componentSubType) +
        L" manufacturer=" + AppleFormatIdText(description.componentManufacturer) +
        L" flags=" + HResultText(static_cast<HRESULT>(description.componentFlags));
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

std::uint64_t EnsureAudioUnitEpoch(void* unit) {
    if (!unit) return 0;
    std::lock_guard lock(g_audioUnitEpochMutex);
    const auto existing = g_audioUnitEpochs.find(unit);
    if (existing != g_audioUnitEpochs.end()) return existing->second;
    const auto epoch = g_audioUnitEpochSequence.fetch_add(1) + 1;
    g_audioUnitEpochs.emplace(unit, epoch);
    return epoch;
}

std::uint64_t FindAudioUnitEpoch(void* unit) {
    std::lock_guard lock(g_audioUnitEpochMutex);
    const auto existing = g_audioUnitEpochs.find(unit);
    return existing == g_audioUnitEpochs.end() ? 0 : existing->second;
}

void RetireAudioUnitEpoch(void* unit) {
    std::lock_guard lock(g_audioUnitEpochMutex);
    g_audioUnitEpochs.erase(unit);
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

void CommitInitializedAudioUnitFormat(void* unit, std::uint64_t epoch) {
    AudioUnitInputFormatState state{};
    if (!FindAudioUnitInputFormat(unit, state) || !IsAppleLinearPcm(&state.format) ||
        state.format.mSampleRate < 8000.0 || state.format.mSampleRate > 768000.0 ||
        state.format.mChannelsPerFrame != 2) return;
    ActiveGraphFormatCommit commit{};
    commit.sampleRate = static_cast<std::uint32_t>(state.format.mSampleRate + 0.5);
    commit.channels = state.format.mChannelsPerFrame;
    commit.sourceBitDepth = state.sourceBitDepth;
    commit.committedTick = GetTickCount64();
    commit.sequence = g_activeGraphFormatSequence.fetch_add(1) + 1;
    commit.unit = unit;
    commit.unitEpoch = epoch;
    {
        std::lock_guard lock(g_activeGraphFormatMutex);
        g_activeGraphFormat = commit;
    }
    Log(L"Active graph format committed by AudioUnitInitialize rate=" +
        std::to_wstring(commit.sampleRate) + L" sequence=" +
        std::to_wstring(commit.sequence) + L" unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) + L" epoch=" +
        std::to_wstring(epoch) + L" sourceBitDepth=" +
        std::to_wstring(commit.sourceBitDepth) + L" observation=" +
        std::to_wstring(state.observationId));
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
        g_sourceSampleRate.store(local.sampleRate);
        g_sourceChannels.store(local.channels);
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
    g_sourceSampleRate.store(rate);
    g_sourceChannels.store(destination->mChannelsPerFrame);
    constexpr std::uint32_t appleLossless = 0x716C6163; // 'qlac'
    if (source->mFormatID == appleLossless) {
        g_lastQlacSampleRate.store(rate);
        g_lastQlacChannels.store(destination->mChannelsPerFrame);
        const auto rateText = std::to_wstring(rate);
        const auto channelText = std::to_wstring(destination->mChannelsPerFrame);
        WritePrivateProfileStringW(L"mod", L"last_qlac_rate", rateText.c_str(), g_iniPath.c_str());
        WritePrivateProfileStringW(L"mod", L"last_qlac_channels", channelText.c_str(),
                                   g_iniPath.c_str());
    }
    Log(L"Decoded source format observed rate=" + std::to_wstring(rate) +
        L" channels=" + std::to_wstring(destination->mChannelsPerFrame) +
        L" encodedFormat=" + AppleFormatIdText(source->mFormatID) +
        L" observation=" + std::to_wstring(observationId) +
        L" tid=" + std::to_wstring(GetCurrentThreadId()) +
        L" qpc=" + std::to_wstring(g_threadGraphFormat.qpc) +
        L" converter=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
        L" caller=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(caller)));
}

WAVEFORMATEXTENSIBLE IntegerPcmFormat(std::uint32_t sampleRate, std::uint16_t channels,
                                      std::uint16_t validBits,
                                      std::uint16_t containerBits = 32) {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = channels;
    format.Format.nSamplesPerSec = sampleRate;
    format.Format.wBitsPerSample = containerBits;
    format.Format.nBlockAlign = static_cast<WORD>(channels * containerBits / 8u);
    format.Format.nAvgBytesPerSec = sampleRate * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = validBits;
    format.dwChannelMask = channels == 2 ? 0x3 : 0;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return format;
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

void ObserveAVCFExclusiveOptimization(void* playerItem, const wchar_t* source) {
    if (!playerItem || !ExclusiveRequested()) return;
    const int observed = g_getAVCFExclusiveOptimization
                             ? static_cast<int>(g_getAVCFExclusiveOptimization(playerItem))
                             : -1;
    std::wostringstream out;
    out << L"AVCF exclusive playback optimization left unchanged source=" << source
        << L" playerItem=" << playerItem << L" getter=" << observed;
    Log(out.str());
}

AppleOSStatus __cdecl HookCoreMediaInternalSetCurrentTime(
    void* playerItem, const AppleCMTime* time, std::uint32_t options) {
    std::shared_ptr<LocalPcmQueue> queue;
    {
        std::lock_guard lock(g_activeLocalQueueMutex);
        queue = g_activeLocalQueue;
    }
    if (queue) queue->FlushForSeek();
    std::wostringstream out;
    out << L"CoreMedia itemasync_SetCurrentTime playerItem=" << playerItem
        << L" options=0x" << std::hex << options << std::dec
        << L" flushedActiveQueue=" << (queue ? 1 : 0);
    if (time) {
        out << L" value=" << time->value << L" timescale=" << time->timescale
            << L" flags=0x" << std::hex << time->flags << std::dec
            << L" epoch=" << time->epoch;
        if (time->timescale > 0) {
            out << L" seconds=" << std::setprecision(9)
                << (static_cast<double>(time->value) /
                    static_cast<double>(time->timescale));
        }
    }
    out << L" tid=" << GetCurrentThreadId() << L" qpc=" << CurrentQpc();
    Log(out.str());
    const AppleOSStatus result =
        g_originalCoreMediaInternalSetCurrentTime(playerItem, time, options);
    Log(L"CoreMedia itemasync_SetCurrentTime queued result=" +
        std::to_wstring(result) + L" qpc=" + std::to_wstring(CurrentQpc()));
    return result;
}

void __cdecl HookAVCFSetAudioTapProcessor(void* playerItem, void* processor) {
    g_originalAVCFSetAudioTapProcessor(playerItem, processor);
    ObserveAVCFExclusiveOptimization(playerItem, L"SetAudioTapProcessor");
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

AppleOSStatus __cdecl HookFigAlternatePlaybackBitrateMonitorCreate(
    void* allocator, void* selectionBoss, void* bandwidthEstimator, void* networkClock,
    void* arg5, void* arg6, void* arg7, void* arg8, void* arg9,
    unsigned char startsOnFirstEligibleVariant, void** outMonitor) {
    const std::uint32_t preferences =
        g_lastVariantPreferences.load(std::memory_order_acquire);
    std::wostringstream value;
    value << std::hex << preferences;
    Log(L"FigAlternateFilterMonitorCreateForPlaybackBitrate startsOnFirstEligibleVariant=" +
        std::to_wstring(static_cast<unsigned int>(startsOnFirstEligibleVariant)) +
        L" variantPreferences=0x" + value.str());
    return g_originalFigAlternatePlaybackBitrateMonitorCreate(
        allocator, selectionBoss, bandwidthEstimator, networkClock, arg5, arg6, arg7, arg8,
        arg9, startsOnFirstEligibleVariant, outMonitor);
}

AppleOSStatus __cdecl HookFigAlternateLosslessPreferenceFilterCreate(
    void* allocator, unsigned char preferLossless, void* outFilter) {
    const AppleOSStatus result = g_originalFigAlternateLosslessPreferenceFilterCreate(
        allocator, preferLossless, outFilter);
    void* filter = outFilter ? *static_cast<void**>(outFilter) : nullptr;
    Log(L"FigAlternateLosslessAudioPreferenceFilterCreate preferLossless=" +
        std::to_wstring(static_cast<unsigned int>(preferLossless)) + L" result=" +
        std::to_wstring(result) + L" filter=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(filter)));
    return result;
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
    if (result == 0 && filter) g_threadEligibleLosslessFilterTick = GetTickCount64();
    Log(L"FigAlternateEligibleLosslessAudioFilterCreate options=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(options)) +
        L" strictRequested=" + std::to_wstring(strictRequested) +
        L" strictSubtypeFilter=" + std::to_wstring(strictSubtypeFilter) + L" result=" +
        std::to_wstring(result) + L" filter=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(filter)));
    return result;
}

AppleOSStatus __cdecl HookFigAlternateFilterTreeSetFallbackBranch(void* tree, void* branch) {
    const auto now = GetTickCount64();
    const bool followsEligibleLossless = g_threadEligibleLosslessFilterTick != 0 &&
        now - g_threadEligibleLosslessFilterTick <= 1000;
    Log(L"FigAlternateFilterTreeSetFallbackBranch tree=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(tree)) + L" branch=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(branch)) +
        L" followsEligibleLossless=" + std::to_wstring(followsEligibleLossless));
    return g_originalFigAlternateFilterTreeSetFallbackBranch(tree, branch);
}

AppleOSStatus __cdecl HookFigAlternateLossyPreferenceFilterCreate(
    void* allocator, void* outFilter) {
    const AppleOSStatus result = g_originalFigAlternateLossyPreferenceFilterCreate(
        allocator, outFilter);
    void* filter = outFilter ? *static_cast<void**>(outFilter) : nullptr;
    Log(L"FigAlternateLossyAudioPreferenceFilterCreate result=" +
        std::to_wstring(result) + L" filter=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(filter)));
    return result;
}

unsigned char __cdecl HookFigAlternateHasLosslessAudio(void* alternate) {
    const unsigned char result = g_originalFigAlternateHasLosslessAudio(alternate);
    const auto call = g_figAlternateHasLosslessCalls.fetch_add(1) + 1;
    if (call <= 128 || (call % 256) == 0) {
        Log(L"FigAlternateHasLosslessAudio call=" + std::to_wstring(call) +
            L" alternate=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(alternate)) +
            L" result=" + std::to_wstring(static_cast<unsigned int>(result)));
    }
    return result;
}

bool ReadCfUInt32(const void* dictionary, const void* key, std::uint32_t& value) {
    if (!dictionary || !key || !g_cfDictionaryGetValue || !g_cfNumberGetValue ||
        !g_cfGetTypeID || !g_cfNumberGetTypeID) return false;
    const void* number = g_cfDictionaryGetValue(dictionary, key);
    if (!number || g_cfGetTypeID(number) != g_cfNumberGetTypeID()) return false;
    std::int32_t signedValue{};
    constexpr std::int32_t cfNumberSInt32Type = 3;
    if (!g_cfNumberGetValue(number, cfNumberSInt32Type, &signedValue) || signedValue < 0) return false;
    value = static_cast<std::uint32_t>(signedValue);
    return true;
}

bool ReadCfSampleRate(const void* dictionary, std::uint32_t& value) {
    if (ReadCfUInt32(dictionary, g_avcfSampleRateKey, value)) return true;
    if (!dictionary || !g_avcfSampleRateKey || !g_cfDictionaryGetValue ||
        !g_cfNumberGetValue || !g_cfGetTypeID || !g_cfNumberGetTypeID) return false;
    const void* number = g_cfDictionaryGetValue(dictionary, g_avcfSampleRateKey);
    if (!number || g_cfGetTypeID(number) != g_cfNumberGetTypeID()) return false;
    double doubleValue{};
    constexpr std::int32_t cfNumberFloat64Type = 6;
    if (!g_cfNumberGetValue(number, cfNumberFloat64Type, &doubleValue) ||
        doubleValue < 8000.0 || doubleValue > 768000.0) return false;
    value = static_cast<std::uint32_t>(doubleValue + 0.5);
    return true;
}

void LogAssetTrackFormatDescription(void* assetTrack, const void* description,
                                    std::int64_t index) {
    if (!description || !g_cfGetTypeID || !g_cfDictionaryGetTypeID ||
        g_cfGetTypeID(description) != g_cfDictionaryGetTypeID()) return;
    std::uint32_t sampleRate{}, bitDepth{}, channels{}, formatId{};
    const bool hasRate = ReadCfSampleRate(description, sampleRate);
    const bool hasDepth = ReadCfUInt32(description, g_avcfLinearPcmBitDepthKey, bitDepth);
    const bool hasChannels = ReadCfUInt32(description, g_avcfNumberOfChannelsKey, channels);
    const bool hasFormat = ReadCfUInt32(description, g_avcfFormatIdKey, formatId);
    if (!hasRate && !hasDepth && !hasChannels && !hasFormat) return;
    Log(L"AVCF asset-track format description track=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(assetTrack)) + L" index=" +
        std::to_wstring(index) + L" rate=" +
        (hasRate ? std::to_wstring(sampleRate) : L"unknown") + L" bitDepth=" +
        (hasDepth ? std::to_wstring(bitDepth) : L"unknown") + L" channels=" +
        (hasChannels ? std::to_wstring(channels) : L"unknown") + L" formatId=" +
        (hasFormat ? AppleFormatIdText(formatId) : L"unknown") + L" tid=" +
        std::to_wstring(GetCurrentThreadId()) + L" qpc=" + std::to_wstring(CurrentQpc()));
}

void* __cdecl HookAVCFAssetTrackCopyFormatDescriptions(void* assetTrack) {
    void* descriptions = g_originalAVCFAssetTrackCopyFormatDescriptions(assetTrack);
    if (!descriptions || !g_cfGetTypeID || !g_cfArrayGetTypeID ||
        !g_cfArrayGetCount || !g_cfArrayGetValueAtIndex ||
        g_cfGetTypeID(descriptions) != g_cfArrayGetTypeID()) return descriptions;
    const auto count = g_cfArrayGetCount(descriptions);
    if (count < 0 || count > 64) return descriptions;
    for (std::int64_t index = 0; index < count; ++index) {
        LogAssetTrackFormatDescription(assetTrack,
            g_cfArrayGetValueAtIndex(descriptions, index), index);
    }
    return descriptions;
}

bool LaunchMediaRoundTripAfterFailedNaturalTransition(
        std::uint64_t commitSequence, DWORD& helperPid) {
    if (!commitSequence) return false;
    auto scheduled = g_scheduledNaturalRecoveryCommitSequence.load(std::memory_order_acquire);
    while (scheduled != commitSequence) {
        if (g_scheduledNaturalRecoveryCommitSequence.compare_exchange_weak(
                scheduled, commitSequence, std::memory_order_acq_rel)) break;
    }
    if (scheduled == commitSequence) return true;
    wchar_t modulePath[32768]{};
    const DWORD chars = GetModuleFileNameW(
        g_module, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (!chars || chars >= std::size(modulePath)) {
        g_scheduledNaturalRecoveryCommitSequence.store(0, std::memory_order_release);
        return false;
    }
    const std::filesystem::path helper =
        std::filesystem::path(modulePath).parent_path() / L"am_exclusive_media_control.exe";
    if (GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"Natural cross-rate recovery helper is missing path=" + helper.wstring());
        g_scheduledNaturalRecoveryCommitSequence.store(0, std::memory_order_release);
        return false;
    }
    std::wstring command = L"\"" + helper.wstring() + L"\" restart-current";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(
        helper.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, helper.parent_path().c_str(), &startup, &process);
    if (!ok) {
        Log(L"Natural cross-rate recovery CreateProcess failed error=" +
            std::to_wstring(GetLastError()) + L" path=" + helper.wstring());
        g_scheduledNaturalRecoveryCommitSequence.store(0, std::memory_order_release);
        return false;
    }
    helperPid = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Log(L"Natural cross-rate recovery Next-confirm-Previous round trip launched after failed output Start "
        L"commitSequence=" + std::to_wstring(commitSequence) + L" pid=" +
        std::to_wstring(helperPid));
    return true;
}

bool LaunchMediaControlCommand(std::wstring_view action, DWORD& helperPid) {
    wchar_t modulePath[32768]{};
    const DWORD chars = GetModuleFileNameW(
        g_module, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (!chars || chars >= std::size(modulePath) || action.empty()) return false;
    const std::filesystem::path helper =
        std::filesystem::path(modulePath).parent_path() / L"am_exclusive_media_control.exe";
    if (GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring command = L"\"" + helper.wstring() + L"\" " + std::wstring(action);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, helper.parent_path().c_str(),
                        &startup, &process)) {
        return false;
    }
    helperPid = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool RewriteFreshStreamingConverterDestination(
    const AudioStreamBasicDescription* source,
    const AudioStreamBasicDescription* destination,
    void* caller,
    AudioStreamBasicDescription& rewritten,
    ActiveGraphFormatCommit& matchedCommit) {
    if (!ExclusiveRequested() || !source || !destination ||
        !IsAppleLinearPcm(source) || !IsAppleLinearPcm(destination) ||
        source->mChannelsPerFrame != 2 ||
        destination->mChannelsPerFrame != source->mChannelsPerFrame ||
        source->mSampleRate < 8000.0 || source->mSampleRate > 768000.0 ||
        static_cast<std::uint32_t>(source->mSampleRate + 0.5) ==
            static_cast<std::uint32_t>(destination->mSampleRate + 0.5)) {
        return false;
    }

    const auto callerAddress = reinterpret_cast<std::uintptr_t>(caller);
    const auto toolboxBase = g_toolboxBase.load();
    const auto toolboxSize = g_toolboxSize.load();
    if (!toolboxBase || !toolboxSize || callerAddress < toolboxBase ||
        callerAddress >= toolboxBase + toolboxSize) {
        return false;
    }

    {
        std::lock_guard lock(g_activeGraphFormatMutex);
        matchedCommit = g_activeGraphFormat;
    }
    const auto sourceRate = static_cast<std::uint32_t>(source->mSampleRate + 0.5);
    const auto age = matchedCommit.committedTick != 0
        ? GetTickCount64() - matchedCommit.committedTick : UINT64_MAX;
    const bool supportedDepth = matchedCommit.sourceBitDepth == 16 ||
        matchedCommit.sourceBitDepth == 24 ||
        matchedCommit.sourceBitDepth == 32;
    if (age > 500 || matchedCommit.localPcm ||
        matchedCommit.sampleRate != sourceRate ||
        matchedCommit.channels != source->mChannelsPerFrame || !supportedDepth) {
        return false;
    }

    rewritten = *destination;
    rewritten.mSampleRate = source->mSampleRate;
    return true;
}

AppleOSStatus __cdecl HookAudioConverterNew(const AudioStreamBasicDescription* source,
                                             const AudioStreamBasicDescription* destination,
                                             void** converter) {
    void* caller = _ReturnAddress();
    AudioStreamBasicDescription rewrittenDestination{};
    ActiveGraphFormatCommit matchedCommit{};
    const bool destinationRewritten = RewriteFreshStreamingConverterDestination(
        source, destination, caller, rewrittenDestination, matchedCommit);
    const auto* effectiveDestination = destinationRewritten ? &rewrittenDestination : destination;
    Log(L"AudioConverterNew source{" + AppleFormatText(source) + L"} destination{" +
        AppleFormatText(destination) + L"} effectiveDestination{" +
        AppleFormatText(effectiveDestination) + L"}");
    if (destinationRewritten) {
        Log(L"AudioConverterNew corrected fresh streaming SRC destination oldRate=" +
            std::to_wstring(static_cast<std::uint32_t>(destination->mSampleRate + 0.5)) +
            L" newRate=" + std::to_wstring(matchedCommit.sampleRate) +
            L" commitSequence=" + std::to_wstring(matchedCommit.sequence));
    }
    const AppleOSStatus result =
        g_originalAudioConverterNew(source, effectiveDestination, converter);
    if (result == 0 && converter && *converter && source && effectiveDestination) {
        std::shared_ptr<LocalPcmQueue> localQueue;
        ammod::audio::LocalPcmCandidate local{};
        const auto callerAddress = reinterpret_cast<std::uintptr_t>(caller);
        const auto toolboxBase = g_toolboxBase.load();
        const auto toolboxSize = g_toolboxSize.load();
        const bool callerOutsideToolbox = !toolboxBase || callerAddress < toolboxBase ||
            callerAddress >= toolboxBase + toolboxSize;
        if (callerOutsideToolbox && ammod::audio::TryClassifyLocalPcmCandidate(
                PcmFormatView(*source), PcmFormatView(*effectiveDestination), local) &&
            source->mBytesPerFrame != 0 && source->mBytesPerFrame <= 16) {
            localQueue = std::make_shared<LocalPcmQueue>();
            localQueue->sampleRate = local.sampleRate;
            localQueue->channels = local.channels;
            localQueue->bitsPerChannel = local.sourceBitDepth;
            localQueue->bytesPerFrame = source->mBytesPerFrame;
            localQueue->formatFlags = source->mFormatFlags;
            // Do not block Apple's converter callback: it also participates in media-time
            // advancement. The private CoreMedia SetCurrentTime hook is the seek boundary;
            // AudioConverterReset is also used internally during uninterrupted playback.
            localQueue->maximumBytes = static_cast<std::size_t>(local.sampleRate) *
                source->mBytesPerFrame * 10;
            g_threadLocalPcmSourceConverter = *converter;
        }
        std::lock_guard lock(g_converterProbeMutex);
        g_converterProbes[*converter] = {
            *source, *effectiveDestination, 0, destinationRewritten, std::move(localQueue)};
    }
    ObserveDecodedSourceFormat(source, effectiveDestination,
                               result == 0 && converter ? *converter : nullptr, caller);
    Log(L"AudioConverterNew result=" + std::to_wstring(result) +
        L" converter=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(
            result == 0 && converter ? *converter : nullptr)));
    return result;
}

AppleOSStatus __cdecl HookAudioConverterNewSpecific(
    const AudioStreamBasicDescription* source,
    const AudioStreamBasicDescription* destination,
    std::uint32_t classDescriptionCount,
    const void* classDescriptions,
    void** converter) {
    void* caller = _ReturnAddress();
    AudioStreamBasicDescription rewrittenDestination{};
    ActiveGraphFormatCommit matchedCommit{};
    const bool destinationRewritten = RewriteFreshStreamingConverterDestination(
        source, destination, caller, rewrittenDestination, matchedCommit);
    const auto* effectiveDestination = destinationRewritten ? &rewrittenDestination : destination;
    Log(L"AudioConverterNewSpecific source{" + AppleFormatText(source) + L"} destination{" +
        AppleFormatText(destination) + L"} effectiveDestination{" +
        AppleFormatText(effectiveDestination) + L"} classDescriptions=" +
        std::to_wstring(classDescriptionCount));
    if (destinationRewritten) {
        Log(L"AudioConverterNewSpecific corrected fresh streaming SRC destination oldRate=" +
            std::to_wstring(static_cast<std::uint32_t>(destination->mSampleRate + 0.5)) +
            L" newRate=" + std::to_wstring(matchedCommit.sampleRate) +
            L" commitSequence=" + std::to_wstring(matchedCommit.sequence));
    }
    const AppleOSStatus result = g_originalAudioConverterNewSpecific(
        source, effectiveDestination, classDescriptionCount, classDescriptions, converter);
    if (result == 0 && converter && *converter && source && effectiveDestination) {
        std::shared_ptr<LocalPcmQueue> localQueue;
        ammod::audio::LocalPcmCandidate local{};
        const auto callerAddress = reinterpret_cast<std::uintptr_t>(caller);
        const auto toolboxBase = g_toolboxBase.load();
        const auto toolboxSize = g_toolboxSize.load();
        const bool callerOutsideToolbox = !toolboxBase || callerAddress < toolboxBase ||
            callerAddress >= toolboxBase + toolboxSize;
        if (callerOutsideToolbox && ammod::audio::TryClassifyLocalPcmCandidate(
                PcmFormatView(*source), PcmFormatView(*effectiveDestination), local) &&
            source->mBytesPerFrame != 0 && source->mBytesPerFrame <= 16) {
            localQueue = std::make_shared<LocalPcmQueue>();
            localQueue->sampleRate = local.sampleRate;
            localQueue->channels = local.channels;
            localQueue->bitsPerChannel = local.sourceBitDepth;
            localQueue->bytesPerFrame = source->mBytesPerFrame;
            localQueue->formatFlags = source->mFormatFlags;
            localQueue->maximumBytes = static_cast<std::size_t>(local.sampleRate) *
                source->mBytesPerFrame * 10;
            g_threadLocalPcmSourceConverter = *converter;
        }
        std::lock_guard lock(g_converterProbeMutex);
        g_converterProbes[*converter] = {
            *source, *effectiveDestination, 0, destinationRewritten, std::move(localQueue)};
    }
    ObserveDecodedSourceFormat(source, effectiveDestination,
                               result == 0 && converter ? *converter : nullptr, caller);
    Log(L"AudioConverterNewSpecific result=" + std::to_wstring(result) +
        L" converter=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(
            result == 0 && converter ? *converter : nullptr)));
    return result;
}

AppleOSStatus __cdecl HookAudioConverterFillComplexBuffer(
    void* converter, AudioConverterComplexInputDataProcFn inputProc, void* inputUserData,
    std::uint32_t* outputPackets, void* outputData, void* packetDescriptions) {
    std::uint64_t call{};
    AudioStreamBasicDescription source{};
    AudioStreamBasicDescription destination{};
    bool correctedStreamingRate{};
    std::shared_ptr<LocalPcmQueue> localQueue;
    {
        std::lock_guard lock(g_converterProbeMutex);
        const auto found = g_converterProbes.find(converter);
        if (found != g_converterProbes.end()) {
            call = ++found->second.fillCalls;
            source = found->second.source;
            destination = found->second.destination;
            correctedStreamingRate = found->second.correctedStreamingRate;
            localQueue = found->second.localQueue;
        }
    }
    struct InputProbeContext {
        AudioConverterComplexInputDataProcFn original{};
        void* userData{};
        void* converter{};
        std::uint64_t fillCall{};
        std::uint64_t inputCalls{};
        bool correctedStreamingRate{};
        std::shared_ptr<LocalPcmQueue> queue;
    } inputContext{inputProc, inputUserData, converter, call, 0,
                   correctedStreamingRate, localQueue};
    const auto proxyInput = +[](void* inputConverter, std::uint32_t* packets, void* data,
                                void** descriptions, void* context) -> AppleOSStatus {
        auto* probe = static_cast<InputProbeContext*>(context);
        const auto requestedInputPackets = packets ? *packets : 0;
        const AppleOSStatus inputResult = probe->original(
            inputConverter, packets, data, descriptions, probe->userData);
        const auto inputCall = ++probe->inputCalls;
        if (inputResult == 0 && probe->queue && data) {
            const auto* list = static_cast<const AppleAudioBufferList*>(data);
            if (list->mNumberBuffers == 1 && list->mBuffers[0].mData &&
                list->mBuffers[0].mDataByteSize != 0) {
                probe->queue->Push(list->mBuffers[0].mData,
                                   list->mBuffers[0].mDataByteSize);
                AnnounceLocalPcmQueue(probe->queue);
            }
        }
        if (probe->fillCall <= 2 && inputCall <= 4) {
            std::uint32_t buffers{}, bytes{}, channels{};
            if (data) {
                const auto* list = static_cast<const AppleAudioBufferList*>(data);
                buffers = list->mNumberBuffers;
                if (buffers != 0 && buffers <= 64) {
                    bytes = list->mBuffers[0].mDataByteSize;
                    channels = list->mBuffers[0].mNumberChannels;
                }
            }
            Log(L"AudioConverter input callback converter=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(probe->converter)) +
                L" fillCall=" + std::to_wstring(probe->fillCall) +
                L" inputCall=" + std::to_wstring(inputCall) + L" tid=" +
                std::to_wstring(GetCurrentThreadId()) + L" requestedPackets=" +
                std::to_wstring(requestedInputPackets) + L" returnedPackets=" +
                std::to_wstring(packets ? *packets : 0) + L" buffers=" +
                std::to_wstring(buffers) + L" firstBytes=" + std::to_wstring(bytes) +
                L" firstChannels=" + std::to_wstring(channels) +
                L" result=" + std::to_wstring(inputResult));
        }
        return inputResult;
    };
    const bool probeInput = call != 0 && inputProc &&
        (correctedStreamingRate || (localQueue && IsAppleLinearPcm(&source) &&
                                    destination.mSampleRate != source.mSampleRate));
    const auto requestedPackets = outputPackets ? *outputPackets : 0;
    const AppleOSStatus result = g_originalAudioConverterFillComplexBuffer(
        converter, probeInput ? proxyInput : inputProc,
        probeInput ? static_cast<void*>(&inputContext) : inputUserData,
        outputPackets, outputData, packetDescriptions);
    if (call != 0 && call <= 8) {
        std::uint32_t bufferCount{};
        std::uint32_t firstBufferBytes{};
        std::uint32_t firstBufferChannels{};
        if (outputData) {
            const auto* list = static_cast<const AppleAudioBufferList*>(outputData);
            bufferCount = list->mNumberBuffers;
            if (bufferCount != 0 && bufferCount <= 64) {
                firstBufferBytes = list->mBuffers[0].mDataByteSize;
                firstBufferChannels = list->mBuffers[0].mNumberChannels;
            }
        }
        Log(L"AudioConverterFillComplexBuffer converter=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) +
            L" call=" + std::to_wstring(call) + L" tid=" +
            std::to_wstring(GetCurrentThreadId()) + L" requestedPackets=" +
            std::to_wstring(requestedPackets) + L" producedPackets=" +
            std::to_wstring(outputPackets ? *outputPackets : 0) + L" buffers=" +
            std::to_wstring(bufferCount) + L" firstBytes=" +
            std::to_wstring(firstBufferBytes) + L" firstChannels=" +
            std::to_wstring(firstBufferChannels) + L" sourceRate=" +
            std::to_wstring(static_cast<std::uint32_t>(source.mSampleRate + 0.5)) +
            L" sourceBits=" + std::to_wstring(source.mBitsPerChannel) +
            L" destinationRate=" +
            std::to_wstring(static_cast<std::uint32_t>(destination.mSampleRate + 0.5)) +
            L" result=" + std::to_wstring(result));
    }
    return result;
}

AppleOSStatus __cdecl HookAudioConverterDispose(void* converter) {
    std::shared_ptr<LocalPcmQueue> disposedQueue;
    {
        std::lock_guard lock(g_converterProbeMutex);
        const auto found = g_converterProbes.find(converter);
        if (found != g_converterProbes.end()) {
            disposedQueue = found->second.localQueue;
            g_converterProbes.erase(found);
        }
    }
    bool abandonedActive{};
    if (disposedQueue) {
        std::lock_guard lock(g_activeLocalQueueMutex);
        if (g_activeLocalQueue == disposedQueue) {
            disposedQueue->Abandon();
            g_activeLocalQueue.reset();
            abandonedActive = true;
        }
    }
    const AppleOSStatus result = g_originalAudioConverterDispose(converter);
    Log(L"AudioConverterDispose converter=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) + L" result=" +
        std::to_wstring(result) + L" abandonedActiveQueue=" +
        std::to_wstring(abandonedActive));
    return result;
}

AppleOSStatus __cdecl HookAudioConverterReset(void* converter) {
    void* resetStack[6]{};
    const auto resetStackCount = CaptureStackBackTrace(
        1, static_cast<DWORD>(std::size(resetStack)), resetStack, nullptr);
    const auto agentBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto resetRva = [agentBase](void* address) -> std::uintptr_t {
        const auto value = reinterpret_cast<std::uintptr_t>(address);
        return agentBase && value >= agentBase ? value - agentBase : 0;
    };
    const bool seekReset = resetStackCount >= 3 &&
        resetRva(resetStack[0]) == ammod::apple_private::agent_stack::kLocalSeekResetFrame0 &&
        resetRva(resetStack[1]) == ammod::apple_private::agent_stack::kLocalSeekResetFrame1 &&
        (resetRva(resetStack[2]) == ammod::apple_private::agent_stack::kLocalSeekResetFrame2[0] ||
         resetRva(resetStack[2]) == ammod::apple_private::agent_stack::kLocalSeekResetFrame2[1]);
    const AppleOSStatus result = g_originalAudioConverterReset(converter);
    std::shared_ptr<LocalPcmQueue> resetQueue;
    {
        std::lock_guard lock(g_converterProbeMutex);
        const auto found = g_converterProbes.find(converter);
        if (found != g_converterProbes.end()) resetQueue = found->second.localQueue;
    }
    bool activeQueue{};
    bool flushedSeekQueue{};
    if (resetQueue) {
        std::lock_guard lock(g_activeLocalQueueMutex);
        activeQueue = g_activeLocalQueue == resetQueue;
        if (result == 0 && activeQueue && seekReset) {
            resetQueue->FlushForSeek();
            flushedSeekQueue = true;
        }
    }
    std::wstring stackText;
    for (USHORT index = 0; index < resetStackCount; ++index) {
        if (!stackText.empty()) stackText += L" <- ";
        stackText += AddressLocation(resetStack[index]);
    }
    Log(L"AudioConverterReset converter=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(converter)) + L" result=" +
        std::to_wstring(result) + L" activeQueue=" + std::to_wstring(activeQueue) +
        L" seekReset=" + std::to_wstring(seekReset) + L" flushedSeekQueue=" +
        std::to_wstring(flushedSeekQueue) +
        L" stack=" + stackText);
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

std::uint16_t OutputValidBitsForSource(std::uint32_t sourceBitDepth) {
    // Apple Music lossless is at most 24-bit.  This endpoint rejects packed 16/24-bit
    // containers but accepts 24 valid bits in a 32-bit PCM container.  Padding a
    // A 16-bit integer source carried by a wider endpoint container remains exact.
    return ammod::audio::OutputValidBitsForSource(sourceBitDepth);
}

std::uint16_t WaveValidBits(const WAVEFORMATEX* format) {
    if (!format) return 0;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM &&
            extensible->Samples.wValidBitsPerSample != 0) {
            return extensible->Samples.wValidBitsPerSample;
        }
    }
    return format->wBitsPerSample;
}

bool IsWaveFloat(const WAVEFORMATEX* format) {
    if (!format) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    return format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
        reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat ==
            KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

bool IsWaveInteger(const WAVEFORMATEX* format) {
    if (!format) return false;
    if (format->wFormatTag == WAVE_FORMAT_PCM) return true;
    return format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
        reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat ==
            KSDATAFORMAT_SUBTYPE_PCM;
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

bool IsExactRecentQlacObservation(std::uint32_t sampleRate, std::uint32_t channels,
                                  std::uint32_t sourceBitDepth,
                                  std::uint64_t observationId) {
    if (!observationId || !IsSupportedSourceBitDepth(sourceBitDepth)) return false;
    constexpr ULONGLONG evidenceLifetimeMs = 10 * 60 * 1000;
    const auto now = GetTickCount64();
    std::lock_guard lock(g_qlacBitDepthEvidenceMutex);
    for (auto evidence = g_qlacBitDepthEvidence.rbegin();
         evidence != g_qlacBitDepthEvidence.rend(); ++evidence) {
        if (now - evidence->observedTick > evidenceLifetimeMs) break;
        if (evidence->observationId == observationId &&
            evidence->sampleRate == sampleRate && evidence->channels == channels &&
            evidence->sourceBitDepth == sourceBitDepth) return true;
    }
    return false;
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

AppleOSStatus __cdecl HookAudioConverterSetProperty(void* converter, std::uint32_t propertyId,
                                                     std::uint32_t dataSize, const void* data) {
    const AppleOSStatus result =
        g_originalAudioConverterSetProperty(converter, propertyId, dataSize, data);
    ObserveAudioConverterProperty(L"SetProperty", converter, propertyId, data, dataSize, result);
    return result;
}

AppleOSStatus __cdecl HookAudioConverterGetProperty(void* converter, std::uint32_t propertyId,
                                                     std::uint32_t* dataSize, void* data) {
    const AppleOSStatus result =
        g_originalAudioConverterGetProperty(converter, propertyId, dataSize, data);
    ObserveAudioConverterProperty(L"GetProperty", converter, propertyId, data,
                                  dataSize ? *dataSize : 0, result);
    return result;
}

AppleOSStatus __cdecl HookAudioUnitSetProperty(void* unit, std::uint32_t propertyId,
                                                 std::uint32_t scope, std::uint32_t element,
                                                 const void* data, std::uint32_t dataSize) {
    constexpr std::uint32_t streamFormatProperty = 8;
    const auto epoch = EnsureAudioUnitEpoch(unit);
    const bool hardwareOutput = IsHardwareOutputUnit(unit);
    void* const targetUnit = hardwareOutput ? ResolveHardwareOutputUnit(unit) : unit;
    if (hardwareOutput) {
        Log(L"Hardware output AudioUnitSetProperty entered unit=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) + L" epoch=" +
            std::to_wstring(epoch) + L" propertyId=" + std::to_wstring(propertyId) +
            L" propertyFourCC=" + AppleFormatIdText(propertyId) + L" scope=" +
            std::to_wstring(scope) + L" element=" + std::to_wstring(element) +
            L" dataSize=" + std::to_wstring(dataSize) + L" stack=" +
            CaptureCurrentStackText());
    }
    if (propertyId == streamFormatProperty && data &&
        dataSize >= sizeof(AudioStreamBasicDescription)) {
        Log(L"AudioUnitSetProperty StreamFormat unit=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
            L" epoch=" + std::to_wstring(epoch) +
            L" tid=" + std::to_wstring(GetCurrentThreadId()) +
            L" qpc=" + std::to_wstring(CurrentQpc()) +
            L" scope=" + std::to_wstring(scope) +
            L" element=" + std::to_wstring(element) + L" " +
            AppleFormatText(static_cast<const AudioStreamBasicDescription*>(data)));
    }
    const AppleOSStatus result =
        g_originalAudioUnitSetProperty(targetUnit, propertyId, scope, element, data, dataSize);
    if (hardwareOutput && result == 0) {
        RememberHardwareOutputProperty(unit, propertyId, scope, element, data, dataSize);
    }
    if (hardwareOutput) {
        Log(L"Hardware output AudioUnitSetProperty result unit=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) + L" propertyId=" +
            std::to_wstring(propertyId) + L" scope=" + std::to_wstring(scope) +
            L" element=" + std::to_wstring(element) + L" result=" +
            std::to_wstring(result));
    }
    if (propertyId == streamFormatProperty) {
        Log(L"AudioUnitSetProperty StreamFormat unit=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
            L" epoch=" + std::to_wstring(epoch) +
            L" result=" + std::to_wstring(result));
    }
    if (propertyId == streamFormatProperty && result == 0 && scope == 1 &&
        data && dataSize >= sizeof(AudioStreamBasicDescription)) {
        const auto* streamFormat = static_cast<const AudioStreamBasicDescription*>(data);
        if (IsAppleLinearPcm(streamFormat) && streamFormat->mSampleRate >= 8000.0 &&
            streamFormat->mSampleRate <= 768000.0 && streamFormat->mChannelsPerFrame == 2) {
            const auto rate = static_cast<std::uint32_t>(streamFormat->mSampleRate + 0.5);
            const auto now = GetTickCount64();
            const bool hasFreshCandidate = g_threadGraphFormat.observedTick != 0 &&
                now - g_threadGraphFormat.observedTick <= 2000 &&
                g_threadGraphFormat.sampleRate == rate &&
                g_threadGraphFormat.channels == streamFormat->mChannelsPerFrame;
            constexpr std::uint32_t qlac = 0x716C6163; // 'qlac'
            const bool qlacCandidateMatches = hasFreshCandidate &&
                g_threadGraphFormat.encodedFormat == qlac;
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
            if (!sourceBitDepth && !hasFreshCandidate && ResolveUnambiguousQlacBitDepth(
                    rate, streamFormat->mChannelsPerFrame,
                    sourceBitDepth, observationId)) {
                binding = L"cross-thread-unambiguous";
            }
            RememberAudioUnitInputFormat(unit, *streamFormat, sourceBitDepth, observationId);
            Log(L"AudioUnit input format remembered unit=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
                L" epoch=" + std::to_wstring(epoch) + L" rate=" +
                std::to_wstring(rate) + L" sourceBitDepth=" +
                std::to_wstring(sourceBitDepth) + L" observation=" +
                std::to_wstring(observationId) + L" binding=" + binding);
        }
    }
    return result;
}

AppleOSStatus __cdecl HookAudioUnitGetProperty(void* unit, std::uint32_t propertyId,
                                                 std::uint32_t scope, std::uint32_t element,
                                                 void* data, std::uint32_t* dataSize) {
    constexpr std::uint32_t streamFormatProperty = 8;
    const auto epoch = EnsureAudioUnitEpoch(unit);
    void* const targetUnit = IsHardwareOutputUnit(unit)
        ? ResolveHardwareOutputUnit(unit) : unit;
    const AppleOSStatus result =
        g_originalAudioUnitGetProperty(targetUnit, propertyId, scope, element, data, dataSize);
    if (propertyId == streamFormatProperty && result == 0 && data && dataSize &&
        *dataSize >= sizeof(AudioStreamBasicDescription)) {
        const auto* streamFormat = static_cast<const AudioStreamBasicDescription*>(data);
        const auto rate = static_cast<std::uint64_t>(streamFormat->mSampleRate + 0.5);
        const std::uint64_t signature = rate ^ (static_cast<std::uint64_t>(streamFormat->mFormatID) << 20) ^
                                        (static_cast<std::uint64_t>(streamFormat->mFormatFlags) << 8) ^
                                        (static_cast<std::uint64_t>(scope) << 4) ^ element;
        if (g_lastAudioUnitGetFormatSignature.exchange(signature) != signature) {
            Log(L"AudioUnitGetProperty StreamFormat unit=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
                L" epoch=" + std::to_wstring(epoch) +
                L" tid=" + std::to_wstring(GetCurrentThreadId()) +
                L" qpc=" + std::to_wstring(CurrentQpc()) +
                L" scope=" + std::to_wstring(scope) +
                L" element=" + std::to_wstring(element) + L" " + AppleFormatText(streamFormat));
        }
    }
    return result;
}

AppleOSStatus ReplaceHardwareOutputInstance(
        void* logicalUnit, void* currentUnit, const ActiveGraphFormatCommit& commit,
        const AudioStreamBasicDescription& observedInputFormat) {
    (void)observedInputFormat;
    struct ScopedReplacementFormatOverride {
        HardwareOutputReplacementFormatOverride previous;
        ~ScopedReplacementFormatOverride() {
            g_hardwareOutputReplacementFormatOverride = previous;
        }
    } overrideScope{g_hardwareOutputReplacementFormatOverride};
    g_hardwareOutputReplacementFormatOverride = {
        commit.sampleRate, commit.channels, commit.sourceBitDepth, commit.sequence, true};
    HardwareOutputReplayState snapshot{};
    {
        std::lock_guard lock(g_audioUnitEpochMutex);
        const auto state = g_hardwareOutputReplayStates.find(logicalUnit);
        if (state == g_hardwareOutputReplayStates.end() || !state->second.component) {
            Log(L"Hardware output replacement aborted: logical instance has no component state unit=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(logicalUnit)));
            return -10868;
        }
        snapshot = state->second;
    }

    // Apple has already stopped the logical output before this cross-rate Start. Uninitialize the
    // current physical graph before creating its successor so stale WASAPI services are released,
    // but never dispose Apple's logical handle while its owner still retains it.
    const auto retireResult = g_originalAudioUnitUninitialize(currentUnit);
    Log(L"Hardware output replacement uninitialized previousPhysical=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(currentUnit)) + L" result=" +
        std::to_wstring(retireResult));
    if (retireResult != 0) return retireResult;

    void* freshUnit{};
    AppleOSStatus result = g_originalAudioComponentInstanceNew(snapshot.component, &freshUnit);
    if (result != 0 || !freshUnit) {
        Log(L"Hardware output replacement AudioComponentInstanceNew failed result=" +
            std::to_wstring(result));
        return result != 0 ? result : -10868;
    }
    {
        std::lock_guard lock(g_audioUnitEpochMutex);
        g_audioUnitDescriptions[freshUnit] = snapshot.description;
        g_hardwareOutputLogicalByPhysical[freshUnit] = logicalUnit;
    }
    const auto freshEpoch = EnsureAudioUnitEpoch(freshUnit);
    Log(L"Hardware output replacement created logical=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(logicalUnit)) + L" previousPhysical=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(currentUnit)) + L" freshPhysical=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(freshUnit)) + L" freshEpoch=" +
        std::to_wstring(freshEpoch) + L" targetRate=" + std::to_wstring(commit.sampleRate) +
        L" replayProperties=" + std::to_wstring(snapshot.properties.size()));

    bool initialized{};
    auto cleanupFresh = [&] {
        if (initialized) g_originalAudioUnitUninitialize(freshUnit);
        g_originalAudioComponentInstanceDispose(freshUnit);
        std::lock_guard lock(g_audioUnitEpochMutex);
        g_hardwareOutputLogicalByPhysical.erase(freshUnit);
        g_audioUnitDescriptions.erase(freshUnit);
        g_audioUnitEpochs.erase(freshUnit);
    };
    auto replayPhase = [&](bool afterInitialize) -> AppleOSStatus {
        constexpr std::uint32_t streamFormatProperty = 8;
        constexpr std::uint32_t inputScope = 1;
        for (const auto& property : snapshot.properties) {
            if (property.afterInitialize != afterInitialize || property.data.empty()) continue;
            std::vector<std::uint8_t> effectiveData = property.data;
            if (property.propertyId == streamFormatProperty && property.scope == inputScope &&
                effectiveData.size() >= sizeof(AudioStreamBasicDescription)) {
                auto* format = reinterpret_cast<AudioStreamBasicDescription*>(effectiveData.data());
                if (IsAppleLinearPcm(format) && format->mChannelsPerFrame == commit.channels) {
                    format->mSampleRate = static_cast<double>(commit.sampleRate);
                }
            }
            const auto propertyResult = g_originalAudioUnitSetProperty(
                freshUnit, property.propertyId, property.scope, property.element,
                effectiveData.data(), static_cast<std::uint32_t>(effectiveData.size()));
            Log(L"Hardware output replacement replay propertyId=" +
                std::to_wstring(property.propertyId) + L" scope=" +
                std::to_wstring(property.scope) + L" element=" +
                std::to_wstring(property.element) + L" afterInitialize=" +
                std::to_wstring(afterInitialize) + L" result=" +
                std::to_wstring(propertyResult));
            if (propertyResult != 0) return propertyResult;
        }
        return 0;
    };

    result = replayPhase(false);
    if (result != 0) {
        cleanupFresh();
        return result;
    }
    result = g_originalAudioUnitInitialize(freshUnit);
    initialized = result == 0;
    Log(L"Hardware output replacement initialize freshPhysical=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(freshUnit)) + L" result=" +
        std::to_wstring(result));
    if (result != 0) {
        cleanupFresh();
        return result;
    }
    result = replayPhase(true);
    if (result != 0) {
        cleanupFresh();
        return result;
    }

    g_refreshedOutputCommitSequence.store(commit.sequence);
    result = g_originalAudioOutputUnitStart(freshUnit);
    Log(L"Hardware output replacement start freshPhysical=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(freshUnit)) + L" targetRate=" +
        std::to_wstring(commit.sampleRate) + L" result=" + std::to_wstring(result));
    if (result != 0) {
        g_refreshedOutputCommitSequence.store(0);
        cleanupFresh();
        return result;
    }

    void* retiredReplacement{};
    bool stateMissing{};
    {
        std::lock_guard lock(g_audioUnitEpochMutex);
        const auto state = g_hardwareOutputReplayStates.find(logicalUnit);
        if (state == g_hardwareOutputReplayStates.end()) {
            stateMissing = true;
        } else {
            retiredReplacement = state->second.replacement;
            state->second.replacement = freshUnit;
            state->second.initialized = true;
        }
    }
    if (stateMissing) {
        g_originalAudioOutputUnitStop(freshUnit);
        cleanupFresh();
        return -10868;
    }
    if (retiredReplacement && retiredReplacement != freshUnit) {
        g_originalAudioComponentInstanceDispose(retiredReplacement);
        std::lock_guard lock(g_audioUnitEpochMutex);
        g_hardwareOutputLogicalByPhysical.erase(retiredReplacement);
        g_audioUnitDescriptions.erase(retiredReplacement);
        g_audioUnitEpochs.erase(retiredReplacement);
    }
    Log(L"Hardware output replacement committed logical=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(logicalUnit)) + L" physical=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(freshUnit)) + L" rate=" +
        std::to_wstring(commit.sampleRate) + L" commitSequence=" +
        std::to_wstring(commit.sequence));
    return 0;
}

AppleOSStatus __cdecl HookAudioOutputUnitStart(void* unit) {
    // Shared playback must retain Apple's entire output contract, not just its WASAPI tuple.
    // Even GetProperty below may lazily construct a converter; bypass all exclusive work.
    if (!ExclusiveRequested()) {
        Log(L"AudioOutputUnitStart shared passthrough; no output contract mutation");
        return g_originalAudioOutputUnitStart(unit);
    }
    const auto epoch = EnsureAudioUnitEpoch(unit);
    ActiveGraphFormatCommit commit{};
    bool provisionalLocalCommit{};
    const auto now = GetTickCount64();
    if (ExclusiveRequested() && g_threadGraphFormat.observedTick != 0 &&
        now - g_threadGraphFormat.observedTick <= 500 &&
        g_threadGraphFormat.encodedFormat == ammod::audio::kLinearPcm &&
        g_threadGraphFormat.sampleRate >= 8000 &&
        g_threadGraphFormat.sampleRate <= 768000 &&
        g_threadGraphFormat.channels == 2 &&
        IsSupportedSourceBitDepth(g_threadGraphFormat.sourceBitDepth)) {
        commit.sampleRate = g_threadGraphFormat.sampleRate;
        commit.channels = g_threadGraphFormat.channels;
        commit.sourceBitDepth = g_threadGraphFormat.sourceBitDepth;
        commit.committedTick = now;
        commit.sequence = g_activeGraphFormatSequence.fetch_add(1) + 1;
        commit.unit = unit;
        commit.unitEpoch = epoch;
        commit.localPcm = true;
        {
            std::lock_guard lock(g_activeGraphFormatMutex);
            g_activeGraphFormat = commit;
        }
        provisionalLocalCommit = true;
        Log(L"Active graph format provisionally committed by local LPCM OutputUnitStart rate=" +
            std::to_wstring(commit.sampleRate) + L" sequence=" +
            std::to_wstring(commit.sequence) + L" unit=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) + L" epoch=" +
            std::to_wstring(epoch) + L" sourceBitDepth=" +
            std::to_wstring(commit.sourceBitDepth) + L" observation=" +
            std::to_wstring(g_threadGraphFormat.observationId));
    }
    {
        std::lock_guard lock(g_activeGraphFormatMutex);
        if (!provisionalLocalCommit) commit = g_activeGraphFormat;
    }
    constexpr std::uint32_t streamFormatProperty = 8;
    constexpr std::uint32_t inputScope = 1;
    AudioStreamBasicDescription outputInputFormat{};
    std::uint32_t outputInputFormatSize = sizeof(outputInputFormat);
    void* const targetUnit = IsHardwareOutputUnit(unit)
        ? ResolveHardwareOutputUnit(unit) : unit;
    const AppleOSStatus formatRead = g_originalAudioUnitGetProperty
        ? g_originalAudioUnitGetProperty(targetUnit, streamFormatProperty, inputScope, 0,
                                          &outputInputFormat, &outputInputFormatSize)
        : -1;
    const bool commitIsFresh = commit.committedTick != 0 &&
        GetTickCount64() - commit.committedTick <= 500 && commit.channels == 2;
    if (commitIsFresh && commit.localPcm) {
        std::shared_ptr<LocalPcmQueue> queue;
        {
            std::lock_guard lock(g_converterProbeMutex);
            const auto found = g_converterProbes.find(g_threadLocalPcmSourceConverter);
            if (found != g_converterProbes.end()) queue = found->second.localQueue;
        }
        if (queue) {
            queue->Activate();
            std::lock_guard lock(g_activeLocalQueueMutex);
            if (g_activeLocalQueue && g_activeLocalQueue != queue) g_activeLocalQueue->Abandon();
            g_activeLocalQueue = queue;
            g_refreshedOutputCommitSequence.store(commit.sequence);
            Log(L"Local PCM source queue activated converter=" + std::to_wstring(
                reinterpret_cast<std::uintptr_t>(g_threadLocalPcmSourceConverter)) +
                L" rate=" + std::to_wstring(queue->sampleRate) + L" bits=" +
                std::to_wstring(queue->bitsPerChannel) + L" bytesPerFrame=" +
                std::to_wstring(queue->bytesPerFrame) +
                L"; Apple output graph rate left unchanged for frame bridge");
        }
    } else if (formatRead == 0 && outputInputFormatSize >= sizeof(outputInputFormat) &&
        commitIsFresh && IsAppleLinearPcm(&outputInputFormat) &&
        static_cast<std::uint32_t>(outputInputFormat.mSampleRate + 0.5) != commit.sampleRate) {
        const auto previousRate = static_cast<std::uint32_t>(outputInputFormat.mSampleRate + 0.5);
        const bool reusedStreamingOutput = !commit.localPcm &&
            g_currentAudioUnit.load() == unit;
        if (reusedStreamingOutput) {
            DWORD recoveryHelperPid{};
            const bool recoveryScheduled = LaunchMediaRoundTripAfterFailedNaturalTransition(
                commit.sequence, recoveryHelperPid);
            Log(L"AudioOutputUnitStart fail-closed because upstream overlap gate did not rebuild "
                L"the cross-rate output unit=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) + L" epoch=" +
                std::to_wstring(epoch) + L" oldRate=" + std::to_wstring(previousRate) +
                L" newRate=" + std::to_wstring(commit.sampleRate) +
                L" commitSequence=" + std::to_wstring(commit.sequence) +
                L" recoveryScheduled=" + std::to_wstring(recoveryScheduled) +
                L" recoveryHelperPid=" + std::to_wstring(recoveryHelperPid) +
                L" stack=" + CaptureCurrentStackText());
            return -10868;
        }
        outputInputFormat.mSampleRate = static_cast<double>(commit.sampleRate);
        const AppleOSStatus formatWrite = g_originalAudioUnitSetProperty(
            unit, streamFormatProperty, inputScope, 0, &outputInputFormat,
            sizeof(outputInputFormat));
        Log(L"AudioOutputUnitStart refreshed output contract from active graph commit unit=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) + L" epoch=" +
            std::to_wstring(epoch) + L" oldRate=" + std::to_wstring(previousRate) +
            L" newRate=" + std::to_wstring(commit.sampleRate) + L" commitSequence=" +
            std::to_wstring(commit.sequence) + L" commitUnit=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(commit.unit)) + L" result=" +
            std::to_wstring(formatWrite));
        if (formatWrite == 0) g_refreshedOutputCommitSequence.store(commit.sequence);
    } else if (formatRead == 0 && outputInputFormatSize >= sizeof(outputInputFormat) &&
               commitIsFresh && IsAppleLinearPcm(&outputInputFormat) &&
               static_cast<std::uint32_t>(outputInputFormat.mSampleRate + 0.5) == commit.sampleRate) {
        g_refreshedOutputCommitSequence.store(commit.sequence);
        Log(L"AudioOutputUnitStart output contract already matches active graph commit unit=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) + L" epoch=" +
            std::to_wstring(epoch) + L" rate=" + std::to_wstring(commit.sampleRate) +
            L" commitSequence=" + std::to_wstring(commit.sequence));
    }
    g_currentAudioUnit.store(unit);
    Log(L"AudioOutputUnitStart entered unit=" + std::to_wstring(
        reinterpret_cast<std::uintptr_t>(unit)) + L" epoch=" + std::to_wstring(epoch) +
        L" tid=" + std::to_wstring(GetCurrentThreadId()) +
        L" qpc=" + std::to_wstring(CurrentQpc()));
    const AppleOSStatus result = g_originalAudioOutputUnitStart(targetUnit);
    Log(L"AudioOutputUnitStart result=" + std::to_wstring(result));
    if (result != 0 && provisionalLocalCommit) {
        std::lock_guard lock(g_activeGraphFormatMutex);
        if (g_activeGraphFormat.sequence == commit.sequence) g_activeGraphFormat = {};
        if (g_refreshedOutputCommitSequence.load() == commit.sequence) {
            g_refreshedOutputCommitSequence.store(0);
        }
        Log(L"Local LPCM provisional graph commit rolled back sequence=" +
            std::to_wstring(commit.sequence));
    }
    return result;
}

bool ExclusiveRequested();

AppleOSStatus __cdecl HookAudioOutputUnitStop(void* unit) {
    const auto epoch = FindAudioUnitEpoch(unit);
    void* const targetUnit = IsHardwareOutputUnit(unit)
        ? ResolveHardwareOutputUnit(unit) : unit;
    const AppleOSStatus result = g_originalAudioOutputUnitStop(targetUnit);
    {
        std::lock_guard lock(g_activeLocalQueueMutex);
        if (g_activeLocalQueue) {
            g_activeLocalQueue->FlushForSeek();
            Log(L"AudioOutputUnitStop flushed local PCM queue for seek/pause while keeping "
                L"the current converter writable");
        }
    }
    Log(L"AudioOutputUnitStop unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
        L" epoch=" + std::to_wstring(epoch) +
        L" result=" + std::to_wstring(result));
    return result;
}

AppleOSStatus __cdecl HookAudioUnitInitialize(void* unit) {
    const auto epoch = EnsureAudioUnitEpoch(unit);
    void* const logicalOutput = LogicalHardwareOutputUnit(unit);
    void* const targetUnit = logicalOutput ? ResolveHardwareOutputUnit(unit) : unit;
    Log(L"AudioUnitInitialize entered unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
        L" epoch=" + std::to_wstring(epoch) +
        L" tid=" + std::to_wstring(GetCurrentThreadId()) +
        L" qpc=" + std::to_wstring(CurrentQpc()));
    const AppleOSStatus result = g_originalAudioUnitInitialize(targetUnit);
    Log(L"AudioUnitInitialize unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
        L" epoch=" + std::to_wstring(epoch) + L" result=" + std::to_wstring(result));
    if (result == 0) {
        if (logicalOutput) {
            std::lock_guard lock(g_audioUnitEpochMutex);
            const auto state = g_hardwareOutputReplayStates.find(logicalOutput);
            if (state != g_hardwareOutputReplayStates.end()) state->second.initialized = true;
        }
        CommitInitializedAudioUnitFormat(unit, epoch);
    }
    return result;
}

AppleOSStatus __cdecl HookAudioUnitUninitialize(void* unit) {
    const auto epoch = FindAudioUnitEpoch(unit);
    void* const logicalOutput = LogicalHardwareOutputUnit(unit);
    void* const targetUnit = logicalOutput ? ResolveHardwareOutputUnit(unit) : unit;
    const AppleOSStatus result = g_originalAudioUnitUninitialize(targetUnit);
    Log(L"AudioUnitUninitialize unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
        L" epoch=" + std::to_wstring(epoch) +
        L" result=" + std::to_wstring(result));
    if (logicalOutput && result == 0) {
        std::lock_guard lock(g_audioUnitEpochMutex);
        const auto state = g_hardwareOutputReplayStates.find(logicalOutput);
        if (state != g_hardwareOutputReplayStates.end()) state->second.initialized = false;
    }
    RetireAudioUnitEpoch(unit);
    return result;
}

AppleOSStatus __cdecl HookAudioUnitRender(void* unit, std::uint32_t* actionFlags,
                                          const void* timestamp, std::uint32_t bus,
                                          std::uint32_t frames, void* buffers) {
    AppleAudioTimeStamp observedTimestamp{};
    AppleAudioTimeStamp effectiveTimestamp{};
    const bool hasTimestamp = timestamp != nullptr;
    if (timestamp) {
        std::memcpy(&observedTimestamp, timestamp, sizeof(observedTimestamp));
        effectiveTimestamp = observedTimestamp;
        std::lock_guard lock(g_audioUnitRenderTemplateMutex);
        g_audioUnitRenderTimestamp = effectiveTimestamp;
        g_audioUnitRenderBus = bus;
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        g_audioUnitRenderTemplateQpc = qpc.QuadPart;
        g_hasAudioUnitRenderTemplate = true;
    }
    const AppleOSStatus result =
        g_originalAudioUnitRender(IsHardwareOutputUnit(unit)
            ? ResolveHardwareOutputUnit(unit) : unit,
            actionFlags, timestamp, bus, frames, buffers);
    const auto call = g_audioUnitRenderCalls.fetch_add(1) + 1;
    if (call <= 8 || result != 0) {
        Log(L"AudioUnitRender call=" + std::to_wstring(call) +
            L" frames=" + std::to_wstring(frames) +
            L" rawSampleTime=" + (hasTimestamp
                ? std::to_wstring(observedTimestamp.mSampleTime) : L"none") +
            L" effectiveSampleTime=" + (hasTimestamp
                ? std::to_wstring(effectiveTimestamp.mSampleTime) : L"none") +
            L" timestampBridged=0" +
            L" hostTime=" + (hasTimestamp
                ? std::to_wstring(observedTimestamp.mHostTime) : L"none") +
            L" rateScalar=" + (hasTimestamp
                ? std::to_wstring(observedTimestamp.mRateScalar) : L"none") +
            L" timestampFlags=0x" + (hasTimestamp
                ? HResultText(static_cast<HRESULT>(observedTimestamp.mFlags)).substr(2) : L"none") +
            L" result=" + std::to_wstring(result));
    }
    return result;
}

AppleOSStatus __cdecl HookAudioUnitAddRenderNotify(void* unit, void* callback, void* refCon) {
    const AppleOSStatus result = g_originalAudioUnitAddRenderNotify(
        IsHardwareOutputUnit(unit) ? ResolveHardwareOutputUnit(unit) : unit,
        callback, refCon);
    Log(L"AudioUnitAddRenderNotify unit=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) + L" epoch=" +
        std::to_wstring(EnsureAudioUnitEpoch(unit)) + L" hardwareOutput=" +
        std::to_wstring(IsHardwareOutputUnit(unit)) + L" callback=" + std::to_wstring(
            reinterpret_cast<std::uintptr_t>(callback)) +
        L" refCon=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(refCon)) +
        L" result=" + std::to_wstring(result) + L" stack=" + CaptureCurrentStackText());
    return result;
}

AppleOSStatus __cdecl HookAudioComponentInstanceNew(void* component, void** instance) {
    AppleAudioComponentDescription description{};
    const AppleOSStatus descriptionResult = g_audioComponentGetDescription
        ? g_audioComponentGetDescription(component, &description) : -1;
    const AppleOSStatus result = g_originalAudioComponentInstanceNew(component, instance);
    if (result == 0 && instance && *instance) {
        std::lock_guard lock(g_audioUnitEpochMutex);
        g_audioUnitDescriptions[*instance] = description;
        if (IsHardwareOutputDescription(description)) {
            HardwareOutputReplayState state{};
            state.component = component;
            state.description = description;
            g_hardwareOutputReplayStates[*instance] = std::move(state);
        }
    }
    Log(L"AudioComponentInstanceNew component=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(component)) +
        L" instance=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(
            instance ? *instance : nullptr)) +
        L" descriptionResult=" + std::to_wstring(descriptionResult) + L" " +
        ComponentDescriptionText(description) + L" result=" + std::to_wstring(result) +
        (IsHardwareOutputDescription(description)
            ? L" stack=" + CaptureCurrentStackText() : L""));
    return result;
}

AppleOSStatus __cdecl HookAudioComponentInstanceDispose(void* instance) {
    const auto epoch = FindAudioUnitEpoch(instance);
    AppleAudioComponentDescription description{};
    bool descriptionKnown{};
    void* replacement{};
    bool logicalHardwareOutput{};
    {
        std::lock_guard lock(g_audioUnitEpochMutex);
        const auto found = g_audioUnitDescriptions.find(instance);
        if (found != g_audioUnitDescriptions.end()) {
            description = found->second;
            descriptionKnown = true;
        }
        const auto state = g_hardwareOutputReplayStates.find(instance);
        if (state != g_hardwareOutputReplayStates.end()) {
            logicalHardwareOutput = true;
            replacement = state->second.replacement;
        }
    }
    AppleOSStatus replacementResult{};
    if (replacement && replacement != instance) {
        g_originalAudioUnitUninitialize(replacement);
        replacementResult = g_originalAudioComponentInstanceDispose(replacement);
    }
    const AppleOSStatus result = g_originalAudioComponentInstanceDispose(instance);
    Log(L"AudioComponentInstanceDispose instance=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(instance)) +
        L" epoch=" + std::to_wstring(epoch) +
        L" descriptionKnown=" + std::to_wstring(descriptionKnown) +
        (descriptionKnown ? L" " + ComponentDescriptionText(description) : L"") +
        L" replacement=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(replacement)) +
        L" replacementResult=" + std::to_wstring(replacementResult) +
        L" result=" + std::to_wstring(result) +
        (descriptionKnown && IsHardwareOutputDescription(description)
            ? L" stack=" + CaptureCurrentStackText() : L""));
    if (result == 0) {
        std::lock_guard lock(g_audioUnitEpochMutex);
        if (logicalHardwareOutput) g_hardwareOutputReplayStates.erase(instance);
        if (replacement) {
            g_hardwareOutputLogicalByPhysical.erase(replacement);
            g_audioUnitDescriptions.erase(replacement);
            g_audioUnitEpochs.erase(replacement);
        }
        g_hardwareOutputLogicalByPhysical.erase(instance);
        g_audioUnitDescriptions.erase(instance);
        g_audioUnitEpochs.erase(instance);
    }
    return result;
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

#if 0 // Retired v1 audio-core installation path; v2 is installed below.
[[maybe_unused]] bool InstallLegacyAudioCoreHooksDisabled() {
    HMODULE toolbox = GetModuleHandleW(L"CoreAudioToolbox.dll");
    HMODULE avFoundation = GetModuleHandleW(L"AVFoundationCF.dll");
    HMODULE coreFoundation = GetModuleHandleW(L"CoreFoundation.dll");
    HMODULE coreMedia = GetModuleHandleW(L"CoreMedia.dll");
    if (!toolbox || !avFoundation || !coreFoundation || !coreMedia) return false;
    if (!g_toolboxBase.load()) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(toolbox);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const BYTE*>(toolbox) + dos->e_lfanew);
        g_toolboxBase.store(reinterpret_cast<std::uintptr_t>(toolbox));
        g_toolboxSize.store(nt->OptionalHeader.SizeOfImage);
    }

    auto install = [toolbox](const char* exportName, void* detour, auto& original,
                             const wchar_t* label) {
        void* target = reinterpret_cast<void*>(GetProcAddress(toolbox, exportName));
        return target && HookAddress(target, detour, original, label);
    };

    // The Agent can construct its long-lived hardware output while the remaining hooks are still
    // being installed. Observe component identity first so every later property call can be bound
    // to the exact auou/ahlw instance instead of racing an untyped handle.
    g_audioComponentGetDescription = reinterpret_cast<AudioComponentGetDescriptionFn>(
        GetProcAddress(toolbox, "AudioComponentGetDescription"));
    const bool componentNew = install(
        "AudioComponentInstanceNew", reinterpret_cast<void*>(&HookAudioComponentInstanceNew),
        g_originalAudioComponentInstanceNew, L"AudioComponentInstanceNew");
    const bool componentDispose = install(
        "AudioComponentInstanceDispose", reinterpret_cast<void*>(&HookAudioComponentInstanceDispose),
        g_originalAudioComponentInstanceDispose, L"AudioComponentInstanceDispose");

    const bool converter = install("AudioConverterNew", reinterpret_cast<void*>(&HookAudioConverterNew),
                                   g_originalAudioConverterNew, L"AudioConverterNew");
    const bool converterSpecific = install(
        "AudioConverterNewSpecific", reinterpret_cast<void*>(&HookAudioConverterNewSpecific),
        g_originalAudioConverterNewSpecific, L"AudioConverterNewSpecific");
    const bool converterSetProperty = install(
        "AudioConverterSetProperty", reinterpret_cast<void*>(&HookAudioConverterSetProperty),
        g_originalAudioConverterSetProperty, L"AudioConverterSetProperty");
    const bool converterGetProperty = install(
        "AudioConverterGetProperty", reinterpret_cast<void*>(&HookAudioConverterGetProperty),
        g_originalAudioConverterGetProperty, L"AudioConverterGetProperty");
    const bool converterFill = install(
        "AudioConverterFillComplexBuffer",
        reinterpret_cast<void*>(&HookAudioConverterFillComplexBuffer),
        g_originalAudioConverterFillComplexBuffer, L"AudioConverterFillComplexBuffer");
    const bool converterReset = install(
        "AudioConverterReset", reinterpret_cast<void*>(&HookAudioConverterReset),
        g_originalAudioConverterReset, L"AudioConverterReset");
    const bool converterDispose = install(
        "AudioConverterDispose", reinterpret_cast<void*>(&HookAudioConverterDispose),
        g_originalAudioConverterDispose, L"AudioConverterDispose");
    const bool setProperty = install("AudioUnitSetProperty",
                                     reinterpret_cast<void*>(&HookAudioUnitSetProperty),
                                     g_originalAudioUnitSetProperty, L"AudioUnitSetProperty");
    const bool getProperty = install("AudioUnitGetProperty",
                                     reinterpret_cast<void*>(&HookAudioUnitGetProperty),
                                     g_originalAudioUnitGetProperty, L"AudioUnitGetProperty");
    const bool outputStart = install("AudioOutputUnitStart",
                                     reinterpret_cast<void*>(&HookAudioOutputUnitStart),
                                     g_originalAudioOutputUnitStart, L"AudioOutputUnitStart");
    const bool outputStop = install("AudioOutputUnitStop",
                                    reinterpret_cast<void*>(&HookAudioOutputUnitStop),
                                    g_originalAudioOutputUnitStop, L"AudioOutputUnitStop");
    const bool unitInitialize = install("AudioUnitInitialize",
                                        reinterpret_cast<void*>(&HookAudioUnitInitialize),
                                        g_originalAudioUnitInitialize, L"AudioUnitInitialize");
    const bool unitUninitialize = install("AudioUnitUninitialize",
                                          reinterpret_cast<void*>(&HookAudioUnitUninitialize),
                                          g_originalAudioUnitUninitialize, L"AudioUnitUninitialize");
    const bool unitRender = install("AudioUnitRender", reinterpret_cast<void*>(&HookAudioUnitRender),
                                    g_originalAudioUnitRender, L"AudioUnitRender");
    const bool renderNotify = install("AudioUnitAddRenderNotify",
                                      reinterpret_cast<void*>(&HookAudioUnitAddRenderNotify),
                                      g_originalAudioUnitAddRenderNotify, L"AudioUnitAddRenderNotify");
    void* waitTarget = reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WaitForSingleObject"));
    const bool waitHook = waitTarget && HookAddress(
        waitTarget, reinterpret_cast<void*>(&HookWaitForSingleObject),
        g_originalWaitForSingleObject, L"WaitForSingleObject");
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
    // Apple Music 1.6.4.90 / CoreMedia 1.1540.23042.0 x64. Local-file scrubbing
    // bypasses the public IMFMediaEngine and AVCF seek exports and enters this private
    // itemasync dispatcher. Fail closed on any package drift before touching the RVA.
    auto* coreMediaSetCurrentTime = reinterpret_cast<BYTE*>(coreMedia) +
        ammod::apple_private::core_media_seek::kSetCurrentTimeRva;
    const bool coreMediaSeekSignature = std::memcmp(
        coreMediaSetCurrentTime,
        ammod::apple_private::core_media_seek::kSetCurrentTimePrologue.data(),
        ammod::apple_private::core_media_seek::kSetCurrentTimePrologue.size()) == 0;
    const bool coreMediaSeek = coreMediaSeekSignature && HookAddress(
        coreMediaSetCurrentTime,
        reinterpret_cast<void*>(&HookCoreMediaInternalSetCurrentTime),
        g_originalCoreMediaInternalSetCurrentTime,
        L"CoreMedia private itemasync_SetCurrentTime");
    if (!coreMediaSeekSignature) {
        Log(L"CoreMedia private itemasync_SetCurrentTime signature mismatch; seek hook disabled");
    }
    g_cfGetTypeID = reinterpret_cast<CFGetTypeIDFn>(
        GetProcAddress(coreFoundation, "CFGetTypeID"));
    g_cfArrayGetCount = reinterpret_cast<CFArrayGetCountFn>(
        GetProcAddress(coreFoundation, "CFArrayGetCount"));
    g_cfArrayGetValueAtIndex = reinterpret_cast<CFArrayGetValueAtIndexFn>(
        GetProcAddress(coreFoundation, "CFArrayGetValueAtIndex"));
    g_cfDictionaryGetValue = reinterpret_cast<CFDictionaryGetValueFn>(
        GetProcAddress(coreFoundation, "CFDictionaryGetValue"));
    g_cfNumberGetValue = reinterpret_cast<CFNumberGetValueFn>(
        GetProcAddress(coreFoundation, "CFNumberGetValue"));
    g_cfArrayGetTypeID = reinterpret_cast<CFTypeIdFn>(
        GetProcAddress(coreFoundation, "CFArrayGetTypeID"));
    g_cfDictionaryGetTypeID = reinterpret_cast<CFTypeIdFn>(
        GetProcAddress(coreFoundation, "CFDictionaryGetTypeID"));
    g_cfNumberGetTypeID = reinterpret_cast<CFTypeIdFn>(
        GetProcAddress(coreFoundation, "CFNumberGetTypeID"));
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
    if (g_cfStringCreateWithCString) {
        g_streamQualityPreferenceKey = g_cfStringCreateWithCString(
            nullptr, "preferredStreamPlaybackAudioQuality", cfStringEncodingUtf8);
        g_losslessEnabledPreferenceKey = g_cfStringCreateWithCString(
            nullptr, "losslessEnabled", cfStringEncodingUtf8);
    }
    g_figAlternateAllowableMediaSubtypeFilterCreate =
        reinterpret_cast<FigAlternateAllowableMediaSubtypeFilterCreateFn>(
            GetProcAddress(coreMedia, "FigAlternateAllowableMediaSubtypeFilterCreate"));
    auto exportedCfObject = [avFoundation](const char* exportName) -> void* {
        void* address = reinterpret_cast<void*>(GetProcAddress(avFoundation, exportName));
        return address ? *reinterpret_cast<void**>(address) : nullptr;
    };
    g_avcfSampleRateKey = exportedCfObject("AVCFSampleRateKey");
    g_avcfLinearPcmBitDepthKey = exportedCfObject("AVCFLinearPCMBitDepthKey");
    g_avcfNumberOfChannelsKey = exportedCfObject("AVCFNumberOfChannelsKey");
    g_avcfFormatIdKey = exportedCfObject("AVCFFormatIDKey");
    g_setAVCFExclusiveOptimization = reinterpret_cast<AVCFSetExclusiveOptimizationFn>(
        GetProcAddress(avFoundation, "AVCFPlayerItemSetShouldOptimizeForExclusivePlayback"));
    g_getAVCFExclusiveOptimization = reinterpret_cast<AVCFGetExclusiveOptimizationFn>(
        GetProcAddress(avFoundation, "AVCFPlayerItemShouldOptimizeForExclusivePlayback"));
    const bool audioTap = installAVCF(
        "AVCFPlayerItemSetAudioTapProcessor", reinterpret_cast<void*>(&HookAVCFSetAudioTapProcessor),
        g_originalAVCFSetAudioTapProcessor, L"AVCFPlayerItemSetAudioTapProcessor");
    const bool preferredRate = installAVCF(
        "AVCFPlayerItemSetPreferredMaximumAudioSampleRate",
        reinterpret_cast<void*>(&HookAVCFSetPreferredMaximumSampleRate),
        g_originalAVCFSetPreferredMaximumSampleRate,
        L"AVCFPlayerItemSetPreferredMaximumAudioSampleRate");
    const bool variantPreferences = installAVCF(
        "AVCFPlayerItemSetVariantPreferences",
        reinterpret_cast<void*>(&HookAVCFSetVariantPreferences),
        g_originalAVCFSetVariantPreferences,
        L"AVCFPlayerItemSetVariantPreferences");
    const bool trackFormats = installAVCF(
        "AVCFAssetTrackCopyFormatDescriptions",
        reinterpret_cast<void*>(&HookAVCFAssetTrackCopyFormatDescriptions),
        g_originalAVCFAssetTrackCopyFormatDescriptions,
        L"AVCFAssetTrackCopyFormatDescriptions");
    const bool losslessPreference = installCoreMedia(
        "FigAlternateLosslessAudioPreferenceFilterCreate",
        reinterpret_cast<void*>(&HookFigAlternateLosslessPreferenceFilterCreate),
        g_originalFigAlternateLosslessPreferenceFilterCreate,
        L"FigAlternateLosslessAudioPreferenceFilterCreate");
    const bool highestLossless = ammod::quality::Install(coreMedia, coreFoundation, Log);
    if (!highestLossless) Log(L"Highest ALAC quality filter unavailable; tier 20 admission will fail closed");
    const bool eligibleLossless = installCoreMedia(
        "FigAlternateEligibleLosslessAudioFilterCreate",
        reinterpret_cast<void*>(&HookFigAlternateEligibleLosslessFilterCreate),
        g_originalFigAlternateEligibleLosslessFilterCreate,
        L"FigAlternateEligibleLosslessAudioFilterCreate");
    const bool lossyPreference = installCoreMedia(
        "FigAlternateLossyAudioPreferenceFilterCreate",
        reinterpret_cast<void*>(&HookFigAlternateLossyPreferenceFilterCreate),
        g_originalFigAlternateLossyPreferenceFilterCreate,
        L"FigAlternateLossyAudioPreferenceFilterCreate");
    const bool hasLossless = installCoreMedia(
        "FigAlternateHasLosslessAudio",
        reinterpret_cast<void*>(&HookFigAlternateHasLosslessAudio),
        g_originalFigAlternateHasLosslessAudio,
        L"FigAlternateHasLosslessAudio");
    const bool playbackBitrateMonitor = installCoreMedia(
        "FigAlternateFilterMonitorCreateForPlaybackBitrate",
        reinterpret_cast<void*>(&HookFigAlternatePlaybackBitrateMonitorCreate),
        g_originalFigAlternatePlaybackBitrateMonitorCreate,
        L"FigAlternateFilterMonitorCreateForPlaybackBitrate");
    const bool filterTreeFallback = installCoreMedia(
        "FigAlternateFilterTreeSetFallbackBranch",
        reinterpret_cast<void*>(&HookFigAlternateFilterTreeSetFallbackBranch),
        g_originalFigAlternateFilterTreeSetFallbackBranch,
        L"FigAlternateFilterTreeSetFallbackBranch");
    const bool cfMetadataReady = g_cfGetTypeID && g_cfArrayGetCount &&
        g_cfArrayGetValueAtIndex && g_cfDictionaryGetValue && g_cfNumberGetValue &&
        g_cfArrayGetTypeID && g_cfDictionaryGetTypeID && g_cfNumberGetTypeID &&
        g_avcfSampleRateKey && g_avcfLinearPcmBitDepthKey &&
        g_avcfNumberOfChannelsKey && g_avcfFormatIdKey && g_cfNumberCreate &&
        g_cfArrayCreate && g_cfRelease && g_cfTypeArrayCallbacks &&
        g_figAlternateAllowableMediaSubtypeFilterCreate &&
        g_cfPreferencesGetAppIntegerValue && g_cfPreferencesGetAppBooleanValue &&
        g_cfPreferencesCurrentApplication && g_streamQualityPreferenceKey &&
        g_losslessEnabledPreferenceKey;
    return converter && converterSpecific && converterSetProperty && converterGetProperty &&
           converterFill && converterReset && converterDispose &&
           setProperty && getProperty && outputStart && outputStop &&
           unitInitialize && unitUninitialize && unitRender && renderNotify &&
           componentNew && componentDispose && g_audioComponentGetDescription &&
           waitHook && audioTap && preferredRate && variantPreferences && trackFormats &&
           coreMediaSeek &&
           cfMetadataReady && losslessPreference && eligibleLossless && lossyPreference &&
           hasLossless && playbackBitrateMonitor && filterTreeFallback &&
           g_setAVCFExclusiveOptimization && g_getAVCFExclusiveOptimization;
}
#endif

void ReportSuccessfulRenderAfterStart();
void CheckExclusiveRenderTimeout();
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

class RetirableService {
public:
    virtual void Retire() = 0;
    virtual HRESULT Rebind(IAudioClient* client) = 0;
    virtual void SwitchLocalQueue(const std::shared_ptr<LocalPcmQueue>&) {}
    virtual IUnknown* Unknown() = 0;
protected:
    virtual ~RetirableService() = default;
};

class RenderClientProxy final : public IAudioRenderClient, public RetirableService {
public:
    RenderClientProxy(IAudioRenderClient* inner,
                      std::atomic<std::uint32_t>* conversionSourceBitDepth,
                      std::atomic<std::uint16_t>* conversionChannels,
                      std::atomic<std::uint16_t>* conversionContainerBits,
                      std::atomic<std::uint32_t>* frameBridgeAppleRate,
                       std::atomic<std::uint32_t>* frameBridgeDeviceRate,
                       std::shared_ptr<LocalPcmQueue> localQueue)
        : inner_(inner), conversionSourceBitDepth_(conversionSourceBitDepth),
          conversionChannels_(conversionChannels),
           conversionContainerBits_(conversionContainerBits),
           frameBridgeAppleRate_(frameBridgeAppleRate),
           frameBridgeDeviceRate_(frameBridgeDeviceRate),
           localQueue_(std::move(localQueue)) {}

    void Retire() override {
        if (auto* retired = inner_.exchange(nullptr)) retired->Release();
    }
    HRESULT Rebind(IAudioClient* client) override {
        if (!client) return E_POINTER;
        IAudioRenderClient* replacement{};
        const HRESULT hr = client->GetService(__uuidof(IAudioRenderClient),
                                              reinterpret_cast<void**>(&replacement));
        if (FAILED(hr) || !replacement) return hr;
        if (auto* retired = inner_.exchange(replacement)) retired->Release();
        return S_OK;
    }
    void SwitchLocalQueue(const std::shared_ptr<LocalPcmQueue>& queue) override {
        std::lock_guard lock(queueMutex_);
        localQueue_ = queue;
        frameBridgeLogged_.store(false);
        frameBridgeUnderrunLogged_.store(false);
        frameBridgeDrainLogged_.store(false);
    }
    IUnknown* Unknown() override { return static_cast<IAudioRenderClient*>(this); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioRenderClient)) {
            *object = static_cast<IAudioRenderClient*>(this);
            AddRef();
            return S_OK;
        }
        auto* inner = inner_.load();
        return inner ? inner->QueryInterface(iid, object) : E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 requestedFrames, BYTE** data) override {
        if (!data) return E_POINTER;
        *data = nullptr;
        auto* inner = inner_.load();
        const auto appleRate = frameBridgeAppleRate_ ? frameBridgeAppleRate_->load() : 0;
        const auto deviceRate = frameBridgeDeviceRate_ ? frameBridgeDeviceRate_->load() : 0;
        std::shared_ptr<LocalPcmQueue> localQueue;
        {
            std::lock_guard lock(queueMutex_);
            localQueue = localQueue_;
            pendingLocalQueue_ = localQueue;
        }
        const bool frameBridge = appleRate && deviceRate && localQueue;
        const UINT32 deviceFrames = frameBridge
            ? ammod::audio::ScaleFrameCount(requestedFrames, appleRate, deviceRate)
            : requestedFrames;
        BYTE* deviceBuffer{};
        HRESULT result = inner ? inner->GetBuffer(deviceFrames, &deviceBuffer) : E_UNEXPECTED;
        const auto sourceBitDepth = conversionSourceBitDepth_
            ? conversionSourceBitDepth_->load() : 0;
        const std::uint16_t channels = conversionChannels_
            ? conversionChannels_->load() : std::uint16_t{};
        const auto containerBits = conversionContainerBits_
            ? conversionContainerBits_->load() : std::uint16_t{};
        if (SUCCEEDED(result) && deviceBuffer) {
            BYTE* appleBuffer = deviceBuffer;
            if (frameBridge || (sourceBitDepth == 16 && channels && containerBits == 16)) {
                try {
                    stagingBuffer_.resize(static_cast<std::size_t>(requestedFrames) * channels *
                                          sizeof(float));
                    std::fill(stagingBuffer_.begin(), stagingBuffer_.end(), BYTE{});
                    appleBuffer = stagingBuffer_.data();
                } catch (const std::bad_alloc&) {
                    inner->ReleaseBuffer(0, 0);
                    result = E_OUTOFMEMORY;
                    deviceBuffer = nullptr;
                    appleBuffer = nullptr;
                }
            }
            *data = appleBuffer;
            pendingAppleBuffer_.store(appleBuffer);
        } else {
            pendingAppleBuffer_.store(nullptr);
        }
        pendingDeviceBuffer_.store(deviceBuffer);
        pendingFrames_.store(SUCCEEDED(result) ? requestedFrames : 0);
        pendingDeviceFrames_.store(SUCCEEDED(result) ? deviceFrames : 0);
        pendingSourceBitDepth_.store(SUCCEEDED(result) ? sourceBitDepth : 0);
        pendingChannels_.store(SUCCEEDED(result) ? channels : std::uint16_t{});
        pendingContainerBits_.store(SUCCEEDED(result) ? containerBits : std::uint16_t{});
        const auto call = g_renderGetCalls.fetch_add(1) + 1;
        g_renderRequestedFrames.fetch_add(requestedFrames);
        g_renderLastRequestedFrames.store(requestedFrames);
        g_renderLastGetResult.store(result);
        if (call == 1 || (FAILED(result) && !g_renderGetFailureLogged.exchange(true))) {
            Log(L"IAudioRenderClient::GetBuffer call=" + std::to_wstring(call) +
                L" requestedFrames=" + std::to_wstring(requestedFrames) +
                L" deviceFrames=" + std::to_wstring(deviceFrames) +
                L" result=" + HResultText(result));
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 writtenFrames, DWORD flags) override {
        auto* inner = inner_.load();
        auto* appleBuffer = pendingAppleBuffer_.exchange(nullptr);
        auto* deviceBuffer = pendingDeviceBuffer_.exchange(nullptr);
        const auto pendingFrames = pendingFrames_.exchange(0);
        const auto pendingDeviceFrames = pendingDeviceFrames_.exchange(0);
        const auto sourceBitDepth = pendingSourceBitDepth_.exchange(0);
        const auto channels = pendingChannels_.exchange(0);
        const auto containerBits = pendingContainerBits_.exchange(0);
        std::shared_ptr<LocalPcmQueue> localQueue;
        {
            std::lock_guard lock(queueMutex_);
            localQueue = std::move(pendingLocalQueue_);
        }
        const bool frameBridge = localQueue && pendingDeviceFrames != 0 &&
            frameBridgeAppleRate_ && frameBridgeAppleRate_->load() != 0;
        bool bridged{};
        bool bridgeExact{};
        bool crossFormatRebuild{};
        UINT32 bridgedDeviceFrames = pendingDeviceFrames;
        if (frameBridge && deviceBuffer && writtenFrames != 0 && writtenFrames <= pendingFrames) {
            UINT32 copiedDeviceFrames{};
            bridgeExact = localQueue->Pop(deviceBuffer, pendingDeviceFrames, &copiedDeviceFrames);
            std::shared_ptr<LocalPcmQueue> promotedQueue;
            std::uint64_t promotedGeneration{};
            if (!bridgeExact && PromotePendingLocalQueueAtDrain(
                    localQueue, promotedQueue, promotedGeneration, crossFormatRebuild)) {
                const auto remainingFrames = pendingDeviceFrames - copiedDeviceFrames;
                UINT32 promotedFrames{};
                if (remainingFrames != 0) {
                    promotedQueue->Pop(
                        deviceBuffer + static_cast<std::size_t>(copiedDeviceFrames) *
                            localQueue->bytesPerFrame,
                        remainingFrames, &promotedFrames);
                }
                copiedDeviceFrames += promotedFrames;
                bridgeExact = copiedDeviceFrames == pendingDeviceFrames;
                Log(L"Local PCM drained queue handed off at sample boundary generation=" +
                    std::to_wstring(promotedGeneration) + L" requestedFrames=" +
                    std::to_wstring(pendingDeviceFrames) + L" promotedFrames=" +
                    std::to_wstring(promotedFrames) + L" exact=" +
                    std::to_wstring(bridgeExact));
            }
            // Exclusive-mode ReleaseBuffer must submit the entire acquired endpoint buffer.
            // At a cross-format item boundary the formats cannot share that buffer, so retain every
            // old-format tail frame, leave Pop's zero-filled remainder as inter-item silence, then
            // rebuild the endpoint immediately after the full old buffer is released.
            bridgedDeviceFrames = bridgeExact
                ? copiedDeviceFrames : pendingDeviceFrames;
            bool queueDropped{};
            {
                std::lock_guard queueLock(localQueue->mutex);
                queueDropped = localQueue->droppedFrames != 0;
            }
            if (promotedQueue) {
                std::lock_guard queueLock(promotedQueue->mutex);
                queueDropped = queueDropped || promotedQueue->droppedFrames != 0;
            }
            if (queueDropped) {
                inner->ReleaseBuffer(0, 0);
                DisableExclusiveIntent(L"local_pcm_queue_dropped_frames",
                    AUDCLNT_E_BUFFER_ERROR, {}, nullptr,
                    ammod::ipc::ErrorCategory::Internal);
                Log(L"Local PCM integrity failure; refusing to submit altered samples to WASAPI");
                return AUDCLNT_E_BUFFER_ERROR;
            }
            if ((localQueue->formatFlags & ammod::audio::kFormatFlagIsFloat) != 0 &&
                localQueue->bitsPerChannel == 32 && copiedDeviceFrames != 0) {
                ammod::audio::Float32ToLeftAlignedPcm32InPlace(
                    deviceBuffer,
                    static_cast<std::size_t>(copiedDeviceFrames) * localQueue->channels,
                    32);
            }
            bridged = true;
            if (crossFormatRebuild) {
                Log(L"Local PCM cross-format drain padded old endpoint buffer tailFrames=" +
                    std::to_wstring(copiedDeviceFrames) + L" bufferFrames=" +
                    std::to_wstring(pendingDeviceFrames) +
                    L"; endpoint rebuild armed after ReleaseBuffer");
            } else if (!bridgeExact && !frameBridgeDrainLogged_.exchange(true)) {
                Log(L"Local PCM frame bridge short drain requestedFrames=" +
                    std::to_wstring(pendingDeviceFrames) + L" copiedFrames=" +
                    std::to_wstring(copiedDeviceFrames) +
                    L"; preserving all source tail frames, zero-padding the unused exclusive "
                    L"buffer, and awaiting Apple Stop");
            }
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                const auto silentCount = g_renderBridgeSilentFlags.fetch_add(1) + 1;
                if (silentCount == 1) {
                    Log(L"Local PCM frame bridge overrode stale Apple SILENT flag because native "
                        L"source data was available");
                }
            }
            if (!frameBridgeLogged_.exchange(true)) {
                Log(L"Local PCM frame bridge first render appleFrames=" +
                    std::to_wstring(writtenFrames) + L" deviceFrames=" +
                    std::to_wstring(pendingDeviceFrames) + L" exact=" +
                    std::to_wstring(bridgeExact));
            }
            if (!crossFormatRebuild && !bridgeExact &&
                !frameBridgeUnderrunLogged_.exchange(true)) {
                std::lock_guard queueLock(localQueue->mutex);
                Log(L"Local PCM frame bridge underrun rate=" +
                    std::to_wstring(localQueue->sampleRate) + L" requestedFrames=" +
                    std::to_wstring(pendingDeviceFrames) + L" queuedFrames=" +
                    std::to_wstring(localQueue->bytesPerFrame
                        ? localQueue->sizeBytes / localQueue->bytesPerFrame : 0) +
                    L" capturedFrames=" + std::to_wstring(localQueue->capturedFrames) +
                    L" consumedFrames=" + std::to_wstring(localQueue->consumedFrames) +
                    L" droppedFrames=" + std::to_wstring(localQueue->droppedFrames) +
                    L" underrunFrames=" + std::to_wstring(localQueue->underrunFrames));
            }
        }
        if (!bridged && sourceBitDepth && channels && appleBuffer && deviceBuffer &&
            writtenFrames != 0 &&
            writtenFrames <= pendingFrames && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            const auto sampleCount = static_cast<std::size_t>(writtenFrames) * channels;
            std::size_t finiteNonzero{};
            std::size_t nonfinite{};
            std::size_t offSourceLattice{};
            double inputPeak{};
            for (std::size_t index = 0; index < sampleCount; ++index) {
                float sample{};
                std::memcpy(&sample, appleBuffer + index * sizeof(float), sizeof(sample));
                if (!std::isfinite(sample)) {
                    ++nonfinite;
                    continue;
                }
                const auto magnitude = std::abs(static_cast<double>(sample));
                if (magnitude != 0.0) ++finiteNonzero;
                if (magnitude > inputPeak) inputPeak = magnitude;
                if (sourceBitDepth <= 24) {
                    const double scaled = std::ldexp(
                        static_cast<double>(sample), static_cast<int>(sourceBitDepth - 1));
                    const double maximum = std::ldexp(1.0, sourceBitDepth - 1) - 1.0;
                    const double minimum = -std::ldexp(1.0, sourceBitDepth - 1);
                    if (scaled != std::trunc(scaled) || scaled < minimum || scaled > maximum) {
                        ++offSourceLattice;
                    }
                }
            }
            if (nonfinite || inputPeak > 1.0 || offSourceLattice) {
                inner->ReleaseBuffer(0, 0);
                DisableExclusiveIntent(L"non_lattice_decoder_output",
                                       E_INVALIDARG, {}, nullptr,
                                       ammod::ipc::ErrorCategory::Internal);
                Log(L"Decoder output violated exact source PCM lattice; nonfinite=" +
                    std::to_wstring(nonfinite) + L" offLattice=" +
                    std::to_wstring(offSourceLattice) +
                    L"; refusing WASAPI submission");
                return E_INVALIDARG;
            }
            const bool converted = containerBits == 16 && sourceBitDepth == 16
                ? ammod::audio::Float32ToPcm16(appleBuffer, deviceBuffer, sampleCount)
                : ammod::audio::Float32ToLeftAlignedPcm32InPlace(
                    deviceBuffer, sampleCount, sourceBitDepth);
            if (!sampleDiagnosticLogged_.load() && (finiteNonzero || nonfinite)) {
                std::size_t outputNonzero{};
                std::uint32_t outputPeak{};
                for (std::size_t index = 0; index < sampleCount; ++index) {
                    std::int32_t sample{};
                    if (containerBits == 16) {
                        std::int16_t packed{};
                        std::memcpy(&packed, deviceBuffer + index * sizeof(packed), sizeof(packed));
                        sample = packed;
                    } else {
                        std::memcpy(&sample, deviceBuffer + index * sizeof(sample), sizeof(sample));
                    }
                    if (sample != 0) ++outputNonzero;
                    const auto magnitude = sample == std::numeric_limits<std::int32_t>::min()
                        ? std::uint32_t{0x80000000u}
                        : static_cast<std::uint32_t>(sample < 0 ? -sample : sample);
                    if (magnitude > outputPeak) outputPeak = magnitude;
                }
                if (!sampleDiagnosticLogged_.exchange(true)) {
                    std::wostringstream diagnostic;
                    diagnostic << L"Render conversion first nonzero buffer inputFiniteNonzero="
                               << finiteNonzero << L" inputNonfinite=" << nonfinite
                               << L" inputPeak=" << std::setprecision(12) << inputPeak
                               << L" outputNonzero=" << outputNonzero
                               << L" outputPeak=" << outputPeak
                               << L" containerBits=" << containerBits
                               << L" samples=" << sampleCount;
                    Log(diagnostic.str());
                }
            }
            if (converted && conversionLoggedDepth_.exchange(sourceBitDepth) != sourceBitDepth) {
                Log(L"IAudioRenderClient restored float32 decoder output to integer PCM "
                    L"sourceBitDepth=" + std::to_wstring(sourceBitDepth) +
                    L" containerBits=" + std::to_wstring(containerBits) +
                    L" channels=" + std::to_wstring(channels));
            }
            if (!converted) {
                inner->ReleaseBuffer(0, 0);
                DisableExclusiveIntent(L"lossless_integer_restoration_failed",
                                       AUDCLNT_E_UNSUPPORTED_FORMAT, {}, nullptr,
                                       ammod::ipc::ErrorCategory::FormatUnsupported);
                Log(L"Render conversion integrity failure; refusing WASAPI submission");
                return AUDCLNT_E_UNSUPPORTED_FORMAT;
            }
        }
        const UINT32 releasedFrames = frameBridge ? bridgedDeviceFrames : writtenFrames;
        const DWORD releasedFlags = bridged ? (flags & ~AUDCLNT_BUFFERFLAGS_SILENT) : flags;
        const HRESULT result = inner ? inner->ReleaseBuffer(releasedFrames, releasedFlags)
                                     : E_UNEXPECTED;
        const auto call = g_renderReleaseCalls.fetch_add(1) + 1;
        g_renderWrittenFrames.fetch_add(writtenFrames);
        g_renderLastWrittenFrames.store(writtenFrames);
        g_renderLastReleaseResult.store(result);
        if (call == 1 || (FAILED(result) && !g_renderReleaseFailureLogged.exchange(true))) {
            Log(L"IAudioRenderClient::ReleaseBuffer call=" + std::to_wstring(call) +
                L" writtenFrames=" + std::to_wstring(writtenFrames) +
                L" deviceFrames=" + std::to_wstring(releasedFrames) +
                L" flags=0x" + HResultText(static_cast<HRESULT>(flags)).substr(2) +
                L" result=" + HResultText(result));
        }
        if (SUCCEEDED(result) && releasedFrames != 0) {
            ReportSuccessfulRenderAfterStart();
        }
        if (SUCCEEDED(result) && crossFormatRebuild) {
            if (RebuildPendingLocalQueueAtDrain(localQueue)) {
                Log(L"Local PCM cross-format drain rebuilt endpoint after full old-buffer release");
            } else {
                Log(L"Local PCM cross-format drain could not rebuild the pending endpoint");
            }
        }
        return result;
    }

private:
    ~RenderClientProxy() { Retire(); }
    std::atomic<ULONG> references_{1};
    std::atomic<IAudioRenderClient*> inner_{};
    std::atomic<std::uint32_t>* conversionSourceBitDepth_{};
    std::atomic<std::uint16_t>* conversionChannels_{};
    std::atomic<std::uint16_t>* conversionContainerBits_{};
    std::atomic<std::uint32_t>* frameBridgeAppleRate_{};
    std::atomic<std::uint32_t>* frameBridgeDeviceRate_{};
    std::mutex queueMutex_;
    std::shared_ptr<LocalPcmQueue> localQueue_;
    std::shared_ptr<LocalPcmQueue> pendingLocalQueue_;
    std::vector<BYTE> stagingBuffer_;
    std::atomic<BYTE*> pendingAppleBuffer_{};
    std::atomic<BYTE*> pendingDeviceBuffer_{};
    std::atomic<UINT32> pendingFrames_{};
    std::atomic<UINT32> pendingDeviceFrames_{};
    std::atomic<std::uint32_t> pendingSourceBitDepth_{};
    std::atomic<std::uint16_t> pendingChannels_{};
    std::atomic<std::uint16_t> pendingContainerBits_{};
    std::atomic<std::uint32_t> conversionLoggedDepth_{};
    std::atomic<bool> sampleDiagnosticLogged_{};
    std::atomic<bool> frameBridgeLogged_{};
    std::atomic<bool> frameBridgeUnderrunLogged_{};
    std::atomic<bool> frameBridgeDrainLogged_{};
};

class AudioClockProxy final : public IAudioClock, public RetirableService {
public:
    explicit AudioClockProxy(IAudioClock* inner) : inner_(inner) {}
    void Retire() override {
        if (auto* retired = inner_.exchange(nullptr)) retired->Release();
    }
    HRESULT Rebind(IAudioClient* client) override {
        if (!client) return E_POINTER;
        IAudioClock* replacement{};
        const HRESULT hr = client->GetService(__uuidof(IAudioClock),
                                              reinterpret_cast<void**>(&replacement));
        if (FAILED(hr) || !replacement) return hr;
        if (auto* retired = inner_.exchange(replacement)) retired->Release();
        return S_OK;
    }
    IUnknown* Unknown() override { return static_cast<IAudioClock*>(this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioClock)) {
            *object = static_cast<IAudioClock*>(this);
            AddRef();
            return S_OK;
        }
        auto* inner = inner_.load();
        return inner ? inner->QueryInterface(iid, object) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* frequency) override {
        auto* inner = inner_.load();
        return inner ? inner->GetFrequency(frequency) : AUDCLNT_E_DEVICE_INVALIDATED;
    }
    HRESULT STDMETHODCALLTYPE GetPosition(UINT64* position, UINT64* qpcPosition) override {
        auto* inner = inner_.load();
        return inner ? inner->GetPosition(position, qpcPosition) : AUDCLNT_E_DEVICE_INVALIDATED;
    }
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* characteristics) override {
        auto* inner = inner_.load();
        return inner ? inner->GetCharacteristics(characteristics) : AUDCLNT_E_DEVICE_INVALIDATED;
    }
private:
    ~AudioClockProxy() { Retire(); }
    std::atomic<ULONG> references_{1};
    std::atomic<IAudioClock*> inner_{};
};

class AudioStreamVolumeProxy final : public IAudioStreamVolume, public RetirableService {
public:
    explicit AudioStreamVolumeProxy(IAudioStreamVolume* inner) : inner_(inner) {
        UINT32 count{};
        const HRESULT countHr = inner ? inner->GetChannelCount(&count) : E_UNEXPECTED;
        std::vector<float> levels(count);
        const HRESULT levelsHr = inner && count ? inner->GetAllVolumes(count, levels.data()) : countHr;
        std::wostringstream message;
        message << L"IAudioStreamVolume initial countResult=" << HResultText(countHr)
                << L" count=" << count << L" levelsResult=" << HResultText(levelsHr);
        for (UINT32 index = 0; SUCCEEDED(levelsHr) && index < count; ++index) {
            message << L" ch" << index << L"=" << std::setprecision(9) << levels[index];
        }
        Log(message.str());
        const HRESULT unityHr = ForceUnity(inner);
        Log(L"IAudioStreamVolume forced to unity for bit-perfect exclusive contract result=" +
            HResultText(unityHr));
    }
    void Retire() override { if (auto* retired = inner_.exchange(nullptr)) retired->Release(); }
    HRESULT Rebind(IAudioClient* client) override {
        if (!client) return E_POINTER;
        IAudioStreamVolume* replacement{};
        const HRESULT hr = client->GetService(__uuidof(IAudioStreamVolume),
                                              reinterpret_cast<void**>(&replacement));
        if (FAILED(hr) || !replacement) return hr;
        const HRESULT unityHr = ForceUnity(replacement);
        if (FAILED(unityHr)) {
            replacement->Release();
            return unityHr;
        }
        if (auto* retired = inner_.exchange(replacement)) retired->Release();
        return S_OK;
    }
    IUnknown* Unknown() override { return static_cast<IAudioStreamVolume*>(this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioStreamVolume)) {
            *object = static_cast<IAudioStreamVolume*>(this); AddRef(); return S_OK;
        }
        auto* inner = inner_.load();
        return inner ? inner->QueryInterface(iid, object) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --references_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetChannelCount(UINT32* count) override { auto* p=inner_.load(); return p?p->GetChannelCount(count):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE SetChannelVolume(UINT32 index, const float level) override {
        auto* p = inner_.load();
        const HRESULT hr = p ? p->SetChannelVolume(index, 1.0f) : AUDCLNT_E_DEVICE_INVALIDATED;
        Log(L"IAudioStreamVolume::SetChannelVolume index=" + std::to_wstring(index) +
            L" requestedLevel=" + std::to_wstring(level) +
            L" appliedLevel=1 result=" + HResultText(hr));
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetChannelVolume(UINT32 index, float* level) override {
        auto* p = inner_.load();
        const HRESULT hr = p ? p->GetChannelVolume(index, level) : AUDCLNT_E_DEVICE_INVALIDATED;
        Log(L"IAudioStreamVolume::GetChannelVolume index=" + std::to_wstring(index) +
            L" level=" + std::to_wstring(SUCCEEDED(hr) && level ? *level : -1.0f) +
            L" result=" + HResultText(hr));
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetAllVolumes(UINT32 count, const float* levels) override {
        auto* p = inner_.load();
        std::vector<float> unity(count, 1.0f);
        const HRESULT hr = p ? p->SetAllVolumes(count, unity.data())
                             : AUDCLNT_E_DEVICE_INVALIDATED;
        std::wostringstream message;
        message << L"IAudioStreamVolume::SetAllVolumes count=" << count;
        for (UINT32 index = 0; levels && index < count; ++index) {
            message << L" ch" << index << L"=" << std::setprecision(9) << levels[index];
        }
        message << L" applied=unity result=" << HResultText(hr);
        Log(message.str());
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetAllVolumes(UINT32 count, float* levels) override {
        auto* p = inner_.load();
        const HRESULT hr = p ? p->GetAllVolumes(count, levels) : AUDCLNT_E_DEVICE_INVALIDATED;
        std::wostringstream message;
        message << L"IAudioStreamVolume::GetAllVolumes count=" << count;
        for (UINT32 index = 0; SUCCEEDED(hr) && levels && index < count; ++index) {
            message << L" ch" << index << L"=" << std::setprecision(9) << levels[index];
        }
        message << L" result=" << HResultText(hr);
        Log(message.str());
        return hr;
    }
private:
    static HRESULT ForceUnity(IAudioStreamVolume* volume) {
        if (!volume) return E_POINTER;
        UINT32 count{};
        HRESULT hr = volume->GetChannelCount(&count);
        if (FAILED(hr) || count == 0) return FAILED(hr) ? hr : E_UNEXPECTED;
        std::vector<float> unity(count, 1.0f);
        return volume->SetAllVolumes(count, unity.data());
    }
    ~AudioStreamVolumeProxy() { Retire(); }
    std::atomic<ULONG> references_{1};
    std::atomic<IAudioStreamVolume*> inner_{};
};

class AudioSessionControlProxy final : public IAudioSessionControl2, public RetirableService {
public:
    explicit AudioSessionControlProxy(IAudioSessionControl2* inner) : inner_(inner) {}
    void Retire() override { if (auto* retired = inner_.exchange(nullptr)) retired->Release(); }
    HRESULT Rebind(IAudioClient* client) override {
        if (!client) return E_POINTER;
        IAudioSessionControl* base{};
        HRESULT hr = client->GetService(__uuidof(IAudioSessionControl),
                                        reinterpret_cast<void**>(&base));
        if (FAILED(hr) || !base) return hr;
        IAudioSessionControl2* replacement{};
        hr = base->QueryInterface(__uuidof(IAudioSessionControl2),
                                  reinterpret_cast<void**>(&replacement));
        base->Release();
        if (FAILED(hr) || !replacement) return hr;
        if (auto* retired = inner_.exchange(replacement)) retired->Release();
        return S_OK;
    }
    IUnknown* Unknown() override { return static_cast<IAudioSessionControl2*>(this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioSessionControl) ||
            iid == __uuidof(IAudioSessionControl2)) {
            *object = static_cast<IAudioSessionControl2*>(this); AddRef(); return S_OK;
        }
        auto* inner = inner_.load();
        return inner ? inner->QueryInterface(iid, object) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --references_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetState(AudioSessionState* state) override { auto* p=inner_.load(); return p?p->GetState(state):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE GetDisplayName(LPWSTR* name) override { auto* p=inner_.load(); return p?p->GetDisplayName(name):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE SetDisplayName(LPCWSTR name, LPCGUID context) override { auto* p=inner_.load(); return p?p->SetDisplayName(name,context):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE GetIconPath(LPWSTR* path) override { auto* p=inner_.load(); return p?p->GetIconPath(path):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE SetIconPath(LPCWSTR path, LPCGUID context) override { auto* p=inner_.load(); return p?p->SetIconPath(path,context):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE GetGroupingParam(GUID* grouping) override { auto* p=inner_.load(); return p?p->GetGroupingParam(grouping):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE SetGroupingParam(LPCGUID grouping, LPCGUID context) override { auto* p=inner_.load(); return p?p->SetGroupingParam(grouping,context):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE RegisterAudioSessionNotification(IAudioSessionEvents* events) override { auto* p=inner_.load(); return p?p->RegisterAudioSessionNotification(events):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE UnregisterAudioSessionNotification(IAudioSessionEvents* events) override { auto* p=inner_.load(); return p?p->UnregisterAudioSessionNotification(events):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE GetSessionIdentifier(LPWSTR* id) override { auto* p=inner_.load(); return p?p->GetSessionIdentifier(id):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE GetSessionInstanceIdentifier(LPWSTR* id) override { auto* p=inner_.load(); return p?p->GetSessionInstanceIdentifier(id):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE GetProcessId(DWORD* pid) override { auto* p=inner_.load(); return p?p->GetProcessId(pid):AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE IsSystemSoundsSession() override { auto* p=inner_.load(); return p?p->IsSystemSoundsSession():AUDCLNT_E_DEVICE_INVALIDATED; }
    HRESULT STDMETHODCALLTYPE SetDuckingPreference(BOOL optOut) override { auto* p=inner_.load(); return p?p->SetDuckingPreference(optOut):AUDCLNT_E_DEVICE_INVALIDATED; }
private:
    ~AudioSessionControlProxy() { Retire(); }
    std::atomic<ULONG> references_{1};
    std::atomic<IAudioSessionControl2*> inner_{};
};

void ResetRenderStatistics() {
    g_renderGetCalls.store(0);
    g_renderReleaseCalls.store(0);
    g_renderRequestedFrames.store(0);
    g_renderWrittenFrames.store(0);
    g_renderBridgeSilentFlags.store(0);
    g_renderLastRequestedFrames.store(0);
    g_renderLastWrittenFrames.store(0);
    g_renderLastGetResult.store(S_OK);
    g_renderLastReleaseResult.store(S_OK);
    g_renderGetFailureLogged.store(false);
    g_renderReleaseFailureLogged.store(false);
    g_deviceEventSignals.store(0);
    g_forwardedAppleEventSignals.store(0);
    g_failedAppleEventSignals.store(0);
    g_audioUnitRenderCalls.store(0);
    g_modPumpCalls.store(0);
    g_nonzeroRenderActionFlags.store(0);
    g_lastRenderActionFlags.store(0);
    g_maxDeviceEventGapUs.store(0);
    g_maxPumpDurationUs.store(0);
    g_lateDeviceEvents.store(0);
}

std::wstring RenderStatisticsText() {
    return L"getCalls=" + std::to_wstring(g_renderGetCalls.load()) +
           L" releaseCalls=" + std::to_wstring(g_renderReleaseCalls.load()) +
           L" requestedFrames=" + std::to_wstring(g_renderRequestedFrames.load()) +
           L" writtenFrames=" + std::to_wstring(g_renderWrittenFrames.load()) +
           L" bridgeSilentOverrides=" + std::to_wstring(g_renderBridgeSilentFlags.load()) +
           L" lastRequest=" + std::to_wstring(g_renderLastRequestedFrames.load()) +
           L" lastWrite=" + std::to_wstring(g_renderLastWrittenFrames.load()) +
           L" lastGet=" + HResultText(g_renderLastGetResult.load()) +
           L" lastRelease=" + HResultText(g_renderLastReleaseResult.load()) +
           L" deviceEventSignals=" + std::to_wstring(g_deviceEventSignals.load()) +
           L" forwardedAppleSignals=" + std::to_wstring(g_forwardedAppleEventSignals.load()) +
           L" failedAppleSignals=" + std::to_wstring(g_failedAppleEventSignals.load()) +
           L" audioUnitRenderCalls=" + std::to_wstring(g_audioUnitRenderCalls.load()) +
           L" modPumpCalls=" + std::to_wstring(g_modPumpCalls.load()) +
           L" nonzeroActionFlags=" + std::to_wstring(g_nonzeroRenderActionFlags.load()) +
           L" lastActionFlags=0x" + HResultText(
               static_cast<HRESULT>(g_lastRenderActionFlags.load())).substr(2) +
           L" maxEventGapUs=" + std::to_wstring(g_maxDeviceEventGapUs.load()) +
           L" maxPumpUs=" + std::to_wstring(g_maxPumpDurationUs.load()) +
           L" lateEvents=" + std::to_wstring(g_lateDeviceEvents.load());
}

DWORD WINAPI RenderStatisticsLoop(void*) {
    std::uint64_t previousSignature{};
    for (;;) {
        Sleep(5000);
        const auto currentGetCalls = g_renderGetCalls.load();
        const auto currentSignals = g_deviceEventSignals.load();
        const auto signature = currentGetCalls ^ (currentSignals << 32);
        if (signature != previousSignature) {
            previousSignature = signature;
            Log(L"IAudioRenderClient periodic " + RenderStatisticsText());
        }
        CheckExclusiveRenderTimeout();
    }
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

std::wstring CurrentDefaultEndpointId() {
    IMMDeviceEnumerator* enumerator{};
    IMMDevice* endpoint{};
    std::wstring result;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator))) && enumerator) {
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &endpoint)) && endpoint) {
            result = DeviceId(endpoint);
            endpoint->Release();
        }
        enumerator->Release();
    }
    return result;
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

void ReportSuccessfulRenderAfterStart() {
    if (!g_exclusiveAwaitingFirstRender.exchange(false)) return;
    std::wstring endpoint;
    {
        std::lock_guard lock(g_activeEndpointMutex);
        endpoint = g_activeEndpoint;
    }
    Log(L"Exclusive render confirmed after Start; publishing Active");
    SendAgentEvent(ammod::ipc::MessageType::StatusChanged,
                   ammod::ipc::RuntimeState::Active, true, endpoint);
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
            g_nativeP0Puller.DisableForUiOff();
            g_ipcEnabled.store(false, std::memory_order_release);
            g_audioCoreGate.SetUiEnabled(false);
            Log(L"audio core v2 faulted; UI intent forced off result=" + HResultText(result));
        } else {
            Log(L"audio core v2 rejected current playback; AME remains armed result=" +
                HResultText(result));
            DWORD helperPid{};
            const bool launched = LaunchMediaControlCommand(L"reject-current", helperPid);
            Log(L"audio core v2 rejected current playback; stop-and-unload helper launched=" +
                std::to_wstring(launched) + L" pid=" + std::to_wstring(helperPid));
        }
        SendAgentEvent(ammod::ipc::MessageType::AttemptFailed,
                       playbackRejected ? ammod::ipc::RuntimeState::WaitingForStream
                                        : ammod::ipc::RuntimeState::FailedOff,
                       playbackRejected, {}, nullptr, result,
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

std::int32_t AudioCoreGraphControlCallback(void*, void* unit, bool resume) noexcept {
    if (!unit) return -1;
    const auto operation = resume ? g_v2OriginalAudioOutputUnitStart
                                  : g_v2OriginalAudioOutputUnitStop;
    if (!operation) return -1;
    const auto result = operation(unit);
    Log(std::wstring(L"audio core v2 graph ") + (resume ? L"resume" : L"pause") +
        L" unit=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(unit)) +
        L" result=" + std::to_wstring(result) + L" qpc=" +
        std::to_wstring(CurrentQpc()));
    return static_cast<std::int32_t>(result);
}

bool ReadV2RenderTemplate(AppleAudioTimeStamp& timestamp, void*& unit,
                          std::uint32_t& bus, std::uint32_t& frames,
                          std::int64_t& qpc) noexcept {
    // The snapshot contains a non-trivial timestamp and a raw unit pointer.
    // Keep the copy race-free; this lock is held only for the few scalar
    // assignments and is never taken while calling into Core Audio.
    std::lock_guard lock(g_audioUnitRenderTemplateMutex);
    if (g_v2RenderTemplateSequence.load(std::memory_order_acquire) == 0) {
        return false;
    }
    timestamp = g_v2RenderTemplate;
    unit = g_v2RenderTemplateUnit;
    bus = g_v2RenderTemplateBus;
    frames = g_v2RenderTemplateFrames;
    qpc = g_v2RenderTemplateQpc;
    return unit != nullptr;
}

bool AudioCoreGraphRenderCallback(void*, void* unit, void*,
                                  std::uint32_t requestedFrames,
                                  std::uint32_t sampleRate, std::uint32_t channels,
                                  bool planar) noexcept {
    constexpr std::uint32_t kMaxPumpFrames = 32768;
    if (!unit || !g_v2OriginalAudioUnitRender || requestedFrames == 0 ||
        requestedFrames > kMaxPumpFrames || sampleRate < 8000 || sampleRate > 768000 ||
        channels != 2) {
        g_v2PumpFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // This callback is invoked by the single v2 WASAPI render thread. The
    // fixed scratch storage keeps every steady-state pull allocation-free and
    // is large enough for a 20 ms quantum at the highest accepted rate.
    thread_local std::array<float, kMaxPumpFrames * 2u> scratch{};

    // The P0 converter is attached to a child AudioUnit, while Apple's normal
    // scheduler enters the hardware-output parent. AudioCoreRuntime supplies
    // that parent as `unit` after observing the parent start. Keep this target
    // fixed here; the last native template contributes only bus/timestamp and
    // quantum information, never a replacement object pointer.
    void* pullUnit = unit;
    std::uint32_t pullBus = 0;
    std::uint32_t pullFrames = requestedFrames;
    std::uint32_t pullRate = sampleRate;
    std::uint32_t pullChannels = channels;
    bool pullPlanar = planar;
    AppleAudioTimeStamp observedTemplate{};
    void* observedUnit{};
    std::uint32_t observedBus{};
    std::uint32_t observedFrames{};
    std::int64_t observedQpc{};
    const bool haveTemplate = ReadV2RenderTemplate(
        observedTemplate, observedUnit, observedBus, observedFrames, observedQpc);
    const auto frequency = g_qpcFrequency.load(std::memory_order_acquire);
    const auto nowQpc = CurrentQpc();
    const bool recentTemplate = haveTemplate && observedQpc > 0 && frequency > 0 &&
        nowQpc >= observedQpc &&
        static_cast<std::uint64_t>(nowQpc - observedQpc) <=
            static_cast<std::uint64_t>(frequency) * 2u;
    AudioUnitInputFormatState pullFormat{};
    const bool havePullFormat = FindAudioUnitInputFormat(pullUnit, pullFormat) &&
        IsAppleLinearPcm(&pullFormat.format) &&
        std::isfinite(pullFormat.format.mSampleRate) &&
        pullFormat.format.mSampleRate >= 8000.0 &&
        pullFormat.format.mSampleRate <= 768000.0 &&
        pullFormat.format.mChannelsPerFrame == 2;
    if (havePullFormat) {
        pullRate = static_cast<std::uint32_t>(pullFormat.format.mSampleRate + 0.5);
        pullChannels = pullFormat.format.mChannelsPerFrame;
        pullPlanar = (pullFormat.format.mFormatFlags &
                      ammod::audio_v2::kAppleFormatFlagIsNonInterleaved) != 0;
    }
    if (recentTemplate) {
        pullBus = observedBus;
        if (observedFrames != 0 && observedFrames <= kMaxPumpFrames) {
            pullFrames = observedFrames;
        }
    }
    if (!pullUnit || pullFrames == 0 || pullFrames > kMaxPumpFrames ||
        pullRate < 8000 || pullRate > 768000 || pullChannels != 2) {
        g_v2PumpFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    struct PumpBufferList final {
        std::uint32_t mNumberBuffers{};
        std::uint32_t mReserved{};
        AppleAudioBuffer mBuffers[2]{};
    } buffers{};
    if (pullPlanar) {
        buffers.mNumberBuffers = 2;
        buffers.mBuffers[0] = {1u, pullFrames * sizeof(float), scratch.data()};
        buffers.mBuffers[1] = {1u, pullFrames * sizeof(float),
                               scratch.data() + kMaxPumpFrames};
    } else {
        buffers.mNumberBuffers = 1;
        buffers.mBuffers[0] = {pullChannels, pullFrames * pullChannels * sizeof(float),
                               scratch.data()};
    }

    thread_local void* timestampUnit{};
    thread_local bool timestampReady{};
    thread_local double nextSampleTime{};
    thread_local std::uint64_t nextHostTime{};
    thread_local double rateScalar{1.0};
    thread_local std::uint32_t timestampFlags{};
    thread_local std::uint32_t renderBus{};
    if (!timestampReady || timestampUnit != pullUnit) {
        if (recentTemplate && std::isfinite(observedTemplate.mSampleTime)) {
            double baseSampleTime = observedTemplate.mSampleTime;
            AudioUnitInputFormatState templateFormat{};
            const bool haveTemplateFormat = observedUnit &&
                FindAudioUnitInputFormat(observedUnit, templateFormat) &&
                std::isfinite(templateFormat.format.mSampleRate) &&
                templateFormat.format.mSampleRate >= 8000.0 &&
                templateFormat.format.mSampleRate <= 768000.0;
            if (havePullFormat && haveTemplateFormat &&
                templateFormat.format.mSampleRate != pullFormat.format.mSampleRate) {
                baseSampleTime *= pullFormat.format.mSampleRate /
                    templateFormat.format.mSampleRate;
            }
            nextSampleTime = std::max(0.0, baseSampleTime) + pullFrames;
            nextHostTime = observedTemplate.mHostTime;
            if (nextHostTime != 0 && frequency > 0 && pullFrames != 0) {
                nextHostTime += static_cast<std::uint64_t>(
                    (static_cast<long double>(pullFrames) * frequency) / pullRate);
            }
            rateScalar = std::isfinite(observedTemplate.mRateScalar) &&
                observedTemplate.mRateScalar > 0.0 ? observedTemplate.mRateScalar : 1.0;
            timestampFlags = observedTemplate.mFlags;
            renderBus = pullBus;
        } else {
            nextSampleTime = 0.0;
            nextHostTime = 0;
            rateScalar = 1.0;
            timestampFlags = 1u; // kAudioTimeStampSampleTimeValid
            renderBus = 0;
        }
        timestampUnit = pullUnit;
        timestampReady = true;
    }

    AppleAudioTimeStamp timestamp{};
    timestamp.mSampleTime = nextSampleTime;
    timestamp.mHostTime = nextHostTime;
    timestamp.mRateScalar = rateScalar;
    timestamp.mFlags = timestampFlags | 1u;
    if (nextHostTime != 0) timestamp.mFlags |= 2u; // kAudioTimeStampHostTimeValid
    std::uint32_t actionFlags{};
    const auto result = g_v2OriginalAudioUnitRender(
        pullUnit, &actionFlags, &timestamp, renderBus, pullFrames, &buffers);
    const auto call = g_v2PumpCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (result != 0) {
        g_v2PumpFailures.fetch_add(1, std::memory_order_relaxed);
        Log(L"audio core v2 graph pull failed call=" + std::to_wstring(call) +
            L" unit=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(pullUnit)) +
            L" frames=" + std::to_wstring(pullFrames) + L" result=" +
            std::to_wstring(result));
        return false;
    }

    nextSampleTime += pullFrames;
    if (nextHostTime != 0 && frequency > 0) {
        nextHostTime += static_cast<std::uint64_t>(
            (static_cast<long double>(pullFrames) * frequency) / pullRate);
    }
    if (call <= 4) {
        Log(L"audio core v2 graph pull call=" + std::to_wstring(call) +
            L" unit=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(pullUnit)) +
            L" requested=" + std::to_wstring(requestedFrames) +
            L" frames=" + std::to_wstring(pullFrames) +
            L" rate=" + std::to_wstring(pullRate) +
            L" pullFormat=" + std::to_wstring(havePullFormat) +
            L" templateRecent=" + std::to_wstring(recentTemplate) +
            L" templateUnit=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(observedUnit)) +
            L" sampleTime=" +
            std::to_wstring(timestamp.mSampleTime) + L" layout=" +
            (pullPlanar ? L"planar-float32" : L"interleaved-float32"));
    }
    return true;
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
        g_converterProbes[converter] = {*source, *destination, 0, false, queue};
    }
    g_threadLocalPcmSourceConverter = converter;
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
        std::lock_guard lock(g_audioUnitRenderTemplateMutex);
        g_v2RenderTemplateSequence.fetch_add(1, std::memory_order_acq_rel);
        g_v2RenderTemplate = observed;
        g_v2RenderTemplateUnit = unit;
        g_v2RenderTemplateBus = bus;
        g_v2RenderTemplateFrames = frames;
        g_v2RenderTemplateQpc = enterQpc;
        g_v2RenderTemplateSequence.fetch_add(1, std::memory_order_release);
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
    WritePrivateProfileStringW(L"mod", L"mode", L"probe", g_iniPath.c_str());
    g_ipcEnabled.store(false);
    Log(L"exclusive intent auto-disabled reason=" + reason + L" result=" + HResultText(hr));
    if (category == ammod::ipc::ErrorCategory::None) category = ammod::ipc::CategorizeAudioError(hr);
    SendAgentEvent(ammod::ipc::MessageType::AttemptFailed, ammod::ipc::RuntimeState::FailedOff,
                   false, endpoint, format, hr, category, reason);
}

void CheckExclusiveRenderTimeout() {
    if (!g_exclusiveAwaitingFirstRender.load()) return;
    const auto started = g_exclusiveStartTick.load();
    if (!started || GetTickCount64() - started < 3000 || g_deviceEventSignals.load() < 10) return;
    if (!g_exclusiveAwaitingFirstRender.exchange(false)) return;
    std::wstring endpoint;
    {
        std::lock_guard lock(g_activeEndpointMutex);
        endpoint = g_activeEndpoint;
    }
    DisableExclusiveIntent(L"exclusive_render_stalled", HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                           endpoint, nullptr, ammod::ipc::ErrorCategory::Timeout);
}

class AudioClientProxy final : public IAudioClient3 {
public:
    AudioClientProxy(IAudioClient* inner, IMMDevice* device, DWORD activationContext,
                      const PROPVARIANT* activationParams, std::wstring endpoint)
        : inner_(inner), device_(device), activationContext_(activationContext),
          proxyId_(g_audioClientSequence.fetch_add(1) + 1),
          sourceSampleRate_(g_sourceSampleRate.load()),
          sourceChannels_(static_cast<std::uint16_t>(g_sourceChannels.load())),
          endpoint_(std::move(endpoint)) {
        experimentalPumpEnabled_ = ReadBool(L"experimental_mod_pump", false);
        if (ExclusiveRequested()) {
            const auto parsedRate = g_startupLockedSampleRate.load();
            const auto parsedChannels = g_startupLockedChannels.load();
            if (parsedRate >= 8000 && parsedRate <= 768000 &&
                parsedChannels > 0 && parsedChannels <= 8) {
                sourceSampleRate_ = parsedRate;
                sourceChannels_ = static_cast<std::uint16_t>(parsedChannels);
                startupFormatLocked_ = true;
                Log(L"AudioClientProxy using pre-restart locked QLAC format rate=" +
                    std::to_wstring(sourceSampleRate_) + L" channels=" +
                    std::to_wstring(sourceChannels_));
            }
        }
        if (device_) device_->AddRef();
        PropVariantInit(&activationParams_);
        if (activationParams && SUCCEEDED(PropVariantCopy(&activationParams_, activationParams))) {
            hasActivationParams_ = true;
        }
        RefreshCapabilities();
        Log(L"AudioClientProxy created proxy=" + std::to_wstring(proxyId_) +
            L" object=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(this)) +
            L" inner=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(inner_)) +
            L" tid=" + std::to_wstring(GetCurrentThreadId()) +
            L" qpc=" + std::to_wstring(CurrentQpc()));
    }

    AudioClientProxy(const AudioClientProxy&) = delete;
    AudioClientProxy& operator=(const AudioClientProxy&) = delete;

    bool CanReuseExclusive(const std::wstring& endpoint) {
        std::lock_guard lock(mutex_);
        return exclusiveInitialized_ && _wcsicmp(endpoint_.c_str(), endpoint.c_str()) == 0;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioClient)) {
            *object = static_cast<IAudioClient*>(this);
        } else if (iid == __uuidof(IAudioClient2) && supports2_) {
            *object = static_cast<IAudioClient2*>(this);
        } else if (iid == __uuidof(IAudioClient3) && supports3_) {
            *object = static_cast<IAudioClient3*>(this);
        } else {
            std::lock_guard lock(mutex_);
            const HRESULT hr = inner_ ? inner_->QueryInterface(iid, object) : E_NOINTERFACE;
            Log(L"AudioClientProxy QueryInterface forwarded iid=" + GuidText(iid) +
                L" result=" + HResultText(hr));
            return hr;
        }
        AddRef();
        Log(L"AudioClientProxy QueryInterface iid=" + GuidText(iid) + L" result=0x00000000");
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE shareMode, DWORD flags,
                                         REFERENCE_TIME duration, REFERENCE_TIME periodicity,
        const WAVEFORMATEX* format, LPCGUID sessionGuid) override {
        std::lock_guard lock(mutex_);
        initializeFlags_ = flags;
        initializeDuration_ = duration;
        if (sessionGuid) {
            initializeSessionGuid_ = *sessionGuid;
            hasInitializeSessionGuid_ = true;
        } else {
            hasInitializeSessionGuid_ = false;
        }
        const auto mode = ExclusiveRequested() ? std::wstring(L"exclusive") : std::wstring(L"probe");
        const WAVEFORMATEX* appleRequestedFormat = format;
        WAVEFORMATEXTENSIBLE committedFormat{};
        ActiveGraphFormatCommit commit{};
        {
            std::lock_guard commitLock(g_activeGraphFormatMutex);
            commit = g_activeGraphFormat;
        }
        const bool commitIsFresh = commit.committedTick != 0 &&
            GetTickCount64() - commit.committedTick <= 500 && commit.channels == 2;
        const bool restoringFloatDecoder = commitIsFresh && IsWaveFloat(format) &&
            (commit.sourceBitDepth == 16 || commit.sourceBitDepth == 24);
        const auto committedContainerBits = restoringFloatDecoder && commit.sourceBitDepth == 16
            ? std::uint16_t{16} : std::uint16_t{32};
        const auto committedValidBits = restoringFloatDecoder && commit.sourceBitDepth == 16
            ? std::uint16_t{16}
            : (restoringFloatDecoder ? std::uint16_t{32}
                                     : OutputValidBitsForSource(commit.sourceBitDepth));
        const auto appleRequestedValidBits = WaveValidBits(format);
        const bool staleStreamingRateTransition = ExclusiveRequested() &&
            exclusiveInitialized_ && format && format->nChannels == 2 && commitIsFresh &&
            !commit.localPcm &&
            g_refreshedOutputCommitSequence.load() == commit.sequence &&
            format->nSamplesPerSec != commit.sampleRate;
        if (staleStreamingRateTransition) {
            const auto oldRate = format->nSamplesPerSec;
            const auto newRate = commit.sampleRate;
            Log(L"Streaming cross-rate stale graph rejected before WASAPI Initialize proxy=" +
                std::to_wstring(proxyId_) + L" oldRate=" + std::to_wstring(oldRate) +
                L" newRate=" + std::to_wstring(newRate) + L" outputUnit=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(g_currentAudioUnit.load())) +
                L" commitUnit=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(commit.unit)) +
                L" commitSequence=" + std::to_wstring(commit.sequence));
            if (streamStarted_.exchange(false) && inner_) inner_->Stop();
            if (inner_) inner_->Reset();
            RetireTrackedServices();
            if (inner_) {
                inner_->Release();
                inner_ = nullptr;
            }
            exclusiveInitialized_ = false;
            exclusiveStarted_.store(false);
            handoffPending_ = false;
            reattachDeviceEvent_ = false;
            return AUDCLNT_E_DEVICE_INVALIDATED;
        }
        if (ExclusiveRequested() && format &&
            format->nChannels == 2 && commitIsFresh &&
            g_refreshedOutputCommitSequence.load() == commit.sequence &&
            (format->nSamplesPerSec != commit.sampleRate ||
             appleRequestedValidBits != committedValidBits)) {
            committedFormat = IntegerPcmFormat(
                commit.sampleRate, 2, committedValidBits, committedContainerBits);
            format = &committedFormat.Format;
            Log(L"IAudioClient::Initialize replaced stale cached Apple format from active graph "
                L"commit proxy=" + std::to_wstring(proxyId_) + L" oldRate=" +
                std::to_wstring(appleRequestedFormat->nSamplesPerSec) + L" newRate=" +
                std::to_wstring(commit.sampleRate) + L" oldValidBits=" +
                std::to_wstring(appleRequestedValidBits) + L" newValidBits=" +
                std::to_wstring(committedValidBits) + L" sourceBitDepth=" +
                std::to_wstring(commit.sourceBitDepth) + L" commitSequence=" +
                std::to_wstring(commit.sequence) + L" commitUnit=" +
                std::to_wstring(reinterpret_cast<std::uintptr_t>(commit.unit)));
        }
        const auto conversionSourceBitDepth = commitIsFresh && commit.sourceBitDepth
            ? commit.sourceBitDepth : sourceBitDepth_;
        const bool supportedIntegerContainer = format &&
            ((format->wBitsPerSample == 16 && format->nBlockAlign == 4 &&
              conversionSourceBitDepth == 16) ||
             (format->wBitsPerSample == 32 && format->nBlockAlign == 8));
        renderFloatToInteger_ = ExclusiveRequested() && IsWaveFloat(appleRequestedFormat) &&
            IsWaveInteger(format) && format && format->nChannels == 2 &&
            supportedIntegerContainer &&
            (conversionSourceBitDepth == 16 || conversionSourceBitDepth == 24);
        renderConversionSourceBitDepth_ = renderFloatToInteger_
            ? conversionSourceBitDepth : 0;
        renderConversionChannels_ = format ? format->nChannels : 0;
        renderConversionContainerBits_ = renderFloatToInteger_ && format
            ? format->wBitsPerSample : 0;
        renderFrameBridgeAppleRate_ = commitIsFresh && commit.localPcm &&
            appleRequestedFormat && format &&
            appleRequestedFormat->nSamplesPerSec != format->nSamplesPerSec
            ? appleRequestedFormat->nSamplesPerSec : 0;
        renderFrameBridgeDeviceRate_ = renderFrameBridgeAppleRate_.load() && format
            ? format->nSamplesPerSec : 0;
        {
            std::lock_guard queueLock(g_activeLocalQueueMutex);
            renderLocalQueue_ = renderFrameBridgeAppleRate_.load() ? g_activeLocalQueue : nullptr;
        }
        const bool queuedPcm16 = renderLocalQueue_ &&
            renderLocalQueue_->bitsPerChannel == 16 &&
            renderLocalQueue_->bytesPerFrame == 4 &&
            (renderLocalQueue_->formatFlags &
                ammod::audio::kFormatFlagIsSignedInteger) != 0;
        const bool queuedFloat32 = renderLocalQueue_ &&
            renderLocalQueue_->bitsPerChannel == 32 &&
            renderLocalQueue_->bytesPerFrame == 8 &&
            (renderLocalQueue_->formatFlags & ammod::audio::kFormatFlagIsFloat) != 0;
        if (renderLocalQueue_ && !queuedPcm16 && !queuedFloat32) {
            renderFrameBridgeAppleRate_ = 0;
            renderFrameBridgeDeviceRate_ = 0;
            renderLocalQueue_.reset();
        }
        if (renderFloatToInteger_) {
            Log(L"Float decoder output requires in-place integer endpoint conversion "
                L"sourceBitDepth=" + std::to_wstring(renderConversionSourceBitDepth_.load()) +
                L" targetValidBits=" + std::to_wstring(WaveValidBits(format)));
        }
        if (renderFrameBridgeAppleRate_.load() && renderLocalQueue_) {
            Log(L"Local PCM frame bridge configured appleRate=" +
                std::to_wstring(renderFrameBridgeAppleRate_.load()) + L" deviceRate=" +
                std::to_wstring(renderFrameBridgeDeviceRate_.load()) + L" sourceBits=" +
                std::to_wstring(renderLocalQueue_->bitsPerChannel));
        }
        std::wostringstream request;
        request << L"IAudioClient::Initialize requested shareMode=" << static_cast<int>(shareMode)
                << L" proxy=" << proxyId_
                << L" object=" << reinterpret_cast<std::uintptr_t>(this)
                << L" tid=" << GetCurrentThreadId()
                << L" qpc=" << CurrentQpc()
                << L" flags=0x" << std::hex << flags << std::dec << L" duration100ns=" << duration
                << L" periodicity100ns=" << periodicity << L" endpoint=" << endpoint_ << L" "
                << FormatText(format) << L" mode=" << mode;
        if (appleRequestedFormat != format) {
            request << L" appleRequested{" << FormatText(appleRequestedFormat) << L"}";
        }
        Log(request.str());

        if (!inner_) return E_UNEXPECTED;
        if (ExclusiveRequested() && exclusiveInitialized_ && format &&
            initializedSampleRate_ == format->nSamplesPerSec &&
            initializedChannels_ == format->nChannels &&
            initializedBits_ == format->wBitsPerSample &&
            initializedValidBits_ == WaveValidBits(format) &&
            initializedBlockAlign_ == format->nBlockAlign) {
            HRESULT stopHr = S_OK;
            if (streamStarted_.exchange(false)) {
                stopHr = inner_->Stop();
                if (SUCCEEDED(stopHr)) staleStopDebt_.fetch_add(1);
            }
            const HRESULT resetHr = SUCCEEDED(stopHr) ? inner_->Reset() : stopHr;
            if (SUCCEEDED(resetHr)) {
                exclusiveStarted_.store(false);
                handoffPending_ = true;
                Log(L"Exclusive persistent-client handoff prepared; Initialize virtualized S_OK " +
                    FormatText(format));
                return S_OK;
            }
            Log(L"Exclusive persistent-client handoff failed stop=" + HResultText(stopHr) +
                L" reset=" + HResultText(resetHr));
            return FAILED(stopHr) ? stopHr : resetHr;
        }
        if (ExclusiveRequested() && exclusiveInitialized_ && format) {
            const HRESULT rebuildHr = RebuildPersistentExclusive(flags, duration, format, sessionGuid);
            if (SUCCEEDED(rebuildHr)) return rebuildHr;
            DisableExclusiveIntent(L"exclusive_rate_change_rebuild_failed", rebuildHr,
                                   endpoint_, format);
            exclusiveInitialized_ = false;
            return InitializeFreshShared(shareMode, flags, duration, periodicity,
                                         format, sessionGuid, rebuildHr);
        }
        if (_wcsicmp(mode.c_str(), L"exclusive") != 0 ||
            shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE || !format) {
            DWORD passthroughFlags = flags;
            if (usingVirtualMix_ && shareMode == AUDCLNT_SHAREMODE_SHARED) {
                passthroughFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                Log(L"Shared recovery enabled Audio Engine conversion for virtual integer format");
            }
            const HRESULT hr = inner_->Initialize(shareMode, passthroughFlags, duration,
                                                  periodicity, format, sessionGuid);
            exclusiveInitialized_ = shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE && SUCCEEDED(hr);
            Log(L"Initialize passthrough result=" + HResultText(hr));
            return hr;
        }

        SendAgentEvent(ammod::ipc::MessageType::StatusChanged, ammod::ipc::RuntimeState::Probing,
                       true, endpoint_, format);

        const auto configuredTarget = ReadSetting(L"target_endpoint", L"");
        const auto expectedEndpoint = configuredTarget.empty() ? CurrentDefaultEndpointId() : configuredTarget;
        if (expectedEndpoint.empty() || _wcsicmp(expectedEndpoint.c_str(), endpoint_.c_str()) != 0) {
            LogDefaultEndpointEnvironment(eConsole, L"console-current");
            LogDefaultEndpointEnvironment(eMultimedia, L"multimedia-current");
            const HRESULT reason = expectedEndpoint.empty() ? AUDCLNT_E_DEVICE_INVALIDATED :
                                                               AUDCLNT_E_WRONG_ENDPOINT_TYPE;
            DisableExclusiveIntent(L"default_endpoint_mismatch", reason, endpoint_, format,
                                   ammod::ipc::ErrorCategory::DeviceChanged);
            return InitializeFreshShared(shareMode, flags, duration, periodicity, format, sessionGuid, reason);
        }

        const HRESULT supported = inner_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, format, nullptr);
        if (supported != S_OK) {
            Log(L"Exclusive exact format unsupported result=" + HResultText(supported));
            DisableExclusiveIntent(L"format_unsupported", supported, endpoint_, format,
                                   ammod::ipc::ErrorCategory::FormatUnsupported);
            return InitializeFreshShared(shareMode, flags, duration, periodicity, format, sessionGuid, supported);
        }

        REFERENCE_TIME defaultPeriod{}, minimumPeriod{};
        const HRESULT periodResult = inner_->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
        REFERENCE_TIME selectedPeriod = ReadPeriod();
        if (selectedPeriod == 0) {
            // Apple sizes and schedules its render requests from the original shared duration.
            // Exclusive event mode requires duration == periodicity, so preserve that quantum.
            selectedPeriod = (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && duration > 0
                                 ? duration
                                 : defaultPeriod;
        }
        if (FAILED(periodResult) || selectedPeriod <= 0) selectedPeriod = 100000;
        g_expectedPeriodUs.store(static_cast<std::uint64_t>(selectedPeriod / 10));

        DWORD exclusiveFlags = flags | AUDCLNT_STREAMFLAGS_NOPERSIST;
        exclusiveFlags &= ~(AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                            AUDCLNT_STREAMFLAGS_LOOPBACK);
        const bool eventDriven = (exclusiveFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
        REFERENCE_TIME exclusiveDuration = eventDriven ? selectedPeriod : selectedPeriod * 4;

        std::wostringstream attempt;
        attempt << L"Exclusive attempt flags=0x" << std::hex << exclusiveFlags << std::dec
                << L" duration100ns=" << exclusiveDuration << L" periodicity100ns=" << selectedPeriod
                << L" eventDriven=" << eventDriven;
        Log(attempt.str());
        SendAgentEvent(ammod::ipc::MessageType::StatusChanged,
                       ammod::ipc::RuntimeState::Initializing, true, endpoint_, format);

        HRESULT exclusiveHr = inner_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, exclusiveFlags,
                                                  exclusiveDuration, selectedPeriod, format, sessionGuid);
        Log(L"Exclusive Initialize result=" + HResultText(exclusiveHr));
        if (SUCCEEDED(exclusiveHr)) {
            exclusiveInitialized_ = true;
            RememberInitializedFormat(format);
            Log(L"Exclusive client initialized; waiting for SetEventHandle/Start before Active");
            return exclusiveHr;
        }

        if (exclusiveHr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            SendAgentEvent(ammod::ipc::MessageType::StatusChanged,
                           ammod::ipc::RuntimeState::AlignRetry, true, endpoint_, format,
                           exclusiveHr, ammod::ipc::ErrorCategory::BufferSizeNotAligned);
            UINT32 alignedFrames{};
            const HRESULT sizeHr = inner_->GetBufferSize(&alignedFrames);
            Log(L"Exclusive alignment GetBufferSize result=" + HResultText(sizeHr) +
                L" frames=" + std::to_wstring(alignedFrames));
            if (SUCCEEDED(sizeHr) && alignedFrames && format->nSamplesPerSec) {
                IAudioClient* fresh{};
                const HRESULT activationHr = ActivateFresh(&fresh);
                if (SUCCEEDED(activationHr) && fresh) {
                    exclusiveDuration = (10000000LL * alignedFrames + format->nSamplesPerSec - 1) /
                                        format->nSamplesPerSec;
                    const HRESULT retryHr = fresh->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                               exclusiveFlags, exclusiveDuration,
                                                               selectedPeriod, format, sessionGuid);
                    Log(L"Exclusive aligned fresh-client retry result=" + HResultText(retryHr) +
                        L" duration100ns=" + std::to_wstring(exclusiveDuration));
                    if (SUCCEEDED(retryHr)) {
                        ReplaceInner(fresh);
                        exclusiveInitialized_ = true;
                        RememberInitializedFormat(format);
                        Log(L"Exclusive aligned client initialized; waiting for Start before Active");
                        return retryHr;
                    }
                    exclusiveHr = retryHr;
                    fresh->Release();
                } else {
                    exclusiveHr = activationHr;
                    Log(L"Exclusive aligned fresh-client activation failed result=" +
                        HResultText(activationHr));
                }
            } else {
                exclusiveHr = sizeHr;
            }
        }

        if (exclusiveHr == AUDCLNT_E_DEVICE_IN_USE) {
            // During a track/format change Apple tears down the old AudioUnit and constructs the
            // replacement concurrently.  The endpoint can remain reserved for a short interval
            // after Stop/Uninitialize.  Never retry Initialize on the failed client: activate a
            // fresh one each time, as required by the WASAPI initialization contract.
            constexpr unsigned retryCount = 20;
            for (unsigned attemptIndex = 1; attemptIndex <= retryCount; ++attemptIndex) {
                Sleep(50);
                IAudioClient* fresh{};
                const HRESULT activationHr = ActivateFresh(&fresh);
                if (FAILED(activationHr) || !fresh) {
                    exclusiveHr = activationHr;
                    Log(L"Exclusive device-busy fresh activation attempt=" +
                        std::to_wstring(attemptIndex) + L" result=" + HResultText(activationHr));
                    continue;
                }
                const HRESULT retryHr = fresh->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                           exclusiveFlags, exclusiveDuration,
                                                           selectedPeriod, format, sessionGuid);
                Log(L"Exclusive device-busy fresh retry attempt=" +
                    std::to_wstring(attemptIndex) + L" result=" + HResultText(retryHr));
                if (SUCCEEDED(retryHr)) {
                    ReplaceInner(fresh);
                    exclusiveInitialized_ = true;
                    RememberInitializedFormat(format);
                    Log(L"Exclusive device-busy retry recovered; waiting for Start before Active");
                    return retryHr;
                }
                exclusiveHr = retryHr;
                fresh->Release();
                if (retryHr != AUDCLNT_E_DEVICE_IN_USE) break;
            }
        }

        DisableExclusiveIntent(L"exclusive_initialize_failed", exclusiveHr, endpoint_, format);
        return InitializeFreshShared(shareMode, flags, duration, periodicity, format, sessionGuid,
                                     exclusiveHr);
    }

    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override {
        std::lock_guard lock(mutex_);
        const HRESULT result = inner_ ? inner_->GetBufferSize(frames) : E_UNEXPECTED;
        UINT32 deviceFrames{};
        if (SUCCEEDED(result) && frames) {
            deviceFrames = *frames;
            bufferFrames_ = deviceFrames;
            const auto appleRate = renderFrameBridgeAppleRate_.load();
            const auto deviceRate = renderFrameBridgeDeviceRate_.load();
            if (appleRate && deviceRate) {
                *frames = ammod::audio::ScaleFrameCount(deviceFrames, deviceRate, appleRate);
            }
        }
        if (!bufferSizeLogged_) {
            bufferSizeLogged_ = true;
            Log(L"IAudioClient::GetBufferSize result=" + HResultText(result) +
                L" frames=" + std::to_wstring(SUCCEEDED(result) && frames ? *frames : 0) +
                L" deviceFrames=" + std::to_wstring(deviceFrames));
        }
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency) override {
        std::lock_guard lock(mutex_); return inner_ ? inner_->GetStreamLatency(latency) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* frames) override {
        std::lock_guard lock(mutex_);
        const HRESULT result = inner_ ? inner_->GetCurrentPadding(frames) : E_UNEXPECTED;
        const UINT32 devicePadding = SUCCEEDED(result) && frames ? *frames : 0;
        const bool virtualized = SUCCEEDED(result) && frames && exclusiveInitialized_ &&
                                 exclusiveStarted_.load() && g_deviceEventSignals.load() != 0;
        if (virtualized) *frames = 0;
        const auto call = g_paddingCalls.fetch_add(1) + 1;
        if (call <= 12 || FAILED(result)) {
            Log(L"IAudioClient::GetCurrentPadding call=" + std::to_wstring(call) +
                L" frames=" + std::to_wstring(SUCCEEDED(result) && frames ? *frames : 0) +
                L" deviceFrames=" + std::to_wstring(devicePadding) +
                L" virtualized=" + std::to_wstring(virtualized) +
                L" result=" + HResultText(result) +
                L" exclusiveInitialized=" + std::to_wstring(exclusiveInitialized_));
        }
        return result;
    }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE mode, const WAVEFORMATEX* format,
                                                WAVEFORMATEX** closest) override {
        std::lock_guard lock(mutex_); return inner_ ? inner_->IsFormatSupported(mode, format, closest) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override {
        if (!format) return E_POINTER;
        *format = nullptr;
        std::lock_guard lock(mutex_);
        if (!inner_) return E_UNEXPECTED;
        Log(L"GetMixFormat entered proxy=" + std::to_wstring(proxyId_) +
            L" object=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(this)) +
            L" tid=" + std::to_wstring(GetCurrentThreadId()) +
            L" qpc=" + std::to_wstring(CurrentQpc()) +
            L" candidateObservation=" + std::to_wstring(g_threadGraphFormat.observationId) +
            L" candidateAgeMs=" + std::to_wstring(g_threadGraphFormat.observedTick
                ? GetTickCount64() - g_threadGraphFormat.observedTick : 0) +
            L" candidateConverter=" + std::to_wstring(
                reinterpret_cast<std::uintptr_t>(g_threadGraphFormat.converter)) +
            L" candidateCaller=" + std::to_wstring(
                reinterpret_cast<std::uintptr_t>(g_threadGraphFormat.caller)));
        if (ExclusiveRequested()) {
            const auto now = GetTickCount64();
            if (g_hardwareOutputReplacementFormatOverride.active &&
                g_hardwareOutputReplacementFormatOverride.sampleRate >= 8000 &&
                g_hardwareOutputReplacementFormatOverride.sampleRate <= 768000 &&
                g_hardwareOutputReplacementFormatOverride.channels == 2) {
                sourceSampleRate_ = g_hardwareOutputReplacementFormatOverride.sampleRate;
                sourceChannels_ = static_cast<std::uint16_t>(
                    g_hardwareOutputReplacementFormatOverride.channels);
                sourceBitDepth_ = g_hardwareOutputReplacementFormatOverride.sourceBitDepth;
                startupFormatLocked_ = false;
                Log(L"GetMixFormat bound to scoped hardware-output replacement commit rate=" +
                    std::to_wstring(sourceSampleRate_) + L" sourceBitDepth=" +
                    std::to_wstring(sourceBitDepth_) + L" proxy=" +
                    std::to_wstring(proxyId_) + L" commitSequence=" +
                    std::to_wstring(
                        g_hardwareOutputReplacementFormatOverride.commitSequence));
            } else {
                const bool graphCandidateIsFresh = g_threadGraphFormat.observedTick != 0 &&
                now - g_threadGraphFormat.observedTick <= 2000 &&
                g_threadGraphFormat.sampleRate >= 8000 &&
                g_threadGraphFormat.sampleRate <= 768000 &&
                g_threadGraphFormat.channels == 2;
                if (graphCandidateIsFresh) {
                    sourceSampleRate_ = g_threadGraphFormat.sampleRate;
                    sourceChannels_ = static_cast<std::uint16_t>(g_threadGraphFormat.channels);
                    if (g_threadGraphFormat.sourceBitDepth) {
                        sourceBitDepth_ = g_threadGraphFormat.sourceBitDepth;
                    }
                    startupFormatLocked_ = false;
                    Log(L"GetMixFormat bound to same-thread graph candidate rate=" +
                        std::to_wstring(sourceSampleRate_) + L" encodedFormat=" +
                        AppleFormatIdText(g_threadGraphFormat.encodedFormat) +
                        L" sourceBitDepth=" + std::to_wstring(sourceBitDepth_) +
                        L" proxy=" + std::to_wstring(proxyId_) +
                        L" observation=" + std::to_wstring(g_threadGraphFormat.observationId));
                }
            }
        }
        if (ExclusiveRequested() && sourceSampleRate_ && sourceChannels_ == 2) {
            const auto desiredValidBits = OutputValidBitsForSource(sourceBitDepth_);
            const auto desiredContainerBits = sourceBitDepth_ == 16
                ? std::uint16_t{16} : std::uint16_t{32};
            auto integerFormat = IntegerPcmFormat(sourceSampleRate_, sourceChannels_,
                                                  sourceBitDepth_ == 16 ? std::uint16_t{16}
                                                                        : desiredValidBits,
                                                  desiredContainerBits);
            HRESULT support = inner_->IsFormatSupported(
                AUDCLNT_SHAREMODE_EXCLUSIVE, &integerFormat.Format, nullptr);
            if (support != S_OK && desiredContainerBits == 32 && desiredValidBits != 32) {
                integerFormat.Samples.wValidBitsPerSample = 32;
                support = inner_->IsFormatSupported(
                    AUDCLNT_SHAREMODE_EXCLUSIVE, &integerFormat.Format, nullptr);
            }
            if (support == S_OK) {
                auto* allocated = static_cast<WAVEFORMATEXTENSIBLE*>(
                    CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
                if (!allocated) return E_OUTOFMEMORY;
                *allocated = integerFormat;
                *format = &allocated->Format;
                usingVirtualMix_ = true;
                Log(L"GetMixFormat virtualized for source-rate exclusive " + FormatText(*format));
                return S_OK;
            }
            Log(L"Source-rate integer exclusive format unsupported rate=" +
                std::to_wstring(sourceSampleRate_) + L" result=" + HResultText(support));
        }
        const HRESULT result = inner_->GetMixFormat(format);
        if (SUCCEEDED(result) && format && *format) {
            Log(L"GetMixFormat passthrough " + FormatText(*format));
        }
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* defaultPeriod,
                                              REFERENCE_TIME* minimumPeriod) override {
        std::lock_guard lock(mutex_); return inner_ ? inner_->GetDevicePeriod(defaultPeriod, minimumPeriod) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE Start() override {
        std::lock_guard lock(mutex_);
        if (exclusiveInitialized_) {
            ResetRenderStatistics();
            g_paddingCalls.store(0);
            std::lock_guard endpointLock(g_activeEndpointMutex);
            g_activeEndpoint = endpoint_;
        }
        const HRESULT result = inner_ ? inner_->Start() : E_UNEXPECTED;
        if (SUCCEEDED(result)) streamStarted_.store(true);
        Log(L"IAudioClient::Start result=" + HResultText(result) +
            L" exclusiveInitialized=" + std::to_wstring(exclusiveInitialized_) +
            L" proxy=" + std::to_wstring(proxyId_) +
            L" currentAudioUnit=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(
                g_currentAudioUnit.load())) +
            L" currentAudioUnitEpoch=" + std::to_wstring(
                FindAudioUnitEpoch(g_currentAudioUnit.load())) +
            L" tid=" + std::to_wstring(GetCurrentThreadId()) +
            L" qpc=" + std::to_wstring(CurrentQpc()));
        if (SUCCEEDED(result) && exclusiveInitialized_) {
            RegisterActiveExclusive();
            audioUnit_ = g_currentAudioUnit.load();
            exclusiveStarted_.store(true);
            postStartEvents_.store(0);
            g_exclusiveStartTick.store(GetTickCount64());
            g_exclusiveAwaitingFirstRender.store(true);
            Log(L"Exclusive Start accepted; waiting for first post-Start ReleaseBuffer before Active");
        } else if (SUCCEEDED(result) && !ExclusiveRequested()) {
            SendAgentEvent(ammod::ipc::MessageType::StatusChanged,
                           ammod::ipc::RuntimeState::Off, false, endpoint_);
        }
        return result;
    }
    HRESULT STDMETHODCALLTYPE Stop() override {
        std::lock_guard lock(mutex_);
        if (staleStopDebt_.load() != 0) {
            staleStopDebt_.fetch_sub(1);
            Log(L"IAudioClient::Stop suppressed for retired Apple graph after persistent handoff");
            return S_OK;
        }
        streamStarted_.store(false);
        exclusiveStarted_.store(false);
        const HRESULT result = inner_ ? inner_->Stop() : E_UNEXPECTED;
        Log(L"IAudioClient::Stop result=" + HResultText(result) + L" " + RenderStatisticsText());
        return result;
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        std::lock_guard lock(mutex_);
        const HRESULT result = inner_ ? inner_->Reset() : E_UNEXPECTED;
        Log(L"IAudioClient::Reset result=" + HResultText(result));
        return result;
    }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE eventHandle) override {
        std::lock_guard lock(mutex_);
        HRESULT result = E_UNEXPECTED;
        if (inner_ && exclusiveInitialized_) {
            appleEvent_ = eventHandle;
            if (handoffPending_ && deviceEvent_) {
                handoffPending_ = false;
                result = S_OK;
                Log(L"Exclusive persistent-client handoff updated Apple event handle");
            } else if (reattachDeviceEvent_ && deviceEvent_) {
                result = inner_->SetEventHandle(deviceEvent_);
                if (SUCCEEDED(result)) reattachDeviceEvent_ = false;
                Log(L"Exclusive rate-change rebuilt client attached existing device event result=" +
                    HResultText(result));
            } else {
            if (!deviceEvent_) deviceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!eventBridgeStop_) eventBridgeStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!deviceEvent_ || !eventBridgeStop_) {
                result = HRESULT_FROM_WIN32(GetLastError());
            } else {
                result = inner_->SetEventHandle(deviceEvent_);
                if (SUCCEEDED(result) && !eventBridgeThread_) {
                    eventBridgeThread_ = CreateThread(nullptr, 0, EventBridgeEntry, this, 0, nullptr);
                    if (!eventBridgeThread_) result = HRESULT_FROM_WIN32(GetLastError());
                }
            }
            }
        } else if (inner_) {
            result = inner_->SetEventHandle(eventHandle);
        }
        Log(L"IAudioClient::SetEventHandle handle=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(eventHandle)) +
            L" innerHandle=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(
                exclusiveInitialized_ ? deviceEvent_ : eventHandle)) +
            L" result=" + HResultText(result));
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetService(REFIID iid, void** service) override {
        std::lock_guard lock(mutex_);
        const HRESULT result = inner_ ? inner_->GetService(iid, service) : E_UNEXPECTED;
        Log(L"IAudioClient::GetService iid=" + GuidText(iid) + L" result=" + HResultText(result));
        if (SUCCEEDED(result) && service && *service && exclusiveInitialized_ &&
            iid == __uuidof(IAudioClock)) {
            auto* clock = static_cast<IAudioClock*>(*service);
            UINT64 frequency{};
            const HRESULT frequencyHr = clock->GetFrequency(&frequency);
            Log(L"Exclusive IAudioClock frequency result=" + HResultText(frequencyHr) +
                L" ticksPerSecond=" + std::to_wstring(frequency));
            if (experimentalPumpEnabled_ && !exclusiveClock_) {
                exclusiveClock_ = clock;
                exclusiveClockFrequency_ = frequency;
                exclusiveClock_->AddRef();
            }
            auto* proxy = new (std::nothrow) AudioClockProxy(clock);
            if (!proxy) {
                clock->Release();
                *service = nullptr;
                return E_OUTOFMEMORY;
            }
            TrackService(proxy);
            *service = static_cast<IAudioClock*>(proxy);
            Log(L"IAudioClock wrapped for generation-safe exclusive rebuild");
        }
        if (SUCCEEDED(result) && service && *service && iid == __uuidof(IAudioRenderClient)) {
            if (exclusiveInitialized_ && experimentalPumpEnabled_ && !exclusiveRenderClient_) {
                exclusiveRenderClient_ = static_cast<IAudioRenderClient*>(*service);
                exclusiveRenderClient_->AddRef();
            }
            auto* proxy = new (std::nothrow) RenderClientProxy(
                static_cast<IAudioRenderClient*>(*service),
                &renderConversionSourceBitDepth_, &renderConversionChannels_,
                &renderConversionContainerBits_, &renderFrameBridgeAppleRate_,
                &renderFrameBridgeDeviceRate_, renderLocalQueue_);
            if (!proxy) {
                static_cast<IAudioRenderClient*>(*service)->Release();
                *service = nullptr;
                return E_OUTOFMEMORY;
            }
            TrackService(proxy);
            *service = static_cast<IAudioRenderClient*>(proxy);
            Log(renderFloatToInteger_
                ? L"IAudioRenderClient wrapped for telemetry and lossless float-to-integer restoration"
                : L"IAudioRenderClient wrapped for frame-count/error telemetry; sample data untouched");
        }
        if (SUCCEEDED(result) && service && *service && exclusiveInitialized_ &&
            iid == __uuidof(IAudioStreamVolume)) {
            auto* proxy = new (std::nothrow) AudioStreamVolumeProxy(
                static_cast<IAudioStreamVolume*>(*service));
            if (!proxy) {
                static_cast<IAudioStreamVolume*>(*service)->Release();
                *service = nullptr;
                return E_OUTOFMEMORY;
            }
            TrackService(proxy);
            *service = static_cast<IAudioStreamVolume*>(proxy);
            Log(L"IAudioStreamVolume wrapped for generation-safe exclusive rebuild");
        }
        if (SUCCEEDED(result) && service && *service && exclusiveInitialized_ &&
            iid == __uuidof(IAudioSessionControl)) {
            auto* base = static_cast<IAudioSessionControl*>(*service);
            IAudioSessionControl2* control2{};
            if (SUCCEEDED(base->QueryInterface(__uuidof(IAudioSessionControl2),
                                               reinterpret_cast<void**>(&control2))) && control2) {
                base->Release();
                auto* proxy = new (std::nothrow) AudioSessionControlProxy(control2);
                if (!proxy) {
                    control2->Release();
                    *service = nullptr;
                    return E_OUTOFMEMORY;
                }
                TrackService(proxy);
                *service = static_cast<IAudioSessionControl*>(proxy);
                Log(L"IAudioSessionControl2 wrapped for generation-safe exclusive rebuild");
            }
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE IsOffloadCapable(AUDIO_STREAM_CATEGORY category, BOOL* capable) override {
        std::lock_guard lock(mutex_);
        IAudioClient2* client{};
        const HRESULT hr = GetInner2(&client);
        if (FAILED(hr)) return hr;
        const HRESULT result = client->IsOffloadCapable(category, capable);
        client->Release();
        return result;
    }

    HRESULT STDMETHODCALLTYPE SetClientProperties(const AudioClientProperties* properties) override {
        if (!properties) return E_POINTER;
        std::lock_guard lock(mutex_);
        IAudioClient2* client{};
        const HRESULT hr = GetInner2(&client);
        if (FAILED(hr)) return hr;
        const HRESULT result = client->SetClientProperties(properties);
        client->Release();
        if (SUCCEEDED(result)) {
            cachedProperties_ = *properties;
            hasCachedProperties_ = true;
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE GetBufferSizeLimits(const WAVEFORMATEX* format, BOOL eventDriven,
                                                  REFERENCE_TIME* minimum,
                                                  REFERENCE_TIME* maximum) override {
        std::lock_guard lock(mutex_);
        IAudioClient2* client{};
        const HRESULT hr = GetInner2(&client);
        if (FAILED(hr)) return hr;
        const HRESULT result = client->GetBufferSizeLimits(format, eventDriven, minimum, maximum);
        client->Release();
        return result;
    }

    HRESULT STDMETHODCALLTYPE GetSharedModeEnginePeriod(const WAVEFORMATEX* format,
                                                        UINT32* defaultFrames,
                                                        UINT32* fundamentalFrames,
                                                        UINT32* minimumFrames,
                                                        UINT32* maximumFrames) override {
        std::lock_guard lock(mutex_);
        IAudioClient3* client{};
        const HRESULT hr = GetInner3(&client);
        if (FAILED(hr)) return hr;
        const HRESULT result = client->GetSharedModeEnginePeriod(format, defaultFrames,
                                                                  fundamentalFrames, minimumFrames,
                                                                  maximumFrames);
        client->Release();
        return result;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentSharedModeEnginePeriod(WAVEFORMATEX** format,
                                                               UINT32* frames) override {
        std::lock_guard lock(mutex_);
        IAudioClient3* client{};
        const HRESULT hr = GetInner3(&client);
        if (FAILED(hr)) return hr;
        const HRESULT result = client->GetCurrentSharedModeEnginePeriod(format, frames);
        client->Release();
        return result;
    }

    HRESULT STDMETHODCALLTYPE InitializeSharedAudioStream(DWORD flags, UINT32 periodFrames,
                                                          const WAVEFORMATEX* format,
                                                          LPCGUID sessionGuid) override {
        if (ExclusiveRequested() && format &&
            format->nSamplesPerSec) {
            const REFERENCE_TIME period =
                (10000000LL * periodFrames + format->nSamplesPerSec - 1) / format->nSamplesPerSec;
            Log(L"IAudioClient3::InitializeSharedAudioStream redirected through exclusive policy");
            return Initialize(AUDCLNT_SHAREMODE_SHARED, flags, period, 0, format, sessionGuid);
        }
        std::lock_guard lock(mutex_);
        IAudioClient3* client{};
        const HRESULT hr = GetInner3(&client);
        if (FAILED(hr)) return hr;
        const HRESULT result = client->InitializeSharedAudioStream(flags, periodFrames, format, sessionGuid);
        client->Release();
        Log(L"IAudioClient3::InitializeSharedAudioStream passthrough result=" + HResultText(result));
        return result;
    }

    bool PromotePendingLocalQueueAtDrain(
        const std::shared_ptr<LocalPcmQueue>& drainedQueue,
        std::shared_ptr<LocalPcmQueue>& promotedQueue,
        std::uint64_t& promotedGeneration,
        bool& requiresEndpointRebuild) {
        requiresEndpointRebuild = false;
        std::shared_ptr<LocalPcmQueue> queue;
        std::uint64_t generation{};
        {
            std::lock_guard pendingLock(g_pendingLocalQueueMutex);
            generation = g_pendingLocalQueueGeneration;
            queue = g_pendingLocalQueue;
        }
        if (!drainedQueue || !queue || queue == drainedQueue) return false;

        std::lock_guard lock(mutex_);
        if (generation <= appliedLocalQueueGeneration_ || !exclusiveInitialized_ ||
            !streamStarted_.load() ||
            !renderFrameBridgeAppleRate_.load() || renderLocalQueue_ != drainedQueue) {
            return false;
        }

        std::size_t queuedFrames{};
        bool abandoned{};
        {
            std::lock_guard queueLock(queue->mutex);
            abandoned = queue->abandoned;
            queuedFrames = queue->bytesPerFrame ? queue->sizeBytes / queue->bytesPerFrame : 0;
        }
        if (abandoned || queuedFrames == 0) return false;

        const std::uint16_t targetContainerBits = queue->bitsPerChannel == 16 ? 16 : 32;
        const bool sameEndpointFormat = initializedSampleRate_ == queue->sampleRate &&
            initializedChannels_ == queue->channels && initializedBits_ == targetContainerBits &&
            initializedValidBits_ == targetContainerBits &&
            drainedQueue->bytesPerFrame == queue->bytesPerFrame;
        if (!sameEndpointFormat) {
            requiresEndpointRebuild = true;
            Log(L"Local PCM drained queue found a different-format successor; arming endpoint "
                L"rebuild generation=" + std::to_wstring(generation));
            return false;
        }

        CommitLocalQueueSwitch(queue, generation);
        promotedQueue = queue;
        promotedGeneration = generation;
        Log(L"Local PCM true-drain switch reused exclusive endpoint generation=" +
            std::to_wstring(generation) + L" rate=" + std::to_wstring(queue->sampleRate) +
            L" bits=" + std::to_wstring(queue->bitsPerChannel) + L" queuedFrames=" +
            std::to_wstring(queuedFrames));
        return true;
    }

    bool RebuildPendingLocalQueueAtDrain(
        const std::shared_ptr<LocalPcmQueue>& drainedQueue) {
        {
            std::lock_guard lock(mutex_);
            if (!drainedQueue || renderLocalQueue_ != drainedQueue ||
                !exclusiveInitialized_ || !streamStarted_.load()) return false;
        }
        return ApplyPendingLocalQueueSwitch();
    }

private:
    void RememberInitializedFormat(const WAVEFORMATEX* format) {
        if (!format) return;
        initializedSampleRate_ = format->nSamplesPerSec;
        initializedChannels_ = format->nChannels;
        initializedBits_ = format->wBitsPerSample;
        initializedValidBits_ = WaveValidBits(format);
        initializedBlockAlign_ = format->nBlockAlign;
        startupFormatLocked_ = false;
    }

    void TrackService(RetirableService* service) {
        if (!service) return;
        service->Unknown()->AddRef();
        trackedServices_.push_back(service);
    }

    void RetireTrackedServices() {
        for (auto* service : trackedServices_) {
            service->Retire();
            service->Unknown()->Release();
        }
        trackedServices_.clear();
    }

    void SwitchTrackedLocalQueue(const std::shared_ptr<LocalPcmQueue>& queue) {
        for (auto* service : trackedServices_) {
            service->SwitchLocalQueue(queue);
        }
    }

    void CommitLocalQueueSwitch(const std::shared_ptr<LocalPcmQueue>& queue,
                                std::uint64_t generation) {
        std::shared_ptr<LocalPcmQueue> previous;
        {
            std::lock_guard queueLock(g_activeLocalQueueMutex);
            previous = g_activeLocalQueue;
            g_activeLocalQueue = queue;
        }
        queue->Activate();
        renderLocalQueue_ = queue;
        SwitchTrackedLocalQueue(queue);
        appliedLocalQueueGeneration_ = generation;
        if (previous && previous != queue) previous->Abandon();
    }

    bool ApplyPendingLocalQueueSwitch() {
        std::shared_ptr<LocalPcmQueue> queue;
        std::uint64_t generation{};
        {
            std::lock_guard pendingLock(g_pendingLocalQueueMutex);
            generation = g_pendingLocalQueueGeneration;
            queue = g_pendingLocalQueue;
        }
        if (!queue || generation <= appliedLocalQueueGeneration_) return false;

        std::lock_guard lock(mutex_);
        if (!exclusiveInitialized_ || !streamStarted_.load() ||
            !renderFrameBridgeAppleRate_.load()) return false;
        if (queue == renderLocalQueue_) {
            appliedLocalQueueGeneration_ = generation;
            return false;
        }

        const std::uint16_t targetContainerBits = queue->bitsPerChannel == 16 ? 16 : 32;
        const std::uint16_t targetValidBits = targetContainerBits;
        const bool sameEndpointFormat = initializedSampleRate_ == queue->sampleRate &&
            initializedChannels_ == queue->channels && initializedBits_ == targetContainerBits &&
            initializedValidBits_ == targetValidBits;
        if (sameEndpointFormat) {
            CommitLocalQueueSwitch(queue, generation);
            Log(L"Local PCM lightweight switch reused exclusive endpoint generation=" +
                std::to_wstring(generation) + L" rate=" + std::to_wstring(queue->sampleRate) +
                L" bits=" + std::to_wstring(queue->bitsPerChannel));
            return false;
        }

        const auto target = IntegerPcmFormat(queue->sampleRate, 2, targetValidBits,
                                             targetContainerBits);
        IAudioClient* fresh{};
        HRESULT hr = ActivateFresh(&fresh);
        if (FAILED(hr) || !fresh) {
            Log(L"Local PCM lightweight switch activation failed result=" + HResultText(hr));
            return false;
        }
        hr = fresh->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &target.Format, nullptr);
        if (hr != S_OK) {
            fresh->Release();
            Log(L"Local PCM lightweight switch unsupported format " + FormatText(&target.Format) +
                L" result=" + HResultText(hr));
            return false;
        }

        const HRESULT stopHr = inner_ ? inner_->Stop() : E_UNEXPECTED;
        const HRESULT resetHr = SUCCEEDED(stopHr) && inner_ ? inner_->Reset() : stopHr;
        Log(L"Local PCM lightweight switch stopping old endpoint stop=" + HResultText(stopHr) +
            L" reset=" + HResultText(resetHr));
        streamStarted_.store(false);
        exclusiveStarted_.store(false);
        for (auto* service : trackedServices_) service->Retire();
        if (inner_) {
            inner_->Release();
            inner_ = nullptr;
        }

        REFERENCE_TIME selectedPeriod = ReadPeriod();
        if (selectedPeriod <= 0) selectedPeriod = initializeDuration_ > 0
            ? initializeDuration_ : 100000;
        DWORD exclusiveFlags = initializeFlags_ | AUDCLNT_STREAMFLAGS_NOPERSIST;
        exclusiveFlags &= ~(AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                            AUDCLNT_STREAMFLAGS_LOOPBACK);
        const bool eventDriven = (exclusiveFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
        const REFERENCE_TIME exclusiveDuration = eventDriven ? selectedPeriod : selectedPeriod * 4;
        LPCGUID sessionGuid = hasInitializeSessionGuid_ ? &initializeSessionGuid_ : nullptr;
        constexpr unsigned retryCount = 20;
        for (unsigned attempt = 0;; ++attempt) {
            hr = fresh->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, exclusiveFlags,
                                   exclusiveDuration, selectedPeriod, &target.Format, sessionGuid);
            Log(L"Local PCM lightweight switch Initialize attempt=" +
                std::to_wstring(attempt + 1) + L" result=" + HResultText(hr));
            if (SUCCEEDED(hr)) break;
            fresh->Release();
            fresh = nullptr;
            if (hr != AUDCLNT_E_DEVICE_IN_USE || attempt >= retryCount) {
                DisableExclusiveIntent(L"local_pcm_lightweight_switch_failed", hr, endpoint_,
                                       &target.Format);
                return false;
            }
            Sleep(25);
            const HRESULT activationHr = ActivateFresh(&fresh);
            if (FAILED(activationHr) || !fresh) {
                DisableExclusiveIntent(L"local_pcm_lightweight_switch_activation_failed",
                                       activationHr, endpoint_, &target.Format);
                return false;
            }
        }

        HANDLE replacementDeviceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!replacementDeviceEvent) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            fresh->Release();
            DisableExclusiveIntent(L"local_pcm_lightweight_switch_event_create_failed", hr,
                                   endpoint_, &target.Format);
            return false;
        }
        hr = fresh->SetEventHandle(replacementDeviceEvent);
        if (FAILED(hr)) {
            CloseHandle(replacementDeviceEvent);
            fresh->Release();
            DisableExclusiveIntent(L"local_pcm_lightweight_switch_event_failed", hr, endpoint_,
                                   &target.Format);
            return false;
        }
        UINT32 deviceFrames{};
        hr = fresh->GetBufferSize(&deviceFrames);
        IAudioRenderClient* primeRender{};
        if (SUCCEEDED(hr)) {
            hr = fresh->GetService(__uuidof(IAudioRenderClient),
                                   reinterpret_cast<void**>(&primeRender));
        }
        BYTE* primeBuffer{};
        if (SUCCEEDED(hr) && primeRender) hr = primeRender->GetBuffer(deviceFrames, &primeBuffer);
        bool primeExact{};
        if (SUCCEEDED(hr) && primeRender && primeBuffer) {
            primeExact = queue->Pop(primeBuffer, deviceFrames);
            if ((queue->formatFlags & ammod::audio::kFormatFlagIsFloat) != 0 &&
                queue->bitsPerChannel == 32) {
                ammod::audio::Float32ToLeftAlignedPcm32InPlace(
                    primeBuffer, static_cast<std::size_t>(deviceFrames) * queue->channels, 32);
            }
            hr = primeRender->ReleaseBuffer(deviceFrames, 0);
        }
        if (primeRender) primeRender->Release();
        if (FAILED(hr) || !primeBuffer) {
            CloseHandle(replacementDeviceEvent);
            fresh->Release();
            DisableExclusiveIntent(L"local_pcm_lightweight_switch_preroll_failed", hr,
                                   endpoint_, &target.Format);
            return false;
        }
        Log(L"Local PCM lightweight switch pre-rolled endpoint deviceFrames=" +
            std::to_wstring(deviceFrames) + L" exact=" + std::to_wstring(primeExact));
        bool rebindFailed{};
        for (auto* service : trackedServices_) {
            const HRESULT serviceHr = service->Rebind(fresh);
            if (FAILED(serviceHr)) {
                rebindFailed = true;
                Log(L"Local PCM lightweight switch service rebind failed result=" +
                    HResultText(serviceHr));
            }
        }
        if (rebindFailed) {
            CloseHandle(replacementDeviceEvent);
            fresh->Release();
            DisableExclusiveIntent(L"local_pcm_lightweight_switch_rebind_failed", E_FAIL,
                                   endpoint_, &target.Format);
            return false;
        }

        inner_ = fresh;
        RefreshCapabilities();
        RememberInitializedFormat(&target.Format);
        renderFrameBridgeDeviceRate_ = queue->sampleRate;
        renderConversionSourceBitDepth_ = queue->bitsPerChannel;
        renderConversionChannels_ = static_cast<std::uint16_t>(queue->channels);
        renderConversionContainerBits_ = targetContainerBits;
        bufferFrames_ = deviceFrames;
        CommitLocalQueueSwitch(queue, generation);
        hr = inner_->Start();
        if (FAILED(hr)) {
            CloseHandle(replacementDeviceEvent);
            DisableExclusiveIntent(L"local_pcm_lightweight_switch_start_failed", hr, endpoint_,
                                   &target.Format);
            return false;
        }
        const HANDLE retiredDeviceEvent = deviceEvent_;
        if (retiredDeviceEvent) retiredDeviceEvents_.push_back(retiredDeviceEvent);
        deviceEvent_ = replacementDeviceEvent;
        streamStarted_.store(true);
        exclusiveStarted_.store(true);
        // A cross-format rebuild can run on Apple's render thread while EventBridgeLoop is blocked
        // indefinitely on the old endpoint event. Wake that retired handle once so the bridge loop
        // returns, reloads deviceEvent_, and starts waiting on the replacement endpoint immediately.
        const bool eventBridgeWoken = retiredDeviceEvent && SetEvent(retiredDeviceEvent);
        Log(L"Local PCM lightweight switch rebuilt exclusive endpoint generation=" +
            std::to_wstring(generation) + L" rate=" + std::to_wstring(queue->sampleRate) +
            L" bits=" + std::to_wstring(queue->bitsPerChannel) + L" deviceFrames=" +
            std::to_wstring(deviceFrames) + L" eventBridgeWoken=" +
            std::to_wstring(eventBridgeWoken));
        return true;
    }

    HRESULT RebuildPersistentExclusive(DWORD flags, REFERENCE_TIME duration,
                                       const WAVEFORMATEX* format, LPCGUID sessionGuid) {
        Log(L"Exclusive persistent client rate change oldRate=" +
            std::to_wstring(initializedSampleRate_) + L" newRate=" +
            std::to_wstring(format->nSamplesPerSec));
        IAudioClient* fresh{};
        HRESULT hr = ActivateFresh(&fresh);
        if (FAILED(hr) || !fresh) return hr;
        hr = fresh->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, format, nullptr);
        if (hr != S_OK) {
            fresh->Release();
            return hr;
        }

        if (streamStarted_.exchange(false)) inner_->Stop();
        inner_->Reset();
        RetireTrackedServices();
        IAudioClient* retired = inner_;
        inner_ = nullptr;
        retired->Release();

        REFERENCE_TIME selectedPeriod = ReadPeriod();
        if (selectedPeriod <= 0) selectedPeriod = duration > 0 ? duration : 100000;
        DWORD exclusiveFlags = flags | AUDCLNT_STREAMFLAGS_NOPERSIST;
        exclusiveFlags &= ~(AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                            AUDCLNT_STREAMFLAGS_LOOPBACK);
        const bool eventDriven = (exclusiveFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0;
        const REFERENCE_TIME exclusiveDuration = eventDriven ? selectedPeriod : selectedPeriod * 4;
        constexpr unsigned releaseRetryCount = 20;
        for (unsigned attempt = 0;; ++attempt) {
            hr = fresh->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, exclusiveFlags,
                                   exclusiveDuration, selectedPeriod, format, sessionGuid);
            Log(L"Exclusive persistent rate-change Initialize attempt=" +
                std::to_wstring(attempt + 1) + L" result=" + HResultText(hr));
            if (SUCCEEDED(hr)) break;
            fresh->Release();
            fresh = nullptr;
            if (hr != AUDCLNT_E_DEVICE_IN_USE || attempt >= releaseRetryCount) return hr;
            Sleep(25);
            const HRESULT activationHr = ActivateFresh(&fresh);
            if (FAILED(activationHr) || !fresh) return activationHr;
        }
        inner_ = fresh;
        RefreshCapabilities();
        exclusiveInitialized_ = true;
        exclusiveStarted_.store(false);
        handoffPending_ = false;
        reattachDeviceEvent_ = true;
        RememberInitializedFormat(format);
        return S_OK;
    }

    void RegisterActiveExclusive() {
        AddRef();
        AudioClientProxy* previous{};
        {
            std::lock_guard registryLock(g_activeExclusiveProxyMutex);
            if (g_activeExclusiveProxy == this) {
                Release();
                return;
            }
            previous = g_activeExclusiveProxy;
            g_activeExclusiveProxy = this;
        }
        if (previous) previous->Release();
        Log(L"Registered process-persistent exclusive audio client proxy=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(this)));
    }

    ~AudioClientProxy() {
        std::lock_guard lock(mutex_);
        if (eventBridgeStop_) SetEvent(eventBridgeStop_);
        if (eventBridgeThread_) {
            WaitForSingleObject(eventBridgeThread_, 2000);
            CloseHandle(eventBridgeThread_);
        }
        if (deviceEvent_) CloseHandle(deviceEvent_);
        for (const auto eventHandle : retiredDeviceEvents_) {
            if (eventHandle) CloseHandle(eventHandle);
        }
        if (eventBridgeStop_) CloseHandle(eventBridgeStop_);
        if (exclusiveRenderClient_) exclusiveRenderClient_->Release();
        if (exclusiveClock_) exclusiveClock_->Release();
        RetireTrackedServices();
        if (inner_) inner_->Release();
        if (device_) device_->Release();
        if (hasActivationParams_) PropVariantClear(&activationParams_);
    }

    static DWORD WINAPI EventBridgeEntry(void* context) {
        static_cast<AudioClientProxy*>(context)->EventBridgeLoop();
        return 0;
    }

    void EventBridgeLoop() {
        DWORD taskIndex{};
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
        Log(L"Exclusive event/render thread MMCSS=" + std::to_wstring(mmcss != nullptr));
        HANDLE events[2]{eventBridgeStop_, deviceEvent_};
        std::int64_t previousEventQpc{};
        for (;;) {
            events[1] = deviceEvent_;
            const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0 || wait != WAIT_OBJECT_0 + 1) break;
            LARGE_INTEGER eventQpc{};
            QueryPerformanceCounter(&eventQpc);
            const auto frequency = g_qpcFrequency.load();
            if (previousEventQpc && frequency > 0) {
                const auto gapUs = static_cast<std::uint64_t>(
                    (eventQpc.QuadPart - previousEventQpc) * 1000000LL / frequency);
                UpdateMaximum(g_maxDeviceEventGapUs, gapUs);
                const auto expected = g_expectedPeriodUs.load();
                if (expected && gapUs > expected + expected / 2) g_lateDeviceEvents.fetch_add(1);
            }
            previousEventQpc = eventQpc.QuadPart;
            g_deviceEventSignals.fetch_add(1);
            if (exclusiveStarted_.load()) {
                if (experimentalPumpEnabled_ && modPumpActive_.load()) {
                    if (PumpExclusiveBuffer()) continue;
                } else if (experimentalPumpEnabled_ && g_renderGetCalls.load() == 0) {
                    const auto eventNumber = postStartEvents_.fetch_add(1) + 1;
                    if (eventNumber > 10 && PumpExclusiveBuffer()) {
                        modPumpActive_.store(true);
                        continue;
                    }
                }
            }
            if (appleEvent_ && SetEvent(appleEvent_)) g_forwardedAppleEventSignals.fetch_add(1);
            else g_failedAppleEventSignals.fetch_add(1);
        }
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    bool PumpExclusiveBuffer() {
        LARGE_INTEGER pumpStart{};
        QueryPerformanceCounter(&pumpStart);
        if (!exclusiveRenderClient_ || !audioUnit_ || !bufferFrames_) return false;
        if (!pumpTimestampReady_) {
            std::lock_guard lock(g_audioUnitRenderTemplateMutex);
            if (!g_hasAudioUnitRenderTemplate) return false;
            pumpTimestamp_ = g_audioUnitRenderTimestamp;
            pumpBaseHostTime_ = g_audioUnitRenderTimestamp.mHostTime;
            pumpBus_ = g_audioUnitRenderBus;
            pumpTemplateQpc_ = g_audioUnitRenderTemplateQpc;
            pumpTimestampReady_ = true;
        }

        BYTE* data{};
        const HRESULT getHr = exclusiveRenderClient_->GetBuffer(bufferFrames_, &data);
        g_renderGetCalls.fetch_add(1);
        g_renderRequestedFrames.fetch_add(bufferFrames_);
        g_renderLastRequestedFrames.store(bufferFrames_);
        g_renderLastGetResult.store(getHr);
        if (FAILED(getHr) || !data) {
            Log(L"Exclusive Mod pump GetBuffer failed result=" + HResultText(getHr));
            exclusiveStarted_.store(false);
            DisableExclusiveIntent(L"exclusive_pump_get_buffer_failed", getHr, endpoint_);
            return false;
        }

        bool clockAnchored{};
        if (exclusiveClock_ && exclusiveClockFrequency_ && sourceSampleRate_) {
            UINT64 devicePosition{}, qpcPosition100ns{};
            if (SUCCEEDED(exclusiveClock_->GetPosition(&devicePosition, &qpcPosition100ns))) {
                pumpTimestamp_.mSampleTime =
                    static_cast<double>(devicePosition) * sourceSampleRate_ /
                    static_cast<double>(exclusiveClockFrequency_) + bufferFrames_;
                const auto frequency = g_qpcFrequency.load();
                if (frequency > 0) {
                    pumpTimestamp_.mHostTime = qpcPosition100ns *
                        static_cast<std::uint64_t>(frequency) / 10000000ULL;
                }
                clockAnchored = true;
            }
        }
        if (!clockAnchored) pumpTimestamp_.mSampleTime += bufferFrames_;
        if (!clockAnchored && pumpTemplateQpc_ != 0) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            pumpTimestamp_.mHostTime = pumpBaseHostTime_ +
                static_cast<std::uint64_t>(now.QuadPart - pumpTemplateQpc_);
        }
        std::uint32_t actionFlags{};
        AppleAudioBufferList buffers{};
        buffers.mNumberBuffers = 1;
        buffers.mBuffers[0].mNumberChannels = sourceChannels_ ? sourceChannels_ : 2;
        buffers.mBuffers[0].mDataByteSize = bufferFrames_ *
            static_cast<std::uint32_t>(sizeof(std::int32_t)) * buffers.mBuffers[0].mNumberChannels;
        buffers.mBuffers[0].mData = data;
        const AppleOSStatus renderResult = g_originalAudioUnitRender(
            audioUnit_, &actionFlags, &pumpTimestamp_, pumpBus_, bufferFrames_, &buffers);
        g_audioUnitRenderCalls.fetch_add(1);
        g_lastRenderActionFlags.store(actionFlags);
        if (actionFlags != 0) g_nonzeroRenderActionFlags.fetch_add(1);

        const DWORD releaseFlags = renderResult == 0 ? 0 : AUDCLNT_BUFFERFLAGS_SILENT;
        const HRESULT releaseHr = exclusiveRenderClient_->ReleaseBuffer(bufferFrames_, releaseFlags);
        LARGE_INTEGER pumpEnd{};
        QueryPerformanceCounter(&pumpEnd);
        const auto frequency = g_qpcFrequency.load();
        if (frequency > 0) {
            UpdateMaximum(g_maxPumpDurationUs, static_cast<std::uint64_t>(
                (pumpEnd.QuadPart - pumpStart.QuadPart) * 1000000LL / frequency));
        }
        g_renderReleaseCalls.fetch_add(1);
        g_renderWrittenFrames.fetch_add(bufferFrames_);
        g_renderLastWrittenFrames.store(bufferFrames_);
        g_renderLastReleaseResult.store(releaseHr);
        const auto pumpCall = g_modPumpCalls.fetch_add(1) + 1;
        if (pumpCall == 1 || renderResult != 0 || FAILED(releaseHr)) {
            Log(L"Exclusive Mod pump call=" + std::to_wstring(pumpCall) +
                L" frames=" + std::to_wstring(bufferFrames_) +
                L" renderResult=" + std::to_wstring(renderResult) +
                L" actionFlags=0x" + HResultText(static_cast<HRESULT>(actionFlags)).substr(2) +
                L" clockAnchored=" + std::to_wstring(clockAnchored) +
                L" sampleTime=" + std::to_wstring(pumpTimestamp_.mSampleTime) +
                L" hostTime=" + std::to_wstring(pumpTimestamp_.mHostTime) +
                L" releaseResult=" + HResultText(releaseHr));
        }
        if (renderResult != 0 || FAILED(releaseHr)) {
            exclusiveStarted_.store(false);
            const HRESULT failure = FAILED(releaseHr) ? releaseHr : E_FAIL;
            DisableExclusiveIntent(L"exclusive_pump_render_failed", failure, endpoint_);
            return false;
        }
        ReportSuccessfulRenderAfterStart();
        return true;
    }

    void RefreshCapabilities() {
        supports2_ = false;
        supports3_ = false;
        if (!inner_) return;
        IAudioClient2* client2{};
        if (SUCCEEDED(inner_->QueryInterface(__uuidof(IAudioClient2), reinterpret_cast<void**>(&client2)))) {
            supports2_ = true;
            client2->Release();
        }
        IAudioClient3* client3{};
        if (SUCCEEDED(inner_->QueryInterface(__uuidof(IAudioClient3), reinterpret_cast<void**>(&client3)))) {
            supports3_ = true;
            client3->Release();
        }
    }

    HRESULT GetInner2(IAudioClient2** client) {
        if (!client) return E_POINTER;
        *client = nullptr;
        return inner_ ? inner_->QueryInterface(__uuidof(IAudioClient2),
                                                reinterpret_cast<void**>(client)) : E_NOINTERFACE;
    }

    HRESULT GetInner3(IAudioClient3** client) {
        if (!client) return E_POINTER;
        *client = nullptr;
        return inner_ ? inner_->QueryInterface(__uuidof(IAudioClient3),
                                                reinterpret_cast<void**>(client)) : E_NOINTERFACE;
    }

    HRESULT ActivateFresh(IAudioClient** fresh) {
        if (!fresh) return E_POINTER;
        *fresh = nullptr;
        if (!device_ || !g_originalDeviceActivate) return E_UNEXPECTED;
        const HRESULT hr = g_originalDeviceActivate(
            device_, __uuidof(IAudioClient), activationContext_,
            hasActivationParams_ ? &activationParams_ : nullptr, reinterpret_cast<void**>(fresh));
        if (SUCCEEDED(hr) && *fresh && hasCachedProperties_) {
            IAudioClient2* client2{};
            if (SUCCEEDED((*fresh)->QueryInterface(__uuidof(IAudioClient2),
                                                   reinterpret_cast<void**>(&client2)))) {
                const HRESULT propertyHr = client2->SetClientProperties(&cachedProperties_);
                client2->Release();
                if (FAILED(propertyHr)) {
                    (*fresh)->Release();
                    *fresh = nullptr;
                    return propertyHr;
                }
            }
        }
        return hr;
    }

    void ReplaceInner(IAudioClient* fresh) {
        IAudioClient* previous = inner_;
        inner_ = fresh;
        RefreshCapabilities();
        if (previous) previous->Release();
    }

    HRESULT InitializeFreshShared(AUDCLNT_SHAREMODE shareMode, DWORD flags,
                                  REFERENCE_TIME duration, REFERENCE_TIME periodicity,
                                  const WAVEFORMATEX* format, LPCGUID sessionGuid,
                                  HRESULT exclusiveFailure) {
        renderFloatToInteger_ = false;
        renderConversionSourceBitDepth_ = 0;
        renderConversionChannels_ = 0;
        renderConversionContainerBits_ = 0;
        renderFrameBridgeAppleRate_ = 0;
        renderFrameBridgeDeviceRate_ = 0;
        renderLocalQueue_.reset();
        if (!ReadBool(L"fallback_shared", true)) return exclusiveFailure;
        IAudioClient* fresh{};
        const HRESULT activationHr = ActivateFresh(&fresh);
        if (FAILED(activationHr) || !fresh) {
            Log(L"Fresh shared fallback activation failed result=" + HResultText(activationHr));
            return activationHr;
        }
        DWORD fallbackFlags = flags;
        if (usingVirtualMix_ && shareMode == AUDCLNT_SHAREMODE_SHARED) {
            fallbackFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                             AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        }
        const HRESULT fallbackHr = fresh->Initialize(shareMode, fallbackFlags, duration, periodicity,
                                                     format, sessionGuid);
        Log(L"Fresh shared fallback Initialize result=" + HResultText(fallbackHr));
        if (SUCCEEDED(fallbackHr)) ReplaceInner(fresh);
        else fresh->Release();
        return fallbackHr;
    }

    std::atomic<ULONG> references_{1};
    std::recursive_mutex mutex_;
    IAudioClient* inner_{};
    IMMDevice* device_{};
    DWORD activationContext_{};
    std::uint64_t proxyId_{};
    PROPVARIANT activationParams_{};
    bool hasActivationParams_{};
    bool supports2_{};
    bool supports3_{};
    bool hasCachedProperties_{};
    AudioClientProperties cachedProperties_{};
    bool exclusiveInitialized_{};
    bool handoffPending_{};
    bool reattachDeviceEvent_{};
    bool startupFormatLocked_{};
    bool bufferSizeLogged_{};
    bool usingVirtualMix_{};
    bool renderFloatToInteger_{};
    std::atomic<std::uint32_t> renderConversionSourceBitDepth_{};
    std::atomic<std::uint16_t> renderConversionChannels_{};
    std::atomic<std::uint16_t> renderConversionContainerBits_{};
    std::atomic<std::uint32_t> renderFrameBridgeAppleRate_{};
    std::atomic<std::uint32_t> renderFrameBridgeDeviceRate_{};
    std::shared_ptr<LocalPcmQueue> renderLocalQueue_;
    UINT32 bufferFrames_{};
    IAudioRenderClient* exclusiveRenderClient_{};
    IAudioClock* exclusiveClock_{};
    UINT64 exclusiveClockFrequency_{};
    void* audioUnit_{};
    std::atomic<bool> exclusiveStarted_{};
    std::atomic<bool> streamStarted_{};
    std::atomic<unsigned> staleStopDebt_{};
    std::atomic<bool> modPumpActive_{};
    std::atomic<std::uint32_t> postStartEvents_{};
    AppleAudioTimeStamp pumpTimestamp_{};
    std::uint32_t pumpBus_{};
    std::int64_t pumpTemplateQpc_{};
    std::uint64_t pumpBaseHostTime_{};
    bool pumpTimestampReady_{};
    bool experimentalPumpEnabled_{};
    HANDLE appleEvent_{};
    HANDLE deviceEvent_{};
    std::vector<HANDLE> retiredDeviceEvents_;
    HANDLE eventBridgeStop_{};
    HANDLE eventBridgeThread_{};
    std::uint32_t sourceSampleRate_{};
    std::uint16_t sourceChannels_{};
    std::uint32_t sourceBitDepth_{};
    std::uint32_t initializedSampleRate_{};
    std::uint16_t initializedChannels_{};
    std::uint16_t initializedBits_{};
    std::uint16_t initializedValidBits_{};
    std::uint16_t initializedBlockAlign_{};
    std::wstring endpoint_;
    std::vector<RetirableService*> trackedServices_;
    std::uint64_t appliedLocalQueueGeneration_{};
    DWORD initializeFlags_{};
    REFERENCE_TIME initializeDuration_{};
    GUID initializeSessionGuid_{};
    bool hasInitializeSessionGuid_{};
};

bool PromotePendingLocalQueueAtDrain(
    const std::shared_ptr<LocalPcmQueue>& drainedQueue,
    std::shared_ptr<LocalPcmQueue>& promotedQueue,
    std::uint64_t& promotedGeneration,
    bool& requiresEndpointRebuild) {
    AudioClientProxy* active{};
    {
        std::lock_guard registryLock(g_activeExclusiveProxyMutex);
        active = g_activeExclusiveProxy;
        if (active) active->AddRef();
    }
    if (!active) return false;
    const bool promoted = active->PromotePendingLocalQueueAtDrain(
        drainedQueue, promotedQueue, promotedGeneration, requiresEndpointRebuild);
    active->Release();
    return promoted;
}

bool RebuildPendingLocalQueueAtDrain(
    const std::shared_ptr<LocalPcmQueue>& drainedQueue) {
    AudioClientProxy* active{};
    {
        std::lock_guard registryLock(g_activeExclusiveProxyMutex);
        active = g_activeExclusiveProxy;
        if (active) active->AddRef();
    }
    if (!active) return false;
    const bool rebuilt = active->RebuildPendingLocalQueueAtDrain(drainedQueue);
    active->Release();
    return rebuilt;
}


HRESULT STDMETHODCALLTYPE HookDeviceActivate(IMMDevice* self, REFIID iid, DWORD context,
                                              PROPVARIANT* params, void** object) {
    const HRESULT hr = g_originalDeviceActivate(self, iid, context, params, object);
    if (SUCCEEDED(hr) && object && *object &&
        (iid == __uuidof(IAudioClient) || iid == __uuidof(IAudioClient2) ||
         iid == __uuidof(IAudioClient3))) {
        const auto endpoint = DeviceId(self);
        if (ExclusiveRequested()) {
            AudioClientProxy* active{};
            {
                std::lock_guard registryLock(g_activeExclusiveProxyMutex);
                active = g_activeExclusiveProxy;
                if (active) active->AddRef();
            }
            if (active && active->CanReuseExclusive(endpoint)) {
                void* reused{};
                const HRESULT reuseHr = active->QueryInterface(iid, &reused);
                active->Release();
                if (SUCCEEDED(reuseHr) && reused) {
                    static_cast<IUnknown*>(*object)->Release();
                    *object = reused;
                    Log(L"IMMDevice::Activate reused process-persistent exclusive client iid=" +
                        GuidText(iid) + L" endpoint=" + endpoint +
                        L" returned=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(reused)) +
                        L" tid=" + std::to_wstring(GetCurrentThreadId()) +
                        L" qpc=" + std::to_wstring(CurrentQpc()));
                    return S_OK;
                }
            } else if (active) {
                active->Release();
            }
        }
        auto* returned = reinterpret_cast<IUnknown*>(*object);
        IAudioClient* client{};
        const HRESULT baseHr = returned->QueryInterface(__uuidof(IAudioClient),
                                                        reinterpret_cast<void**>(&client));
        if (FAILED(baseHr) || !client) {
            Log(L"IMMDevice::Activate audio interface could not query IAudioClient result=" +
                HResultText(baseHr));
            return hr;
        }
        auto* proxy = new (std::nothrow) AudioClientProxy(client, self, context, params, endpoint);
        if (!proxy) {
            client->Release();
            returned->Release();
            *object = nullptr;
            return E_OUTOFMEMORY;
        }
        void* wrapped{};
        const HRESULT wrapHr = proxy->QueryInterface(iid, &wrapped);
        proxy->Release();
        if (FAILED(wrapHr) || !wrapped) {
            returned->Release();
            *object = nullptr;
            return wrapHr;
        }
        returned->Release();
        *object = wrapped;
        Log(L"IMMDevice::Activate returned proxied audio client iid=" + GuidText(iid) +
            L" endpoint=" + endpoint +
            L" returned=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(wrapped)) +
            L" tid=" + std::to_wstring(GetCurrentThreadId()) +
            L" qpc=" + std::to_wstring(CurrentQpc()));
    }
    return hr;
}

void HookDevice(IMMDevice* device) {
    void** table = Vtable(device);
    if (table) HookAddress(table[3], reinterpret_cast<void*>(&HookDeviceActivate), g_originalDeviceActivate,
                           L"IMMDevice::Activate");
}

HRESULT STDMETHODCALLTYPE HookCollectionItem(IMMDeviceCollection* self, UINT index, IMMDevice** device) {
    const HRESULT hr = g_originalCollectionItem(self, index, device);
    if (SUCCEEDED(hr) && device && *device) HookDevice(*device);
    return hr;
}

void HookCollection(IMMDeviceCollection* collection) {
    void** table = Vtable(collection);
    if (table) HookAddress(table[4], reinterpret_cast<void*>(&HookCollectionItem), g_originalCollectionItem,
                           L"IMMDeviceCollection::Item");
}

HRESULT STDMETHODCALLTYPE HookEnumAudioEndpoints(IMMDeviceEnumerator* self, EDataFlow flow,
                                                  DWORD state, IMMDeviceCollection** collection) {
    const HRESULT hr = g_originalEnumAudioEndpoints(self, flow, state, collection);
    if (SUCCEEDED(hr) && collection && *collection) HookCollection(*collection);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookGetDefaultEndpoint(IMMDeviceEnumerator* self, EDataFlow flow,
                                                  ERole role, IMMDevice** device) {
    const HRESULT hr = g_originalGetDefaultEndpoint(self, flow, role, device);
    if (SUCCEEDED(hr) && device && *device) HookDevice(*device);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookGetDevice(IMMDeviceEnumerator* self, LPCWSTR id, IMMDevice** device) {
    const HRESULT hr = g_originalGetDevice(self, id, device);
    if (SUCCEEDED(hr) && device && *device) HookDevice(*device);
    return hr;
}

void HookEnumerator(IMMDeviceEnumerator* enumerator) {
    void** table = Vtable(enumerator);
    if (!table) return;
    HookAddress(table[3], reinterpret_cast<void*>(&HookEnumAudioEndpoints), g_originalEnumAudioEndpoints,
                L"IMMDeviceEnumerator::EnumAudioEndpoints");
    HookAddress(table[4], reinterpret_cast<void*>(&HookGetDefaultEndpoint), g_originalGetDefaultEndpoint,
                L"IMMDeviceEnumerator::GetDefaultAudioEndpoint");
    HookAddress(table[5], reinterpret_cast<void*>(&HookGetDevice), g_originalGetDevice,
                L"IMMDeviceEnumerator::GetDevice");
}

HRESULT WINAPI HookCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context,
                                    REFIID iid, LPVOID* object) {
    const HRESULT hr = g_originalCoCreateInstance(clsid, outer, context, iid, object);
    if (SUCCEEDED(hr) && object && *object && iid == __uuidof(IMMDeviceEnumerator)) {
        HookEnumerator(static_cast<IMMDeviceEnumerator*>(*object));
    }
    return hr;
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
                g_ipcEnabled.store(enabled, std::memory_order_release);
                if (!enabled) g_nativeP0Puller.DisableForUiOff();
                g_audioCoreGate.SetUiEnabled(enabled);
                g_audioCoreRuntime.SetUiEnabled(enabled);
                Log(L"broker status received enabled=" +
                    std::to_wstring(incoming.enabledIntent) + L" state=" +
                    std::to_wstring(static_cast<unsigned>(incoming.state)));
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
        Log(L"broker IPC disconnected; exclusive intent forced off");
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
    const auto lockedRateText = ReadSetting(L"locked_source_rate", L"0");
    const auto lockedChannelsText = ReadSetting(L"locked_source_channels", L"0");
    const auto lockedRate = wcstoul(lockedRateText.c_str(), nullptr, 10);
    const auto lockedChannels = wcstoul(lockedChannelsText.c_str(), nullptr, 10);
    if (lockedRate >= 8000 && lockedRate <= 768000 &&
        lockedChannels > 0 && lockedChannels <= 8) {
        g_startupLockedSampleRate.store(lockedRate);
        g_startupLockedChannels.store(lockedChannels);
    }
    WritePrivateProfileStringW(L"mod", L"locked_source_rate", nullptr, g_iniPath.c_str());
    WritePrivateProfileStringW(L"mod", L"locked_source_channels", nullptr, g_iniPath.c_str());
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
    if (g_startupLockedSampleRate.load()) {
        Log(L"consumed one-shot startup QLAC format rate=" +
            std::to_wstring(g_startupLockedSampleRate.load()) + L" channels=" +
            std::to_wstring(g_startupLockedChannels.load()));
    }
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency)) g_qpcFrequency.store(frequency.QuadPart);

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
