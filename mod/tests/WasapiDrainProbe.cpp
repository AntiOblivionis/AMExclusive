// Hardware regression probe. Uses silence on the current default endpoint.
// Build target am_exclusive_drain_probe, then run with --run-on-default-device.
#include "audio_v2/WasapiExclusiveSink.h"
#include <cstdio>
#include <cstring>
using namespace ammod::audio_v2;

struct Source final : IIntegerPcmSource {
    unsigned count{};
    bool wait{};
    unsigned tail{}; // 0 = empty, 1 = partial, 2 = full terminal packet
    ULONGLONG until{};
    bool initialWait{};
    ULONGLONG initialUntil{};
    HRESULT Prepare(std::uint32_t) noexcept override {
        if (count >= 21) return E_UNEXPECTED; // No source reads during drain.
        if (initialWait && count == 0) {
            if (!initialUntil) initialUntil = GetTickCount64() + 200;
            if (GetTickCount64() < initialUntil) return kAudioSourceWouldBlock;
        }
        if (wait && count == 20) {
            if (!until) until = GetTickCount64() + 200;
            if (GetTickCount64() < until) return kAudioSourceWouldBlock;
        }
        return S_OK;
    }
    HRESULT Fill(std::int32_t* p, std::uint32_t n,
                 std::uint32_t& written, bool& eos) noexcept override {
        if (count >= 21) return E_UNEXPECTED;
        eos = ++count == 21;
        written = eos ? (tail == 2 ? n : tail == 1 ? n / 2 : 0) : n;
        std::memset(p, 0, written * 2 * sizeof(*p));
        return S_OK;
    }
};

bool Run(unsigned rate, unsigned bits, bool wait, unsigned tail, bool pause,
         bool notified = false, bool initialWait = false) {
    struct Event {
        HANDLE handle{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        ~Event() { if (handle) CloseHandle(handle); }
    } ready;
    if (!ready.handle) return false;
    Source source;
    source.wait = wait;
    source.initialWait = initialWait;
    source.tail = tail;
    WasapiExclusiveSink sink;
    WasapiSinkConfig config;
    config.format = {rate, 2, static_cast<std::uint16_t>(bits), static_cast<std::uint16_t>(bits)};
    config.periodFrames = rate * 30 / 1000;
    config.sourceReadyEvent = notified ? ready.handle : nullptr;
    auto hr = sink.Open(config);
    std::printf("OPEN rate=%u bits=%u wait=%d tail=%u pause=%d notify=%d initialWait=%d hr=%08lx frames=%u\n",
        rate, bits, wait, tail, pause, notified, initialWait, hr, sink.BufferFrames());
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
    const bool passed = SUCCEEDED(hr) && complete && SUCCEEDED(stats.lastError) &&
        source.count == 21 && stats.underruns == 0 && (!wait || stats.sourceWaits > 0) &&
        (!pause || paused);
    std::printf("RESULT %s complete=%d hr=%08lx waits=%llu fills=%u\n",
        passed ? "PASS" : "FAIL", complete, stats.lastError, stats.sourceWaits, source.count);
    return passed;
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--run-on-default-device") != 0) {
        std::puts("Explicit opt-in required: --run-on-default-device (exclusive silent audio)");
        return 2;
    }
    int failures = 0;
    for (unsigned rate : {192000u, 96000u, 44100u}) {
        for (bool wait : {false, true}) {
            for (unsigned tail : {0u, 1u, 2u})
                if (!Run(rate, 24, wait, tail, false)) ++failures;
            if (!Run(rate, 24, wait, 1, true)) ++failures;
            if (!Run(rate, 16, wait, 1, false)) ++failures;
            if (!Run(rate, 24, wait, 1, false, true, true)) ++failures;
        }
    }
    std::printf("36 hardware cases: %d failed\n", failures);
    return failures ? 1 : 0;
}
