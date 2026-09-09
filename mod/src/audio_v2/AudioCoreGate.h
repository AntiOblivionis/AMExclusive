#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace ammod::audio_v2 {

// The UI intent is an authority boundary for Audio Core v2. A token is tied
// to one enable generation, so an old callback cannot keep capturing or
// suppressing Apple's renderer after the switch is turned off and on again.
enum class AudioCoreGatePhase : std::uint8_t {
    Off,
    Armed,
    Capturing,
    Prebuffered,
    Active,
    Faulted,
};

struct AudioCoreGateToken final {
    std::uint64_t generation{};
};

class AudioCoreGate final {
public:
    AudioCoreGate() noexcept = default;

    AudioCoreGate(const AudioCoreGate&) = delete;
    AudioCoreGate& operator=(const AudioCoreGate&) = delete;

    // Quality locking deliberately does not call this class. This switch
    // controls only capture, endpoint ownership and native-output cutover.
    void SetUiEnabled(bool enabled) noexcept {
        if (!enabled) {
            uiEnabled_.store(false, std::memory_order_release);
            generation_.fetch_add(1, std::memory_order_acq_rel);
            phase_.store(AudioCoreGatePhase::Off, std::memory_order_release);
            return;
        }

        bool expected = false;
        if (uiEnabled_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            generation_.fetch_add(1, std::memory_order_acq_rel);
            phase_.store(AudioCoreGatePhase::Armed, std::memory_order_release);
        }
    }

    bool UiEnabled() const noexcept {
        return uiEnabled_.load(std::memory_order_acquire);
    }

    std::uint64_t Generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

    AudioCoreGatePhase Phase() const noexcept {
        return phase_.load(std::memory_order_acquire);
    }

    std::optional<AudioCoreGateToken> BeginCapture() noexcept {
        if (!UiEnabled()) return std::nullopt;
        auto expected = AudioCoreGatePhase::Armed;
        if (!phase_.compare_exchange_strong(expected, AudioCoreGatePhase::Capturing,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return std::nullopt;
        }
        return AudioCoreGateToken{Generation()};
    }

    bool MarkPrebuffered(AudioCoreGateToken token) noexcept {
        if (!ValidToken(token)) return false;
        auto expected = AudioCoreGatePhase::Capturing;
        return phase_.compare_exchange_strong(expected, AudioCoreGatePhase::Prebuffered,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    // Native output may be suppressed only after the replacement sink is
    // initialized and prebuffered. This is the sole transition that makes
    // ShouldSuppressNative() true.
    bool CommitCutover(AudioCoreGateToken token) noexcept {
        if (!ValidToken(token)) return false;
        auto expected = AudioCoreGatePhase::Prebuffered;
        return phase_.compare_exchange_strong(expected, AudioCoreGatePhase::Active,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    void Fail(AudioCoreGateToken token) noexcept {
        if (ValidToken(token)) phase_.store(AudioCoreGatePhase::Faulted,
                                            std::memory_order_release);
    }

    bool CanCapture(AudioCoreGateToken token) const noexcept {
        if (!ValidToken(token)) return false;
        const auto phase = phase_.load(std::memory_order_acquire);
        return phase == AudioCoreGatePhase::Capturing ||
               phase == AudioCoreGatePhase::Prebuffered ||
               phase == AudioCoreGatePhase::Active;
    }

    bool CanOpenEndpoint(AudioCoreGateToken token) const noexcept {
        if (!ValidToken(token)) return false;
        const auto phase = phase_.load(std::memory_order_acquire);
        return phase == AudioCoreGatePhase::Capturing ||
               phase == AudioCoreGatePhase::Prebuffered;
    }

    bool CanMutateAudio(AudioCoreGateToken token) const noexcept {
        if (!ValidToken(token)) return false;
        const auto phase = phase_.load(std::memory_order_acquire);
        return phase == AudioCoreGatePhase::Capturing ||
               phase == AudioCoreGatePhase::Prebuffered ||
               phase == AudioCoreGatePhase::Active;
    }

    bool ShouldSuppressNative(AudioCoreGateToken token) const noexcept {
        return ValidToken(token) &&
               phase_.load(std::memory_order_acquire) == AudioCoreGatePhase::Active;
    }

private:
    bool ValidToken(AudioCoreGateToken token) const noexcept {
        return token.generation != 0 && token.generation == Generation() && UiEnabled();
    }

    std::atomic<bool> uiEnabled_{};
    std::atomic<std::uint64_t> generation_{};
    std::atomic<AudioCoreGatePhase> phase_{AudioCoreGatePhase::Off};
};

} // namespace ammod::audio_v2
