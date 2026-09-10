#pragma once

#include <cmath>
#include <array>
#include <cstddef>
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
    return bitDepth == 16 || bitDepth == 24 || bitDepth == 32;
}

constexpr std::uint16_t OutputValidBitsForSource(std::uint32_t sourceBitDepth) noexcept {
    return sourceBitDepth == 16 || sourceBitDepth == 24 || sourceBitDepth == 32
        ? static_cast<std::uint16_t>(sourceBitDepth) : 0;
}

struct BitPerfectFormatTuple final {
    std::uint16_t validBits{};
    std::uint16_t containerBits{};
    constexpr bool operator==(const BitPerfectFormatTuple&) const noexcept = default;
};

struct BitPerfectFormatCandidates final {
    std::array<BitPerfectFormatTuple, 3> values{};
    std::size_t count{};
};

constexpr BitPerfectFormatCandidates SelectBitPerfectFormatCandidates(
    std::uint32_t sourceBitDepth, bool localFloat32 = false) noexcept {
    if (localFloat32) {
        return sourceBitDepth == 32
            ? BitPerfectFormatCandidates{{{{32, 32}, {}, {}}}, 1}
            : BitPerfectFormatCandidates{};
    }
    switch (sourceBitDepth) {
    case 16: return {{{{16, 16}, {16, 24}, {16, 32}}}, 3};
    case 24: return {{{{24, 24}, {24, 32}, {}}}, 2};
    case 32: return {{{{32, 32}, {}, {}}}, 1};
    default: return {};
    }
}

constexpr bool IsUnsupportedLocalInt32(std::uint32_t formatId,
                                       std::uint32_t formatFlags,
                                       std::uint32_t sourceBitDepth) noexcept {
    return formatId == kLinearPcm && sourceBitDepth == 32 &&
           (formatFlags & kFormatFlagIsFloat) == 0 &&
           (formatFlags & kFormatFlagIsSignedInteger) != 0;
}

static_assert(SelectBitPerfectFormatCandidates(16).count == 3);
static_assert(SelectBitPerfectFormatCandidates(16).values[0] == BitPerfectFormatTuple{16, 16});
static_assert(SelectBitPerfectFormatCandidates(16).values[1] == BitPerfectFormatTuple{16, 24});
static_assert(SelectBitPerfectFormatCandidates(16).values[2] == BitPerfectFormatTuple{16, 32});
static_assert(SelectBitPerfectFormatCandidates(24).count == 2);
static_assert(SelectBitPerfectFormatCandidates(24).values[0] == BitPerfectFormatTuple{24, 24});
static_assert(SelectBitPerfectFormatCandidates(24).values[1] == BitPerfectFormatTuple{24, 32});
static_assert(SelectBitPerfectFormatCandidates(32, true).count == 1);
static_assert(SelectBitPerfectFormatCandidates(32, true).values[0] == BitPerfectFormatTuple{32, 32});
static_assert(SelectBitPerfectFormatCandidates(20).count == 0);

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
