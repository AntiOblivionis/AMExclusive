#pragma once

#include <cstdint>

namespace ammod::audio_v2 {

// The Apple AudioBufferList ABI is deliberately represented by these small
// POD views instead of exposing CoreAudioToolbox headers to the v2 core. The
// hook copies only the two descriptor words and the buffer pointers/lengths;
// sample memory remains owned by Apple's FillComplexBuffer call.
struct ApplePcmBufferView final {
    std::uint32_t numberChannels{};
    std::uint32_t dataByteSize{};
    const void* data{};
};

struct ApplePcmBufferListView final {
    std::uint32_t numberBuffers{};
    const ApplePcmBufferView* buffers{};
};

struct ApplePcmFormatView final {
    std::uint32_t formatId{};
    std::uint32_t formatFlags{};
    std::uint32_t bytesPerPacket{};
    std::uint32_t framesPerPacket{};
    std::uint32_t bytesPerFrame{};
    std::uint32_t channelsPerFrame{};
    std::uint32_t bitsPerChannel{};
    std::uint32_t sampleRate{};
};

inline constexpr std::uint32_t kAppleLinearPcm = 0x6C70636Du; // 'lpcm'
inline constexpr std::uint32_t kAppleFormatFlagIsFloat = 1u << 0;
inline constexpr std::uint32_t kAppleFormatFlagIsNonInterleaved = 1u << 5;

enum class ApplePcmLayout : std::uint8_t {
    Invalid,
    InterleavedFloat32,
    PlanarFloat32,
};

struct ApplePcmOutputView final {
    ApplePcmLayout layout{ApplePcmLayout::Invalid};
    const float* interleaved{};
    const float* left{};
    const float* right{};
    std::uint32_t frames{};
};

// The native status belongs to Apple's callback/decoder contract. A nonzero
// status may accompany a nonzero decoded block while the converter drains its
// internal buffer, so status alone must not gate the v2 observer/queue.
inline bool ShouldForwardPcmOutput(bool uiEnabled, std::int32_t nativeResult,
                                   std::uint32_t producedPackets,
                                   bool outputDataPresent,
                                   bool outputPacketsPresent) noexcept {
    (void)nativeResult;
    return uiEnabled && producedPackets != 0 && outputDataPresent && outputPacketsPresent;
}

// Parse only the decoder's float32 LPCM return layouts that the v2 queue can
// consume without a conversion stage. The check is intentionally strict:
// malformed/unknown layouts are ignored and the Apple path remains untouched.
inline bool ParseApplePcmOutput(const ApplePcmFormatView& format,
                                const ApplePcmBufferListView& list,
                                std::uint32_t producedPackets,
                                ApplePcmOutputView& output) noexcept {
    output = {};
    if (format.formatId != kAppleLinearPcm ||
        (format.formatFlags & kAppleFormatFlagIsFloat) == 0 ||
        format.bitsPerChannel != 32 || format.channelsPerFrame != 2 ||
        format.framesPerPacket != 1 || format.bytesPerPacket == 0 ||
        format.sampleRate < 8000 || format.sampleRate > 768000 ||
        !list.buffers || list.numberBuffers == 0 || list.numberBuffers > 8 ||
        producedPackets == 0) {
        return false;
    }

    const bool planar = (format.formatFlags & kAppleFormatFlagIsNonInterleaved) != 0;
    if (!planar) {
        if (list.numberBuffers != 1) return false;
        const auto& buffer = list.buffers[0];
        if (buffer.numberChannels != 2 || !buffer.data ||
            format.bytesPerFrame != sizeof(float) * 2u ||
            buffer.dataByteSize < sizeof(float) * 2u ||
            buffer.dataByteSize % (sizeof(float) * 2u) != 0) {
            return false;
        }
        const auto frameCount = static_cast<std::uint64_t>(buffer.dataByteSize) /
            (sizeof(float) * 2u);
        if (frameCount > UINT32_MAX || frameCount != producedPackets) return false;
        const auto frames = static_cast<std::uint32_t>(frameCount);
        output.layout = ApplePcmLayout::InterleavedFloat32;
        output.interleaved = static_cast<const float*>(buffer.data);
        output.frames = frames;
        return true;
    }

    if (list.numberBuffers != 2 || format.bytesPerFrame != sizeof(float)) return false;
    const auto& left = list.buffers[0];
    const auto& right = list.buffers[1];
    if (left.numberChannels != 1 || right.numberChannels != 1 ||
        !left.data || !right.data || left.dataByteSize < sizeof(float) ||
        right.dataByteSize != left.dataByteSize ||
        left.dataByteSize % sizeof(float) != 0) {
        return false;
    }
    const auto frameCount = static_cast<std::uint64_t>(left.dataByteSize) / sizeof(float);
    if (frameCount > UINT32_MAX || frameCount != producedPackets) return false;
    const auto frames = static_cast<std::uint32_t>(frameCount);
    output.layout = ApplePcmLayout::PlanarFloat32;
    output.left = static_cast<const float*>(left.data);
    output.right = static_cast<const float*>(right.data);
    output.frames = frames;
    return true;
}

} // namespace ammod::audio_v2
