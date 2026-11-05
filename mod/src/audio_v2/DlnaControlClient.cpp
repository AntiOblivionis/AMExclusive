#include "DlnaControlClient.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <string>

namespace ammod::audio_v2 {
namespace {

constexpr wchar_t kAvTransport[] = L"urn:schemas-upnp-org:service:AVTransport:1";
constexpr wchar_t kRenderingControl[] = L"urn:schemas-upnp-org:service:RenderingControl:1";
constexpr wchar_t kConnectionManager[] = L"urn:schemas-upnp-org:service:ConnectionManager:1";
constexpr wchar_t kOhPlaylist[] = L"urn:av-openhome-org:service:Playlist:1";
constexpr wchar_t kOhTransport[] = L"urn:av-openhome-org:service:Transport:1";
constexpr wchar_t kOhTime[] = L"urn:av-openhome-org:service:Time:1";
constexpr wchar_t kOhVolume[] = L"urn:av-openhome-org:service:Volume:1";
constexpr wchar_t kOhInfo[] = L"urn:av-openhome-org:service:Info:1";

struct HttpResult final {
    DWORD status{};
    std::wstring body;
};

struct InternetHandle final {
    HINTERNET handle{};
    ~InternetHandle() {
        if (handle) WinHttpCloseHandle(handle);
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) : handle(value) {}
    operator HINTERNET() const noexcept { return handle; }
};

std::string ToUtf8(const std::wstring& text) {
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
                                          static_cast<int>(text.size()),
                                          nullptr, 0);
    if (chars <= 0) return {};
    std::wstring result(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), chars);
    return result;
}

bool HttpRequest(const std::wstring& url, const std::wstring& method,
                 const std::wstring& headers, const std::wstring& body,
                 HttpResult& result, std::wstring& error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        error = L"invalid URL";
        return false;
    }
    std::wstring host(parts.lpszHostName ? parts.lpszHostName : L"", parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath ? parts.lpszUrlPath : L"", parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength && parts.lpszExtraInfo) {
        path += std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    const WORD port = parts.nPort ? parts.nPort : (secure ? 443 : 80);

    InternetHandle session(WinHttpOpen(L"AMExclusive/0.1",
                                       WINHTTP_ACCESS_TYPE_NO_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = L"WinHttpOpen failed";
        return false;
    }
    WinHttpSetTimeouts(session, 3000, 3000, 5000, 8000);
    InternetHandle connect(WinHttpConnect(session, host.c_str(), port, 0));
    if (!connect) {
        error = L"connect failed";
        return false;
    }
    InternetHandle request(WinHttpOpenRequest(
        connect, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        error = L"request creation failed";
        return false;
    }
    const auto bodyBytes = ToUtf8(body);
    const DWORD bodySize = static_cast<DWORD>(bodyBytes.size());
    if (!WinHttpSendRequest(request,
                            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                            bodySize ? const_cast<char*>(bodyBytes.data()) : nullptr,
                            bodySize, bodySize, 0)) {
        error = L"send failed error=" + std::to_wstring(GetLastError());
        return false;
    }
    if (!WinHttpReceiveResponse(request, nullptr)) {
        error = L"response failed error=" + std::to_wstring(GetLastError());
        return false;
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX)) {
        result.status = status;
    }
    std::string bytes;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) break;
        if (!available) break;
        const std::size_t offset = bytes.size();
        if (bytes.size() + available > 128u * 1024u) {
            error = L"response too large";
            return false;
        }
        bytes.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, bytes.data() + offset, available, &read)) {
            error = L"read failed";
            return false;
        }
        bytes.resize(offset + read);
    }
    result.body = FromUtf8(bytes);
    return true;
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::wstring Trim(std::wstring value) {
    while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && std::iswspace(value.back())) value.pop_back();
    return value;
}

} // namespace

std::wstring DlnaControlClient::HostFromLocation(const std::wstring& location) {
    const auto scheme = location.find(L"://");
    if (scheme == std::wstring::npos) return {};
    const auto start = scheme + 3;
    const auto end = location.find_first_of(L"/:?", start);
    return location.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
}

std::wstring DlnaControlClient::TimeText(std::uint32_t seconds) {
    std::wstringstream text;
    const auto hours = seconds / 3600u;
    const auto minutes = (seconds / 60u) % 60u;
    const auto secs = seconds % 60u;
    text << hours << L':' << (minutes < 10 ? L"0" : L"") << minutes
         << L':' << (secs < 10 ? L"0" : L"") << secs;
    return text.str();
}

bool DlnaControlClient::LoadDescription(DlnaTarget& target, std::wstring& error) noexcept {
    // The old short-circuit returned early whenever avTransportControl was
    // already set, which silently skipped OpenHome discovery on a re-Open and
    // left openHomePlaylist empty even for devices that advertise it. The
    // description GET is cheap and runs once per Open; just always reload so
    // every service endpoint is resolved against the current device state.
    if (target.location.empty()) {
        error = L"device description URL is missing";
        return false;
    }
    HttpResult result;
    if (!HttpRequest(target.location, L"GET", {}, {}, result, error)) return false;
    if (result.status != 200 && result.status != 0) {
        error = L"description HTTP status " + std::to_wstring(result.status);
        return false;
    }
    upnp::DeviceDescription description;
    if (!upnp::ParseDeviceDescription(result.body, target.location, description)) {
        error = L"device description XML is invalid";
        return false;
    }
    if (!description.udn.empty()) target.udn = description.udn;
    if (!description.friendlyName.empty()) target.friendlyName = description.friendlyName;
    auto find = [&description](const wchar_t* type, std::wstring& url) {
        return upnp::FindServiceControl(description, type, url);
    };
    target.avTransport = find(kAvTransport, target.avTransportControl);
    target.volume = find(kRenderingControl, target.renderingControl);
    if (!find(kConnectionManager, target.connectionManager)) target.connectionManager.clear();
    target.openHome = find(kOhPlaylist, target.openHomePlaylist);
    if (target.openHome) {
        (void)find(kOhTransport, target.openHomeTransport);
        (void)find(kOhTime, target.openHomeTime);
        (void)find(kOhVolume, target.openHomeVolume);
        (void)find(kOhInfo, target.openHomeInfo);
    }
    target.nextUri = target.avTransport;
    target.seek = target.avTransport || target.openHome;
    return !target.avTransportControl.empty() || !target.openHomePlaylist.empty();
}

bool DlnaControlClient::PostSoap(const std::wstring& controlUrl,
                                 const std::wstring& serviceType,
                                 const std::wstring& action,
                                 const std::wstring& actionBody,
                                 std::wstring& response,
                                 std::wstring& error) noexcept {
    const std::wstring body = upnp::BuildSoapEnvelope(serviceType, action, actionBody);
    const std::wstring soapAction =
        L"SOAPACTION: \"" + serviceType + L"#" + action + L"\"\r\n";
    const std::wstring headers =
        L"Content-Type: text/xml; charset=\"utf-8\"\r\n" + soapAction;
    HttpResult result;
    if (!HttpRequest(controlUrl, L"POST", headers, body, result, error)) return false;
    if (result.status != 200 && result.status != 500) {
        error = L"SOAP HTTP status " + std::to_wstring(result.status);
        return false;
    }
    response = std::move(result.body);
    return true;
}

bool DlnaControlClient::Call(const std::wstring& controlUrl,
                             const std::wstring& serviceType,
                             const std::wstring& action,
                             const std::wstring& actionBody,
                             std::vector<upnp::SoapValue>& values,
                             std::wstring& error) noexcept {
    if (controlUrl.empty()) {
        error = L"service control URL is missing";
        return false;
    }
    std::wstring response;
    if (!PostSoap(controlUrl, serviceType, action, actionBody, response, error)) return false;
    int errorCode = 0;
    std::wstring errorDescription;
    if (!upnp::ParseSoapResponse(response, action, values, errorCode, errorDescription)) {
        error = errorCode
            ? L"UPnP error " + std::to_wstring(errorCode) + L": " + errorDescription
            : L"SOAP response is invalid";
        return false;
    }
    return true;
}

bool DlnaControlClient::FindValue(const std::vector<upnp::SoapValue>& values,
                                  const wchar_t* name, std::wstring& value) {
    for (const auto& entry : values) {
        if (entry.name == name) {
            value = entry.value;
            return true;
        }
    }
    return false;
}

bool DlnaControlClient::ParseTime(const std::wstring& text, std::uint32_t& seconds) {
    seconds = 0;
    if (text.empty()) return false;
    std::wstringstream stream(text);
    std::wstring part;
    std::vector<std::uint32_t> parts;
    while (std::getline(stream, part, L':')) {
        if (part.empty()) return false;
        parts.push_back(static_cast<std::uint32_t>(_wtoi(part.c_str())));
    }
    if (parts.size() == 3) {
        seconds = parts[0] * 3600u + parts[1] * 60u + parts[2];
        return true;
    }
    if (parts.size() == 2) {
        seconds = parts[0] * 60u + parts[1];
        return true;
    }
    return false;
}

bool DlnaControlClient::GetSinkProtocolInfo(const DlnaTarget& target,
                                            std::wstring& protocolInfo,
                                            std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    if (!Call(target.connectionManager, kConnectionManager, L"GetProtocolInfo", {},
              values, error)) {
        return false;
    }
    if (!FindValue(values, L"Sink", protocolInfo)) {
        error = L"GetProtocolInfo response lacks Sink";
        return false;
    }
    return true;
}

bool DlnaControlClient::SetAvTransportUri(const DlnaTarget& target,
                                          const std::wstring& uri,
                                          const std::wstring& metadata,
                                          std::wstring& error) noexcept {
    const std::wstring body =
        L"<InstanceID>0</InstanceID><CurrentURI>" + upnp::EscapeXml(uri) +
        L"</CurrentURI><CurrentURIMetaData>" + upnp::EscapeXml(metadata) +
        L"</CurrentURIMetaData>";
    std::vector<upnp::SoapValue> values;
    return Call(target.avTransportControl, kAvTransport, L"SetAVTransportURI", body,
                values, error);
}

bool DlnaControlClient::SetNextAvTransportUri(const DlnaTarget& target,
                                              const std::wstring& uri,
                                              const std::wstring& metadata,
                                              std::wstring& error) noexcept {
    const std::wstring body =
        L"<InstanceID>0</InstanceID><NextURI>" + upnp::EscapeXml(uri) +
        L"</NextURI><NextURIMetaData>" + upnp::EscapeXml(metadata) +
        L"</NextURIMetaData>";
    std::vector<upnp::SoapValue> values;
    return Call(target.avTransportControl, kAvTransport, L"SetNextAVTransportURI", body,
                values, error);
}

bool DlnaControlClient::Play(const DlnaTarget& target, std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.avTransportControl, kAvTransport, L"Play",
                L"<InstanceID>0</InstanceID><Speed>1</Speed>", values, error);
}

bool DlnaControlClient::Pause(const DlnaTarget& target, std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.avTransportControl, kAvTransport, L"Pause",
                L"<InstanceID>0</InstanceID>", values, error);
}

bool DlnaControlClient::Stop(const DlnaTarget& target, std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.avTransportControl, kAvTransport, L"Stop",
                L"<InstanceID>0</InstanceID>", values, error);
}

bool DlnaControlClient::SeekRelative(const DlnaTarget& target, std::uint32_t seconds,
                                     std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.avTransportControl, kAvTransport, L"Seek",
                L"<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>" +
                    TimeText(seconds) + L"</Target>", values, error);
}

bool DlnaControlClient::GetTransportState(const DlnaTarget& target, std::wstring& state,
                                          std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    if (!Call(target.avTransportControl, kAvTransport, L"GetTransportInfo",
              L"<InstanceID>0</InstanceID>", values, error)) {
        return false;
    }
    if (!FindValue(values, L"CurrentTransportState", state)) {
        error = L"GetTransportInfo response lacks state";
        return false;
    }
    return true;
}

bool DlnaControlClient::GetPosition(const DlnaTarget& target,
                                    std::uint32_t& positionSeconds,
                                    std::uint32_t& durationSeconds,
                                    std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    if (!Call(target.avTransportControl, kAvTransport, L"GetPositionInfo",
              L"<InstanceID>0</InstanceID>", values, error)) {
        return false;
    }
    std::wstring relTime;
    if (FindValue(values, L"RelTime", relTime)) (void)ParseTime(relTime, positionSeconds);
    std::wstring duration;
    if (FindValue(values, L"TrackDuration", duration)) (void)ParseTime(duration, durationSeconds);
    return true;
}

bool DlnaControlClient::SetVolume(const DlnaTarget& target, std::uint32_t volume,
                                  std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.renderingControl, kRenderingControl, L"SetVolume",
                L"<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>" +
                    std::to_wstring(volume) + L"</DesiredVolume>", values, error);
}

bool DlnaControlClient::GetVolume(const DlnaTarget& target, std::uint32_t& volume,
                                  std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    if (!Call(target.renderingControl, kRenderingControl, L"GetVolume",
              L"<InstanceID>0</InstanceID><Channel>Master</Channel>", values, error)) {
        return false;
    }
    std::wstring text;
    if (!FindValue(values, L"CurrentVolume", text)) {
        error = L"GetVolume response lacks CurrentVolume";
        return false;
    }
    volume = static_cast<std::uint32_t>(_wtoi(text.c_str()));
    return true;
}

bool DlnaControlClient::OpenHomeWrite(const DlnaTarget& target, std::uint32_t id,
                                       const std::wstring& uris,
                                       const std::wstring& titles,
                                       std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    const std::wstring body =
        L"<Id>" + std::to_wstring(id) + L"</Id><Uris>" + upnp::EscapeXml(uris) +
        L"</Uris><Titles>" + upnp::EscapeXml(titles) + L"</Titles>";
    return Call(target.openHomePlaylist, kOhPlaylist, L"Write", body, values, error);
}

bool DlnaControlClient::OpenHomePlaylistPlay(const DlnaTarget& target,
                                             std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.openHomePlaylist, kOhPlaylist, L"Play", {}, values, error);
}

bool DlnaControlClient::OpenHomePlaylistPlayId(const DlnaTarget& target,
                                               std::uint32_t id,
                                               std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.openHomePlaylist, kOhPlaylist, L"Play",
                L"<Id>" + std::to_wstring(id) + L"</Id>", values, error);
}

bool DlnaControlClient::OpenHomeDeleteAll(const DlnaTarget& target,
                                          std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.openHomePlaylist, kOhPlaylist, L"DeleteAll", {}, values, error);
}

bool DlnaControlClient::OpenHomeInsert(const DlnaTarget& target, std::uint32_t afterId,
                                       const std::wstring& uri,
                                       const std::wstring& metadata,
                                       std::uint32_t& newId,
                                       std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    const std::wstring body =
        L"<AfterId>" + std::to_wstring(afterId) + L"</AfterId><Uri>" +
        upnp::EscapeXml(uri) + L"</Uri><Metadata>" + upnp::EscapeXml(metadata) +
        L"</Metadata>";
    if (!Call(target.openHomePlaylist, kOhPlaylist, L"Insert", body, values, error)) {
        return false;
    }
    std::wstring text;
    newId = FindValue(values, L"NewId", text)
        ? static_cast<std::uint32_t>(_wtoi(text.c_str())) : 0;
    return true;
}

bool DlnaControlClient::OpenHomePlay(const DlnaTarget& target,
                                     std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.openHomeTransport, kOhTransport, L"Play", {}, values, error);
}

bool DlnaControlClient::OpenHomePause(const DlnaTarget& target,
                                      std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.openHomeTransport, kOhTransport, L"Pause", {}, values, error);
}

bool DlnaControlClient::OpenHomeStop(const DlnaTarget& target,
                                     std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.openHomeTransport, kOhTransport, L"Stop", {}, values, error);
}

bool DlnaControlClient::OpenHomeRead(const DlnaTarget& target, std::uint32_t id,
                                     std::wstring& metadata, std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    if (!Call(target.openHomePlaylist, kOhPlaylist, L"Read",
              L"<Id>" + std::to_wstring(id) + L"</Id>", values, error)) {
        return false;
    }
    if (!FindValue(values, L"Metadata", metadata)) {
        error = L"OpenHome Read response lacks Metadata";
        return false;
    }
    return true;
}

bool DlnaControlClient::OpenHomeSeekId(const DlnaTarget& target, std::uint32_t id,
                                       std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.openHomePlaylist, kOhPlaylist, L"SeekId",
                L"<Value>" + std::to_wstring(id) + L"</Value>", values, error);
}

bool DlnaControlClient::OpenHomeSeekSecondAbsolute(const DlnaTarget& target,
                                                   std::uint32_t seconds,
                                                   std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.openHomePlaylist, kOhPlaylist, L"SeekSecondAbsolute",
                L"<Value>" + std::to_wstring(seconds) + L"</Value>", values, error);
}

bool DlnaControlClient::OpenHomeTime(const DlnaTarget& target,
                                     std::uint32_t& positionSeconds,
                                     std::uint32_t& durationSeconds,
                                     std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    if (!Call(target.openHomeTime, kOhTime, L"Time", {}, values, error)) return false;
    std::wstring text;
    if (FindValue(values, L"RelTime", text)) (void)ParseTime(text, positionSeconds);
    if (FindValue(values, L"Duration", text)) (void)ParseTime(text, durationSeconds);
    return true;
}

bool DlnaControlClient::OpenHomeSetVolume(const DlnaTarget& target, std::uint32_t volume,
                                          std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    return Call(target.openHomeVolume, kOhVolume, L"SetVolume",
                L"<Value>" + std::to_wstring(volume) + L"</Value>", values, error);
}

bool DlnaControlClient::OpenHomeGetVolume(const DlnaTarget& target, std::uint32_t& volume,
                                          std::wstring& error) noexcept {
    std::vector<upnp::SoapValue> values;
    if (!Call(target.openHomeVolume, kOhVolume, L"Volume", {}, values, error)) return false;
    std::wstring text;
    if (!FindValue(values, L"Value", text)) {
        error = L"OpenHome Volume response lacks Value";
        return false;
    }
    volume = static_cast<std::uint32_t>(_wtoi(text.c_str()));
    return true;
}

bool DlnaControlClient::OpenHomeInfo(const DlnaTarget& target,
                                     std::vector<upnp::SoapValue>& values,
                                     std::wstring& error) noexcept {
    if (Call(target.openHomeInfo, kOhInfo, L"GetInfo", {}, values, error)) return true;
    // Some implementations expose Track instead of GetInfo.
    values.clear();
    return Call(target.openHomeInfo, kOhInfo, L"Track", {}, values, error);
}

std::wstring DlnaControlClient::ProtocolInfoFor(DlnaStreamShape shape,
                                                const PcmFormat& format,
                                                bool converted) {
    // CI=1 means "fetch this through the connection manager's transcoder".
    // We transcode server-side and hand the renderer a WAV it can decode
    // natively, so the flag must stay 0 - a VEGA G2.2 stopped consuming a
    // transcoded stream entirely when it saw CI=1 (the only time a track
    // failed, and the only time the flag was set).
    const std::wstring ci = L"0";
    // 0x01700000 flags: streaming mode, no interactive control, no background
    // transfer, no connection stall. Seek and time flags are deliberately 0.
    const std::wstring flags =
        L"DLNA.ORG_CI=" + ci +
        L";DLNA.ORG_FLAGS=01700000000000000000000000000000";
    switch (shape) {
    case DlnaStreamShape::FlacFile:
        // File-shaped FLAC: a declared length and byte ranges are offered, so
        // OP=01 is truthful. The "x-" spelling is what renderers advertise;
        // the registered audio/flac is dropped by a VEGA G2.2.
        return L"http-get:*:audio/x-flac:DLNA.ORG_OP=01;" + flags;
    case DlnaStreamShape::WavFile:
    default:
        // WAV is served file-shaped with a declared length and byte ranges
        // (AURALiC-class renderers re-request the payload as "Range: bytes=44-"),
        // so OP=01 - byte seek supported, time seek not - matches the server.
        return L"http-get:*:audio/wav:DLNA.ORG_OP=01;" + flags;
    }
    // Unreachable: every shape above returns. Kept so the compiler still
    // checks the return type if a shape is ever added without a case.
    return L"http-get:*:audio/wav:DLNA.ORG_OP=01;" + flags;
}

bool DlnaControlClient::ProtocolAllows(const std::wstring& sinkProtocolInfo,
                                       bool wav, std::uint32_t sampleRate,
                                       std::uint16_t bits, std::uint16_t channels) {
    std::wstringstream entries(sinkProtocolInfo);
    std::wstring entry;
    while (std::getline(entries, entry, L',')) {
        entry = Trim(entry);
        if (entry.empty()) continue;
        std::wstringstream fields(entry);
        std::wstring field;
        std::vector<std::wstring> parts;
        while (std::getline(fields, field, L':')) parts.push_back(Trim(field));
        if (parts.size() < 3) continue;
        // parts[2] is the media type with its parameters, e.g.
        // "audio/L16;rate=48000;channels=2". Split at the first ';' so the
        // comparison is against "audio/l16" and not the whole parameter list.
        const auto& typed = parts[2];
        const auto semicolon = typed.find(L';');
        const std::wstring mediaType = Lower(semicolon == std::wstring::npos
            ? typed : typed.substr(0, semicolon));
        const std::wstring parameters = semicolon == std::wstring::npos
            ? std::wstring{} : typed.substr(semicolon);
        // An unrestricted "*" entry only proves that the sink accepts an
        // unspecified content type; it is not a statement that the exact
        // rate/depth combination below is supported. Only exact matches count
        // as evidence, otherwise a device would be told to pull a format it
        // cannot decode.
        if (mediaType == L"*") continue;
        if (wav && (mediaType == L"audio/wav" || mediaType == L"audio/x-wav")) {
            return true;
        }
        if (!wav && (mediaType == L"audio/l16" || mediaType == L"audio/l24")) {
            const auto ratePos = parameters.find(L"rate=");
            if (ratePos != std::wstring::npos && sampleRate != 0) {
                std::wstringstream rateValue(parameters.substr(ratePos + 5));
                std::uint32_t declared = 0;
                rateValue >> declared;
                if (declared != 0 && declared != sampleRate) continue;
            }
            const auto channelsPos = parameters.find(L"channels=");
            if (channelsPos != std::wstring::npos) {
                std::wstringstream channelValue(parameters.substr(channelsPos + 9));
                std::uint32_t declared = 0;
                channelValue >> declared;
                if (declared != 0 && declared != channels) continue;
            }
            if (mediaType == L"audio/l16" && bits > 16) continue;
            if (mediaType == L"audio/l24" && bits > 24) continue;
            return true;
        }
    }
    return false;
}

} // namespace ammod::audio_v2
