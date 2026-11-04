#pragma once

#include <windows.h>

#include <cstddef>
#include <mutex>

namespace ammod::audio_v2 {

// Windows caps VirtualLock at the process's minimum working-set size, which
// defaults to a few hundred kilobytes - far below the capture queue and the
// stream ring. Raising the floor once lets every later lock succeed; no
// privilege is needed, SetProcessWorkingSetSize only refuses unreasonable
// requests. The floor is permanent for the process lifetime, so the value is
// grown monotonically and never lowered again.
inline bool EnsureLockableWorkingSet(std::size_t bytes) noexcept {
    static std::mutex mutex;
    static std::size_t granted = 0;
    std::lock_guard lock(mutex);
    if (granted >= bytes) return true;
    // Headroom for the next stream: a format change can ask for a bigger ring.
    const std::size_t target = bytes + 16ull * 1024 * 1024;
    const std::size_t maximum = target + 128ull * 1024 * 1024;
    if (!SetProcessWorkingSetSize(GetCurrentProcess(), target, maximum)) return false;
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
