#pragma once

#include "PcmTypes.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ammod::audio_v2 {

// P0 is Apple's normalized float32 representation. For a source integer depth,
// the exact container code is the signed code left-aligned in a 32-bit PCM word.
// No gain, dither, clipping or sample-rate conversion is allowed here.
inline bool MapP0Sample(float sample, std::uint16_t validBits,
                        std::int32_t& output) noexcept {
    if (!IsSupportedSourceDepth(validBits) || !std::isfinite(sample) ||
        sample < -1.0f || sample >= 1.0f) {
        return false;
    }

    // Integer-source P0 must already lie exactly on the original PCM lattice.
    // Scaling an IEEE float by 2^31 is exact for 16/24-bit source codes; do not
    // run a general-purpose rounding algorithm on the real-time sink thread.
    // Any fractional result means the sample has been altered and must fail
    // closed rather than being rounded into a plausible PCM code.
    const double scaled = static_cast<double>(sample) * 2147483648.0;
    if (!std::isfinite(scaled) ||
        scaled < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        scaled > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        return false;
    }

    const auto container = static_cast<std::int64_t>(scaled);
    if (static_cast<double>(container) != scaled) return false;
    const auto paddingBits = static_cast<std::uint16_t>(32u - validBits);
    const auto paddingMask = paddingBits == 32
        ? std::numeric_limits<std::uint32_t>::max()
        : ((std::uint32_t{1} << paddingBits) - 1u);
    if ((static_cast<std::uint32_t>(container) & paddingMask) != 0u) return false;

    output = static_cast<std::int32_t>(container);
    const float roundTrip = static_cast<float>(
        static_cast<double>(output) / 2147483648.0);
    // A decoder is allowed to spell numeric zero as either +0.0f or -0.0f;
    // the integer PCM code is identical. Numeric equality keeps that harmless
    // sign bit from rejecting an otherwise exact source-lattice sample.
    if (roundTrip != sample) {
        return false;
    }
    return true;
}

inline bool MapP0Interleaved(const float* input, std::int32_t* output,
                             std::size_t sampleCount,
                             std::uint16_t validBits) noexcept {
    if (!input || !output || !IsSupportedSourceDepth(validBits)) return false;
    for (std::size_t index = 0; index < sampleCount; ++index) {
        if (!MapP0Sample(input[index], validBits, output[index])) return false;
    }
    return true;
}

} // namespace ammod::audio_v2
