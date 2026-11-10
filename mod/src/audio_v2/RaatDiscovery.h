#pragma once

#include "RaatTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ammod::raat {

// SOOD discovery. Mirrors the discovery side of the RAAT sender: a UDP query
// ("SOOD\x02Q", optionally carrying a service_id TLV) is sent to the RAAT
// discovery group 239.255.90.90 and to the limited broadcast address on port
// 9003, with the multicast egress interface pinned to the machine's outbound
// address. Devices answer with "SOOD\x02R" carrying a TLV map of strings
// (tcp_port, raat_version, output_name, vendor_model, ...).
//
// Endpoints that answer without a RAAT tcp_port are woken through the 9200
// signal band (RaatProtocol::WakeDevice) and only published once a control
// port is known: an output without a port cannot be opened.
//
// The broker owns the long-lived instance so device found/lost events can be
// pushed to the panel. RaatSink uses QueryOnce() to re-resolve a persisted
// device whose cached address or port went stale.
// SOOD UDP port. 0 (or any unset ini value) means the well-known 9003.
// The panel's "SOOD port" setting is applied by the broker before discovery
// starts; RaatSink::QueryOnce uses the same value.
void SetSoodPort(std::uint16_t port) noexcept;

class RaatDiscovery final {
public:
    using Callback = std::function<void(const RaatDevice& device)>;
    using LogCallback = std::function<void(const std::wstring& text)>;

    RaatDiscovery();
    ~RaatDiscovery();

    RaatDiscovery(const RaatDiscovery&) = delete;
    RaatDiscovery& operator=(const RaatDiscovery&) = delete;

    bool Start(Callback callback, LogCallback log, std::wstring& error) noexcept;
    void Stop() noexcept;
    void Refresh() noexcept;
    std::vector<RaatDevice> Snapshot() const;

    // One-shot query used by the sink when the persisted address/port fails.
    // Matches by deviceId (unique_id/_tid). Returns false on timeout.
    // unicastAddress is optional: when set, the query burst is also sent
    // straight to that renderer. The reference sender's "scan for Roon Ready
    // devices" is a UNICAST SOOD query carrying the controller identity;
    // multicast/broadcast-only queries never reach the firmware's
    // controller-registration path.
    static bool QueryOnce(const std::wstring& deviceId,
                          std::uint32_t timeoutMs,
                          RaatDevice& device,
                          std::wstring& error,
                          const std::wstring& unicastAddress = {}) noexcept;

private:
    struct Impl;
    // Shared so a wake worker can keep the state alive while it reports back.
    std::shared_ptr<Impl> impl_;
};

// Offline check of the SOOD wire format (query packet bytes, TLV parsing,
// 0xFFFF handling, source filtering). Returns 0 on success. Used by
// am_exclusive_raat_selftest so a regression fails a test instead of silently
// losing every device again.
int DiscoveryWireSelfTest();

} // namespace ammod::raat
