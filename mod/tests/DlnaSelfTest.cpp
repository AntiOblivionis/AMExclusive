// End-to-end self test for the DLNA/OpenHome control point + stream server.
//
// Drives DlnaSink with a synthetic integer PCM source against a local OpenHome
// renderer (tools/oh_test_renderer.py by default), completely bypassing Apple
// Music, the broker and the UI. This validates the whole AM-side chain that
// the production hook uses: LoadDescription -> GetSinkProtocolInfo ->
// BuildFormatCandidates -> SetupStream -> ProducerMain/PackChunk ->
// DlnaStreamSession ring -> chunked HTTP -> OpenHome Insert/Play -> renderer
// pull. Exit code 0 means the renderer actually received and drained bytes.
//
// Build:  cmake --build build --config Release --target am_exclusive_dlna_selftest
// Run:    build\Release\am_exclusive_dlna_selftest.exe [renderer-desc-url]

#include "audio_v2/DlnaSink.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>

using namespace ammod::audio_v2;

namespace {

// Deterministic 440 Hz stereo sine, canonical full-scale int32 (the same
// lattice PackCanonicalPcm32 / PackLpcmNetworkOrder expect: 16-bit content
// left-justified into int32).
class SineSource final : public IIntegerPcmSource {
public:
    SineSource(std::uint32_t rate, std::uint32_t seconds)
        : rate_(rate), total_(static_cast<std::uint64_t>(rate) * seconds) {}

    HRESULT Prepare(std::uint32_t capacityFrames) noexcept override {
        (void)capacityFrames;
        return produced_ >= total_ ? S_FALSE : S_OK;
    }

    HRESULT Fill(std::int32_t* destination, std::uint32_t capacityFrames,
                 std::uint32_t& writtenFrames, bool& endOfStream) noexcept override {
        writtenFrames = 0;
        endOfStream = false;
        if (!destination || capacityFrames == 0) return E_INVALIDARG;
        const std::uint64_t remaining = total_ - produced_;
        const std::uint32_t n =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(capacityFrames, remaining));
        for (std::uint32_t frame = 0; frame < n; ++frame) {
            const auto index = produced_ + frame;
            const auto sample = static_cast<std::int32_t>(
                12000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 *
                                   static_cast<double>(index) / static_cast<double>(rate_)));
            const std::int32_t canonical = sample << 16;
            destination[frame * 2u] = canonical;
            destination[frame * 2u + 1u] = canonical;
        }
        produced_ += n;
        writtenFrames = n;
        endOfStream = produced_ >= total_;
        return S_OK;
    }

private:
    std::uint32_t rate_{};
    std::uint64_t total_{};
    std::uint64_t produced_{};
};

const wchar_t* StateName(AudioSinkState state) {
    switch (state) {
    case AudioSinkState::Closed: return L"Closed";
    case AudioSinkState::Open: return L"Open";
    case AudioSinkState::Running: return L"Running";
    case AudioSinkState::Faulted: return L"Faulted";
    }
    return L"?";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring location =
        argc > 1 ? argv[1] : L"http://10.2.0.37:49812/desc.xml";

    // Optional second argument: how many seconds of synthetic audio to produce.
    // A longer run is what shows whether a renderer keeps pulling at its own
    // pace instead of reading a fixed probe amount and giving up.
    const int seconds = argc > 2 ? std::max(4, _wtoi(argv[2])) : 8;
    SineSource source(44100, static_cast<std::uint32_t>(seconds));
    DlnaSink sink;

    AudioSinkOpenParams params{};
    params.format.sampleRate = 44100;
    params.format.channels = 2;
    params.format.validBits = 16;
    params.format.containerBits = 16;
    params.format.encoding = PcmEncoding::SignedInteger;
    params.format.interleaved = true;
    params.format.sourceValidBits = 16;
    params.streamBufferMs = 1500;
    params.allowResampling = false;
    params.dlna.location = location;

    const HRESULT opened = sink.Open(params);
    std::wprintf(L"[selftest] Open(%ls) -> 0x%08lX\n", location.c_str(), opened);
    if (FAILED(opened)) {
        std::wprintf(L"[selftest] FAIL: Open returned 0x%08lX\n", opened);
        return 1;
    }
    std::wprintf(L"[selftest] negotiated format rate=%u ch=%u validBits=%u containerBits=%u\n",
                 sink.Format().sampleRate, sink.Format().channels,
                 sink.Format().validBits, sink.Format().containerBits);

    const HRESULT started = sink.Start(source);
    std::wprintf(L"[selftest] Start -> 0x%08lX state=%ls\n", started, StateName(sink.State()));
    if (FAILED(started)) {
        std::wprintf(L"[selftest] FAIL: Start returned 0x%08lX\n", started);
        sink.Close();
        return 1;
    }

    for (int i = 0; i < 6 || (i * 2) < seconds + 4; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const auto stats = sink.Stats();
        std::wprintf(L"[selftest] t=%ds state=%ls submittedFrames=%llu bytesServed=%llu "
                     L"bufferedFrames=%llu controlFailures=%llu lastError=0x%08lX\n",
                     (i + 1) * 2, StateName(stats.state),
                     static_cast<unsigned long long>(stats.submittedFrames),
                     static_cast<unsigned long long>(stats.bytesServed),
                     static_cast<unsigned long long>(stats.bufferedFrames),
                     static_cast<unsigned long long>(stats.controlFailures),
                     stats.lastError);
    }

    const auto finalStats = sink.Stats();
    sink.Stop();
    sink.Close();

    const bool ok = finalStats.bytesServed > 0 &&
                    finalStats.state != AudioSinkState::Faulted;
    std::wprintf(L"[selftest] %ls: bytesServed=%llu submittedFrames=%llu\n",
                 ok ? L"PASS" : L"FAIL",
                 static_cast<unsigned long long>(finalStats.bytesServed),
                 static_cast<unsigned long long>(finalStats.submittedFrames));
    return ok ? 0 : 1;
}
