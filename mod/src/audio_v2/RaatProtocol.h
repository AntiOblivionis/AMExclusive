#pragma once

#include <winsock2.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ammod::raat {

std::string ToUtf8(const std::wstring& value);
std::wstring ToWide(const std::string& value);

std::string JsonEscape(const std::string& value);
bool JsonFindString(const std::string& json, const std::string& key, std::string& out);
std::string JsonGetString(const std::string& json, const std::string& key);
bool JsonFindInt(const std::string& json, const std::string& key, std::int64_t& out);
std::int64_t JsonGetInt(const std::string& json, const std::string& key,
                        std::int64_t fallback = 0);

std::uint64_t NowMicroseconds() noexcept;
// The renderer's clock domain is nanoseconds: RAATCLKR carries
// get_local_time() values and req_start adds min_offset to the same
// unit. Keep this separate from the discovery/telemetry timestamps.
std::uint64_t NowNanoseconds() noexcept;
std::wstring NewUuidString();

// Process-wide WSAStartup (idempotent, never calls WSACleanup). The hook
// process may not have initialized Winsock before RAAT is selected.
bool EnsureWinsock() noexcept;

// TCP helper: connect with a bounded timeout and write/read fixed frames.
class TcpConnection final {
public:
    TcpConnection() = default;
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    bool Connect(const std::wstring& address, std::uint16_t port,
                 std::uint32_t timeoutMs, std::wstring& error) noexcept;
    void Close() noexcept;
    bool IsOpen() const noexcept { return socket_ != INVALID_SOCKET; }

    bool SendAll(const void* data, std::size_t size,
                 std::uint32_t timeoutMs) noexcept;
    bool ReadSome(void* data, std::size_t capacity, std::size_t& received,
                  std::uint32_t timeoutMs) noexcept;

    SOCKET Handle() const noexcept { return socket_; }
    std::wstring LocalAddress() const noexcept;

private:
    SOCKET socket_{INVALID_SOCKET};
};

// Reconnect budget used when Request() has to re-establish a dropped session.
inline constexpr std::uint32_t kControlReconnectTimeoutMs = 1500;

// LL frame types. The 0x80000000 bit is NOT a global "real wire format" flag:
// it selects which side of the renderer handles the request.
//
//   type = 0x80000001  -> internal / C-layer request. The firmware handles it
//                         itself (load_script, get_log, hi). Any other request
//                         sent with this bit gets {"status":"NotImplemented"}
//                         WITHOUT ever reaching the Lua dispatcher.
//   type = 0x00000001  -> Lua-dispatched request (echo, info, setup, stream,
//                         sync, start, stop, teardown, ...). Only valid after
//                         load_script installed a message handler on the
//                         session; before that the C layer answers
//                         NotImplemented.
//
// A captured foobar2000 session against an AURALiC VEGA G2.2 (raat_version
// 1.1.41) shows exactly this split: load_script and get_log carry
// 80 00 00 01, while echo/setup/stream/sync/start/stop/teardown all carry
// 00 00 00 01. Responses mirror the request: 80 00 00 02 for internal calls,
// 00 00 00 02 for Lua replies. The device's own log (raat_server.c:433
// GOT[LL] -> SENT[LL]) confirms the C layer answers a high-bit echo in 19 us
// while the Lua script is demonstrably running.
inline constexpr std::uint32_t kLlTypeFlag = 0x80000000u;
inline constexpr std::uint32_t kLlTypeRequest = 0x00000001u;      // Lua dispatch
inline constexpr std::uint32_t kLlTypeRequestInternal = 0x80000001u; // C layer
inline constexpr std::uint32_t kLlTypeResponse = 0x00000002u;
inline constexpr std::uint32_t kLlTypeResponseInternal = 0x80000002u;
inline constexpr std::uint32_t kLlTypeArtwork = 0x80000003u;
inline constexpr std::uint32_t kLlTypeHello = 0x80000004u;

// True for requests the renderer's C layer answers itself. These must be sent
// with kLlTypeRequestInternal; every other request must NOT carry the bit or
// the Lua dispatcher never sees it.
bool IsInternalRequest(const std::string& json) noexcept;

// RAAT control channel (the "LL" protocol).
//
// Hello frame   : u32be total_len(16), u32be type(0x80000004), u32be 2000, u32be 10000
// Request frame : u32be total_len, u32be type(1 or 0x80000001), u32be rid, json
// Response frame: u32be total_len, u32be type(2 or 0x80000002), u32be rid, u8 final, json
// Artwork frame : u32be total_len, u32be type(0x80000003), binary blob
class ControlChannel final {
public:
    bool Connect(const std::wstring& address, std::uint16_t port,
                 std::uint32_t timeoutMs, std::wstring& error) noexcept;
    void Close() noexcept;
    bool IsOpen() const noexcept { return connection_.IsOpen(); }
    std::wstring LocalAddress() const noexcept { return connection_.LocalAddress(); }

    // Sends one request and returns the first response carrying the same rid.
    // Many RAAT handlers answer non-final (Buffering/Ready/Playing), so callers
    // must not insist on final=true.
    bool Request(const std::string& json, std::uint32_t timeoutMs,
                 std::string& response, bool& final, std::wstring& error) noexcept;

    // Fire-and-forget request. echo/info are Lua-dispatched, so before
    // load_script has installed the dispatcher they can only answer
    // NotImplemented; the greeting burst must therefore not be waited on.
    bool SendProbe(const std::string& json, std::uint32_t timeoutMs) noexcept;

    // Wait for the renderer's 8-byte type-3 "ready" event. Not used by the sink
    // any more (direct testing showed the dispatcher installs without waiting
    // for it), but kept for tools/tests that want to observe the event.
    bool WaitForReady(std::uint32_t timeoutMs, std::wstring& error) noexcept;

    // Rid used by the most recent Request()/SendFrame(). Some Lua handlers
    // answer one request with several frames sharing that rid (setup sends
    // {"status":"OutputMessage"} and only later the frame carrying the ports),
    // so the caller has to keep reading on the same rid.
    std::uint32_t LastRequestRid() const noexcept { return lastRid_; }

    // Reads the next frame with `expectedRid`. Public for the multi-frame
    // responses described above; Request() uses it internally.
    bool ReadFrame(std::uint32_t expectedRid, std::uint32_t timeoutMs,
                   std::string& response, bool& final, std::wstring& error) noexcept;

private:
    bool SendHello(std::wstring& error) noexcept;
    bool SendFrame(std::uint32_t rid, const std::string& json,
                   std::uint32_t timeoutMs) noexcept;

    TcpConnection connection_;
    std::vector<std::uint8_t> pending_;
    std::uint32_t nextRid_{1};
    std::uint32_t lastRid_{0};
    // Remembered so Request() can reconnect once when the device drops the
    // session mid-conversation.
    std::wstring address_;
    std::uint16_t port_{};
};

// Minimal RoonBridge/RAATServer "signal band" wake: TCP 9200 carries line
// delimited JSON. enumerate_devices lists outputs and their tcp_port;
// enable_device brings a sleeping output up.
bool WakeDevice(const std::wstring& address, const std::wstring& deviceId,
                std::uint16_t& controlPort, std::wstring& error) noexcept;

} // namespace ammod::raat
