#pragma once

#include <audioclient.h>
#include <algorithm>
#include <cstdint>
#include <mutex>

namespace ammod::audio_v2 {

// One software-terminal ledger shared by all render and clock service refs.
// This is the Apple decoder scheduler, not the physical DAC consumption clock.
// Each existing adaptive pump tick consumes one 10 ms audio quantum. Reads and
// reservations never consume audio; only a successful Release queues it.
class SoftwareRenderBuffer final {
public:
    SoftwareRenderBuffer(UINT32 rate, UINT32 capacity) noexcept
        : rate_(rate), capacity_(capacity) {}

    HRESULT Acquire(const void* owner, UINT32 frames) noexcept {
        std::lock_guard lock(mutex_);
        if (owner_) return AUDCLNT_E_OUT_OF_ORDER;
        if (frames > capacity_ - padding_) return AUDCLNT_E_BUFFER_TOO_LARGE;
        if (frames) { owner_ = owner; reserved_ = frames; }
        return S_OK;
    }

    HRESULT Commit(const void* owner, UINT32 frames, DWORD flags) noexcept {
        std::lock_guard lock(mutex_);
        if (flags & ~AUDCLNT_BUFFERFLAGS_SILENT) return E_INVALIDARG;
        if (!owner_ && frames == 0) return S_OK;
        if (owner_ != owner) return AUDCLNT_E_OUT_OF_ORDER;
        if (frames > reserved_) return AUDCLNT_E_INVALID_SIZE;
        padding_ += frames; // silent packets also occupy time; zero cancels
        owner_ = nullptr;
        reserved_ = 0;
        return S_OK;
    }

    void Abandon(const void* owner) noexcept {
        std::lock_guard lock(mutex_);
        if (owner_ == owner) { owner_ = nullptr; reserved_ = 0; }
    }

    HRESULT Start() noexcept {
        std::lock_guard lock(mutex_);
        if (running_) return AUDCLNT_E_NOT_STOPPED;
        running_ = true;
        return S_OK;
    }

    void Stop() noexcept {
        std::lock_guard lock(mutex_);
        running_ = false; // pause preserves queued frames and the clock
    }

    HRESULT Reset() noexcept {
        std::lock_guard lock(mutex_);
        if (running_) return AUDCLNT_E_NOT_STOPPED;
        if (owner_) return AUDCLNT_E_BUFFER_OPERATION_PENDING;
        padding_ = 0;
        position_ = 0;
        remainder_ = 0;
        return S_OK;
    }

    void Tick() noexcept {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        // Carry the fractional frame (e.g. 11025 Hz) instead of rounding up
        // every tick. Delayed/coalesced wakes never fabricate extra packets.
        const auto numerator = static_cast<std::uint64_t>(rate_) + remainder_;
        const auto quantum = static_cast<UINT32>(numerator / 100u);
        remainder_ = static_cast<UINT32>(numerator % 100u);
        const auto consumed = std::min(padding_, quantum);
        padding_ -= consumed;
        position_ += consumed;
    }

    UINT32 Padding() const noexcept {
        std::lock_guard lock(mutex_);
        return padding_;
    }

    UINT64 Position() const noexcept {
        std::lock_guard lock(mutex_);
        return position_;
    }

    UINT32 Rate() const noexcept { return rate_; }

private:
    mutable std::mutex mutex_;
    const UINT32 rate_;
    const UINT32 capacity_;
    const void* owner_{};
    UINT32 reserved_{};
    UINT32 padding_{};
    UINT32 remainder_{};
    UINT64 position_{};
    bool running_{};
};

} // namespace ammod::audio_v2
