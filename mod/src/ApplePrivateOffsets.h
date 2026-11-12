#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Apple-private ABI inventory for the supported Apple Music / package-framework
// x64 builds. Each entry records the semantic role and the fingerprint that must
// accompany it. Public API constants live elsewhere; private RVAs, object fields,
// call-stack fingerprints and observed binary layouts are kept here so a package
// update has one review surface.
namespace ammod::apple_private {

namespace verified_build {
// Supported Apple Music builds; the authoritative per-build fingerprints live in
// core_media_filter::kCoreMediaBuilds and agent_stack::kLocalSeekFingerprints.
inline constexpr wchar_t kAppleMusicFileVersion[] = L"1.6.4.90 / 1.7.0.155";
inline constexpr wchar_t kPackageVersion[] = L"1.1540.23042.0 / 1.1792.39682.0";
inline constexpr wchar_t kArchitecture[] = L"x64";
} // namespace verified_build

namespace core_media_filter {
// Object field offsets of the Fig alternate-filter classes. Shared by the
// supported CoreMedia builds below; only the apply-method RVAs move between
// releases.
inline constexpr std::size_t kFilterClassTable = 0x18;
inline constexpr std::size_t kClassTableMethods = 0x10;
inline constexpr std::size_t kSimpleApplySlot = 0x18;
inline constexpr std::size_t kTreeApplySlot = 0x20;
inline constexpr std::size_t kSimplePredicate = 0x40;
inline constexpr std::size_t kTreeChildren = 0x30;
inline constexpr std::size_t kTreeFallback = 0x38;
inline constexpr std::size_t kStrictContext = 0x68;
// Fingerprint: the first 16 bytes at each apply RVA. The quality lock refuses to
// patch kTreeApplyRva unless both sequences match the loaded image, so a
// CoreMedia update degrades to "quality lock unavailable" instead of detouring
// whatever code now lives at that address.
inline constexpr std::size_t kPrologueBytes = 16;
inline constexpr std::array<std::uint8_t, kPrologueBytes> kSimpleApplyPrologue{
    0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x48, 0x8D,
    0x68, 0xA1, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, kPrologueBytes> kTreeApplyPrologue{
    0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44,
    0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48};

// Per-build fingerprint table. The two supported generations have identical
// prologues and identical AllowableMediaSubtypes constructor priority (0x375,
// passed by FigAlternateAllowableMediaSubtypeFilterCreate); only the apply RVAs
// moved. CoreMedia 1.7 class-table layout was re-derived from the exported
// FigSimpleAlternateFilterCreate / FigAlternateFilterTreeCreate class metadata:
//
//   simple class table 0x119C5E8 -> methods 0x119C690 -> apply slot 0x18 = 0x7DC3A0
//   tree   class table 0x119C7C0 -> methods 0x119C870 -> apply slot 0x20 = 0x7F4A60
//
// Both method tables still carry the two apply pointers at the same slot offsets,
// which is what CollectStrict classifies on.
struct CoreMediaBuild final {
    wchar_t const* appleMusicFileVersion;
    wchar_t const* packageVersion;
    std::uintptr_t simpleApplyRva;
    std::uintptr_t treeApplyRva;
    std::array<std::uint8_t, kPrologueBytes> simpleApplyPrologue;
    std::array<std::uint8_t, kPrologueBytes> treeApplyPrologue;
    std::uint32_t strictConstructorPriority;
};

inline constexpr std::array<CoreMediaBuild, 2> kCoreMediaBuilds{{
    // Apple Music 1.6.4.90 / package-framework 1.1540.23042.0
    {L"1.6.4.90", L"1.1540.23042.0", 0x006CA8E0, 0x006E2F10,
     kSimpleApplyPrologue, kTreeApplyPrologue, 0x375},
    // Apple Music 1.7.0.155 / package-framework 1.1792.39682.0
    {L"1.7.0.155", L"1.1792.39682.0", 0x007DC3A0, 0x007F4A60,
     kSimpleApplyPrologue, kTreeApplyPrologue, 0x375},
}};
} // namespace core_media_filter

namespace agent_stack {
// Fingerprint for the AMPLibraryAgent.exe local-file seek path calling
// AudioConverterReset (the hook DLL's host image, resolved through
// GetModuleHandleW(nullptr)). These are image-relative return-address RVAs,
// not absolute process addresses, and each observed RVA sits directly after an
// E8 CALL. The frame-2 slot has two observed compiler paths per build.
//
// Builds: 1.6.4.90 came from the original verification. 1.7.0.155 was captured
// from a native_trace run of a local-file seek on the installed 1.7 package:
//   frame0 = return into the reset wrapper (0x435FC0, converter magic checked)
//   frame1 = return into the 'adec' decoder method that calls it (0x86680C)
//   frame2 = return into the seek handler that calls the decoder (0xB5BB68)
// The 1.7 values were observed as the recurring pair 0xB5BCA1 / 0xB5BD64.
inline constexpr std::size_t kLocalSeekResetFrames = 3;

struct LocalSeekFingerprint final {
    wchar_t const* appleMusicFileVersion;
    std::uintptr_t frame0;
    std::uintptr_t frame1;
    std::array<std::uintptr_t, 2> frame2;
};

inline constexpr std::array<LocalSeekFingerprint, 3> kLocalSeekFingerprints{{
    {L"1.6.4.90", 0x0040B9FA, 0x0082F6BC, {0x00B1E8AD, 0x00B1E972}},
    {L"1.7.0.155", 0x00435FE6, 0x008669AB, {0x00B5BCA1, 0x00B5BD64}},
    // Static 1.7.0.156 seek candidate; live seek validation is pending.
    {L"1.7.0.156", 0x00435FE6, 0x008669AB, {0x00B5BD15, 0x00B5BDD8}},
}};

inline bool MatchesLocalSeekReset(std::uintptr_t frame0, std::uintptr_t frame1,
                                  std::uintptr_t frame2) noexcept {
    for (const auto& fingerprint : kLocalSeekFingerprints) {
        if (frame0 == fingerprint.frame0 && frame1 == fingerprint.frame1 &&
            (frame2 == fingerprint.frame2[0] || frame2 == fingerprint.frame2[1])) {
            return true;
        }
    }
    return false;
}
} // namespace agent_stack

namespace audio_converter_property {
inline constexpr std::uint32_t kCursor = 0x63706563; // 'cpec'
inline constexpr std::uint32_t kCurrentInputDescription = 0x61636964; // 'acid'
inline constexpr std::uint32_t kCurrentOutputDescription = 0x61636F64; // 'acod'
} // namespace audio_converter_property

namespace alac_cookie {
// ALACSpecificConfig can appear bare, after an atom header, or after two atom
// headers. Fingerprint: supported bit depth/channels/rate sanity checks at use.
inline constexpr std::array<std::uint32_t, 3> kCandidateOffsets{0, 12, 24};
inline constexpr std::uint32_t kConfigBytes = 24;
inline constexpr std::uint32_t kBitDepth = 5;
inline constexpr std::uint32_t kChannels = 9;
inline constexpr std::uint32_t kSampleRate = 20;
} // namespace alac_cookie

} // namespace ammod::apple_private
