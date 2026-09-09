#pragma once

#include <cmath>
#include <cstdint>

namespace ammod::audio {

inline constexpr std::uint32_t kLinearPcm = 0x6C70636D; // 'lpcm'
inline constexpr std::uint32_t kFormatFlagIsFloat = 1u << 0;
inline constexpr std::uint32_t kFormatFlagIsSignedInteger = 1u << 2;
inline constexpr std::uint32_t kFormatFlagIsNonInterleaved = 1u << 5;

struct ApplePcmFormat {
    double sampleRate{};
    std::uint32_t formatId{};
    std::uint32_t formatFlags{};
    std::uint32_t bytesPerPacket{};
    std::uint32_t framesPerPacket{};
    std::uint32_t bytesPerFrame{};
    std::uint32_t channels{};
    std::uint32_t bitsPerChannel{};
};

struct LocalPcmCandidate {
    std::uint32_t sampleRate{};
    std::uint32_t channels{};
    std::uint32_t sourceBitDepth{};
    bool sourceIsFloat{};
};

constexpr bool IsSupportedSourceBitDepth(std::uint32_t bitDepth) noexcept {
    return bitDepth == 16 || bitDepth == 20 || bitDepth == 24 || bitDepth == 32;
}

constexpr std::uint16_t OutputValidBitsForSource(std::uint32_t sourceBitDepth) noexcept {
    return sourceBitDepth == 32 ? 32 : 24;
}

constexpr std::uint32_t ScaleFrameCount(std::uint32_t frames,
                                        std::uint32_t fromRate,
                                        std::uint32_t toRate) noexcept {
    if (!frames || !fromRate || !toRate) return 0;
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(frames) * toRate + fromRate / 2u) / fromRate);
}

constexpr bool PreferExistingCompressedCandidate(std::uint32_t existingFormat,
                                                  std::uint32_t existingRate,
                                                  std::uint32_t existingChannels,
                                                  std::uint64_t existingAgeMs,
                                                  std::uint32_t localRate,
                                                  std::uint32_t localChannels) noexcept {
    return existingFormat != 0 && existingFormat != kLinearPcm && existingAgeMs <= 2000 &&
           existingRate == localRate && existingChannels == localChannels;
}

inline bool IsSaneLinearPcm(const ApplePcmFormat& format) noexcept {
    if (format.formatId != kLinearPcm || format.sampleRate < 8000.0 ||
        format.sampleRate > 768000.0 || format.channels != 2 ||
        !IsSupportedSourceBitDepth(format.bitsPerChannel) ||
        format.framesPerPacket != 1 || format.bytesPerFrame == 0 ||
        format.bytesPerPacket != format.bytesPerFrame) {
        return false;
    }
    const bool isFloat = (format.formatFlags & kFormatFlagIsFloat) != 0;
    const bool isSignedInteger = (format.formatFlags & kFormatFlagIsSignedInteger) != 0;
    if (isFloat == isSignedInteger || (isFloat && format.bitsPerChannel != 32)) return false;

    const std::uint32_t storedChannels =
        (format.formatFlags & kFormatFlagIsNonInterleaved) != 0 ? 1u : format.channels;
    const std::uint32_t minimumBytes =
        ((format.bitsPerChannel + 7u) / 8u) * storedChannels;
    const std::uint32_t maximumBytes = 4u * storedChannels;
    return format.bytesPerFrame >= minimumBytes && format.bytesPerFrame <= maximumBytes;
}

inline bool TryClassifyLocalPcmCandidate(const ApplePcmFormat& source,
                                         const ApplePcmFormat& destination,
                                         LocalPcmCandidate& candidate) noexcept {
    if (!IsSaneLinearPcm(source) || !IsSaneLinearPcm(destination)) return false;
    if ((destination.formatFlags & kFormatFlagIsFloat) == 0 ||
        destination.bitsPerChannel != 32) {
        return false;
    }
    const double roundedRate = std::floor(source.sampleRate + 0.5);
    if (std::fabs(source.sampleRate - roundedRate) > 0.01) return false;
    candidate.sampleRate = static_cast<std::uint32_t>(roundedRate);
    candidate.channels = source.channels;
    candidate.sourceBitDepth = source.bitsPerChannel;
    candidate.sourceIsFloat = (source.formatFlags & kFormatFlagIsFloat) != 0;
    return true;
}

} // namespace ammod::audio
