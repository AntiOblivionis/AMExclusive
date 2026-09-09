#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ammod::audio_v2 {

struct LocalFloatIntegerizerStats final {
    std::uint64_t samples{};
    std::uint64_t clippedSamples{};
    std::uint64_t nonFiniteSamples{};
};

// Native float32 local files are not an integer-source bit-perfect case.  When
// the endpoint cannot accept float32 Exclusive directly, this compatibility
// layer deterministically quantizes only that local-source class to signed
// PCM32.  It is intentionally separate from ExactSampleMapper so the proven
// ALAC/integer lattice path cannot silently relax its fail-closed rules.
inline bool IntegerizeLocalFloatSample(float sample, std::int32_t& output,
                                       LocalFloatIntegerizerStats& stats) noexcept {
    ++stats.samples;
    if (!std::isfinite(sample)) {
        ++stats.nonFiniteSamples;
        return false;
    }

    if (sample >= 1.0f) {
        if (sample > 1.0f || sample == 1.0f) ++stats.clippedSamples;
        output = std::numeric_limits<std::int32_t>::max();
        return true;
    }
    if (sample <= -1.0f) {
        if (sample < -1.0f) ++stats.clippedSamples;
        output = std::numeric_limits<std::int32_t>::min();
        return true;
    }

    // float -> double is exact, and multiplying by 2^31 is exact.  Implement
    // round-to-nearest, ties-to-even explicitly so the result does not depend
    // on the host thread's floating-point rounding mode.
    const double scaled = static_cast<double>(sample) * 2147483648.0;
    const double lower = std::floor(scaled);
    const double fraction = scaled - lower;
    const double rounded = fraction < 0.5
        ? lower
        : (fraction > 0.5
               ? lower + 1.0
               : (std::fmod(lower, 2.0) == 0.0 ? lower : lower + 1.0));
    if (!std::isfinite(rounded) ||
        rounded < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        rounded > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        return false;
    }
    output = static_cast<std::int32_t>(rounded);
    return true;
}

inline bool IntegerizeLocalFloatInterleaved(const float* input,
                                             std::int32_t* output,
                                             std::size_t sampleCount,
                                             LocalFloatIntegerizerStats& stats) noexcept {
    if (!input || !output) return false;
    for (std::size_t index = 0; index < sampleCount; ++index) {
        if (!IntegerizeLocalFloatSample(input[index], output[index], stats)) return false;
    }
    return true;
}

} // namespace ammod::audio_v2
