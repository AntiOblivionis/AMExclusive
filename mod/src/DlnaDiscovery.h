#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ammod::dlna {

struct DiscoveredDevice final {
    std::wstring udn;
    std::wstring friendlyName;
    std::wstring location;
    bool openHome{};
    bool avTransport{};
    bool online{};
};

// SSDP discovery runs in the broker process: it is long-lived, unpackaged,
// and already owns persisted user state. It never touches Apple's processes.
class DlnaDiscovery final {
public:
    using Callback = std::function<void(const DiscoveredDevice& device)>;
    using LogCallback = std::function<void(const std::wstring& text)>;

    DlnaDiscovery() = default;
    ~DlnaDiscovery();

    DlnaDiscovery(const DlnaDiscovery&) = delete;
    DlnaDiscovery& operator=(const DlnaDiscovery&) = delete;

    bool Start(Callback callback, LogCallback log, std::wstring& error) noexcept;
    void Stop() noexcept;
    void Refresh() noexcept;
    std::vector<DiscoveredDevice> Snapshot() const;

    // Adds a device from "http://host:port/device.xml", "http://host:port/"
    // or "host:port". Fails closed unless the description proves a renderer
    // (AVTransport or OpenHome Playlist).
    bool AddManualDevice(const std::wstring& input, std::wstring& error) noexcept;

private:
    void ThreadMain() noexcept;
    void ScanOnce() noexcept;
    bool FetchDescription(const std::wstring& location, DiscoveredDevice& device) noexcept;
    void Publish(const DiscoveredDevice& device) noexcept;
    void LogLine(const std::wstring& text) const;

    Callback callback_;
    LogCallback log_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> refresh_{false};
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::unordered_map<std::wstring, DiscoveredDevice> devices_;
    std::unordered_map<std::wstring, std::uint64_t> lastSeenTick_;
};

} // namespace ammod::dlna
