#pragma once

#include <windows.h>

namespace ammod::audio {

// Stable, project-owned HRESULTs used across the audio core, Agent IPC and UI.
inline constexpr HRESULT kUnsupportedLocalInt32 =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x201);
inline constexpr HRESULT kBitPerfectFormatUnavailable =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x202);
inline constexpr HRESULT kExclusiveFormatUnavailable =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x203);
inline constexpr HRESULT kEndpointDrainTimeout =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x204);
inline constexpr HRESULT kDlnaDeviceUnavailable =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x205);
inline constexpr HRESULT kDlnaFormatUnsupported =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x206);
inline constexpr HRESULT kDlnaStreamFailed =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x207);
inline constexpr HRESULT kDlnaControlFailed =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x208);
inline constexpr HRESULT kDlnaSeekUnsupported =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x209);

constexpr bool IsPlaybackRejection(HRESULT error) noexcept {
    return error == kUnsupportedLocalInt32 || error == kBitPerfectFormatUnavailable ||
           error == kExclusiveFormatUnavailable || error == kDlnaDeviceUnavailable ||
           error == kDlnaFormatUnsupported;
}

} // namespace ammod::audio
