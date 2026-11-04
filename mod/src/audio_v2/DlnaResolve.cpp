#include "DlnaResolve.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace ammod::audio_v2 {
namespace {

bool EnsureWinsock() noexcept {
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ready;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), bytes, nullptr, nullptr);
    return result;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring result(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), chars);
    return result;
}

// Case-insensitive lookup of a single SSDP header value. The response is a
// short CRLF text block; no full parser is needed here.
std::string HeaderValue(const std::string& response, const char* name) {
    const std::size_t nameLength = std::strlen(name);
    std::size_t pos = 0;
    while (pos < response.size()) {
        std::size_t lineEnd = response.find("\r\n", pos);
        if (lineEnd == std::string::npos) lineEnd = response.size();
        if (lineEnd > pos + nameLength &&
            _strnicmp(response.data() + pos, name, nameLength) == 0 &&
            response[pos + nameLength] == ':') {
            std::size_t valueStart = pos + nameLength + 1;
            while (valueStart < lineEnd &&
                   (response[valueStart] == ' ' || response[valueStart] == '\t')) {
                ++valueStart;
            }
            std::size_t valueEnd = lineEnd;
            while (valueEnd > valueStart &&
                   (response[valueEnd - 1] == '\r' || response[valueEnd - 1] == ' ' ||
                    response[valueEnd - 1] == '\t')) {
                --valueEnd;
            }
            return response.substr(valueStart, valueEnd - valueStart);
        }
        pos = lineEnd + 2;
    }
    return {};
}

// USN is "uuid:<device>[:[:]<service>]"; only the UUID identifies the device.
std::wstring UdnFromUsn(const std::wstring& usn) {
    const auto begin = usn.find(L"uuid:");
    if (begin == std::wstring::npos) return {};
    const auto end = usn.find(L"::", begin);
    return usn.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
}

// Every usable IPv4 interface. Sending one M-SEARCH from the default route is
// not enough: on a multi-homed host the multicast leaves through the wrong
// adapter and the renderer never answers. The broker solves this the same way.
std::vector<std::wstring> LocalIPv4Addresses() {
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
                unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* address =
                reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            if (address->sin_addr.S_un.S_un_b.s_b1 == 169 &&
                address->sin_addr.S_un.S_un_b.s_b2 == 254) {
                continue;
            }
            char text[64]{};
            if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) {
                addresses.push_back(Widen(std::string(text, std::strlen(text))));
            }
        }
    }
    return addresses;
}

} // namespace

bool ResolveLocationByUdn(const std::wstring& udn, std::uint32_t timeoutMs,
                          std::wstring& location) noexcept {
    if (udn.empty() || timeoutMs == 0) return false;
    if (!EnsureWinsock()) return false;

    const auto interfaces = LocalIPv4Addresses();
    if (interfaces.empty()) return false;

    // Split the budget across interfaces so a multi-homed host still answers in
    // bounded time.
    const auto perInterface =
        std::max<std::uint32_t>(400u, timeoutMs / static_cast<std::uint32_t>(interfaces.size()));

    // Ask for MediaRenderer explicitly and for everything as a fallback: some
    // OpenHome stacks only answer the broader search.
    const char* const searches[] = {
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n",
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: ssdp:all\r\n\r\n",
    };

    std::string buffer(4096, '\0');
    for (const auto& local : interfaces) {
        const SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) continue;

        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_port = 0;
        const std::string localText = Narrow(local);
        if (InetPtonA(AF_INET, localText.c_str(), &bindAddress.sin_addr) != 1 ||
            bind(sock, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) ==
                SOCKET_ERROR) {
            closesocket(sock);
            continue;
        }
        // Pin the outgoing multicast to this adapter, exactly like the broker's
        // discovery worker does.
        (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF,
                         reinterpret_cast<const char*>(&bindAddress.sin_addr),
                         sizeof(bindAddress.sin_addr));
        DWORD ttl = 2;
        (void)setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL,
                         reinterpret_cast<const char*>(&ttl), sizeof(ttl));

        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(1900);
        InetPtonA(AF_INET, "239.255.255.250", &destination.sin_addr);
        for (const char* request : searches) {
            for (int attempt = 0; attempt < 2; ++attempt) {
                (void)sendto(sock, request, static_cast<int>(std::strlen(request)), 0,
                             reinterpret_cast<const sockaddr*>(&destination),
                             sizeof(destination));
            }
        }

        bool found = false;
        const ULONGLONG deadline = GetTickCount64() + perInterface;
        while (!found) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            const ULONGLONG remaining = deadline - now;

            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            timeval wait{};
            wait.tv_sec = static_cast<long>(remaining / 1000);
            wait.tv_usec = static_cast<long>((remaining % 1000) * 1000);
            if (select(0, &readSet, nullptr, nullptr, &wait) <= 0) break;

            sockaddr_in from{};
            int fromLength = static_cast<int>(sizeof(from));
            const int received = recvfrom(
                sock, buffer.data(), static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (received <= 0) continue;

            const std::string response(buffer.data(), static_cast<std::size_t>(received));
            const std::wstring usn = Widen(HeaderValue(response, "usn"));
            if (usn.empty()) continue;
            const std::wstring candidate = UdnFromUsn(usn);
            if (candidate.empty() || _wcsicmp(candidate.c_str(), udn.c_str()) != 0) continue;

            const std::wstring resolved = Widen(HeaderValue(response, "location"));
            if (resolved.empty()) continue;
            location = resolved;
            found = true;
        }

        closesocket(sock);
        if (found) return true;
    }
    return false;
}

} // namespace ammod::audio_v2
