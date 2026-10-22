#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ammod::audio_v2 {

// Canonical left-aligned signed PCM. Use nonsubtractive TPDF dither only when
// discarding source precision; widening/native paths never pass through here.
// State belongs to one conversion segment, independent of chunk size/pauses.
class PcmQuantizer final {
public:
    void Reset() noexcept {
        state_ = {0x853c49e6748fea9bull, 0xda3e39cb94b95bdbull};
    }

    std::int32_t Convert(double sample, std::uint16_t bits,
                         std::uint32_t channel, bool dither) noexcept {
        const auto quantum = std::int64_t{1} << (32u - bits);
        double code = sample / static_cast<double>(quantum);
        if (dither) {
            // Two independent uniforms produce a triangular distribution of
            // two LSB peak-to-peak, before rounding once to the output lattice.
            const double first = Uniform(channel);
            const double second = Uniform(channel);
            code += first - second;
        }
        const double minimum = -std::ldexp(1.0, bits - 1u);
        const double maximum = std::ldexp(1.0, bits - 1u) - 1.0;
        const auto rounded = static_cast<std::int64_t>(
            std::clamp(std::round(code), minimum, maximum));
        return static_cast<std::int32_t>(rounded * quantum);
    }

private:
    double Uniform(std::uint32_t channel) noexcept {
        // SplitMix64: independent channel streams, no allocation or shared RNG.
        auto value = (state_[channel] += 0x9e3779b97f4a7c15ull);
        value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
        value ^= value >> 31u;
        return static_cast<double>(value >> 11u) * 0x1.0p-53;
    }
    std::array<std::uint64_t, 2> state_{0x853c49e6748fea9bull, 0xda3e39cb94b95bdbull};
};

} // namespace ammod::audio_v2
