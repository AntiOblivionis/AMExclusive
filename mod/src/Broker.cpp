#include "BoundedLog.h"
#include "IpcProtocol.h"
#include "IpcTransport.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

using namespace ammod::ipc;

struct Client {
    HANDLE pipe{};
    Role role{Role::Unknown};
    DWORD pid{};
};

std::mutex g_mutex;
std::mutex g_logMutex;
std::atomic<bool> g_shutdown{};
std::vector<Client> g_clients;
Message g_status = [] {
    auto message = NewMessage(MessageType::StatusChanged, Role::Broker);
    message.state = RuntimeState::Off;
    return message;
}();

void Log(const std::wstring& text);
void Broadcast(const Message& message, HANDLE source);

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

std::filesystem::path ProcessImagePath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::wstring path(32768, L'\0');
    DWORD chars = static_cast<DWORD>(path.size());
    const bool queried = QueryFullProcessImageNameW(process, 0, path.data(), &chars) != FALSE;
    CloseHandle(process);
    if (!queried) return {};
    path.resize(chars);
    return path;
}

bool IsAppleMusicProcess(DWORD pid) {
    return !ProcessImagePath(pid).empty();
}

bool IsAppleMusicAgentProcess(DWORD pid) {
    return !ProcessImagePath(pid).empty();
}

std::filesystem::path BrokerIniPath() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return L"am-exclusive.ini";
    return std::filesystem::path(path).parent_path() / L"am-exclusive.ini";
}

bool InjectModule(DWORD pid, const std::filesystem::path& dllPath) {
    const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                         PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE process = OpenProcess(access, FALSE, pid);
    if (!process) return false;
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

DWORD WINAPI ProcessWatcher(void*) {
    wchar_t brokerPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, brokerPath, static_cast<DWORD>(std::size(brokerPath)));
    const auto directory = std::filesystem::path(brokerPath).parent_path();
    const auto uiDll = directory / L"am-exclusive-ui.dll";
    const auto hookDll = directory / L"am-exclusive-hook.dll";
    DWORD boundHostPid{};
    HANDLE boundHostProcess{};
    const ULONGLONG bindDeadline = GetTickCount64() + 3000ULL;
    while (!g_shutdown.load()) {
        if (boundHostProcess) {
            const DWORD hostState = WaitForSingleObject(boundHostProcess, 0);
            if (hostState == WAIT_OBJECT_0) {
                Log(L"companion exit: bound AppleMusic host terminated pid=" +
                    std::to_wstring(boundHostPid));
                g_shutdown.store(true);
                break;
            }
            if (hostState == WAIT_FAILED) {
                Log(L"companion exit: bound AppleMusic host wait failed pid=" +
                    std::to_wstring(boundHostPid));
                g_shutdown.store(true);
                break;
            }
        }

        bool uiPresent = false;
        if (!boundHostProcess) {
            const auto uiProcesses = FindProcesses(L"AppleMusic.exe");
            for (DWORD pid : uiProcesses) {
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
                boundHostPid = pid;
                boundHostProcess = host;
                uiPresent = true;
                Log(L"companion bound to AppleMusic host pid=" + std::to_wstring(pid));
                break;
            }
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
            const bool ok = InjectModule(boundHostPid, uiDll);
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
                const bool ok = InjectModule(pid, hookDll);
                if (ok) agentHookReady = true;
                Log(L"watcher Agent injection pid=" + std::to_wstring(pid) +
                    L" ok=" + std::to_wstring(ok));
            }
        }

        // The companion is a strict child of one AppleMusic.exe lifetime. Before
        // binding, poll briefly for the AppModel-started host. After binding, wait
        // on that exact process handle instead of treating AMPLibraryAgent as an
        // alternate host. This means an Agent that lingers cannot keep the
        // companion alive, and a later AppleMusic process gets a fresh companion.
        DWORD watcherDelayMs = 500;
        if (!boundHostProcess) watcherDelayMs = 25;
        else if (!anyAgentPresent) watcherDelayMs = 25;
        else if (anyAgentPresent && !agentHookReady) watcherDelayMs = 100;
        else if (agentPresent) watcherDelayMs = 500;

        if (boundHostProcess) {
            const DWORD wait = WaitForSingleObject(boundHostProcess, watcherDelayMs);
            if (wait == WAIT_OBJECT_0) {
                Log(L"companion exit: bound AppleMusic host terminated pid=" +
                    std::to_wstring(boundHostPid));
                g_shutdown.store(true);
                break;
            }
            if (wait == WAIT_FAILED) {
                Log(L"companion exit: bound AppleMusic host wait failed pid=" +
                    std::to_wstring(boundHostPid));
                g_shutdown.store(true);
                break;
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
        if (removedRole == Role::Agent) {
            const bool anyAgent = std::any_of(g_clients.begin(), g_clients.end(),
                [](const Client& client) { return client.role == Role::Agent; });
            if (!anyAgent) {
                ++g_status.generation;
                g_status.state = g_status.enabledIntent
                    ? RuntimeState::WaitingForStream : RuntimeState::Off;
                g_status.error = ErrorCategory::None;
                g_status.hresult = S_OK;
                g_status.senderPid = GetCurrentProcessId();
                GetSystemTimeAsFileTime(&g_status.timestampUtc);
                status = g_status;
                publishStatus = true;
            }
        }
    }
    if (publishStatus) Broadcast(status, nullptr);
}

std::vector<std::pair<HANDLE, DWORD>> SnapshotTargets(HANDLE source) {
    std::vector<std::pair<HANDLE, DWORD>> targets;
    std::lock_guard lock(g_mutex);
    auto append = [&](const Client& client) {
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
        BroadcastToRole(incoming, Role::Agent);
        return;
    }
    Message status{};
    {
        std::lock_guard lock(g_mutex);
        if (incoming.type == MessageType::SetEnabled) {
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
        } else if (incoming.type == MessageType::StatusChanged) {
            const auto generation = std::max(g_status.generation + 1, incoming.generation);
            const auto persistedIntent = g_status.enabledIntent;
            g_status = incoming;
            g_status.type = MessageType::StatusChanged;
            g_status.role = Role::Broker;
            g_status.generation = generation;
            // Agent lifecycle/status events describe the current processing
            // phase; they do not represent a user toggle.  Preserve the
            // broker-owned intent so an Agent's initial Off/Disabled event
            // cannot erase a persisted exclusive request.  An explicit
            // AttemptFailed remains the only Agent message that clears it.
            if (incoming.role == Role::Agent) {
                g_status.enabledIntent = persistedIntent;
                if (persistedIntent && incoming.state == RuntimeState::Off) {
                    g_status.state = RuntimeState::WaitingForStream;
                }
            }
        } else if (incoming.type == MessageType::AttemptFailed) {
            const auto generation = std::max(g_status.generation + 1, incoming.generation);
            g_status = incoming;
            g_status.type = MessageType::StatusChanged;
            g_status.role = Role::Broker;
            g_status.generation = generation;
            g_status.enabledIntent = 0;
            g_status.state = RuntimeState::FailedOff;
        }
        if (incoming.type == MessageType::SetEnabled || incoming.type == MessageType::AttemptFailed) {
            WritePrivateProfileStringW(L"mod", L"mode",
                g_status.enabledIntent ? L"exclusive" : L"probe", BrokerIniPath().c_str());
        }
        g_status.senderPid = GetCurrentProcessId();
        GetSystemTimeAsFileTime(&g_status.timestampUtc);
        status = g_status;
    }
    Broadcast(status, source);
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
    Message initialStatus{};
    bool publishAgentBoundary = false;
    {
        std::lock_guard lock(g_mutex);
        const bool knownAgentPid = hello.role == Role::Agent &&
            std::any_of(g_clients.begin(), g_clients.end(), [&](const Client& client) {
                return client.role == Role::Agent && client.pid == hello.senderPid;
            });
        if (hello.role == Role::Agent && !knownAgentPid) {
            // Runtime state is process-scoped. A newly spawned Agent must never
            // inherit Active/Initializing from a dead predecessor merely because
            // the broker is intentionally long-lived. Preserve the persisted
            // exclusive intent, but reset ownership to WaitingForStream until the
            // new Agent publishes its own lifecycle. This keeps direct-track UI
            // fencing from treating a cold first-play as a switch away from an
            // imaginary old owner.
            ++g_status.generation;
            g_status.state = g_status.enabledIntent
                ? RuntimeState::WaitingForStream : RuntimeState::Off;
            g_status.error = ErrorCategory::None;
            g_status.hresult = S_OK;
            g_status.senderPid = GetCurrentProcessId();
            GetSystemTimeAsFileTime(&g_status.timestampUtc);
            publishAgentBoundary = true;
        }
        g_clients.push_back({pipe, hello.role, hello.senderPid});
        initialStatus = g_status;
    }
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
    g_status.state = g_status.enabledIntent ? RuntimeState::WaitingForStream : RuntimeState::Off;
    Log(L"companion started pid=" + std::to_wstring(GetCurrentProcessId()) +
        L" persistedIntent=" + std::to_wstring(g_status.enabledIntent));
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
