# Changelog

All notable changes to this fork are documented here. The baseline is the
upstream [AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive)
`main` branch at the time of the fork (2026-11-04).

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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
