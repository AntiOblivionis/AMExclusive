#include "AsioExclusiveSink.h"
#include "LocalPlaybackTuning.h"

#include "../AudioErrors.h"
#include "DlnaLog.h"

#include <objbase.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>

namespace ammod::audio_v2 {
namespace {

// ASIO callbacks carry no user context, so the single live sink is published
// here. The coordinator owns exactly one sink at a time.
std::atomic<AsioExclusiveSink*> g_activeSink{nullptr};

HRESULT HResultFromAsio(asio::ASIOError error) noexcept {
    return error == asio::ASE_OK ? S_OK : static_cast<HRESULT>(error);
}

// ASIO reports granularity -1 for power-of-two sizes, >1 for a multiple, and
// 0 when any size in range is accepted.
std::uint32_t ChooseBufferSize(std::uint32_t requested, long minSize, long maxSize,
                               long granularity) noexcept {
    if (requested == 0) return 0;
    if (minSize > 0 && requested < static_cast<std::uint32_t>(minSize)) {
        requested = static_cast<std::uint32_t>(minSize);
    }
    if (maxSize > 0 && requested > static_cast<std::uint32_t>(maxSize)) {
        requested = static_cast<std::uint32_t>(maxSize);
    }
    if (granularity == -1) {
        std::uint32_t power = 1;
        while (power <= requested / 2u) power <<= 1u;
        requested = power;
    } else if (granularity > 1) {
        const auto multiple = static_cast<std::uint32_t>(granularity);
        requested = (requested / multiple) * multiple;
        if (requested == 0) requested = multiple;
    }
    if (minSize > 0 && requested < static_cast<std::uint32_t>(minSize)) {
        requested = static_cast<std::uint32_t>(minSize);
    }
    return requested;
}

void Detail(const std::wstring& text) {
    LogDlnaLine(L"asio " + text);
}

} // namespace

AsioExclusiveSink::~AsioExclusiveSink() {
    Close();
}

HRESULT AsioExclusiveSink::Open(const AudioSinkOpenParams& params) noexcept {
    Close();
    params_ = params;
    format_ = params.format;
    threadTuningApplied_.store(false, std::memory_order_release);
    NotifyLifecycle(AudioSinkLifecycleEvent::OpenEnter, S_OK);

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
    eosCallbacksRemaining_.store(0, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    appliedSampleRate_.store(0, std::memory_order_release);

    if (!IsValidFormat(format_)) {
        RecordError(E_INVALIDARG);
        NotifyLifecycle(AudioSinkLifecycleEvent::OpenExit, E_INVALIDARG);
        return E_INVALIDARG;
    }
    if (params_.asioDriverClsid.empty()) {
        Detail(L"open failed: no ASIO driver selected");
        RecordError(E_INVALIDARG);
        NotifyLifecycle(AudioSinkLifecycleEvent::OpenExit, E_INVALIDARG);
        return E_INVALIDARG;
    }

    HRESULT hr = LoadDriver();
    if (SUCCEEDED(hr)) hr = ApplySampleRate();
    if (SUCCEEDED(hr)) hr = CreateDriverBuffers();
    if (FAILED(hr)) {
        RecordError(hr);
        ReleaseDriver();
        NotifyLifecycle(AudioSinkLifecycleEvent::OpenExit, hr);
        return hr;
    }

    state_.store(AudioSinkState::Open, std::memory_order_release);
    NotifyLifecycle(AudioSinkLifecycleEvent::OpenExit, S_OK);
    return S_OK;
}

HRESULT AsioExclusiveSink::LoadDriver() noexcept {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(apartment)) {
        comInitialized_ = true;
    } else if (apartment != RPC_E_CHANGED_MODE) {
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioDriverLoad, apartment);
        return apartment;
    }

    CLSID clsid{};
    if (CLSIDFromString(params_.asioDriverClsid.c_str(), &clsid) != S_OK) {
        Detail(L"invalid driver CLSID=" + params_.asioDriverClsid);
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioDriverLoad, E_INVALIDARG);
        return E_INVALIDARG;
    }

    asio::IASIO* driver = nullptr;
    const HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                        clsid, reinterpret_cast<void**>(&driver));
    if (FAILED(hr) || !driver) {
        const HRESULT failure = FAILED(hr) ? hr : E_FAIL;
        Detail(L"CoCreateInstance failed clsid=" + params_.asioDriverClsid +
               L" hr=" + std::to_wstring(static_cast<unsigned long>(failure)));
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioDriverLoad, failure);
        return failure;
    }
    driver_ = driver;

    if (driver_->init(GetDesktopWindow()) != asio::ASIOTrue) {
        char message[256]{};
        driver_->getErrorMessage(message);
        message[255] = '\0';
        Detail(L"driver init failed message=" +
               std::wstring(message, message + std::strlen(message)));
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioDriverLoad, E_FAIL);
        return E_FAIL;
    }

    char driverName[64]{};
    driver_->getDriverName(driverName);
    driverName[63] = '\0';
    const long driverVersion = driver_->getDriverVersion();
    Detail(L"driver loaded name=" +
           std::wstring(driverName, driverName + std::strlen(driverName)) +
           L" version=" + std::to_wstring(driverVersion) +
           L" requested=" + params_.asioDriverName + L" rate=" +
           std::to_wstring(format_.sampleRate));

    long inputs = 0;
    long outputs = 0;
    const auto channels = driver_->getChannels(&inputs, &outputs);
    if (channels != asio::ASE_OK) {
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioDriverLoad, HResultFromAsio(channels));
        return HResultFromAsio(channels);
    }
    if (outputs < static_cast<long>(kSupportedChannels)) {
        Detail(L"driver has too few outputs count=" + std::to_wstring(outputs));
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioDriverLoad, E_FAIL);
        return E_FAIL;
    }

    NotifyLifecycle(AudioSinkLifecycleEvent::AsioDriverLoad, S_OK);
    return S_OK;
}

HRESULT AsioExclusiveSink::ApplySampleRate() noexcept {
    if (!driver_) return E_UNEXPECTED;
    const auto requested = format_.sampleRate;

    const auto canSampleRate = driver_->canSampleRate(
        static_cast<asio::ASIOSampleRate>(requested));
    if (canSampleRate != asio::ASE_OK) {
        Detail(L"sample rate rejected rate=" + std::to_wstring(requested) +
               L" asioError=" + std::to_wstring(canSampleRate));
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioSampleRate,
                        HResultFromAsio(canSampleRate));
        return ammod::audio::kBitPerfectFormatUnavailable;
    }

    asio::ASIOSampleRate current = 0.0;
    const bool read = driver_->getSampleRate(&current) == asio::ASE_OK;
    if (!read || static_cast<std::uint32_t>(current + 0.5) != requested) {
        // setSampleRate resets the driver clock; it must run while stopped and
        // before createBuffers, which is exactly where Open calls it.
        const auto set = driver_->setSampleRate(static_cast<asio::ASIOSampleRate>(requested));
        if (set != asio::ASE_OK) {
            NotifyLifecycle(AudioSinkLifecycleEvent::AsioSampleRate, HResultFromAsio(set));
            return HResultFromAsio(set);
        }
        bool applied = false;
        for (int attempt = 0; attempt != 20; ++attempt) {
            current = 0.0;
            if (driver_->getSampleRate(&current) == asio::ASE_OK &&
                static_cast<std::uint32_t>(current + 0.5) == requested) {
                applied = true;
                break;
            }
            Sleep(25);
        }
        if (!applied) {
            Detail(L"sample rate did not settle rate=" + std::to_wstring(requested));
            NotifyLifecycle(AudioSinkLifecycleEvent::AsioSampleRate, E_FAIL);
            return E_FAIL;
        }
    }

    const auto applied = static_cast<std::uint32_t>(current + 0.5);
    appliedSampleRate_.store(applied, std::memory_order_release);
    NotifyLifecycle(AudioSinkLifecycleEvent::AsioSampleRate, S_OK);
    return S_OK;
}

HRESULT AsioExclusiveSink::CreateDriverBuffers() noexcept {
    if (!driver_) return E_UNEXPECTED;

    long minSize = 0;
    long maxSize = 0;
    long preferred = 0;
    long granularity = 0;
    const auto sizeInfo = driver_->getBufferSize(&minSize, &maxSize, &preferred, &granularity);
    if (sizeInfo != asio::ASE_OK) {
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioBufferSize, HResultFromAsio(sizeInfo));
        return HResultFromAsio(sizeInfo);
    }

    const std::uint32_t requested = params_.periodFrames != 0
        ? params_.periodFrames
        : static_cast<std::uint32_t>(preferred > 0 ? preferred : minSize);
    const std::uint32_t chosen = ChooseBufferSize(requested, minSize, maxSize, granularity);
    if (chosen == 0) {
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioBufferSize, E_FAIL);
        return E_FAIL;
    }

    asio::ASIOChannelInfo channelInfo{};
    channelInfo.channel = 0;
    channelInfo.isInput = asio::ASIOFalse;
    const auto info = driver_->getChannelInfo(&channelInfo);
    if (info != asio::ASE_OK) {
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioBuffersCreated, HResultFromAsio(info));
        return HResultFromAsio(info);
    }
    driverSampleType_ = channelInfo.type;
    if (!ResolveSampleMode(EffectiveSourceValidBits(format_))) {
        Detail(L"unsupported sample type=" + std::to_wstring(channelInfo.type) +
               L" sourceBits=" + std::to_wstring(EffectiveSourceValidBits(format_)));
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioBuffersCreated,
                        ammod::audio::kBitPerfectFormatUnavailable);
        return ammod::audio::kBitPerfectFormatUnavailable;
    }

    for (long channel = 0; channel < static_cast<long>(kSupportedChannels); ++channel) {
        bufferInfos_[channel].isInput = asio::ASIOFalse;
        bufferInfos_[channel].channelNum = channel;
        bufferInfos_[channel].buffers[0] = nullptr;
        bufferInfos_[channel].buffers[1] = nullptr;
    }

    callbacks_.bufferSwitch = &AsioExclusiveSink::BufferSwitchThunk;
    callbacks_.sampleRateDidChange = &AsioExclusiveSink::SampleRateChangedThunk;
    callbacks_.asioMessage = &AsioExclusiveSink::AsioMessageThunk;
    callbacks_.bufferSwitchTimeInfo = &AsioExclusiveSink::BufferSwitchTimeInfoThunk;

    const auto created = driver_->createBuffers(bufferInfos_,
                                                static_cast<long>(kSupportedChannels),
                                                static_cast<long>(chosen), &callbacks_);
    if (created != asio::ASE_OK) {
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioBuffersCreated, HResultFromAsio(created));
        return HResultFromAsio(created);
    }

    try {
        interleaved_.assign(static_cast<std::size_t>(chosen) * kSupportedChannels, 0);
    } catch (const std::bad_alloc&) {
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioBuffersCreated, E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }

    bufferFrames_ = chosen;
    Detail(L"buffers created size=" + std::to_wstring(chosen) +
           L" range=" + std::to_wstring(minSize) + L".." + std::to_wstring(maxSize) +
           L" granularity=" + std::to_wstring(granularity) +
           L" sampleType=" + std::to_wstring(driverSampleType_) +
           L" sampleBytes=" + std::to_wstring(sampleBytes_));
    NotifyLifecycle(AudioSinkLifecycleEvent::AsioBuffersCreated, S_OK);
    return S_OK;
}

bool AsioExclusiveSink::ResolveSampleMode(std::uint16_t sourceBits) noexcept {
    // Canonical PCM is MSB-aligned in 32 bits. Every 32-bit aligned ASIO
    // layout takes it verbatim; packed layouts need a byte-level shift.
    switch (driverSampleType_) {
    case asio::ASIOSTInt32LSB:
        sampleMode_ = SampleMode::Int32Aligned;
        sampleBytes_ = 4;
        return true;
    case asio::ASIOSTInt32LSB24:
        if (sourceBits > 24) return false;
        sampleMode_ = SampleMode::Int32Aligned;
        sampleBytes_ = 4;
        return true;
    case asio::ASIOSTInt32LSB20:
        if (sourceBits > 20) return false;
        sampleMode_ = SampleMode::Int32Aligned;
        sampleBytes_ = 4;
        return true;
    case asio::ASIOSTInt32LSB18:
        if (sourceBits > 18) return false;
        sampleMode_ = SampleMode::Int32Aligned;
        sampleBytes_ = 4;
        return true;
    case asio::ASIOSTInt32LSB16:
        if (sourceBits > 16) return false;
        sampleMode_ = SampleMode::Int32Aligned;
        sampleBytes_ = 4;
        return true;
    case asio::ASIOSTInt24LSB:
        if (sourceBits > 24) return false;
        sampleMode_ = SampleMode::Int24Packed;
        sampleBytes_ = 3;
        return true;
    case asio::ASIOSTInt16LSB:
        if (sourceBits > 16) return false;
        sampleMode_ = SampleMode::Int16Packed;
        sampleBytes_ = 2;
        return true;
    case asio::ASIOSTFloat32LSB:
        sampleMode_ = SampleMode::Float32;
        sampleBytes_ = 4;
        return true;
    default:
        return false;
    }
}

HRESULT AsioExclusiveSink::Start(IIntegerPcmSource& source) noexcept {
    NotifyLifecycle(AudioSinkLifecycleEvent::StartEnter, S_OK);
    const auto finish = [this](HRESULT result) noexcept {
        NotifyLifecycle(AudioSinkLifecycleEvent::StartExit, result);
        return result;
    };
    if (state_.load(std::memory_order_acquire) != AudioSinkState::Open ||
        !driver_ || bufferFrames_ == 0) {
        return finish(E_UNEXPECTED);
    }

    source_ = &source;
    waitingForSource_.store(false, std::memory_order_release);
    endOfStreamSubmitted_.store(false, std::memory_order_release);
    endOfStreamComplete_.store(false, std::memory_order_release);
    eosCallbacksRemaining_.store(0, std::memory_order_release);
    resumeBlocked_.store(false, std::memory_order_release);
    g_activeSink.store(this, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    const auto started = driver_->start();
    if (started != asio::ASE_OK) {
        running_.store(false, std::memory_order_release);
        g_activeSink.store(nullptr, std::memory_order_release);
        source_ = nullptr;
        RecordError(HResultFromAsio(started));
        state_.store(AudioSinkState::Faulted, std::memory_order_release);
        return finish(HResultFromAsio(started));
    }

    state_.store(AudioSinkState::Running, std::memory_order_release);
    NotifyLifecycle(AudioSinkLifecycleEvent::AsioStarted, S_OK);
    return finish(S_OK);
}

void AsioExclusiveSink::Stop() noexcept {
    const auto current = state_.load(std::memory_order_acquire);
    if (current != AudioSinkState::Running && !running_.load(std::memory_order_acquire)) return;
    NotifyLifecycle(AudioSinkLifecycleEvent::StopEnter, S_OK);

    HRESULT lifecycleResult = S_OK;
    running_.store(false, std::memory_order_release);
    g_activeSink.store(nullptr, std::memory_order_release);
    source_ = nullptr;
    if (driver_) {
        const auto stopped = driver_->stop();
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioStopped, HResultFromAsio(stopped));
        if (stopped != asio::ASE_OK && stopped != asio::ASE_NotPresent) {
            RecordError(HResultFromAsio(stopped));
            lifecycleResult = HResultFromAsio(stopped);
        }
    }
    waitingForSource_.store(false, std::memory_order_release);
    resumeBlocked_.store(false, std::memory_order_release);
    if (current != AudioSinkState::Faulted) {
        state_.store(AudioSinkState::Open, std::memory_order_release);
    }
    NotifyLifecycle(AudioSinkLifecycleEvent::StopExit, lifecycleResult);
}

void AsioExclusiveSink::Pause() noexcept {
    // ASIO has no pause state: stopping the transport leaves the buffers and
    // the negotiated sample rate intact, so Start() can resume this generation.
    Stop();
}

void AsioExclusiveSink::Close() noexcept {
    if (state_.load(std::memory_order_acquire) == AudioSinkState::Closed &&
        !driver_ && !comInitialized_) {
        return;
    }
    NotifyLifecycle(AudioSinkLifecycleEvent::CloseEnter, S_OK);
    Stop();
    ReleaseDriver();
    std::fill(interleaved_.begin(), interleaved_.end(), 0);
    interleaved_.clear();
    bufferFrames_ = 0;
    appliedSampleRate_.store(0, std::memory_order_release);
    source_ = nullptr;
    endOfStreamComplete_.store(true, std::memory_order_release);
    state_.store(AudioSinkState::Closed, std::memory_order_release);
    NotifyLifecycle(AudioSinkLifecycleEvent::CloseExit,
                    static_cast<HRESULT>(lastError_.load(std::memory_order_acquire)));
}

void AsioExclusiveSink::ReleaseDriver() noexcept {
    if (driver_) {
        const auto disposed = driver_->disposeBuffers();
        NotifyLifecycle(AudioSinkLifecycleEvent::AsioBuffersDisposed,
                        HResultFromAsio(disposed));
        driver_->Release();
        driver_ = nullptr;
    }
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

void AsioExclusiveSink::SetResumeBlocked(bool blocked) noexcept {
    resumeBlocked_.store(blocked, std::memory_order_release);
    if (!blocked && params_.sourceReadyEvent) SetEvent(params_.sourceReadyEvent);
}

void AsioExclusiveSink::BufferSwitchThunk(long bufferIndex,
                                          asio::ASIOBool /*directProcess*/) noexcept {
    if (auto* sink = g_activeSink.load(std::memory_order_acquire)) {
        sink->FillBuffer(bufferIndex);
    }
}

asio::ASIOTime* AsioExclusiveSink::BufferSwitchTimeInfoThunk(
    asio::ASIOTime* /*params*/, long bufferIndex, asio::ASIOBool /*directProcess*/) noexcept {
    if (auto* sink = g_activeSink.load(std::memory_order_acquire)) {
        sink->FillBuffer(bufferIndex);
    }
    return nullptr;
}

void AsioExclusiveSink::SampleRateChangedThunk(asio::ASIOSampleRate rate) noexcept {
    if (auto* sink = g_activeSink.load(std::memory_order_acquire)) {
        sink->appliedSampleRate_.store(static_cast<std::uint32_t>(rate + 0.5),
                                       std::memory_order_release);
    }
}

long AsioExclusiveSink::AsioMessageThunk(long selector, long value, void* /*message*/,
                                         double* /*opt*/) noexcept {
    switch (selector) {
    case asio::kAsioSelectorSupported:
        return (value == asio::kAsioEngineVersion || value == asio::kAsioSupportsTimeInfo ||
                value == asio::kAsioSupportsTimeCode ||
                value == asio::kAsioResetRequest ||
                value == asio::kAsioBufferSizeChange ||
                value == asio::kAsioResyncRequest ||
                value == asio::kAsioSelectorSupported) ? 1L : 0L;
    case asio::kAsioEngineVersion:
        return 2L;
    case asio::kAsioSupportsTimeInfo:
        return 1L;
    case asio::kAsioSupportsTimeCode:
        return 0L;
    case asio::kAsioResetRequest:
    case asio::kAsioBufferSizeChange:
    case asio::kAsioResyncRequest:
        if (auto* sink = g_activeSink.load(std::memory_order_acquire)) {
            Detail(L"driver requested reset/resize selector=" + std::to_wstring(selector));
            sink->RecordError(ammod::audio::kAsioDeviceReset);
        }
        return 1L;
    default:
        return 0L;
    }
}

void AsioExclusiveSink::WriteSilence(long bufferIndex) noexcept {
    const auto index = static_cast<std::size_t>(bufferIndex & 1);
    const auto bytes = static_cast<std::size_t>(bufferFrames_) * sampleBytes_;
    if (bytes == 0) return;
    for (std::uint32_t channel = 0; channel < kSupportedChannels; ++channel) {
        if (void* destination = bufferInfos_[channel].buffers[index]) {
            std::memset(destination, 0, bytes);
        }
    }
}

void AsioExclusiveSink::FillBuffer(long bufferIndex) noexcept {
    // The driver created this thread, not us. Apply the optional local tuning
    // once, before any sample fill; later callbacks only touch driver buffers.
    if (!threadTuningApplied_.load(std::memory_order_acquire)) {
        threadTuningApplied_.store(true, std::memory_order_release);
        LocalPlaybackTuningRuntime::Instance().ApplyToCurrentThread();
    }
    // Once the producer is sealed, let the driver play out both halves of its
    // double buffer before reporting the generation as drained.
    if (endOfStreamSubmitted_.load(std::memory_order_acquire)) {
        auto remaining = eosCallbacksRemaining_.load(std::memory_order_acquire);
        if (remaining > 0) {
            if (eosCallbacksRemaining_.compare_exchange_strong(
                    remaining, remaining - 1, std::memory_order_acq_rel) &&
                remaining == 1) {
                endOfStreamComplete_.store(true, std::memory_order_release);
            }
            WriteSilence(bufferIndex);
            return;
        }
    }

    if (!running_.load(std::memory_order_acquire) ||
        resumeBlocked_.load(std::memory_order_acquire) || !source_) {
        WriteSilence(bufferIndex);
        return;
    }

    IIntegerPcmSource* source = source_;
    HRESULT prepared = source->Prepare(bufferFrames_);
    if (prepared == kAudioSourceWouldBlock) {
        waitingForSource_.store(true, std::memory_order_release);
        sourceWaits_.fetch_add(1, std::memory_order_relaxed);
        WriteSilence(bufferIndex);
        return;
    }
    if (FAILED(prepared)) {
        getBufferFailures_.fetch_add(1, std::memory_order_relaxed);
        RecordError(prepared);
        WriteSilence(bufferIndex);
        return;
    }
    if (prepared == S_FALSE) {
        waitingForSource_.store(false, std::memory_order_release);
        endOfStreamSubmitted_.store(true, std::memory_order_release);
        eosCallbacksRemaining_.store(
            submittedFrames_.load(std::memory_order_acquire) == 0 ? 1u : 2u,
            std::memory_order_release);
        if (submittedFrames_.load(std::memory_order_acquire) == 0) {
            endOfStreamComplete_.store(true, std::memory_order_release);
        }
        WriteSilence(bufferIndex);
        return;
    }
    waitingForSource_.store(false, std::memory_order_release);

    std::uint32_t written = 0;
    bool endOfStream = false;
    const HRESULT hr = source->Fill(interleaved_.data(), bufferFrames_, written, endOfStream);
    if (hr == kAudioSourceWouldBlock) {
        waitingForSource_.store(true, std::memory_order_release);
        sourceWaits_.fetch_add(1, std::memory_order_relaxed);
        WriteSilence(bufferIndex);
        return;
    }
    if (FAILED(hr) || written > bufferFrames_) {
        getBufferFailures_.fetch_add(1, std::memory_order_relaxed);
        underruns_.fetch_add(1, std::memory_order_relaxed);
        RecordError(FAILED(hr) ? hr : E_FAIL);
        WriteSilence(bufferIndex);
        return;
    }

    if (written < bufferFrames_) {
        std::memset(interleaved_.data() +
                        static_cast<std::size_t>(written) * kSupportedChannels,
                    0,
                    static_cast<std::size_t>(bufferFrames_ - written) *
                        kSupportedChannels * sizeof(std::int32_t));
        controlledSilenceFrames_.fetch_add(bufferFrames_ - written, std::memory_order_relaxed);
    }

    const auto index = static_cast<std::size_t>(bufferIndex & 1);
    for (std::uint32_t channel = 0; channel < kSupportedChannels; ++channel) {
        void* destination = bufferInfos_[channel].buffers[index];
        if (!destination) continue;
        switch (sampleMode_) {
        case SampleMode::Int32Aligned: {
            auto* output = static_cast<std::int32_t*>(destination);
            for (std::uint32_t frame = 0; frame < bufferFrames_; ++frame) {
                output[frame] = interleaved_[static_cast<std::size_t>(frame) *
                                                 kSupportedChannels + channel];
            }
            break;
        }
        case SampleMode::Int24Packed: {
            auto* output = static_cast<std::uint8_t*>(destination);
            for (std::uint32_t frame = 0; frame < bufferFrames_; ++frame) {
                const auto value = static_cast<std::uint32_t>(
                    interleaved_[static_cast<std::size_t>(frame) * kSupportedChannels +
                                 channel]) >> 8u;
                output[static_cast<std::size_t>(frame) * 3u] = static_cast<std::uint8_t>(value);
                output[static_cast<std::size_t>(frame) * 3u + 1u] =
                    static_cast<std::uint8_t>(value >> 8u);
                output[static_cast<std::size_t>(frame) * 3u + 2u] =
                    static_cast<std::uint8_t>(value >> 16u);
            }
            break;
        }
        case SampleMode::Int16Packed: {
            auto* output = static_cast<std::uint8_t*>(destination);
            for (std::uint32_t frame = 0; frame < bufferFrames_; ++frame) {
                const auto value = static_cast<std::uint32_t>(
                    interleaved_[static_cast<std::size_t>(frame) * kSupportedChannels +
                                 channel]) >> 16u;
                output[static_cast<std::size_t>(frame) * 2u] = static_cast<std::uint8_t>(value);
                output[static_cast<std::size_t>(frame) * 2u + 1u] =
                    static_cast<std::uint8_t>(value >> 8u);
            }
            break;
        }
        case SampleMode::Float32: {
            auto* output = static_cast<float*>(destination);
            for (std::uint32_t frame = 0; frame < bufferFrames_; ++frame) {
                const auto val = interleaved_[static_cast<std::size_t>(frame) *
                                              kSupportedChannels + channel];
                output[frame] = static_cast<float>(val) / 2147483648.0f;
            }
            break;
        }
        }
    }

    submittedFrames_.fetch_add(bufferFrames_, std::memory_order_relaxed);
    submittedBuffers_.fetch_add(1, std::memory_order_relaxed);
    if (endOfStream) {
        endOfStreamSubmitted_.store(true, std::memory_order_release);
        eosCallbacksRemaining_.store(2u, std::memory_order_release);
    }
}

void AsioExclusiveSink::RecordError(HRESULT error) noexcept {
    if (FAILED(error)) lastError_.store(error, std::memory_order_release);
}

void AsioExclusiveSink::NotifyLifecycle(AudioSinkLifecycleEvent event,
                                        HRESULT result) noexcept {
    if (params_.onLifecycle) {
        params_.onLifecycle(params_.lifecycleContext, event, result);
    }
}

AudioSinkStats AsioExclusiveSink::Stats() const noexcept {
    AudioSinkStats stats{};
    stats.kind = AudioSinkKind::Asio;
    stats.state = state_.load(std::memory_order_acquire);
    stats.submittedFrames = submittedFrames_.load(std::memory_order_acquire);
    stats.submittedBuffers = submittedBuffers_.load(std::memory_order_acquire);
    stats.controlledSilenceFrames = controlledSilenceFrames_.load(std::memory_order_acquire);
    stats.sourceWaits = sourceWaits_.load(std::memory_order_acquire);
    stats.underruns = underruns_.load(std::memory_order_acquire);
    stats.getBufferFailures = getBufferFailures_.load(std::memory_order_acquire);
    stats.releaseBufferFailures = releaseBufferFailures_.load(std::memory_order_acquire);
    stats.lastError = static_cast<HRESULT>(lastError_.load(std::memory_order_acquire));
    return stats;
}

} // namespace ammod::audio_v2
