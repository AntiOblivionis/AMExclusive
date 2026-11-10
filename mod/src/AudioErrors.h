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
inline constexpr HRESULT kRaatDeviceUnavailable =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x20A);
inline constexpr HRESULT kRaatFormatUnsupported =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x20B);
inline constexpr HRESULT kRaatStreamFailed =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x20C);
inline constexpr HRESULT kRaatControlFailed =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x20D);
inline constexpr HRESULT kRaatScriptRejected =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x20E);
inline constexpr HRESULT kAsioDeviceReset =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x20F);

constexpr bool IsPlaybackRejection(HRESULT error) noexcept {
    return error == kUnsupportedLocalInt32 || error == kBitPerfectFormatUnavailable ||
           error == kExclusiveFormatUnavailable || error == kDlnaDeviceUnavailable ||
           error == kDlnaFormatUnsupported || error == kRaatDeviceUnavailable ||
           error == kRaatFormatUnsupported;
}

} // namespace ammod::audio
