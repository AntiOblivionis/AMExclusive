#include "BoundedLog.h"
#include "IpcProtocol.h"
#include "IpcTransport.h"

#include <windows.h>
#include <appmodel.h>
#include <commctrl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t kDefaultHardwareBufferMs = 20;
constexpr std::uint32_t kMinimumHardwareBufferMs = 1;
constexpr std::uint32_t kMaximumHardwareBufferMs = 100;

std::filesystem::path BrokerIniPath();

std::uint32_t ReadHardwareBufferMs() {
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"mod", L"period_100ns", L"200000", value,
        static_cast<DWORD>(std::size(value)), BrokerIniPath().c_str());
    wchar_t* end{};
    const auto period100ns = wcstoull(value, &end, 10);
    if (end == value || *end != L'\0') return kDefaultHardwareBufferMs;
    const auto milliseconds = (period100ns + 5000u) / 10000u;
    if (milliseconds < kMinimumHardwareBufferMs ||
        milliseconds > kMaximumHardwareBufferMs) {
        return kDefaultHardwareBufferMs;
    }
    return static_cast<std::uint32_t>(milliseconds);
}

bool WriteHardwareBufferMs(std::uint32_t milliseconds) {
    const auto value = std::to_wstring(static_cast<std::uint64_t>(milliseconds) * 10000u);
    const auto path = BrokerIniPath();
    if (!WritePrivateProfileStringW(L"mod", L"period_100ns", value.c_str(), path.c_str())) {
        return false;
    }
    (void)WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return ReadHardwareBufferMs() == milliseconds;
}

bool ReadAllowResampling() {
    return GetPrivateProfileIntW(L"mod", L"allow_resampling", 0, BrokerIniPath().c_str()) == 1;
}

bool WriteAllowResampling(bool enabled) {
    const auto path = BrokerIniPath();
    if (!WritePrivateProfileStringW(L"mod", L"allow_resampling",
                                    enabled ? L"1" : L"0", path.c_str())) {
        return false;
    }
    (void)WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return ReadAllowResampling() == enabled;
}

using namespace ammod::ipc;

struct Client {
    HANDLE pipe{};
    Role role{Role::Unknown};
    DWORD pid{};
};

std::mutex g_mutex;
std::mutex g_logMutex;
std::atomic<bool> g_shutdown{};
std::atomic<std::uint64_t> g_lastErrorDialogSignature{};
std::vector<Client> g_clients;
HANDLE g_authoritativeAgentPipe{INVALID_HANDLE_VALUE};
Message g_status = [] {
    auto message = NewMessage(MessageType::StatusChanged, Role::Broker);
    message.state = RuntimeState::Off;
    return message;
}();

void Log(const std::wstring& text);
void Broadcast(const Message& message, HANDLE source);
std::vector<DWORD> FindProcesses(const wchar_t* executableName);

bool UseChineseUi() noexcept {
    const LANGID language = GetUserDefaultUILanguage();
    return PRIMARYLANGID(language) == LANG_CHINESE;
}

HWND FindAppleMusicWindow() noexcept {
    struct Search final {
        DWORD pid{};
        HWND window{};
    } search;
    const auto processes = FindProcesses(L"AppleMusic.exe");
    if (processes.empty()) return nullptr;
    search.pid = processes.front();
    EnumWindows(+[](HWND window, LPARAM context) -> BOOL {
        auto* search = reinterpret_cast<Search*>(context);
        DWORD pid{};
        GetWindowThreadProcessId(window, &pid);
        if (pid != search->pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) {
            return TRUE;
        }
        search->window = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

HRESULT CALLBACK ErrorDialogCallback(HWND dialog, UINT notification,
                                     WPARAM, LPARAM, LONG_PTR callbackData) {
    if (notification == TDN_CREATED && dialog) {
        const HWND owner = reinterpret_cast<HWND>(callbackData);
        RECT dialogRect{};
        RECT anchorRect{};
        const bool haveDialog = GetWindowRect(dialog, &dialogRect) != FALSE;
        bool haveAnchor = owner && IsWindow(owner) && GetWindowRect(owner, &anchorRect) != FALSE;
        const HMONITOR monitor = MonitorFromWindow(
            haveAnchor ? owner : dialog, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        const bool haveMonitor = monitor && GetMonitorInfoW(monitor, &monitorInfo) != FALSE;
        if (!haveAnchor && haveMonitor) {
            anchorRect = monitorInfo.rcWork;
            haveAnchor = true;
        }
        LONG x = 0;
        LONG y = 0;
        if (haveDialog && haveAnchor) {
            const LONG width = dialogRect.right - dialogRect.left;
            const LONG height = dialogRect.bottom - dialogRect.top;
            x = anchorRect.left + ((anchorRect.right - anchorRect.left) - width) / 2;
            y = anchorRect.top + ((anchorRect.bottom - anchorRect.top) - height) / 2;
            if (haveMonitor) {
                x = std::clamp(x, monitorInfo.rcWork.left,
                               std::max(monitorInfo.rcWork.left, monitorInfo.rcWork.right - width));
                y = std::clamp(y, monitorInfo.rcWork.top,
                               std::max(monitorInfo.rcWork.top, monitorInfo.rcWork.bottom - height));
            }
        }
        SetWindowPos(dialog, HWND_TOPMOST, x, y, 0, 0,
                     SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(dialog);
    }
    return S_OK;
}

void ShowErrorDialogAsync(ErrorCategory error, HRESULT result,
                          std::uint64_t generation, bool playbackFailure = true) {
    if (error == ErrorCategory::None) error = ErrorCategory::Internal;
    const auto signature = (generation << 16u) ^ static_cast<std::uint64_t>(error);
    if (signature != 0 && g_lastErrorDialogSignature.exchange(signature) == signature) {
        return;
    }
    std::thread([error, result, generation, playbackFailure] {
        // Let a normal Apple Music shutdown remove its window before deciding
        // whether a disconnected Agent represents a user-visible failure.
        Sleep(250);
        {
            std::lock_guard lock(g_mutex);
            if (g_status.generation != generation || g_status.error != error ||
                !g_status.enabledIntent) return;
        }
        const auto previousDpi = SetThreadDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const bool chinese = UseChineseUi();
        const HWND owner = FindAppleMusicWindow();
        if (!owner) {
            Log(L"error dialog suppressed because Apple Music has no visible window error=" +
                std::to_wstring(static_cast<unsigned>(error)));
            if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
            return;
        }
        // Runtime already retired the failed audio generation. GSMTC offers
        // no authoritative Apple item token here; clearing its current item
        // after this delay could instead clear a newly selected song.
        const wchar_t* reason{};
        switch (error) {
        case ErrorCategory::UnsupportedLocalInt32:
            reason = chinese ? L"不支持本地 32 位整数音频。"
                             : L"Local 32-bit integer audio is not supported.";
            break;
        case ErrorCategory::BitPerfectFormatUnavailable:
            reason = chinese ? L"DAC 不支持当前音源格式。"
                             : L"The DAC does not support the current source format.";
            break;
        case ErrorCategory::ExclusiveFormatUnavailable:
            reason = chinese ? L"尝试兼容格式转换后，仍未找到设备支持的 WASAPI 独占输出格式。"
                             : L"No supported WASAPI exclusive format was found after sample-rate and bit-depth conversion.";
            break;
        case ErrorCategory::FormatUnsupported:
            reason = chinese ? L"声卡驱动拒绝了当前独占格式。"
                             : L"The audio driver rejected the current exclusive format.";
            break;
        case ErrorCategory::ExclusiveNotAllowed:
            reason = chinese ? L"当前播放设备未允许应用使用独占模式。"
                             : L"Exclusive mode is disabled for the current playback device.";
            break;
        case ErrorCategory::DeviceInUse:
            reason = chinese ? L"当前播放设备正被其他程序独占。"
                             : L"The current playback device is in exclusive use by another app.";
            break;
        case ErrorCategory::DeviceChanged:
        case ErrorCategory::DeviceInvalidated:
            reason = chinese ? L"播放设备已变化或不可用。"
                             : L"The playback device changed or is unavailable.";
            break;
        case ErrorCategory::EndpointUnavailable:
            reason = chinese ? L"当前默认播放设备不可用。"
                             : L"The current default playback device is unavailable.";
            break;
        case ErrorCategory::AudioServiceUnavailable:
            reason = chinese ? L"Windows 音频服务当前不可用。"
                             : L"The Windows Audio service is currently unavailable.";
            break;
        case ErrorCategory::BufferSizeNotAligned:
            reason = chinese ? L"声卡驱动不支持当前硬件缓冲区。"
                             : L"The audio driver does not support the current hardware buffer.";
            break;
        case ErrorCategory::Timeout:
            reason = chinese ? L"独占设备已启动，但 Apple Music 未继续提交音频。"
                             : L"The exclusive device started, but Apple Music stopped submitting audio.";
            break;
        case ErrorCategory::EndpointDrainTimeout:
            reason = chinese ? L"等待播放设备完成歌曲尾部播放时超时。请重试歌曲。"
                             : L"Timed out waiting for the playback device to finish the track tail. Retry the song.";
            break;
        case ErrorCategory::HookLoadFailed:
            reason = chinese
                ? L"独占音频组件（am-exclusive-hook.dll）缺失或无法加载。请检查杀毒软件隔离区，再运行 AMExclusive-Setup.exe 修复安装。"
                : L"The exclusive audio component (am-exclusive-hook.dll) is missing or could not be loaded. Check antivirus quarantine, then run AMExclusive-Setup.exe to repair the installation.";
            break;
        case ErrorCategory::ProtocolVersionMismatch:
            reason = chinese
                ? L"独占音频组件版本不一致。请修复安装后重启 Apple Music。"
                : L"The exclusive audio components have incompatible versions. Repair the installation and restart Apple Music.";
            break;
        case ErrorCategory::PipeUnavailable:
        case ErrorCategory::BrokerUnavailable:
        case ErrorCategory::AgentNotFound:
        case ErrorCategory::AgentExited:
        case ErrorCategory::AgentNotOwner:
            reason = chinese ? L"独占音频组件意外断开或未能启动。"
                             : L"The exclusive audio component disconnected or could not start.";
            break;
        case ErrorCategory::AccessDenied:
            reason = chinese ? L"Windows 拒绝了独占音频组件所需的访问权限。"
                             : L"Windows denied access required by the exclusive audio component.";
            break;
        default:
            reason = chinese ? L"初始化或输出独占音频时发生错误。"
                             : L"An error occurred while initializing or rendering exclusive audio.";
            break;
        }
        std::wstring text = reason;
        if (FAILED(result)) {
            std::wostringstream code;
            code << L"\n\n" << (chinese ? L"错误代码：0x" : L"Error code: 0x")
                 << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0')
                 << static_cast<std::uint32_t>(result);
            text += code.str();
        }
        if (playbackFailure) text += chinese
            ? L"\n\n本次音频输出已停止；独占模式保持开启。请选择或重试歌曲。"
            : L"\n\nThis audio output stopped; Exclusive mode remains enabled. Select or retry a song.";
        const wchar_t* title = L"AMExclusive";
        const wchar_t* mainInstruction =
            (error == ErrorCategory::UnsupportedLocalInt32 ||
             error == ErrorCategory::BitPerfectFormatUnavailable ||
             error == ErrorCategory::ExclusiveFormatUnavailable)
            ? (chinese ? L"无法播放" : L"Unable to play")
            : (chinese ? L"独占输出失败" : L"Exclusive output failed");
        const wchar_t* button = chinese ? L"确定" : L"OK";
        TASKDIALOG_BUTTON buttons[]{{IDOK, button}};
        TASKDIALOGCONFIG dialog{};
        dialog.cbSize = sizeof(dialog);
        dialog.hwndParent = owner;
        dialog.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
        dialog.pszWindowTitle = L"AMExclusive";
        dialog.pszMainIcon = TD_ERROR_ICON;
        dialog.pszMainInstruction = mainInstruction;
        dialog.pszContent = text.c_str();
        dialog.cButtons = static_cast<UINT>(std::size(buttons));
        dialog.pButtons = buttons;
        dialog.nDefaultButton = IDOK;
        dialog.pfCallback = ErrorDialogCallback;
        dialog.lpCallbackData = reinterpret_cast<LONG_PTR>(dialog.hwndParent);
        Log(L"error dialog show error=" +
            std::to_wstring(static_cast<unsigned>(error)) +
            L" parent=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(dialog.hwndParent)));
        if (dialog.hwndParent) SetForegroundWindow(dialog.hwndParent);
        int pressed{};
        const HRESULT hr = TaskDialogIndirect(&dialog, &pressed, nullptr, nullptr);
        Log(L"error dialog closed hr=" + std::to_wstring(hr) +
            L" button=" + std::to_wstring(pressed));
        if (FAILED(hr)) {
            // Extremely old/unusual Common Controls configurations may not
            // expose TaskDialog. Keep a final fallback rather than losing a
            // user-visible exclusive-output error entirely.
            MessageBoxW(dialog.hwndParent, text.c_str(), title,
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
    }).detach();
}

std::vector<DWORD> FindProcesses(const wchar_t* executableName) {
    std::vector<DWORD> result;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot, &entry)) {
        do {
            DWORD targetSession{}, ownSession{};
            if (_wcsicmp(entry.szExeFile, executableName) == 0 &&
                ProcessIdToSessionId(entry.th32ProcessID, &targetSession) &&
                ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) &&
                targetSession == ownSession) result.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool IsModuleLoaded(DWORD pid, const wchar_t* moduleName) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{sizeof(entry)};
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

std::filesystem::path ProcessImagePath(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD chars = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &chars)) return {};
    path.resize(chars);
    return path;
}

std::filesystem::path ProcessImagePath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    const auto image = ProcessImagePath(process);
    CloseHandle(process);
    return image;
}

constexpr wchar_t kApplePackageFamilyName[] = L"AppleInc.AppleMusicWin_nzyj5cx40ttqa";

bool SameImageFile(const std::filesystem::path& actual, const std::filesystem::path& expected) {
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE first = CreateFileW(actual.c_str(), FILE_READ_ATTRIBUTES, share, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (first == INVALID_HANDLE_VALUE) return false;
    HANDLE second = CreateFileW(expected.c_str(), FILE_READ_ATTRIBUTES, share, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    const bool same = second != INVALID_HANDLE_VALUE && GetFileInformationByHandle(first, &a) &&
        GetFileInformationByHandle(second, &b) && a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
    CloseHandle(first);
    if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
    return same;
}

bool IsInstalledAppleImage(const std::filesystem::path& image, const wchar_t* imageName) {
    if (_wcsicmp(image.filename().c_str(), imageName) != 0) return false;
    UINT32 count{}, chars{};
    const LONG sizing = GetPackagesByPackageFamily(kApplePackageFamilyName, &count, nullptr, &chars, nullptr);
    if (sizing != ERROR_INSUFFICIENT_BUFFER || count == 0 || count > 128 || chars > 65536) return false;
    std::vector<PWSTR> names(count);
    std::vector<wchar_t> buffer(chars);
    if (GetPackagesByPackageFamily(kApplePackageFamilyName, &count, names.data(), &chars, buffer.data()) != ERROR_SUCCESS)
        return false;
    for (UINT32 i = 0; i < count; ++i) {
        UINT32 pathChars{};
        if (GetPackagePathByFullName(names[i], &pathChars, nullptr) != ERROR_INSUFFICIENT_BUFFER ||
            pathChars == 0 || pathChars > 32768) continue;
        std::wstring path(pathChars, L'\0');
        if (GetPackagePathByFullName(names[i], &pathChars, path.data()) != ERROR_SUCCESS) continue;
        path.resize(pathChars - 1);
        // File ID alone also accepts a hard link placed beside untrusted DLLs.
        // Require the registered package directory's identity as well.
        if (SameImageFile(image.parent_path(), path) &&
            SameImageFile(image, std::filesystem::path(path) / imageName)) return true;
    }
    return false;
}

bool IsPackagedAppleProcess(HANDLE process, const wchar_t* imageName) {
    const auto image = ProcessImagePath(process);
    if (image.empty() || !IsInstalledAppleImage(image, imageName)) return false;
    USHORT processMachine{}, nativeMachine{};
    if (!IsWow64Process2(process, &processMachine, &nativeMachine) ||
        (processMachine != IMAGE_FILE_MACHINE_AMD64 &&
         !(processMachine == IMAGE_FILE_MACHINE_UNKNOWN && nativeMachine == IMAGE_FILE_MACHINE_AMD64))) return false;
    wchar_t family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1]{};
    UINT32 familyLength = static_cast<UINT32>(std::size(family));
    const LONG status = GetPackageFamilyName(process, &familyLength, family);
    // A legitimate unpackaged Agent is supported only after the registered
    // package's exact executable file identity has been proved above.
    return status == APPMODEL_ERROR_NO_PACKAGE ||
        (status == ERROR_SUCCESS && _wcsicmp(family, kApplePackageFamilyName) == 0);
}

bool IsPackagedAppleProcess(DWORD pid, const wchar_t* imageName) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    const bool valid = IsPackagedAppleProcess(process, imageName);
    CloseHandle(process);
    return valid;
}
bool IsAppleMusicProcess(DWORD pid) {
    return IsPackagedAppleProcess(pid, L"AppleMusic.exe");
}

ULONGLONG ProcessCreationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return value.QuadPart;
}

bool IsAppleMusicAgentProcess(DWORD pid) {
    return IsPackagedAppleProcess(pid, L"AMPLibraryAgent.exe");
}

std::filesystem::path BrokerIniPath() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return L"am-exclusive.ini";
    return std::filesystem::path(path).parent_path() / L"am-exclusive.ini";
}

bool InjectModule(DWORD pid, const std::filesystem::path& dllPath, const wchar_t* imageName) {
    const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                         PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE process = OpenProcess(access, FALSE, pid);
    if (!process) return false;
    if (!IsPackagedAppleProcess(process, imageName)) { CloseHandle(process); return false; }
    const std::wstring dll = dllPath.wstring();
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) {
        if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    const DWORD wait = WaitForSingleObject(thread, 30000);
    DWORD module{};
    if (wait == WAIT_OBJECT_0) GetExitCodeThread(thread, &module);
    CloseHandle(thread);
    if (wait == WAIT_OBJECT_0) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return wait == WAIT_OBJECT_0 && module != 0;
}

// A missing or unloadable hook DLL (antivirus quarantine is the usual cause)
// used to be invisible: the watcher skipped injection silently and the UI sat
// at "Enabled - Waiting for playback" while audio stayed on the shared path.
// Publish it as a status error once, and clear it once the hook is in place.
void ReportComponentFailure(ErrorCategory category, HRESULT hr, bool requireNoAgent = false) {
    Message status{};
    {
        std::lock_guard lock(g_mutex);
        if (requireNoAgent && g_authoritativeAgentPipe != INVALID_HANDLE_VALUE) return;
        if (g_status.error == category) return;
        ++g_status.generation;
        g_status.error = category;
        g_status.hresult = hr;
        g_status.senderPid = GetCurrentProcessId();
        GetSystemTimeAsFileTime(&g_status.timestampUtc);
        status = g_status;
    }
    Log(L"component failure published error=" +
        std::to_wstring(static_cast<unsigned>(category)) +
        L" hr=" + std::to_wstring(static_cast<unsigned long>(hr)));
    Broadcast(status, nullptr);
    if (status.enabledIntent) ShowErrorDialogAsync(category, hr, status.generation, false);
}

void ClearComponentFailure(ErrorCategory category) {
    Message status{};
    {
        std::lock_guard lock(g_mutex);
        if (g_status.error != category) return;
        ++g_status.generation;
        g_status.error = ErrorCategory::None;
        g_status.hresult = S_OK;
        g_status.senderPid = GetCurrentProcessId();
        GetSystemTimeAsFileTime(&g_status.timestampUtc);
        status = g_status;
    }
    Log(L"component failure cleared error=" +
        std::to_wstring(static_cast<unsigned>(category)));
    Broadcast(status, nullptr);
}

DWORD WINAPI ProcessWatcher(void*) {
    wchar_t brokerPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, brokerPath, static_cast<DWORD>(std::size(brokerPath)));
    const auto directory = std::filesystem::path(brokerPath).parent_path();
    const auto uiDll = directory / L"am-exclusive-ui.dll";
    const auto hookDll = directory / L"am-exclusive-hook.dll";
    DWORD boundHostPid{};
    HANDLE boundHostProcess{};
    const ULONGLONG bindDeadline = GetTickCount64() + 3000ULL;
    unsigned agentInjectionFailures = 0;

    // Bind the newest live AppleMusic.exe in this session. When the user quits
    // and relaunches within the old process's teardown, both are alive for a
    // moment; the snapshot order is undefined and binding the exiting one made
    // the companion exit as soon as it died, leaving the new instance without
    // UI injection and with no way to start another companion.
    const auto bindNewestHost = [&](DWORD excludePid) {
        DWORD bestPid{};
        HANDLE best{};
        ULONGLONG bestCreated{};
        for (DWORD pid : FindProcesses(L"AppleMusic.exe")) {
            if (pid == excludePid) continue;
            if (!IsAppleMusicProcess(pid)) {
                Log(L"watcher could not resolve AppleMusic PID=" + std::to_wstring(pid));
                continue;
            }
            HANDLE host = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                      FALSE, pid);
            if (!host) {
                Log(L"watcher could not bind AppleMusic PID=" + std::to_wstring(pid));
                continue;
            }
            if (WaitForSingleObject(host, 0) == WAIT_OBJECT_0) {
                CloseHandle(host);
                continue;
            }
            const ULONGLONG created = ProcessCreationTime(host);
            if (!best || created > bestCreated) {
                if (best) CloseHandle(best);
                best = host;
                bestPid = pid;
                bestCreated = created;
            } else {
                CloseHandle(host);
            }
        }
        if (!best) return false;
        boundHostPid = bestPid;
        boundHostProcess = best;
        Log(L"companion bound to AppleMusic host pid=" + std::to_wstring(bestPid));
        return true;
    };
    // The bound host is gone. A newer AppleMusic.exe in this session takes
    // over the binding (the relaunch race above); otherwise the companion
    // exits as before. Returns true when the watcher should keep running.
    const auto hostLost = [&](const wchar_t* why) {
        const DWORD lostPid = boundHostPid;
        CloseHandle(boundHostProcess);
        boundHostProcess = nullptr;
        boundHostPid = 0;
        agentInjectionFailures = 0;
        if (bindNewestHost(lostPid)) {
            Log(std::wstring(L"companion rebound after host ") + why + L" pid=" +
                std::to_wstring(lostPid) + L" -> " + std::to_wstring(boundHostPid));
            return true;
        }
        Log(std::wstring(L"companion exit: bound AppleMusic host ") + why + L" pid=" +
            std::to_wstring(lostPid));
        g_shutdown.store(true);
        return false;
    };

    while (!g_shutdown.load()) {
        if (boundHostProcess) {
            const DWORD hostState = WaitForSingleObject(boundHostProcess, 0);
            if (hostState == WAIT_OBJECT_0) {
                if (!hostLost(L"terminated")) break;
            } else if (hostState == WAIT_FAILED) {
                if (!hostLost(L"wait failed")) break;
            }
        }

        bool uiPresent = false;
        if (!boundHostProcess) {
            uiPresent = bindNewestHost(0);
            if (!boundHostProcess && GetTickCount64() >= bindDeadline) {
                Log(L"companion exit: no AppleMusic host appeared within startup grace");
                g_shutdown.store(true);
                break;
            }
        } else {
            uiPresent = true;
        }

        if (uiPresent &&
            !IsModuleLoaded(boundHostPid, L"am-exclusive-ui.dll") &&
            std::filesystem::exists(uiDll)) {
            const bool ok = InjectModule(boundHostPid, uiDll, L"AppleMusic.exe");
            Log(L"watcher UI injection pid=" + std::to_wstring(boundHostPid) +
                L" ok=" + std::to_wstring(ok));
        }

        bool agentPresent = false;
        bool agentHookReady = false;
        const auto agentProcesses = FindProcesses(L"AMPLibraryAgent.exe");
        const bool anyAgentPresent = !agentProcesses.empty();
        for (DWORD pid : agentProcesses) {
            if (!IsAppleMusicAgentProcess(pid)) {
                Log(L"watcher could not resolve AMPLibraryAgent PID=" + std::to_wstring(pid));
                continue;
            }
            agentPresent = true;
            agentHookReady = IsModuleLoaded(pid, L"am-exclusive-hook.dll");
            if (boundHostProcess && !agentHookReady && std::filesystem::exists(hookDll)) {
                const bool ok = InjectModule(pid, hookDll, L"AMPLibraryAgent.exe");
                if (ok) agentHookReady = true;
                Log(L"watcher Agent injection pid=" + std::to_wstring(pid) +
                    L" ok=" + std::to_wstring(ok));
            }
        }

        // Only an identity-verified agent counts (agentPresent, not
        // anyAgentPresent): a same-session process that merely carries the
        // name is never injected and must not produce a failure report.
        if (!agentPresent) {
            agentInjectionFailures = 0;
        } else if (boundHostProcess) {
            if (agentHookReady) {
                // A loaded module stays loaded even if its file is later moved
                // or renamed, so a missing file is only a failure while the
                // hook is not in the agent.
                agentInjectionFailures = 0;
                ClearComponentFailure(ErrorCategory::HookLoadFailed);
            } else if (!std::filesystem::exists(hookDll)) {
                ReportComponentFailure(ErrorCategory::HookLoadFailed,
                                       HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
            } else if (++agentInjectionFailures >= 20) {
                // Twenty consecutive 100 ms attempts: the file exists but never
                // becomes a loaded module (blocked load, wrong architecture,
                // access denied).
                ReportComponentFailure(ErrorCategory::HookLoadFailed, E_FAIL);
            }
        }

        // The companion follows the AppleMusic.exe host, never the agent. Before
        // binding, poll briefly for the AppModel-started host. After binding, wait
        // on that exact process handle instead of treating AMPLibraryAgent as an
        // alternate host, so a lingering Agent cannot keep the companion alive.
        // When the host dies, a newer AppleMusic.exe in this session is adopted
        // (quit-and-relaunch race); otherwise the companion exits and the next
        // launch gets a fresh one from the scheduled task.
        DWORD watcherDelayMs = 500;
        if (!boundHostProcess) watcherDelayMs = 25;
        else if (!anyAgentPresent) watcherDelayMs = 25;
        else if (anyAgentPresent && !agentHookReady) watcherDelayMs = 100;
        else if (agentPresent) watcherDelayMs = 500;

        if (boundHostProcess) {
            const DWORD wait = WaitForSingleObject(boundHostProcess, watcherDelayMs);
            if (wait == WAIT_OBJECT_0) {
                if (!hostLost(L"terminated")) break;
            } else if (wait == WAIT_FAILED) {
                if (!hostLost(L"wait failed")) break;
            }
        } else {
            Sleep(watcherDelayMs);
        }
    }
    if (boundHostProcess) CloseHandle(boundHostProcess);
    return 0;
}

void Log(const std::wstring& text) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wostringstream line;
    line << std::setfill(L'0') << std::setw(4) << now.wYear << L'-' << std::setw(2) << now.wMonth
         << L'-' << std::setw(2) << now.wDay << L'T' << std::setw(2) << now.wHour << L':'
         << std::setw(2) << now.wMinute << L':' << std::setw(2) << now.wSecond << L'.'
         << std::setw(3) << now.wMilliseconds << L" uptimeMs=" << GetTickCount64()
         << L" pid=" << GetCurrentProcessId() << L" tid=" << GetCurrentThreadId() << L" " << text;
    const auto logPath = BrokerIniPath().parent_path() / L"am-exclusive-broker.log";
    ammod::logging::AppendWideLine(logPath, g_logMutex, line.str(), 4ull * 1024ull * 1024ull, 2);
}

void RemoveClient(HANDLE pipe) {
    Message status{};
    bool publishStatus = false;
    bool showError = false;
    {
        std::lock_guard lock(g_mutex);
        Role removedRole = Role::Unknown;
        for (const auto& client : g_clients) {
            if (client.pipe == pipe) {
                removedRole = client.role;
                break;
            }
        }
        std::erase_if(g_clients, [pipe](const Client& client) { return client.pipe == pipe; });
        if (removedRole == Role::Agent && pipe == g_authoritativeAgentPipe) {
            g_authoritativeAgentPipe = INVALID_HANDLE_VALUE;
            {
                CoCreateGuid(&g_status.requestId);
                const bool failedWhileRunning = g_status.enabledIntent &&
                    (g_status.state == RuntimeState::Requested ||
                     g_status.state == RuntimeState::Probing ||
                     g_status.state == RuntimeState::Initializing ||
                     g_status.state == RuntimeState::AlignRetry ||
                     g_status.state == RuntimeState::Active);
                ++g_status.generation;
                g_status.state = g_status.enabledIntent
                    ? RuntimeState::WaitingForStream : RuntimeState::Off;
                g_status.error = failedWhileRunning
                    ? ErrorCategory::PipeUnavailable : ErrorCategory::None;
                g_status.hresult = failedWhileRunning
                    ? HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE) : S_OK;
                g_status.senderPid = GetCurrentProcessId();
                GetSystemTimeAsFileTime(&g_status.timestampUtc);
                status = g_status;
                publishStatus = true;
                showError = failedWhileRunning;
            }
        }
    }
    if (publishStatus) Broadcast(status, nullptr);
    if (showError) ShowErrorDialogAsync(status.error, status.hresult, status.generation);
}

std::vector<std::pair<HANDLE, DWORD>> SnapshotTargets(HANDLE source) {
    std::vector<std::pair<HANDLE, DWORD>> targets;
    std::lock_guard lock(g_mutex);
    auto append = [&](const Client& client) {
        if (client.role == Role::Agent && client.pipe != g_authoritativeAgentPipe) return;
        HANDLE duplicate{};
        if (DuplicateHandle(GetCurrentProcess(), client.pipe, GetCurrentProcess(), &duplicate,
                            0, FALSE, DUPLICATE_SAME_ACCESS)) {
            targets.emplace_back(duplicate, client.pid);
        }
    };
    if (source) {
        for (const auto& client : g_clients) if (client.pipe == source) append(client);
    }
    for (const auto& client : g_clients) if (client.pipe != source) append(client);
    return targets;
}

void Broadcast(const Message& message, HANDLE source = nullptr) {
    auto targets = SnapshotTargets(source);
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const auto [pipe, pid] = targets[index];
        if (index == 0 && source) {
            Log(L"source reply begin pid=" + std::to_wstring(pid));
            const bool written = WriteMessage(pipe, message);
            Log(L"source reply complete pid=" + std::to_wstring(pid) +
                L" ok=" + std::to_wstring(written));
            CloseHandle(pipe);
        } else {
            std::thread([pipe, pid, message] {
                Log(L"async broadcast begin pid=" + std::to_wstring(pid));
                const bool written = WriteMessage(pipe, message);
                Log(L"async broadcast complete pid=" + std::to_wstring(pid) +
                    L" ok=" + std::to_wstring(written));
                CloseHandle(pipe);
            }).detach();
        }
    }
}

void BroadcastToRole(const Message& message, Role role) {
    std::vector<std::pair<HANDLE, DWORD>> targets;
    {
        std::lock_guard lock(g_mutex);
        for (const auto& client : g_clients) {
            if (client.role != role) continue;
            if (role == Role::Agent && client.pipe != g_authoritativeAgentPipe) continue;
            HANDLE duplicate{};
            if (DuplicateHandle(GetCurrentProcess(), client.pipe, GetCurrentProcess(), &duplicate,
                                0, FALSE, DUPLICATE_SAME_ACCESS)) {
                targets.emplace_back(duplicate, client.pid);
            }
        }
    }
    for (const auto& [pipe, pid] : targets) {
        const bool written = WriteMessage(pipe, message);
        Log(L"role relay pid=" + std::to_wstring(pid) +
            L" role=" + std::to_wstring(static_cast<unsigned>(role)) +
            L" ok=" + std::to_wstring(written));
        CloseHandle(pipe);
    }
}

void PublishStatus(HANDLE source) {
    Message status{};
    {
        std::lock_guard lock(g_mutex);
        g_status.senderPid = GetCurrentProcessId();
        GetSystemTimeAsFileTime(&g_status.timestampUtc);
        status = g_status;
    }
    Broadcast(status, source);
}

void HandleMessage(const Message& incoming, HANDLE source) {
    Log(L"handle type=" + std::to_wstring(static_cast<unsigned>(incoming.type)) +
        L" role=" + std::to_wstring(static_cast<unsigned>(incoming.role)));
    if (incoming.type == MessageType::TransportIntent &&
        (incoming.role == Role::Ui || incoming.role == Role::Controller)) {
        {
            std::lock_guard lock(g_mutex);
            if (std::none_of(g_clients.begin(), g_clients.end(), [&](const Client& client) {
                    return client.pipe == source && client.role == incoming.role && client.pid == incoming.senderPid;
                })) return;
        }
        BroadcastToRole(incoming, Role::Agent);
        return;
    }
    Message status{};
    bool showError = false;
    {
        std::lock_guard lock(g_mutex);
        if (std::none_of(g_clients.begin(), g_clients.end(), [&](const Client& client) {
                return client.pipe == source && client.role == incoming.role && client.pid == incoming.senderPid;
            })) return;
        const auto intentToken = g_status.requestId;
        if (incoming.role == Role::Agent &&
            (source != g_authoritativeAgentPipe || incoming.minor < 2 ||
             !IsEqualGUID(incoming.requestId, intentToken))) return;
        if (incoming.type == MessageType::SetEnabled) {
            if (incoming.role != Role::Ui && incoming.role != Role::Controller) return;
            const bool newIntent = incoming.enabledIntent != 0;
            g_status.generation++;
            g_status.enabledIntent = newIntent ? 1 : 0;
            if (newIntent) {
                g_status.state = RuntimeState::Requested;
            } else {
                const bool liveExclusive = g_status.state == RuntimeState::Active ||
                                           g_status.state == RuntimeState::Initializing ||
                                           g_status.state == RuntimeState::AlignRetry;
                g_status.state = liveExclusive ? RuntimeState::DisablePending : RuntimeState::Off;
            }
            g_status.error = ErrorCategory::None;
            g_status.hresult = S_OK;
        } else if (incoming.type == MessageType::SetHardwareBuffer &&
                   (incoming.role == Role::Ui || incoming.role == Role::Controller)) {
            if (incoming.hardwareBufferMs >= kMinimumHardwareBufferMs &&
                incoming.hardwareBufferMs <= kMaximumHardwareBufferMs) {
                if (WriteHardwareBufferMs(incoming.hardwareBufferMs)) {
                    g_status.generation++;
                    g_status.hardwareBufferMs = incoming.hardwareBufferMs;
                    g_status.error = ErrorCategory::None;
                    g_status.hresult = S_OK;
                    Log(L"hardware buffer persisted milliseconds=" +
                        std::to_wstring(g_status.hardwareBufferMs));
                } else {
                    Log(L"hardware buffer persistence failed milliseconds=" +
                        std::to_wstring(incoming.hardwareBufferMs) + L" error=" +
                        std::to_wstring(GetLastError()));
                }
            }
        } else if (incoming.type == MessageType::SetAllowResampling &&
                   (incoming.role == Role::Ui || incoming.role == Role::Controller)) {
            if (incoming.allowResampling <= 1) {
                if (WriteAllowResampling(incoming.allowResampling != 0)) {
                    g_status.generation++;
                    g_status.allowResampling = incoming.allowResampling;
                    Log(L"allow resampling persisted enabled=" +
                        std::to_wstring(g_status.allowResampling));
                } else {
                    Log(L"allow resampling persistence failed error=" +
                        std::to_wstring(GetLastError()));
                }
            }
        } else if (incoming.type == MessageType::StatusChanged && incoming.role == Role::Agent) {
            const auto generation = std::max(g_status.generation + 1, incoming.generation);
            const auto persistedIntent = g_status.enabledIntent;
            const auto persistedHardwareBufferMs = g_status.hardwareBufferMs;
            const auto persistedAllowResampling = g_status.allowResampling;
            g_status = incoming;
            g_status.type = MessageType::StatusChanged;
            g_status.role = Role::Broker;
            g_status.generation = generation;
            // Agent lifecycle/status events describe the current processing
            // phase; they do not represent a user toggle. Preserve the
            // broker-owned intent so an Agent event cannot erase the user's
            // persisted exclusive request.
            if (incoming.role == Role::Agent) {
                g_status.enabledIntent = persistedIntent;
                g_status.hardwareBufferMs = persistedHardwareBufferMs;
                g_status.allowResampling = persistedAllowResampling;
                if (persistedIntent && incoming.state == RuntimeState::Off) {
                    g_status.state = RuntimeState::WaitingForStream;
                }
            }
        } else if (incoming.type == MessageType::AttemptFailed && incoming.role == Role::Agent) {
            const auto generation = std::max(g_status.generation + 1, incoming.generation);
            const auto persistedIntent = g_status.enabledIntent;
            const auto persistedHardwareBufferMs = g_status.hardwareBufferMs;
            const auto persistedAllowResampling = g_status.allowResampling;
            g_status = incoming;
            g_status.type = MessageType::StatusChanged;
            g_status.role = Role::Broker;
            g_status.generation = generation;
            g_status.hardwareBufferMs = persistedHardwareBufferMs;
            g_status.allowResampling = persistedAllowResampling;
            if (persistedIntent) {
                // Retire the failed attempt, not the user's AME preference.
                // The Agent receives this status and arms a fresh generation
                // for the next naturally opened song.
                g_status.enabledIntent = persistedIntent;
                g_status.state = RuntimeState::WaitingForStream;
                showError = true;
            } else {
                g_status.enabledIntent = 0;
                g_status.state = RuntimeState::Off;
            }
        } else {
            return;
        }
        if (incoming.type == MessageType::SetEnabled) {
            WritePrivateProfileStringW(L"mod", L"mode",
                g_status.enabledIntent ? L"exclusive" : L"probe", BrokerIniPath().c_str());
        }
        g_status.requestId = intentToken;
        if (incoming.type == MessageType::SetEnabled) CoCreateGuid(&g_status.requestId);
        if (!g_status.enabledIntent) {
            g_status.state = RuntimeState::Off;
            g_status.error = ErrorCategory::None;
            g_status.hresult = S_OK;
        }
        g_status.senderPid = GetCurrentProcessId();
        GetSystemTimeAsFileTime(&g_status.timestampUtc);
        status = g_status;
    }
    Broadcast(status, source);
    if (showError) ShowErrorDialogAsync(status.error, status.hresult, status.generation);
}

// Caller holds g_mutex. A connection is an authority boundary even when its
// process ID is unchanged; status broadcasts can arrive out of order.
std::vector<HANDLE> BeginAgentConnectionLocked(HANDLE pipe) {
    std::vector<HANDLE> superseded;
    for (const auto& client : g_clients)
        if (client.role == Role::Agent && client.pipe != pipe) superseded.push_back(client.pipe);
    g_authoritativeAgentPipe = pipe;
    CoCreateGuid(&g_status.requestId);
    ++g_status.generation;
    g_status.state = g_status.enabledIntent ? RuntimeState::WaitingForStream : RuntimeState::Off;
    g_status.error = ErrorCategory::None;
    g_status.hresult = S_OK;
    g_status.senderPid = GetCurrentProcessId();
    GetSystemTimeAsFileTime(&g_status.timestampUtc);
    return superseded;
}

void ClientThread(HANDLE pipe) {
    Message hello{};
    if (!ReadMessage(pipe, hello) || hello.type != MessageType::Hello) {
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }
    ULONG clientPid{};
    if (!GetNamedPipeClientProcessId(pipe, &clientPid) || clientPid != hello.senderPid) {
        Log(L"rejected pipe client with mismatched sender pid");
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }
    Log(L"hello role=" + std::to_wstring(static_cast<unsigned>(hello.role)) +
        L" pid=" + std::to_wstring(hello.senderPid));
    if ((hello.role == Role::Agent && !IsAppleMusicAgentProcess(hello.senderPid)) ||
        (hello.role == Role::Ui && !IsAppleMusicProcess(hello.senderPid))) {
        Log(L"rejected unsupported or unverified package peer");
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }
    if (hello.role == Role::Agent && hello.minor < 2) {
        ReportComponentFailure(ErrorCategory::ProtocolVersionMismatch,
                               HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), true);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }
    Message initialStatus{};
    bool publishAgentBoundary = false;
    std::vector<HANDLE> supersededAgents;
    {
        std::lock_guard lock(g_mutex);
        if (hello.role == Role::Agent) {
            supersededAgents = BeginAgentConnectionLocked(pipe);
            publishAgentBoundary = true;
        }
        g_clients.push_back({pipe, hello.role, hello.senderPid});
        initialStatus = g_status;
    }
    for (HANDLE retired : supersededAgents) DisconnectNamedPipe(retired);
    auto ack = NewMessage(MessageType::HelloAck, Role::Broker);
    ack.generation = initialStatus.generation;
    Log(L"write hello ack pid=" + std::to_wstring(hello.senderPid));
    if (!WriteMessage(pipe, ack) || !WriteMessage(pipe, initialStatus)) {
        RemoveClient(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }
    Log(L"hello complete pid=" + std::to_wstring(hello.senderPid));
    if (publishAgentBoundary) Broadcast(initialStatus, nullptr);

    Message incoming{};
    for (;;) {
        if (!ReadMessageTimeout(pipe, incoming, 250)) {
            if (GetLastError() == ERROR_TIMEOUT) continue;
            break;
        }
        if (incoming.type == MessageType::Goodbye) break;
        if (incoming.type == MessageType::GetStatus) PublishStatus(pipe);
        else HandleMessage(incoming, pipe);
    }
    RemoveClient(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

} // namespace

int wmain() {
    using namespace ammod::ipc;
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto sid = CurrentUserSid();
    if (sid.empty()) return 2;
    const auto mutexName = L"Local\\AMExclusive.Broker." + sid;
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!instanceMutex) return 5;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex);
        return 0;
    }
    wchar_t savedMode[32]{};
    GetPrivateProfileStringW(L"mod", L"mode", L"probe", savedMode,
        static_cast<DWORD>(std::size(savedMode)), BrokerIniPath().c_str());
    g_status.enabledIntent = _wcsicmp(savedMode, L"exclusive") == 0 ? 1 : 0;
    g_status.hardwareBufferMs = ReadHardwareBufferMs();
    g_status.allowResampling = ReadAllowResampling() ? 1 : 0;
    g_status.state = g_status.enabledIntent ? RuntimeState::WaitingForStream : RuntimeState::Off;
    Log(L"companion started pid=" + std::to_wstring(GetCurrentProcessId()) +
        L" persistedIntent=" + std::to_wstring(g_status.enabledIntent) +
        L" hardwareBufferMs=" + std::to_wstring(g_status.hardwareBufferMs) +
        L" allowResampling=" + std::to_wstring(g_status.allowResampling));
    const auto pipeName = PipeName();
    PipeSecurity security;
    if (pipeName.empty() || !security.IsValid()) {
        std::wcerr << L"Unable to build current-user pipe security\n";
        CloseHandle(instanceMutex);
        return 2;
    }
    std::wcout << L"Broker ready: " << pipeName << L'\n';
    if (HANDLE watcher = CreateThread(nullptr, 0, ProcessWatcher, nullptr, 0, nullptr)) {
        CloseHandle(watcher);
    } else {
        return 4;
    }

    while (!g_shutdown.load()) {
        HANDLE pipe = CreateNamedPipeW(
            pipeName.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT,
            8, 64 * 1024, 64 * 1024, 0, security.Attributes());
        if (pipe == INVALID_HANDLE_VALUE) return 3;
        bool connected = false;
        while (!g_shutdown.load()) {
            if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
                connected = true;
                break;
            }
            if (GetLastError() != ERROR_PIPE_LISTENING) break;
            Sleep(50);
        }
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }
        DWORD pipeMode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
        if (!SetNamedPipeHandleState(pipe, &pipeMode, nullptr, nullptr)) {
            CloseHandle(pipe);
            continue;
        }
        Log(L"accepted pipe client");
        std::thread(ClientThread, pipe).detach();
    }
    Log(L"companion shutdown complete");
    // No host remains. Terminate detached IPC workers without destructing mutexes under them.
    ExitProcess(0);
}
