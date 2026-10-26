#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace ammod::ui {

enum class PlaybackState { Unknown, Paused, Playing };

// PropertyChanged can arrive between the two property assignments. Only a
// coherent snapshot may change playback intent; buffering/stopped is not Play.
constexpr PlaybackState ClassifyPlaybackState(bool playing, bool paused) noexcept {
    if (playing == paused) return PlaybackState::Unknown;
    return playing ? PlaybackState::Playing : PlaybackState::Paused;
}

struct PlaybackSnapshot {
    bool playing{};
    std::uint64_t queueItem{};
};

inline std::optional<PlaybackSnapshot> ParsePlaybackSnapshot(std::wstring_view action) noexcept {
    PlaybackSnapshot snapshot;
    if (action.starts_with(L"playback_playing:")) {
        snapshot.playing = true;
        action.remove_prefix(std::wstring_view{L"playback_playing:"}.size());
    } else if (action.starts_with(L"playback_paused:")) {
        action.remove_prefix(std::wstring_view{L"playback_paused:"}.size());
    } else return std::nullopt;
    if (action.empty()) return std::nullopt;
    for (const auto character : action) {
        if (character < L'0' || character > L'9') return std::nullopt;
        const auto digit = static_cast<unsigned>(character - L'0');
        if (snapshot.queueItem > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return std::nullopt;
        snapshot.queueItem = snapshot.queueItem * 10 + digit;
    }
    return snapshot;
}

} // namespace ammod::ui
