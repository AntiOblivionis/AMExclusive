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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    SineSource(std::uint32_t rate, std::uint32_t seconds, std::uint32_t bits)
        : rate_(rate), total_(static_cast<std::uint64_t>(rate) * seconds),
          shift_(32u - bits) {}

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
            const std::int32_t canonical = sample << shift_;
            destination[frame * 2u] = canonical;
            destination[frame * 2u + 1u] = canonical;
        }
        produced_ += n;
        writtenFrames = n;
        endOfStream = produced_ >= total_;
        // Produce at 1.0x like Apple's realtime decode. Without this the whole
        // track appears instantly, the producer reaches the first track
        // boundary before Start has queued anything, and the test exercises a
        // path that cannot happen in production.
        const auto now = std::chrono::steady_clock::now();
        if (!paced_) {
            start_ = now;
            paced_ = true;
        }
        const auto due = start_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(static_cast<double>(produced_) /
                                          static_cast<double>(rate_)));
        if (due > now) std::this_thread::sleep_until(due);
        return S_OK;
    }

private:
    std::uint32_t rate_{};
    std::uint64_t total_{};
    std::uint32_t shift_{16};
    std::uint64_t produced_{};
    bool paced_{};
    std::chrono::steady_clock::time_point start_{};
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

// Simulates Apple Music's actual playlist behaviour across a format change:
//   * PCM arrives at 1.0x from a per-track decoded source;
//   * the runtime hands each track its own graph (rate/bit depth);
//   * at track EOF the old graph is torn down (Stop) and the runtime rebinds
//     by closing and re-opening the SAME sink object with a new format;
//   * OpenHome receives a fresh DeleteAll + Insert + Play for the new item.
// Unlike the single-track test, this exercises the live Close+Open path that
// keeps the HTTP server and alias alive across a format change.
int RunPlaylistSimulation(const std::wstring& location, int seconds,
                          std::uint32_t bufferMs) {
    struct Track {
        std::uint32_t rate;
        std::uint16_t bits;
        const wchar_t* name;
    };
    const Track tracks[] = {
        {44100u, 16u, L"44.1 kHz / 16 bit"},
        {192000u, 24u, L"192 kHz / 24 bit"},
    };
    DlnaSink sink; // ONE object, reused exactly like the runtime reuses it
    bool allOk = true;
    for (std::size_t i = 0; i < std::size(tracks); ++i) {
        const auto& track = tracks[i];
        SineSource source(track.rate, static_cast<std::uint32_t>(seconds),
                          track.bits);
        AudioSinkOpenParams params{};
        params.format.sampleRate = track.rate;
        params.format.channels = 2;
        params.format.validBits = track.bits;
        params.format.containerBits = track.bits;
        params.format.encoding = PcmEncoding::SignedInteger;
        params.format.interleaved = true;
        params.format.sourceValidBits = track.bits;
        params.streamShape = DlnaStreamShape::FlacLive;
        // SMTC would report the exact track duration; use it as the boundary.
        params.streamSeconds = static_cast<std::uint32_t>(seconds);
        params.streamBufferMs = bufferMs;
        params.allowResampling = false;
        params.dlna.location = location;

        const HRESULT opened = sink.Open(params);
        std::wprintf(L"[playlist] track %llu %ls: Open -> 0x%08lX "
                     L"negotiated rate=%u bits=%u\n",
                     static_cast<unsigned long long>(i + 1), track.name, opened,
                     sink.Format().sampleRate, sink.Format().validBits);
        std::fflush(stdout);
        if (FAILED(opened)) return 1;

        const HRESULT started = sink.Start(source);
        std::wprintf(L"[playlist] track %llu: Start -> 0x%08lX state=%ls\n",
                     static_cast<unsigned long long>(i + 1), started,
                     StateName(sink.State()));
        std::fflush(stdout);
        if (FAILED(started)) return 1;

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(seconds + 20);
        for (int tick = 2;; tick += 2) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const auto stats = sink.Stats();
            std::wprintf(L"[playlist] t=%ds state=%ls submitted=%llu "
                         L"bytesServed=%llu buffered=%llu errors=%llu\n",
                         tick, StateName(stats.state),
                         static_cast<unsigned long long>(stats.submittedFrames),
                         static_cast<unsigned long long>(stats.bytesServed),
                         static_cast<unsigned long long>(stats.bufferedFrames),
                         static_cast<unsigned long long>(stats.controlFailures));
            std::fflush(stdout);
            if (sink.EndOfStreamComplete() ||
                std::chrono::steady_clock::now() > deadline) break;
        }
        const auto finalStats = sink.Stats();
        const bool ok = finalStats.submittedFrames > 0 &&
                        finalStats.bytesServed > 0 &&
                        finalStats.state != AudioSinkState::Faulted;
        std::wprintf(L"[playlist] track %llu %ls: submitted=%llu "
                     L"bytesServed=%llu lastError=0x%08lX -> %ls\n",
                     static_cast<unsigned long long>(i + 1), track.name,
                     static_cast<unsigned long long>(finalStats.submittedFrames),
                     static_cast<unsigned long long>(finalStats.bytesServed),
                     finalStats.lastError, ok ? L"OK" : L"FAIL");
        std::fflush(stdout);
        allOk = allOk && ok;

        // AM: streaming producer EOF -> runtime tears down the graph and
        // rebinds for the next format. Stop + Close on the same sink object.
        std::wprintf(L"[playlist] track %llu: EOF -> Stop + Close (runtime rebind)\n",
                     static_cast<unsigned long long>(i + 1));
        std::fflush(stdout);
        sink.Stop();
        sink.Close();
    }
    std::wprintf(L"[playlist] %ls\n", allOk ? L"PASS" : L"FAIL");
    std::fflush(stdout);
    return allOk ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring location =
        argc > 1 ? argv[1] : L"http://10.2.0.37:49812/desc.xml";

    // Optional second argument: how many seconds of synthetic audio to produce.
    // A longer run is what shows whether a renderer keeps pulling at its own
    // pace instead of reading a fixed probe amount and giving up.
    const int seconds = argc > 2 ? std::max(4, _wtoi(argv[2])) : 8;
    const std::wstring shapeText = argc > 3 ? argv[3] : L"wav";
    const std::uint32_t rate = argc > 4 ? static_cast<std::uint32_t>(_wtoi(argv[4])) : 44100u;
    const std::uint32_t bits = argc > 5 ? static_cast<std::uint32_t>(_wtoi(argv[5])) : 16u;
    // Optional: declared track length and pull-buffer depth. A track length
    // shorter than the produced audio exercises live auto-advance repeatedly.
    const int trackSeconds = argc > 6
        ? std::max(1, _wtoi(argv[6])) : seconds;
    const std::uint32_t bufferMs = argc > 7
        ? static_cast<std::uint32_t>(std::clamp(_wtoi(argv[7]), 300, 10000)) : 1500u;
    if (shapeText == L"playlist") {
        return RunPlaylistSimulation(location, seconds, bufferMs);
    }
    SineSource source(rate, static_cast<std::uint32_t>(seconds), bits);
    DlnaSink sink;

    AudioSinkOpenParams params{};
    params.format.sampleRate = rate;
    params.format.channels = 2;
    params.format.validBits = static_cast<std::uint16_t>(bits);
    params.format.containerBits = static_cast<std::uint16_t>(bits);
    params.format.encoding = PcmEncoding::SignedInteger;
    params.format.interleaved = true;
    params.format.sourceValidBits = static_cast<std::uint16_t>(bits);
    params.streamShape = DlnaStreamShapeFromText(shapeText.c_str());
    // Without Apple Music there is no SMTC timeline, so the declared length has
    // to come from the test itself or the file shape would advertise a guess.
    params.streamSeconds = static_cast<std::uint32_t>(trackSeconds);
    params.streamBufferMs = bufferMs;
    params.allowResampling = false;
    params.dlna.location = location;

    const HRESULT opened = sink.Open(params);
    std::wprintf(L"[selftest] shape=%ls rate=%u bits=%u Open(%ls) -> 0x%08lX\n",
                 shapeText.c_str(), rate, bits, location.c_str(), opened);
    std::fflush(stdout);
    if (FAILED(opened)) {
        std::wprintf(L"[selftest] FAIL: Open returned 0x%08lX\n", opened);
        return 1;
    }
    std::wprintf(L"[selftest] negotiated format rate=%u ch=%u validBits=%u containerBits=%u\n",
                 sink.Format().sampleRate, sink.Format().channels,
                 sink.Format().validBits, sink.Format().containerBits);

    std::fflush(stdout);
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
