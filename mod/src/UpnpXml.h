#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ammod::upnp {

struct ServiceEndpoint final {
    std::wstring serviceType;
    std::wstring controlUrl;
};

struct DeviceDescription final {
    std::wstring udn;
    std::wstring friendlyName;
    std::wstring location;
    std::vector<ServiceEndpoint> services;
};

struct SoapValue final {
    std::wstring name;
    std::wstring value;
};

// Parses a UPnP device description without resolving external entities.
bool ParseDeviceDescription(const std::wstring& xml, const std::wstring& location,
                            DeviceDescription& description) noexcept;

// Case-sensitive service type lookup (UPnP service types are exact strings).
bool FindServiceControl(const DeviceDescription& description,
                        const std::wstring& serviceType,
                        std::wstring& controlUrl) noexcept;

// Resolves a relative controlURL against the device description URL.
std::wstring ResolveUrl(const std::wstring& base, const std::wstring& relative);

std::wstring EscapeXml(const std::wstring& text);

std::wstring BuildSoapEnvelope(const std::wstring& serviceType,
                               const std::wstring& action,
                               const std::wstring& actionBody);

// Returns false on a UPnP fault. errorCode is the UPnPError errorCode value
// (for example 701/714/716) and errorDescription its text.
bool ParseSoapResponse(const std::wstring& xml,
                       const std::wstring& expectedAction,
                       std::vector<SoapValue>& values,
                       int& errorCode,
                       std::wstring& errorDescription) noexcept;

// Minimal DIDL-Lite for an LPCM/WAV stream. protocolInfo is copied verbatim.
// broadcast selects the network-radio item class. A live stream that declares
// no length must be an audioBroadcast: a musicTrack without a duration is
// reported as a zero-length item by OpenHome playlist renderers, which then
// stop the transport as soon as it starts. When the stream is served
// file-shaped (declared Content-Length), pass broadcast=false plus duration
// and size so the renderer treats it as an ordinary track.
std::wstring BuildDidlLite(const std::wstring& id,
                           const std::wstring& title,
                           const std::wstring& url,
                           const std::wstring& protocolInfo,
                           const std::wstring& duration,
                           bool broadcast = false,
                           std::uint64_t sizeBytes = 0,
                           std::uint32_t sampleRate = 0,
                           std::uint32_t bitsPerSample = 0,
                           std::uint32_t channels = 0);

} // namespace ammod::upnp
