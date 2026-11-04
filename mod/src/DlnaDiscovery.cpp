#include <winsock2.h>
#include <ws2tcpip.h>

#include "DlnaDiscovery.h"
#include "UpnpXml.h"

#include <windows.h>
#include <iphlpapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ammod::dlna {
namespace {

const wchar_t kSsdpAddress[] = L"239.255.255.250";
constexpr std::uint16_t kSsdpPort = 1900;
constexpr ULONGLONG kOfflineAfterMs = 90u * 1000u;

struct HttpText final {
    DWORD status{};
    std::wstring body;
};

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                          static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        result.data(), bytes, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring result(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), chars);
    return result;
}

bool HttpGetText(const std::wstring& url, HttpText& result) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) return false;
    std::wstring host(parts.lpszHostName ? parts.lpszHostName : L"", parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath ? parts.lpszUrlPath : L"", parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength && parts.lpszExtraInfo) {
        path += std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";
    HINTERNET session = WinHttpOpen(L"AMExclusiveDiscovery/0.1",
                                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 1500, 1500, 3000, 5000);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort ? parts.nPort : 80, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                                     WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     parts.nScheme == INTERNET_SCHEME_HTTPS
                                                         ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    bool ok = request != nullptr && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                                       nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            ok = false;
        } else {
            result.status = status;
        }
    }
    std::string bytes;
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || !available) break;
        if (bytes.size() + available > 256u * 1024u) {
            ok = false;
            break;
        }
        const auto offset = bytes.size();
        bytes.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, bytes.data() + offset, available, &read)) {
            ok = false;
            break;
        }
        bytes.resize(offset + read);
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (ok) result.body = FromUtf8(bytes);
    return ok;
}

std::map<std::wstring, std::wstring> ParseSsdpHeaders(const std::string& response) {
    std::map<std::wstring, std::wstring> headers;
    std::size_t start = response.find("\r\n");
    if (start == std::string::npos) return headers;
    start += 2;
    while (start < response.size()) {
        const auto end = response.find("\r\n", start);
        if (end == std::string::npos) break;
        const std::string line = response.substr(start, end - start);
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::wstring name = FromUtf8(line.substr(0, colon));
            std::wstring value = FromUtf8(line.substr(colon + 1));
            std::transform(name.begin(), name.end(), name.begin(),
                           [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
            while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
            while (!value.empty() && std::iswspace(value.back())) value.pop_back();
            headers[name] = value;
        }
        start = end + 2;
    }
    return headers;
}

std::vector<std::wstring> LocalAddresses() {
    std::vector<std::wstring> addresses;
    ULONG size = 16u * 1024u;
    std::vector<BYTE> buffer(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    if (status != NO_ERROR) return addresses;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* address = reinterpret_cast<const sockaddr_in*>(
                unicast->Address.lpSockaddr);
            if (address->sin_addr.S_un.S_un_b.s_b1 == 169 &&
                address->sin_addr.S_un.S_un_b.s_b2 == 254) continue;
            char text[64]{};
            if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) {
                addresses.emplace_back(text, text + std::strlen(text));
            }
        }
    }
    return addresses;
}

std::wstring Trim(std::wstring value) {
    while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && std::iswspace(value.back())) value.pop_back();
    return value;
}

std::wstring UdnFromUsn(const std::wstring& usn) {
    if (usn.empty()) return {};
    const auto separator = usn.find(L"::");
    return separator == std::wstring::npos ? usn : usn.substr(0, separator);
}

} // namespace

DlnaDiscovery::~DlnaDiscovery() {
    Stop();
}

bool DlnaDiscovery::Start(Callback callback, LogCallback log, std::wstring& error) noexcept {
    if (thread_.joinable()) return true;
    if (!callback) {
        error = L"discovery callback is empty";
        return false;
    }
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        error = L"Winsock initialization failed";
        return false;
    }
    callback_ = std::move(callback);
    log_ = std::move(log);
    stop_.store(false, std::memory_order_release);
    thread_ = std::thread(&DlnaDiscovery::ThreadMain, this);
    return true;
}

void DlnaDiscovery::LogLine(const std::wstring& text) const {
    if (log_) log_(L"dlna " + text);
}

void DlnaDiscovery::Stop() noexcept {
    stop_.store(true, std::memory_order_release);
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DlnaDiscovery::Refresh() noexcept {
    refresh_.store(true, std::memory_order_release);
    wake_.notify_all();
}

std::vector<DiscoveredDevice> DlnaDiscovery::Snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<DiscoveredDevice> result;
    result.reserve(devices_.size());
    for (const auto& entry : devices_) result.push_back(entry.second);
    return result;
}

void DlnaDiscovery::ThreadMain() noexcept {
    LogLine(L"ssdp discovery thread started");
    for (;;) {
        ScanOnce();
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, std::chrono::seconds(30), [&] {
            return stop_.load(std::memory_order_acquire) ||
                   refresh_.load(std::memory_order_acquire);
        });
        const bool stopping = stop_.load(std::memory_order_acquire);
        refresh_.store(false, std::memory_order_release);
        if (stopping) break;
    }
}

void DlnaDiscovery::ScanOnce() noexcept {
    const auto addresses = LocalAddresses();
    if (addresses.empty()) {
        LogLine(L"no usable IPv4 adapter");
        return;
    }
    std::wstring addressText;
    for (const auto& address : addresses) {
        if (!addressText.empty()) addressText += L",";
        addressText += address;
    }
    LogLine(L"scan interfaces=" + addressText);

    const std::array<const wchar_t*, 3> searchTypes{
        L"upnp:rootdevice",
        L"urn:schemas-upnp-org:device:MediaRenderer:1",
        L"urn:av-openhome-org:device:OpenHomeMediaDevice:1",
    };
    std::set<std::wstring> seenLocations;
    std::set<std::wstring> seenUdns;
    unsigned responseCount = 0;
    unsigned rendererCount = 0;

    for (const auto& localAddress : addresses) {
        const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) {
            LogLine(L"ssdp socket failed iface=" + localAddress);
            continue;
        }
        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_port = 0;
        const std::string local = Narrow(localAddress);
        inet_pton(AF_INET, local.c_str(), &bindAddress.sin_addr);
        if (bind(socket, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) ==
            SOCKET_ERROR) {
            LogLine(L"ssdp bind failed iface=" + localAddress);
            closesocket(socket);
            continue;
        }
        (void)setsockopt(socket, IPPROTO_IP, IP_MULTICAST_IF,
                         reinterpret_cast<const char*>(&bindAddress.sin_addr),
                         sizeof(bindAddress.sin_addr));
        DWORD ttl = 2;
        (void)setsockopt(socket, IPPROTO_IP, IP_MULTICAST_TTL,
                         reinterpret_cast<const char*>(&ttl), sizeof(ttl));

        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(kSsdpPort);
        inet_pton(AF_INET, "239.255.255.250", &target.sin_addr);
        for (const auto* type : searchTypes) {
            const std::wstring search =
                L"M-SEARCH * HTTP/1.1\r\n"
                L"HOST: 239.255.255.250:1900\r\n"
                L"MAN: \"ssdp:discover\"\r\n"
                L"MX: 3\r\n"
                L"ST: " + std::wstring(type) + L"\r\n\r\n";
            const std::string searchBytes = Narrow(search);
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                sendto(socket, searchBytes.c_str(), static_cast<int>(searchBytes.size()), 0,
                       reinterpret_cast<const sockaddr*>(&target), sizeof(target));
            }
            LogLine(L"ssdp search iface=" + localAddress + L" st=" + type);
        }

        const auto deadline = GetTickCount64() + 4500;
        while (GetTickCount64() < deadline) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(socket, &readSet);
            timeval timeout{};
            timeout.tv_usec = 200000;
            const int selected = select(
                static_cast<int>(static_cast<std::intptr_t>(socket) + 1),
                &readSet, nullptr, nullptr, &timeout);
            if (selected <= 0) continue;
            char buffer[4096]{};
            sockaddr_in from{};
            int fromSize = sizeof(from);
            const int received = recvfrom(socket, buffer, sizeof(buffer), 0,
                                          reinterpret_cast<sockaddr*>(&from), &fromSize);
            if (received <= 0) continue;
            ++responseCount;
            const auto headers = ParseSsdpHeaders(
                std::string(buffer, static_cast<std::size_t>(received)));
            const auto location = headers.find(L"location");
            const auto st = headers.find(L"st");
            const auto usn = headers.find(L"usn");
            char fromText[64]{};
            inet_ntop(AF_INET, &from.sin_addr, fromText, sizeof(fromText));
            std::wstring responseText =
                L"ssdp response from=" + std::wstring(fromText, fromText + std::strlen(fromText)) +
                L" st=" + (st == headers.end() ? L"" : st->second) +
                L" usn=" + (usn == headers.end() ? L"" : usn->second) +
                L" location=" + (location == headers.end() ? L"" : location->second);
            LogLine(responseText);
            if (location == headers.end() || location->second.empty()) {
                LogLine(L"ssdp response dropped: no LOCATION");
                continue;
            }
            const std::wstring udn = usn == headers.end()
                ? std::wstring{} : UdnFromUsn(usn->second);
            if (!seenLocations.insert(location->second).second) continue;
            DiscoveredDevice device;
            device.location = location->second;
            device.udn = udn;
            if (!FetchDescription(device.location, device)) continue;
            if (!device.openHome && !device.avTransport) {
                LogLine(L"description dropped: no AVTransport/Playlist device=" +
                        device.friendlyName + L" udn=" + device.udn);
                continue;
            }
            if (!device.udn.empty() && !seenUdns.insert(device.udn).second) continue;
            ++rendererCount;
            DiscoveredDevice published;
            bool publish = false;
            {
                std::lock_guard lock(mutex_);
                auto& cached = devices_[device.udn];
                const bool changed = cached.friendlyName != device.friendlyName ||
                    cached.location != device.location ||
                    cached.openHome != device.openHome ||
                    cached.avTransport != device.avTransport ||
                    !cached.online;
                device.online = true;
                cached = device;
                lastSeenTick_[device.udn] = GetTickCount64();
                if (changed) {
                    published = device;
                    publish = true;
                }
            }
            // Never invoke the broker callback while holding discovery state:
            // it broadcasts to UI threads that may immediately ask for a snapshot.
            if (publish) Publish(published);
        }
        closesocket(socket);
    }
    // SSDP multicast is unreliable on multi-homed or firewalled hosts: a
    // renderer can be perfectly reachable over unicast HTTP yet never answer
    // M-SEARCH (or its answers never arrive), which made reachable devices
    // show as offline. For every known-but-offline device, probe its
    // description directly and revive it when reachable, so manually added /
    // configured targets stay online without depending on multicast.
    {
        std::vector<DiscoveredDevice> offline;
        {
            std::lock_guard lock(mutex_);
            for (const auto& entry : devices_) {
                if (!entry.second.online) offline.push_back(entry.second);
            }
        }
        for (const auto& candidate : offline) {
            DiscoveredDevice probed;
            probed.location = candidate.location;
            probed.udn = candidate.udn;
            if (!FetchDescription(probed.location, probed)) continue;
            if (!probed.openHome && !probed.avTransport) continue;
            const std::wstring key = probed.udn.empty() ? candidate.udn : probed.udn;
            DiscoveredDevice published;
            bool publish = false;
            {
                std::lock_guard lock(mutex_);
                auto& cached = devices_[key];
                const bool changed = !cached.online ||
                    cached.friendlyName != probed.friendlyName ||
                    cached.location != probed.location ||
                    cached.openHome != probed.openHome ||
                    cached.avTransport != probed.avTransport;
                probed.online = true;
                cached = probed;
                lastSeenTick_[cached.udn] = GetTickCount64();
                ++rendererCount;
                if (changed) {
                    published = cached;
                    publish = true;
                }
            }
            LogLine(L"direct probe revived device=" + probed.friendlyName +
                    L" location=" + probed.location);
            if (publish) Publish(published);
        }
    }
    LogLine(L"scan complete responses=" + std::to_wstring(responseCount) +
            L" renderers=" + std::to_wstring(rendererCount));

    const auto now = GetTickCount64();
    std::vector<DiscoveredDevice> lost;
    {
        std::lock_guard lock(mutex_);
        for (auto& entry : lastSeenTick_) {
            if (now - entry.second > kOfflineAfterMs) {
                auto found = devices_.find(entry.first);
                if (found != devices_.end() && found->second.online) {
                    found->second.online = false;
                    lost.push_back(found->second);
                }
            }
        }
    }
    for (const auto& device : lost) Publish(device);
}

bool DlnaDiscovery::FetchDescription(const std::wstring& location,
                                     DiscoveredDevice& device) noexcept {
    HttpText result;
    if (!HttpGetText(location, result)) {
        LogLine(L"description GET failed location=" + location);
        return false;
    }
    if (result.status != 200 && result.status != 0) {
        LogLine(L"description GET status=" + std::to_wstring(result.status) +
                L" location=" + location);
        return false;
    }
    upnp::DeviceDescription description;
    if (!upnp::ParseDeviceDescription(result.body, location, description)) {
        LogLine(L"description parse failed location=" + location +
                L" bytes=" + std::to_wstring(result.body.size()));
        return false;
    }
    if (!description.udn.empty()) device.udn = description.udn;
    if (device.udn.empty()) {
        LogLine(L"description has no UDN location=" + location);
        return false;
    }
    device.friendlyName = description.friendlyName.empty()
        ? device.udn : description.friendlyName;
    for (const auto& service : description.services) {
        if (service.serviceType == L"urn:av-openhome-org:service:Playlist:1") {
            device.openHome = true;
        }
        if (service.serviceType == L"urn:schemas-upnp-org:service:AVTransport:1") {
            device.avTransport = true;
        }
    }
    return true;
}

bool DlnaDiscovery::AddManualDevice(const std::wstring& input, std::wstring& error) noexcept {
    std::wstring text = Trim(input);
    if (text.empty()) {
        error = L"manual device address is empty";
        return false;
    }
    if (text.find(L"://") == std::wstring::npos) text = L"http://" + text;
    const auto scheme = text.find(L"://");
    const auto path = text.find(L'/', scheme + 3);
    const bool rootOnly = path == std::wstring::npos || text.substr(path).size() <= 1;
    std::vector<std::wstring> candidates;
    if (rootOnly) {
        std::wstring base = text;
        while (!base.empty() && base.back() == L'/') base.pop_back();
        candidates.push_back(base + L"/device.xml");
        candidates.push_back(base + L"/description.xml");
    } else {
        candidates.push_back(text);
    }

    DiscoveredDevice device;
    std::wstring lastError = L"cannot fetch device description";
    bool added = false;
    for (const auto& location : candidates) {
        DiscoveredDevice candidate;
        candidate.location = location;
        if (!FetchDescription(location, candidate)) continue;
        if (!candidate.openHome && !candidate.avTransport) {
            lastError = L"description is not a MediaRenderer/OpenHome device";
            continue;
        }
        candidate.online = true;
        device = std::move(candidate);
        added = true;
        break;
    }
    if (!added) {
        error = lastError;
        LogLine(L"manual add failed input=" + input + L" reason=" + error);
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        devices_[device.udn] = device;
        lastSeenTick_[device.udn] = GetTickCount64();
    }
    LogLine(L"manual device added name=" + device.friendlyName +
            L" udn=" + device.udn + L" location=" + device.location);
    Publish(device);
    return true;
}

void DlnaDiscovery::Publish(const DiscoveredDevice& device) noexcept {
    // callback_ is installed before the worker starts and never mutated after.
    if (callback_) callback_(device);
}

} // namespace ammod::dlna
