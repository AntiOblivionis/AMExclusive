#pragma once

#include <windows.h>
#include <audioclient.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace ammod::ipc {

inline constexpr std::uint32_t kMagic = 0x50494D41; // "AMIP" little-endian
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 0;
inline constexpr std::size_t kEndpointIdChars = 256;
inline constexpr std::size_t kEndpointNameChars = 128;
inline constexpr std::size_t kFormatChars = 160;
inline constexpr std::size_t kDetailChars = 256;

enum class Role : std::uint16_t {
    Unknown = 0,
    Broker = 1,
    Ui = 2,
    Agent = 3,
    Controller = 4,
};

enum class MessageType : std::uint16_t {
    Invalid = 0,
    Hello = 1,
    HelloAck = 2,
    SetEnabled = 3,
    GetStatus = 4,
    StatusChanged = 5,
    AttemptFailed = 6,
    Heartbeat = 7,
    Goodbye = 8,
    // Ephemeral UI transport intent. It is relayed by the Broker to the Agent
    // and never mutates the persisted exclusive-mode status. The existing
    // Message layout is deliberately reused: `detail` carries the action and
    // `generation` carries the seek gesture epoch.
    TransportIntent = 9,
};

enum class RuntimeState : std::uint16_t {
    Off = 0,
    Requested = 1,
    WaitingForStream = 2,
    Probing = 3,
    Initializing = 4,
    AlignRetry = 5,
    Active = 6,
    DisablePending = 7,
    FailedOff = 8,
};

enum class ErrorCategory : std::uint16_t {
    None = 0,
    ProtocolVersionMismatch,
    MalformedMessage,
    AccessDenied,
    PipeUnavailable,
    BrokerUnavailable,
    UiVersionUnsupported,
    HookLoadFailed,
    AgentNotFound,
    AgentExited,
    AgentNotOwner,
    EndpointUnavailable,
    DeviceChanged,
    DeviceInvalidated,
    FormatUnsupported,
    ExclusiveNotAllowed,
    DeviceInUse,
    AudioServiceUnavailable,
    BufferSizeNotAligned,
    InitializeFailed,
    Timeout,
    Internal,
};

#pragma pack(push, 1)
struct Message {
    std::uint32_t magic{kMagic};
    std::uint16_t major{kProtocolMajor};
    std::uint16_t minor{kProtocolMinor};
    MessageType type{MessageType::Invalid};
    Role role{Role::Unknown};
    std::uint32_t bytes{sizeof(Message)};
    GUID requestId{};
    FILETIME timestampUtc{};
    std::uint32_t senderPid{};
    std::uint32_t uiPid{};
    std::uint32_t agentPid{};
    std::uint64_t generation{};
    RuntimeState state{RuntimeState::Off};
    ErrorCategory error{ErrorCategory::None};
    HRESULT hresult{S_OK};
    std::uint8_t enabledIntent{};
    std::uint8_t reserved[7]{};
    wchar_t endpointId[kEndpointIdChars]{};
    wchar_t endpointName[kEndpointNameChars]{};
    wchar_t format[kFormatChars]{};
    wchar_t detail[kDetailChars]{};
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<Message>);
static_assert(sizeof(Message) < 4096);

inline bool IsValid(const Message& message) noexcept {
    return message.magic == kMagic && message.major == kProtocolMajor &&
           message.bytes == sizeof(Message) && message.type != MessageType::Invalid &&
           message.role != Role::Unknown;
}

inline ErrorCategory CategorizeAudioError(HRESULT hr) noexcept {
    switch (hr) {
    case S_OK: return ErrorCategory::None;
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return ErrorCategory::FormatUnsupported;
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return ErrorCategory::ExclusiveNotAllowed;
    case AUDCLNT_E_DEVICE_IN_USE: return ErrorCategory::DeviceInUse;
    case AUDCLNT_E_DEVICE_INVALIDATED: return ErrorCategory::DeviceInvalidated;
    case AUDCLNT_E_SERVICE_NOT_RUNNING: return ErrorCategory::AudioServiceUnavailable;
    case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED: return ErrorCategory::BufferSizeNotAligned;
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED: return ErrorCategory::EndpointUnavailable;
    default: return ErrorCategory::InitializeFailed;
    }
}

inline std::wstring_view MessageKey(ErrorCategory category) noexcept {
    switch (category) {
    case ErrorCategory::None: return L"none";
    case ErrorCategory::FormatUnsupported: return L"format_unsupported";
    case ErrorCategory::ExclusiveNotAllowed: return L"exclusive_not_allowed";
    case ErrorCategory::DeviceInUse: return L"device_in_use";
    case ErrorCategory::DeviceChanged: return L"device_changed";
    case ErrorCategory::DeviceInvalidated: return L"device_invalidated";
    case ErrorCategory::AudioServiceUnavailable: return L"audio_service_unavailable";
    case ErrorCategory::BufferSizeNotAligned: return L"buffer_size_not_aligned";
    case ErrorCategory::EndpointUnavailable: return L"endpoint_unavailable";
    case ErrorCategory::HookLoadFailed: return L"hook_load_failed";
    case ErrorCategory::PipeUnavailable:
    case ErrorCategory::BrokerUnavailable: return L"broker_unavailable";
    default: return L"initialization_error";
    }
}

} // namespace ammod::ipc
