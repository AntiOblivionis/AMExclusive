#pragma once

#include <cmath>
#include <cstdint>

namespace ammod::quality {

constexpr std::uint32_t ProfileCeiling(bool qualityLockEnabled, bool known, bool lossless,
                                        std::uint16_t tier) noexcept {
    if (!qualityLockEnabled || !known || !lossless) return 0;
    return tier == 20 ? 192000u : tier == 15 ? 48000u : 0u;
}

// Owned by one item/filter, never a process-global "latest decoder" value.
// A bandwidth-limited reevaluation may remove the best candidate from its input;
// that must produce no eligible result, not lower the quality already established.
struct Selection {
    std::uint32_t ceiling{};
    std::uint32_t bestRate{};

    std::uint32_t Rate(double rate) const noexcept {
        if (!std::isfinite(rate) || rate < 8000 || rate > ceiling ||
            std::floor(rate) != rate) return 0;
        return static_cast<std::uint32_t>(rate);
    }

    void Observe(bool allowedCodec, double rate) noexcept {
        const auto candidate = allowedCodec ? Rate(rate) : 0;
        if (candidate > bestRate) bestRate = candidate;
    }

    bool Accept(bool allowedCodec, double rate) const noexcept {
        return allowedCodec && bestRate != 0 && Rate(rate) == bestRate;
    }
};

} // namespace ammod::quality
