#pragma once

#include "AudioCoreGate.h"

#include <atomic>
#include <cstdint>

namespace ammod::audio_v2 {

// Owns the final native-output handoff. AudioCoreGate describes the complete
// v2 lifecycle; this latch is the smaller boundary consulted by the native
// renderer detour. It stays open until the replacement sink has been started,
// and it is cleared before a generation is retired.
class AppleOutputGate final {
public:
    explicit AppleOutputGate(AudioCoreGate& coreGate) noexcept
        : coreGate_(coreGate) {}

    AppleOutputGate(const AppleOutputGate&) = delete;
    AppleOutputGate& operator=(const AppleOutputGate&) = delete;

    void Reset() noexcept {
        suppressedGeneration_.store(0, std::memory_order_release);
    }

    // CommitCutover is the only operation that can close Apple's native
    // output. The coordinator calls it only after sink Start succeeds.
    bool CommitCutover(AudioCoreGateToken token) noexcept {
        // Publish the latch before publishing the Active phase. ShouldSuppress
        // requires both values, so this ordering removes the inverse window
        // in which the phase is Active but the native detour still sees an
        // open latch. If the phase transition loses a race, clear the latch
        // immediately and leave the generation fail-closed.
        suppressedGeneration_.store(token.generation, std::memory_order_release);
        if (coreGate_.CommitCutover(token)) return true;
        std::uint64_t expected = token.generation;
        suppressedGeneration_.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
        coreGate_.Fail(token);
        return false;
    }

    // Rollback is safe to call after a failed start or during UI shutdown.
    // Clearing the latch happens first, so an in-flight native callback sees
    // an open output path even if the phase transition follows it.
    void Rollback(AudioCoreGateToken token) noexcept {
        std::uint64_t expected = token.generation;
        suppressedGeneration_.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
        coreGate_.Fail(token);
    }

    bool ShouldSuppress(AudioCoreGateToken token) const noexcept {
        if (token.generation == 0 ||
            suppressedGeneration_.load(std::memory_order_acquire) != token.generation ||
            token.generation != coreGate_.Generation() || !coreGate_.UiEnabled()) {
            return false;
        }
        const auto phase = coreGate_.Phase();
        // Keep Apple's path closed while the coordinator thread tears down a
        // failed replacement sink. Rollback/Reset clears the latch only after
        // that sink is stopped, so a fatal producer or render error cannot
        // create a brief double-render window.
        return phase == AudioCoreGatePhase::Active || phase == AudioCoreGatePhase::Faulted;
    }

    std::uint64_t SuppressedGeneration() const noexcept {
        return suppressedGeneration_.load(std::memory_order_acquire);
    }

private:
    AudioCoreGate& coreGate_;
    std::atomic<std::uint64_t> suppressedGeneration_{};
};

} // namespace ammod::audio_v2
