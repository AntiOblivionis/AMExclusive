#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <mutex>

namespace ammod::audio_v2 {

// The audiohost process enables this. Setting a hard floor inside
// AppleMusic.exe would pin the player's whole working set, so the hook keeps
// the soft behavior.
inline std::atomic<bool>& HardWorkingSetFloorEnabled() noexcept {
    static std::atomic<bool> enabled{false};
    return enabled;
}

inline void SetHardWorkingSetFloorEnabled(bool enabled) noexcept {
    HardWorkingSetFloorEnabled().store(enabled, std::memory_order_release);
}

#ifndef QUOTA_LIMITS_HARDWS_MIN_ENABLE
#define QUOTA_LIMITS_HARDWS_MIN_ENABLE 0x00010000
#endif

// Raise the minimum working set so VirtualLock can cover the buffers and the
// trimmer stops touching them. The Ex form with HARDWS_MIN makes the floor
// binding (the memory manager may not trim below it); it needs only the usual
// quota privilege and works with an ordinary user token. Falls back to the
// plain soft limit when it is unavailable or refused.
inline bool RaiseWorkingSetFloor(std::size_t bytes) noexcept {
    if (bytes == 0) return false;
    const SIZE_T minimum = static_cast<SIZE_T>(bytes);
    const SIZE_T maximum = minimum + static_cast<SIZE_T>(128ull * 1024 * 1024);
    if (HardWorkingSetFloorEnabled().load(std::memory_order_acquire)) {
        using SetWorkingSetExFn = BOOL(WINAPI*)(HANDLE, SIZE_T, SIZE_T, DWORD);
        static SetWorkingSetExFn setEx = []() noexcept -> SetWorkingSetExFn {
            const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            return kernel ? reinterpret_cast<SetWorkingSetExFn>(
                GetProcAddress(kernel, "SetProcessWorkingSetSizeEx")) : nullptr;
        }();
        if (setEx && setEx(GetCurrentProcess(), minimum, maximum,
                           QUOTA_LIMITS_HARDWS_MIN_ENABLE)) {
            return true;
        }
    }
    return SetProcessWorkingSetSize(GetCurrentProcess(), minimum, maximum) != FALSE;
}

// Windows caps VirtualLock at the process's minimum working-set size, which
// defaults to a few hundred kilobytes - far below the capture queue and the
// stream ring. Raising the floor once lets every later lock succeed; no
// privilege is needed. The floor is permanent for the process lifetime, so the
// value is grown monotonically and never lowered again.
inline bool EnsureLockableWorkingSet(std::size_t bytes) noexcept {
    static std::mutex mutex;
    static std::size_t granted = 0;
    std::lock_guard lock(mutex);
    if (granted >= bytes) return true;
    // Headroom for the next stream: a format change can ask for a bigger ring.
    const std::size_t target = bytes + 16ull * 1024 * 1024;
    if (!RaiseWorkingSetFloor(target)) return false;
    granted = target;
    return true;
}

// Best effort: the buffers work unchanged when the lock is refused, a trimmed
// page only costs a page fault on whichever thread touches it next.
inline bool LockBuffer(void* base, std::size_t bytes) noexcept {
    if (!base || bytes == 0) return false;
    if (!EnsureLockableWorkingSet(bytes)) return false;
    return VirtualLock(base, bytes) != FALSE;
}

inline void UnlockBuffer(void* base, std::size_t bytes) noexcept {
    if (!base || bytes == 0) return;
    (void)VirtualUnlock(base, bytes);
}

} // namespace ammod::audio_v2
