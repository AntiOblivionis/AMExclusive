# Changelog

All notable changes to this fork are documented here. The baseline is the
upstream [AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive)
`main` branch at the time of the fork (2026-11-04).

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.2.3] - 2026-11-05

### Fixed

- **File-shaped FLAC played as full-scale noise.** `FlacEncoder` was fed the
  canonical Q31 samples the WAV path packs (a 24-bit sample is `value >> 8`,
  i.e. left-justified in 32 bits) but wrote their low `bitsPerSample` bits, so
  every sample reached the renderer scrambled. The encoder now shifts each
  sample down to the stream depth itself, which makes the FLAC payload
  bit-identical to the WAV payload for the same PCM. Confirmed by decoding a
  captured entity: the decoded samples match the source exactly (a 12000-level
  440 Hz tone decodes as 12000, not 3072000), with no ffmpeg errors.
- **The final frame of a verbatim stream announced the wrong block size.** The
  zero-padding that keeps every frame the same size ran *after* the frame header
  had been written, so the last frame declared 256 frames while carrying 4096.
  Padding now happens before the header. A captured 40 s entity walks cleanly:
  469 frames, all `blockCode = 0xC`, all CRC-16 correct, ending exactly at the
  declared `Content-Length`.
- `Server:` response header still reported `0.2.1` in the 0.2.2 build.

## [0.2.2] - 2026-11-04

### Added

- **A second wire shape for the DLNA stream**, selectable from the AME panel
  and persisted as `[dlna] stream_format`. `wav` stays the default and is
  byte-for-byte unchanged. Only the two shapes that actually play on tested
  hardware are offered: WAV file and FLAC file.
  The chunked variants that were tried first (`flac`, `live`) were removed
  outright: a VEGA G2.2 reads one chunk, drops the connection, retries, and
  gives up, and offering them in the panel produced a silent "no sound" that
  looked like a fault in this app rather than a renderer limitation.
  - `flac-file` — file-shaped FLAC (the one offered in the panel).

  New `mod/src/audio_v2/FlacEncoder.{h,cpp}`: fixed block size 4096,
  independent channels, fixed predictors order 0–4, RICE2 residuals,
  CRC-8/CRC-16, 16/20/24-bit, plus a verbatim mode with constant frame size.
  Verified against ffmpeg: decoded output is bit-identical to the source at
  24/48k, 24/96k and 16/44.1k; compressed ratio 21–37% of raw PCM.
- DIDL `<res>` now also carries `sampleFrequency`, `bitsPerSample` and
  `nrAudioChannels`, which strict renderers cross-check against the container
  and the HTTP `Content-Type`.
- `DLNA.ORG_OP` now follows the shape: `01` only for the seekable file shape,
  `00` for FLAC and live, so a renderer is never promised byte ranges that the
  server will not honour.
- **`flac-file` — FLAC served as an ordinary file.** Verbatim subframes give
  every frame an identical size, so `FlacEncoder::VerbatimSize()` can compute
  the exact entity length before the first sample exists; the response then
  carries a real `Content-Length`, `Accept-Ranges: bytes` and `206` replies.
  No compression (bandwidth is within 1% of WAV), but it is the only way to
  satisfy a renderer that refuses an unbounded body while also refusing WAV.
  Verified on a VEGA G2.2: `state=Playing`, `filetype=FLAC`, `48000`, `24-bit`,
  position advancing. The same device rejects the chunked FLAC shape outright.
- DIDL `<res>` gained `sampleFrequency`, `bitsPerSample` and `nrAudioChannels`.
- HTTP responses now carry `Date` (required by RFC 7231 for 1.1) and a
  `Server` banner; `contentFeatures.dlna.org` gained `DLNA.ORG_PN=WAV`; the
  `transferMode.dlna.org` header uses the conventional lower-case spelling.
- The WAV MIME follows the renderer: when it advertises `audio/x-wav` or
  `audio/wave` but not `audio/wav`, that spelling is used instead.

### Fixed

- Chunked shapes now start every HTTP connection at the **oldest retained
  byte** instead of the newest. `NewReader` began at `produced_` because the
  file shape always reconnects with a byte range; a chunked stream has no
  ranges, so a renderer that probes and then opens a second connection read
  zero bytes and stopped. Measured: probe drained 60 KB, playback connection
  got an immediate end-of-stream.
- `FlushEncoderTail` emits the encoder's final partial block. Without it the
  last up-to-4096 frames of every FLAC track were dropped and the stream ended
  mid-frame.
- FLAC STREAMINFO declares `min_block = max_block = 4096`. Reporting a smaller
  minimum made renderers treat the stream as variable-block.
- FLAC advertises `audio/x-flac`, not the registered `audio/flac`. Measured on
  a VEGA G2.2: its ConnectionManager lists exactly `http-get:*:audio/x-flac:*`
  and drops a stream that announces the standard spelling.
- Chunked bodies go out in 8 KB pieces rather than 64 KB: a renderer that
  decodes while receiving stalls on a full 64 KB burst.

### Verified

Measured against a physical AURALiC VEGA G2.2 (`LightningPlayer`, OpenHome):

| shape | result |
|---|---|
| `wav` | plays, 24-bit/48 kHz, device pulls the whole entity |
| `flac` | **rejected** — device reads one chunk, drops the connection, retries, gives up |
| `live` | **rejected** — same behaviour |

The renderer itself does support FLAC: the same 10 s 24-bit FLAC served as a
**file** (Content-Length + Range) plays with `state=Playing`, `48000`, `24`.
So the limitation is that this device only accepts file-shaped bodies, not the
codec. FLAC and live remain available for renderers that accept chunked
transfer (Lumin and most software stacks); the VEGA G2.2 stays on `wav`.

## [0.2.1] - 2026-11-04

### Fixed

- **DLNA output no longer dies when the renderer re-binds its HTTP port.**
  AURALiC / `LightningPlayer`-class devices pick a new UPnP port every few
  minutes while staying up, so the `location` cached in `am-exclusive.ini`
  goes stale. Previously `DlnaSink::Open` retried that stale address four
  times and gave up, leaving the renderer silent while Apple Music kept
  playing locally.
  - New `mod/src/audio_v2/DlnaResolve.{h,cpp}`: resolves the current
    description URL from a live M-SEARCH by **UDN**, sending from every usable
    IPv4 interface (the same adapter-selection strategy the broker uses) so a
    multi-homed host does not lose the multicast.
  - `DlnaSink::Open` now re-resolves the location by UDN when the cached
    description fetch fails, then retries against the current URL. Measured
    31–47 ms against a live VEGA G2.2.
  - `Broker` now records the newest location SSDP reports and adopts it once
    the UDN matches the bound device: it rewrites the ini and pushes a fresh
    `SetOutputTarget` to the agent, so a running stack converges without a
    restart or re-picking the device in the UI.
  - `am-exclusive-hook` and the DLNA self-test now link `iphlpapi`.

## [0.2.0] - 2026-11-04

First release of the OpenHome/DLNA derivative.

### Added

- **DLNA / OpenHome output.** SSDP renderer discovery,
  `ConnectionManager.GetSinkProtocolInfo` format negotiation, and OpenHome
  `Playlist.DeleteAll + Insert + Play` control. Captured PCM is served over HTTP
  from inside the Apple Music process. Each track opens a new URL and renegotiates
  sample rate and bit depth.
- **File-shaped live HTTP stream.** `Content-Length`, `Accept-Ranges: bytes` and
  `206 + Content-Range` responses, because `LightningPlayer`-class renderers reject
  a body with no declared total length. Audio is still produced in real time.
- **True track duration.** `am_exclusive_media_control.exe watch` reads the Windows
  media session timeline; the declared WAV size and DIDL `duration` now match the
  song. New keys: `[dlna] follow_track_duration`, `[dlna] stream_seconds` (fallback).
- **Forced Hi-Res tier.** `[mod] force_hires=1` raises the requested tier to 20
  (up to 24-bit/192 kHz) without an external DAC. The strict filter still selects
  the highest rate actually present in the item; AAC is rejected whenever a lossless
  variant exists, and AAC-only items remain playable.
- **Second verified CoreMedia profile.** Apple Music `1.1792.39682.0` /
  CoreMedia `1.2 (3365.13)` alongside upstream's `3310.2`. Unknown images disable
  the quality lock instead of patching unreviewed code.
- **Real-time buffer hardening.** `VirtualLock` on both the capture queue and the
  stream ring, with a working-set quota lift; the DLNA producer thread now runs at
  `THREAD_PRIORITY_ABOVE_NORMAL`.
- **DLNA self-test** (`mod/tests/DlnaSelfTest.cpp`) and installer support for
  preserving DLNA configuration across upgrades.
- Community and compliance files: `NOTICE.md`, `CHANGELOG.md`, `CONTRIBUTING.md`,
  `SECURITY.md`, `CODE_OF_CONDUCT.md`, issue/PR templates.

### Changed

- `mod/src/ApplePrivateOffsets.h`: literal 16-byte instruction prologues replaced
  by SHA-256 digests; verification now hashes the running image's first 16 bytes
  through Windows CNG. No Apple code is embedded in the source tree.
- README (English and Chinese) rewritten to describe the fork, its verified
  environment (`96 kHz / 24-bit` on AURALiC VEGA G2.2), limitations and
  configuration.
- `.gitignore` extended to exclude local research data, personal device
  configuration backups, audio samples and build artifacts.

### Known limitations

- The renderer cannot seek: the stream is produced in real time. Seek inside Apple
  Music instead; it rebuilds the audio graph and the fork follows with a new URL.
- Declared length is track duration plus 3 seconds, so playback ends with the
  renderer waiting on a short tail. Cosmetic only.
- The first track after Apple Music starts may fall back to `stream_seconds` while
  the media-session helper initialises.
- `force_hires=1` with WASAPI exclusive on a 48 kHz-only DAC may fail to open a
  96/192 kHz endpoint. Use `force_hires=0` or `allow_resampling=1`.

## [0.1.0] - upstream baseline

WASAPI exclusive output, forced highest lossless quality, bit-perfect integer PCM
path, optional format conversion, and the single-build CoreMedia quality lock.
See the [upstream repository](https://github.com/AntiOblivionis/AMExclusive) for
its own history and changelog.
