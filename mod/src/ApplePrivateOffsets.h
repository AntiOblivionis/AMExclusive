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
inline constexpr std::uintptr_t kSimpleApplyRva = 0x006CA8E0;
inline constexpr std::uintptr_t kTreeApplyRva = 0x006E2F10;
// Image fingerprint: the first 16 bytes at each RVA in the verified
// CoreMedia.dll (file version 1.2 (3310.2), shipped in Apple Music 1.6.4.90).
// The quality lock refuses to patch kTreeApplyRva unless both sequences match
// the loaded image, so a CoreMedia update degrades to "quality lock
// unavailable" instead of detouring whatever code now lives at that address.
inline constexpr std::size_t kPrologueBytes = 16;
inline constexpr std::array<std::uint8_t, kPrologueBytes> kSimpleApplyPrologue{
    0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x48, 0x8D,
    0x68, 0xA1, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00};
inline constexpr std::array<std::uint8_t, kPrologueBytes> kTreeApplyPrologue{
    0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44,
    0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48};
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
