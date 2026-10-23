#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <utility>

namespace ammod {

// After ExitProcess has terminated the other threads, detours must not enter
// a mutex, COM, or an allocator owned by one of those threads. Trivial storage
// remains readable until the OS removes the image.
inline std::atomic<bool> processExiting{};
inline bool ProcessExiting() noexcept {
    return processExiting.load(std::memory_order_acquire);
}

// Normal owners still destroy their object. Process-exit abandonment suppresses
// the entire member-destruction chain, not merely the outer Stop() call.
template<class T>
class ProcessLifetimeOwner final {
public:
    template<class... Args>
    explicit ProcessLifetimeOwner(Args&&... args) noexcept(noexcept(T(std::forward<Args>(args)...))) {
        ::new (static_cast<void*>(storage_)) T(std::forward<Args>(args)...);
    }
    ~ProcessLifetimeOwner() { if (!abandoned_.load(std::memory_order_acquire)) Get().~T(); }
    ProcessLifetimeOwner(const ProcessLifetimeOwner&) = delete;
    ProcessLifetimeOwner& operator=(const ProcessLifetimeOwner&) = delete;
    T& Get() noexcept { return *std::launder(reinterpret_cast<T*>(storage_)); }
    const T& Get() const noexcept { return *std::launder(reinterpret_cast<const T*>(storage_)); }
    void Abandon() noexcept { abandoned_.store(true, std::memory_order_release); }

private:
    alignas(T) std::byte storage_[sizeof(T)];
    std::atomic<bool> abandoned_{};
};

} // namespace ammod
