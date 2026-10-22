#pragma once

#include "PcmTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ammod::audio_v2 {

// Probe in descending order. The native rate is
// always first, including rates not present in this standard-rate list.
inline constexpr std::array<std::uint32_t, 18> kResamplingSampleRates{
    768000, 705600, 384000, 352800, 192000, 176400, 96000, 88200, 64000,
    48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000,
};

inline constexpr std::size_t kMaxRateCandidates = kResamplingSampleRates.size() + 2;

struct SampleRateCandidates final {
    std::array<std::uint32_t, kMaxRateCandidates> values{};
    std::size_t count{};
};

constexpr SampleRateCandidates SelectOutputSampleRates(
    std::uint32_t sourceRate, bool allowResampling, std::uint32_t deviceRateHint = 0) noexcept {
    if (sourceRate < 8000 || sourceRate > 768000) return {};

    SampleRateCandidates result{};
    result.values[result.count++] = sourceRate;
    if (!allowResampling) return result;

    // Native first, then the nearest higher rate (retaining source bandwidth),
    // then descending lower rates. A device's mix rate is only a probe hint;
    // every candidate must still pass exclusive support and initialization.
    const auto append = [&](std::uint32_t rate) {
        if (rate < 8000 || rate > 768000) return;
        for (std::size_t i = 0; i < result.count; ++i)
            if (result.values[i] == rate) return;
        result.values[result.count++] = rate;
    };
    append(deviceRateHint);
    for (const auto rate : kResamplingSampleRates) {
        append(rate);
    }
    const auto before = [=](std::uint32_t a, std::uint32_t b) {
        if ((a > sourceRate) != (b > sourceRate)) return a > sourceRate;
        return a > sourceRate ? a < b : a > b;
    };
    for (std::size_t i = 2; i < result.count; ++i) {
        const auto rate = result.values[i];
        auto j = i;
        while (j > 1 && before(rate, result.values[j - 1])) {
            result.values[j] = result.values[j - 1];
            --j;
        }
        result.values[j] = rate;
    }
    return result;
}

struct EndpointFormatCandidates final {
    // Original candidates, two 24-bit and three 16-bit containers, and float32.
    std::array<PcmFormat, kMaxRateCandidates * (kMaxFormatCandidates + 6)> values{};
    std::size_t count{};
};

inline EndpointFormatCandidates SelectEndpointFormats(
    const PcmFormat& source, const PcmFormat* native, std::uint32_t nativeCount,
    bool allowConversion, std::uint32_t deviceRateHint = 0) noexcept {
    EndpointFormatCandidates result{};
    if (!IsValidFormat(source) || !native || nativeCount == 0 ||
        nativeCount > kMaxFormatCandidates) return result;
    const auto sourceBits = EffectiveSourceValidBits(source);
    const auto appendFloat = [&](std::uint32_t rate) {
        auto candidate = source;
        candidate.sampleRate = rate;
        candidate.sourceValidBits = sourceBits;
        candidate.validBits = candidate.containerBits = 32;
        candidate.encoding = PcmEncoding::IeeeFloat;
        result.values[result.count++] = candidate;
    };
    const auto append = [&](std::uint32_t rate, std::uint16_t bits) {
        // Float32 retains more resolution at low levels than uniform PCM24;
        // try it before dithered reduction of a 32-bit source to integer24.
        if (allowConversion && sourceBits > 24 && bits == 24) appendFloat(rate);
        if (bits == sourceBits) {
            for (std::uint32_t i = 0; i < nativeCount; ++i) {
                auto candidate = native[i];
                candidate.sampleRate = rate;
                result.values[result.count++] = candidate;
            }
        } else {
            for (const std::uint16_t container : {std::uint16_t{16}, std::uint16_t{24}, std::uint16_t{32}}) {
                if (container < bits) continue;
                auto candidate = source;
                candidate.sampleRate = rate;
                candidate.sourceValidBits = sourceBits;
                candidate.validBits = bits;
                candidate.containerBits = container;
                result.values[result.count++] = candidate;
            }
        }
        // Float32 carries 24 significant bits. Prefer integer source precision
        // first; place float above reducing a 24/32-bit source to integer16.
        if (allowConversion && sourceBits <= 24 && bits == sourceBits) appendFloat(rate);
    };
    if (!allowConversion) {
        append(source.sampleRate, sourceBits);
        return result;
    }
    const auto rates = SelectOutputSampleRates(source.sampleRate, true, deviceRateHint);
    const auto fullBandFloor = source.sampleRate < 44100u ? source.sampleRate : 44100u;
    // Original lossless output is first. Within full-band rates, preserve
    // precision before ultrasonic bandwidth (48/24 before 96/16). Only then
    // consider lower-bandwidth outputs, best rate first (48/16 before 8/24).
    const std::array<std::uint16_t, 3> depths{sourceBits, 24, 16};
    for (std::size_t d = 0; d < depths.size(); ++d) {
        const auto bits = depths[d];
        if (d != 0 && bits >= sourceBits) continue;
        for (std::size_t r = 0; r < rates.count; ++r)
            if (rates.values[r] >= fullBandFloor) append(rates.values[r], bits);
    }
    for (std::size_t r = 0; r < rates.count; ++r) {
        if (rates.values[r] >= fullBandFloor) continue;
        append(rates.values[r], sourceBits);
        if (sourceBits > 24) append(rates.values[r], 24);
        if (sourceBits > 16) append(rates.values[r], 16);
    }
    return result;
}

} // namespace ammod::audio_v2
