#include "RaatProtocol.h"

#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace ammod::raat {
namespace {

std::uint32_t ReadBe32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24u) |
           (static_cast<std::uint32_t>(data[1]) << 16u) |
           (static_cast<std::uint32_t>(data[2]) << 8u) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint16_t ReadBe16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8u) |
        static_cast<std::uint16_t>(data[1]));
}

void WriteBe32(std::uint8_t* data, std::uint32_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 24u);
    data[1] = static_cast<std::uint8_t>(value >> 16u);
    data[2] = static_cast<std::uint8_t>(value >> 8u);
    data[3] = static_cast<std::uint8_t>(value);
}

bool WaitWritable(SOCKET socket, std::uint32_t timeoutMs) noexcept {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeoutMs / 1000u);
    tv.tv_usec = static_cast<long>((timeoutMs % 1000u) * 1000u);
    return select(0, nullptr, &set, nullptr, &tv) > 0;
}

bool WaitReadable(SOCKET socket, std::uint32_t timeoutMs) noexcept {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeoutMs / 1000u);
    tv.tv_usec = static_cast<long>((timeoutMs % 1000u) * 1000u);
    return select(0, &set, nullptr, nullptr, &tv) > 0;
}

std::string ValueAfterKey(const std::string& json, const std::string& key,
                          std::size_t& valueStart) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != ':') return {};
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    valueStart = pos;
    return json.substr(pos);
}

} // namespace

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        out.data(), length);
    return out;
}

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + value.size() / 8u + 8u);
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (ch < 0x20) {
                char buffer[8]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
    }
    return out;
}

bool JsonFindString(const std::string& json, const std::string& key,
                    std::string& out) {
    std::size_t start = 0;
    const auto rest = ValueAfterKey(json, key, start);
    if (rest.empty() || rest[0] != '"') return false;
    out.clear();
    for (std::size_t i = 1; i < rest.size(); ++i) {
        const char ch = rest[i];
        if (ch == '"') return true;
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (i + 1 >= rest.size()) return false;
        const char escape = rest[++i];
        switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            // Keep BMP escapes as UTF-8. Surrogate pairs are rare in the
            // fields we consume; leave the low surrogate to the caller.
            if (i + 4 >= rest.size()) return false;
            unsigned codepoint = 0;
            for (int digit = 0; digit < 4; ++digit) {
                const char hex = rest[++i];
                codepoint <<= 4u;
                if (hex >= '0' && hex <= '9') codepoint += static_cast<unsigned>(hex - '0');
                else if (hex >= 'a' && hex <= 'f') codepoint += static_cast<unsigned>(hex - 'a' + 10);
                else if (hex >= 'A' && hex <= 'F') codepoint += static_cast<unsigned>(hex - 'A' + 10);
                else return false;
            }
            if (codepoint < 0x80) {
                out.push_back(static_cast<char>(codepoint));
            } else if (codepoint < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (codepoint >> 6u)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3Fu)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (codepoint >> 12u)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6u) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3Fu)));
            }
            break;
        }
        default: out.push_back(escape); break;
        }
    }
    return false;
}

std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string out;
    return JsonFindString(json, key, out) ? out : std::string{};
}

bool JsonFindInt(const std::string& json, const std::string& key,
                 std::int64_t& out) {
    std::size_t start = 0;
    const auto rest = ValueAfterKey(json, key, start);
    if (rest.empty()) return false;
    std::size_t pos = 0;
    while (pos < rest.size() && (rest[pos] == ' ' || rest[pos] == '\t')) ++pos;
    bool negative = false;
    if (pos < rest.size() && rest[pos] == '-') {
        negative = true;
        ++pos;
    }
    if (pos >= rest.size() || rest[pos] < '0' || rest[pos] > '9') return false;
    std::int64_t value = 0;
    while (pos < rest.size() && rest[pos] >= '0' && rest[pos] <= '9') {
        value = value * 10 + (rest[pos] - '0');
        if (value > 0x7fffffff) break;
        ++pos;
    }
    out = negative ? -value : value;
    return true;
}

std::int64_t JsonGetInt(const std::string& json, const std::string& key,
                        std::int64_t fallback) {
    std::int64_t out = fallback;
    return JsonFindInt(json, key, out) ? out : fallback;
}

std::uint64_t NowMicroseconds() noexcept {
    using Clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
}

std::uint64_t NowNanoseconds() noexcept {
    using Clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

bool EnsureWinsock() noexcept {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static bool initialized = false;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID parameter, PVOID*) -> BOOL {
        WSADATA data{};
        *static_cast<bool*>(parameter) = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        return TRUE;
    }, &initialized, nullptr);
    return initialized;
}

std::wstring NewUuidString() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        wchar_t fallback[64]{};
        std::swprintf(fallback, std::size(fallback), L"%08lx-%08lx",
                      static_cast<unsigned long>(GetTickCount64()),
                      static_cast<unsigned long>(GetCurrentProcessId()));
        return fallback;
    }
    wchar_t text[40]{};
    std::swprintf(text, std::size(text),
                  L"%08lX-%04X-%04X-%04X-%012llX",
                  static_cast<unsigned long>(guid.Data1),
                  static_cast<unsigned>(guid.Data2),
                  static_cast<unsigned>(guid.Data3),
                  static_cast<unsigned>(guid.Data4[0]) * 256u +
                      static_cast<unsigned>(guid.Data4[1]),
                  static_cast<unsigned long long>(guid.Data4[2]) << 32u |
                      static_cast<unsigned long long>(guid.Data4[3]) << 24u |
                      static_cast<unsigned long long>(guid.Data4[4]) << 16u |
                      static_cast<unsigned long long>(guid.Data4[5]) << 8u |
                      static_cast<unsigned long long>(guid.Data4[6]));
    return text;
}

TcpConnection::~TcpConnection() { Close(); }

std::wstring TcpConnection::LocalAddress() const noexcept {
    if (socket_ == INVALID_SOCKET) return {};
    sockaddr_in local{};
    int length = sizeof(local);
    if (getsockname(socket_, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
        return {};
    }
    char text[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) == nullptr) return {};
    return ToWide(text);
}

void TcpConnection::Close() noexcept {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

bool TcpConnection::Connect(const std::wstring& address, std::uint16_t port,
                            std::uint32_t timeoutMs, std::wstring& error) noexcept {
    Close();
    if (!EnsureWinsock()) {
        error = L"WSAStartup failed";
        return false;
    }
    const std::string text = ToUtf8(address);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (inet_pton(AF_INET, text.c_str(), &endpoint.sin_addr) != 1) {
        error = L"invalid IPv4 address " + address;
        return false;
    }
    socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
        error = L"socket failed " + std::to_wstring(WSAGetLastError());
        return false;
    }
    u_long nonBlocking = 1;
    (void)ioctlsocket(socket_, FIONBIO, &nonBlocking);
    const int connected = ::connect(socket_,
                                    reinterpret_cast<const sockaddr*>(&endpoint),
                                    sizeof(endpoint));
    if (connected == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        if (code != WSAEWOULDBLOCK) {
            error = L"connect failed " + std::to_wstring(code);
            Close();
            return false;
        }
        if (!WaitWritable(socket_, timeoutMs)) {
            error = L"connect timed out";
            Close();
            return false;
        }
        int socketError = 0;
        int length = sizeof(socketError);
        if (getsockopt(socket_, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socketError), &length) != 0 ||
            socketError != 0) {
            error = L"connect failed " + std::to_wstring(socketError);
            Close();
            return false;
        }
    }
    nonBlocking = 0;
    (void)ioctlsocket(socket_, FIONBIO, &nonBlocking);
    return true;
}

bool TcpConnection::SendAll(const void* data, std::size_t size,
                            std::uint32_t timeoutMs) noexcept {
    if (socket_ == INVALID_SOCKET) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (sent < size) {
        if (!WaitWritable(socket_, 50)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            continue;
        }
        const int chunk = send(socket_, reinterpret_cast<const char*>(bytes + sent),
                               static_cast<int>(std::min<std::size_t>(
                                   size - sent, 65536)), 0);
        if (chunk <= 0) {
            const int code = WSAGetLastError();
            if (code == WSAEWOULDBLOCK) continue;
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
}

bool TcpConnection::ReadSome(void* data, std::size_t capacity,
                             std::size_t& received, std::uint32_t timeoutMs) noexcept {
    received = 0;
    if (socket_ == INVALID_SOCKET) return false;
    if (!WaitReadable(socket_, timeoutMs)) return false;
    const int chunk = recv(socket_, static_cast<char*>(data),
                           static_cast<int>(capacity), 0);
    if (chunk <= 0) return false;
    received = static_cast<std::size_t>(chunk);
    return true;
}

bool ControlChannel::Connect(const std::wstring& address, std::uint16_t port,
                             std::uint32_t timeoutMs, std::wstring& error) noexcept {
    pending_.clear();
    nextRid_ = 1;
    if (!connection_.Connect(address, port, timeoutMs, error)) {
        address_.clear();
        port_ = 0;
        return false;
    }
    address_ = address;
    port_ = port;
    // Every captured session opens with the "hi" frame. The reference sender
    // sends it before echo/info; without it the firmware never installs the
    // dispatcher that answers later requests.
    std::wstring helloError;
    (void)SendHello(helloError);
    return true;
}

bool ControlChannel::SendHello(std::wstring& error) noexcept {
    std::uint8_t frame[16]{};
    WriteBe32(frame, 16u);
    WriteBe32(frame + 4, kLlTypeHello);
    WriteBe32(frame + 8, 2000u);
    WriteBe32(frame + 12, 10000u);
    if (!connection_.SendAll(frame, sizeof(frame), 1500u)) {
        error = L"hello send failed";
        return false;
    }
    // The device answers with a short non-LL ack (6 zero bytes on the VEGA) and
    // then, every ~2 s, with an 8-byte type-3 event. Keep whatever is available
    // in pending_; ReadFrame() skips both while looking for a real reply.
    std::uint8_t ack[64]{};
    std::size_t received = 0;
    if (connection_.ReadSome(ack, sizeof(ack), received, 300u) && received > 0) {
        pending_.insert(pending_.end(), ack, ack + received);
    }
    return true;
}

bool ControlChannel::WaitForReady(std::uint32_t timeoutMs,
                                  std::wstring& error) noexcept {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    for (;;) {
        while (pending_.size() >= 8) {
            const std::uint32_t total = ReadBe32(pending_.data());
            if (total < 8 || total > 8u * 1024u * 1024u) {
                // Six zero bytes of hi-ack, or a desync: slide one byte and
                // resynchronise instead of declaring a bad frame.
                pending_.erase(pending_.begin());
                continue;
            }
            if (pending_.size() < total) break;
            const std::uint32_t type = ReadBe32(pending_.data() + 4) & 0x7fffffffu;
            pending_.erase(pending_.begin(), pending_.begin() + total);
            if (type == 3) {
                // Anything left is device-side event traffic, never a reply
                // to a request: do not let it poison ReadFrame.
                pending_.clear();
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            pending_.clear();
            error = L"device ready event timed out";
            return false;
        }
        std::uint8_t buffer[1024]{};
        std::size_t received = 0;
        if (!connection_.ReadSome(buffer, sizeof(buffer), received, 100u)) {
            if (!connection_.IsOpen()) {
                pending_.clear();
                error = L"control connection closed";
                return false;
            }
            continue;
        }
        pending_.insert(pending_.end(), buffer, buffer + received);
    }
}

bool ControlChannel::SendProbe(const std::string& json,
                               std::uint32_t timeoutMs) noexcept {
    return SendFrame(nextRid_++, json, timeoutMs);
}

void ControlChannel::Close() noexcept {
    connection_.Close();
    pending_.clear();
}

namespace {

// Extracts the value of a top-level `"request":"..."` field. Cheap and
// allocation-free so it is safe to call on every outbound frame.
std::string RequestNameOf(const std::string& json) noexcept {
    const auto key = json.find("\"request\"");
    if (key == std::string::npos) return {};
    auto colon = json.find(':', key);
    if (colon == std::string::npos) return {};
    auto quote = json.find('"', colon);
    if (quote == std::string::npos) return {};
    const auto end = json.find('"', quote + 1);
    if (end == std::string::npos) return {};
    return json.substr(quote + 1, end - quote - 1);
}

}  // namespace

bool IsInternalRequest(const std::string& json) noexcept {
    // Requests the renderer's C layer implements itself. Everything else is
    // owned by the injected Lua script and must go out without the high bit.
    static const char* const kInternal[] = {
        "load_script", "get_log", "set_log_level", "hi", "force_exit",
    };
    const std::string name = RequestNameOf(json);
    if (name.empty()) return false;
    for (const char* candidate : kInternal) {
        if (name == candidate) return true;
    }
    return false;
}

bool ControlChannel::SendFrame(std::uint32_t rid, const std::string& json,
                               std::uint32_t timeoutMs) noexcept {
    const std::uint32_t total = static_cast<std::uint32_t>(8u + 4u + json.size());
    std::vector<std::uint8_t> frame(total);
    WriteBe32(frame.data(), total);
    WriteBe32(frame.data() + 4, IsInternalRequest(json) ? kLlTypeRequestInternal
                                                        : kLlTypeRequest);
    WriteBe32(frame.data() + 8, rid);
    if (!json.empty()) {
        std::memcpy(frame.data() + 12, json.data(), json.size());
    }
    return connection_.SendAll(frame.data(), frame.size(), timeoutMs);
}

bool ControlChannel::ReadFrame(std::uint32_t expectedRid, std::uint32_t timeoutMs,
                               std::string& response, bool& final,
                               std::wstring& error) noexcept {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    for (;;) {
        // A complete frame is 8 bytes of header plus body. Parse whatever is
        // already buffered first.
        while (pending_.size() >= 8) {
            const std::uint32_t total = ReadBe32(pending_.data());
            if (total < 8 || total > 8u * 1024u * 1024u) {
                error = L"bad control frame length " + std::to_wstring(total);
                return false;
            }
            if (pending_.size() < total) break;
            const std::uint32_t type = ReadBe32(pending_.data() + 4);
            // Responses arrive as 0x00000002 (Lua reply) or 0x80000002 (C-layer
            // reply, e.g. load_script/get_log). Both are accepted; the rid is
            // what pairs them with a request.
            const std::uint32_t plainType = type & ~kLlTypeFlag;
            if (plainType == 3) {
                pending_.erase(pending_.begin(), pending_.begin() + total);
                continue;
            }
            if (plainType != 2 || total < 13) {
                pending_.erase(pending_.begin(), pending_.begin() + total);
                continue;
            }
            const std::uint32_t rid = ReadBe32(pending_.data() + 8);
            const bool isFinal = pending_[12] != 0;
            response.assign(reinterpret_cast<const char*>(pending_.data()) + 13,
                            total - 13);
            pending_.erase(pending_.begin(), pending_.begin() + total);
            if (rid != expectedRid) continue;
            final = isFinal;
            return true;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            error = L"control response timed out";
            return false;
        }
        uint8_t buffer[4096]{};
        std::size_t received = 0;
        if (!connection_.ReadSome(buffer, sizeof(buffer), received, 100)) {
            if (!connection_.IsOpen()) {
                error = L"control connection closed";
                return false;
            }
            continue;
        }
        pending_.insert(pending_.end(), buffer, buffer + received);
    }
}

bool ControlChannel::Request(const std::string& json, std::uint32_t timeoutMs,
                             std::string& response, bool& final,
                             std::wstring& error) noexcept {
    response.clear();
    final = false;
    error.clear();
    if (!connection_.IsOpen()) {
        error = L"control connection not open";
        return false;
    }
    const std::uint32_t rid = nextRid_++;
    lastRid_ = rid;
    if (!SendFrame(rid, json, timeoutMs)) {
        error = L"control request send failed";
        return false;
    }
    if (ReadFrame(rid, timeoutMs, response, final, error)) return true;

    // A read failure usually means the device dropped the session (its Lua
    // dispatcher reloaded, or it closed the socket). Sending the same request
    // again on a dead connection would fail identically, so reconnect once and
    // retry with a fresh rid. Only do this when the socket is actually gone:
    // retrying a live-but-slow device would double every timeout.
    if (connection_.IsOpen() && error == L"control response timed out") return false;
    if (address_.empty() || port_ == 0) return false;
    Close();
    std::wstring reconnectError;
    if (!Connect(address_, port_, kControlReconnectTimeoutMs, reconnectError)) {
        error = L"control reconnect failed: " + reconnectError;
        return false;
    }
    const std::uint32_t retryRid = nextRid_++;
    lastRid_ = retryRid;
    if (!SendFrame(retryRid, json, timeoutMs)) {
        error = L"control request resend failed";
        return false;
    }
    return ReadFrame(retryRid, timeoutMs, response, final, error);
}

namespace {

// One "device" object out of an enumerate_devices reply. The signal band sends
// a JSON array of objects; we only need the identity and the RAAT port, and we
// must not confuse one output's tcp_port with another output's.
struct WakeDeviceEntry final {
    std::string deviceId;
    std::string uniqueId;
    std::uint16_t tcpPort{};
};

// Splits the reply into the per-device `{ ... }` objects inside its "devices"
// array and keeps the ones that carry a numeric tcp_port. Replies arrive as a
// stream of JSON lines, so a truncated tail is normal and must not discard the
// complete objects that preceded it.
std::vector<WakeDeviceEntry> ParseWakeInventory(const std::string& text) {
    // Narrow down to the device array. A reply may be wrapped in
    // {"devices":[...]}, may be the bare array, or (from some bridges) a single
    // object per line.
    std::size_t region = 0;
    std::size_t regionEnd = text.size();
    const std::size_t key = text.find("\"devices\"");
    if (key != std::string::npos) {
        const std::size_t open = text.find('[', key);
        if (open != std::string::npos) {
            region = open + 1;
            int depth = 1;
            bool inString = false;
            std::size_t scan = region;
            for (; scan < text.size(); ++scan) {
                const char ch = text[scan];
                if (inString) {
                    if (ch == '\\') {
                        ++scan;
                    } else if (ch == '"') {
                        inString = false;
                    }
                    continue;
                }
                if (ch == '"') {
                    inString = true;
                } else if (ch == '[') {
                    ++depth;
                } else if (ch == ']') {
                    if (--depth == 0) break;
                }
            }
            if (scan < text.size()) regionEnd = scan;
        }
    }

    std::vector<WakeDeviceEntry> entries;
    std::size_t pos = region;
    while (pos < regionEnd) {
        const std::size_t begin = text.find('{', pos);
        if (begin == std::string::npos || begin >= regionEnd) break;
        int depth = 0;
        std::size_t end = begin;
        bool inString = false;
        for (; end < regionEnd; ++end) {
            const char ch = text[end];
            if (inString) {
                if (ch == '\\') {
                    ++end;
                } else if (ch == '"') {
                    inString = false;
                }
                continue;
            }
            if (ch == '"') {
                inString = true;
            } else if (ch == '{') {
                ++depth;
            } else if (ch == '}') {
                if (--depth == 0) {
                    ++end;
                    break;
                }
            }
        }
        if (depth != 0) break;  // truncated object: stop, keep what we have
        const std::string object = text.substr(begin, end - begin);
        pos = end;

        const std::int64_t port = JsonGetInt(object, "tcp_port", 0);
        if (port <= 0 || port > 65535) continue;
        WakeDeviceEntry entry;
        entry.tcpPort = static_cast<std::uint16_t>(port);
        entry.deviceId = JsonGetString(object, "device_id");
        entry.uniqueId = JsonGetString(object, "unique_id");
        entries.push_back(std::move(entry));
    }
    return entries;
}

// Picks the inventory entry that belongs to the requested endpoint. An empty
// deviceId means "the only output", which is what a single-output RoonBridge
// exposes.
bool SelectWakeEntry(const std::vector<WakeDeviceEntry>& entries,
                     const std::string& deviceId,
                     WakeDeviceEntry& selected) {
    if (entries.empty()) return false;
    if (deviceId.empty()) {
        if (entries.size() != 1) return false;
        selected = entries.front();
        return true;
    }
    for (const auto& entry : entries) {
        if (entry.deviceId == deviceId || entry.uniqueId == deviceId) {
            selected = entry;
            return true;
        }
    }
    return false;
}

} // namespace

bool WakeDevice(const std::wstring& address, const std::wstring& deviceId,
                std::uint16_t& controlPort, std::wstring& error) noexcept {
    error.clear();
    TcpConnection connection;
    if (!connection.Connect(address, 9200, 2000, error)) return false;

    const std::string enumerate = "{\"request\":\"enumerate_devices\",\"subscription_id\":\"0\"}\n";
    if (!connection.SendAll(enumerate.data(), enumerate.size(), 2000)) {
        error = L"wake enumerate send failed";
        return false;
    }

    const std::string wanted = ToUtf8(deviceId);

    // Reads until the reply has a complete "devices" array (or the window
    // closes) and returns whatever inventory was accumulated.
    const auto readInventory = [&](std::string& text) -> bool {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        std::size_t received = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            char buffer[1024]{};
            if (!connection.ReadSome(buffer, sizeof(buffer) - 1, received, 200)) {
                continue;
            }
            text.append(buffer, received);
            if (text.find("\"devices\"") != std::string::npos &&
                text.find(']') != std::string::npos) {
                return true;
            }
            if (text.size() > 65536) return true;
        }
        return !text.empty();
    };

    std::string firstResponse;
    (void)readInventory(firstResponse);
    WakeDeviceEntry selected;
    if (SelectWakeEntry(ParseWakeInventory(firstResponse), wanted, selected)) {
        controlPort = selected.tcpPort;
        connection.Close();
        return true;
    }

    // Nothing usable yet: ask the signal band to bring the output up, keep the
    // subscription alive, then re-enumerate.
    if (!wanted.empty()) {
        const std::string enable = "{\"request\":\"enable_device\",\"device_id\":\"" +
            JsonEscape(wanted) + "\",\"subscription_id\":\"0\"}\n";
        (void)connection.SendAll(enable.data(), enable.size(), 2000);
    }
    (void)connection.SendAll("BEGIN_KEEPALIVE\n", 16, 2000);

    std::string secondResponse;
    (void)readInventory(secondResponse);
    WakeDeviceEntry afterEnable;
    if (SelectWakeEntry(ParseWakeInventory(secondResponse), wanted, afterEnable)) {
        controlPort = afterEnable.tcpPort;
        connection.Close();
        return true;
    }

    error = L"enable_device not confirmed (9200 silent/closed)";
    connection.Close();
    return false;
}

} // namespace ammod::raat
