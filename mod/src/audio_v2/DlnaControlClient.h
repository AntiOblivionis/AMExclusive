#pragma once

#include "AudioSink.h"
#include "PcmTypes.h"
#include "../UpnpXml.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ammod::audio_v2 {

// A device whose description has been parsed and whose service endpoints are
// known. Loading is idempotent and performs no network access once complete.
class DlnaControlClient final {
public:
    DlnaControlClient() = default;
    ~DlnaControlClient() = default;

    DlnaControlClient(const DlnaControlClient&) = delete;
    DlnaControlClient& operator=(const DlnaControlClient&) = delete;

    bool LoadDescription(DlnaTarget& target, std::wstring& error) noexcept;
    bool GetSinkProtocolInfo(const DlnaTarget& target, std::wstring& protocolInfo,
                             std::wstring& error) noexcept;

    bool SetAvTransportUri(const DlnaTarget& target, const std::wstring& uri,
                           const std::wstring& metadata, std::wstring& error) noexcept;
    bool SetNextAvTransportUri(const DlnaTarget& target, const std::wstring& uri,
                               const std::wstring& metadata, std::wstring& error) noexcept;
    bool Play(const DlnaTarget& target, std::wstring& error) noexcept;
    bool Pause(const DlnaTarget& target, std::wstring& error) noexcept;
    bool Stop(const DlnaTarget& target, std::wstring& error) noexcept;
    bool SeekRelative(const DlnaTarget& target, std::uint32_t seconds,
                      std::wstring& error) noexcept;
    bool GetTransportState(const DlnaTarget& target, std::wstring& state,
                           std::wstring& error) noexcept;
    bool GetPosition(const DlnaTarget& target, std::uint32_t& positionSeconds,
                     std::uint32_t& durationSeconds, std::wstring& error) noexcept;

    bool SetVolume(const DlnaTarget& target, std::uint32_t volume,
                   std::wstring& error) noexcept;
    bool GetVolume(const DlnaTarget& target, std::uint32_t& volume,
                   std::wstring& error) noexcept;

    // OpenHome Playlist / Time / Volume / Info. These are the primary path for
    // OpenHome renderers; AVTransport remains the transport-control fallback.
    bool OpenHomeDeleteAll(const DlnaTarget& target, std::wstring& error) noexcept;
    // Legacy OpenHome (v0 draft) exposes Playlist.Write(Id, Uris, Titles) instead
    // of Insert. hifi-relay drives such devices with Write + Playlist.Play.
    bool OpenHomeWrite(const DlnaTarget& target, std::uint32_t id,
                       const std::wstring& uris, const std::wstring& titles,
                       std::wstring& error) noexcept;
    // BubbleUPnPServer / foo_upnp expose Playlist.Play with no arguments.
    bool OpenHomePlaylistPlay(const DlnaTarget& target, std::wstring& error) noexcept;
    bool OpenHomePlaylistPlayId(const DlnaTarget& target, std::uint32_t id,
                                std::wstring& error) noexcept;
    bool OpenHomeInsert(const DlnaTarget& target, std::uint32_t afterId,
                        const std::wstring& uri, const std::wstring& metadata,
                        std::uint32_t& newId, std::wstring& error) noexcept;
    bool OpenHomePlay(const DlnaTarget& target, std::wstring& error) noexcept;
    bool OpenHomePause(const DlnaTarget& target, std::wstring& error) noexcept;
    bool OpenHomeStop(const DlnaTarget& target, std::wstring& error) noexcept;
    bool OpenHomeRead(const DlnaTarget& target, std::uint32_t id, std::wstring& metadata,
                      std::wstring& error) noexcept;
    bool OpenHomeSeekId(const DlnaTarget& target, std::uint32_t id,
                        std::wstring& error) noexcept;
    bool OpenHomeSeekSecondAbsolute(const DlnaTarget& target, std::uint32_t seconds,
                                    std::wstring& error) noexcept;
    bool OpenHomeTime(const DlnaTarget& target, std::uint32_t& positionSeconds,
                      std::uint32_t& durationSeconds, std::wstring& error) noexcept;
    bool OpenHomeSetVolume(const DlnaTarget& target, std::uint32_t volume,
                           std::wstring& error) noexcept;
    bool OpenHomeGetVolume(const DlnaTarget& target, std::uint32_t& volume,
                           std::wstring& error) noexcept;
    bool OpenHomeInfo(const DlnaTarget& target, std::vector<upnp::SoapValue>& values,
                      std::wstring& error) noexcept;

    static std::wstring HostFromLocation(const std::wstring& location);
    static std::wstring ProtocolInfoFor(DlnaStreamShape shape, const PcmFormat& format,
                                        bool converted);
    static bool ProtocolAllows(const std::wstring& sinkProtocolInfo, bool wav,
                               std::uint32_t sampleRate, std::uint16_t bits,
                               std::uint16_t channels);
    static std::wstring TimeText(std::uint32_t seconds);

private:
    bool PostSoap(const std::wstring& controlUrl, const std::wstring& serviceType,
                  const std::wstring& action, const std::wstring& actionBody,
                  std::wstring& response, std::wstring& error) noexcept;
    bool Call(const std::wstring& controlUrl, const std::wstring& serviceType,
              const std::wstring& action, const std::wstring& actionBody,
              std::vector<upnp::SoapValue>& values, std::wstring& error) noexcept;
    static bool FindValue(const std::vector<upnp::SoapValue>& values,
                          const wchar_t* name, std::wstring& value);
    static bool ParseTime(const std::wstring& text, std::uint32_t& seconds);
};

} // namespace ammod::audio_v2
