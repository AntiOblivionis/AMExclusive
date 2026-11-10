#pragma once

#include <cstdint>
#include <string>

namespace ammod::raat {

// One Roon Ready output discovered over SOOD. address is the IPv4 literal the
// device announced; controlPort is the RAAT control (LL) port. deviceId is the
// stable identity we expose to foobar2000/broker and persist in the ini.
struct RaatDevice final {
    std::wstring deviceId;
    std::wstring uniqueId;
    std::wstring tid;
    std::wstring friendlyName;
    std::wstring address;
    std::wstring raatVersion;
    std::uint16_t controlPort{};
    bool online{};
};

// Parse "1.1.47" (or "1.1.47-..." / "v1.1.47") and compare numerically.
// Unknown/empty versions are treated as legacy by the caller.
inline bool RaatVersionAtLeast(const std::wstring& version,
                               int major, int minor, int patch) noexcept {
    int parts[3]{};
    int index = 0;
    bool sawDigit = false;
    for (wchar_t ch : version) {
        if (ch >= L'0' && ch <= L'9') {
            sawDigit = true;
            if (index < 3) {
                parts[index] = parts[index] * 10 + static_cast<int>(ch - L'0');
                if (parts[index] > 100000) parts[index] = 100000;
            }
        } else if (ch == L'.') {
            if (index < 2) ++index;
        } else if (sawDigit) {
            if (index >= 2) break;
            // A suffix (space, dash) after the numeric part ends the version.
            if (index >= 1) break;
        }
    }
    if (!sawDigit) return false;
    if (parts[0] != major) return parts[0] > major;
    if (parts[1] != minor) return parts[1] > minor;
    return parts[2] >= patch;
}

inline bool RaatVersionIsNewGen(const std::wstring& version) noexcept {
    return RaatVersionAtLeast(version, 1, 1, 47);
}

} // namespace ammod::raat
