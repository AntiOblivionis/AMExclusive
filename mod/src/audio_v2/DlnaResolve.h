#pragma once

#include <cstdint>
#include <string>

namespace ammod::audio_v2 {

// Resolve the current device description URL for a renderer UDN with a live
// SSDP M-SEARCH.
//
// Renderers of the AURALiC / LightningPlayer class re-bind their UPnP HTTP
// port every few minutes, so the location persisted in am-exclusive.ini goes
// stale while nothing else changes. The UDN is the only stable identity, so a
// failed open re-resolves the URL from the network instead of retrying an
// address that no longer listens.
//
// Returns true and fills `location` when a device advertising the same UDN
// answers within `timeoutMs`. Never throws; safe to call off the audio thread.
bool ResolveLocationByUdn(const std::wstring& udn, std::uint32_t timeoutMs,
                          std::wstring& location) noexcept;

} // namespace ammod::audio_v2
