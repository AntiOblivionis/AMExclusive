#include "ResampledPcmSource.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <numeric>

namespace ammod::audio_v2 {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kKaiserBeta = 10.8;
constexpr std::uint32_t kMaximumPhases = 2048;
constexpr std::size_t kMaximumCoefficients = 4u * 1024u * 1024u;
constexpr std::uint32_t kMaximumOutputFrames = 1024u * 1024u;

double BesselI0(double value) noexcept {
    const double squared = value * value * 0.25;
    double sum = 1.0;
    double term = 1.0;
    for (unsigned index = 1; index < 64; ++index) {
        term *= squared / (static_cast<double>(index) * index);
        sum += term;
        if (term < sum * 1.0e-16) break;
    }
    return sum;
}

} // namespace

HRESULT ResampledPcmSource::Configure(QueuedPcmSource& source,
                                     const PcmFormat& input,
                                     const PcmFormat& output,
                                     std::uint32_t maxOutputFrames) noexcept {
    Reset();
    if (!IsValidFormat(input) || !IsValidFormat(output) ||
        output.sampleRate >= input.sampleRate ||
        EffectiveSourceValidBits(output) != EffectiveSourceValidBits(input) ||
        maxOutputFrames == 0 || maxOutputFrames > kMaximumOutputFrames) {
        return E_INVALIDARG;
    }

    const double ratio = static_cast<double>(output.sampleRate) / input.sampleRate;
    // The transition band is 90%-100% of the destination Nyquist frequency.
    // A 160/ratio tap Kaiser window provides roughly 100 dB rejection by the
    // destination Nyquist. Expanding the support at lower rates maintains the
    // same frequency response for integer and fractional conversion ratios.
    radius_ = static_cast<std::uint32_t>(std::ceil(80.0 / ratio));
    taps_ = radius_ * 2u + 1u;
    const auto exactPhases = output.sampleRate / std::gcd(input.sampleRate, output.sampleRate);
    const auto memoryLimitedPhases = static_cast<std::uint32_t>(
        kMaximumCoefficients / taps_ - 1u);
    phases_ = std::min({exactPhases, kMaximumPhases, memoryLimitedPhases});
    if (phases_ == 0) return E_INVALIDARG;

    ringCapacityFrames_ = static_cast<std::size_t>(taps_) + kMaxBlockFrames + 2u;
    try {
        coefficients_.resize(static_cast<std::size_t>(phases_ + 1u) * taps_);
        // A mirrored ring makes every FIR window contiguous. It avoids an
        // integer remainder per tap/channel on the real-time render thread.
        ring_.resize(ringCapacityFrames_ * kSupportedChannels * 2u);
        staged_.resize(static_cast<std::size_t>(maxOutputFrames) * kSupportedChannels);
    } catch (...) {
        Reset();
        return E_OUTOFMEMORY;
    }

    const double cutoff = 0.475 * ratio;
    const double windowScale = 1.0 / BesselI0(kKaiserBeta);
    for (std::uint32_t phase = 0; phase <= phases_; ++phase) {
        auto* coefficients = coefficients_.data() + static_cast<std::size_t>(phase) * taps_;
        const double fraction = static_cast<double>(phase) / phases_;
        double sum = 0.0;
        for (std::uint32_t tap = 0; tap < taps_; ++tap) {
            const double distance = static_cast<double>(tap) - radius_ - fraction;
            const double normalized = distance / radius_;
            double coefficient = 0.0;
            if (std::abs(normalized) <= 1.0) {
                const double argument = 2.0 * kPi * cutoff * distance;
                const double sinc = std::abs(argument) < 1.0e-12
                    ? 1.0 : std::sin(argument) / argument;
                const double window = BesselI0(kKaiserBeta *
                    std::sqrt(std::max(0.0, 1.0 - normalized * normalized))) * windowScale;
                coefficient = 2.0 * cutoff * sinc * window;
            }
            coefficients[tap] = coefficient;
            sum += coefficient;
        }
        // Preserve DC independently of phase; interpolation also retains this
        // normalization when unusually coprime rates need a bounded phase bank.
        for (std::uint32_t tap = 0; tap < taps_; ++tap) coefficients[tap] /= sum;
    }

    source_ = &source;
    input_ = input;
    output_ = output;
    maxOutputFrames_ = maxOutputFrames;
    return S_OK;
}

void ResampledPcmSource::Reset() noexcept {
    source_ = nullptr;
    input_ = {};
    output_ = {};
    maxOutputFrames_ = 0;
    radius_ = 0;
    taps_ = 0;
    phases_ = 0;
    ringCapacityFrames_ = 0;
    coefficients_.clear();
    ring_.clear();
    staged_.clear();
    stagedFrames_ = 0;
    streamEnded_ = false;
    outputEnded_ = false;
    ResetSegment();
    pendingSourceFrames_.store(0, std::memory_order_release);
}

void ResampledPcmSource::ResetSegment() noexcept {
    inputFrames_ = 0;
    positionFrame_ = 0;
    positionRemainder_ = 0;
    ringFirstFrame_ = 0;
    ringHead_ = 0;
    ringFrames_ = 0;
    segmentEnded_ = false;
}

bool ResampledPcmSource::CanProvide(std::uint32_t capacityFrames) const noexcept {
    return source_ && capacityFrames <= maxOutputFrames_ &&
        (stagedFrames_ >= capacityFrames || outputEnded_);
}

std::size_t ResampledPcmSource::AvailableFrames() const noexcept {
    if (!source_) return 0;
    const auto pending = PendingSourceFrames();
    const auto queued = source_->AvailableFrames();
    const auto total = queued > SIZE_MAX - pending ? SIZE_MAX : queued + pending;
    // Divide first to make the diagnostic safe even for very long streams.
    return (total / input_.sampleRate) * output_.sampleRate +
        (total % input_.sampleRate) * output_.sampleRate / input_.sampleRate;
}

HRESULT ResampledPcmSource::EnsureLookahead() noexcept {
    while (!segmentEnded_ && inputFrames_ <= positionFrame_ + radius_) {
        const auto freeFrames = ringCapacityFrames_ - ringFrames_;
        const auto request = static_cast<std::uint32_t>(
            std::min<std::size_t>(freeFrames, kMaxBlockFrames));
        if (request == 0) return E_UNEXPECTED;
        std::uint32_t written = 0;
        bool ended = false;
        const auto result = source_->ReadForResampling(readBuffer_.data(), request,
                                                       written, ended, inputFrames_ != 0);
        if (result == S_FALSE) {
            segmentEnded_ = true;
            break;
        }
        if (FAILED(result) && result != kAudioSourceWouldBlock) return result;
        if (written > request || inputFrames_ >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                written - radius_ - 1u) return E_UNEXPECTED;

        for (std::uint32_t frame = 0; frame < written; ++frame) {
            const auto slot = (ringHead_ + ringFrames_ + frame) % ringCapacityFrames_;
            for (std::uint32_t channel = 0; channel < kSupportedChannels; ++channel) {
                const auto sample = readBuffer_[static_cast<std::size_t>(frame) * kSupportedChannels + channel];
                ring_[slot * kSupportedChannels + channel] = sample;
                ring_[(slot + ringCapacityFrames_) * kSupportedChannels + channel] = sample;
            }
        }
        ringFrames_ += written;
        inputFrames_ += written;
        if (ended) {
            segmentEnded_ = true;
            streamEnded_ = true;
        }
        if (written == 0 && !ended) return kAudioSourceWouldBlock;
    }
    return S_OK;
}

void ResampledPcmSource::GenerateFrame(std::int32_t* destination) noexcept {
    const auto scaledPhase = static_cast<std::uint64_t>(positionRemainder_) * phases_;
    const auto phase = static_cast<std::uint32_t>(scaledPhase / output_.sampleRate);
    const double fraction = static_cast<double>(scaledPhase % output_.sampleRate) / output_.sampleRate;
    const auto* lower = coefficients_.data() + static_cast<std::size_t>(phase) * taps_;
    const auto* upper = lower + taps_;
    double accumulated[kSupportedChannels]{};
    const auto first = static_cast<std::int64_t>(positionFrame_) - radius_;
    // Zero extension compensates the FIR latency without appending fabricated
    // media duration: positions outside the input are omitted from the sum.
    const auto begin = std::max(first, static_cast<std::int64_t>(ringFirstFrame_));
    const auto end = std::min(first + taps_, static_cast<std::int64_t>(inputFrames_));
    if (begin < end) {
        const auto offset = static_cast<std::size_t>(begin - static_cast<std::int64_t>(ringFirstFrame_));
        const auto slot = (ringHead_ + offset) % ringCapacityFrames_;
        const auto* samples = ring_.data() + slot * kSupportedChannels;
        const auto count = static_cast<std::uint32_t>(end - begin);
        const auto coefficientOffset = static_cast<std::size_t>(begin - first);
        lower += coefficientOffset;
        upper += coefficientOffset;
        if (fraction == 0.0) {
            for (std::uint32_t tap = 0; tap < count; ++tap) {
                accumulated[0] += lower[tap] * samples[static_cast<std::size_t>(tap) * 2u];
                accumulated[1] += lower[tap] * samples[static_cast<std::size_t>(tap) * 2u + 1u];
            }
        } else {
            for (std::uint32_t tap = 0; tap < count; ++tap) {
                const double coefficient = lower[tap] + fraction * (upper[tap] - lower[tap]);
                accumulated[0] += coefficient * samples[static_cast<std::size_t>(tap) * 2u];
                accumulated[1] += coefficient * samples[static_cast<std::size_t>(tap) * 2u + 1u];
            }
        }
    }
    const auto bits = EffectiveSourceValidBits(output_);
    const auto quantum = std::int64_t{1} << (32u - bits);
    const double minimum = -std::ldexp(1.0, bits - 1u);
    const double maximum = std::ldexp(1.0, bits - 1u) - 1.0;
    for (std::uint32_t channel = 0; channel < kSupportedChannels; ++channel) {
        const double rounded = std::round(accumulated[channel] / static_cast<double>(quantum));
        const auto code = static_cast<std::int64_t>(std::clamp(rounded, minimum, maximum));
        destination[channel] = static_cast<std::int32_t>(code * quantum);
    }

    const auto nextRemainder = static_cast<std::uint64_t>(positionRemainder_) + input_.sampleRate;
    positionFrame_ += nextRemainder / output_.sampleRate;
    positionRemainder_ = static_cast<std::uint32_t>(nextRemainder % output_.sampleRate);
    DiscardHistory();
}

void ResampledPcmSource::DiscardHistory() noexcept {
    const auto earliestNeeded = positionFrame_ > radius_ ? positionFrame_ - radius_ : 0;
    if (earliestNeeded <= ringFirstFrame_) return;
    const auto discard = static_cast<std::size_t>(
        std::min<std::uint64_t>(earliestNeeded - ringFirstFrame_, ringFrames_));
    ringHead_ = (ringHead_ + discard) % ringCapacityFrames_;
    ringFirstFrame_ += discard;
    ringFrames_ -= discard;
}

void ResampledPcmSource::UpdatePending() noexcept {
    const auto forward = inputFrames_ > positionFrame_ ? inputFrames_ - positionFrame_ : 0u;
    const auto stagedInput = (static_cast<std::uint64_t>(stagedFrames_) * input_.sampleRate +
        output_.sampleRate - 1u) / output_.sampleRate;
    pendingSourceFrames_.store(static_cast<std::size_t>(forward + stagedInput),
                              std::memory_order_release);
}

HRESULT ResampledPcmSource::Prepare(std::uint32_t capacityFrames) noexcept {
    if (!source_ || capacityFrames == 0 || capacityFrames > maxOutputFrames_) return E_INVALIDARG;
    while (stagedFrames_ < capacityFrames && !outputEnded_) {
        const auto result = EnsureLookahead();
        if (FAILED(result)) {
            UpdatePending();
            return result;
        }
        if (segmentEnded_ && positionFrame_ >= inputFrames_) {
            if (streamEnded_) {
                outputEnded_ = true;
                break;
            }
            ResetSegment();
            continue;
        }
        GenerateFrame(staged_.data() + static_cast<std::size_t>(stagedFrames_) * kSupportedChannels);
        ++stagedFrames_;
        // Avoid requiring an extra empty endpoint submission when the final
        // sample coincides exactly with the requested period boundary.
        if (streamEnded_ && positionFrame_ >= inputFrames_) outputEnded_ = true;
    }
    UpdatePending();
    return S_OK;
}

HRESULT ResampledPcmSource::Fill(std::int32_t* destination, std::uint32_t capacityFrames,
                                std::uint32_t& writtenFrames, bool& endOfStream) noexcept {
    writtenFrames = 0;
    endOfStream = false;
    if (!destination || capacityFrames == 0 || !source_ ||
        capacityFrames > maxOutputFrames_) return E_INVALIDARG;
    if (!CanProvide(capacityFrames)) return kAudioSourceWouldBlock;
    writtenFrames = std::min(capacityFrames, stagedFrames_);
    std::memcpy(destination, staged_.data(),
                static_cast<std::size_t>(writtenFrames) * kSupportedChannels * sizeof(*destination));
    stagedFrames_ -= writtenFrames;
    if (stagedFrames_ != 0) {
        std::memmove(staged_.data(),
                     staged_.data() + static_cast<std::size_t>(writtenFrames) * kSupportedChannels,
                     static_cast<std::size_t>(stagedFrames_) * kSupportedChannels * sizeof(*destination));
    }
    endOfStream = outputEnded_ && stagedFrames_ == 0;
    UpdatePending();
    return S_OK;
}

} // namespace ammod::audio_v2
