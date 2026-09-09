#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace ammod::audio {

inline std::int64_t QuantizeFloat32(float input, std::uint32_t sourceBitDepth) {
    const std::int64_t negativeLimit = -(std::int64_t{1} << (sourceBitDepth - 1));
    const std::int64_t positiveLimit = (std::int64_t{1} << (sourceBitDepth - 1)) - 1;
    if (std::isnan(input)) return 0;
    if (input >= 1.0f) return positiveLimit;
    if (input <= -1.0f) return negativeLimit;
    const double quantizer = static_cast<double>(std::uint64_t{1} << (sourceBitDepth - 1));
    auto quantized = static_cast<std::int64_t>(std::llround(
        static_cast<double>(input) * quantizer));
    if (quantized > positiveLimit) quantized = positiveLimit;
    if (quantized < negativeLimit) quantized = negativeLimit;
    return quantized;
}

inline bool Float32ToLeftAlignedPcm32InPlace(void* buffer, std::size_t sampleCount,
                                             std::uint32_t sourceBitDepth) {
    if (!buffer || (sourceBitDepth != 16 && sourceBitDepth != 20 &&
                    sourceBitDepth != 24 && sourceBitDepth != 32)) {
        return false;
    }

    auto* bytes = static_cast<std::byte*>(buffer);
    const std::uint32_t paddingBits = 32u - sourceBitDepth;

    for (std::size_t index = 0; index < sampleCount; ++index) {
        float input{};
        std::memcpy(&input, bytes + index * sizeof(float), sizeof(input));

        const auto quantized = QuantizeFloat32(input, sourceBitDepth);

        const auto leftAligned = static_cast<std::int32_t>(quantized *
            static_cast<std::int64_t>(std::uint64_t{1} << paddingBits));
        std::memcpy(bytes + index * sizeof(leftAligned), &leftAligned, sizeof(leftAligned));
    }
    return true;
}

inline bool Float32ToPcm16(const void* inputBuffer, void* outputBuffer,
                           std::size_t sampleCount) {
    if (!inputBuffer || !outputBuffer) return false;
    const auto* input = static_cast<const std::byte*>(inputBuffer);
    auto* output = static_cast<std::byte*>(outputBuffer);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        float sample{};
        std::memcpy(&sample, input + index * sizeof(sample), sizeof(sample));
        const auto quantized = static_cast<std::int16_t>(QuantizeFloat32(sample, 16));
        std::memcpy(output + index * sizeof(quantized), &quantized, sizeof(quantized));
    }
    return true;
}

} // namespace ammod::audio
