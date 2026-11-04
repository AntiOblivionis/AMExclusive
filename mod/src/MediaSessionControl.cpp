#include "BoundedLog.h"

#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

using namespace winrt;
using namespace Windows::Media::Control;

namespace {

void LogRecovery(const std::string& message) {
    static std::mutex logMutex;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    const auto logPath = std::filesystem::path(path).parent_path() / L"am-exclusive-recovery.log";
    std::wstring line = L"uptimeMs=" + std::to_wstring(GetTickCount64()) +
                        L" pid=" + std::to_wstring(GetCurrentProcessId()) + L" ";
    line.append(message.begin(), message.end());
    ammod::logging::AppendWideLine(logPath, logMutex, line, 1ull * 1024ull * 1024ull, 2);
}

struct MediaIdentity {
    std::wstring title;
    std::wstring artist;
    std::wstring albumTitle;
    std::int32_t trackNumber{};
    std::int64_t timelineEnd100ns{};
};

MediaIdentity ReadMediaIdentity(const GlobalSystemMediaTransportControlsSession& session) {
    const auto media = session.TryGetMediaPropertiesAsync().get();
    const auto timeline = session.GetTimelineProperties();
    return {
        media.Title().c_str(),
        media.Artist().c_str(),
        media.AlbumTitle().c_str(),
        media.TrackNumber(),
        timeline.EndTime().count(),
    };
}

bool HasConfirmedItemChange(const MediaIdentity& before, const MediaIdentity& current) {
    if (!before.title.empty() && current.title.empty()) return false;
    if (!before.title.empty() && current.title != before.title) return true;
    if (!before.artist.empty() && !current.artist.empty() && current.artist != before.artist) return true;
    if (!before.albumTitle.empty() && !current.albumTitle.empty() && current.albumTitle != before.albumTitle) return true;
    if (current.trackNumber != 0 && before.trackNumber != 0 && current.trackNumber != before.trackNumber) return true;
    return current.timelineEnd100ns > 0 && before.timelineEnd100ns > 0 &&
           current.timelineEnd100ns != before.timelineEnd100ns;
}

bool HasReturnedToItem(const MediaIdentity& expected, const MediaIdentity& current) {
    if (!expected.title.empty()) return current.title == expected.title;
    if (!expected.artist.empty() && current.artist != expected.artist) return false;
    if (!expected.albumTitle.empty() && current.albumTitle != expected.albumTitle) return false;
    if (expected.trackNumber != 0 && current.trackNumber != expected.trackNumber) return false;
    if (expected.timelineEnd100ns > 0 && current.timelineEnd100ns != expected.timelineEnd100ns) return false;
    return !expected.artist.empty() || !expected.albumTitle.empty() ||
           expected.trackNumber != 0 || expected.timelineEnd100ns > 0;
}

struct RoundTripResult {
    bool primaryAccepted{};
    bool itemChangeConfirmed{};
    std::uint32_t itemChangeWaitMs{};
    MediaIdentity visited{};
    bool compensatingAccepted{};
    bool returned{};
    std::uint32_t returnWaitMs{};
};

bool RestartCurrentItem(const GlobalSystemMediaTransportControlsSession& session,
                        const MediaIdentity& original,
                        RoundTripResult& result) {
    result = {};
    result.primaryAccepted = session.TrySkipPreviousAsync().get();
    if (!result.primaryAccepted) return false;

    constexpr std::uint32_t pollMs = 50;
    constexpr std::uint32_t timeoutMs = 6000;
    while (result.itemChangeWaitMs < timeoutMs) {
        Sleep(pollMs);
        result.itemChangeWaitMs += pollMs;
        result.visited = ReadMediaIdentity(session);
        if (HasConfirmedItemChange(original, result.visited)) {
            result.itemChangeConfirmed = true;
            break;
        }
    }
    if (!result.itemChangeConfirmed) return false;

    result.compensatingAccepted = session.TrySkipNextAsync().get();
    if (!result.compensatingAccepted) return false;

    while (result.returnWaitMs < timeoutMs) {
        Sleep(pollMs);
        result.returnWaitMs += pollMs;
        if (HasReturnedToItem(original, ReadMediaIdentity(session))) {
            result.returned = true;
            break;
        }
    }
    return result.returned;
}

} // namespace

namespace {

// The DLNA sink needs two facts about the track that Apple Music is playing:
// how long it is (the served WAV has to declare a length before its first byte
// leaves) and when it changes (a new track needs a new URL, because the WAV
// header carries the sample rate). Both live in SMTC, but reading them costs a
// WinRT round trip, so this process reads them once and streams the result on
// stdout instead of letting the audio path poll.
//
// Wire format, one line per refresh:
//   TRACK<TAB>durationMs<TAB>title
// A line is emitted when the timeline end changes and every few seconds after
// that, so the reader can tell "no news" apart from "the helper died".
constexpr std::uint32_t kPollMs = 800;
constexpr std::uint64_t kRefreshMs = 3000;

void WriteUtf8Line(const std::wstring& text) {
    if (text.empty()) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                          static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        utf8.data(), bytes, nullptr, nullptr);
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int WatchTracks() {
    init_apartment(apartment_type::multi_threaded);
    auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    GlobalSystemMediaTransportControlsSession session{nullptr};
    std::int64_t lastEnd100ns = -1;
    std::wstring title;
    std::uint64_t lastPrint = 0;
    for (;;) {
        try {
            if (!session) {
                for (const auto& candidate : manager.GetSessions()) {
                    const std::wstring source = candidate.SourceAppUserModelId().c_str();
                    if (source.find(L"AppleMusic") != std::wstring::npos) {
                        session = candidate;
                        break;
                    }
                }
            }
            if (session) {
                const auto timeline = session.GetTimelineProperties();
                const auto end100ns =
                    (timeline.EndTime() - timeline.StartTime()).count();
                const auto now = GetTickCount64();
                if (end100ns != lastEnd100ns) {
                    // A different timeline end means a different item: fetch the
                    // metadata once here rather than on every poll.
                    const auto media = session.TryGetMediaPropertiesAsync().get();
                    title = media.Title().c_str();
                    lastEnd100ns = end100ns;
                    WriteUtf8Line(L"TRACK\t" +
                                  std::to_wstring(end100ns > 0 ? end100ns / 10000 : 0) +
                                  L"\t" + title);
                    lastPrint = now;
                } else if (now - lastPrint >= kRefreshMs) {
                    WriteUtf8Line(L"TRACK\t" +
                                  std::to_wstring(end100ns > 0 ? end100ns / 10000 : 0) +
                                  L"\t" + title);
                    lastPrint = now;
                }
            }
        } catch (...) {
            // Apple Music closing, or the session going stale, surfaces as a
            // COM failure. Drop it and look again; the reader simply keeps the
            // last value until a new line arrives.
            session = nullptr;
            lastEnd100ns = -1;
        }
        Sleep(kPollMs);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && _wcsicmp(argv[1], L"watch") == 0) {
        return WatchTracks();
    }
    if (argc != 2 || (_wcsicmp(argv[1], L"restart-current") != 0 &&
                      _wcsicmp(argv[1], L"reject-current") != 0)) return 2;

    init_apartment(apartment_type::multi_threaded);
    const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    GlobalSystemMediaTransportControlsSession appleSession{nullptr};
    for (const auto& session : manager.GetSessions()) {
        const std::wstring source = session.SourceAppUserModelId().c_str();
        if (source.find(L"AppleMusic") != std::wstring::npos) {
            appleSession = session;
            break;
        }
    }
    if (!appleSession) return 3;

    if (_wcsicmp(argv[1], L"reject-current") == 0) {
        const bool stopAccepted = appleSession.TryStopAsync().get();
        bool stopConfirmed = false;
        if (stopAccepted) {
            constexpr std::uint32_t pollMs = 25;
            constexpr std::uint32_t timeoutMs = 750;
            for (std::uint32_t waited = 0; waited <= timeoutMs; waited += pollMs) {
                if (appleSession.GetPlaybackInfo().PlaybackStatus() ==
                    GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped) {
                    stopConfirmed = true;
                    break;
                }
                Sleep(pollMs);
            }
        }

        // Stop is the strongest public GSMTC teardown for the current media
        // session. If Apple refuses it, fail closed with pause + rewind while
        // AME has already retired its own captured/load state.
        bool paused = stopConfirmed;
        bool reset = stopConfirmed;
        if (!stopConfirmed) {
            paused = appleSession.TryPauseAsync().get();
            const auto timeline = appleSession.GetTimelineProperties();
            reset = appleSession.TryChangePlaybackPositionAsync(
                timeline.StartTime().count()).get();
        }
        LogRecovery("reject-current stopAccepted=" + std::to_string(stopAccepted) +
                    " stopConfirmed=" + std::to_string(stopConfirmed) +
                    " paused=" + std::to_string(paused) +
                    " reset=" + std::to_string(reset));
        return (stopConfirmed || (paused && reset)) ? 0 : 5;
    }

    const MediaIdentity original = ReadMediaIdentity(appleSession);
    const auto controls = appleSession.GetPlaybackInfo().Controls();
    if (!controls.IsPreviousEnabled()) return 4;

    RoundTripResult result{};
    LogRecovery("begin strategy=previous-confirm-next originalEnd100ns=" +
                std::to_string(original.timelineEnd100ns));
    const bool succeeded = RestartCurrentItem(appleSession, original, result);
    LogRecovery("complete primaryAccepted=" + std::to_string(result.primaryAccepted) +
                " changed=" + std::to_string(result.itemChangeConfirmed) +
                " compensatingAccepted=" + std::to_string(result.compensatingAccepted) +
                " returned=" + std::to_string(result.returned));
    return succeeded ? 0 : 5;
}
