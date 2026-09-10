#pragma once

#include <windows.h>

namespace ammod::audio {

// Stable, project-owned HRESULTs used across the audio core, Agent IPC and UI.
inline constexpr HRESULT kUnsupportedLocalInt32 =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x201);
inline constexpr HRESULT kBitPerfectFormatUnavailable =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x202);

constexpr bool IsPlaybackRejection(HRESULT error) noexcept {
    return error == kUnsupportedLocalInt32 || error == kBitPerfectFormatUnavailable;
}

} // namespace ammod::audio
