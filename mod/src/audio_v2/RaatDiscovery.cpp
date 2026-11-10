#include "RaatDiscovery.h"

#include "RaatProtocol.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ammod::raat {
namespace {

inline constexpr std::uint16_t kSoodPort = 9003;
std::atomic<std::uint16_t> g_soodPort{kSoodPort};

std::uint16_t SoodPort() noexcept {
    const auto port = g_soodPort.load(std::memory_order_relaxed);
    return port ? port : kSoodPort;
}

// RAAT discovery group is 239.255.90.90. Note that 239.255.90.91 is the
// *bus* group (BUS_IP in the device-side roon_tcp.lua, UDP 9004) and is not
// used for SOOD at all. The address is parsed with inet_pton below so there is
// no chance of a double byte-order conversion (inet_addr form of .90 is
// 0x5A5AFFEF; htonl(0x5B5AFFEF) would send to 91.90.255.239).
inline constexpr char kRaatMulticastGroupText[] = "239.255.90.90";

inline constexpr std::uint32_t kQueryIntervalMs = 2000;
inline constexpr std::uint32_t kBurstRounds = 2;
inline constexpr std::uint32_t kBurstSpacingMs = 120;
inline constexpr std::uint32_t kReceiveTimeoutMs = 100;
inline constexpr std::uint64_t kOfflineAfterMs = 3500;
inline constexpr std::uint64_t kWakeRetryMs = 5000;

// The reference sender prefixes some queries with a service_id TLV so the
// device server can attribute the request. Devices answer a bare query too,
// but sending both variants costs nothing and matches the reference sender.
inline constexpr const char* kServiceIds[] = {
    "5e2042ad-9bc5-4508-be92-ff68f19bdc93",
    "d7634b85-8190-470f-aa51-6cb5538dc1b9",
};

std::wstring Widen(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        out.data(), length);
    return out;
}

// SOOD query: "SOOD", version 2, 'Q', then TLV records.
std::string QueryPacket(const char* serviceId) {
    std::string packet("SOOD\x02Q", 6);
    if (!serviceId || !*serviceId) return packet;
    const std::string key = "service_id";
    const std::size_t valueLength = std::strlen(serviceId);
    packet.push_back(static_cast<char>(key.size()));
    packet += key;
    packet.push_back(static_cast<char>((valueLength >> 8u) & 0xffu));
    packet.push_back(static_cast<char>(valueLength & 0xffu));
    packet += serviceId;
    return packet;
}

std::vector<std::string> QueryPackets() {
    std::vector<std::string> packets;
    packets.push_back(QueryPacket(nullptr));
    for (const char* serviceId : kServiceIds) packets.push_back(QueryPacket(serviceId));
    return packets;
}

// One send to one destination. The destination address is the only thing that
// varies, so callers build it once and reuse it.
bool SendTo(SOCKET socket, const std::string& packet, std::uint32_t address) noexcept {
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(SoodPort());
    destination.sin_addr.s_addr = address;
    return sendto(socket, packet.data(), static_cast<int>(packet.size()), 0,
                  reinterpret_cast<const sockaddr*>(&destination),
                  sizeof(destination)) >= 0;
}

// Best-effort egress address: a connected UDP socket reports the source address
// the routing table would pick. 0 means "no usable route", in which case the
// caller leaves the interface choice to the stack.
std::uint32_t DetectEgressIp() noexcept {
    for (const char* probe : {"1.1.1.1", "8.8.8.8", "223.5.5.5"}) {
        SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) continue;
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(53);
        std::uint32_t address = 0;
        if (inet_pton(AF_INET, probe, &destination.sin_addr) == 1 &&
            connect(socket, reinterpret_cast<const sockaddr*>(&destination),
                    sizeof(destination)) == 0) {
            sockaddr_in local{};
            int length = sizeof(local);
            if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
                address = local.sin_addr.s_addr;
            }
        }
        closesocket(socket);
        if (address != 0) return address;
    }
    return 0;
}

// One IPv4 interface of this machine: its address plus the directed broadcast
// of its subnet.
struct LocalInterface final {
    std::uint32_t address{};    // network order
    std::uint32_t broadcast{};  // network order, 0 when the prefix is unknown
};

// Every usable IPv4 interface, primary (default route) first. Roon endpoints are
// commonly on a second NIC - a USB adapter, a dedicated audio VLAN, a Hyper-V
// vSwitch - so pinning only the default-route interface misses them entirely.
//
// GetAdaptersAddresses is the only reliable source here: gethostname() +
// getaddrinfo() usually resolves to one address (or fails outright when the
// host name is not in DNS), which would silently hide the audio NIC.
std::vector<LocalInterface> DetectLocalInterfaces() {
    std::vector<LocalInterface> interfaces;
    const auto add = [&interfaces](std::uint32_t address, std::uint32_t mask) {
        if (address == 0) return;
        const std::uint8_t first = static_cast<std::uint8_t>(ntohl(address) >> 24u);
        if (first == 0 || first == 127 || first >= 224) return;
        for (const auto& existing : interfaces) {
            if (existing.address == address) return;
        }
        LocalInterface entry;
        entry.address = address;
        // A /31 or /32 has no usable directed broadcast.
        const std::uint32_t hostMask = ntohl(mask);
        if (hostMask != 0 && hostMask != 0xFFFFFFFFu) {
            entry.broadcast = address | ~mask;
        }
        interfaces.push_back(entry);
    };

    add(DetectEgressIp(), 0xFFFFFF00u);

    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        storage.assign(size, 0);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    if (status == NO_ERROR) {
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            // Skip adapters that cannot carry discovery traffic.
            if (adapter->OperStatus != IfOperStatusUp) continue;
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* unicast = adapter->FirstUnicastAddress; unicast;
                 unicast = unicast->Next) {
                if (!unicast->Address.lpSockaddr) continue;
                if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
                const auto* address =
                    reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                // OnLinkPrefixLength is the prefix, so the mask follows from it.
                const unsigned prefix = unicast->OnLinkPrefixLength <= 32
                    ? unicast->OnLinkPrefixLength : 24;
                const std::uint32_t mask = prefix == 0 ? 0u
                    : htonl(static_cast<std::uint32_t>(0xFFFFFFFFull << (32 - prefix)));
                add(address->sin_addr.s_addr, mask);
            }
        }
    }

    // Last resort: let the stack choose. A zero interface still reaches the
    // default route and is better than sending nothing.
    if (interfaces.empty()) interfaces.push_back(LocalInterface{});
    return interfaces;
}

std::vector<std::uint32_t> LocalAddressesOf(const std::vector<LocalInterface>& interfaces) {
    std::vector<std::uint32_t> addresses;
    addresses.reserve(interfaces.size());
    for (const auto& entry : interfaces) addresses.push_back(entry.address);
    return addresses;
}

std::wstring IpText(std::uint32_t networkOrderAddress) {
    char text[INET_ADDRSTRLEN]{};
    in_addr address{};
    address.s_addr = networkOrderAddress;
    if (!inet_ntop(AF_INET, &address, text, sizeof(text))) return L"?";
    return Widen(text);
}

// A response from one of these sources is never a remote endpoint.
bool RejectSource(std::uint32_t networkOrderAddress,
                  const std::vector<std::uint32_t>& localAddresses) noexcept {
    const std::uint32_t host = ntohl(networkOrderAddress);
    const std::uint8_t first = static_cast<std::uint8_t>(host >> 24u);
    const std::uint8_t second = static_cast<std::uint8_t>(host >> 16u);
    if (first == 0) return true;                       // 0.0.0.0/8
    if (first == 127) return true;                     // loopback
    if (first == 169 && second == 254) return true;     // link-local
    if (first >= 224) return true;                     // multicast / reserved
    for (const std::uint32_t local : localAddresses) {
        if (local != 0 && local == networkOrderAddress) return true;  // ourselves
    }
    return false;
}

bool ParseTlv(const std::uint8_t* data, std::size_t size,
              std::map<std::string, std::string>& fields) noexcept {
    if (size < 6) return false;
    if (std::memcmp(data, "SOOD", 4) != 0 || data[4] != 2 || data[5] != 'R') {
        return false;
    }
    std::size_t pos = 6;
    while (pos < size) {
        const std::uint8_t keyLength = data[pos++];
        if (pos + keyLength > size) break;
        std::string key(reinterpret_cast<const char*>(data + pos), keyLength);
        pos += keyLength;
        if (pos + 2 > size) break;
        const std::uint16_t valueLength = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data[pos]) << 8u) |
            static_cast<std::uint16_t>(data[pos + 1u]));
        pos += 2;
        // 0xFFFF is the protocol's "no value" marker: the entry carries no
        // payload and parsing continues with the next record. Treating it as a
        // 65535-byte value would swallow every field that follows it.
        if (valueLength == 0xFFFFu) {
            fields.erase(key);
            continue;
        }
        if (pos + valueLength > size) break;
        if (!key.empty()) {
            fields[std::move(key)] = std::string(
                reinterpret_cast<const char*>(data + pos), valueLength);
        }
        pos += valueLength;
    }
    return !fields.empty();
}

std::wstring Field(const std::map<std::string, std::string>& fields,
                   const char* key) {
    const auto it = fields.find(key);
    return it == fields.end() ? std::wstring{} : Widen(it->second);
}

std::uint16_t PortField(const std::map<std::string, std::string>& fields) {
    for (const char* key : {"tcp_port", "_tcp_port", "port"}) {
        const auto it = fields.find(key);
        if (it == fields.end() || it->second.empty()) continue;
        int value = 0;
        for (char ch : it->second) {
            if (ch < '0' || ch > '9') continue;
            value = value * 10 + (ch - '0');
            if (value > 65535) value = 65535;
        }
        if (value > 0) return static_cast<std::uint16_t>(value);
    }
    return 0;
}

std::wstring NameFromFields(const std::map<std::string, std::string>& fields) {
    for (const char* key : {"output_name", "vendor_model", "model", "vendor",
                            "machine_name", "machine", "host"}) {
        const auto value = Field(fields, key);
        if (!value.empty()) return value;
    }
    return L"(unnamed)";
}

std::wstring AddressOf(const sockaddr_in& address) {
    char text[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
    return Widen(text);
}

bool MakeDevice(const sockaddr_in& from,
                const std::map<std::string, std::string>& fields,
                RaatDevice& device) {
    device.address = AddressOf(from);
    device.tid = Field(fields, "_tid");
    device.uniqueId = Field(fields, "unique_id");
    device.deviceId = device.uniqueId.empty()
        ? (device.tid.empty() ? device.address + L":" +
              std::to_wstring(PortField(fields)) : device.tid)
        : device.uniqueId;
    device.friendlyName = NameFromFields(fields);
    device.raatVersion = Field(fields, "raat_version");
    device.controlPort = PortField(fields);
    // A sleeping endpoint may announce itself without tcp_port. Keep it so the
    // wake path (9200) can bring the output up, but do not publish it until a
    // RAAT control port is known - an output without a port cannot be opened.
    device.online = true;
    return !device.deviceId.empty();
}

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(GetTickCount64());
}

} // namespace

void SetSoodPort(std::uint16_t port) noexcept {
    g_soodPort.store(port ? port : kSoodPort, std::memory_order_relaxed);
}

struct RaatDiscovery::Impl final
    : std::enable_shared_from_this<RaatDiscovery::Impl> {
    Callback callback;
    LogCallback log;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> refresh{false};
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::unordered_map<std::wstring, RaatDevice> devices;
    std::unordered_map<std::wstring, std::uint64_t> lastSeen;
    std::unordered_map<std::wstring, std::uint64_t> lastWake;
    std::unordered_set<std::wstring> wakeInFlight;

    struct WakeOutcome {
        std::wstring deviceId;
        std::wstring address;
        std::uint16_t port{};
        bool ok{};
        std::wstring error;
    };
    std::vector<WakeOutcome> wakeResults;

    void LogLine(const std::wstring& text) const {
        if (log) log(text);
    }

    void Publish(const RaatDevice& device, bool force) {
        bool changed = false;
        {
            std::lock_guard lock(mutex);
            const auto it = devices.find(device.deviceId);
            if (it == devices.end()) {
                devices.emplace(device.deviceId, device);
                changed = true;
            } else {
                changed = force || it->second.online != device.online ||
                    it->second.address != device.address ||
                    it->second.controlPort != device.controlPort ||
                    it->second.friendlyName != device.friendlyName ||
                    it->second.raatVersion != device.raatVersion;
                if (changed) it->second = device;
            }
        }
        if (!changed) return;
        LogLine(std::wstring(L"raat device ") + (device.online ? L"online " : L"offline ") +
                device.friendlyName + L" id=" + device.deviceId + L" at " +
                device.address + L":" + std::to_wstring(device.controlPort) +
                L" raat=" + device.raatVersion);
        if (callback) callback(device);
    }

    // Wake runs on its own thread: the 9200 exchange can take seconds and must
    // never stall the receive loop. The result is handed back through
    // wakeResults and published by the discovery thread.
    void ScheduleWake(const RaatDevice& device, std::uint64_t now) {
        if (device.address.empty()) return;
        {
            std::lock_guard lock(mutex);
            auto& last = lastWake[device.deviceId];
            if (last != 0 && now - last < kWakeRetryMs) return;
            last = now;
            if (!wakeInFlight.insert(device.deviceId).second) return;
        }
        std::weak_ptr<Impl> weak = weak_from_this();
        const std::wstring address = device.address;
        const std::wstring deviceId = device.deviceId;
        std::thread([weak, address, deviceId] {
            std::uint16_t port = 0;
            std::wstring error;
            const bool ok = WakeDevice(address, deviceId, port, error);
            auto self = weak.lock();
            if (!self) return;
            WakeOutcome outcome;
            outcome.deviceId = deviceId;
            outcome.address = address;
            outcome.port = port;
            outcome.ok = ok;
            outcome.error = error;
            std::lock_guard lock(self->mutex);
            self->wakeInFlight.erase(deviceId);
            self->wakeResults.push_back(std::move(outcome));
        }).detach();
    }

    void DrainWakeResults() {
        std::vector<WakeOutcome> outcomes;
        {
            std::lock_guard lock(mutex);
            outcomes.swap(wakeResults);
        }
        for (auto& outcome : outcomes) {
            if (!outcome.ok || outcome.port == 0) {
                LogLine(L"raat discovery: wake of " + outcome.address + L" not confirmed (" +
                        (outcome.error.empty() ? L"no RAAT port" : outcome.error) + L")");
                continue;
            }
            RaatDevice updated;
            bool found = false;
            {
                std::lock_guard lock(mutex);
                const auto it = devices.find(outcome.deviceId);
                if (it != devices.end()) {
                    it->second.controlPort = outcome.port;
                    it->second.online = true;
                    updated = it->second;
                    found = true;
                }
            }
            if (!found) continue;
            LogLine(L"raat discovery: wake brought up " + updated.friendlyName + L" at " +
                    updated.address + L":" + std::to_wstring(updated.controlPort));
            Publish(updated, true);
        }
    }

    bool Start(Callback cb, LogCallback logger, std::wstring& error) {
        callback = std::move(cb);
        log = std::move(logger);
        stop.store(false, std::memory_order_release);
        auto self = shared_from_this();
        thread = std::thread([self] { self->ThreadMain(); });
        error.clear();
        return true;
    }

    void Stop() {
        stop.store(true, std::memory_order_release);
        refresh.store(true, std::memory_order_release);
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }

    void ThreadMain() {
        if (!EnsureWinsock()) {
            LogLine(L"raat discovery: WSAStartup failed");
            return;
        }

        const std::vector<LocalInterface> interfaces = DetectLocalInterfaces();
        const std::vector<std::uint32_t> localAddresses = LocalAddressesOf(interfaces);
        if (localAddresses.size() == 1 && localAddresses.front() == 0) {
            LogLine(L"raat discovery: no default route detected "
                    L"(multicast egress may vanish)");
        } else {
            std::wstring text = L"raat discovery: outbound iface ip =";
            for (const std::uint32_t address : localAddresses) {
                text += L" " + IpText(address);
            }
            LogLine(text);
        }

        sockaddr_in multicast{};
        multicast.sin_family = AF_INET;
        multicast.sin_port = htons(SoodPort());
        if (inet_pton(AF_INET, kRaatMulticastGroupText, &multicast.sin_addr) != 1) {
            LogLine(L"raat discovery: bad multicast group");
            return;
        }
        const std::uint32_t multicastAddress = multicast.sin_addr.s_addr;

        const BOOL yes = TRUE;
        // Joins the group on every local address so a device on a secondary NIC
        // is heard. Sends are pinned per interface in sendBurst below.
        const auto prepareSocket = [&](SOCKET socket, bool joinMulticast,
                                       const wchar_t* label) {
            setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&yes), sizeof(yes));
            setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
                       reinterpret_cast<const char*>(&yes), sizeof(yes));
            // IP_MULTICAST_LOOP stays at its default (on): a device server
            // running on this very machine must still see the query. Our own
            // echo is harmless because ParseTlv only accepts 'R' replies.
            if (joinMulticast) {
                for (const std::uint32_t address : localAddresses) {
                    ip_mreq membership{};
                    membership.imr_multiaddr.s_addr = multicastAddress;
                    membership.imr_interface.s_addr = address;
                    if (setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                   reinterpret_cast<const char*>(&membership),
                                   sizeof(membership)) == SOCKET_ERROR) {
                        // Broadcast and unicast still work without the group.
                        LogLine(std::wstring(L"raat discovery: multicast join on ") + label +
                                L" via " + IpText(address) + L" failed (" +
                                std::to_wstring(WSAGetLastError()) +
                                L"); unicast still captured");
                    }
                }
            }
        };

        // Roon devices answer SOOD to the well-known port 9003 rather than to
        // the query's source port, and they also broadcast their own
        // announcements there. Without a passive listener on 9003 a real
        // endpoint is silently missed, so if the port is taken we fall back to
        // an ephemeral port instead of giving up.
        SOCKET passive = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (passive != INVALID_SOCKET) {
            prepareSocket(passive, true, L"passive");
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = INADDR_ANY;
            local.sin_port = htons(SoodPort());
            if (bind(passive, reinterpret_cast<const sockaddr*>(&local),
                     sizeof(local)) == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                sockaddr_in fallback{};
                fallback.sin_family = AF_INET;
                fallback.sin_addr.s_addr = INADDR_ANY;
                fallback.sin_port = 0;
                if (bind(passive, reinterpret_cast<const sockaddr*>(&fallback),
                         sizeof(fallback)) == SOCKET_ERROR) {
                    LogLine(L"raat discovery: no passive listener at all (" +
                            std::to_wstring(WSAGetLastError()) + L")");
                    closesocket(passive);
                    passive = INVALID_SOCKET;
                } else {
                    LogLine(L"raat discovery: passive port 9003 busy (" +
                            std::to_wstring(error) +
                            L"); ephemeral fallback bound");
                }
            } else {
                LogLine(L"raat discovery: passive listener bound :9003");
            }
        }

        SOCKET query = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (query != INVALID_SOCKET) {
            prepareSocket(query, true, L"query");
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = INADDR_ANY;
            local.sin_port = 0;
            if (bind(query, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
                LogLine(L"raat discovery: cannot bind query socket: " +
                        std::to_wstring(WSAGetLastError()));
                closesocket(query);
                query = INVALID_SOCKET;
            }
        }
        if (query == INVALID_SOCKET && passive == INVALID_SOCKET) {
            LogLine(L"raat discovery: no usable SOOD socket");
            return;
        }

        const std::vector<std::string> packets = QueryPackets();

        // One burst per interface: pin the multicast egress to that interface,
        // then send the group query, the limited broadcast and the interface's
        // own directed broadcast. The directed broadcast is what actually
        // reaches a device on a second NIC: Windows routes 255.255.255.255
        // through the default-route interface only, so without it a dedicated
        // audio NIC or a Hyper-V vSwitch endpoint is never queried.
        const auto sendBurst = [&](SOCKET socket) {
            bool sent = false;
            for (const LocalInterface& interface : interfaces) {
                if (interface.address != 0) {
                    ip_mreq egressOption{};
                    egressOption.imr_multiaddr.s_addr = multicastAddress;
                    egressOption.imr_interface.s_addr = interface.address;
                    (void)setsockopt(socket, IPPROTO_IP, IP_MULTICAST_IF,
                                     reinterpret_cast<const char*>(&egressOption),
                                     sizeof(egressOption));
                }
                for (const auto& packet : packets) {
                    sent = SendTo(socket, packet, multicastAddress) || sent;
                    sent = SendTo(socket, packet, INADDR_BROADCAST) || sent;
                    if (interface.broadcast != 0) {
                        sent = SendTo(socket, packet, interface.broadcast) || sent;
                    }
                }
            }
            return sent;
        };

        const auto handleResponse = [&](SOCKET socket, std::uint64_t now) {
            std::uint8_t buffer[2048]{};
            sockaddr_in from{};
            int fromLength = sizeof(from);
            const int received = recvfrom(socket, reinterpret_cast<char*>(buffer),
                                          sizeof(buffer), 0,
                                          reinterpret_cast<sockaddr*>(&from),
                                          &fromLength);
            if (received <= 0) return;
            if (RejectSource(from.sin_addr.s_addr, localAddresses)) return;
            std::map<std::string, std::string> fields;
            if (!ParseTlv(buffer, static_cast<std::size_t>(received), fields)) return;
            RaatDevice device;
            if (!MakeDevice(from, fields, device)) return;
            {
                std::lock_guard lock(mutex);
                lastSeen[device.deviceId] = now;
            }
            if (device.controlPort != 0) {
                Publish(device, false);
            } else {
                // Announced but not serving yet: ask the signal band (9200) to
                // bring the output up; Publish happens once a port is known.
                LogLine(L"raat discovery: " + device.friendlyName + L" on " + device.address +
                        L" has no RAAT tcp_port yet - asking 9200 to enable it");
                {
                    std::lock_guard lock(mutex);
                    const auto it = devices.find(device.deviceId);
                    if (it == devices.end()) devices.emplace(device.deviceId, device);
                    else it->second = device;
                }
                ScheduleWake(device, now);
            }
        };

        std::uint64_t lastQuery = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const auto now = NowMs();
            if (refresh.exchange(false, std::memory_order_acq_rel) ||
                now - lastQuery >= kQueryIntervalMs) {
                lastQuery = now;
                bool sent = false;
                for (std::uint32_t round = 0; round < kBurstRounds; ++round) {
                    if (query != INVALID_SOCKET) sent = sendBurst(query) || sent;
                    if (passive != INVALID_SOCKET) sent = sendBurst(passive) || sent;
                    if (round + 1 < kBurstRounds) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(kBurstSpacingMs));
                    }
                }
                if (!sent) {
                    LogLine(L"raat discovery: query send failed " +
                            std::to_wstring(WSAGetLastError()));
                }
            }

            fd_set readSet;
            FD_ZERO(&readSet);
            if (query != INVALID_SOCKET) FD_SET(query, &readSet);
            if (passive != INVALID_SOCKET) FD_SET(passive, &readSet);
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = kReceiveTimeoutMs * 1000;
            const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
            if (ready > 0) {
                if (query != INVALID_SOCKET && FD_ISSET(query, &readSet)) {
                    handleResponse(query, now);
                }
                if (passive != INVALID_SOCKET && FD_ISSET(passive, &readSet)) {
                    handleResponse(passive, now);
                }
            }

            DrainWakeResults();

            // Expire devices that stopped answering the query bursts.
            std::vector<RaatDevice> expired;
            {
                std::lock_guard lock(mutex);
                for (auto& [id, device] : devices) {
                    const auto seen = lastSeen.find(id);
                    if (!device.online) continue;
                    if (seen == lastSeen.end() || now - seen->second > kOfflineAfterMs) {
                        device.online = false;
                        if (device.controlPort != 0) expired.push_back(device);
                    }
                }
            }
            for (const auto& device : expired) Publish(device, true);
        }

        if (query != INVALID_SOCKET) closesocket(query);
        if (passive != INVALID_SOCKET) closesocket(passive);
    }
};

RaatDiscovery::RaatDiscovery() : impl_(std::make_shared<Impl>()) {}
RaatDiscovery::~RaatDiscovery() { Stop(); }

bool RaatDiscovery::Start(Callback callback, LogCallback log,
                          std::wstring& error) noexcept {
    return impl_->Start(std::move(callback), std::move(log), error);
}

void RaatDiscovery::Stop() noexcept {
    if (impl_) impl_->Stop();
}

void RaatDiscovery::Refresh() noexcept {
    if (!impl_) return;
    impl_->refresh.store(true, std::memory_order_release);
    impl_->wake.notify_all();
}

std::vector<RaatDevice> RaatDiscovery::Snapshot() const {
    std::vector<RaatDevice> out;
    if (!impl_) return out;
    std::lock_guard lock(impl_->mutex);
    out.reserve(impl_->devices.size());
    for (const auto& [_, device] : impl_->devices) {
        // Outputs without a RAAT control port cannot be opened; keep them out
        // of the list until the wake path has produced a port.
        if (device.controlPort == 0) continue;
        out.push_back(device);
    }
    return out;
}

bool RaatDiscovery::QueryOnce(const std::wstring& deviceId,
                              std::uint32_t timeoutMs,
                              RaatDevice& device,
                              std::wstring& error,
                              const std::wstring& unicastAddress) noexcept {
    error.clear();
    std::uint32_t unicastTarget = 0;
    if (!unicastAddress.empty()) {
        inet_pton(AF_INET, ToUtf8(unicastAddress).c_str(), &unicastTarget);
    }
    if (!EnsureWinsock()) {
        error = L"WSAStartup failed";
        return false;
    }

    sockaddr_in multicast{};
    multicast.sin_family = AF_INET;
    multicast.sin_port = htons(SoodPort());
    if (inet_pton(AF_INET, kRaatMulticastGroupText, &multicast.sin_addr) != 1) {
        error = L"bad multicast group";
        return false;
    }
    const std::uint32_t multicastAddress = multicast.sin_addr.s_addr;

    const std::vector<LocalInterface> interfaces = DetectLocalInterfaces();
    const std::vector<std::uint32_t> localAddresses = LocalAddressesOf(interfaces);

    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        error = L"socket failed " + std::to_wstring(WSAGetLastError());
        return false;
    }
    const BOOL yes = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));
    setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&yes), sizeof(yes));
    for (const std::uint32_t address : localAddresses) {
        ip_mreq membership{};
        membership.imr_multiaddr.s_addr = multicastAddress;
        membership.imr_interface.s_addr = address;
        (void)setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         reinterpret_cast<const char*>(&membership),
                         sizeof(membership));
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(SoodPort());
    if (bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        // Another controller owns 9003; fall back to an ephemeral source port.
        sockaddr_in fallback{};
        fallback.sin_family = AF_INET;
        fallback.sin_addr.s_addr = INADDR_ANY;
        fallback.sin_port = 0;
        if (bind(socket, reinterpret_cast<const sockaddr*>(&fallback),
                 sizeof(fallback)) == SOCKET_ERROR) {
            error = L"SOOD bind failed " + std::to_wstring(WSAGetLastError());
            closesocket(socket);
            return false;
        }
    }

    // A captured foobar2000 "scan for Roon Ready devices" burst declares a
    // controller identity in the SOOD query. Without _tid / _replyaddr /
    // _replyport the renderer answers discovery but silently refuses to
    // install the Lua dispatcher, so every later LL request returns
    // {"status":"NotImplemented"} even though load_script says Success.
    // Query packet (142 bytes) = SOOD\x02Q + service_id + _tid(UUID) +
    // _replyaddr(local IP) + _replyport(this socket).
    std::uint16_t replyPort = SoodPort();
    {
        sockaddr_in bound{};
        int length = sizeof(bound);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
            replyPort = ntohs(bound.sin_port);
        }
    }
    std::string replyAddr;
    {
        const std::uint32_t egress = DetectEgressIp();
        char text[INET_ADDRSTRLEN]{};
        in_addr address{};
        address.s_addr = egress;
        if (egress != 0 && inet_ntop(AF_INET, &address, text, sizeof(text))) {
            replyAddr = text;
        }
    }
    if (replyAddr.empty()) replyAddr = "127.0.0.1";
    const auto addTlv = [](std::string packet, const char* key,
                           const std::string& value) {
        packet.push_back(static_cast<char>(std::strlen(key)));
        packet += key;
        packet.push_back(static_cast<char>((value.size() >> 8u) & 0xffu));
        packet.push_back(static_cast<char>(value.size() & 0xffu));
        packet += value;
        return packet;
    };
    std::vector<std::string> packets;
    for (const char* serviceId : kServiceIds) {
        std::string packet("SOOD\x02Q", 6);
        packet = addTlv(std::move(packet), "service_id", serviceId);
        std::string tid = ToUtf8(NewUuidString());
        for (char& c : tid) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        packet = addTlv(std::move(packet), "_tid", tid);
        packet = addTlv(std::move(packet), "_replyaddr", replyAddr);
        packet = addTlv(std::move(packet), "_replyport", std::to_string(replyPort));
        packets.push_back(std::move(packet));
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    std::uint64_t lastQuery = 0;
    bool found = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = NowMs();
        if (now - lastQuery >= kBurstSpacingMs * 2) {
            lastQuery = now;
            for (const LocalInterface& interface : interfaces) {
                if (interface.address != 0) {
                    ip_mreq egressOption{};
                    egressOption.imr_multiaddr.s_addr = multicastAddress;
                    egressOption.imr_interface.s_addr = interface.address;
                    (void)setsockopt(socket, IPPROTO_IP, IP_MULTICAST_IF,
                                     reinterpret_cast<const char*>(&egressOption),
                                     sizeof(egressOption));
                }
                for (const auto& packet : packets) {
                    SendTo(socket, packet, multicastAddress);
                    SendTo(socket, packet, INADDR_BROADCAST);
                    if (interface.broadcast != 0) {
                        SendTo(socket, packet, interface.broadcast);
                    }
                    // The scan that makes the renderer register a controller
                    // is unicast to the device itself (captured from
                    // foobar2000), not just multicast/broadcast.
                    if (unicastTarget != 0) {
                        SendTo(socket, packet, unicastTarget);
                    }
                }
            }
        }
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket, &readSet);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = kReceiveTimeoutMs * 1000;
        const int ready = select(0, &readSet, nullptr, nullptr, &tv);
        if (ready <= 0 || !FD_ISSET(socket, &readSet)) continue;
        std::uint8_t buffer[2048]{};
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int received = recvfrom(socket, reinterpret_cast<char*>(buffer),
                                      sizeof(buffer), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received <= 0) continue;
        if (RejectSource(from.sin_addr.s_addr, localAddresses)) continue;
        std::map<std::string, std::string> fields;
        if (!ParseTlv(buffer, static_cast<std::size_t>(received), fields)) continue;
        RaatDevice candidate;
        if (!MakeDevice(from, fields, candidate)) continue;
        if (deviceId.empty() || candidate.deviceId == deviceId ||
            candidate.uniqueId == deviceId || candidate.tid == deviceId) {
            device = candidate;
            found = true;
            break;
        }
    }
    closesocket(socket);
    if (!found && error.empty()) error = L"SOOD query timed out";
    return found;
}

namespace {

std::string AppendTlv(const std::string& packet, const char* key, const std::string& value) {
    std::string out = packet;
    out.push_back(static_cast<char>(std::strlen(key)));
    out += key;
    out.push_back(static_cast<char>((value.size() >> 8u) & 0xffu));
    out.push_back(static_cast<char>(value.size() & 0xffu));
    out += value;
    return out;
}

std::string AppendEmptyTlv(const std::string& packet, const char* key) {
    std::string out = packet;
    out.push_back(static_cast<char>(std::strlen(key)));
    out += key;
    out.push_back(static_cast<char>(0xff));
    out.push_back(static_cast<char>(0xff));
    return out;
}

} // namespace

int DiscoveryWireSelfTest() {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* what) {
        if (!condition) {
            std::printf("sood-check FAIL: %s\n", what);
            ++failures;
        }
    };

    // 1. The discovery group must be 239.255.90.90 in network order. The old
    //    constant produced 91.90.255.239 and killed multicast entirely.
    sockaddr_in group{};
    check(inet_pton(AF_INET, kRaatMulticastGroupText, &group.sin_addr) == 1,
          "group parses");
    check(group.sin_addr.s_addr == 0x5A5AFFEFu, "group is 239.255.90.90");
    check(ntohl(group.sin_addr.s_addr) >> 24 == 239, "group first octet 239");

    // 2. Query packets: bare plus one per service_id, each well formed.
    const std::vector<std::string> packets = QueryPackets();
    check(!packets.empty(), "query packets exist");
    check(packets.front() == std::string("SOOD\x02Q", 6), "bare query is 6 bytes");
    for (const auto& packet : packets) {
        check(packet.size() >= 6 && std::memcmp(packet.data(), "SOOD", 4) == 0 &&
              packet[4] == 2 && packet[5] == 'Q', "query header");
    }
    check(packets.size() == 1 + sizeof(kServiceIds) / sizeof(kServiceIds[0]),
          "one query per service id");

    // 3. Plain reply.
    {
        std::string reply("SOOD\x02R", 6);
        reply = AppendTlv(reply, "_tid", "5e2042ad-9bc5-4508-be92-ff68f19bdc93");
        reply = AppendTlv(reply, "unique_id", "dev-1");
        reply = AppendTlv(reply, "tcp_port", "9100");
        reply = AppendTlv(reply, "raat_version", "1.1.47");
        reply = AppendTlv(reply, "output_name", "Living Room");
        std::map<std::string, std::string> fields;
        check(ParseTlv(reinterpret_cast<const std::uint8_t*>(reply.data()),
                       reply.size(), fields), "plain reply parses");
        check(fields.size() == 5, "plain reply field count");
        check(PortField(fields) == 9100, "plain reply tcp_port");
        check(NameFromFields(fields) == L"Living Room", "plain reply name");
    }

    // 4. A 0xFFFF value length is the "no value" marker; it must not swallow
    //    the fields that follow it (this is what hid tcp_port on real gear).
    {
        std::string reply("SOOD\x02R", 6);
        reply = AppendTlv(reply, "unique_id", "dev-2");
        reply = AppendTlv(reply, "stale", "x");
        reply = AppendEmptyTlv(reply, "stale");
        reply = AppendTlv(reply, "tcp_port", "9200");
        std::map<std::string, std::string> fields;
        check(ParseTlv(reinterpret_cast<const std::uint8_t*>(reply.data()),
                       reply.size(), fields), "0xFFFF reply parses");
        check(PortField(fields) == 9200, "0xFFFF keeps later fields");
        check(fields.count("stale") == 0, "0xFFFF clears the key");
    }

    // 5. Truncated tail: keep what was complete, do not mis-parse the rest.
    {
        std::string reply("SOOD\x02R", 6);
        reply = AppendTlv(reply, "unique_id", "dev-3");
        reply = AppendTlv(reply, "tcp_port", "9300");
        reply.push_back(4);
        reply += "name";
        reply.push_back(0);
        reply.push_back(0x40);
        reply += "short";
        std::map<std::string, std::string> fields;
        check(ParseTlv(reinterpret_cast<const std::uint8_t*>(reply.data()),
                       reply.size(), fields), "truncated reply parses");
        check(PortField(fields) == 9300, "truncated reply keeps earlier fields");
    }

    // 6. Only version 2 'R' replies are accepted.
    {
        std::map<std::string, std::string> fields;
        std::string query = AppendTlv("SOOD\x02Q", "x", "y");
        std::string v3 = AppendTlv("SOOD\x03R", "x", "y");
        const std::string junk = "HTTP/1.1 200 OK";
        check(!ParseTlv(reinterpret_cast<const std::uint8_t*>(query.data()),
                        query.size(), fields), "query is not a reply");
        check(!ParseTlv(reinterpret_cast<const std::uint8_t*>(v3.data()),
                        v3.size(), fields), "version 3 rejected");
        check(!ParseTlv(reinterpret_cast<const std::uint8_t*>(junk.data()),
                        junk.size(), fields), "non-SOOD rejected");
    }

    // 7. Source filter: our own packets, loopback, link-local and multicast
    //    sources never become devices.
    {
        // Network order, as sockaddr_in stores it.
        const std::vector<std::uint32_t> local = {
            htonl(0xC0A80809u),  // 192.168.8.9
            htonl(0x0A000001u),  // 10.0.0.1
        };
        struct Case { const char* ip; bool reject; } cases[] = {
            {"192.168.8.50", false}, {"127.0.0.1", true}, {"169.254.3.4", true},
            {"0.0.0.0", true}, {"224.0.0.251", true}, {"239.255.90.90", true},
            {"192.168.8.9", true}, {"10.0.0.1", true},
        };
        for (const auto& item : cases) {
            sockaddr_in address{};
            if (inet_pton(AF_INET, item.ip, &address.sin_addr) != 1) {
                check(false, "filter address parses");
                continue;
            }
            check(RejectSource(address.sin_addr.s_addr, local) == item.reject,
                  item.ip);
        }
    }

    if (failures == 0) std::printf("sood-check ok\n");
    return failures == 0 ? 0 : 8;
}

} // namespace ammod::raat
