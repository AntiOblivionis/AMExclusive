#pragma once

#include "IpcProtocol.h"

#include <windows.h>

#include <string>

namespace ammod::ipc {

std::wstring CurrentUserSid();
std::wstring PipeName();
Message NewMessage(MessageType type, Role role);
bool ReadMessage(HANDLE pipe, Message& message);
bool ReadMessageTimeout(HANDLE pipe, Message& message, DWORD timeoutMs);
bool WriteMessage(HANDLE pipe, const Message& message);

class PipeSecurity final {
public:
    PipeSecurity();
    ~PipeSecurity();
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    SECURITY_ATTRIBUTES* Attributes() noexcept { return valid_ ? &attributes_ : nullptr; }
    bool IsValid() const noexcept { return valid_; }

private:
    SECURITY_ATTRIBUTES attributes_{};
    PSECURITY_DESCRIPTOR descriptor_{};
    bool valid_{};
};

} // namespace ammod::ipc
