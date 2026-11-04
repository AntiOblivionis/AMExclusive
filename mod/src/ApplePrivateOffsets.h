#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Apple-private ABI inventory for the verified Apple Music 1.6.4.90 /
// package-framework 1.1540.23042.0 x64 build. Each entry records the semantic
// role and the fingerprint that must accompany it. Public API constants live
// elsewhere; private RVAs, object fields, call-stack fingerprints and observed
// binary layouts are kept here so a package update has one review surface.
namespace ammod::apple_private {

namespace verified_build {
inline constexpr wchar_t kAppleMusicFileVersion[] = L"1.6.4.90";
inline constexpr wchar_t kPackageVersion[] = L"1.1540.23042.0";
inline constexpr wchar_t kArchitecture[] = L"x64";
} // namespace verified_build

namespace core_media_filter {
inline constexpr std::size_t kFilterClassTable = 0x18;
inline constexpr std::size_t kClassTableMethods = 0x10;
inline constexpr std::size_t kSimpleApplySlot = 0x18;
inline constexpr std::size_t kTreeApplySlot = 0x20;
inline constexpr std::size_t kSimplePredicate = 0x40;
inline constexpr std::size_t kTreeChildren = 0x30;
inline constexpr std::size_t kTreeFallback = 0x38;
inline constexpr std::size_t kStrictContext = 0x68;
// Constructor fingerprint for the verified AllowableMediaSubtypes simple filter.
// A different priority/callback shape means the private ABI has drifted.
inline constexpr std::uint32_t kStrictConstructorPriority = 0x375;
// Fingerprint: the method-table slots above must resolve to these CoreMedia RVAs.
// Each verified CoreMedia build gets one profile. The quality lock refuses to
// patch an image whose prologues do not match a profile at the recorded RVAs,
// so an Apple update degrades to "quality lock unavailable" instead of
// detouring whatever code now lives at those addresses.
struct BuildProfile final {
    const wchar_t* label;
    std::uintptr_t simpleApplyRva;
    std::uintptr_t treeApplyRva;
};
inline constexpr BuildProfile kBuildProfiles[] = {
    // Apple Music 1.6.4.90 / package 1.1540.23042.0
    {L"CoreMedia 1.2 (3310.2) / AM 1.6.4.90", 0x006CA8E0, 0x006E2F10},
    // Apple Music 1.1792.39682.0 / package 1.1792.39682.0
    // simple apply is the unique 16-byte prologue match; the tree apply was
    // identified by its own string references (ftreefilter_applyReturningInfo,
    // ftreefilter_appendAndReleaseLeafInfo) inside the verified function body.
    {L"CoreMedia 1.2 (3365.13) / AM 1.1792.39682.0", 0x007DC3A0, 0x007F4A60},
};
// Image fingerprint. Both verified CoreMedia builds share the same function
// prologues. Only SHA-256 digests of those short instruction sequences are kept
// here: the check in LosslessQualityLock.cpp hashes kPrologueBytes of the loaded
// image and compares digests. This preserves fail-closed version verification
// without embedding any fragment of Apple's machine code in the source tree.
inline constexpr std::size_t kPrologueBytes = 16;
inline constexpr std::size_t kPrologueDigestBytes = 32;
inline constexpr std::array<std::uint8_t, kPrologueDigestBytes> kSimpleApplyDigest{
    0x3D, 0x0F, 0x1C, 0x98, 0xD2, 0x19, 0x08, 0xA5,
    0x1D, 0xBA, 0xCA, 0xF9, 0xDD, 0x3F, 0xDC, 0x70,
    0x6F, 0xDC, 0x11, 0x2F, 0xC8, 0xD8, 0x33, 0xC1,
    0x15, 0x1E, 0xA5, 0x78, 0x77, 0xD5, 0x6D, 0xD3};
inline constexpr std::array<std::uint8_t, kPrologueDigestBytes> kTreeApplyDigest{
    0xC7, 0xAA, 0xC4, 0xF2, 0x21, 0x3A, 0xF2, 0xF3,
    0xA9, 0xB2, 0xC1, 0x0E, 0x76, 0x8A, 0x80, 0x4D,
    0x20, 0xE2, 0x4C, 0x60, 0x63, 0x7E, 0xDF, 0x2D,
    0xDB, 0x08, 0xE6, 0x79, 0xE7, 0x05, 0xA9, 0x20};
} // namespace core_media_filter

namespace agent_stack {
// Fingerprint for the AMPLibraryAgent.exe local-file seek path calling
// AudioConverterReset (the hook DLL's host image, resolved through
// GetModuleHandleW(nullptr)). These are image-relative return-address RVAs,
// not absolute process addresses. The third frame has two observed compiler
// paths. Each RVA sits directly after an E8 CALL in the verified build.
inline constexpr std::size_t kLocalSeekResetFrames = 3;
inline constexpr std::uintptr_t kLocalSeekResetFrame0 = 0x0040B9FA;
inline constexpr std::uintptr_t kLocalSeekResetFrame1 = 0x0082F6BC;
inline constexpr std::array<std::uintptr_t, 2> kLocalSeekResetFrame2{
    0x00B1E8AD, 0x00B1E972};
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
