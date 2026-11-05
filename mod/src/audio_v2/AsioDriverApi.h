#pragma once

// Minimal host-side ASIO interface declarations.
//
// Only what a single stereo output sink needs: driver loading, buffer
// create/dispose, transport start/stop and the bufferSwitch callbacks.
//
// The declarations below mirror the Steinberg ASIO SDK v2.3 headers
// (common/asio.h and common/iasiodrv.h) exactly. ASIO has no interface
// versioning and no IID negotiation: the host calls the driver through raw
// vtable slots, so a reordered, renamed, resized or truncated declaration
// silently calls the wrong method or overruns a driver-written struct.
//
// Three details are load-bearing and easy to get wrong:
//   * the IASIO method order,
//   * the numeric values of the ASIOSampleType constants (the LSB family is
//     16..20 and 24..27, not a contiguous 0-based range),
//   * the ASIOChannelInfo field order, which includes isActive.
// The SDK also forces 4-byte packing, reproduced below.
//
// This file is intentionally independent of the ASIO SDK headers so the build
// needs no external SDK checkout.

#include <windows.h>
#include <unknwn.h>

namespace ammod::audio_v2::asio {

using ASIOBool = long;
using ASIOError = long;
using ASIOSampleRate = double;

inline constexpr ASIOBool ASIOFalse = 0;
inline constexpr ASIOBool ASIOTrue = 1;

inline constexpr ASIOError ASE_OK = 0;
inline constexpr ASIOError ASE_NotPresent = -1000;
inline constexpr ASIOError ASE_HWMalfunction = -999;
inline constexpr ASIOError ASE_InvalidParameter = -998;
inline constexpr ASIOError ASE_InvalidMode = -997;
inline constexpr ASIOError ASE_SPNotAdvancing = -996;
inline constexpr ASIOError ASE_NoClock = -995;
inline constexpr ASIOError ASE_NoMemory = -994;

#pragma pack(push, 4)

// Sample container layouts. The values are the SDK's, not a 0-based sequence.
// The 32-bit aligned variants keep the significant bits in the most
// significant positions, which is exactly the canonical layout produced by the
// project's PCM mapping layer.
using ASIOSampleType = long;
enum : long {
    ASIOSTInt16MSB = 0,
    ASIOSTInt24MSB = 1,
    ASIOSTInt32MSB = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,
    ASIOSTInt32MSB16 = 8,
    ASIOSTInt32MSB18 = 9,
    ASIOSTInt32MSB20 = 10,
    ASIOSTInt32MSB24 = 11,
    ASIOSTInt16LSB = 16,
    ASIOSTInt24LSB = 17,
    ASIOSTInt32LSB = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTFloat64LSB = 20,
    ASIOSTInt32LSB16 = 24,
    ASIOSTInt32LSB18 = 25,
    ASIOSTInt32LSB20 = 26,
    ASIOSTInt32LSB24 = 27,
};

struct ASIOSamples final {
    unsigned long hi;
    unsigned long lo;
};

struct ASIOTimeStamp final {
    unsigned long hi;
    unsigned long lo;
};

struct ASIOTimeCode final {
    double speed;
    ASIOSamples timeCodeSamples;
    unsigned long flags;
    char future[64];
};

struct ASIOTime final {
    long reserved[4];
    ASIOTimeCode timeCode;
};

struct ASIOClockSource final {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
};

struct ASIOChannelInfo final {
    long channel;
    ASIOBool isInput;
    // Written by the driver. Omitting this field shifts channelGroup/type/name
    // and makes the driver overrun the struct.
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
};

struct ASIOBufferInfo final {
    ASIOBool isInput;
    long channelNum;
    // Filled in by the driver during createBuffers: buffers[0] and buffers[1]
    // are the two halves of the driver-owned double buffer for this channel.
    void* buffers[2];
};

struct ASIOCallbacks final {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex,
                                      ASIOBool directProcess);
};

struct IASIO : IUnknown {
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* message) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize,
                                    long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                                    long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

#pragma pack(pop)

inline constexpr long kAsioSelectorSupported = 1;
inline constexpr long kAsioEngineVersion = 2;
inline constexpr long kAsioResetRequest = 3;
inline constexpr long kAsioBufferSizeChange = 4;
inline constexpr long kAsioResyncRequest = 5;
inline constexpr long kAsioLatenciesChanged = 6;
inline constexpr long kAsioSupportsTimeInfo = 7;
inline constexpr long kAsioSupportsTimeCode = 8;

// The driver is chosen at runtime from the panel. ASIO drivers register their
// CLSID under HKLM\SOFTWARE\ASIO\<name>; LocalOutputDevices enumerates them
// and the sink loads the selected one with CoCreateInstance.

} // namespace ammod::audio_v2::asio
