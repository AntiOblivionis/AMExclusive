#pragma once

#include "IntegerPcmSource.h"
#include "QueuedPcmSource.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ammod::audio_v2 {

// Streaming, band-limited downsampling for an endpoint which rejects the
// original sample rate. Configure owns every allocation; Prepare stages a
// complete device period before WASAPI GetBuffer, and Fill only copies it.
// Stop/start and producer starvation preserve the filter and rational clock.
// A new Configure (generation change) or an explicit queue discontinuity resets
// that history. Transformed samples must never be advertised as bit-perfect.
class ResampledPcmSource final : public IIntegerPcmSource {
public:
    HRESULT Configure(QueuedPcmSource& source,
                      const PcmFormat& input,
                      const PcmFormat& output,
                      std::uint32_t maxOutputFrames) noexcept;
    void Reset() noexcept;

    HRESULT Prepare(std::uint32_t capacityFrames) noexcept override;
    bool CanProvide(std::uint32_t capacityFrames) const noexcept override;
    std::size_t AvailableFrames() const noexcept override;
    HRESULT Fill(std::int32_t* destination, std::uint32_t capacityFrames,
                 std::uint32_t& writtenFrames, bool& endOfStream) noexcept override;

    // Excludes the wrapped queue and obsolete filter history. The coordinator
    // uses this atomic snapshot to avoid retiring delayed/staged media early.
    // A short, unsealed source must still be sealed by the producer before its
    // final lookahead can drain.
    std::size_t PendingSourceFrames() const noexcept {
        return pendingSourceFrames_.load(std::memory_order_acquire);
    }

private:
    HRESULT EnsureLookahead() noexcept;
    void ResetSegment() noexcept;
    void UpdatePending() noexcept;
    void GenerateFrame(std::int32_t* destination) noexcept;
    void DiscardHistory() noexcept;

    QueuedPcmSource* source_{};
    PcmFormat input_{};
    PcmFormat output_{};
    std::uint32_t maxOutputFrames_{};
    std::uint32_t radius_{};
    std::uint32_t taps_{};
    std::uint32_t phases_{};
    std::size_t ringCapacityFrames_{};
    std::vector<double> coefficients_;
    std::vector<std::int32_t> ring_;
    std::vector<std::int32_t> staged_;
    std::array<std::int32_t, kMaxBlockSamples> readBuffer_{};

    std::uint64_t inputFrames_{};
    std::uint64_t positionFrame_{};
    std::uint32_t positionRemainder_{};
    std::uint64_t ringFirstFrame_{};
    std::size_t ringHead_{};
    std::size_t ringFrames_{};
    std::uint32_t stagedFrames_{};
    bool segmentEnded_{};
    bool streamEnded_{};
    bool outputEnded_{};
    std::atomic<std::size_t> pendingSourceFrames_{};
};

} // namespace ammod::audio_v2
