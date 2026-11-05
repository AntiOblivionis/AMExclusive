#include "LocalOutputDevices.h"

#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <objbase.h>

#include <algorithm>
#include <cwchar>

namespace ammod::local_devices {
namespace {

struct ComScope final {
    HRESULT result{S_OK};
    bool opened{};
    ComScope() noexcept {
        result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        opened = result == S_OK || result == S_FALSE;
    }
    ~ComScope() {
        if (opened) CoUninitialize();
    }
    bool Ok() const noexcept {
        return result == S_OK || result == S_FALSE || result == RPC_E_CHANGED_MODE;
    }
};

std::wstring ReadRegistryString(HKEY key, const wchar_t* valueName) noexcept {
    wchar_t buffer[512]{};
    DWORD bytes = sizeof(buffer);
    DWORD type = 0;
    const LONG status = RegQueryValueExW(key, valueName, nullptr, &type,
                                         reinterpret_cast<LPBYTE>(buffer), &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    buffer[(bytes / sizeof(wchar_t)) < 512 ? (bytes / sizeof(wchar_t)) : 511] = L'\0';
    return buffer;
}

void EnumerateAsioView(REGSAM view, bool usable, std::vector<Device>& out) noexcept {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
                      KEY_READ | view, &root) != ERROR_SUCCESS) {
        return;
    }
    wchar_t name[256]{};
    for (DWORD index = 0;; ++index) {
        DWORD nameBytes = static_cast<DWORD>(std::size(name));
        FILETIME unused{};
        const LONG status = RegEnumKeyExW(root, index, name, &nameBytes, nullptr,
                                          nullptr, nullptr, &unused);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;

        HKEY driverKey = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ | view, &driverKey) != ERROR_SUCCESS) {
            continue;
        }
        const std::wstring clsidText = ReadRegistryString(driverKey, L"CLSID");
        const std::wstring description = ReadRegistryString(driverKey, L"Description");
        RegCloseKey(driverKey);

        CLSID clsid{};
        if (clsidText.empty() || !ParseAsioClsid(clsidText, clsid)) continue;

        Device device;
        device.backend = Backend::Asio;
        device.id = clsidText;
        device.name = description.empty() ? std::wstring(name) : description;
        device.usable = usable;
        if (!usable) device.note = L"32-bit driver (cannot load in a 64-bit host)";
        // A 64-bit registration wins over a 32-bit one with the same CLSID.
        const bool duplicate = std::any_of(out.begin(), out.end(), [&](const Device& existing) {
            return existing.backend == Backend::Asio && existing.id == device.id;
        });
        if (!duplicate) out.push_back(std::move(device));
    }
    RegCloseKey(root);
}

} // namespace

bool ParseAsioClsid(const std::wstring& text, CLSID& clsid) noexcept {
    if (text.size() < 38) return false;
    return CLSIDFromString(text.c_str(), &clsid) == S_OK;
}

const wchar_t* BackendName(Backend backend) noexcept {
    return backend == Backend::Asio ? L"ASIO" : L"WASAPI";
}

std::vector<Device> EnumerateWasapi() noexcept {
    std::vector<Device> result;
    ComScope com;
    if (!com.Ok()) return result;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return result;

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        if (SUCCEEDED(collection->GetCount(&count))) {
            for (UINT index = 0; index < count; ++index) {
                IMMDevice* device = nullptr;
                if (FAILED(collection->Item(index, &device)) || !device) continue;
                wchar_t* id = nullptr;
                if (SUCCEEDED(device->GetId(&id)) && id) {
                    Device entry;
                    entry.backend = Backend::Wasapi;
                    entry.id = id;
                    IPropertyStore* properties = nullptr;
                    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
                        properties) {
                        PROPVARIANT value{};
                        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
                            value.vt == VT_LPWSTR && value.pwszVal) {
                            entry.name = value.pwszVal;
                            CoTaskMemFree(value.pwszVal);
                        }
                        properties->Release();
                    }
                    if (entry.name.empty()) entry.name = L"Audio endpoint " + std::to_wstring(index + 1);
                    result.push_back(std::move(entry));
                    CoTaskMemFree(id);
                }
                device->Release();
            }
        }
        collection->Release();
    }
    enumerator->Release();
    return result;
}

std::vector<Device> EnumerateAsio() noexcept {
    std::vector<Device> result;
    EnumerateAsioView(KEY_WOW64_64KEY, true, result);
    EnumerateAsioView(KEY_WOW64_32KEY, false, result);
    return result;
}

std::vector<Device> Enumerate() noexcept {
    auto result = EnumerateWasapi();
    auto asio = EnumerateAsio();
    result.insert(result.end(), std::make_move_iterator(asio.begin()),
                  std::make_move_iterator(asio.end()));
    return result;
}

} // namespace ammod::local_devices
