#include "IpcTransport.h"

#include <sddl.h>

#include <vector>

namespace ammod::ipc {

std::wstring CurrentUserSid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<std::byte> buffer(bytes);
    if (!bytes || !GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);
    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    LPWSTR text{};
    if (!ConvertSidToStringSidW(user->User.Sid, &text) || !text) return {};
    std::wstring result(text);
    LocalFree(text);
    return result;
}

std::wstring PipeName() {
    const auto sid = CurrentUserSid();
    return sid.empty() ? std::wstring{} : L"\\\\.\\pipe\\AppleMusicExclusive.v1." + sid;
}

Message NewMessage(MessageType type, Role role) {
    Message message{};
    message.type = type;
    message.role = role;
    message.senderPid = GetCurrentProcessId();
    GetSystemTimeAsFileTime(&message.timestampUtc);
    CoCreateGuid(&message.requestId);
    return message;
}

bool ReadMessage(HANDLE pipe, Message& message) {
    DWORD read{};
    if (!ReadFile(pipe, &message, sizeof(message), &read, nullptr) || read != sizeof(message)) return false;
    return IsValid(message);
}

bool ReadMessageTimeout(HANDLE pipe, Message& message, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        DWORD available{};
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available >= sizeof(Message)) return ReadMessage(pipe, message);
        if (GetTickCount64() >= deadline) {
            SetLastError(ERROR_TIMEOUT);
            return false;
        }
        Sleep(10);
    }
}

bool WriteMessage(HANDLE pipe, const Message& message) {
    if (!IsValid(message)) return false;
    DWORD written{};
    return WriteFile(pipe, &message, sizeof(message), &written, nullptr) && written == sizeof(message);
}

PipeSecurity::PipeSecurity() {
    const auto sid = CurrentUserSid();
    if (sid.empty()) return;
    const auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr)) return;
    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = descriptor_;
    attributes_.bInheritHandle = FALSE;
    valid_ = true;
}

PipeSecurity::~PipeSecurity() {
    if (descriptor_) LocalFree(descriptor_);
}

} // namespace ammod::ipc
