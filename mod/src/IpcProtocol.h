#pragma once

#include "AudioErrors.h"

#include <windows.h>
#include <audioclient.h>

#include <cstdint>
#include <type_traits>

namespace ammod::ipc {

inline constexpr std::uint32_t kMagic = 0x50494D41; // "AMIP" little-endian
inline constexpr std::uint16_t kProtocolMajor = 2;
inline constexpr std::uint16_t kProtocolMinor = 1;
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
    SetHardwareBuffer = 10,
    SetAllowResampling = 11,
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
    UnsupportedLocalInt32,
    BitPerfectFormatUnavailable,
    ExclusiveFormatUnavailable,
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
    std::uint32_t hardwareBufferMs{20};
    std::uint8_t enabledIntent{};
    // Protocol 2.1 uses a formerly reserved byte; 2.0 peers leave this false.
    std::uint8_t allowResampling{};
    std::uint8_t reserved[6]{};
    wchar_t endpointId[kEndpointIdChars]{};
    wchar_t endpointName[kEndpointNameChars]{};
    wchar_t format[kFormatChars]{};
    wchar_t detail[kDetailChars]{};
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<Message>);
static_assert(sizeof(Message) == 1680, "Keep the protocol 2.x wire layout stable");
static_assert(sizeof(Message) < 4096);

inline bool IsValid(const Message& message) noexcept {
    return message.magic == kMagic && message.major == kProtocolMajor &&
           message.bytes == sizeof(Message) && message.type != MessageType::Invalid &&
           message.role != Role::Unknown;
}

inline ErrorCategory CategorizeAudioError(HRESULT hr) noexcept {
    switch (hr) {
    case S_OK: return ErrorCategory::None;
    case ammod::audio::kUnsupportedLocalInt32:
        return ErrorCategory::UnsupportedLocalInt32;
    case ammod::audio::kBitPerfectFormatUnavailable:
        return ErrorCategory::BitPerfectFormatUnavailable;
    case ammod::audio::kExclusiveFormatUnavailable:
        return ErrorCategory::ExclusiveFormatUnavailable;
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


} // namespace ammod::ipc
