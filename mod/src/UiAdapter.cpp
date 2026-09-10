#include "BoundedLog.h"
#include "IpcProtocol.h"
#include "IpcTransport.h"

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
#include <winrt/Microsoft.UI.Dispatching.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
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
std::atomic<std::uint64_t> g_lastLoggedGeneration{UINT64_MAX};
std::atomic<HHOOK> g_uiHook{};
std::atomic<std::uint64_t> g_transportEpoch{};
std::atomic<std::uint64_t> g_activeSeekEpoch{};
std::atomic<ULONGLONG> g_lastSkipPointerTick{};
std::deque<Message> g_transportPending;
std::uintptr_t g_transportButtonIdentity{};
std::uintptr_t g_skipBackIdentity{};
std::uintptr_t g_skipForwardIdentity{};
std::uintptr_t g_trackListPointerRootIdentity{};
std::uintptr_t g_lastTrackRowPressIdentity{};
std::uint64_t g_lastTrackRowPressEpoch{};
ULONGLONG g_lastTrackRowPressTick{};
std::uintptr_t g_scrubberIdentity{};
std::uintptr_t g_scrubberThumbIdentity{};
HWND g_mainWindow{};
winrt::weak_ref<Controls::ToggleSwitch> g_toggle;
Microsoft::UI::Dispatching::DispatcherQueue g_dispatcher{nullptr};
Microsoft::UI::Dispatching::DispatcherQueueTimer g_statusRefreshTimer{nullptr};

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
            L"独占设备已启动，但 Apple Music 未继续提交音频；已自动关闭独占模式",
            L"Apple Music stopped sending audio; exclusive mode was turned off automatically");
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

void BeginSeekGesture() {
    if (g_activeSeekEpoch.load(std::memory_order_acquire) != 0) return;
    const auto epoch = g_transportEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::uint64_t expected{};
    if (g_activeSeekEpoch.compare_exchange_strong(
            expected, epoch, std::memory_order_acq_rel, std::memory_order_acquire)) {
        QueueTransport(L"seek_begin", epoch);
    }
}

void CommitSeekGesture() {
    const auto epoch = g_activeSeekEpoch.exchange(0, std::memory_order_acq_rel);
    if (epoch) QueueTransport(L"seek_commit", epoch);
}

HWND FindMainWindow() {
    struct Context { DWORD pid; HWND window; } context{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(value);
        DWORD pid{};
        GetWindowThreadProcessId(window, &pid);
        if (pid == context.pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
            context.window = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.window;
}

using GetAppPropertyFn = Windows::Foundation::IInspectable(__cdecl*)(hstring const&);

Xaml::Window GetMainXamlWindow() {
    HMODULE utils = GetModuleHandleW(L"AMP.Utils.dll");
    if (!utils) return nullptr;
    constexpr char exportName[] =
        "?GetAppProperty@SharedUtils@@YA?AUIInspectable@Foundation@Windows@winrt@@AEBUhstring@5@@Z";
    auto function = reinterpret_cast<GetAppPropertyFn>(GetProcAddress(utils, exportName));
    if (!function) return nullptr;
    try {
        return function(hstring(L"MainWindow")).try_as<Xaml::Window>();
    } catch (const hresult_error& error) {
        Log(L"GetAppProperty(MainWindow) failed hr=0x" +
            [&] { std::wostringstream text; text << std::hex << error.code().value; return text.str(); }());
        return nullptr;
    }
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

Controls::TextBlock FindStatusText(const Controls::ToggleSwitch& toggle) {
    if (!toggle) return nullptr;
    try {
        auto current = Media::VisualTreeHelper::GetParent(toggle);
        while (current) {
            if (auto found = FindByName(current, L"AMExclusiveModeStatus")
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
            winrt::weak_ref<Controls::ToggleSwitch> target;
            {
                std::lock_guard targetLock(g_uiTargetMutex);
                target = g_toggle;
            }
            if (auto control = target.get()) ApplyStatus(control);
        });
        g_statusRefreshTimer.Start();
        Log(L"UI status refresh timer started intervalMs=250");
    }
}

void ScheduleStatusRefresh() {
    winrt::weak_ref<Controls::ToggleSwitch> toggle;
    Microsoft::UI::Dispatching::DispatcherQueue dispatcher{nullptr};
    {
        std::lock_guard lock(g_uiTargetMutex);
        toggle = g_toggle;
        dispatcher = g_dispatcher;
    }
    if (dispatcher) {
        dispatcher.TryEnqueue([toggle] {
            if (auto control = toggle.get()) ApplyStatus(control);
        });
    }
}

void AttachTransportObservers() {
    try {
        auto window = GetMainXamlWindow();
        if (!window) return;
        auto content = window.Content().try_as<Xaml::DependencyObject>();
        if (!content) return;

        // Track rows are ListViewItem controls. Apple begins direct playback on
        // the second press of the normal double-click gesture, but that path
        // does not traverse the Previous/Next buttons. Observe the routed
        // pointer at the window content root so the second press can publish a
        // synchronous skip fence before Apple disposes the currently playing
        // local converter. A single selection click is deliberately ignored.
        if (auto pointerRoot = content.try_as<Xaml::UIElement>()) {
            const auto identity = reinterpret_cast<std::uintptr_t>(get_abi(pointerRoot));
            if (identity != g_trackListPointerRootIdentity) {
                g_trackListPointerRootIdentity = identity;
                pointerRoot.AddHandler(
                    Xaml::UIElement::PointerPressedEvent(),
                    box_value(Input::PointerEventHandler(
                        [](auto&&, Input::PointerRoutedEventArgs const& args) {
                            auto current = args.OriginalSource().try_as<Xaml::DependencyObject>();
                            for (unsigned depth = 0; current && depth < 16; ++depth) {
                                if (auto row = current.try_as<Controls::ListViewItem>()) {
                                    const auto rowIdentity =
                                        reinterpret_cast<std::uintptr_t>(get_abi(row));
                                    const auto now = GetTickCount64();
                                    const bool secondPress =
                                        rowIdentity != 0 &&
                                        rowIdentity == g_lastTrackRowPressIdentity &&
                                        now >= g_lastTrackRowPressTick &&
                                        now - g_lastTrackRowPressTick <= 650;
                                    if (secondPress) {
                                        const auto epoch = g_lastTrackRowPressEpoch;
                                        g_lastTrackRowPressIdentity = 0;
                                        g_lastTrackRowPressEpoch = 0;
                                        g_lastTrackRowPressTick = 0;
                                        if (epoch != 0) {
                                            QueueTransport(L"skip", epoch);
                                            Log(L"transport direct-track double-press fence epoch=" +
                                                std::to_wstring(epoch));
                                        } else {
                                            Log(L"transport direct-track double-press ignored; no armed owner");
                                        }
                                    } else {
                                        g_lastTrackRowPressIdentity = rowIdentity;
                                        g_lastTrackRowPressTick = now;
                                        g_lastTrackRowPressEpoch = 0;
                                        const auto status = StatusSnapshot();
                                        if (status.state == RuntimeState::Active) {
                                            const auto epoch =
                                                g_transportEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
                                            g_lastTrackRowPressEpoch = epoch;
                                            QueueTransport(L"selection_arm", epoch);
                                            Log(L"transport direct-track selection arm epoch=" +
                                                std::to_wstring(epoch));
                                        }
                                    }
                                    break;
                                }
                                current = Media::VisualTreeHelper::GetParent(current);
                            }
                        })),
                    true);
                Log(L"transport observer attached control=TrackListPointerRoot");
            }
        }

        auto buttonNode = FindByName(content, L"TransportControl_PlayPauseStop");
        if (!buttonNode) {
            buttonNode = FindByAutomationId(content, L"TransportControl_PlayPauseStop");
        }
        if (auto button = buttonNode.try_as<Controls::Button>()) {
            const auto identity = reinterpret_cast<std::uintptr_t>(get_abi(button));
            if (identity != g_transportButtonIdentity) {
                g_transportButtonIdentity = identity;
                button.Click([](auto&&, auto&&) {
                    QueueTransport(L"play_pause");
                });
                Log(L"transport observer attached control=PlayPauseStop");
            }
        }

        auto attachSkipObserver = [&](std::wstring_view automationId,
                                      std::uintptr_t& rememberedIdentity) {
            auto node = FindByName(content, automationId);
            if (!node) node = FindByAutomationId(content, automationId);
            if (auto button = node.try_as<Controls::Button>()) {
                const auto identity = reinterpret_cast<std::uintptr_t>(get_abi(button));
                if (identity != rememberedIdentity) {
                    rememberedIdentity = identity;
                    button.AddHandler(
                        Xaml::UIElement::PointerPressedEvent(),
                        box_value(Input::PointerEventHandler([](auto&&, auto&&) {
                            QueueSkipIntent(true);
                        })),
                        true);
                    button.Click([](auto&&, auto&&) {
                        QueueSkipIntent(false);
                    });
                    Log(L"transport observer attached control=" + std::wstring(automationId));
                }
            }
        };
        attachSkipObserver(L"TransportControl_SkipBack", g_skipBackIdentity);
        attachSkipObserver(L"TransportControl_SkipForward", g_skipForwardIdentity);

        if (auto scrubber = FindByName(content, L"LCDScrubber").try_as<Controls::Slider>()) {
            const auto identity = reinterpret_cast<std::uintptr_t>(get_abi(scrubber));
            if (identity != g_scrubberIdentity) {
                g_scrubberIdentity = identity;
                scrubber.AddHandler(
                    Xaml::UIElement::PointerPressedEvent(),
                    box_value(Input::PointerEventHandler([](auto&&, auto&&) {
                        BeginSeekGesture();
                    })),
                    true);
                scrubber.AddHandler(
                    Xaml::UIElement::PointerReleasedEvent(),
                    box_value(Input::PointerEventHandler([](auto&&, auto&&) {
                        CommitSeekGesture();
                    })),
                    true);
                Log(L"transport observer attached control=LCDScrubber");
            }
            if (auto thumb = FindByClass(scrubber, L"Thumb").try_as<Primitives::Thumb>()) {
                const auto thumbIdentity = reinterpret_cast<std::uintptr_t>(get_abi(thumb));
                if (thumbIdentity != g_scrubberThumbIdentity) {
                    g_scrubberThumbIdentity = thumbIdentity;
                    thumb.DragStarted([](auto&&, auto&&) {
                        BeginSeekGesture();
                    });
                    thumb.DragCompleted([](auto&&, auto&&) {
                        CommitSeekGesture();
                    });
                    Log(L"transport observer attached control=LCDScrubberThumb");
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

void AttachOrRefreshToggle() {
    try {
        auto window = GetMainXamlWindow();
        if (!window) return;
        auto content = window.Content().try_as<Xaml::DependencyObject>();
        if (!content) return;
        auto page = FindByClass(content, L"PlaybackSettingsPage");
        if (!page) return;

        if (auto existing = FindByName(page, L"AMExclusiveModeToggle")) {
            if (auto toggle = existing.try_as<Controls::ToggleSwitch>()) {
                RememberToggle(toggle);
                ApplyStatus(toggle);
            }
            return;
        }

        Controls::StackPanel target{nullptr};
        unsigned score{};
        FindBestStackPanel(page, target, score);
        if (!target || !score) {
            Log(L"PlaybackSettingsPage found but no toggle-bearing StackPanel anchor was verified");
            return;
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
        });
        Controls::TextBlock title;
        title.Text(Localized(L"独占模式", L"Exclusive mode"));
        Controls::TextBlock description;
        description.Text(Localized(
            L"对当前默认播放设备使用 WASAPI 独占输出；无损播放只允许 ALAC。",
            L"Use WASAPI exclusive output on the current default playback device; lossless playback allows ALAC only."));
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
        target.Children().Append(card);
        RememberToggle(toggle);
        ApplyStatus(toggle);
        Log(L"Exclusive mode settings card attached to PlaybackSettingsPage");
    } catch (const hresult_error& error) {
        std::wostringstream text;
        text << L"UI attach failed hr=0x" << std::hex << error.code().value << L" " << error.message().c_str();
        Log(text.str());
    } catch (...) {
        Log(L"UI attach failed with unknown exception");
    }
}

LRESULT CALLBACK UiThreadHook(int code, WPARAM wParam, LPARAM lParam) {
    HHOOK hook = g_uiHook.exchange(nullptr);
    if (hook) UnhookWindowsHookEx(hook);
    if (code >= 0) {
        AttachTransportObservers();
        AttachOrRefreshToggle();
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
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
            if (!ReadMessageTimeout(pipe, incoming, 100)) {
                if (GetLastError() == ERROR_TIMEOUT) continue;
                break;
            }
            if (incoming.type == MessageType::StatusChanged) {
                {
                    std::lock_guard lock(g_statusMutex);
                    g_status = incoming;
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
    Log(L"UI adapter loaded");
    if (HANDLE pipeThread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr)) CloseHandle(pipeThread);

    for (;;) {
        if (!g_mainWindow || !IsWindow(g_mainWindow)) g_mainWindow = FindMainWindow();
        if (g_mainWindow && !g_uiHook.load()) {
            const DWORD threadId = GetWindowThreadProcessId(g_mainWindow, nullptr);
            HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, UiThreadHook, g_module, threadId);
            if (hook) {
                g_uiHook.store(hook);
                PostMessageW(g_mainWindow, WM_NULL, 0, 0);
            }
        }
        Sleep(750);
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
