#pragma once

// JPLAY-style local playback tuning for the WASAPI / ASIO paths.
//
// Everything here is best effort and inert by default: with enabled=false the
// runtime keeps the exact pre-tuning behavior (0.1 s cutover, 256-block queue,
// MMCSS-only scheduling). No sample is ever read, scaled, resampled or
// reordered here - this module only changes where the bits live, which core
// carries them and how precisely the OS timer wakes the transport.
//
// All public functions are noexcept. The real-time entry point
// ApplyToCurrentThread() reads cached atomics and calls two kernel32 APIs; it
// never allocates, logs, resolves a function or takes a lock. Resolve/privilege
// work happens in Configure()/AllocateLargePages(), both control-plane calls.

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ammod::audio_v2 {

struct LocalPlaybackTuning final {
    // Master switch. Disabled reproduces the original pipeline exactly.
    bool enabled{};
    // Target deep reservoir for local playback, in milliseconds. This is a
    // *goal*, never a cutover gate: Apple Music may only be able to decode a
    // few seconds ahead, and playback must still start when it cannot.
    std::uint32_t reservoirMs{100};
    // 0 = leave the OS timer alone. 5000 = 0.5 ms (JPLAY Clock=0),
    // 10000 = 1 ms, 156250 = 15.6 ms (Windows default).
    std::uint32_t timerResolution100ns{};
    // Pin the render/pump/ASIO callback threads to one dedicated core.
    bool dedicatedCore{};
    // HIGH_PRIORITY_CLASS for the whole process while playback is armed.
    // This is the mild, opt-in equivalent of JPLAY Throttle: it raises only
    // this process, it never lowers or suspends anyone else's threads.
    bool processPriority{};
    // Disable Windows' dynamic priority boost on the audio threads so their
    // scheduling stays deterministic (MMCSS still owns the base priority).
    bool disableThreadBoost{true};
    // Try MEM_LARGE_PAGES for the capture queue; silently falls back to the
    // ordinary allocator when the privilege or contiguous memory is missing.
    bool largePages{};
    // Hard ceiling on capture-queue blocks (one block = kMaxBlockFrames).
    std::uint32_t maxQueueBlocks{4096};

    void Clamp() noexcept {
        if (reservoirMs < 100) reservoirMs = 100;
        if (reservoirMs > 30000) reservoirMs = 30000;
        if (timerResolution100ns != 0 && timerResolution100ns < 5000) timerResolution100ns = 5000;
        if (timerResolution100ns > 156250) timerResolution100ns = 156250;
        if (maxQueueBlocks < 256) maxQueueBlocks = 256;
        if (maxQueueBlocks > 16384) maxQueueBlocks = 16384;
    }

    bool SameAs(const LocalPlaybackTuning& other) const noexcept {
        return enabled == other.enabled &&
               reservoirMs == other.reservoirMs &&
               timerResolution100ns == other.timerResolution100ns &&
               dedicatedCore == other.dedicatedCore &&
               processPriority == other.processPriority &&
               disableThreadBoost == other.disableThreadBoost &&
               largePages == other.largePages &&
               maxQueueBlocks == other.maxQueueBlocks;
    }
};

class LocalPlaybackTuningRuntime final {
public:
    static LocalPlaybackTuningRuntime& Instance() noexcept {
        static LocalPlaybackTuningRuntime instance;
        return instance;
    }

    // Control plane: called when a local generation is bound and at shutdown.
    void Configure(const LocalPlaybackTuning& requested) noexcept {
        LocalPlaybackTuning tuning = requested;
        tuning.Clamp();
        if (!tuning.enabled) {
            Restore();
            enabled_.store(false, std::memory_order_release);
            dedicatedCore_.store(false, std::memory_order_release);
            disableThreadBoost_.store(false, std::memory_order_release);
            coreMask_.store(0, std::memory_order_release);
            return;
        }

        ConfigureTimerResolution(tuning.timerResolution100ns);
        ConfigureProcessPriority(tuning.processPriority);

        DWORD_PTR processAffinity = 0;
        DWORD_PTR systemAffinity = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &processAffinity, &systemAffinity) ||
            processAffinity == 0) {
            processAffinity = 1;
        }
        coreMask_.store(static_cast<std::uint64_t>(HighestAffinityBit(processAffinity)),
                        std::memory_order_release);
        dedicatedCore_.store(tuning.dedicatedCore, std::memory_order_release);
        disableThreadBoost_.store(tuning.disableThreadBoost, std::memory_order_release);
        enabled_.store(true, std::memory_order_release);
    }

    void Restore() noexcept {
        if (timerActive_) {
            unsigned long actual = 0;
            if (auto set = ResolveNtSetTimerResolution()) {
                const unsigned long restore = previousTimerResolution_ != 0
                    ? previousTimerResolution_ : 156250ul;
                (void)set(restore, 1, &actual);
            }
            timerActive_ = false;
        }
        if (priorityActive_) {
            if (previousPriorityClass_ != 0) {
                (void)SetPriorityClass(GetCurrentProcess(), previousPriorityClass_);
            }
            priorityActive_ = false;
        }
        enabled_.store(false, std::memory_order_release);
    }

    // Real-time safe. Call once when an audio thread starts or on the first
    // ASIO bufferSwitch (the driver owns that thread, so we cannot create it).
    void ApplyToCurrentThread() noexcept {
        if (!enabled_.load(std::memory_order_acquire)) return;
        const HANDLE thread = GetCurrentThread();
        if (disableThreadBoost_.load(std::memory_order_acquire)) {
            (void)SetThreadPriorityBoost(thread, TRUE);
        }
        const auto mask = coreMask_.load(std::memory_order_acquire);
        if (dedicatedCore_.load(std::memory_order_acquire) && mask != 0) {
            (void)SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(mask));
        }
    }

    // Control plane only (PcmQueue construction). Returns nullptr on any
    // failure so the caller can fall back to std::allocator unchanged.
    static void* AllocateLargePages(std::size_t bytes, std::size_t& granted) noexcept {
        granted = 0;
        if (bytes == 0) return nullptr;
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel) return nullptr;
        using GetLargePageMinimumFn = std::size_t(WINAPI*)();
        auto getMinimum = reinterpret_cast<GetLargePageMinimumFn>(
            GetProcAddress(kernel, "GetLargePageMinimum"));
        if (!getMinimum) return nullptr;
        const std::size_t page = getMinimum();
        if (page == 0 || bytes > (static_cast<std::size_t>(-1) - (page - 1))) return nullptr;
        const std::size_t rounded = ((bytes + page - 1) / page) * page;
        if (!EnableLockMemoryPrivilege()) return nullptr;
        void* base = VirtualAlloc(nullptr, rounded,
                                  MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
                                  PAGE_READWRITE);
        if (!base) return nullptr;
        granted = rounded;
        return base;
    }

    static void FreeLargePages(void* base) noexcept {
        if (base) (void)VirtualFree(base, 0, MEM_RELEASE);
    }

private:
    LocalPlaybackTuningRuntime() noexcept = default;
    ~LocalPlaybackTuningRuntime() = default;
    LocalPlaybackTuningRuntime(const LocalPlaybackTuningRuntime&) = delete;
    LocalPlaybackTuningRuntime& operator=(const LocalPlaybackTuningRuntime&) = delete;

    using NtQueryTimerResolutionFn = long(WINAPI*)(unsigned long*, unsigned long*, unsigned long*);
    using NtSetTimerResolutionFn = long(WINAPI*)(unsigned long, unsigned char, unsigned long*);

    static NtQueryTimerResolutionFn ResolveNtQueryTimerResolution() noexcept {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return nullptr;
        return reinterpret_cast<NtQueryTimerResolutionFn>(
            GetProcAddress(ntdll, "NtQueryTimerResolution"));
    }

    static NtSetTimerResolutionFn ResolveNtSetTimerResolution() noexcept {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return nullptr;
        return reinterpret_cast<NtSetTimerResolutionFn>(
            GetProcAddress(ntdll, "NtSetTimerResolution"));
    }

    static DWORD_PTR HighestAffinityBit(DWORD_PTR mask) noexcept {
        DWORD_PTR chosen = 0;
        for (unsigned index = 0; index < sizeof(DWORD_PTR) * 8u; ++index) {
            const DWORD_PTR bit = static_cast<DWORD_PTR>(1) << index;
            if (mask & bit) chosen = bit;
        }
        return chosen != 0 ? chosen : 1;
    }

    static bool EnableLockMemoryPrivilege() noexcept {
        HMODULE advapi = LoadLibraryExW(L"advapi32.dll", nullptr,
                                        LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!advapi) advapi = GetModuleHandleW(L"advapi32.dll");
        if (!advapi) return false;
        using OpenProcessTokenFn = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
        using LookupPrivilegeFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, PLUID);
        using AdjustPrivilegesFn = BOOL(WINAPI*)(HANDLE, BOOL, PTOKEN_PRIVILEGES,
                                                 DWORD, PTOKEN_PRIVILEGES, PDWORD);
        auto openToken = reinterpret_cast<OpenProcessTokenFn>(
            GetProcAddress(advapi, "OpenProcessToken"));
        auto lookup = reinterpret_cast<LookupPrivilegeFn>(
            GetProcAddress(advapi, "LookupPrivilegeValueW"));
        auto adjust = reinterpret_cast<AdjustPrivilegesFn>(
            GetProcAddress(advapi, "AdjustTokenPrivileges"));
        if (!openToken || !lookup || !adjust) return false;

        HANDLE token = nullptr;
        if (!openToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                       &token)) return false;
        TOKEN_PRIVILEGES privileges{};
        bool ok = lookup(nullptr, L"SeLockMemoryPrivilege",
                         &privileges.Privileges[0].Luid) != FALSE;
        if (ok) {
            privileges.PrivilegeCount = 1;
            privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            SetLastError(ERROR_SUCCESS);
            ok = adjust(token, FALSE, &privileges, sizeof(privileges),
                        nullptr, nullptr) != FALSE && GetLastError() == ERROR_SUCCESS;
        }
        (void)CloseHandle(token);
        return ok;
    }

    void ConfigureTimerResolution(std::uint32_t desired100ns) noexcept {
        auto set = ResolveNtSetTimerResolution();
        if (!set) return;
        if (desired100ns == 0) {
            if (timerActive_) {
                unsigned long actual = 0;
                const unsigned long restore = previousTimerResolution_ != 0
                    ? previousTimerResolution_ : 156250ul;
                (void)set(restore, 1, &actual);
                timerActive_ = false;
            }
            return;
        }
        if (!timerActive_) {
            unsigned long minimum = 0, maximum = 0, current = 0;
            if (auto query = ResolveNtQueryTimerResolution()) {
                (void)query(&minimum, &maximum, &current);
            }
            previousTimerResolution_ = current != 0 ? current : 156250ul;
        }
        unsigned long actual = 0;
        if (set(static_cast<unsigned long>(desired100ns), 1, &actual) >= 0) {
            timerActive_ = true;
        }
    }

    void ConfigureProcessPriority(bool enabled) noexcept {
        if (enabled) {
            if (!priorityActive_) {
                previousPriorityClass_ = GetPriorityClass(GetCurrentProcess());
                if (previousPriorityClass_ != 0 &&
                    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
                    priorityActive_ = true;
                }
            }
            return;
        }
        if (priorityActive_) {
            if (previousPriorityClass_ != 0) {
                (void)SetPriorityClass(GetCurrentProcess(), previousPriorityClass_);
            }
            priorityActive_ = false;
        }
    }

    std::atomic<bool> enabled_{};
    std::atomic<bool> dedicatedCore_{};
    std::atomic<bool> disableThreadBoost_{};
    std::atomic<std::uint64_t> coreMask_{};

    // Control-plane-only state, accessed from Configure()/Restore().
    bool timerActive_{};
    unsigned long previousTimerResolution_{};
    bool priorityActive_{};
    DWORD previousPriorityClass_{};
};

} // namespace ammod::audio_v2
