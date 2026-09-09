#pragma once

#include "PcmTypes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ammod::audio_v2 {

inline constexpr std::size_t kConverterRegistryCapacity = 128;

struct ConverterRegistration final {
    void* converter{};
    PcmFormat outputFormat{};
    std::uint32_t sourceBitDepth{};
    std::uint32_t encodedFormat{};
    std::uint64_t observationId{};
    std::uint64_t mediaGeneration{};
    std::uint32_t appleFormatFlags{};
    std::uint32_t appleBytesPerPacket{};
    std::uint32_t appleFramesPerPacket{};
    std::uint32_t appleBytesPerFrame{};
    std::uint32_t appleBitsPerChannel{};
    std::uint32_t appleChannelsPerFrame{};
};

struct ConverterRegistrationSnapshot final {
    std::size_t slot{};
    ConverterRegistration registration{};
    std::uint64_t nextFrame{};
    bool discontinuity{};
    bool bound{};
};

// A fixed-slot registry bridges non-real-time converter creation and the
// allocation-free FillComplexBuffer callback. Every published field is
// atomic; a callback can therefore race a dispose/rebind without observing a
// torn descriptor or dereferencing a freed map node. A reserved key value
// (1) is never exposed as a live converter pointer.
class AudioCoreConverterRegistry final {
public:
    AudioCoreConverterRegistry() noexcept = default;

    AudioCoreConverterRegistry(const AudioCoreConverterRegistry&) = delete;
    AudioCoreConverterRegistry& operator=(const AudioCoreConverterRegistry&) = delete;

    bool Register(const ConverterRegistration& registration) noexcept {
        if (!registration.converter || registration.mediaGeneration == 0 ||
            !IsValidFormat(registration.outputFormat) ||
            (registration.sourceBitDepth != 0 &&
             (registration.sourceBitDepth > UINT16_MAX ||
              !IsSupportedSourceDepth(static_cast<std::uint16_t>(registration.sourceBitDepth))))) {
            return false;
        }
        Slot* slot = FindSlot(registration.converter);
        if (!slot) {
            for (auto& candidate : slots_) {
                std::uintptr_t expected = 0;
                if (!candidate.key.compare_exchange_strong(
                        expected, kReservedKey, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    continue;
                }
                slot = &candidate;
                break;
            }
        }
        if (!slot) return false;

        // A replacement first disappears from callback lookup, then its
        // immutable descriptor is written, and finally the real key is
        // released with release semantics.
        slot->key.store(kReservedKey, std::memory_order_release);
        slot->rate.store(registration.outputFormat.sampleRate, std::memory_order_relaxed);
        slot->channels.store(registration.outputFormat.channels, std::memory_order_relaxed);
        slot->validBits.store(registration.outputFormat.validBits, std::memory_order_relaxed);
        slot->containerBits.store(registration.outputFormat.containerBits, std::memory_order_relaxed);
        slot->encoding.store(static_cast<std::uint8_t>(registration.outputFormat.encoding),
                             std::memory_order_relaxed);
        slot->interleaved.store(registration.outputFormat.interleaved,
                                std::memory_order_relaxed);
        slot->sourceValidBits.store(registration.outputFormat.sourceValidBits,
                                    std::memory_order_relaxed);
        slot->sourceBitDepth.store(registration.sourceBitDepth, std::memory_order_relaxed);
        slot->encodedFormat.store(registration.encodedFormat, std::memory_order_relaxed);
        slot->observationId.store(registration.observationId, std::memory_order_relaxed);
        slot->mediaGeneration.store(registration.mediaGeneration, std::memory_order_relaxed);
        slot->appleFormatFlags.store(registration.appleFormatFlags, std::memory_order_relaxed);
        slot->appleBytesPerPacket.store(registration.appleBytesPerPacket, std::memory_order_relaxed);
        slot->appleFramesPerPacket.store(registration.appleFramesPerPacket, std::memory_order_relaxed);
        slot->appleBytesPerFrame.store(registration.appleBytesPerFrame, std::memory_order_relaxed);
        slot->appleBitsPerChannel.store(registration.appleBitsPerChannel, std::memory_order_relaxed);
        slot->appleChannelsPerFrame.store(registration.appleChannelsPerFrame, std::memory_order_relaxed);
        slot->nextFrame.store(0, std::memory_order_relaxed);
        slot->discontinuity.store(true, std::memory_order_relaxed);
        slot->bound.store(false, std::memory_order_relaxed);
        slot->key.store(reinterpret_cast<std::uintptr_t>(registration.converter),
                        std::memory_order_release);
        return true;
    }

    bool UpdateSourcePrecision(void* converter, std::uint32_t sourceBitDepth,
                               std::uint32_t sampleRate, std::uint32_t channels) noexcept {
        if (!converter || !IsSupportedSourceDepth(static_cast<std::uint16_t>(sourceBitDepth))) {
            return false;
        }
        auto* slot = FindSlot(converter);
        if (!slot) return false;
        if (slot->rate.load(std::memory_order_acquire) != sampleRate ||
            slot->channels.load(std::memory_order_acquire) != channels) {
            return false;
        }
        slot->sourceBitDepth.store(sourceBitDepth, std::memory_order_release);
        slot->sourceValidBits.store(static_cast<std::uint16_t>(sourceBitDepth),
                                    std::memory_order_release);
        if (sourceBitDepth == 32) {
            slot->validBits.store(32, std::memory_order_release);
        }
        return true;
    }

    // The ALAC cookie is sometimes applied to the decoder-side converter
    // while the float32 P0 converter was created immediately before it.  The
    // two handles are therefore not guaranteed to be identical.  Resolve
    // that cross-handle relation on the coordinator thread by selecting the
    // newest compressed registration for the same decoded rate/channel pair.
    bool UpdateCompressedPrecision(std::uint32_t sourceBitDepth,
                                   std::uint32_t sampleRate,
                                   std::uint32_t channels,
                                   std::uint64_t observationId,
                                   void*& matchedConverter) noexcept {
        matchedConverter = nullptr;
        if (!IsSupportedSourceDepth(static_cast<std::uint16_t>(sourceBitDepth)) ||
            sampleRate == 0 || channels == 0) return false;
        Slot* best{};
        std::uint64_t bestObservation = 0;
        for (auto& slot : slots_) {
            if (slot.key.load(std::memory_order_acquire) <= kReservedKey ||
                slot.rate.load(std::memory_order_relaxed) != sampleRate ||
                slot.channels.load(std::memory_order_relaxed) != channels) {
                continue;
            }
            const auto encoded = slot.encodedFormat.load(std::memory_order_relaxed);
            if (encoded != 0x616C6163u && encoded != 0x716C6163u) continue;
            const auto observed = slot.observationId.load(std::memory_order_relaxed);
            if (observationId != 0 && observed != observationId) continue;
            if (observed < bestObservation) continue;
            best = &slot;
            bestObservation = observed;
        }
        if (!best) return false;
        best->sourceBitDepth.store(sourceBitDepth, std::memory_order_release);
        best->sourceValidBits.store(sourceBitDepth, std::memory_order_release);
        if (sourceBitDepth == 32) best->validBits.store(32, std::memory_order_release);
        const auto key = best->key.load(std::memory_order_acquire);
        if (key <= kReservedKey) return false;
        matchedConverter = reinterpret_cast<void*>(key);
        return true;
    }

    bool Bind(void* converter, std::uint64_t mediaGeneration) noexcept {
        auto* slot = FindSlot(converter);
        if (!slot || mediaGeneration == 0 ||
            slot->mediaGeneration.load(std::memory_order_acquire) != mediaGeneration ||
            !IsSupportedSourceDepth(static_cast<std::uint16_t>(
                slot->sourceBitDepth.load(std::memory_order_acquire)))) {
            return false;
        }
        slot->nextFrame.store(0, std::memory_order_release);
        slot->discontinuity.store(true, std::memory_order_release);
        slot->bound.store(true, std::memory_order_release);
        return true;
    }

    // Resolve an active-graph candidate from a format observed on another
    // thread. The AudioUnit property path supplies an observation id when it
    // can; otherwise prefer the newest supported compressed/PCM registration
    // with matching rate, channels and source precision. This runs on the
    // coordinator thread, never from FillComplexBuffer.
    bool FindCandidate(std::uint32_t sampleRate, std::uint32_t channels,
                       std::uint32_t sourceBitDepth,
                       std::uint64_t observationId,
                       void*& converter) const noexcept {
        converter = nullptr;
        std::uint64_t bestObservation = 0;
        const auto matches = [&](const Slot& slot, bool exactObservation) {
            if (slot.key.load(std::memory_order_acquire) <= kReservedKey ||
                slot.rate.load(std::memory_order_relaxed) != sampleRate ||
                slot.channels.load(std::memory_order_relaxed) != channels ||
                slot.sourceBitDepth.load(std::memory_order_relaxed) != sourceBitDepth ||
                !IsSupportedSourceDepth(static_cast<std::uint16_t>(sourceBitDepth))) {
                return false;
            }
            const auto encoded = slot.encodedFormat.load(std::memory_order_relaxed);
            if (encoded != 0x616C6163u && encoded != 0x716C6163u &&
                encoded != 0x6C70636Du) { // 'alac', 'qlac' or 'lpcm'
                return false;
            }
            const auto observed = slot.observationId.load(std::memory_order_relaxed);
            return (!exactObservation || (observationId != 0 && observed == observationId)) &&
                observed >= bestObservation;
        };

        // An exact observation id is authoritative. A converter can only be
        // returned once the key is still published after the descriptor read.
        if (observationId != 0) {
            for (const auto& slot : slots_) {
                if (!matches(slot, true)) continue;
                const auto key = slot.key.load(std::memory_order_acquire);
                if (key <= kReservedKey) continue;
                converter = reinterpret_cast<void*>(key);
                return true;
            }
        }

        bestObservation = 0;
        for (const auto& slot : slots_) {
            if (!matches(slot, false)) continue;
            const auto key = slot.key.load(std::memory_order_acquire);
            if (key <= kReservedKey) continue;
            const auto observed = slot.observationId.load(std::memory_order_relaxed);
            if (observed < bestObservation) continue;
            bestObservation = observed;
            converter = reinterpret_cast<void*>(key);
        }
        return converter != nullptr;
    }

    // Same lookup as FindCandidate, but used when AudioUnit stream-format
    // metadata has no source precision.  The caller receives the precision
    // carried by the selected compressed registration and can then bind the
    // exact P0 converter without guessing from the output container.
    bool FindCandidateAnyPrecision(std::uint32_t sampleRate, std::uint32_t channels,
                                   std::uint64_t observationId,
                                   void*& converter,
                                   std::uint32_t& sourceBitDepth) const noexcept {
        converter = nullptr;
        sourceBitDepth = 0;
        const Slot* best{};
        std::uint64_t bestObservation = 0;
        for (const auto& slot : slots_) {
            if (slot.key.load(std::memory_order_acquire) <= kReservedKey ||
                slot.rate.load(std::memory_order_relaxed) != sampleRate ||
                slot.channels.load(std::memory_order_relaxed) != channels) {
                continue;
            }
            const auto encoded = slot.encodedFormat.load(std::memory_order_relaxed);
            if (encoded != 0x616C6163u && encoded != 0x716C6163u) continue;
            const auto depth = slot.sourceBitDepth.load(std::memory_order_relaxed);
            // Zero is a valid provisional state: the P0 converter can be
            // attached to the AudioUnit before the ALAC cookie arrives.  The
            // coordinator will remain Capturing until UpdateSourcePrecision
            // supplies a supported depth, then complete the bind.
            if (depth != 0 && !IsSupportedSourceDepth(static_cast<std::uint16_t>(depth))) continue;
            const auto observed = slot.observationId.load(std::memory_order_relaxed);
            if (observationId != 0 && observed != observationId) continue;
            if (observed < bestObservation) continue;
            best = &slot;
            bestObservation = observed;
        }
        if (!best) return false;
        const auto key = best->key.load(std::memory_order_acquire);
        if (key <= kReservedKey) return false;
        converter = reinterpret_cast<void*>(key);
        sourceBitDepth = best->sourceBitDepth.load(std::memory_order_relaxed);
        return sourceBitDepth == 0 ||
            IsSupportedSourceDepth(static_cast<std::uint16_t>(sourceBitDepth));
    }

    bool UpdateMediaGeneration(void* converter, std::uint64_t mediaGeneration) noexcept {
        if (!converter || mediaGeneration == 0) return false;
        auto* slot = FindSlot(converter);
        if (!slot || slot->key.load(std::memory_order_acquire) == kReservedKey) return false;
        slot->mediaGeneration.store(mediaGeneration, std::memory_order_release);
        return true;
    }

    void Unbind(void* converter) noexcept {
        if (auto* slot = FindSlot(converter)) {
            slot->bound.store(false, std::memory_order_release);
            slot->discontinuity.store(true, std::memory_order_release);
        }
    }

    void Reset(void* converter) noexcept {
        if (auto* slot = FindSlot(converter)) {
            slot->nextFrame.store(0, std::memory_order_release);
            slot->discontinuity.store(true, std::memory_order_release);
        }
    }

    void Retire(void* converter) noexcept {
        if (auto* slot = FindSlot(converter)) {
            slot->bound.store(false, std::memory_order_release);
            slot->key.store(0, std::memory_order_release);
        }
    }

    void RetireAll() noexcept {
        for (auto& slot : slots_) {
            slot.bound.store(false, std::memory_order_release);
            slot.key.store(0, std::memory_order_release);
        }
    }

    bool Snapshot(void* converter, ConverterRegistrationSnapshot& snapshot) const noexcept {
        if (!converter) return false;
        const auto key = reinterpret_cast<std::uintptr_t>(converter);
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            const auto& slot = slots_[index];
            if (slot.key.load(std::memory_order_acquire) != key) continue;
            snapshot.slot = index;
            snapshot.registration.converter = converter;
            snapshot.registration.outputFormat = {
                slot.rate.load(std::memory_order_relaxed),
                static_cast<std::uint16_t>(slot.channels.load(std::memory_order_relaxed)),
                static_cast<std::uint16_t>(slot.validBits.load(std::memory_order_relaxed)),
                static_cast<std::uint16_t>(slot.containerBits.load(std::memory_order_relaxed)),
                static_cast<PcmEncoding>(slot.encoding.load(std::memory_order_relaxed)),
                slot.interleaved.load(std::memory_order_relaxed),
                static_cast<std::uint16_t>(slot.sourceValidBits.load(std::memory_order_relaxed)),
            };
            snapshot.registration.sourceBitDepth =
                slot.sourceBitDepth.load(std::memory_order_relaxed);
            snapshot.registration.encodedFormat =
                slot.encodedFormat.load(std::memory_order_relaxed);
            snapshot.registration.observationId =
                slot.observationId.load(std::memory_order_relaxed);
            snapshot.registration.mediaGeneration =
                slot.mediaGeneration.load(std::memory_order_relaxed);
            snapshot.registration.appleFormatFlags =
                slot.appleFormatFlags.load(std::memory_order_relaxed);
            snapshot.registration.appleBytesPerPacket =
                slot.appleBytesPerPacket.load(std::memory_order_relaxed);
            snapshot.registration.appleFramesPerPacket =
                slot.appleFramesPerPacket.load(std::memory_order_relaxed);
            snapshot.registration.appleBytesPerFrame =
                slot.appleBytesPerFrame.load(std::memory_order_relaxed);
            snapshot.registration.appleBitsPerChannel =
                slot.appleBitsPerChannel.load(std::memory_order_relaxed);
            snapshot.registration.appleChannelsPerFrame =
                slot.appleChannelsPerFrame.load(std::memory_order_relaxed);
            snapshot.nextFrame = slot.nextFrame.load(std::memory_order_relaxed);
            snapshot.discontinuity = slot.discontinuity.load(std::memory_order_relaxed);
            snapshot.bound = slot.bound.load(std::memory_order_acquire);
            // Validate publication once more. A concurrent retire/reuse makes
            // this callback a no-op instead of using a mixed descriptor.
            return slot.key.load(std::memory_order_acquire) == key;
        }
        return false;
    }

    bool ClaimFrames(void* converter, std::uint32_t frames,
                     std::uint64_t& firstFrame, bool& discontinuity) noexcept {
        if (!converter || frames == 0) return false;
        const auto key = reinterpret_cast<std::uintptr_t>(converter);
        for (auto& slot : slots_) {
            if (slot.key.load(std::memory_order_acquire) != key ||
                !slot.bound.load(std::memory_order_acquire)) continue;
            firstFrame = slot.nextFrame.fetch_add(frames, std::memory_order_acq_rel);
            discontinuity = slot.discontinuity.exchange(false, std::memory_order_acq_rel);
            return slot.key.load(std::memory_order_acquire) == key &&
                   slot.bound.load(std::memory_order_acquire);
        }
        return false;
    }

private:
    struct Slot final {
        static constexpr std::uintptr_t kEmpty = 0;
        std::atomic<std::uintptr_t> key{kEmpty};
        std::atomic<std::uint32_t> rate{};
        std::atomic<std::uint32_t> channels{};
        std::atomic<std::uint32_t> validBits{};
        std::atomic<std::uint32_t> containerBits{};
        std::atomic<std::uint8_t> encoding{};
        std::atomic<bool> interleaved{};
        std::atomic<std::uint32_t> sourceValidBits{};
        std::atomic<std::uint32_t> sourceBitDepth{};
        std::atomic<std::uint32_t> encodedFormat{};
        std::atomic<std::uint64_t> observationId{};
        std::atomic<std::uint64_t> mediaGeneration{};
        std::atomic<std::uint32_t> appleFormatFlags{};
        std::atomic<std::uint32_t> appleBytesPerPacket{};
        std::atomic<std::uint32_t> appleFramesPerPacket{};
        std::atomic<std::uint32_t> appleBytesPerFrame{};
        std::atomic<std::uint32_t> appleBitsPerChannel{};
        std::atomic<std::uint32_t> appleChannelsPerFrame{};
        std::atomic<std::uint64_t> nextFrame{};
        std::atomic<bool> discontinuity{};
        std::atomic<bool> bound{};
    };

    static constexpr std::uintptr_t kReservedKey = 1;

    Slot* FindSlot(void* converter) noexcept {
        const auto key = reinterpret_cast<std::uintptr_t>(converter);
        for (auto& slot : slots_) {
            if (slot.key.load(std::memory_order_acquire) == key) return &slot;
        }
        return nullptr;
    }

    const Slot* FindSlot(void* converter) const noexcept {
        const auto key = reinterpret_cast<std::uintptr_t>(converter);
        for (const auto& slot : slots_) {
            if (slot.key.load(std::memory_order_acquire) == key) return &slot;
        }
        return nullptr;
    }

    std::array<Slot, kConverterRegistryCapacity> slots_{};
};

} // namespace ammod::audio_v2
