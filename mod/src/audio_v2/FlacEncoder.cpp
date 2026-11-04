#include "FlacEncoder.h"

#include <algorithm>
#include <cstring>

namespace ammod::audio_v2 {
namespace {

std::uint8_t Crc8(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<std::uint8_t>((crc << 1) ^ 0x07)
                               : static_cast<std::uint8_t>(crc << 1);
        }
    }
    return crc;
}

std::uint16_t Crc16(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x8005)
                                 : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

// Writes bits most-significant-first, which is what FLAC requires.
class BitWriter final {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) noexcept : out_(out) {}

    // Bits are packed most-significant-first into whole bytes. `count` is
    // capped so the accumulator never shifts past 64 bits.
    void Write(std::uint64_t value, unsigned count) noexcept {
        if (count == 0) return;
        if (count > 56) {
            Write(value >> 56, count - 56);
            Write(value & ((1ull << 56) - 1ull), 56);
            return;
        }
        value &= (1ull << count) - 1ull;
        accumulator_ = (accumulator_ << count) | value;
        bits_ += count;
        while (bits_ >= 8) {
            const unsigned shift = bits_ - 8;
            out_.push_back(static_cast<std::uint8_t>((accumulator_ >> shift) & 0xFF));
            bits_ -= 8;
        }
        accumulator_ = bits_ ? (accumulator_ & ((1ull << bits_) - 1ull)) : 0;
    }

    void WriteSigned(std::int64_t value, unsigned count) noexcept {
        Write(static_cast<std::uint64_t>(value), count);
    }

    // Rice unary part: `quotient` zero bits followed by a terminating one.
    // It must go through Write: a raw byte push here would corrupt the stream
    // whenever the writer is not already byte-aligned, which is the normal case
    // in the middle of a subframe.
    void WriteUnary(std::uint64_t quotient) noexcept {
        while (quotient >= 32) {
            Write(0, 32);
            quotient -= 32;
        }
        if (quotient > 0) Write(0, static_cast<unsigned>(quotient));
        Write(1, 1);
    }

    void AlignToByte() noexcept {
        if (bits_ != 0) {
            accumulator_ <<= (8 - bits_);
            out_.push_back(static_cast<std::uint8_t>(accumulator_));
            accumulator_ = 0;
            bits_ = 0;
        }
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint64_t accumulator_{};
    unsigned bits_{};
};

// FLAC frame-header sample-size code. 0 means "take it from STREAMINFO".
unsigned SampleSizeCode(unsigned bits) noexcept {
    switch (bits) {
    case 8: return 1;
    case 12: return 2;
    case 16: return 4;
    case 20: return 5;
    case 24: return 6;
    case 32: return 7;
    default: return 6;
    }
}

// Returns the 4-bit code. When `extraBits` is non-zero the caller must append
// `extra` using that width right after the two header bytes.
unsigned SampleRateCode(std::uint32_t rate, std::uint32_t& extra,
                        unsigned& extraBits) noexcept {
    extra = 0;
    extraBits = 0;
    switch (rate) {
    case 88200: return 0x1;
    case 176400: return 0x2;
    case 192000: return 0x3;
    case 8000: return 0x4;
    case 16000: return 0x5;
    case 22050: return 0x6;
    case 24000: return 0x7;
    case 32000: return 0x8;
    case 44100: return 0x9;
    case 48000: return 0xA;
    case 96000: return 0xB;
    default: break;
    }
    if (rate % 1000u == 0 && rate / 1000u <= 255u) {
        extra = rate / 1000u;
        extraBits = 8;
        return 0xC;
    }
    if (rate <= 65535u) {
        extra = rate;
        extraBits = 16;
        return 0xD;
    }
    // Out of range for a 16-bit explicit rate; fall back to 48 kHz so the
    // stream stays decodable instead of silently declaring garbage.
    return 0xA;
}

std::uint64_t ZigZag(std::int64_t value) noexcept {
    std::uint64_t encoded = static_cast<std::uint64_t>(value) << 1;
    if (value < 0) encoded = ~encoded;
    return encoded;
}

// Rice parameter that minimises the coded residual size. Early exit keeps this
// cheap on typical music, where the optimum is found in the first few tries.
unsigned ChooseRice(const std::vector<std::int64_t>& residual) noexcept {
    unsigned bestK = 0;
    std::uint64_t bestBits = ~0ull;
    for (unsigned k = 0; k <= 30; ++k) {
        std::uint64_t bits = 0;
        for (const std::int64_t value : residual) {
            const std::uint64_t encoded = ZigZag(value);
            bits += (encoded >> k) + 1ull + k;
            if (bits >= bestBits) break;
        }
        if (bits < bestBits) {
            bestBits = bits;
            bestK = k;
        }
    }
    return bestK;
}

// Fixed-predictor residual for one channel. order 0..4 are the only predictors
// FLAC defines for the fixed type; higher orders need LPC, which this encoder
// deliberately does not implement.
void FixedResidual(const std::int32_t* samples, std::uint32_t frames,
                   unsigned order, std::int64_t* residual) noexcept {
    for (std::uint32_t i = order; i < frames; ++i) {
        std::int64_t prediction = 0;
        switch (order) {
        case 0: prediction = 0; break;
        case 1: prediction = samples[i - 1]; break;
        case 2: prediction = 2ll * samples[i - 1] - samples[i - 2]; break;
        case 3: prediction = 3ll * samples[i - 1] - 3ll * samples[i - 2] + samples[i - 3]; break;
        case 4:
            prediction = 4ll * samples[i - 1] - 6ll * samples[i - 2] +
                         4ll * samples[i - 3] - samples[i - 4];
            break;
        default: prediction = 0; break;
        }
        residual[i - order] = static_cast<std::int64_t>(samples[i]) - prediction;
    }
}

} // namespace

void FlacEncoder::Reset(std::uint32_t sampleRate, std::uint32_t channels,
                        std::uint32_t bitsPerSample, bool verbatim,
                        std::uint64_t totalSamples) noexcept {
    verbatim_ = verbatim;
    totalSamples_ = totalSamples;
    sampleRate_ = sampleRate;
    channels_ = std::min<std::uint32_t>(std::max<std::uint32_t>(channels, 1u), 8u);
    bits_ = std::min<std::uint32_t>(std::max<std::uint32_t>(bitsPerSample, 8u), 24u);
    started_ = false;
    frameNumber_ = 0;
    framesEncoded_ = 0;
    blockFrames_ = 0;
    block_.clear();
    block_.reserve(static_cast<std::size_t>(kBlockSize) * channels_);
    channel_.reserve(kBlockSize);
    residual_.reserve(kBlockSize);
    pending_.clear();
}

std::uint64_t FlacEncoder::VerbatimSize(std::uint32_t sampleRate, std::uint32_t channels,
                                        std::uint32_t bitsPerSample,
                                        std::uint64_t totalSamples) noexcept {
    (void)sampleRate;
    if (totalSamples == 0) return 0;
    const std::uint32_t channelCount =
        std::min<std::uint32_t>(std::max<std::uint32_t>(channels, 1u), 8u);
    const std::uint32_t bits =
        std::min<std::uint32_t>(std::max<std::uint32_t>(bitsPerSample, 8u), 24u);
    const std::uint64_t frameCount = (totalSamples + kBlockSize - 1) / kBlockSize;
    // Verbatim subframe: 8-bit header plus every sample at full width.
    const std::uint64_t bodyPerFrame =
        static_cast<std::uint64_t>(channelCount) * (1ull + kBlockSize * (bits / 8u));
    std::uint64_t total = 42; // "fLaC" + STREAMINFO block
    for (std::uint64_t frame = 0; frame < frameCount; ++frame) {
        // 4 fixed bytes + UTF-8 frame number + CRC-8, then the body and CRC-16.
        std::uint64_t headerBytes = 4;
        if (frame < 0x80ull) headerBytes += 1;
        else if (frame < 0x800ull) headerBytes += 2;
        else if (frame < 0x10000ull) headerBytes += 3;
        else headerBytes += 4;
        total += headerBytes + 1 + bodyPerFrame + 2;
    }
    return total;
}

void FlacEncoder::EmitStreamHeader() noexcept {
    static const std::uint8_t kMagic[4] = {'f', 'L', 'a', 'C'};
    pending_.insert(pending_.end(), kMagic, kMagic + 4);

    std::uint8_t info[34]{};
    // Both bounds are kBlockSize. A short final block is legal in a fixed-block
    // stream and declares its real size in the frame header, but reporting a
    // smaller minimum made renderers treat the stream as variable-block and
    // reject it: measured against a VEGA G2.2, min=16/max=4096 was read 64 KB at
    // a time and then dropped, while min=max=4096 plays.
    info[0] = static_cast<std::uint8_t>(kBlockSize >> 8);
    info[1] = static_cast<std::uint8_t>(kBlockSize & 0xFF);
    info[2] = info[0];
    info[3] = info[1];
    // min/max frame size stay 0 ("unknown").
    // 20-bit sample rate, 3-bit channels-1, 5-bit bits-1, 36-bit total samples.
    const std::uint32_t rate = sampleRate_ & 0xFFFFFu;
    const std::uint32_t channels = (channels_ - 1u) & 0x7u;
    const std::uint32_t bits = (bits_ - 1u) & 0x1Fu;
    info[10] = static_cast<std::uint8_t>((rate >> 12) & 0xFF);
    info[11] = static_cast<std::uint8_t>((rate >> 4) & 0xFF);
    info[12] = static_cast<std::uint8_t>(((rate & 0xF) << 4) | (channels << 1) |
                                         ((bits >> 4) & 0x1));
    // 36-bit total sample count, split across the low nibble of info[13] and
    // info[14..17]. 0 stays "unknown".
    const std::uint64_t total = totalSamples_ & ((1ull << 36) - 1ull);
    info[13] = static_cast<std::uint8_t>(((bits & 0xF) << 4) | ((total >> 32) & 0xF));
    info[14] = static_cast<std::uint8_t>((total >> 24) & 0xFF);
    info[15] = static_cast<std::uint8_t>((total >> 16) & 0xFF);
    info[16] = static_cast<std::uint8_t>((total >> 8) & 0xFF);
    info[17] = static_cast<std::uint8_t>(total & 0xFF);
    // info[18..33] MD5 = 0 ("unknown").

    // Metadata block header: last-block flag + type 0 (STREAMINFO) + 24-bit length.
    pending_.push_back(0x80);
    pending_.push_back(0);
    pending_.push_back(0);
    pending_.push_back(34);
    pending_.insert(pending_.end(), info, info + 34);
}

void FlacEncoder::WriteFrameHeader(std::vector<std::uint8_t>& frame) noexcept {
    frame.push_back(0xFF);
    frame.push_back(0xF8); // 14-bit sync tail, reserved 0, fixed block size

    std::uint32_t rateExtra = 0;
    unsigned rateExtraBits = 0;
    const unsigned rateCode = SampleRateCode(sampleRate_, rateExtra, rateExtraBits);

    // A short final block must declare its real length, so 4096 keeps the
    // compact code and anything else uses the explicit 16-bit form.
    const bool fullBlock = blockFrames_ == kBlockSize;
    const unsigned blockCode = fullBlock ? 0xC : 0x7;
    const unsigned channelCode = (channels_ - 1u) & 0x7u;
    const unsigned sizeCode = SampleSizeCode(bits_);
    frame.push_back(static_cast<std::uint8_t>((blockCode << 4) | rateCode));
    frame.push_back(static_cast<std::uint8_t>((channelCode << 4) | (sizeCode << 1)));

    // UTF-8 coded frame number (fixed-blocksize streams use the frame index).
    // Order matters: FLAC puts the coded number and the header CRC *before*
    // the optional block-size / sample-rate extension bytes. Emitting the
    // extensions first desynchronises every strict decoder at the first frame
    // whose size is not implied by its 4-bit code.
    const std::uint64_t number = frameNumber_;
    if (number < 0x80ull) {
        frame.push_back(static_cast<std::uint8_t>(number));
    } else if (number < 0x800ull) {
        frame.push_back(static_cast<std::uint8_t>(0xC0 | (number >> 6)));
        frame.push_back(static_cast<std::uint8_t>(0x80 | (number & 0x3F)));
    } else if (number < 0x10000ull) {
        frame.push_back(static_cast<std::uint8_t>(0xE0 | (number >> 12)));
        frame.push_back(static_cast<std::uint8_t>(0x80 | ((number >> 6) & 0x3F)));
        frame.push_back(static_cast<std::uint8_t>(0x80 | (number & 0x3F)));
    } else {
        frame.push_back(static_cast<std::uint8_t>(0xF0 | (number >> 18)));
        frame.push_back(static_cast<std::uint8_t>(0x80 | ((number >> 12) & 0x3F)));
        frame.push_back(static_cast<std::uint8_t>(0x80 | ((number >> 6) & 0x3F)));
        frame.push_back(static_cast<std::uint8_t>(0x80 | (number & 0x3F)));
    }

    // Extension bytes for a short block or a non-standard sample rate.
    if (!fullBlock) {
        const std::uint32_t declared = blockFrames_ - 1u;
        frame.push_back(static_cast<std::uint8_t>((declared >> 8) & 0xFF));
        frame.push_back(static_cast<std::uint8_t>(declared & 0xFF));
    }
    if (rateExtraBits == 8) {
        frame.push_back(static_cast<std::uint8_t>(rateExtra & 0xFF));
    } else if (rateExtraBits == 16) {
        frame.push_back(static_cast<std::uint8_t>((rateExtra >> 8) & 0xFF));
        frame.push_back(static_cast<std::uint8_t>(rateExtra & 0xFF));
    }

    // CRC-8 closes the header and covers everything above, extensions included.
    frame.push_back(Crc8(frame.data(), frame.size()));
}

void FlacEncoder::EncodeBlock() noexcept {
    if (blockFrames_ == 0) return;

    std::vector<std::uint8_t> frame;
    frame.reserve(static_cast<std::size_t>(blockFrames_) * channels_ * 4u + 64u);
    WriteFrameHeader(frame);

    BitWriter writer(frame);
    // A verbatim stream must keep a constant frame size for its declared length
    // to hold, so the final short block is zero-padded to a full block.
    if (verbatim_ && blockFrames_ < kBlockSize) {
        block_.resize(static_cast<std::size_t>(kBlockSize) * channels_, 0);
        blockFrames_ = kBlockSize;
    }
    const std::uint32_t frames = blockFrames_;
    for (std::uint32_t ch = 0; ch < channels_; ++ch) {
        channel_.resize(frames);
        for (std::uint32_t i = 0; i < frames; ++i) {
            channel_[i] = block_[static_cast<std::size_t>(i) * channels_ + ch];
        }

        if (verbatim_) {
            writer.Write(0, 1);              // mandatory zero bit
            writer.Write(0b000001u, 6);      // VERBATIM subframe
            writer.Write(0, 1);              // no wasted bits
            for (std::uint32_t i = 0; i < frames; ++i) {
                writer.WriteSigned(channel_[i], bits_);
            }
            continue;
        }
        const std::uint32_t maxOrder =
            std::min<std::uint32_t>(4u, frames > 1u ? frames - 1u : 0u);
        unsigned bestOrder = 0;
        std::uint64_t bestSum = ~0ull;
        residual_.resize(frames);
        std::vector<std::int64_t> bestResidual;
        for (unsigned order = 0; order <= maxOrder; ++order) {
            FixedResidual(channel_.data(), frames, order, residual_.data());
            std::uint64_t sum = 0;
            for (std::uint32_t i = order; i < frames; ++i) {
                const std::int64_t value = residual_[i - order];
                sum += static_cast<std::uint64_t>(value < 0 ? -value : value);
            }
            if (sum < bestSum) {
                bestSum = sum;
                bestOrder = order;
                bestResidual.assign(residual_.begin(), residual_.begin() + (frames - order));
            }
        }

        writer.Write(0, 1);                                // mandatory zero bit
        writer.Write(0b001000u | bestOrder, 6);            // FIXED subframe, order N
        writer.Write(0, 1);                                // no wasted bits
        for (unsigned i = 0; i < bestOrder; ++i) {
            writer.WriteSigned(channel_[i], bits_);
        }
        // Residual coding method 1 = RICE2, whose parameter field is 5 bits
        // (0..30). Method 0 only allows 4 bits (0..14), which cannot describe
        // the residual of a 24-bit signal: the chosen parameter would be
        // truncated and every following symbol would decode as garbage.
        writer.Write(1, 2);
        writer.Write(0, 4); // partition order 0 (a single partition)
        const unsigned rice = std::min<unsigned>(ChooseRice(bestResidual), 30u);
        writer.Write(rice, 5);
        for (const std::int64_t value : bestResidual) {
            const std::uint64_t encoded = ZigZag(value);
            writer.WriteUnary(encoded >> rice);
            if (rice != 0) {
                writer.Write(encoded & ((1ull << rice) - 1ull), rice);
            }
        }
    }
    writer.AlignToByte();

    const std::uint16_t crc = Crc16(frame.data(), frame.size());
    frame.push_back(static_cast<std::uint8_t>(crc >> 8));
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));

    pending_.insert(pending_.end(), frame.begin(), frame.end());
    framesEncoded_ += blockFrames_;
    ++frameNumber_;
    blockFrames_ = 0;
    // The accumulation buffer must be emptied here: Push appends to its end
    // while EncodeBlock reads from index 0, so leaving stale samples behind
    // would re-encode the previous block into every following frame.
    block_.clear();
}

void FlacEncoder::Push(const std::int32_t* interleaved, std::uint32_t frames) noexcept {
    if (!interleaved || frames == 0) return;
    if (!started_) {
        EmitStreamHeader();
        started_ = true;
    }
    std::uint32_t offset = 0;
    while (offset < frames) {
        const std::uint32_t take =
            std::min<std::uint32_t>(frames - offset, kBlockSize - blockFrames_);
        const std::size_t base = static_cast<std::size_t>(offset) * channels_;
        block_.insert(block_.end(), interleaved + base,
                      interleaved + base + static_cast<std::size_t>(take) * channels_);
        blockFrames_ += take;
        offset += take;
        if (blockFrames_ == kBlockSize) EncodeBlock();
    }
}

void FlacEncoder::Drain(std::vector<std::uint8_t>& out) noexcept {
    if (pending_.empty()) return;
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

void FlacEncoder::Finish(std::vector<std::uint8_t>& out) noexcept {
    if (!started_) {
        EmitStreamHeader();
        started_ = true;
    }
    if (blockFrames_ > 0) EncodeBlock();
    Drain(out);
}

} // namespace ammod::audio_v2
