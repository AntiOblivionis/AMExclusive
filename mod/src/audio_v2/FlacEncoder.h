#pragma once

#include <cstdint>
#include <vector>

namespace ammod::audio_v2 {

// Minimal streaming FLAC encoder.
//
// A renderer that refuses an unbounded WAV body still accepts a FLAC stream:
// FLAC carries its own framing, so STREAMINFO may declare "total samples
// unknown" (0) and the encoder can emit frame by frame as PCM arrives. That
// removes the need to know the track length up front, which is exactly what a
// real-time capture cannot provide.
//
// Subset implemented (all of it legal FLAC, decoded by any conforming decoder):
//   - fixed block size, independent channels
//   - fixed predictors of order 0..4, chosen per channel per frame
//   - Rice coding with a single partition (partition order 0)
//   - 16/20/24-bit samples
//   - STREAMINFO with total_samples = 0 and MD5 = 0 ("unknown")
// No LPC, no mid/side, no wasted-bit packing: those only buy compression ratio,
// and every one of them is an extra chance to emit a stream a strict renderer
// rejects.
class FlacEncoder final {
public:
    // `verbatim` disables prediction and Rice coding: every subframe stores raw
    // samples, so every frame has an identical, computable size. That is what
    // makes a live stream expressible as a *file* with a declared
    // Content-Length - the only shape some renderers accept.
    // `totalSamples` is written into STREAMINFO. 0 means "unknown", which is
    // what a real-time stream must declare; the file shape knows the value and
    // should publish it so the renderer can show a duration.
    void Reset(std::uint32_t sampleRate, std::uint32_t channels,
               std::uint32_t bitsPerSample, bool verbatim = false,
               std::uint64_t totalSamples = 0) noexcept;

    // Exact byte size of a verbatim stream for `totalSamples` frames, header
    // included. Frame sizes differ only by the UTF-8 frame-number width, which
    // this accounts for, so the value is exact rather than an estimate.
    static std::uint64_t VerbatimSize(std::uint32_t sampleRate, std::uint32_t channels,
                                      std::uint32_t bitsPerSample,
                                      std::uint64_t totalSamples) noexcept;

    // Appends interleaved canonical integer samples: the same left-justified
    // 32-bit (Q31) values the WAV path packs, where a 24-bit sample is written
    // as `value >> 8`. The encoder shifts each sample down to the stream's bit
    // depth itself, because FLAC stores right-justified samples - taking the
    // low bits of a left-justified value yields full-scale noise.
    // Frames are encoded as soon as a whole block is available.
    void Push(const std::int32_t* interleaved, std::uint32_t frames) noexcept;

    // Moves every byte encoded so far into `out` (appends).
    void Drain(std::vector<std::uint8_t>& out) noexcept;

    // Encodes the final partial block and appends it. After this the encoder is
    // finished until Reset.
    void Finish(std::vector<std::uint8_t>& out) noexcept;

    bool Started() const noexcept { return started_; }
    std::uint64_t FramesEncoded() const noexcept { return framesEncoded_; }

private:
    static constexpr std::uint32_t kBlockSize = 4096;

    void EmitStreamHeader() noexcept;
    void EncodeBlock() noexcept;
    void WriteFrameHeader(std::vector<std::uint8_t>& frame) noexcept;

    std::uint32_t sampleRate_{48000};
    std::uint32_t channels_{2};
    std::uint32_t bits_{24};
    // Right shift from the canonical Q31 domain to `bits_`. Zero when the
    // caller already supplies right-justified samples.
    std::uint32_t shift_{32u - 24u};
    bool verbatim_{false};
    bool started_{false};
    std::uint64_t frameNumber_{};
    std::uint64_t framesEncoded_{};
    std::uint64_t totalSamples_{};

    // Interleaved accumulation buffer for one block.
    std::vector<std::int32_t> block_;
    std::uint32_t blockFrames_{};
    // Reused per-frame scratch so encoding allocates nothing on the audio path.
    std::vector<std::int32_t> channel_;
    std::vector<std::int64_t> residual_;

    std::vector<std::uint8_t> pending_;
};

} // namespace ammod::audio_v2
