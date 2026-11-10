#pragma once

#include "AsioDriverApi.h"
#include "AudioSink.h"
#include "IntegerPcmSource.h"
#include "PcmTypes.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace ammod::audio_v2 {

// Native ASIO output. The driver owns the clock and calls bufferSwitch on its
// own real-time thread; this sink only fills the driver-provided double buffer.
// Sample-rate changes are a full Close/Open cycle, which is exactly the
// coordinator's generation boundary when a new Apple converter appears.
//
// Ported from yeluo53/AMExclusiveASIO / magua521/AM-ASIO-MAD (MIT), adapted to
// this fork's IAudioSink contract. The driver is selected by CLSID at runtime
// instead of being compiled into the binary.
class AsioExclusiveSink final : public IAudioSink {
public:
    AsioExclusiveSink() noexcept = default;
    ~AsioExclusiveSink() override;

    AsioExclusiveSink(const AsioExclusiveSink&) = delete;
    AsioExclusiveSink& operator=(const AsioExclusiveSink&) = delete;

    AudioSinkKind Kind() const noexcept override { return AudioSinkKind::Asio; }
    HRESULT Open(const AudioSinkOpenParams& params) noexcept override;
    HRESULT Start(IIntegerPcmSource& source) noexcept override;
    void Pause() noexcept override;
    void Stop() noexcept override;
    void Close() noexcept override;

    AudioSinkState State() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AudioSinkStats Stats() const noexcept override;
    const PcmFormat& Format() const noexcept override { return format_; }
    std::uint32_t BufferFrames() const noexcept override { return bufferFrames_; }

    bool WaitingForSource() const noexcept override {
        return waitingForSource_.load(std::memory_order_acquire);
    }
    bool EndOfStreamSubmitted() const noexcept override {
        return endOfStreamSubmitted_.load(std::memory_order_acquire);
    }
    bool EndOfStreamComplete() const noexcept override {
        return endOfStreamComplete_.load(std::memory_order_acquire);
    }
    void SetResumeBlocked(bool blocked) noexcept override;

    std::uint32_t AppliedSampleRate() const noexcept {
        return appliedSampleRate_.load(std::memory_order_acquire);
    }
    long DriverSampleType() const noexcept { return driverSampleType_; }

private:
    enum class SampleMode : std::uint8_t {
        Int32Aligned,
        Int24Packed,
        Int16Packed,
        Float32,
    };

    // ASIO callbacks are plain function pointers with no user context, so the
    // thunks resolve the single live sink through a file-scope pointer.
    static void BufferSwitchThunk(long bufferIndex, asio::ASIOBool directProcess) noexcept;
    static void SampleRateChangedThunk(asio::ASIOSampleRate rate) noexcept;
    static long AsioMessageThunk(long selector, long value, void* message, double* opt) noexcept;
    static asio::ASIOTime* BufferSwitchTimeInfoThunk(asio::ASIOTime* params,
                                                     long bufferIndex,
                                                     asio::ASIOBool directProcess) noexcept;

    // Real-time path. Must not allocate, lock, block or log.
    void FillBuffer(long bufferIndex) noexcept;
    void WriteSilence(long bufferIndex) noexcept;

    HRESULT LoadDriver() noexcept;
    HRESULT ApplySampleRate() noexcept;
    HRESULT CreateDriverBuffers() noexcept;
    bool ResolveSampleMode(std::uint16_t sourceBits) noexcept;
    void NotifyLifecycle(AudioSinkLifecycleEvent event, HRESULT result) noexcept;
    void RecordError(HRESULT error) noexcept;
    void ReleaseDriver() noexcept;

    AudioSinkOpenParams params_{};
    PcmFormat format_{};
    asio::IASIO* driver_{};
    asio::ASIOCallbacks callbacks_{};
    asio::ASIOBufferInfo bufferInfos_[kSupportedChannels]{};
    std::vector<std::int32_t> interleaved_;
    SampleMode sampleMode_{SampleMode::Int32Aligned};
    std::uint32_t sampleBytes_{4};
    std::uint32_t bufferFrames_{};
    long driverSampleType_{asio::ASIOSTInt32LSB};
    std::atomic<std::uint32_t> appliedSampleRate_{};
    bool comInitialized_{};

    IIntegerPcmSource* source_{};
    std::atomic<bool> running_{};
    std::atomic<bool> resumeBlocked_{};
    std::atomic<bool> waitingForSource_{};
    std::atomic<bool> endOfStreamSubmitted_{};
    std::atomic<bool> endOfStreamComplete_{};
    // The driver owns the bufferSwitch thread; apply local tuning exactly once
    // per opened sink so the real-time callback stays allocation/lock free.
    std::atomic<bool> threadTuningApplied_{};
    std::atomic<std::uint32_t> eosCallbacksRemaining_{};

    std::atomic<AudioSinkState> state_{AudioSinkState::Closed};
    std::atomic<std::uint64_t> submittedFrames_{};
    std::atomic<std::uint64_t> submittedBuffers_{};
    std::atomic<std::uint64_t> controlledSilenceFrames_{};
    std::atomic<std::uint64_t> sourceWaits_{};
    std::atomic<std::uint64_t> underruns_{};
    std::atomic<std::uint64_t> getBufferFailures_{};
    std::atomic<std::uint64_t> releaseBufferFailures_{};
    std::atomic<LONG> lastError_{S_OK};
};

} // namespace ammod::audio_v2
