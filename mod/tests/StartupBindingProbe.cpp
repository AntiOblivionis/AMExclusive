#include "audio_v2/AudioCoreRuntime.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace ammod::audio_v2;

void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

void* const child = reinterpret_cast<void*>(0x1000);
void* const parent = reinterpret_cast<void*>(0x2000);
void* const decoder = reinterpret_cast<void*>(0x3000);
void* const other = reinterpret_cast<void*>(0x4000);

ApplePcmFormatView Source(unsigned rate) {
    return {0x616C6163u, 3, 0, 4096, 0, 2, 0, rate};
}
ApplePcmFormatView Destination(unsigned rate) {
    return {kAppleLinearPcm, 9, 8, 1, 8, 2, 32, rate};
}
void RegisterAlac(AudioCoreRuntime& runtime, unsigned rate, void* converter = decoder) {
    // Exact first-start ordering in the real log: cookie precision precedes
    // internal-decoder registration, after the child Initialize/parent Start.
    runtime.OnConverterSourcePrecision(converter, 24, rate, 2);
    runtime.OnConverterCreated(converter, Source(rate), Destination(rate), 42, true);
}

struct Handoff {
    std::atomic<unsigned> parentCalls{}, nativeCalls{};
    std::atomic<bool> wrongParent{};
};

std::unique_ptr<AudioCoreRuntime> NewRuntime(Handoff* handoff = nullptr) {
    auto runtime = std::make_unique<AudioCoreRuntime>();
    AudioCoreRuntimeCallbacks callbacks{};
    callbacks.context = handoff;
    callbacks.nativeHandoffPreservesScheduling = true;
    if (handoff) {
        callbacks.prepareNativeGraphHandoff = [](void* context, void* unit) noexcept {
            auto& state = *static_cast<Handoff*>(context);
            state.wrongParent.store(unit != parent);
            ++state.parentCalls;
            // Deliberate stop before OpenEndpoint: this probe cannot play audio
            // or acquire a real device. Reaching here proves activation gates.
            return E_ABORT;
        };
        callbacks.prepareNativeHandoff = [](void* context, void*) noexcept {
            ++static_cast<Handoff*>(context)->nativeCalls;
            return E_ABORT;
        };
    }
    Require(runtime->Start(callbacks), "runtime worker starts");
    runtime->SetUiEnabled(true);
    return runtime;
}

void TestStartup(unsigned rate, bool commitLateFormat) {
    Handoff handoff;
    auto runtime = NewRuntime(&handoff);
    runtime->OnAudioUnitInitialized(child, nullptr, 48000, 2, 0, 0);
    runtime->OnAudioUnitStarted(parent);
    RegisterAlac(*runtime, rate);
    Require(!runtime->BoundConverter(), "cookie alone cannot claim an uncommitted graph");
    if (!commitLateFormat) {
        Sleep(60);
        Require(handoff.parentCalls == 0, "old first-start sequence stays unbound");
        std::puts("PASS: original ordering reproduced without late format commit");
        return;
    }
    runtime->OnAudioUnitInputFormatCommitted(child, rate, 2, 24, 42);
    Require(runtime->BoundConverter() == decoder, "late active-child format binds decoder");
    Require(runtime->Coordinator().SourceFormat().sampleRate == rate,
            "source rate remains unchanged");
    runtime->OnNativeClientInitialized(other, L"", AUDCLNT_SHAREMODE_SHARED, 0, nullptr, S_OK);
    runtime->OnNativeClientStarted(other, S_OK);
    const auto frames = rate / 5;
    std::vector<float> samples(frames * 2, 0.0f);
    const ApplePcmBufferView buffer{2, static_cast<unsigned>(samples.size() * sizeof(float)), samples.data()};
    const ApplePcmBufferListView list{1, &buffer};
    Require(runtime->OnPcmOutput(decoder, Destination(rate), list, frames),
            "first source PCM is accepted");
    const auto deadline = GetTickCount64() + 1500;
    while (handoff.parentCalls == 0 && GetTickCount64() < deadline) Sleep(2);
    Require(handoff.parentCalls == 1 && !handoff.wrongParent,
            "early parent Start survives late bind and reaches handoff");
    Require(handoff.nativeCalls == 0, "test stops before physical endpoint work");
    std::printf("PASS: late startup commit and early parent Start at %u Hz\n", rate);
}

int main() {
    TestStartup(48000, false);
    for (unsigned rate : {44100u, 48000u, 96000u, 192000u}) TestStartup(rate, true);
    {
        auto runtime = NewRuntime();
        runtime->OnAudioUnitInitialized(child, nullptr, 48000, 2, 0, 0);
        RegisterAlac(*runtime, 48000);
        runtime->OnAudioUnitInputFormatCommitted(other, 48000, 2, 24, 42);
        Require(!runtime->BoundConverter(), "prefetched/uninitialized unit cannot bind");
        runtime->OnAudioUnitInputFormatCommitted(child, 96000, 2, 24, 42);
        Require(!runtime->BoundConverter(), "different rate cannot bind wrong decoder");
        runtime->OnAudioUnitInputFormatCommitted(child, 48000, 2, 24, 42);
        Require(runtime->BoundConverter() == decoder, "unresolved mismatch remains recoverable");
        RegisterAlac(*runtime, 48000, other);
        runtime->OnAudioUnitInputFormatCommitted(child, 48000, 2, 24, 43);
        Require(runtime->BoundConverter() == decoder, "late prefetch cannot replace live generation");
        std::puts("PASS: unrelated unit, rate mismatch, and live-generation guards");
    }
    {
        auto runtime = NewRuntime();
        runtime->OnAudioUnitInitialized(child, nullptr, 48000, 2, 0, 0);
        runtime->OnAudioUnitStarted(parent);
        runtime->OnAudioUnitUninitialized(child);
        RegisterAlac(*runtime, 48000);
        runtime->OnAudioUnitInputFormatCommitted(child, 48000, 2, 24, 42);
        Require(!runtime->BoundConverter(), "retired graph cannot be resurrected");
        std::puts("PASS: retired-graph guard");
    }
    std::puts("All startup-binding checks passed; no audio endpoint was opened.");
}
