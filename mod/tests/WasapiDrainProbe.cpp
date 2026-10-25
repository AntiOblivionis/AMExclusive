// Hardware regression probe. Uses silence on the current default endpoint.
// Build target am_exclusive_drain_probe, then run with --run-on-default-device.
#include "audio_v2/WasapiExclusiveSink.h"
#include <cstdio>
#include <cstring>
using namespace ammod::audio_v2;

struct Source final : IIntegerPcmSource {
    unsigned count{};
    unsigned total{21}; // Fill() calls; the last one is terminal.
    bool wait{};
    unsigned tail{}; // 0 = empty, 1 = partial, 2 = full terminal packet
    ULONGLONG until{};
    bool initialWait{};
    ULONGLONG initialUntil{};
    bool sealedEmpty{}; // EOF sealed with no PCM ever available.
    HRESULT Prepare(std::uint32_t) noexcept override {
        if (initialWait && count == 0) {
            if (!initialUntil) initialUntil = GetTickCount64() + 200;
            if (GetTickCount64() < initialUntil) return kAudioSourceWouldBlock;
        }
        if (sealedEmpty) return S_FALSE;
        if (count >= total) return E_UNEXPECTED; // No source reads during drain.
        if (wait && count == total - 1) {
            if (!until) until = GetTickCount64() + 200;
            if (GetTickCount64() < until) return kAudioSourceWouldBlock;
        }
        return S_OK;
    }
    HRESULT Fill(std::int32_t* p, std::uint32_t n,
                 std::uint32_t& written, bool& eos) noexcept override {
        if (sealedEmpty || count >= total) return E_UNEXPECTED;
        eos = ++count == total;
        written = eos ? (tail == 2 ? n : tail == 1 ? n / 2 : 0) : n;
        std::memset(p, 0, written * 2 * sizeof(*p));
        return S_OK;
    }
};

bool Run(unsigned rate, unsigned bits, bool wait, unsigned tail, bool pause,
         bool notified = false, bool initialWait = false, unsigned fills = 21,
         bool sealedEmpty = false) {
    struct Event {
        HANDLE handle{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        ~Event() { if (handle) CloseHandle(handle); }
    } ready;
    if (!ready.handle) return false;
    Source source;
    source.wait = wait;
    source.initialWait = initialWait;
    source.tail = tail;
    source.total = fills;
    source.sealedEmpty = sealedEmpty;
    WasapiExclusiveSink sink;
    WasapiSinkConfig config;
    config.format = {rate, 2, static_cast<std::uint16_t>(bits), static_cast<std::uint16_t>(bits)};
    config.periodFrames = rate * 30 / 1000;
    config.sourceReadyEvent = notified ? ready.handle : nullptr;
    auto hr = sink.Open(config);
    std::printf("OPEN rate=%u bits=%u wait=%d tail=%u pause=%d notify=%d initialWait=%d fills=%u sealedEmpty=%d hr=%08lx frames=%u\n",
        rate, bits, wait, tail, pause, notified, initialWait, fills, sealedEmpty, hr, sink.BufferFrames());
    if (FAILED(hr)) return false;
    hr = sink.Start(source);
    const auto deadline = GetTickCount64() + 5000;
    bool paused = false;
    while (SUCCEEDED(hr) && !sink.EndOfStreamComplete() &&
           sink.State() != WasapiSinkState::Faulted && GetTickCount64() < deadline) {
        if (notified && sink.WaitingForSource()) SetEvent(ready.handle);
        if (pause && !paused && sink.EndOfStreamSubmitted()) {
            sink.Pause();
            Sleep(100);
            hr = sink.Start(source);
            paused = true;
        }
        Sleep(5);
    }
    const bool complete = sink.EndOfStreamComplete();
    sink.Stop(); // Join before inspecting the source's non-atomic count.
    const auto stats = sink.Stats();
    // Ordering invariants, every case: the endpoint is never started before
    // its first primed buffer (first Start follows first ReleaseBuffer), and
    // a completed stream that submitted frames was certified by a started
    // clock; one that never submitted must never have started the endpoint.
    const bool startOrder = stats.startsBeforeFirstBuffer == 0 &&
        (stats.submittedFrames == 0 ? stats.clientStarts == 0
                                    : !complete || stats.clientStarts >= 1);
    const bool passed = SUCCEEDED(hr) && complete && SUCCEEDED(stats.lastError) &&
        source.count == fills && stats.underruns == 0 && (!wait || stats.sourceWaits > 0) &&
        (!pause || paused) && startOrder;
    std::printf("RESULT %s complete=%d hr=%08lx waits=%llu fills=%u starts=%llu earlyStarts=%llu\n",
        passed ? "PASS" : "FAIL", complete, stats.lastError, stats.sourceWaits,
        source.count, stats.clientStarts, stats.startsBeforeFirstBuffer);
    return passed;
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--run-on-default-device") != 0) {
        std::puts("Explicit opt-in required: --run-on-default-device (exclusive silent audio)");
        return 2;
    }
    int failures = 0;
    int cases = 0;
    const auto run = [&](unsigned rate, unsigned bits, bool wait, unsigned tail,
                         bool pause, bool notified = false, bool initialWait = false,
                         unsigned fills = 21, bool sealedEmpty = false) {
        ++cases;
        if (!Run(rate, bits, wait, tail, pause, notified, initialWait, fills, sealedEmpty))
            ++failures;
    };
    for (unsigned rate : {192000u, 96000u, 44100u}) {
        for (bool wait : {false, true}) {
            for (unsigned tail : {0u, 1u, 2u})
                run(rate, 24, wait, tail, false);
            run(rate, 24, wait, 1, true);
            run(rate, 16, wait, 1, false);
            run(rate, 24, wait, 1, false, true, true);
        }
        // The very first packet is terminal (empty, partial, full): the EOS
        // drain must wait for the first Start; a stale pre-Start clock
        // position must not certify it (complete requires clientStarts >= 1).
        for (unsigned tail : {0u, 1u, 2u})
            run(rate, 24, false, tail, false, false, false, 1);
        // Terminal first packet arriving after an initial wait: the resume
        // path must keep first-start priming (fill, then Start).
        run(rate, 24, false, 2, false, true, true, 1);
        // EOF sealed with no PCM at all: clean empty completion, and the
        // endpoint must never be started (nothing to drain) - immediately,
        // and after an initial wait (Prepare() == S_FALSE on the resume path).
        run(rate, 24, false, 0, false, false, false, 0, true);
        run(rate, 24, false, 0, false, true, true, 0, true);
    }
    std::printf("%d hardware cases: %d failed\n", cases, failures);
    return failures ? 1 : 0;
}
