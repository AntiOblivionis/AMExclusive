#pragma once

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ksmedia.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ammod::audio_v2 {

inline constexpr std::uint32_t kStereoChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
inline constexpr std::uint32_t kSupportedChannels = 2;
inline constexpr std::uint32_t kMaxFormatCandidates = 3;
inline constexpr std::uint32_t kMaxBlockFrames = 2048;
inline constexpr std::uint32_t kMaxBlockSamples = kMaxBlockFrames * kSupportedChannels;

enum class PcmEncoding : std::uint8_t {
    SignedInteger = 0,
    IeeeFloat = 1,
};

struct PcmFormat final {
    std::uint32_t sampleRate{};
    std::uint16_t channels{};
    // validBits is the valid-bit declaration sent to WASAPI. A source may be
    // narrower than that declaration when the endpoint has no matching
    // packed/valid-bit tuple (for example, 16-bit source in a 24-valid-in-32
    // endpoint container).
    std::uint16_t validBits{};
    std::uint16_t containerBits{};
    PcmEncoding encoding{PcmEncoding::SignedInteger};
    // The queue/sink canonical layout. Planar decoder output is normalized at
    // the queue boundary before this contract is recorded.
    bool interleaved{true};
    // Zero means source precision equals validBits, preserving the compact
    // aggregate form used by existing callers.
    std::uint16_t sourceValidBits{};

    constexpr bool operator==(const PcmFormat&) const noexcept = default;
};

struct PcmBlock final {
    PcmFormat format{};
    std::uint64_t mediaGeneration{};
    std::uint64_t sequence{};
    std::uint64_t firstFrame{};
    std::uint32_t frames{};
    bool discontinuity{};
    bool endOfStream{};
    std::array<float, kMaxBlockSamples> samples{};

    PcmBlock() noexcept = default;

    PcmBlock(const PcmFormat& sourceFormat,
             std::uint64_t generation,
             std::uint64_t blockSequence,
             std::uint64_t sourceFrame,
             std::uint32_t frameCount,
             bool isDiscontinuity,
             bool isEndOfStream,
             const float* source) noexcept
        : format(sourceFormat), mediaGeneration(generation), sequence(blockSequence),
          firstFrame(sourceFrame), frames(frameCount), discontinuity(isDiscontinuity),
          endOfStream(isEndOfStream) {
        if (source && frameCount <= kMaxBlockFrames) {
            std::memcpy(samples.data(), source,
                        static_cast<std::size_t>(frameCount) * kSupportedChannels * sizeof(float));
        } else {
            frames = 0;
        }
    }

    PcmBlock(const PcmFormat& sourceFormat,
             std::uint64_t generation,
             std::uint64_t blockSequence,
             std::uint64_t sourceFrame,
             std::uint32_t frameCount,
             bool isDiscontinuity,
             bool isEndOfStream,
             const float* left,
             const float* right) noexcept
        : format(sourceFormat), mediaGeneration(generation), sequence(blockSequence),
          firstFrame(sourceFrame), frames(frameCount), discontinuity(isDiscontinuity),
          endOfStream(isEndOfStream) {
        if (!left || !right || frameCount > kMaxBlockFrames) {
            frames = 0;
            return;
        }
        for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
            samples[static_cast<std::size_t>(frame) * kSupportedChannels] = left[frame];
            samples[static_cast<std::size_t>(frame) * kSupportedChannels + 1u] = right[frame];
        }
    }

    PcmBlock(const PcmBlock&) = delete;
    PcmBlock& operator=(const PcmBlock&) = delete;
    PcmBlock(PcmBlock&&) = delete;
    PcmBlock& operator=(PcmBlock&&) = delete;
};

constexpr bool IsSupportedSourceDepth(std::uint16_t bits) noexcept {
    return bits == 16 || bits == 24 || bits == 32;
}

constexpr std::uint16_t EffectiveSourceValidBits(const PcmFormat& format) noexcept {
    return format.sourceValidBits == 0 ? format.validBits : format.sourceValidBits;
}

constexpr bool IsValidFormat(const PcmFormat& format) noexcept {
    return format.sampleRate >= 8000 && format.sampleRate <= 768000 &&
           format.channels == kSupportedChannels && format.validBits >= 16 &&
           format.validBits <= format.containerBits &&
           (format.containerBits == 16 || format.containerBits == 24 ||
            format.containerBits == 32) &&
           format.encoding == PcmEncoding::SignedInteger && format.interleaved &&
           IsSupportedSourceDepth(format.validBits) &&
           IsSupportedSourceDepth(EffectiveSourceValidBits(format)) &&
           EffectiveSourceValidBits(format) <= format.validBits;
}

inline bool PackCanonicalPcm32(const std::int32_t* source,
                               BYTE* destination,
                               std::size_t sampleCount,
                               std::uint16_t containerBits) noexcept {
    if ((!source || !destination) && sampleCount != 0) return false;
    if (containerBits == 32) {
        std::memcpy(destination, source, sampleCount * sizeof(*source));
        return true;
    }
    if (containerBits == 24) {
        for (std::size_t index = 0; index < sampleCount; ++index) {
            const auto value = static_cast<std::uint32_t>(source[index]) >> 8u;
            destination[index * 3u] = static_cast<BYTE>(value);
            destination[index * 3u + 1u] = static_cast<BYTE>(value >> 8u);
            destination[index * 3u + 2u] = static_cast<BYTE>(value >> 16u);
        }
        return true;
    }
    if (containerBits == 16) {
        for (std::size_t index = 0; index < sampleCount; ++index) {
            const auto value = static_cast<std::uint32_t>(source[index]) >> 16u;
            destination[index * 2u] = static_cast<BYTE>(value);
            destination[index * 2u + 1u] = static_cast<BYTE>(value >> 8u);
        }
        return true;
    }
    return false;
}

inline bool MakeWasapiFormat(const PcmFormat& source, WAVEFORMATEXTENSIBLE& output) noexcept {
    if (!IsValidFormat(source)) return false;
    output = {};
    output.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    output.Format.nChannels = source.channels;
    output.Format.nSamplesPerSec = source.sampleRate;
    output.Format.wBitsPerSample = source.containerBits;
    output.Format.nBlockAlign = static_cast<WORD>(
        source.channels * (source.containerBits / 8u));
    output.Format.nAvgBytesPerSec = source.sampleRate * output.Format.nBlockAlign;
    output.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    output.Samples.wValidBitsPerSample = source.validBits;
    output.dwChannelMask = kStereoChannelMask;
    output.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return true;
}

} // namespace ammod::audio_v2
