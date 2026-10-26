#include "BoundedLog.h"
#include "IpcProtocol.h"
#include "IpcTransport.h"
#include "UiRefreshState.h"
#include "UiPlaybackState.h"

#include <windows.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include "ApplePlayback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

namespace {

using namespace winrt;
namespace Xaml = Microsoft::UI::Xaml;
namespace Controls = Microsoft::UI::Xaml::Controls;
namespace Primitives = Microsoft::UI::Xaml::Controls::Primitives;
namespace Input = Microsoft::UI::Xaml::Input;
namespace Media = Microsoft::UI::Xaml::Media;
namespace Automation = Microsoft::UI::Xaml::Automation;
using namespace ammod::ipc;
using ObserverRef = winrt::weak_ref<Xaml::DependencyObject>;
using WindowObserverRef = winrt::weak_ref<Xaml::Window>;
namespace ApplePlayback = ammod::apple_private::playback;

HMODULE g_module{};
std::filesystem::path g_logPath;
std::mutex g_logMutex;
std::mutex g_pipeMutex;
std::mutex g_pipeWriteMutex;
std::mutex g_statusMutex;
std::mutex g_uiTargetMutex;
std::mutex g_transportMutex;
HANDLE g_pipe{INVALID_HANDLE_VALUE};
Message g_status{};
std::atomic<bool> g_connected{};
std::atomic<bool> g_suppressToggle{};
std::atomic<int> g_programmaticToggleValue{-1};
std::atomic<int> g_pendingEnabled{-1};
std::atomic<int> g_pendingHardwareBufferMs{-1};
std::atomic<int> g_requestedHardwareBufferMs{-1};
std::atomic<ULONGLONG> g_hardwareBufferAckDeadlineTick{};
std::atomic<bool> g_hardwareBufferInputDirty{};
std::atomic<bool> g_hardwareBufferTextUpdating{};
std::atomic<int> g_pendingAllowResampling{-1};
std::atomic<int> g_requestedAllowResampling{-1};
std::atomic<ULONGLONG> g_resamplingAckDeadlineTick{};
std::atomic<bool> g_suppressResamplingToggle{};
std::atomic<int> g_programmaticResamplingValue{-1};
std::atomic<std::uint64_t> g_lastLoggedGeneration{UINT64_MAX};
std::atomic<std::uint64_t> g_transportEpoch{};
std::atomic<std::uint64_t> g_activeSeekEpoch{};
std::atomic<std::uintptr_t> g_activeSeekOwner{};
std::atomic<ULONGLONG> g_lastSkipPointerTick{};
std::deque<Message> g_transportPending;
// Weak XAML controls must only be resolved on their owning UI thread.
thread_local std::vector<ObserverRef> g_transportButtonObservers;
thread_local std::vector<ObserverRef> g_skipBackObservers;
thread_local std::vector<ObserverRef> g_skipForwardObservers;
thread_local std::vector<ObserverRef> g_trackListPointerRootObservers;
thread_local std::vector<ObserverRef> g_scrubberObservers;
thread_local std::vector<ObserverRef> g_scrubberThumbObservers;
thread_local std::vector<ObserverRef> g_transportPopupObservers;
thread_local std::vector<WindowObserverRef> g_transportWindowObservers;
thread_local std::uintptr_t g_lastTrackRowPressIdentity{};
thread_local std::uint64_t g_lastTrackRowPressEpoch{};
thread_local ULONGLONG g_lastTrackRowPressTick{};
HWINEVENTHOOK g_windowLifecycleHook{};
winrt::weak_ref<Controls::ToggleSwitch> g_toggle;
winrt::weak_ref<Controls::TextBox> g_hardwareBufferBox;
winrt::weak_ref<Controls::ToggleSwitch> g_resamplingToggle;
winrt::weak_ref<Controls::Border> g_outputFormatCard;
Microsoft::UI::Dispatching::DispatcherQueue g_dispatcher{nullptr};
Microsoft::UI::Dispatching::DispatcherQueueTimer g_statusRefreshTimer{nullptr};
WindowObserverRef g_settingsWindow;

void AttachTransportObservers(const Xaml::Window& window, const std::wstring& windowProperty);
bool AttachOrRefreshToggle(const Xaml::DependencyObject& page);
void ScheduleTransportObserverAttach();
void ScheduleUiDiscovery();
void SchedulePlaybackStateSync();

void CALLBACK WindowLifecycleEvent(HWINEVENTHOOK, DWORD event, HWND window,
                                   LONG objectId, LONG childId, DWORD, DWORD) {
    if ((event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_SHOW) ||
        objectId != OBJID_WINDOW || childId != CHILDID_SELF || !window ||
        GetAncestor(window, GA_ROOT) != window) {
        return;
    }
    // HWND ownership does not identify a XAML apartment. Always dispatch via
    // the actual XAML Window, even when this event came from an auxiliary HWND.
    ScheduleUiDiscovery();
}

bool UseChineseUi() {
    static const bool chinese = [] {
        // Follow Apple Music's packaged-app runtime language rather than the
        // legacy Win32 default UI LANGID. Windows can refresh the app runtime
        // language after a display-language change before that LANGID changes.
        try {
            const auto languages = Windows::Globalization::ApplicationLanguages::Languages();
            if (languages && languages.Size() != 0) {
                const std::wstring tag = languages.GetAt(0).c_str();
                return tag.size() >= 2 &&
                       (tag[0] == L'z' || tag[0] == L'Z') &&
                       (tag[1] == L'h' || tag[1] == L'H') &&
                       (tag.size() == 2 || tag[2] == L'-');
            }
        } catch (...) {
        }
        const LANGID language = GetUserDefaultUILanguage();
        return PRIMARYLANGID(language) == LANG_CHINESE;
    }();
    return chinese;
}

std::wstring Localized(std::wstring_view chinese, std::wstring_view english) {
    return std::wstring(UseChineseUi() ? chinese : english);
}

void Log(const std::wstring& message) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wostringstream line;
    line << std::setfill(L'0') << std::setw(4) << now.wYear << L'-' << std::setw(2) << now.wMonth
         << L'-' << std::setw(2) << now.wDay << L'T' << std::setw(2) << now.wHour << L':'
         << std::setw(2) << now.wMinute << L':' << std::setw(2) << now.wSecond << L'.'
         << std::setw(3) << now.wMilliseconds << L" pid=" << GetCurrentProcessId()
         << L" tid=" << GetCurrentThreadId() << L" " << message;
    ammod::logging::AppendWideLine(g_logPath, g_logMutex, line.str(), 2ull * 1024ull * 1024ull, 2);
}

std::wstring ErrorText(ErrorCategory category) {
    switch (category) {
    case ErrorCategory::UnsupportedLocalInt32:
        return Localized(
            L"本地 32 位整数音频不受支持；已停止当前播放并清空音频加载",
            L"Local 32-bit integer audio is unsupported; playback was stopped and the audio load was cleared");
    case ErrorCategory::BitPerfectFormatUnavailable:
        return Localized(
            L"当前 DAC 不支持此音源所需的任何 bit-perfect 位深组合；已停止当前播放并清空音频加载",
            L"The DAC supports none of the bit-perfect depth combinations required by this source; playback was stopped and the audio load was cleared");
    case ErrorCategory::ExclusiveFormatUnavailable:
        return Localized(
            L"尝试兼容格式转换后，仍未找到设备支持的 WASAPI 独占输出格式；已停止当前播放",
            L"No supported WASAPI exclusive format was found after sample-rate and bit-depth conversion; playback was stopped");
    case ErrorCategory::FormatUnsupported:
        return Localized(
            L"Apple 当前共享渲染格式无法直接用于独占输出；这不代表设备不支持独占",
            L"Apple's current render format is not suitable for exclusive output");
    case ErrorCategory::ExclusiveNotAllowed:
        return Localized(L"当前播放设备未允许应用使用独占模式",
                         L"Exclusive mode is disabled for the current playback device");
    case ErrorCategory::DeviceInUse:
        return Localized(L"当前播放设备正被其他程序占用",
                         L"The current playback device is in use by another app");
    case ErrorCategory::DeviceChanged:
    case ErrorCategory::DeviceInvalidated:
        return Localized(L"默认播放设备已变化或不可用",
                         L"The default playback device changed or is unavailable");
    case ErrorCategory::AudioServiceUnavailable:
        return Localized(L"Windows 音频服务当前不可用",
                         L"The Windows Audio service is currently unavailable");
    case ErrorCategory::Timeout:
        return Localized(
            L"独占设备已启动，但 Apple Music 未继续提交音频；独占模式仍保持开启，将在下一首歌曲重试",
            L"Apple Music stopped sending audio; exclusive mode remains enabled and will retry on the next song");
    case ErrorCategory::EndpointDrainTimeout:
        return Localized(
            L"等待播放设备完成歌曲尾部播放时超时；请重试歌曲",
            L"Timed out waiting for the playback device to finish the track tail; retry the song");
    case ErrorCategory::BufferSizeNotAligned:
        return Localized(
            L"声卡驱动拒绝了当前硬件缓冲区；请调整硬件缓冲区后重试",
            L"The audio driver rejected the current hardware buffer; adjust Hardware buffer and try again");
    case ErrorCategory::ProtocolVersionMismatch:
        return Localized(L"独占音频组件版本不一致；请修复安装并重启 Apple Music",
                         L"Audio component versions do not match; repair installation and restart Apple Music");
    case ErrorCategory::HookLoadFailed:
    case ErrorCategory::BrokerUnavailable:
    case ErrorCategory::PipeUnavailable:
        return Localized(L"独占音频组件无法启动",
                         L"The exclusive audio component could not start");
    case ErrorCategory::None:
        return Localized(L"关闭", L"Off");
    default:
        return Localized(L"无法启用独占模式", L"Unable to enable exclusive mode");
    }
}

Message StatusSnapshot() {
    std::lock_guard lock(g_statusMutex);
    return g_status;
}

bool SendEnabled(bool enabled) {
    if (!g_connected.load()) return false;
    g_pendingEnabled.store(enabled ? 1 : 0);
    return true;
}

bool SendHardwareBuffer(std::uint32_t milliseconds) {
    // Keep the requested value across a transient Broker disconnect. The pipe
    // thread sends it immediately when connected and retains it until the
    // Broker echoes the value after verifying the INI write.
    g_requestedHardwareBufferMs.store(static_cast<int>(milliseconds));
    g_pendingHardwareBufferMs.store(static_cast<int>(milliseconds));
    g_hardwareBufferAckDeadlineTick.store(0);
    return true;
}

void SendAllowResampling(bool enabled) {
    g_requestedAllowResampling.store(enabled ? 1 : 0);
    g_pendingAllowResampling.store(enabled ? 1 : 0);
    g_resamplingAckDeadlineTick.store(0);
}

bool TrySendConnectedMessage(const Message& message) {
    if (!g_connected.load(std::memory_order_acquire)) return false;
    std::lock_guard pipeLock(g_pipeMutex);
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    std::lock_guard writeLock(g_pipeWriteMutex);
    return WriteMessage(g_pipe, message);
}

void QueueTransport(std::wstring_view action, std::uint64_t epoch = 0) {
    if (action.empty()) return;
    auto message = NewMessage(MessageType::TransportIntent, Role::Ui);
    message.uiPid = GetCurrentProcessId();
    message.generation = epoch;
    wcsncpy_s(message.detail, action.data(), _TRUNCATE);
    if (TrySendConnectedMessage(message)) {
        Log(L"transport sent immediate action=" + std::wstring(action) +
            L" epoch=" + std::to_wstring(epoch));
        return;
    }
    {
        std::lock_guard lock(g_transportMutex);
        g_transportPending.push_back(message);
    }
    Log(L"transport queued fallback action=" + std::wstring(action) +
        L" epoch=" + std::to_wstring(epoch));
}

void QueueSkipIntent(bool pointerEvent) {
    const auto now = GetTickCount64();
    if (pointerEvent) {
        g_lastSkipPointerTick.store(now, std::memory_order_release);
    } else {
        const auto pointerTick = g_lastSkipPointerTick.exchange(0, std::memory_order_acq_rel);
        if (pointerTick != 0 && now >= pointerTick && now - pointerTick <= 500) {
            Log(L"transport skip click deduplicated after pointer press");
            return;
        }
    }

    const auto epoch = g_transportEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    QueueTransport(L"skip_arm", epoch);
    QueueTransport(L"skip", epoch);
}

void BeginSeekGesture(std::uintptr_t owner) {
    if (!owner || g_activeSeekEpoch.load(std::memory_order_acquire) != 0) return;
    const auto epoch = g_transportEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::uint64_t expected{};
    if (g_activeSeekEpoch.compare_exchange_strong(
            expected, epoch, std::memory_order_acq_rel, std::memory_order_acquire)) {
        g_activeSeekOwner.store(owner, std::memory_order_release);
        QueueTransport(L"seek_begin", epoch);
    }
}

void CommitSeekGesture(std::uintptr_t owner = 0, std::wstring_view reason = {}) {
    const auto activeOwner = g_activeSeekOwner.load(std::memory_order_acquire);
    if (owner != 0 && activeOwner != owner) return;
    const auto epoch = g_activeSeekEpoch.exchange(0, std::memory_order_acq_rel);
    if (!epoch) return;
    g_activeSeekOwner.store(0, std::memory_order_release);
    QueueTransport(L"seek_commit", epoch);
    if (!reason.empty()) {
        Log(L"transport seek terminal reason=" + std::wstring(reason) +
            L" epoch=" + std::to_wstring(epoch));
    }
}

using GetAppPropertyFn = Windows::Foundation::IInspectable(__cdecl*)(hstring const&);

GetAppPropertyFn GetAppPropertyFunction() {
    static std::atomic<GetAppPropertyFn> cached{};
    if (auto function = cached.load(std::memory_order_acquire)) return function;
    const auto function = [] {
        HMODULE utils = GetModuleHandleW(L"AMP.Utils.dll");
        if (!utils) return GetAppPropertyFn{};
        constexpr char exportName[] =
            "?GetAppProperty@SharedUtils@@YA?AUIInspectable@Foundation@Windows@winrt@@AEBUhstring@5@@Z";
        return reinterpret_cast<GetAppPropertyFn>(GetProcAddress(utils, exportName));
    }();
    if (function) cached.store(function, std::memory_order_release);
    return function;
}

Xaml::Window GetXamlWindowProperty(std::wstring_view propertyName) {
    const auto function = GetAppPropertyFunction();
    if (!function || propertyName.empty()) return nullptr;
    try {
        return function(hstring(propertyName)).try_as<Xaml::Window>();
    } catch (const hresult_error& error) {
        if (propertyName == L"MiniPlayerWindow" && error.code() == E_BOUNDS) return nullptr;
        Log(L"GetAppProperty(" + std::wstring(propertyName) + L") failed hr=0x" +
            [&] { std::wostringstream text; text << std::hex << error.code().value; return text.str(); }());
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

void AppendUniqueWindow(std::vector<std::pair<std::wstring, Xaml::Window>>& windows,
                        std::wstring_view propertyName) {
    auto window = GetXamlWindowProperty(propertyName);
    if (!window) return;
    const auto identity = reinterpret_cast<std::uintptr_t>(get_abi(window));
    if (!identity) return;
    if (std::none_of(windows.begin(), windows.end(), [identity](const auto& existing) {
            return reinterpret_cast<std::uintptr_t>(get_abi(existing.second)) == identity;
        })) {
        windows.emplace_back(propertyName, window);
    }
}

std::vector<std::pair<std::wstring, Xaml::Window>> GetTransportXamlWindows() {
    std::vector<std::pair<std::wstring, Xaml::Window>> windows;
    AppendUniqueWindow(windows, L"MainWindow");
    AppendUniqueWindow(windows, L"MiniPlayerWindow");
    return windows;
}

Xaml::DependencyObject FindByClass(const Xaml::DependencyObject& node,
                                   std::wstring_view classFragment) {
    if (!node) return nullptr;
    try {
        const std::wstring className = get_class_name(node).c_str();
        if (className.find(classFragment) != std::wstring::npos) return node;
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            auto found = FindByClass(Media::VisualTreeHelper::GetChild(node, index), classFragment);
            if (found) return found;
        }
    } catch (...) {
    }
    return nullptr;
}

Xaml::DependencyObject FindByName(const Xaml::DependencyObject& node, std::wstring_view name) {
    if (!node) return nullptr;
    try {
        if (auto element = node.try_as<Xaml::FrameworkElement>(); element && element.Name() == name) return node;
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            auto found = FindByName(Media::VisualTreeHelper::GetChild(node, index), name);
            if (found) return found;
        }
    } catch (...) {
    }
    return nullptr;
}

Xaml::DependencyObject FindByAutomationId(const Xaml::DependencyObject& node,
                                          std::wstring_view automationId) {
    if (!node) return nullptr;
    try {
        if (Automation::AutomationProperties::GetAutomationId(node) == automationId) return node;
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            auto found = FindByAutomationId(Media::VisualTreeHelper::GetChild(node, index),
                                            automationId);
            if (found) return found;
        }
    } catch (...) {
    }
    return nullptr;
}

void AppendUniqueNode(std::vector<Xaml::DependencyObject>& nodes,
                      const Xaml::DependencyObject& node) {
    if (!node) return;
    const auto identity = reinterpret_cast<std::uintptr_t>(get_abi(node));
    if (!identity) return;
    if (std::none_of(nodes.begin(), nodes.end(), [identity](const auto& existing) {
            return reinterpret_cast<std::uintptr_t>(get_abi(existing)) == identity;
        })) {
        nodes.push_back(node);
    }
}

void CollectByName(const Xaml::DependencyObject& node, std::wstring_view name,
                   std::vector<Xaml::DependencyObject>& matches) {
    if (!node) return;
    try {
        if (auto element = node.try_as<Xaml::FrameworkElement>(); element && element.Name() == name) {
            AppendUniqueNode(matches, node);
        }
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            CollectByName(Media::VisualTreeHelper::GetChild(node, index), name, matches);
        }
    } catch (...) {
    }
}

void CollectByAutomationId(const Xaml::DependencyObject& node,
                           std::wstring_view automationId,
                           std::vector<Xaml::DependencyObject>& matches) {
    if (!node) return;
    try {
        if (Automation::AutomationProperties::GetAutomationId(node) == automationId) {
            AppendUniqueNode(matches, node);
        }
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            CollectByAutomationId(Media::VisualTreeHelper::GetChild(node, index),
                                  automationId, matches);
        }
    } catch (...) {
    }
}

void CollectSliders(const Xaml::DependencyObject& node,
                    std::vector<Xaml::DependencyObject>& matches) {
    if (!node) return;
    try {
        if (node.try_as<Controls::Slider>()) AppendUniqueNode(matches, node);
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            CollectSliders(Media::VisualTreeHelper::GetChild(node, index), matches);
        }
    } catch (...) {
    }
}

bool RememberObserver(std::vector<ObserverRef>& observers,
                      const Xaml::DependencyObject& node) {
    if (!node) return false;
    const auto identity = reinterpret_cast<std::uintptr_t>(get_abi(node));
    if (!identity) return false;
    bool alreadyObserved = false;
    auto write = observers.begin();
    for (auto read = observers.begin(); read != observers.end(); ++read) {
        auto live = read->get();
        if (!live) continue;
        if (reinterpret_cast<std::uintptr_t>(get_abi(live)) == identity) alreadyObserved = true;
        if (write != read) *write = *read;
        ++write;
    }
    observers.erase(write, observers.end());
    if (alreadyObserved) return false;
    observers.push_back(winrt::make_weak(node));
    return true;
}

bool RememberTransportWindow(const Xaml::Window& window) {
    if (!window) return false;
    const auto identity = reinterpret_cast<std::uintptr_t>(get_abi(window));
    if (!identity) return false;
    bool alreadyObserved = false;
    auto write = g_transportWindowObservers.begin();
    for (auto read = g_transportWindowObservers.begin();
         read != g_transportWindowObservers.end(); ++read) {
        auto live = read->get();
        if (!live) continue;
        if (reinterpret_cast<std::uintptr_t>(get_abi(live)) == identity) {
            alreadyObserved = true;
        }
        if (write != read) *write = *read;
        ++write;
    }
    g_transportWindowObservers.erase(write, g_transportWindowObservers.end());
    if (alreadyObserved) return false;
    g_transportWindowObservers.push_back(winrt::make_weak(window));
    return true;
}

std::uintptr_t DependencyObjectIdentity(const Xaml::DependencyObject& node) {
    return node ? reinterpret_cast<std::uintptr_t>(get_abi(node)) : 0;
}

Controls::Slider ResolveOwningSlider(const Xaml::DependencyObject& node) {
    auto current = node;
    for (unsigned depth = 0; current && depth < 16; ++depth) {
        if (auto slider = current.try_as<Controls::Slider>()) return slider;
        try {
            current = Media::VisualTreeHelper::GetParent(current);
        } catch (...) {
            break;
        }
    }
    return nullptr;
}

unsigned CountToggleDescendants(const Xaml::DependencyObject& node) {
    if (!node) return 0;
    unsigned score = node.try_as<Controls::ToggleSwitch>() ? 1U : 0U;
    try {
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            score += CountToggleDescendants(Media::VisualTreeHelper::GetChild(node, index));
        }
    } catch (...) {
    }
    return score;
}

void FindBestStackPanel(const Xaml::DependencyObject& node, Controls::StackPanel& best,
                        unsigned& bestScore) {
    if (!node) return;
    try {
        if (auto panel = node.try_as<Controls::StackPanel>()) {
            const unsigned score = CountToggleDescendants(panel);
            if (score > bestScore) {
                best = panel;
                bestScore = score;
            }
        }
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            FindBestStackPanel(Media::VisualTreeHelper::GetChild(node, index), best, bestScore);
        }
    } catch (...) {
    }
}

Controls::ToggleSwitch FindLastNativeToggle(const Xaml::DependencyObject& node) {
    Controls::ToggleSwitch last{nullptr};
    if (!node) return last;
    try {
        if (auto toggle = node.try_as<Controls::ToggleSwitch>()) {
            if (toggle.Name() != L"AMExclusiveModeToggle") last = toggle;
        }
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            if (auto child = FindLastNativeToggle(
                    Media::VisualTreeHelper::GetChild(node, index))) {
                last = child;
            }
        }
    } catch (...) {
    }
    return last;
}

Controls::Border FindRenderedCardBorder(const Controls::ToggleSwitch& toggle,
                                        const Xaml::DependencyObject& stopAt) {
    Controls::Border best{nullptr};
    double bestArea{};
    if (!toggle) return best;
    try {
        Xaml::DependencyObject current = toggle;
        while (current && current != stopAt) {
            if (auto border = current.try_as<Controls::Border>()) {
                const double area = border.ActualWidth() * border.ActualHeight();
                if (border.Background() && area > bestArea &&
                    border.ActualWidth() > toggle.ActualWidth() * 3.0) {
                    best = border;
                    bestArea = area;
                }
            }
            current = Media::VisualTreeHelper::GetParent(current);
        }
    } catch (...) {
    }
    return best;
}

Controls::TextBlock FindStatusText(const Controls::ToggleSwitch& toggle,
                                  std::wstring_view name = L"AMExclusiveModeStatus") {
    if (!toggle) return nullptr;
    try {
        auto current = Media::VisualTreeHelper::GetParent(toggle);
        while (current) {
            if (auto found = FindByName(current, name)
                                 .try_as<Controls::TextBlock>()) {
                return found;
            }
            current = Media::VisualTreeHelper::GetParent(current);
        }
    } catch (...) {
    }
    return nullptr;
}

Media::Brush ThemeBrush(std::wstring_view key) {
    try {
        auto application = Xaml::Application::Current();
        if (!application) return nullptr;
        return application.Resources().TryLookup(box_value(hstring(key))).try_as<Media::Brush>();
    } catch (...) {
        return nullptr;
    }
}

void CollectTextBlocks(const Xaml::DependencyObject& node,
                       std::vector<Controls::TextBlock>& blocks) {
    if (!node) return;
    try {
        if (auto text = node.try_as<Controls::TextBlock>()) blocks.push_back(text);
        const int count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index) {
            CollectTextBlocks(Media::VisualTreeHelper::GetChild(node, index), blocks);
        }
    } catch (...) {
    }
}

void ApplyStatus(const Controls::ToggleSwitch& toggle) {
    const auto status = StatusSnapshot();
    const auto prior = g_lastLoggedGeneration.exchange(status.generation);
    if (prior != status.generation) {
        std::wostringstream line;
        line << L"status generation=" << status.generation
             << L" enabled=" << static_cast<unsigned>(status.enabledIntent)
             << L" state=" << static_cast<unsigned>(status.state)
             << L" error=" << static_cast<unsigned>(status.error)
             << L" hr=0x" << std::hex << static_cast<unsigned long>(status.hresult);
        Log(line.str());
    }
    g_suppressToggle.store(true);
    const bool desiredOn = status.enabledIntent != 0;
    if (toggle.IsOn() != desiredOn) {
        g_programmaticToggleValue.store(desiredOn ? 1 : 0);
        toggle.IsOn(desiredOn);
    }
    const bool transition = status.state == RuntimeState::Probing ||
                            status.state == RuntimeState::Initializing ||
                            status.state == RuntimeState::AlignRetry ||
                            status.state == RuntimeState::DisablePending;
    toggle.IsEnabled(!transition);
    std::wstring presentation;
    if (status.state == RuntimeState::Active) {
        presentation = Localized(L"已启用", L"Enabled");
        if (status.endpointName[0]) presentation += L" · " + std::wstring(status.endpointName);
    } else if (status.state == RuntimeState::DisablePending) {
        presentation = Localized(L"正在恢复共享输出…", L"Restoring shared output…");
    } else if (status.enabledIntent && (status.error == ErrorCategory::HookLoadFailed ||
                                      status.error == ErrorCategory::ProtocolVersionMismatch)) {
        // Surface a component failure while preserving the user's intent;
        // "Waiting for playback" would hide the required repair.
        presentation = ErrorText(status.error);
    } else if (status.state == RuntimeState::Requested ||
               status.state == RuntimeState::WaitingForStream) {
        presentation = Localized(L"已开启 · 等待播放", L"Enabled · Waiting for playback");
    } else if (status.enabledIntent) {
        presentation = Localized(L"正在启用当前默认播放设备…",
                                 L"Enabling the current default playback device…");
    } else {
        presentation = ErrorText(status.error);
    }
    toggle.OnContent(nullptr);
    toggle.OffContent(nullptr);
    if (auto statusText = FindStatusText(toggle)) statusText.Text(presentation);
    g_suppressToggle.store(false);
}

void ApplyOutputFormatStatus(const Controls::Border& card) {
    const auto status = StatusSnapshot();
    const bool visible = status.state == RuntimeState::Active && status.format[0];
    std::wstring presentation;
    if (visible) {
        presentation = status.format;
        const std::wstring_view detail(status.detail);
        if (detail == L"converted")
            presentation += Localized(L" · 已转换格式", L" · Format converted");
        else if (detail == L"resampled-requantized")
            presentation += Localized(L" · 已重采样并降低位深", L" · Resampled and bit depth reduced");
        else if (detail == L"requantized")
            presentation += Localized(L" · 已降低位深", L" · Bit depth reduced");
        else if (detail == L"resampled")
            presentation += Localized(L" · 已重采样", L" · Resampled");
        else presentation += L" · bit-perfect";
    }
    // Read the logical children so the text also updates while the card is collapsed.
    if (auto labels = card.Child().try_as<Controls::StackPanel>()) {
        for (auto child : labels.Children()) {
            if (auto text = child.try_as<Controls::TextBlock>();
                text && text.Name() == L"AMExclusiveOutputFormat") {
                text.Text(presentation);
                break;
            }
        }
    }
    card.Visibility(visible ? Xaml::Visibility::Visible : Xaml::Visibility::Collapsed);
}

void ApplyResamplingStatus(const Controls::ToggleSwitch& toggle) {
    if (!toggle) return;
    const int requested = g_requestedAllowResampling.load();
    const bool enabled = requested >= 0 ? requested != 0 : StatusSnapshot().allowResampling != 0;
    g_suppressResamplingToggle.store(true);
    if (toggle.IsOn() != enabled) {
        g_programmaticResamplingValue.store(enabled ? 1 : 0);
        toggle.IsOn(enabled);
    }
    if (auto statusText = FindStatusText(toggle, L"AMExclusiveResamplingStatus")) {
        const auto text = requested >= 0
            ? Localized(L"正在保存…", L"Saving…") : std::wstring{};
        statusText.Text(text);
        statusText.Visibility(text.empty() ? Xaml::Visibility::Collapsed : Xaml::Visibility::Visible);
    }
    g_suppressResamplingToggle.store(false);
}

void ApplyHardwareBufferStatus(const Controls::TextBox& input) {
    if (!input) return;
    const bool empty = input.Text().empty();
    if (!empty && (g_hardwareBufferInputDirty.load() ||
                   input.FocusState() != Xaml::FocusState::Unfocused)) return;
    const int requested = g_requestedHardwareBufferMs.load();
    const auto value = requested >= 0
        ? static_cast<std::uint32_t>(requested)
        : StatusSnapshot().hardwareBufferMs;
    const auto text = std::to_wstring(value ? value : 20u);
    if (input.Text() != text) {
        g_hardwareBufferTextUpdating.store(true);
        input.Text(text);
        g_hardwareBufferTextUpdating.store(false);
    }
}

void CommitHardwareBuffer(const Controls::TextBox& input) {
    if (!input) return;
    const std::wstring text = input.Text().c_str();
    wchar_t* end{};
    const auto value = wcstoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != L'\0' || value < 1 || value > 100) {
        Log(L"hardware buffer edit rejected value=" + text);
        const auto saved = StatusSnapshot().hardwareBufferMs;
        g_hardwareBufferInputDirty.store(false);
        g_hardwareBufferTextUpdating.store(true);
        input.Text(std::to_wstring(saved ? saved : 20u));
        g_hardwareBufferTextUpdating.store(false);
        input.Focus(Xaml::FocusState::Programmatic);
        input.SelectAll();
        return;
    }
    const auto milliseconds = static_cast<std::uint32_t>(value);
    if (milliseconds == StatusSnapshot().hardwareBufferMs &&
        g_requestedHardwareBufferMs.load() < 0) {
        g_hardwareBufferInputDirty.store(false);
        return;
    }
    if (!SendHardwareBuffer(milliseconds)) {
        Log(L"hardware buffer edit could not reach broker");
        ApplyHardwareBufferStatus(input);
        return;
    }
    Log(L"hardware buffer edit queued milliseconds=" +
        std::to_wstring(milliseconds));
}

struct UiHostWatch {
    ObserverRef node;
    Xaml::DependencyProperty property{nullptr};
    std::int64_t propertyToken{};
    event_token loaded{}, unloaded{};
    std::uintptr_t identity{};
};

struct UiWindowContext {
    WindowObserverRef window;
    Microsoft::UI::Dispatching::DispatcherQueue dispatcher{nullptr};
    std::wstring propertyName;
    ammod::UiRefreshState requests;
    std::atomic<bool> closed{};
    std::atomic<bool> routesDirty{true}, transportDirty{true};
    std::atomic<bool> playbackDirty{true};
    // Everything below is confined to dispatcher, including weak resolution.
    ApplePlayback::Player player{nullptr};
    ApplePlayback::PlayerState playerState{nullptr};
    ApplePlayback::NowPlayingItem playingItem{nullptr};
    event_token playerChanged{}, playerStateChanged{};
    event_token playingItemChanged{};
    ammod::ui::PlaybackState lastPlaybackState{ammod::ui::PlaybackState::Unknown};
    std::uint64_t playbackSync{};
    std::uint64_t lastPlaybackItem{};
    std::int32_t playbackError{};
    ObserverRef root, page;
    std::uint64_t rootRevision{};
    ULONGLONG pageObservedTick{};
    bool pageMounted{};
    bool windowObserved{};
    bool ownerListed{};
    std::vector<UiHostWatch> hosts;
    std::unordered_set<std::uintptr_t> hostIds;
    event_token layout{};
    Windows::Foundation::IInspectable tapped{nullptr}, keyDown{nullptr};
    unsigned routeNodes{}, refreshCount{}, routeWalkCount{}, transportWalkCount{};
    ULONGLONG longestRefreshMs{}, lastDiagnosticTick{};
};

std::mutex g_uiWindowsMutex;
std::vector<std::shared_ptr<UiWindowContext>> g_uiWindows;
thread_local std::vector<std::weak_ptr<UiWindowContext>> g_threadUiWindows;
// Only the current MainWindow observes Apple's process-wide Player. A closing
// predecessor may revoke its own observation, never its replacement's.
std::atomic<UiWindowContext*> g_playbackObserverOwner{};
std::atomic<std::uint64_t> g_playbackSync{};
void RefreshUiWindow(const std::shared_ptr<UiWindowContext>& context);

void QueueUiRefresh(const std::shared_ptr<UiWindowContext>& context) {
    const auto ticket = context->requests.Request();
    if (!ticket) return;
    try {
        if (context->dispatcher.TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
            [context, ticket] {
                if (!context->requests.Take(ticket)) return;
                try {
                    RefreshUiWindow(context);
                } catch (const hresult_error& error) {
                    Log(L"window UI refresh failed hr=" +
                        std::to_wstring(static_cast<std::uint32_t>(error.code())));
                } catch (...) {
                    Log(L"window UI refresh failed with unknown exception");
                }
            })) return;
    } catch (...) {
        // A dispatcher may be shutting down while a WinEvent requests work.
    }
    (void)context->requests.Take(ticket);
}

void QueuePlaybackState(const std::weak_ptr<UiWindowContext>& weak) noexcept {
    try {
        if (auto context = weak.lock(); context && !context->closed.load()) {
            context->playbackDirty.store(true);
            QueueUiRefresh(context);
        }
    } catch (...) {}
}

std::uint64_t PlaybackRevision() noexcept {
    LARGE_INTEGER revision{};
    return QueryPerformanceCounter(&revision) && revision.QuadPart > 0
        ? static_cast<std::uint64_t>(revision.QuadPart) : 0;
}

void RevokePlaybackObserver(const std::shared_ptr<UiWindowContext>& context) noexcept {
    auto owner = context.get();
    if (g_playbackObserverOwner.compare_exchange_strong(owner, nullptr)) {
        try {
            if (const auto revision = PlaybackRevision())
                QueueTransport(L"playback_unavailable", revision);
        } catch (...) {}
    }
    context->lastPlaybackState = ammod::ui::PlaybackState::Unknown;
}

void ClearPlaybackObserver(const std::shared_ptr<UiWindowContext>& context) noexcept {
    RevokePlaybackObserver(context);
    try {
        if (context->playerState) context->playerState.PropertyChanged(context->playerStateChanged);
    } catch (...) {}
    try {
        if (context->player) context->player.PropertyChanged(context->playerChanged);
    } catch (...) {}
    try {
        if (context->playingItem) context->playingItem.PropertyChanged(context->playingItemChanged);
    } catch (...) {}
    context->playingItem = nullptr;
    context->playerState = nullptr;
    context->player = nullptr;
}

void RefreshPlaybackState(const std::shared_ptr<UiWindowContext>& context) noexcept {
    try {
        // Never construct another Player or call Play/Pause. Read Apple's
        // existing logical player on this Window's owning dispatcher.
        auto player = ApplePlayback::Player::Current();
        if (!player) { ClearPlaybackObserver(context); return; }
        const std::weak_ptr<UiWindowContext> weak = context;
        if (player != context->player) {
            ClearPlaybackObserver(context);
            context->player = player;
            context->playerChanged = player.PropertyChanged(
                [weak](auto&&, Xaml::Data::PropertyChangedEventArgs const& args) noexcept {
                    try {
                        if (args.PropertyName().empty() || args.PropertyName() == L"PlayerState" ||
                            args.PropertyName() == L"NowPlayingItem")
                            QueuePlaybackState(weak);
                    } catch (...) {}
                });
        }
        auto state = player.PlayerState();
        if (state != context->playerState) {
            if (context->playerState)
                context->playerState.PropertyChanged(context->playerStateChanged);
            context->playerState = state;
            if (state) {
                context->playerStateChanged = state.PropertyChanged(
                    [weak](auto&&, auto&&) noexcept { QueuePlaybackState(weak); });
            }
        }
        if (!state) {
            RevokePlaybackObserver(context);
            return;
        }
        auto item = player.NowPlayingItem();
        if (item != context->playingItem) {
            if (context->playingItem)
                context->playingItem.PropertyChanged(context->playingItemChanged);
            context->playingItem = item;
            if (item) {
                context->playingItemChanged = item.PropertyChanged(
                    [weak](auto&&, Xaml::Data::PropertyChangedEventArgs const& args) noexcept {
                        try {
                            if (args.PropertyName().empty() || args.PropertyName() == L"PlayQueueItemID")
                                QueuePlaybackState(weak);
                        } catch (...) {}
                    });
            }
        }
        const auto queueItem = item ? item.PlayQueueItemID() : 0;
        const bool playing = state.IsPlaying(), paused = state.IsPaused();
        const auto observed = ammod::ui::ClassifyPlaybackState(playing, paused);
        g_playbackObserverOwner.store(context.get());
        context->playbackError = 0;
        const auto sync = g_playbackSync.load();
        if (observed == ammod::ui::PlaybackState::Unknown ||
            (observed == context->lastPlaybackState && queueItem == context->lastPlaybackItem &&
             sync == context->playbackSync)) return;
        // QPC is monotonic across processes in the same boot, so a new UI
        // process cannot reset revisions below those retained by a live Agent.
        const auto revision = PlaybackRevision();
        if (!revision) return;
        QueueTransport(std::wstring(observed == ammod::ui::PlaybackState::Playing
            ? L"playback_playing:" : L"playback_paused:") + std::to_wstring(queueItem),
            revision);
        context->lastPlaybackState = observed;
        context->lastPlaybackItem = queueItem;
        context->playbackSync = sync;
    } catch (const hresult_error& error) {
        ClearPlaybackObserver(context);
        if (context->playbackError != error.code().value) {
            context->playbackError = error.code().value;
            Log(L"Apple player observation failed hr=" +
                std::to_wstring(static_cast<std::uint32_t>(error.code())));
        }
    } catch (...) {
        ClearPlaybackObserver(context);
    }
}

void SchedulePlaybackStateSync() {
    ++g_playbackSync;
    std::lock_guard lock(g_uiWindowsMutex);
    for (const auto& context : g_uiWindows) {
        if (context->propertyName == L"MainWindow") QueuePlaybackState(context);
    }
}

void WakeUiWindow(const std::weak_ptr<UiWindowContext>& weak,
                  std::uint64_t rootRevision) noexcept {
    try {
        if (auto context = weak.lock(); context && !context->closed.load() &&
            context->rootRevision == rootRevision) {
            context->requests.ArmLayout(GetTickCount64());
            context->routesDirty.store(true);
            context->transportDirty.store(true);
            QueueUiRefresh(context);
        }
    } catch (...) {
        // XAML event callbacks must not throw into the host application.
    }
}

void ClearUiRoot(const std::shared_ptr<UiWindowContext>& context) {
    ++context->rootRevision; // In-flight old-root events lose authority first.
    context->requests.StopLayout();
    for (const auto& watch : context->hosts) {
        try {
            if (auto node = watch.node.get()) {
                if (watch.property)
                    node.UnregisterPropertyChangedCallback(watch.property, watch.propertyToken);
                if (auto element = node.try_as<Xaml::FrameworkElement>()) {
                    element.Loaded(watch.loaded);
                    element.Unloaded(watch.unloaded);
                }
            }
        } catch (...) {}
    }
    context->hosts.clear();
    context->hostIds.clear();
    if (auto page = context->page.get().try_as<Xaml::FrameworkElement>()) {
        try { page.LayoutUpdated(context->layout); } catch (...) {}
    }
    if (auto root = context->root.get().try_as<Xaml::FrameworkElement>()) {
        try {
            if (context->tapped)
                root.RemoveHandler(Xaml::UIElement::TappedEvent(), context->tapped);
            if (context->keyDown)
                root.RemoveHandler(Xaml::UIElement::KeyDownEvent(), context->keyDown);
        } catch (...) {}
    }
    context->root = {};
    context->page = {};
    context->tapped = nullptr;
    context->keyDown = nullptr;
    context->pageMounted = false;
}

void ObserveUiHosts(const Xaml::DependencyObject& root,
                    const std::shared_ptr<UiWindowContext>& context,
                    Xaml::DependencyObject& playbackPage) {
    context->routeNodes = static_cast<unsigned>(ammod::WalkUiRoutes(root,
        [](const auto& node) { return reinterpret_cast<std::uintptr_t>(get_abi(node)); },
        [&](const Xaml::DependencyObject& node, auto logical, auto visual) {
        // Scrolling/virtualization is not navigation. Never subscribe to, or
        // descend into, library rows, buttons or our own cards.
        if (node.try_as<Controls::ItemsControl>() || node.try_as<Controls::ItemsRepeater>() ||
            node.try_as<Primitives::ButtonBase>()) return;
        auto element = node.try_as<Xaml::FrameworkElement>();
        if (element && std::wstring_view(element.Name()).starts_with(L"AMExclusive")) return;
        auto page = node.try_as<Controls::Page>();
        const bool isPage = page && std::wstring_view(get_class_name(node)).find(
            L"PlaybackSettingsPage") != std::wstring_view::npos;
        if (isPage) playbackPage = node;
        auto frame = node.try_as<Controls::Frame>();
        auto userControl = node.try_as<Controls::UserControl>();
        const bool isRoot = node == root;
        Xaml::DependencyProperty property{nullptr};
        Xaml::DependencyObject logicalChild{nullptr};
        if (frame) {
            property = Controls::ContentControl::ContentProperty();
            logicalChild = frame.Content().try_as<Xaml::DependencyObject>();
        } else if (userControl) {
            property = Controls::UserControl::ContentProperty();
            logicalChild = userControl.Content();
        } else if (auto content = node.try_as<Controls::ContentControl>()) {
            auto child = content.Content().try_as<Xaml::DependencyObject>();
            if (isRoot || (child && child.try_as<Controls::Page>())) {
                property = Controls::ContentControl::ContentProperty();
                logicalChild = child;
            }
        }
        const auto identity = reinterpret_cast<std::uintptr_t>(get_abi(node));
        // O(1) lookup replaces repeated weak-resolution scans of every host.
        // Reserve the final observer slot for the actual settings page.
        if (element && (frame || userControl || page || isRoot || property) &&
            context->hosts.size() < (isPage ? 64u : 63u) &&
            !context->hostIds.contains(identity)) {
            const std::weak_ptr<UiWindowContext> weak = context;
            const auto revision = context->rootRevision;
            // Loaded does not bubble. Subscribe to the actual Page/container.
            UiHostWatch watch{make_weak(node), property};
            watch.identity = identity;
            watch.loaded = element.Loaded([weak, revision](auto&&, auto&&) noexcept {
                WakeUiWindow(weak, revision);
            });
            watch.unloaded = element.Unloaded([weak, revision, isPage](auto&&, auto&&) noexcept {
                if (auto live = weak.lock(); live && live->rootRevision == revision && isPage)
                    live->pageMounted = false;
                WakeUiWindow(weak, revision);
            });
            if (property) {
                watch.propertyToken = node.RegisterPropertyChangedCallback(property,
                    [weak, revision](auto&&, auto&&) noexcept { WakeUiWindow(weak, revision); });
            }
            context->hosts.push_back(std::move(watch));
            context->hostIds.insert(identity);
        }
        if (isPage) return; // Its native control/template tree is not routing.
        if (logicalChild) { logical(logicalChild); return; }
        const auto count = Media::VisualTreeHelper::GetChildrenCount(node);
        for (int index = 0; index < count; ++index)
            if (!visual(Media::VisualTreeHelper::GetChild(node, index))) break;
    }));
}

void RefreshUiWindow(const std::shared_ptr<UiWindowContext>& context) {
    if (context->closed.load() || !context->dispatcher.HasThreadAccess()) return;
    auto window = context->window.get();
    if (!window || !window.DispatcherQueue().HasThreadAccess()) return;
    if (GetXamlWindowProperty(context->propertyName) != window) {
        ClearPlaybackObserver(context);
        ClearUiRoot(context);
        return; // A queued callback may belong to a replaced Window property.
    }
    const auto refreshStart = GetTickCount64();
    bool discover = context->routesDirty.exchange(false);
    bool transport = context->transportDirty.exchange(false);
    if (!context->ownerListed) {
        g_threadUiWindows.emplace_back(context);
        context->ownerListed = true;
    }
    if (!context->windowObserved) {
        const std::weak_ptr<UiWindowContext> weak = context;
        window.Activated([weak](auto&&, auto&&) noexcept {
            if (auto live = weak.lock()) WakeUiWindow(weak, live->rootRevision);
        });
        window.VisibilityChanged([weak](auto&&, auto&&) noexcept {
            if (auto live = weak.lock()) WakeUiWindow(weak, live->rootRevision);
        });
        window.Closed([weak](auto&&, auto&&) noexcept {
            if (auto live = weak.lock()) {
                live->closed.store(true);
                live->requests.Close();
                ClearPlaybackObserver(live);
                try { ClearUiRoot(live); } catch (...) {}
                try {
                    if (live->propertyName == L"MainWindow") {
                        std::lock_guard lock(g_uiTargetMutex);
                        if (g_settingsWindow.get() == live->window.get()) {
                            if (g_statusRefreshTimer) g_statusRefreshTimer.Stop();
                            g_statusRefreshTimer = nullptr;
                            g_toggle = {}; g_hardwareBufferBox = {};
                            g_resamplingToggle = {}; g_outputFormatCard = {};
                            g_dispatcher = nullptr;
                            g_settingsWindow = {};
                        }
                    }
                } catch (...) {}
            }
        });
        context->windowObserved = true;
        Log(L"UI dispatcher bound window=" + context->propertyName);
    }
    const bool main = context->propertyName == L"MainWindow";
    if (main && (context->playbackDirty.exchange(false) || discover ||
                 g_playbackObserverOwner.load() != context.get()))
        RefreshPlaybackState(context);
    if (main) {
        Microsoft::UI::Dispatching::DispatcherQueue oldDispatcher{nullptr};
        Microsoft::UI::Dispatching::DispatcherQueueTimer oldTimer{nullptr};
        {
            std::lock_guard lock(g_uiTargetMutex);
            if (g_dispatcher != context->dispatcher) {
                oldDispatcher = g_dispatcher;
                oldTimer = g_statusRefreshTimer;
                g_statusRefreshTimer = nullptr;
                g_toggle = {}; g_hardwareBufferBox = {};
                g_resamplingToggle = {}; g_outputFormatCard = {};
                g_dispatcher = context->dispatcher;
            }
            g_settingsWindow = context->window;
        }
        if (oldDispatcher && oldTimer)
            oldDispatcher.TryEnqueue([oldTimer]() noexcept {
                try { oldTimer.Stop(); } catch (...) {}
            });
    }
    auto content = window.Content().try_as<Xaml::DependencyObject>();
    if (content != context->root.get()) {
        discover = transport = true;
        ClearUiRoot(context);
        context->root = content ? make_weak(content) : ObserverRef{};
        if (auto root = content.try_as<Xaml::FrameworkElement>()) {
            const std::weak_ptr<UiWindowContext> weak = context;
            const auto revision = context->rootRevision;
            context->tapped = box_value(Input::TappedEventHandler(
                [weak, revision](auto&&, auto&&) noexcept { WakeUiWindow(weak, revision); }));
            context->keyDown = box_value(Input::KeyEventHandler(
                [weak, revision](auto&&, auto&&) noexcept { WakeUiWindow(weak, revision); }));
            root.AddHandler(Xaml::UIElement::TappedEvent(), context->tapped, true);
            root.AddHandler(Xaml::UIElement::KeyDownEvent(), context->keyDown, true);
            context->requests.ArmLayout(GetTickCount64());
        }
    }
    if (!content) return;
    Xaml::DependencyObject page = context->page.get();
    if (discover) {
        std::erase_if(context->hosts, [&context](const auto& watch) {
            if (watch.node.get()) return false;
            context->hostIds.erase(watch.identity);
            return true;
        });
        page = nullptr;
        ObserveUiHosts(content, context, page);
        ++context->routeWalkCount;
    }
    if (main && page != context->page.get()) {
        if (auto oldPage = context->page.get().try_as<Xaml::FrameworkElement>())
            oldPage.LayoutUpdated(context->layout);
        context->page = page ? make_weak(page) : ObserverRef{};
        context->pageMounted = false;
        context->pageObservedTick = GetTickCount64();
        if (auto element = page.try_as<Xaml::FrameworkElement>()) {
            const std::weak_ptr<UiWindowContext> weak = context;
            const auto weakPage = make_weak(page);
            const auto revision = context->rootRevision;
            context->layout = element.LayoutUpdated(
                [weak, weakPage, revision](auto&&, auto&&) noexcept {
                    try {
                        if (auto live = weak.lock(); live && live->rootRevision == revision &&
                            live->page.get() == weakPage.get() &&
                            live->requests.OnLayout(GetTickCount64())) QueueUiRefresh(live);
                    } catch (...) {}
                });
            context->requests.ArmLayout(GetTickCount64());
            Log(L"PlaybackSettingsPage observed; awaiting loaded/template readiness");
        }
    }
    if (main && page && !context->pageMounted) {
        auto element = page.try_as<Xaml::FrameworkElement>();
        if (element && element.IsLoaded() && AttachOrRefreshToggle(page)) {
            context->pageMounted = true;
            context->requests.StopLayout();
            Log(L"PlaybackSettingsPage cards ready observedToMountMs=" +
                std::to_wstring(GetTickCount64() - context->pageObservedTick));
        }
    }
    if (main && context->pageMounted) context->requests.StopLayout();
    if (transport) {
        AttachTransportObservers(window, context->propertyName);
        ++context->transportWalkCount;
    }
    ++context->refreshCount;
    context->longestRefreshMs = std::max(context->longestRefreshMs, GetTickCount64() - refreshStart);
    const auto now = GetTickCount64();
    if (now - context->lastDiagnosticTick >= 5000) {
        Log(L"UI work window=" + context->propertyName +
            L" refreshes=" + std::to_wstring(context->refreshCount) +
            L" routeScans=" + std::to_wstring(context->routeWalkCount) +
            L" transportScans=" + std::to_wstring(context->transportWalkCount) +
            L" routeNodes=" + std::to_wstring(context->routeNodes) +
            L" hosts=" + std::to_wstring(context->hosts.size()) +
            L" maxRefreshMs=" + std::to_wstring(context->longestRefreshMs));
        context->lastDiagnosticTick = now;
        context->refreshCount = context->routeWalkCount = context->transportWalkCount = 0;
        context->longestRefreshMs = 0;
    }
}

void ScheduleUiDiscovery() {
    // Window.DispatcherQueue is documented as callable from any thread. No
    // XAML content/control is read until the callback reaches that queue.
    try {
        for (const auto& [propertyName, window] : GetTransportXamlWindows()) {
            auto dispatcher = window.DispatcherQueue();
            if (!dispatcher) continue;
            std::shared_ptr<UiWindowContext> context;
            {
                std::lock_guard lock(g_uiWindowsMutex);
                std::erase_if(g_uiWindows, [](const auto& entry) {
                    return !entry->window.get();
                });
                for (const auto& entry : g_uiWindows) {
                    if (entry->window.get() == window) { context = entry; break; }
                }
                if (!context) {
                    context = std::make_shared<UiWindowContext>();
                    context->window = make_weak(window);
                    context->dispatcher = dispatcher;
                    context->propertyName = propertyName;
                    g_uiWindows.push_back(context);
                }
            }
            context->routesDirty.store(true);
            context->transportDirty.store(true);
            QueueUiRefresh(context);
        }
    } catch (const hresult_error& error) {
        Log(L"UI dispatcher discovery failed hr=" +
            std::to_wstring(static_cast<std::uint32_t>(error.code())));
    } catch (...) {
        Log(L"UI dispatcher discovery failed with unknown exception");
    }
}

void RememberToggle(const Controls::ToggleSwitch& toggle) {
    std::lock_guard lock(g_uiTargetMutex);
    g_toggle = make_weak(toggle);
    auto current = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    g_dispatcher = current ? current : toggle.DispatcherQueue();
    if (!g_statusRefreshTimer && g_dispatcher) {
        g_statusRefreshTimer = g_dispatcher.CreateTimer();
        g_statusRefreshTimer.Interval(std::chrono::milliseconds(250));
        g_statusRefreshTimer.IsRepeating(true);
        g_statusRefreshTimer.Tick([](auto&&, auto&&) {
            // Handlers run inside Apple's XAML frame. A failure here must stay
            // inside this DLL; it must never surface as an unhandled error in
            // the host's dispatcher.
            try {
                winrt::weak_ref<Controls::ToggleSwitch> target;
                winrt::weak_ref<Controls::TextBox> hardwareBuffer;
                winrt::weak_ref<Controls::ToggleSwitch> resampling;
                winrt::weak_ref<Controls::Border> outputFormat;
                {
                    std::lock_guard targetLock(g_uiTargetMutex);
                    if (!g_dispatcher || !g_dispatcher.HasThreadAccess()) return;
                    target = g_toggle;
                    hardwareBuffer = g_hardwareBufferBox;
                    resampling = g_resamplingToggle;
                    outputFormat = g_outputFormatCard;
                }
                if (auto control = target.get()) ApplyStatus(control);
                if (auto input = hardwareBuffer.get()) ApplyHardwareBufferStatus(input);
                if (auto control = resampling.get()) ApplyResamplingStatus(control);
                if (auto card = outputFormat.get()) ApplyOutputFormatStatus(card);
            } catch (const winrt::hresult_error& error) {
                // The Apply* helpers raise their suppress flags first and
                // lower them last; a throw in between would leave user
                // toggles ignored until the next successful tick.
                g_suppressToggle.store(false);
                g_suppressResamplingToggle.store(false);
                Log(L"status refresh tick failed hresult=" +
                    std::to_wstring(static_cast<std::uint32_t>(error.code())));
            } catch (...) {
                g_suppressToggle.store(false);
                g_suppressResamplingToggle.store(false);
                Log(L"status refresh tick failed with a non-WinRT exception");
            }
        });
        g_statusRefreshTimer.Start();
        Log(L"UI status refresh timer started intervalMs=250");
    }
}

void RememberHardwareBufferBox(const Controls::TextBox& input) {
    std::lock_guard lock(g_uiTargetMutex);
    if (!g_hardwareBufferBox.get()) g_hardwareBufferInputDirty.store(false);
    g_hardwareBufferBox = make_weak(input);
    auto current = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    if (!g_dispatcher) g_dispatcher = current ? current : input.DispatcherQueue();
}

void RememberResamplingToggle(const Controls::ToggleSwitch& toggle) {
    std::lock_guard lock(g_uiTargetMutex);
    g_resamplingToggle = make_weak(toggle);
}

void RememberOutputFormatCard(const Controls::Border& card) {
    std::lock_guard lock(g_uiTargetMutex);
    g_outputFormatCard = make_weak(card);
}

void ScheduleStatusRefresh() {
    winrt::weak_ref<Controls::ToggleSwitch> toggle;
    winrt::weak_ref<Controls::TextBox> hardwareBuffer;
    winrt::weak_ref<Controls::ToggleSwitch> resampling;
    winrt::weak_ref<Controls::Border> outputFormat;
    Microsoft::UI::Dispatching::DispatcherQueue dispatcher{nullptr};
    {
        std::lock_guard lock(g_uiTargetMutex);
        toggle = g_toggle;
        hardwareBuffer = g_hardwareBufferBox;
        resampling = g_resamplingToggle;
        outputFormat = g_outputFormatCard;
        dispatcher = g_dispatcher;
    }
    if (dispatcher) {
        dispatcher.TryEnqueue([toggle, hardwareBuffer, resampling, outputFormat] {
            try {
                if (auto control = toggle.get()) ApplyStatus(control);
                if (auto input = hardwareBuffer.get()) ApplyHardwareBufferStatus(input);
                if (auto control = resampling.get()) ApplyResamplingStatus(control);
                if (auto card = outputFormat.get()) ApplyOutputFormatStatus(card);
            } catch (const winrt::hresult_error& error) {
                g_suppressToggle.store(false);
                g_suppressResamplingToggle.store(false);
                Log(L"scheduled status refresh failed hresult=" +
                    std::to_wstring(static_cast<std::uint32_t>(error.code())));
            } catch (...) {
                g_suppressToggle.store(false);
                g_suppressResamplingToggle.store(false);
                Log(L"scheduled status refresh failed with a non-WinRT exception");
            }
        });
    }
}

void ScheduleTransportObserverAttach() {
    // Input events already know the current apartment. Reuse its contexts;
    // do not rediscover every Window or walk settings on each pointer event.
    std::erase_if(g_threadUiWindows, [](const auto& weak) { return weak.expired(); });
    for (const auto& weak : g_threadUiWindows) {
        if (auto context = weak.lock(); context && !context->closed.load()) {
            context->transportDirty.store(true);
            QueueUiRefresh(context);
        }
    }
}

void AttachTransportWindowLifecycle(const Xaml::Window& window) {
    if (!window || !RememberTransportWindow(window)) return;
    window.Activated([](auto&&, auto&&) { ScheduleTransportObserverAttach(); });
    window.VisibilityChanged([](auto&&, auto&&) { ScheduleTransportObserverAttach(); });
    window.Closed([](auto&&, auto&&) {
        CommitSeekGesture(0, L"window-closed");
        ScheduleTransportObserverAttach();
    });
}

void AttachTransportPopupLifecycle(const Primitives::Popup& popup) {
    if (!popup || !RememberObserver(g_transportPopupObservers, popup)) return;
    popup.Opened([](auto&&, auto&&) { ScheduleTransportObserverAttach(); });
    popup.Closed([](auto&&, auto&&) {
        CommitSeekGesture(0, L"popup-closed");
        ScheduleTransportObserverAttach();
    });
}

void AttachTransportObservers(const Xaml::Window& window, const std::wstring& windowProperty) {
    try {
        if (!window || !window.DispatcherQueue().HasThreadAccess()) return;
        {
            auto content = window.Content().try_as<Xaml::DependencyObject>();
            if (!content) return;
            AttachTransportWindowLifecycle(window);

            // Preserve the original MainWindow direct-selection behavior while
            // broadening discovery to every live transport presentation.
            if (auto pointerRoot = content.try_as<Xaml::UIElement>();
                pointerRoot && RememberObserver(g_trackListPointerRootObservers, pointerRoot)) {
                pointerRoot.AddHandler(
                    Xaml::UIElement::PointerPressedEvent(),
                    box_value(Input::PointerEventHandler(
                        [](auto&&, Input::PointerRoutedEventArgs const& args) {
                            ScheduleTransportObserverAttach();
                            auto current = args.OriginalSource().try_as<Xaml::DependencyObject>();
                            for (unsigned depth = 0; current && depth < 16; ++depth) {
                                if (auto row = current.try_as<Controls::ListViewItem>()) {
                                    const auto rowIdentity = DependencyObjectIdentity(row);
                                    const auto now = GetTickCount64();
                                    const bool secondPress = rowIdentity != 0 &&
                                        rowIdentity == g_lastTrackRowPressIdentity &&
                                        now >= g_lastTrackRowPressTick &&
                                        now - g_lastTrackRowPressTick <= 650;
                                    if (secondPress) {
                                        const auto epoch = g_lastTrackRowPressEpoch;
                                        g_lastTrackRowPressIdentity = 0;
                                        g_lastTrackRowPressEpoch = 0;
                                        g_lastTrackRowPressTick = 0;
                                        if (epoch != 0) QueueTransport(L"skip", epoch);
                                    } else {
                                        g_lastTrackRowPressIdentity = rowIdentity;
                                        g_lastTrackRowPressTick = now;
                                        g_lastTrackRowPressEpoch = 0;
                                        if (StatusSnapshot().state == RuntimeState::Active) {
                                            const auto epoch =
                                                g_transportEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
                                            g_lastTrackRowPressEpoch = epoch;
                                            QueueTransport(L"selection_arm", epoch);
                                        }
                                    }
                                    break;
                                }
                                try {
                                    current = Media::VisualTreeHelper::GetParent(current);
                                } catch (...) {
                                    break;
                                }
                            }
                        })),
                    true);
                pointerRoot.AddHandler(
                    Xaml::UIElement::TappedEvent(),
                    box_value(Input::TappedEventHandler([](auto&&, auto&&) {
                        ScheduleTransportObserverAttach();
                    })), true);
                pointerRoot.AddHandler(
                    Xaml::UIElement::DoubleTappedEvent(),
                    box_value(Input::DoubleTappedEventHandler([](auto&&, auto&&) {
                        ScheduleTransportObserverAttach();
                    })), true);
            }

            std::vector<Xaml::DependencyObject> playPauseNodes;
            CollectByName(content, L"TransportControl_PlayPauseStop", playPauseNodes);
            CollectByAutomationId(content, L"TransportControl_PlayPauseStop", playPauseNodes);
            for (const auto& node : playPauseNodes) {
                if (auto button = node.try_as<Primitives::ButtonBase>();
                    button && RememberObserver(g_transportButtonObservers, button)) {
                    button.Click([](auto&&, auto&&) {
                        if (!g_playbackObserverOwner.load())
                            QueueTransport(L"play_pause", PlaybackRevision());
                    });
                }
            }

            auto attachSkipObservers = [&](std::wstring_view automationId,
                                           std::vector<ObserverRef>& observers) {
                std::vector<Xaml::DependencyObject> nodes;
                CollectByName(content, automationId, nodes);
                CollectByAutomationId(content, automationId, nodes);
                for (const auto& node : nodes) {
                    if (auto button = node.try_as<Primitives::ButtonBase>();
                        button && RememberObserver(observers, button)) {
                        button.AddHandler(
                            Xaml::UIElement::PointerPressedEvent(),
                            box_value(Input::PointerEventHandler([](auto&&, auto&&) {
                                QueueSkipIntent(true);
                            })), true);
                        button.Click([](auto&&, auto&&) { QueueSkipIntent(false); });
                    }
                }
            };
            attachSkipObservers(L"TransportControl_SkipBack", g_skipBackObservers);
            attachSkipObservers(L"TransportControl_SkipForward", g_skipForwardObservers);

            std::vector<Xaml::DependencyObject> scrubberNodes;
            CollectByName(content, L"LCDScrubber", scrubberNodes);
            CollectByAutomationId(content, L"LCDScrubber", scrubberNodes);
            CollectByAutomationId(content, L"Scrubber", scrubberNodes);
            try {
                const auto popups = Media::VisualTreeHelper::GetOpenPopups(window);
                for (const auto& popup : popups) {
                    AttachTransportPopupLifecycle(popup);
                    auto child = popup.Child().try_as<Xaml::DependencyObject>();
                    if (!child) continue;
                    CollectByName(child, L"LCDScrubber", scrubberNodes);
                    CollectByAutomationId(child, L"LCDScrubber", scrubberNodes);
                    CollectByAutomationId(child, L"Scrubber", scrubberNodes);
                }
            } catch (...) {
            }
            if (windowProperty == L"MiniPlayerWindow" && scrubberNodes.empty()) {
                std::vector<Xaml::DependencyObject> sliders;
                CollectSliders(content, sliders);
                if (sliders.size() == 1) AppendUniqueNode(scrubberNodes, sliders.front());
            }

            for (const auto& node : scrubberNodes) {
                auto scrubber = node.try_as<Controls::Slider>();
                if (!scrubber) continue;
                const auto scrubberIdentity = DependencyObjectIdentity(scrubber);
                if (!scrubberIdentity) continue;
                if (RememberObserver(g_scrubberObservers, scrubber)) {
                    scrubber.AddHandler(
                        Xaml::UIElement::PointerPressedEvent(),
                        box_value(Input::PointerEventHandler(
                            [scrubberIdentity](auto&&, auto&&) {
                                BeginSeekGesture(scrubberIdentity);
                            })), true);
                    scrubber.AddHandler(
                        Xaml::UIElement::PointerReleasedEvent(),
                        box_value(Input::PointerEventHandler(
                            [scrubberIdentity](auto&&, auto&&) {
                                CommitSeekGesture(scrubberIdentity, L"pointer-released");
                            })), true);
                    scrubber.AddHandler(
                        Xaml::UIElement::PointerCanceledEvent(),
                        box_value(Input::PointerEventHandler(
                            [scrubberIdentity](auto&&, auto&&) {
                                CommitSeekGesture(scrubberIdentity, L"pointer-canceled");
                            })), true);
                    scrubber.AddHandler(
                        Xaml::UIElement::PointerCaptureLostEvent(),
                        box_value(Input::PointerEventHandler(
                            [scrubberIdentity](auto&&, auto&&) {
                                CommitSeekGesture(scrubberIdentity, L"capture-lost");
                            })), true);
                    scrubber.Unloaded([scrubberIdentity](auto&&, auto&&) {
                        CommitSeekGesture(scrubberIdentity, L"scrubber-unloaded");
                        ScheduleTransportObserverAttach();
                    });
                }

                if (auto thumb = FindByClass(scrubber, L"Thumb").try_as<Primitives::Thumb>();
                    thumb && RememberObserver(g_scrubberThumbObservers, thumb)) {
                    auto weakThumb = winrt::make_weak(thumb);
                    thumb.DragStarted([weakThumb](auto&&, auto&&) {
                        if (auto live = weakThumb.get()) {
                            if (auto owner = ResolveOwningSlider(live)) {
                                BeginSeekGesture(DependencyObjectIdentity(owner));
                            }
                        }
                    });
                    thumb.DragCompleted([weakThumb](auto&&, auto&&) {
                        if (auto live = weakThumb.get()) {
                            if (auto owner = ResolveOwningSlider(live)) {
                                CommitSeekGesture(DependencyObjectIdentity(owner), L"drag-completed");
                            }
                        }
                    });
                    thumb.Unloaded([weakThumb](auto&&, auto&&) {
                        if (auto live = weakThumb.get()) {
                            if (auto owner = ResolveOwningSlider(live)) {
                                CommitSeekGesture(DependencyObjectIdentity(owner), L"thumb-unloaded");
                            } else {
                                CommitSeekGesture(0, L"thumb-unloaded-detached");
                            }
                        } else {
                            CommitSeekGesture(0, L"thumb-destroyed");
                        }
                        ScheduleTransportObserverAttach();
                    });
                }
            }
        }
    } catch (const hresult_error& error) {
        std::wostringstream text;
        text << L"transport observer attach failed hr=0x" << std::hex
             << error.code().value << L" " << error.message().c_str();
        Log(text.str());
    } catch (...) {
        Log(L"transport observer attach failed with unknown exception");
    }
}

Controls::Border CreateSettingsCard(const Controls::Border& appearance,
                                    const wchar_t* name, const Xaml::UIElement& content) {
    Controls::Border card;
    card.Name(name);
    card.Child(content);
    card.Background(appearance.Background());
    card.BorderBrush(appearance.BorderBrush());
    card.BorderThickness(appearance.BorderThickness());
    card.CornerRadius(appearance.CornerRadius());
    card.Padding(appearance.Padding());
    card.Margin(appearance.Margin());
    card.HorizontalAlignment(appearance.HorizontalAlignment());
    card.VerticalAlignment(appearance.VerticalAlignment());
    card.MinHeight(appearance.MinHeight());
    card.MaxWidth(appearance.MaxWidth());
    return card;
}

std::pair<Controls::Border, Controls::ToggleSwitch> CreateResamplingCard(
    const Controls::Border& card, const Controls::ToggleSwitch& toggle,
    const std::vector<Controls::TextBlock>& nativeText) {
    Controls::TextBlock resamplingTitle;
    resamplingTitle.Text(Localized(
        L"设备不支持原规格时允许兼容格式转换", L"Allow compatible format conversion for unsupported formats"));
    resamplingTitle.TextWrapping(Xaml::TextWrapping::Wrap);
    Controls::TextBlock resamplingDescription;
    resamplingDescription.Text(Localized(
        L"优先保留原始规格；按设备支持情况升／降采样、转换位深或使用浮点格式，保持独占输出与音质锁定。降低位深使用抖动处理。下次打开歌曲时生效；关闭时要求 bit-perfect 格式。",
        L"Prefer the original format; allow up/downsampling, bit-depth conversion or float output as needed. Keeps exclusive output and quality lock, with dither for bit-depth reduction. Applies when the next song opens; off requires a bit-perfect format."));
    resamplingDescription.TextWrapping(Xaml::TextWrapping::Wrap);
    Controls::TextBlock resamplingStatus;
    resamplingStatus.Name(L"AMExclusiveResamplingStatus");
    resamplingStatus.TextWrapping(Xaml::TextWrapping::Wrap);
    resamplingStatus.Visibility(Xaml::Visibility::Collapsed);
    if (!nativeText.empty()) resamplingTitle.Style(nativeText[0].Style());
    if (nativeText.size() > 1) resamplingDescription.Style(nativeText[1].Style());
    if (nativeText.size() > 1) resamplingStatus.Style(nativeText[1].Style());

    Controls::StackPanel resamplingLabels;
    resamplingLabels.VerticalAlignment(Xaml::VerticalAlignment::Center);
    resamplingLabels.Margin(Xaml::Thickness{0, 0, 16, 0});
    resamplingLabels.Children().Append(resamplingTitle);
    resamplingLabels.Children().Append(resamplingDescription);
    resamplingLabels.Children().Append(resamplingStatus);
    Controls::ToggleSwitch resamplingToggle;
    resamplingToggle.Name(L"AMExclusiveResamplingToggle");
    resamplingToggle.Header(nullptr);
    resamplingToggle.OnContent(nullptr);
    resamplingToggle.OffContent(nullptr);
    resamplingToggle.Style(toggle.Style());
    resamplingToggle.Margin(toggle.Margin());
    resamplingToggle.Padding(toggle.Padding());
    resamplingToggle.MinWidth(toggle.MinWidth());
    resamplingToggle.VerticalAlignment(Xaml::VerticalAlignment::Center);
    Automation::AutomationProperties::SetName(resamplingToggle, resamplingTitle.Text());
    Automation::AutomationProperties::SetHelpText(resamplingToggle, resamplingDescription.Text());
    resamplingToggle.Toggled([](auto&& sender, auto&&) {
        try {
            const int marker = g_programmaticResamplingValue.exchange(-1);
            if (g_suppressResamplingToggle.load()) return;
            const bool enabled = sender.template as<Controls::ToggleSwitch>().IsOn();
            if (marker == (enabled ? 1 : 0)) return;
            const int requested = g_requestedAllowResampling.load();
            const bool current = requested >= 0 ? requested != 0 : StatusSnapshot().allowResampling != 0;
            if (enabled == current) return;
            SendAllowResampling(enabled);
            Log(L"resampling setting queued enabled=" + std::to_wstring(enabled));
        } catch (const winrt::hresult_error& error) {
            Log(L"resampling toggle handler failed hresult=" +
                std::to_wstring(static_cast<std::uint32_t>(error.code())));
        } catch (...) {
            Log(L"resampling toggle handler failed with a non-WinRT exception");
        }
    });

    Controls::ColumnDefinition resamplingLabelColumn;
    resamplingLabelColumn.Width(Xaml::GridLength{1.0, Xaml::GridUnitType::Star});
    Controls::ColumnDefinition resamplingToggleColumn;
    resamplingToggleColumn.Width(Xaml::GridLength{1.0, Xaml::GridUnitType::Auto});
    Controls::Grid resamplingContent;
    resamplingContent.ColumnDefinitions().Append(resamplingLabelColumn);
    resamplingContent.ColumnDefinitions().Append(resamplingToggleColumn);
    resamplingContent.Children().Append(resamplingLabels);
    Controls::Grid::SetColumn(resamplingToggle, 1);
    resamplingContent.Children().Append(resamplingToggle);
    auto resamplingCard = CreateSettingsCard(card, L"AMExclusiveResamplingCard", resamplingContent);
    return {resamplingCard, resamplingToggle};
}

Controls::Border CreateOutputFormatCard(
    const Controls::Border& card, const std::vector<Controls::TextBlock>& nativeText) {
    Controls::TextBlock formatTitle;
    formatTitle.Text(Localized(L"音频规格", L"Audio format"));
    formatTitle.TextWrapping(Xaml::TextWrapping::Wrap);
    Controls::TextBlock formatText;
    formatText.Name(L"AMExclusiveOutputFormat");
    formatText.TextWrapping(Xaml::TextWrapping::Wrap);
    if (!nativeText.empty()) formatTitle.Style(nativeText[0].Style());
    if (nativeText.size() > 1) formatText.Style(nativeText[1].Style());

    Controls::StackPanel formatLabels;
    formatLabels.VerticalAlignment(Xaml::VerticalAlignment::Center);
    formatLabels.Children().Append(formatTitle);
    formatLabels.Children().Append(formatText);
    auto formatCard = CreateSettingsCard(card, L"AMExclusiveOutputFormatCard", formatLabels);
    ApplyOutputFormatStatus(formatCard);
    return formatCard;
}

bool AttachOrRefreshToggle(const Xaml::DependencyObject& page) {
    try {
        if (!page || !page.DispatcherQueue().HasThreadAccess()) return false;

        if (auto existing = FindByName(page, L"AMExclusiveModeToggle")) {
            unsigned complete{};
            if (auto toggle = existing.try_as<Controls::ToggleSwitch>()) {
                RememberToggle(toggle);
                ApplyStatus(toggle);
                ++complete;
            }
            if (auto input = FindByName(page, L"AMExclusiveHardwareBufferInput")
                                 .try_as<Controls::TextBox>()) {
                RememberHardwareBufferBox(input);
                ApplyHardwareBufferStatus(input);
                ++complete;
            }
            if (auto resampling = FindByName(page, L"AMExclusiveResamplingToggle")
                                      .try_as<Controls::ToggleSwitch>()) {
                RememberResamplingToggle(resampling);
                ApplyResamplingStatus(resampling);
                ++complete;
            }
            if (auto formatCard = FindByName(page, L"AMExclusiveOutputFormatCard")
                                      .try_as<Controls::Border>()) {
                RememberOutputFormatCard(formatCard);
                ApplyOutputFormatStatus(formatCard);
                ++complete;
            }
            return complete == 4;
        }

        Controls::StackPanel target{nullptr};
        unsigned score{};
        FindBestStackPanel(page, target, score);
        if (!target || !score) {
            Log(L"PlaybackSettingsPage found but no toggle-bearing StackPanel anchor was verified");
            return false;
        }

        auto nativeToggle = FindLastNativeToggle(target);
        auto renderedCard = FindRenderedCardBorder(nativeToggle, target);
        Xaml::FrameworkElement nativeRow{nullptr};
        const auto childCount = target.Children().Size();
        if (childCount) nativeRow = target.Children().GetAt(childCount - 1)
                                        .try_as<Xaml::FrameworkElement>();
        Controls::ToggleSwitch toggle;
        toggle.Name(L"AMExclusiveModeToggle");
        toggle.Header(nullptr);
        if (nativeToggle) {
            toggle.Style(nativeToggle.Style());
            toggle.Margin(nativeToggle.Margin());
            toggle.HorizontalAlignment(nativeToggle.HorizontalAlignment());
            toggle.VerticalAlignment(nativeToggle.VerticalAlignment());
            toggle.MinWidth(nativeToggle.MinWidth());
            toggle.MinHeight(nativeToggle.MinHeight());
            toggle.MaxWidth(nativeToggle.MaxWidth());
            toggle.MaxHeight(nativeToggle.MaxHeight());
            toggle.Padding(nativeToggle.Padding());
            Log(L"Exclusive mode ToggleSwitch adopted native sibling style and layout");
        } else {
            toggle.Margin(Xaml::Thickness{0, 0, 0, 0});
            Log(L"Exclusive mode ToggleSwitch native style reference unavailable");
        }
        Automation::AutomationProperties::SetName(
            toggle, Localized(L"独占模式", L"Exclusive mode"));
        Automation::AutomationProperties::SetHelpText(
            toggle,
            Localized(L"尝试在当前默认播放设备上启用 WASAPI 独占输出",
                      L"Try to use WASAPI exclusive output on the current default playback device"));
        toggle.Toggled([](winrt::Windows::Foundation::IInspectable const& sender,
                          Xaml::RoutedEventArgs const&) {
          try {
            if (g_suppressToggle.load()) return;
            auto control = sender.as<Controls::ToggleSwitch>();
            const bool enabled = control.IsOn();
            int marker = g_programmaticToggleValue.load();
            if (marker == (enabled ? 1 : 0) &&
                g_programmaticToggleValue.compare_exchange_strong(marker, -1)) {
                Log(std::wstring(L"ignored delayed programmatic toggle event enabled=") +
                    (enabled ? L"1" : L"0"));
                return;
            }
            // WinUI may deliver Toggled after ApplyStatus has returned, so a short-lived
            // suppression flag alone cannot distinguish a delayed backend-driven update
            // from an actual click.  A real click always differs from the last broker
            // intent; an equal value is merely the status echo and must not be sent back.
            if (enabled == (StatusSnapshot().enabledIntent != 0)) {
                Log(std::wstring(L"ignored broker-driven toggle event enabled=") +
                    (enabled ? L"1" : L"0"));
                return;
            }
            Log(std::wstring(L"user toggled exclusive mode enabled=") +
                (enabled ? L"1" : L"0"));
            control.IsEnabled(false);
            if (!SendEnabled(enabled)) {
                g_suppressToggle.store(true);
                g_programmaticToggleValue.store(0);
                control.IsOn(false);
                control.IsEnabled(true);
                if (auto statusText = FindStatusText(control)) {
                    statusText.Text(Localized(L"独占音频组件无法启动",
                                              L"The exclusive audio component could not start"));
                }
                g_suppressToggle.store(false);
            }
          } catch (const winrt::hresult_error& error) {
            g_suppressToggle.store(false);
            Log(L"exclusive toggle handler failed hresult=" +
                std::to_wstring(static_cast<std::uint32_t>(error.code())));
          } catch (...) {
            g_suppressToggle.store(false);
            Log(L"exclusive toggle handler failed with a non-WinRT exception");
          }
        });
        Controls::TextBlock title;
        title.Text(Localized(L"独占模式", L"Exclusive mode"));
        Controls::TextBlock description;
        description.Text(Localized(
            L"对当前默认播放设备使用 WASAPI 独占输出。",
            L"Use WASAPI exclusive output on the current default playback device."));
        description.TextWrapping(Xaml::TextWrapping::Wrap);
        Controls::TextBlock statusText;
        statusText.Name(L"AMExclusiveModeStatus");
        statusText.VerticalAlignment(Xaml::VerticalAlignment::Center);
        statusText.TextAlignment(Xaml::TextAlignment::Right);

        std::vector<Controls::TextBlock> nativeText;
        if (nativeRow) CollectTextBlocks(nativeRow, nativeText);
        if (!nativeText.empty()) title.Style(nativeText[0].Style());
        if (nativeText.size() > 1) description.Style(nativeText[1].Style());
        if (nativeText.size() > 2) {
            statusText.Style(nativeText.back().Style());
            statusText.TextAlignment(Xaml::TextAlignment::Right);
        }

        Controls::StackPanel labels;
        labels.VerticalAlignment(Xaml::VerticalAlignment::Center);
        labels.Children().Append(title);
        labels.Children().Append(description);

        Controls::ColumnDefinition labelColumn;
        labelColumn.Width(Xaml::GridLength{1.0, Xaml::GridUnitType::Star});
        Controls::ColumnDefinition toggleColumn;
        toggleColumn.Width(Xaml::GridLength{1.0, Xaml::GridUnitType::Auto});
        Controls::Grid cardContent;
        cardContent.ColumnDefinitions().Append(labelColumn);
        cardContent.ColumnDefinitions().Append(toggleColumn);
        cardContent.Children().Append(labels);
        Controls::StackPanel rightSide;
        rightSide.Orientation(Controls::Orientation::Horizontal);
        rightSide.Spacing(12.0);
        rightSide.VerticalAlignment(Xaml::VerticalAlignment::Center);
        rightSide.Children().Append(statusText);
        toggle.VerticalAlignment(Xaml::VerticalAlignment::Center);
        rightSide.Children().Append(toggle);
        Controls::Grid::SetColumn(rightSide, 1);
        cardContent.Children().Append(rightSide);

        Controls::Border card;
        card.Name(L"AMExclusiveModeCard");
        card.Child(cardContent);
        if (nativeRow) {
            card.Margin(nativeRow.Margin());
            card.HorizontalAlignment(nativeRow.HorizontalAlignment());
            card.VerticalAlignment(nativeRow.VerticalAlignment());
            card.MinHeight(nativeRow.MinHeight());
            card.MaxWidth(nativeRow.MaxWidth());
            if (auto nativeBorder = nativeRow.try_as<Controls::Border>()) {
                card.Background(nativeBorder.Background());
                card.BorderBrush(nativeBorder.BorderBrush());
                card.BorderThickness(nativeBorder.BorderThickness());
                card.CornerRadius(nativeBorder.CornerRadius());
                card.Padding(nativeBorder.Padding());
            } else if (auto nativeControl = nativeRow.try_as<Controls::Control>()) {
                card.Background(nativeControl.Background());
                card.BorderBrush(nativeControl.BorderBrush());
                card.BorderThickness(nativeControl.BorderThickness());
                card.Padding(nativeControl.Padding());
                card.CornerRadius(Xaml::CornerRadius{8, 8, 8, 8});
            } else if (auto nativePanel = nativeRow.try_as<Controls::Panel>()) {
                card.Background(nativePanel.Background());
                card.Padding(Xaml::Thickness{24, 16, 24, 16});
                card.CornerRadius(Xaml::CornerRadius{8, 8, 8, 8});
            }
        } else {
            card.Margin(Xaml::Thickness{0, 12, 0, 0});
            card.Padding(Xaml::Thickness{24, 16, 24, 16});
            card.CornerRadius(Xaml::CornerRadius{8, 8, 8, 8});
        }
        if (renderedCard) {
            card.Background(renderedCard.Background());
            if (auto stroke = ThemeBrush(L"CardStrokeColorDefaultBrush")) {
                card.BorderBrush(stroke);
            } else {
                card.BorderBrush(renderedCard.BorderBrush());
            }
            card.BorderThickness(Xaml::Thickness{1, 1, 1, 1});
            card.CornerRadius(Xaml::CornerRadius{4, 4, 4, 4});
            card.Padding(Xaml::Thickness{16, 11, 8, 11});
            card.MinHeight(0);
            card.Margin(Xaml::Thickness{0, 4, 0, 0});
            card.HorizontalAlignment(Xaml::HorizontalAlignment::Stretch);
            Log(L"Exclusive mode card adopted rendered native card Border");
        } else {
            card.Padding(Xaml::Thickness{16, 11, 8, 11});
            card.CornerRadius(Xaml::CornerRadius{4, 4, 4, 4});
            card.Margin(Xaml::Thickness{0, 4, 0, 0});
            card.BorderThickness(Xaml::Thickness{1, 1, 1, 1});
            if (!card.Background()) {
                card.Background(ThemeBrush(L"CardBackgroundFillColorDefaultBrush"));
            }
            if (!card.BorderBrush()) {
                card.BorderBrush(ThemeBrush(L"CardStrokeColorDefaultBrush"));
            }
            Log(L"Exclusive mode card used WinUI card theme resources");
        }
        if (nativeToggle) {
            const auto pageElement = page.try_as<Xaml::UIElement>();
            card.Loaded([nativeToggle, statusText, pageElement](auto&&, auto&&) {
                if (!pageElement) return;
                try {
                    std::vector<Controls::TextBlock> toggleText;
                    CollectTextBlocks(nativeToggle, toggleText);
                    Controls::TextBlock nativeState{nullptr};
                    double nativeRight = -1.0;
                    for (const auto& text : toggleText) {
                        if (text.Text().empty() || text.ActualWidth() <= 0.0) continue;
                        const auto origin = text.TransformToVisual(pageElement)
                                                .TransformPoint({0.0f, 0.0f});
                        const double right = static_cast<double>(origin.X) + text.ActualWidth();
                        if (right > nativeRight) {
                            nativeState = text;
                            nativeRight = right;
                        }
                    }
                    if (!nativeState || nativeRight < 0.0 || statusText.ActualWidth() <= 0.0) {
                        Log(L"Exclusive mode horizontal status calibration could not find a visible native toggle state label");
                        return;
                    }

                    const auto statusOrigin = statusText.TransformToVisual(pageElement)
                                                  .TransformPoint({0.0f, 0.0f});
                    const double statusRight = static_cast<double>(statusOrigin.X) +
                                               statusText.ActualWidth();
                    const double deltaX = nativeRight - statusRight;
                    if (std::abs(deltaX) > 128.0) {
                        Log(L"Exclusive mode horizontal status calibration rejected implausible delta");
                        return;
                    }

                    Media::TranslateTransform translate;
                    translate.X(deltaX);
                    translate.Y(0.0);
                    statusText.RenderTransform(translate);

                    std::wostringstream line;
                    line << L"Exclusive mode horizontal status calibrated nativeRight="
                         << std::fixed << std::setprecision(2) << nativeRight
                         << L" statusRight=" << statusRight
                         << L" deltaX=" << deltaX;
                    Log(line.str());
                } catch (const hresult_error& error) {
                    std::wostringstream line;
                    line << L"Exclusive mode horizontal status calibration failed hr=0x"
                         << std::hex << error.code().value;
                    Log(line.str());
                } catch (...) {
                    Log(L"Exclusive mode horizontal status calibration failed");
                }
            });
        }

        Controls::TextBlock bufferTitle;
        bufferTitle.Text(Localized(L"硬件缓冲区", L"Hardware buffer"));
        Controls::TextBlock bufferDescription;
        bufferDescription.Text(Localized(
            L"设置 WASAPI 独占输出的硬件缓冲时长（毫秒）。",
            L"Set the WASAPI exclusive hardware buffer duration in milliseconds."));
        bufferDescription.TextWrapping(Xaml::TextWrapping::Wrap);
        if (!nativeText.empty()) bufferTitle.Style(nativeText[0].Style());
        if (nativeText.size() > 1) bufferDescription.Style(nativeText[1].Style());

        Controls::StackPanel bufferLabels;
        bufferLabels.VerticalAlignment(Xaml::VerticalAlignment::Center);
        bufferLabels.Children().Append(bufferTitle);
        bufferLabels.Children().Append(bufferDescription);

        Controls::TextBox bufferInput;
        bufferInput.Name(L"AMExclusiveHardwareBufferInput");
        bufferInput.Width(96.0);
        bufferInput.MaxLength(3);
        bufferInput.TextAlignment(Xaml::TextAlignment::Right);
        bufferInput.VerticalAlignment(Xaml::VerticalAlignment::Center);
        bufferInput.TextChanged([](auto&&, auto&&) {
            if (!g_hardwareBufferTextUpdating.load()) {
                g_hardwareBufferInputDirty.store(true);
            }
        });
        Automation::AutomationProperties::SetName(
            bufferInput, Localized(L"硬件缓冲区（毫秒）", L"Hardware buffer in milliseconds"));
        Automation::AutomationProperties::SetHelpText(
            bufferInput, Localized(L"请输入 1 到 100 之间的整数",
                                   L"Enter an integer from 1 through 100"));
        Controls::Button bufferApply;
        bufferApply.Content(box_value(Localized(L"确定", L"Apply")));
        bufferApply.MinWidth(72.0);
        bufferApply.VerticalAlignment(Xaml::VerticalAlignment::Center);
        Automation::AutomationProperties::SetName(
            bufferApply, Localized(L"应用硬件缓冲区", L"Apply hardware buffer"));
        bufferApply.Click([weakInput = winrt::make_weak(bufferInput)](auto&&, auto&&) {
            try {
                if (auto input = weakInput.get()) CommitHardwareBuffer(input);
            } catch (const winrt::hresult_error& error) {
                Log(L"hardware buffer apply failed hresult=" +
                    std::to_wstring(static_cast<std::uint32_t>(error.code())));
            } catch (...) {
                Log(L"hardware buffer apply failed with a non-WinRT exception");
            }
        });

        Controls::ColumnDefinition bufferLabelColumn;
        bufferLabelColumn.Width(Xaml::GridLength{1.0, Xaml::GridUnitType::Star});
        Controls::ColumnDefinition bufferInputColumn;
        bufferInputColumn.Width(Xaml::GridLength{1.0, Xaml::GridUnitType::Auto});
        Controls::Grid bufferContent;
        bufferContent.ColumnDefinitions().Append(bufferLabelColumn);
        bufferContent.ColumnDefinitions().Append(bufferInputColumn);
        bufferContent.Children().Append(bufferLabels);
        Controls::StackPanel bufferRightSide;
        bufferRightSide.Orientation(Controls::Orientation::Horizontal);
        bufferRightSide.Spacing(8.0);
        bufferRightSide.VerticalAlignment(Xaml::VerticalAlignment::Center);
        bufferRightSide.Children().Append(bufferInput);
        bufferRightSide.Children().Append(bufferApply);
        Controls::Grid::SetColumn(bufferRightSide, 1);
        bufferContent.Children().Append(bufferRightSide);

        Controls::Border bufferCard;
        bufferCard.Name(L"AMExclusiveHardwareBufferCard");
        bufferCard.Child(bufferContent);
        bufferCard.Background(card.Background());
        bufferCard.BorderBrush(card.BorderBrush());
        bufferCard.BorderThickness(card.BorderThickness());
        bufferCard.CornerRadius(card.CornerRadius());
        bufferCard.Padding(card.Padding());
        bufferCard.Margin(card.Margin());
        bufferCard.HorizontalAlignment(card.HorizontalAlignment());
        bufferCard.VerticalAlignment(card.VerticalAlignment());
        bufferCard.MinHeight(card.MinHeight());
        bufferCard.MaxWidth(card.MaxWidth());
        auto [resamplingCard, resamplingToggle] = CreateResamplingCard(card, toggle, nativeText);
        auto formatCard = CreateOutputFormatCard(card, nativeText);
        // Construct all cards before publishing any. If an append fails, only
        // remove this batch, so the next readiness callback can retry cleanly.
        const Xaml::UIElement cards[]{card, bufferCard, resamplingCard, formatCard};
        auto children = target.Children();
        try {
            for (const auto& item : cards) children.Append(item);
        } catch (...) {
            for (const auto& item : cards) {
                std::uint32_t index{};
                if (children.IndexOf(item, index)) children.RemoveAt(index);
            }
            throw;
        }

        RememberToggle(toggle);
        RememberHardwareBufferBox(bufferInput);
        RememberResamplingToggle(resamplingToggle);
        RememberOutputFormatCard(formatCard);
        ApplyStatus(toggle);
        ApplyHardwareBufferStatus(bufferInput);
        ApplyResamplingStatus(resamplingToggle);
        Log(L"Exclusive mode settings card attached to PlaybackSettingsPage");
        Log(L"Hardware buffer settings card attached to PlaybackSettingsPage");
        Log(L"Resampling settings card attached to PlaybackSettingsPage");
        Log(L"Audio format settings card attached to PlaybackSettingsPage");
        return true;
    } catch (const hresult_error& error) {
        std::wostringstream text;
        text << L"UI attach failed hr=0x" << std::hex << error.code().value << L" " << error.message().c_str();
        Log(text.str());
    } catch (...) {
        Log(L"UI attach failed with unknown exception");
    }
    return false;
}

bool StartBrokerIfNeeded() {
    const auto name = PipeName();
    if (name.empty()) return false;
    if (WaitNamedPipeW(name.c_str(), 100)) return true;
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(g_module, modulePath, static_cast<DWORD>(std::size(modulePath)));
    const auto broker = std::filesystem::path(modulePath).parent_path() / L"am-exclusive-broker.exe";
    if (!std::filesystem::exists(broker)) return false;
    std::wstring command = L"\"" + broker.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(broker.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, broker.parent_path().c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return WaitNamedPipeW(name.c_str(), 5000) != FALSE;
}

DWORD WINAPI PipeThread(void*) {
    const auto name = PipeName();
    for (;;) {
        if (!StartBrokerIfNeeded()) {
            Sleep(1000);
            continue;
        }
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
            CloseHandle(pipe);
            Sleep(1000);
            continue;
        }
        {
            std::lock_guard lock(g_pipeMutex);
            g_pipe = pipe;
            auto hello = NewMessage(MessageType::Hello, Role::Ui);
            hello.uiPid = GetCurrentProcessId();
            if (!WriteMessage(pipe, hello)) {
                g_pipe = INVALID_HANDLE_VALUE;
                CloseHandle(pipe);
                continue;
            }
            g_connected.store(true);
        }
        Log(L"UI broker IPC connected");
        SchedulePlaybackStateSync();
        // Broker broadcasts fan out on independent threads, so an older status
        // can arrive after a newer one. Broker generations only ever increase
        // within one broker lifetime; the counter resets per connection.
        std::uint64_t lastStatusGeneration = 0;
        if (const int requested = g_requestedHardwareBufferMs.load(); requested >= 0) {
            g_pendingHardwareBufferMs.store(requested);
            g_hardwareBufferAckDeadlineTick.store(0);
        }
        if (const int requested = g_requestedAllowResampling.load(); requested >= 0) {
            g_pendingAllowResampling.store(requested);
            g_resamplingAckDeadlineTick.store(0);
        }
        Message incoming{};
        for (;;) {
            Message transport{};
            bool hasTransport{};
            {
                std::lock_guard lock(g_transportMutex);
                if (!g_transportPending.empty()) {
                    transport = g_transportPending.front();
                    g_transportPending.pop_front();
                    hasTransport = true;
                }
            }
            if (hasTransport) {
                bool written{};
                {
                    std::lock_guard writeLock(g_pipeWriteMutex);
                    written = WriteMessage(pipe, transport);
                }
                if (!written) {
                    std::lock_guard lock(g_transportMutex);
                    g_transportPending.push_front(transport);
                    break;
                }
            }

            const int pending = g_pendingEnabled.exchange(-1);
            if (pending >= 0) {
                auto message = NewMessage(MessageType::SetEnabled, Role::Ui);
                message.uiPid = GetCurrentProcessId();
                message.enabledIntent = pending ? 1 : 0;
                bool written{};
                {
                    std::lock_guard writeLock(g_pipeWriteMutex);
                    written = WriteMessage(pipe, message);
                }
                if (!written) {
                    g_pendingEnabled.store(pending);
                    break;
                }
            }
            const int pendingHardwareBufferMs = g_pendingHardwareBufferMs.exchange(-1);
            if (pendingHardwareBufferMs >= 0) {
                auto message = NewMessage(MessageType::SetHardwareBuffer, Role::Ui);
                message.uiPid = GetCurrentProcessId();
                message.hardwareBufferMs =
                    static_cast<std::uint32_t>(pendingHardwareBufferMs);
                bool written{};
                {
                    std::lock_guard writeLock(g_pipeWriteMutex);
                    written = WriteMessage(pipe, message);
                }
                if (!written) {
                    g_pendingHardwareBufferMs.store(pendingHardwareBufferMs);
                    break;
                }
                g_hardwareBufferAckDeadlineTick.store(GetTickCount64() + 1000);
            }
            const int pendingAllowResampling = g_pendingAllowResampling.exchange(-1);
            if (pendingAllowResampling >= 0) {
                auto message = NewMessage(MessageType::SetAllowResampling, Role::Ui);
                message.uiPid = GetCurrentProcessId();
                message.allowResampling = pendingAllowResampling ? 1 : 0;
                bool written{};
                {
                    std::lock_guard writeLock(g_pipeWriteMutex);
                    written = WriteMessage(pipe, message);
                }
                if (!written) {
                    g_pendingAllowResampling.store(pendingAllowResampling);
                    break;
                }
                g_resamplingAckDeadlineTick.store(GetTickCount64() + 1000);
            }
            if (!ReadMessageTimeout(pipe, incoming, 100)) {
                if (GetLastError() == ERROR_TIMEOUT) {
                    const int requested = g_requestedHardwareBufferMs.load();
                    const auto deadline = g_hardwareBufferAckDeadlineTick.load();
                    if (requested >= 0 && deadline != 0 && GetTickCount64() >= deadline) {
                        int empty = -1;
                        if (g_pendingHardwareBufferMs.compare_exchange_strong(empty, requested)) {
                            g_hardwareBufferAckDeadlineTick.store(0);
                            Log(L"hardware buffer confirmation timed out; retry queued milliseconds=" +
                                std::to_wstring(requested));
                        }
                    }
                    const int resamplingRequested = g_requestedAllowResampling.load();
                    const auto resamplingDeadline = g_resamplingAckDeadlineTick.load();
                    if (resamplingRequested >= 0 && resamplingDeadline != 0 &&
                        GetTickCount64() >= resamplingDeadline) {
                        int empty = -1;
                        if (g_pendingAllowResampling.compare_exchange_strong(empty, resamplingRequested)) {
                            g_resamplingAckDeadlineTick.store(0);
                            Log(L"resampling confirmation timed out; retry queued");
                        }
                    }
                    continue;
                }
                break;
            }
            if (incoming.type == MessageType::StatusChanged) {
                if (incoming.generation < lastStatusGeneration) {
                    Log(L"ignored stale broker status generation=" +
                        std::to_wstring(incoming.generation) + L" latest=" +
                        std::to_wstring(lastStatusGeneration));
                    continue;
                }
                lastStatusGeneration = incoming.generation;
                bool resyncPlayback{};
                {
                    std::lock_guard lock(g_statusMutex);
                    resyncPlayback = !IsEqualGUID(g_status.requestId, incoming.requestId);
                    g_status = incoming;
                }
                if (resyncPlayback) SchedulePlaybackStateSync();
                const int requested = g_requestedHardwareBufferMs.load();
                if (requested >= 0 &&
                    incoming.hardwareBufferMs == static_cast<std::uint32_t>(requested)) {
                    g_requestedHardwareBufferMs.store(-1);
                    g_hardwareBufferAckDeadlineTick.store(0);
                    g_hardwareBufferInputDirty.store(false);
                    Log(L"hardware buffer confirmed milliseconds=" +
                        std::to_wstring(requested));
                }
                const int resamplingRequested = g_requestedAllowResampling.load();
                if (resamplingRequested >= 0 && incoming.allowResampling == resamplingRequested) {
                    g_requestedAllowResampling.store(-1);
                    g_resamplingAckDeadlineTick.store(0);
                    Log(L"resampling setting confirmed enabled=" +
                        std::to_wstring(resamplingRequested));
                }
                ScheduleStatusRefresh();
            } else if (incoming.type == MessageType::Goodbye) {
                break;
            }
        }
        {
            std::lock_guard lock(g_pipeMutex);
            if (g_pipe == pipe) g_pipe = INVALID_HANDLE_VALUE;
            g_connected.store(false);
        }
        CloseHandle(pipe);
        Log(L"UI broker IPC disconnected");
        Sleep(1000);
    }
}

DWORD WINAPI BootstrapThread(void*) {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(g_module, modulePath, static_cast<DWORD>(std::size(modulePath)));
    g_logPath = std::filesystem::path(modulePath).parent_path() / L"am-exclusive-ui.log";
    {
        std::lock_guard lock(g_statusMutex);
        g_status = NewMessage(MessageType::StatusChanged, Role::Ui);
        g_status.state = RuntimeState::Off;
    }
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Log(L"UI adapter loaded");
    if (HANDLE pipeThread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr)) CloseHandle(pipeThread);

    g_windowLifecycleHook = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, WindowLifecycleEvent,
        GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    Log(L"UI window lifecycle WinEvent hook=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(g_windowLifecycleHook)));

    // This timer only recovers a missed/new Window/root. Actual navigation and
    // page readiness are event-driven on the verified window dispatcher.
    ScheduleUiDiscovery();
    ULONGLONG lastFallbackTick = GetTickCount64();
    DWORD waitMs = 5000;
    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        const auto now = GetTickCount64();
        const auto elapsed = now - lastFallbackTick;
        if (elapsed >= 5000) {
            lastFallbackTick = now;
            ScheduleUiDiscovery();
            waitMs = 5000;
        } else {
            waitMs = static_cast<DWORD>(5000 - elapsed);
        }
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
