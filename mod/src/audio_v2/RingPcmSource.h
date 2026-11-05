#pragma once

// Host-side source: turns the cross-process ring into the IIntegerPcmSource
// that WasapiExclusiveSink / AsioExclusiveSink already know how to drain.

#include "IntegerPcmSource.h"
#include "SharedAudioRing.h"

#include <cstddef>
#include <cstdint>

namespace ammod::audio_v2 {

class RingPcmSource final : public IIntegerPcmSource {
public:
    bool Attach(void* base, HANDLE spaceEvent) noexcept {
        return ring_.Attach(base, spaceEvent);
    }

    bool Attached() const noexcept { return ring_.Attached(); }

    HRESULT Prepare(std::uint32_t capacityFrames) noexcept override {
        if (!ring_.Attached() || capacityFrames == 0) return kAudioSourceWouldBlock;
        if (ring_.Available() >= capacityFrames) return S_OK;
        if (ring_.EndOfStream() && ring_.Available() == 0) return S_FALSE;
        return kAudioSourceWouldBlock;
    }

    std::size_t AvailableFrames() const noexcept override {
        return static_cast<std::size_t>(ring_.Available());
    }

    HRESULT Fill(std::int32_t* destination, std::uint32_t capacityFrames,
                 std::uint32_t& writtenFrames, bool& endOfStream) noexcept override {
        writtenFrames = ring_.Read(destination, capacityFrames);
        endOfStream = ring_.EndOfStream() && ring_.Available() == 0;
        return S_OK;
    }

private:
    SharedRingConsumer ring_{};
};

} // namespace ammod::audio_v2
