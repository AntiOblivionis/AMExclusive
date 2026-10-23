#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_set>

namespace ammod {

// Breadth-first navigation discovery. Callers exclude item/virtualized
// subtrees; even an unexpected host topology cannot turn this into an
// unbounded walk of the music library. Logical routing content has priority.
template <typename Node, typename Identity, typename Expand>
std::size_t WalkUiRoutes(Node root, Identity identity, Expand expand) {
    std::deque<Node> pending;
    if (root) pending.push_back(root);
    std::unordered_set<std::uintptr_t> visited;
    while (!pending.empty() && visited.size() < 512) {
        auto node = pending.front();
        pending.pop_front();
        if (!node || !visited.insert(identity(node)).second) continue;
        expand(node,
            [&pending, &visited](Node child) {
                if (!child) return true;
                if (visited.size() + pending.size() >= 512 && !pending.empty()) pending.pop_back();
                if (visited.size() + pending.size() >= 512) return false;
                pending.push_front(child);
                return true;
            },
            [&pending, &visited](Node child) {
                if (!child) return true;
                if (visited.size() + pending.size() >= 512) return false;
                pending.push_back(child);
                return true;
            });
    }
    return visited.size();
}

// One queue ticket per live window. Failed/stale callbacks may only retire
// their own ticket; closing the window permanently invalidates queued work.
class UiRefreshState {
public:
    std::uint64_t Request() {
        std::lock_guard lock(mutex_);
        if (closed_ || pending_) return 0;
        pending_ = ++serial_;
        return pending_;
    }

    bool Take(std::uint64_t ticket) {
        std::lock_guard lock(mutex_);
        if (closed_ || !ticket || pending_ != ticket) return false;
        pending_ = 0;
        return true;
    }

    void Close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        pending_ = 0;
        layoutBudget_ = 0;
    }

    void ArmLayout(std::uint64_t now) {
        std::lock_guard lock(mutex_);
        if (closed_) return;
        layoutDeadline_ = now + 750;
        layoutBudget_ = 4;
    }

    bool OnLayout(std::uint64_t now) {
        std::lock_guard lock(mutex_);
        if (closed_ || !layoutBudget_) return false;
        if (now >= layoutDeadline_) {
            layoutBudget_ = 0;
            return false;
        }
        --layoutBudget_;
        return true;
    }

    void StopLayout() {
        std::lock_guard lock(mutex_);
        layoutBudget_ = 0;
    }

private:
    std::mutex mutex_;
    std::uint64_t serial_{};
    std::uint64_t pending_{};
    std::uint64_t layoutDeadline_{};
    unsigned layoutBudget_{};
    bool closed_{};
};

} // namespace ammod
