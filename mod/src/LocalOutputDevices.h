#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ammod::local_devices {

enum class Backend : std::uint8_t {
    Wasapi = 0,
    Asio = 1,
};

struct Device final {
    Backend backend{Backend::Wasapi};
    // WASAPI: IMMDevice id. ASIO: the driver's CLSID in "{....}" form.
    std::wstring id;
    std::wstring name;
    // False for a 32-bit-only ASIO driver, which a 64-bit host cannot load.
    bool usable{true};
    std::wstring note;
};

// Enumerates every active eRender endpoint and every registered ASIO driver.
// Safe to call repeatedly and from any thread; COM is initialised locally and
// torn down only when this function opened the apartment itself.
std::vector<Device> Enumerate() noexcept;

std::vector<Device> EnumerateWasapi() noexcept;
std::vector<Device> EnumerateAsio() noexcept;

// Parses a "{XXXXXXXX-XXXX-....}" string into a CLSID. Returns false when the
// text is not a valid GUID or the driver is not registered for this bitness.
bool ParseAsioClsid(const std::wstring& text, CLSID& clsid) noexcept;

// Human-readable helper for the UI: the WASAPI endpoint id is very long, so the
// list shows the friendly name and this returns the id only for persistence.
const wchar_t* BackendName(Backend backend) noexcept;

} // namespace ammod::local_devices
