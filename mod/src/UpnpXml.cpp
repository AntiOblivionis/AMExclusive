#include "UpnpXml.h"

#include <windows.h>
#include <msxml6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>

namespace ammod::upnp {
namespace {

using Microsoft::WRL::ComPtr;

// MSXML is a COM component. The discovery thread and the audio-core worker
// have no COM apartment of their own, so every parse establishes one for the
// lifetime of the document (released before CoUninitialize by declaration order).
class ComApartment final {
public:
    ComApartment() noexcept {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = SUCCEEDED(hr);
        changedMode_ = hr == RPC_E_CHANGED_MODE;
    }
    ~ComApartment() {
        if (owned_ && !changedMode_) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool owned_{};
    bool changedMode_{};
};

class Bstr final {
public:
    Bstr() = default;
    explicit Bstr(const std::wstring& text)
        : value_(SysAllocStringLen(text.c_str(), static_cast<UINT>(text.size()))) {}
    ~Bstr() { SysFreeString(value_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    Bstr(Bstr&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    Bstr& operator=(Bstr&& other) noexcept {
        if (this != &other) {
            SysFreeString(value_);
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }
    BSTR get() const noexcept { return value_; }
    BSTR* put() noexcept { return &value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    BSTR value_{};
};

bool SetBoolProperty(const ComPtr<IXMLDOMDocument2>& document, const wchar_t* name) {
    VARIANT value{};
    VariantInit(&value);
    value.vt = VT_BOOL;
    value.boolVal = VARIANT_TRUE;
    const HRESULT hr = document->setProperty(Bstr(name).get(), value);
    VariantClear(&value);
    return SUCCEEDED(hr);
}

bool SetTextProperty(const ComPtr<IXMLDOMDocument2>& document, const wchar_t* name,
                     const wchar_t* text) {
    VARIANT value{};
    VariantInit(&value);
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(text);
    if (!value.bstrVal) return false;
    const HRESULT hr = document->setProperty(Bstr(name).get(), value);
    VariantClear(&value);
    return SUCCEEDED(hr);
}

ComPtr<IXMLDOMDocument2> LoadDocument(const std::wstring& xml) {
    ComPtr<IXMLDOMDocument2> document;
    if (FAILED(CoCreateInstance(CLSID_DOMDocument60, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&document)))) {
        return {};
    }
    document->put_async(VARIANT_FALSE);
    document->put_validateOnParse(VARIANT_FALSE);
    document->put_resolveExternals(VARIANT_FALSE);
    (void)SetBoolProperty(document, L"ProhibitDTD");
    (void)SetTextProperty(document, L"SelectionLanguage", L"XPath");
    VARIANT_BOOL loaded = VARIANT_FALSE;
    if (FAILED(document->loadXML(Bstr(xml).get(), &loaded)) || loaded == VARIANT_FALSE) {
        return {};
    }
    return document;
}

std::wstring NodeText(const ComPtr<IXMLDOMNode>& node) {
    if (!node) return {};
    Bstr text;
    if (FAILED(node->get_text(text.put())) || !text) return {};
    return std::wstring(text.get(), SysStringLen(text.get()));
}

std::wstring Trim(std::wstring value) {
    while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && std::iswspace(value.back())) value.pop_back();
    return value;
}

ComPtr<IXMLDOMNode> SelectSingle(const ComPtr<IXMLDOMNode>& context,
                                const std::wstring& xpath) {
    if (!context) return {};
    ComPtr<IXMLDOMNode> node;
    if (FAILED(context->selectSingleNode(Bstr(xpath).get(), &node))) return {};
    return node;
}

ComPtr<IXMLDOMNode> SelectSingle(const ComPtr<IXMLDOMDocument2>& document,
                                const std::wstring& xpath) {
    if (!document) return {};
    ComPtr<IXMLDOMNode> node;
    if (FAILED(document->selectSingleNode(Bstr(xpath).get(), &node))) return {};
    return node;
}

std::wstring FindText(const ComPtr<IXMLDOMDocument2>& document, const wchar_t* localName) {
    const std::wstring xpath = std::wstring(L"//*[local-name()='") + localName + L"']";
    return Trim(NodeText(SelectSingle(document, xpath)));
}

} // namespace

std::wstring EscapeXml(const std::wstring& text) {
    std::wstring escaped;
    escaped.reserve(text.size());
    for (const wchar_t ch : text) {
        switch (ch) {
        case L'&': escaped += L"&amp;"; break;
        case L'<': escaped += L"&lt;"; break;
        case L'>': escaped += L"&gt;"; break;
        case L'"': escaped += L"&quot;"; break;
        case L'\'': escaped += L"&apos;"; break;
        default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

std::wstring ResolveUrl(const std::wstring& base, const std::wstring& relative) {
    if (relative.empty()) return {};
    if (relative.rfind(L"http://", 0) == 0 || relative.rfind(L"https://", 0) == 0) {
        return relative;
    }
    const auto scheme = base.find(L"://");
    if (scheme == std::wstring::npos) return relative;
    const auto pathStart = base.find(L'/', scheme + 3);
    const std::wstring root = pathStart == std::wstring::npos ? base : base.substr(0, pathStart);
    if (relative.front() == L'/') return root + relative;
    if (pathStart == std::wstring::npos) return root + L"/" + relative;
    const auto lastSlash = base.find_last_of(L'/');
    if (lastSlash == std::wstring::npos || lastSlash < scheme + 3) {
        return root + L"/" + relative;
    }
    return base.substr(0, lastSlash + 1) + relative;
}

bool ParseDeviceDescription(const std::wstring& xml, const std::wstring& location,
                            DeviceDescription& description) noexcept {
    try {
        const ComApartment apartment;
        const auto document = LoadDocument(xml);
        if (!document) return false;
        description = {};
        description.location = location;
        description.udn = FindText(document, L"UDN");
        description.friendlyName = FindText(document, L"friendlyName");
        ComPtr<IXMLDOMNodeList> services;
        if (FAILED(document->selectNodes(Bstr(L"//*[local-name()='service']").get(),
                                         &services))) {
            return !description.friendlyName.empty();
        }
        long count = 0;
        if (services) services->get_length(&count);
        for (long index = 0; index < count; ++index) {
            ComPtr<IXMLDOMNode> service;
            if (FAILED(services->get_item(index, &service)) || !service) continue;
            ServiceEndpoint endpoint;
            endpoint.serviceType =
                Trim(NodeText(SelectSingle(service, L".//*[local-name()='serviceType']")));
            endpoint.controlUrl =
                Trim(NodeText(SelectSingle(service, L".//*[local-name()='controlURL']")));
            if (endpoint.serviceType.empty() || endpoint.controlUrl.empty()) continue;
            endpoint.controlUrl = ResolveUrl(location, endpoint.controlUrl);
            description.services.push_back(std::move(endpoint));
        }
        return !description.udn.empty() || !description.friendlyName.empty();
    } catch (...) {
        return false;
    }
}

bool FindServiceControl(const DeviceDescription& description,
                        const std::wstring& serviceType,
                        std::wstring& controlUrl) noexcept {
    for (const auto& service : description.services) {
        if (service.serviceType == serviceType) {
            controlUrl = service.controlUrl;
            return true;
        }
    }
    return false;
}

std::wstring BuildSoapEnvelope(const std::wstring& serviceType,
                               const std::wstring& action,
                               const std::wstring& actionBody) {
    std::wstring xml;
    xml.reserve(actionBody.size() + 512);
    xml += L"<?xml version=\"1.0\" encoding=\"utf-8\"?>";
    xml += L"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" ";
    xml += L"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">";
    xml += L"<s:Body><u:";
    xml += action;
    xml += L" xmlns:u=\"";
    xml += EscapeXml(serviceType);
    xml += L"\">";
    xml += actionBody;
    xml += L"</u:";
    xml += action;
    xml += L"></s:Body></s:Envelope>";
    return xml;
}

bool ParseSoapResponse(const std::wstring& xml,
                       const std::wstring& expectedAction,
                       std::vector<SoapValue>& values,
                       int& errorCode,
                       std::wstring& errorDescription) noexcept {
    values.clear();
    errorCode = 0;
    errorDescription.clear();
    try {
        const ComApartment apartment;
        const auto document = LoadDocument(xml);
        if (!document) return false;
        const std::wstring codeText = FindText(document, L"errorCode");
        if (!codeText.empty()) {
            errorCode = _wtoi(codeText.c_str());
            errorDescription = FindText(document, L"errorDescription");
            return false;
        }
        const std::wstring responseName = expectedAction + L"Response";
        ComPtr<IXMLDOMNode> response =
            SelectSingle(document, std::wstring(L"//*[local-name()='") + responseName + L"']");
        if (!response) return false;
        ComPtr<IXMLDOMNodeList> children;
        if (FAILED(response->get_childNodes(&children)) || !children) return true;
        long count = 0;
        children->get_length(&count);
        for (long index = 0; index < count; ++index) {
            ComPtr<IXMLDOMNode> child;
            if (FAILED(children->get_item(index, &child)) || !child) continue;
            // Use baseName (local name without namespace prefix) rather than
            // nodeName. Renderers that emit prefixed SOAP responses such as
            // <cm:Sink> would otherwise produce value.name == L"cm:Sink" and
            // FindValue(values, L"Sink", ...) would miss, which is one way
            // GetSinkProtocolInfo silently returns an empty Sink.
            Bstr name;
            child->get_baseName(name.put());
            SoapValue value;
            if (name) value.name = std::wstring(name.get(), SysStringLen(name.get()));
            value.value = Trim(NodeText(child));
            values.push_back(std::move(value));
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::wstring BuildDidlLite(const std::wstring& id,
                           const std::wstring& title,
                           const std::wstring& url,
                           const std::wstring& protocolInfo,
                           const std::wstring& duration,
                           bool broadcast,
                           std::uint64_t sizeBytes,
                           std::uint32_t sampleRate,
                           std::uint32_t bitsPerSample,
                           std::uint32_t channels) {
    std::wstring xml;
    xml.reserve(512);
    xml += L"<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" ";
    xml += L"xmlns:dc=\"http://purl.org/dc/elements/1.1/\" ";
    xml += L"xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">";
    xml += L"<item id=\"";
    xml += EscapeXml(id);
    xml += L"\" parentID=\"0\" restricted=\"1\"><dc:title>";
    xml += EscapeXml(title);
    // A live LPCM stream has no known duration. Declaring it as a musicTrack
    // without a duration makes OpenHome renderers treat the item as zero
    // length and stop immediately; audioBroadcast is the correct class for an
    // unbounded stream and carries no duration attribute.
    xml += broadcast
        ? L"</dc:title><upnp:class>object.item.audioItem.audioBroadcast</upnp:class>"
        : L"</dc:title><upnp:class>object.item.audioItem.musicTrack</upnp:class>";
    xml += L"<res protocolInfo=\"";
    xml += EscapeXml(protocolInfo);
    xml += L"\"";
    if (!broadcast && !duration.empty()) {
        xml += L" duration=\"";
        xml += EscapeXml(duration);
        xml += L"\"";
    }
    // A file-shaped stream advertises its exact entity length. Renderers that
    // re-request the payload with a byte Range use this to plan their reads,
    // and OpenHome controllers show it as the track size.
    if (!broadcast && sizeBytes != 0) {
        xml += L" size=\"";
        xml += std::to_wstring(sizeBytes);
        xml += L"\"";
    }
    // Strict renderers (Lumin) cross-check these against the container header
    // and the HTTP Content-Type before they will start playback.
    if (sampleRate != 0) {
        xml += L" sampleFrequency=\"";
        xml += std::to_wstring(sampleRate);
        xml += L"\"";
    }
    if (bitsPerSample != 0) {
        xml += L" bitsPerSample=\"";
        xml += std::to_wstring(bitsPerSample);
        xml += L"\"";
    }
    if (channels != 0) {
        xml += L" nrAudioChannels=\"";
        xml += std::to_wstring(channels);
        xml += L"\"";
    }
    xml += L">";
    xml += EscapeXml(url);
    xml += L"</res></item></DIDL-Lite>";
    return xml;
}

} // namespace ammod::upnp
