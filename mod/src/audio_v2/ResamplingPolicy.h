#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ammod::audio_v2 {

// Probe in descending order within each preference group. The native rate is
// always first, including rates not present in this standard-rate list.
inline constexpr std::array<std::uint32_t, 18> kResamplingSampleRates{
    768000, 705600, 384000, 352800, 192000, 176400, 96000, 88200, 64000,
    48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000,
};

inline constexpr std::size_t kMaxRateCandidates = kResamplingSampleRates.size() + 1;

struct SampleRateCandidates final {
    std::array<std::uint32_t, kMaxRateCandidates> values{};
    std::size_t count{};
};

// This policy changes only the sample rate. The caller must retain the source
// channel count and precision when probing endpoint format/container tuples.
constexpr SampleRateCandidates SelectOutputSampleRates(
    std::uint32_t sourceRate, bool allowResampling) noexcept {
    if (sourceRate < 8000 || sourceRate > 768000) return {};

    SampleRateCandidates result{};
    result.values[result.count++] = sourceRate;
    if (!allowResampling) return result;

    // Prefer integer downsampling factors (192 -> 96 -> 64 -> 48 kHz), then
    // noninteger downsampling if no integer-rate format is supported. Never
    // upsample as a compatibility fallback.
    for (const auto rate : kResamplingSampleRates) {
        if (rate < sourceRate && sourceRate % rate == 0) {
            result.values[result.count++] = rate;
        }
    }
    for (const auto rate : kResamplingSampleRates) {
        if (rate < sourceRate && sourceRate % rate != 0) {
            result.values[result.count++] = rate;
        }
    }
    return result;
}

} // namespace ammod::audio_v2
