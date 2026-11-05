#pragma once

// Cross-process single-producer / single-consumer PCM ring.
//
// The hook process (AMPLibraryAgent) writes canonical Q31 interleaved int32
// frames; am-exclusive-audiohost.exe reads them and feeds the real endpoint.
// Both sides map the same named section, so the layout below must stay POD and
// identical in both translation units. Atomics are lock-free on x64 and live in
// the shared pages, which is what makes the indices safe across processes.

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ammod::audio_v2 {

inline constexpr std::uint32_t kSharedRingMagic = 0x31454D41; // 'AME1'
inline constexpr std::uint32_t kSharedRingVersion = 1;
inline constexpr std::uint32_t kSharedRingChannels = 2;
// 2,000,000 frames * 2 ch * 4 B = 16 MB. At 192 kHz that is ~10.4 s.
inline constexpr std::uint32_t kSharedRingMaxFrames = 2000000u;

struct SharedRingHeader final {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t capacityFrames{};
    std::uint32_t channels{};
    std::uint32_t sampleRate{};
    std::uint32_t validBits{};
    std::uint32_t containerBits{};
    std::uint32_t encoding{};
    std::uint32_t endOfStream{};
    std::uint32_t hostState{};   // 0 closed, 1 open, 2 running
    alignas(8) std::atomic<std::uint64_t> writeFrame{};
    alignas(8) std::atomic<std::uint64_t> readFrame{};
    alignas(8) std::atomic<std::uint64_t> generation{};
    alignas(8) std::atomic<std::uint64_t> producedFrames{};
    alignas(8) std::atomic<std::uint64_t> droppedFrames{};
    alignas(8) std::atomic<std::uint64_t> underrunFrames{};
};

inline constexpr std::size_t kSharedRingHeaderBytes = sizeof(SharedRingHeader);
inline constexpr std::size_t kSharedRingBytes =
    sizeof(SharedRingHeader) +
    static_cast<std::size_t>(kSharedRingMaxFrames) * kSharedRingChannels * sizeof(std::int32_t);

inline std::int32_t* SharedRingData(void* base) noexcept {
    return reinterpret_cast<std::int32_t*>(
        static_cast<std::uint8_t*>(base) + sizeof(SharedRingHeader));
}

inline void SharedRingReset(SharedRingHeader* header, std::uint32_t sampleRate,
                            std::uint32_t validBits, std::uint32_t containerBits,
                            std::uint32_t encoding, std::uint64_t generation,
                            std::uint32_t capacityFrames) noexcept {
    header->capacityFrames = capacityFrames;
    header->channels = kSharedRingChannels;
    header->sampleRate = sampleRate;
    header->validBits = validBits;
    header->containerBits = containerBits;
    header->encoding = encoding;
    header->endOfStream = 0;
    header->hostState = 1;
    header->writeFrame.store(0, std::memory_order_relaxed);
    header->readFrame.store(0, std::memory_order_relaxed);
    header->generation.store(generation, std::memory_order_release);
    header->producedFrames.store(0, std::memory_order_relaxed);
    header->droppedFrames.store(0, std::memory_order_relaxed);
    header->underrunFrames.store(0, std::memory_order_relaxed);
}

// Producer side (hook). Never blocks the caller for long: the pump thread may
// wait on the "space available" event, but the AM decoder callback never does.
class SharedRingProducer final {
public:
    bool Attach(void* base, HANDLE spaceEvent) noexcept {
        if (!base) return false;
        header_ = static_cast<SharedRingHeader*>(base);
        if (header_->magic != kSharedRingMagic || header_->version != kSharedRingVersion) {
            header_ = nullptr;
            return false;
        }
        data_ = SharedRingData(base);
        spaceEvent_ = spaceEvent;
        return true;
    }

    bool Attached() const noexcept { return header_ != nullptr; }

    std::uint64_t BufferedFrames() const noexcept {
        if (!header_) return 0;
        const auto write = header_->writeFrame.load(std::memory_order_acquire);
        const auto read = header_->readFrame.load(std::memory_order_acquire);
        return write >= read ? write - read : 0;
    }

    std::uint64_t FreeFrames() const noexcept {
        if (!header_) return 0;
        const auto buffered = BufferedFrames();
        return header_->capacityFrames > buffered ? header_->capacityFrames - buffered : 0;
    }

    // Writes up to frames; returns how many were copied. When the ring is
    // full and waitMs is non-zero, waits for the consumer's space event.
    std::uint32_t Write(const std::int32_t* source, std::uint32_t frames,
                        DWORD waitMs) noexcept {
        if (!header_ || !source || frames == 0 || !data_) return 0;
        const std::uint32_t capacity = header_->capacityFrames;
        if (capacity == 0) return 0;
        if (FreeFrames() < frames) {
            if (waitMs != 0 && spaceEvent_) {
                (void)WaitForSingleObject(spaceEvent_, waitMs);
            }
            if (FreeFrames() < frames) return 0;
        }
        const std::uint64_t write = header_->writeFrame.load(std::memory_order_relaxed);
        std::uint64_t offset = write % capacity;
        const std::uint32_t first = static_cast<std::uint32_t>(
            offset + frames <= capacity ? frames : capacity - offset);
        const std::size_t stride = kSharedRingChannels;
        std::memcpy(data_ + offset * stride, source,
                    static_cast<std::size_t>(first) * stride * sizeof(std::int32_t));
        if (first < frames) {
            std::memcpy(data_, source + static_cast<std::size_t>(first) * stride,
                        static_cast<std::size_t>(frames - first) * stride * sizeof(std::int32_t));
        }
        header_->writeFrame.store(write + frames, std::memory_order_release);
        header_->producedFrames.fetch_add(frames, std::memory_order_relaxed);
        return frames;
    }

    void MarkEndOfStream() noexcept {
        if (header_) header_->endOfStream = 1;
    }

    void MarkHostState(std::uint32_t value) noexcept {
        if (header_) header_->hostState = value;
    }

private:
    SharedRingHeader* header_{};
    std::int32_t* data_{};
    HANDLE spaceEvent_{};
};

// Consumer side (audiohost). Called from the WASAPI render thread or the ASIO
// bufferSwitch callback, so it never blocks and never allocates.
class SharedRingConsumer final {
public:
    bool Attach(void* base, HANDLE spaceEvent) noexcept {
        if (!base) return false;
        header_ = static_cast<SharedRingHeader*>(base);
        if (header_->magic != kSharedRingMagic || header_->version != kSharedRingVersion) {
            header_ = nullptr;
            return false;
        }
        data_ = SharedRingData(base);
        spaceEvent_ = spaceEvent;
        return true;
    }

    bool Attached() const noexcept { return header_ != nullptr; }
    const SharedRingHeader* Header() const noexcept { return header_; }

    std::uint32_t Available() const noexcept {
        if (!header_) return 0;
        const auto write = header_->writeFrame.load(std::memory_order_acquire);
        const auto read = header_->readFrame.load(std::memory_order_relaxed);
        return write >= read ? static_cast<std::uint32_t>(write - read) : 0;
    }

    bool EndOfStream() const noexcept { return header_ && header_->endOfStream != 0; }

    std::uint32_t Read(std::int32_t* destination, std::uint32_t frames) noexcept {
        if (!header_ || !destination || frames == 0 || !data_) return 0;
        const std::uint32_t capacity = header_->capacityFrames;
        if (capacity == 0) return 0;
        const std::uint32_t available = Available();
        const std::uint32_t count = available < frames ? available : frames;
        if (count == 0) return 0;
        const std::uint64_t read = header_->readFrame.load(std::memory_order_relaxed);
        std::uint64_t offset = read % capacity;
        const std::uint32_t first = static_cast<std::uint32_t>(
            offset + count <= capacity ? count : capacity - offset);
        const std::size_t stride = kSharedRingChannels;
        std::memcpy(destination, data_ + offset * stride,
                    static_cast<std::size_t>(first) * stride * sizeof(std::int32_t));
        if (first < count) {
            std::memcpy(destination + static_cast<std::size_t>(first) * stride, data_,
                        static_cast<std::size_t>(count - first) * stride * sizeof(std::int32_t));
        }
        header_->readFrame.store(read + count, std::memory_order_release);
        if (spaceEvent_) SetEvent(spaceEvent_);
        return count;
    }

    void MarkHostState(std::uint32_t value) noexcept {
        if (header_) header_->hostState = value;
    }

private:
    SharedRingHeader* header_{};
    std::int32_t* data_{};
    HANDLE spaceEvent_{};
};

} // namespace ammod::audio_v2
