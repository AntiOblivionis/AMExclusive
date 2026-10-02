#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace ammod::audio_v2 {

// A producer can be temporarily empty while waiting for more decoded media.
// This is a recoverable wait, not a format or endpoint failure.
inline constexpr HRESULT kAudioSourceWouldBlock = static_cast<HRESULT>(0x88770001L);

// Endpoint-facing canonical integer PCM. All frame counts use this source's
// output sample rate, including when it wraps a source at a different rate.
class IIntegerPcmSource {
public:
    virtual ~IIntegerPcmSource() = default;

    // A sink must not acquire an endpoint buffer unless the source can fill
    // the complete period, or has reached an explicit end-of-stream tail.
    // This keeps a network wait from becoming fabricated output samples.
    virtual bool CanProvide(std::uint32_t capacityFrames) const noexcept {
        return capacityFrames != 0;
    }

    // Called before acquiring an endpoint buffer. Streaming converters may
    // consume input and stage output here, preserving partial work on a wait.
    // S_OK promises a full period or an explicit EOS tail for Fill().
    virtual HRESULT Prepare(std::uint32_t capacityFrames) noexcept {
        return CanProvide(capacityFrames) ? S_OK : kAudioSourceWouldBlock;
    }

    // Estimated pending output frames, including any converter lookahead.
    // Diagnostic only: this does not promise immediate availability. Use
    // Prepare() before Fill(); the render thread never drives the producer.
    virtual std::size_t AvailableFrames() const noexcept { return SIZE_MAX; }

    // The sink always asks for a complete endpoint buffer. A source may return
    // fewer frames only when endOfStream is true. It may return
    // kAudioSourceWouldBlock when waiting for more decoded media; the sink
    // keeps the generation alive in that case.
    virtual HRESULT Fill(std::int32_t* destination,
                         std::uint32_t capacityFrames,
                         std::uint32_t& writtenFrames,
                         bool& endOfStream) noexcept = 0;
};

} // namespace ammod::audio_v2
