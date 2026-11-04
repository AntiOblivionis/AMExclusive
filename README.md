# AMExclusive-Openhome

### Apple Music on Windows → WASAPI Exclusive / OpenHome network renderers

[中文](README.zh-CN.md) · [Upstream project](https://github.com/AntiOblivionis/AMExclusive) · [Changelog](CHANGELOG.md) · [Notice](NOTICE.md) · [Contributors](CONTRIBUTORS.md)

**Maintained by [@dxdxdxlm123](https://github.com/dxdxdxlm123).** See
[CONTRIBUTORS.md](CONTRIBUTORS.md) for upstream and fork authorship.

> **Unofficial fork.** This repository is a derivative of
> [AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive) (MIT License).
> It is maintained independently and is not affiliated with, endorsed by, or licensed by
> Apple Inc. or by the upstream author. `amexclusive.tgwgroup.ltd` is the upstream
> project's website; it is not the website of this fork.

## Download — no build required

Prebuilt Windows x64 installers are attached to every
[GitHub Release](https://github.com/dxdxdxlm123/AMExclusive-Openhome/releases).
You do **not** need Visual Studio, CMake, or any source code to install.

| Download | What it is |
|---|---|
| **[Latest release](https://github.com/dxdxdxlm123/AMExclusive-Openhome/releases/latest)** | Recommended — always points at the newest build |
| **[AMExclusive-Setup-0.2.0-20261104.exe](https://github.com/dxdxdxlm123/AMExclusive-Openhome/releases/download/v0.2.0/AMExclusive-Setup-0.2.0-20261104.exe)** | 2.7 MB installer (v0.2.0 direct link) |
| **[AMExclusive-0.2.0-20261104.zip](https://github.com/dxdxdxlm123/AMExclusive-Openhome/releases/download/v0.2.0/AMExclusive-0.2.0-20261104.zip)** | Installer plus a Chinese `使用说明.txt` |

SHA-256 of the `.exe`:
`3676D1A28C58FF3A3F6ADD45D1786C5593BC63DCB29E5BEC1F352408547E5EE9`

**Install:** fully quit Apple Music → right-click the installer →
**Run as administrator** → reopen Apple Music → choose the output in the in-app
panel (WASAPI exclusive for a local DAC, or DLNA after scanning for a renderer).
A Microsoft Store installation of Apple Music on Windows 10/11 x64 is required.

The installer is **not code-signed**, so Windows SmartScreen will warn about an
unknown publisher; verify the checksum above, then choose "More info → Run anyway".

To build from source instead, see [Build and install](#build-and-install).

## What this fork adds

The upstream project provides WASAPI exclusive output and a highest-quality lossless
stream lock. This fork keeps all of that and adds a **DLNA / OpenHome output path**, so
the same captured PCM can be streamed to a network renderer instead of a local DAC.

| | Upstream | This fork |
|---|---|---|
| WASAPI exclusive output | ✅ | ✅ (unchanged) |
| Highest-quality lossless lock | ✅ single CoreMedia build | ✅ two verified builds, fails closed |
| Hi-Res tier without an external DAC | — | ✅ `force_hires=1` |
| DLNA / OpenHome renderer output | — | ✅ |
| Per-track URL + true track duration | — | ✅ via Windows media session |
| Locked audio buffers + raised producer priority | — | ✅ |

## Development and test environment

- Windows 11 x64, Visual Studio 2022 MSVC Build Tools, CMake 3.24+, PowerShell 5.1
- **Verified in this fork:** Apple Music `1.1792.39682.0` / CoreMedia `1.2 (3365.13)` x64
- **Inherited from upstream:** Apple Music `1.6.4.90` / package framework `1.1540.23042.0` x64
- Verified network renderer: AURALiC VEGA G2.2 (`LightningPlayer`, OpenHome),
  playing WAV `96 kHz / 24-bit / 2 ch` with `converted=0` (no resampling, no depth change)

The quality lock depends on private image fingerprints that are tied to specific
Apple Music builds. On any other build it **fails closed**: playback and DLNA output
continue normally, but Apple Music decides the stream quality on its own. Look for
`Highest ALAC quality filter matched build profile` in `am-exclusive.log` to confirm
the lock is active. Other versions and devices are not guaranteed to work.

## Features

### From upstream

1. **WASAPI exclusive output** — submits audio directly to the selected endpoint,
   bypassing the shared-mode Windows mixing path.
2. **Forced highest audio quality** — rejects Apple Music's low-bandwidth downgrade
   and waits for the highest available lossless variant instead of falling back to AAC.
3. **Bit-perfect output** — integer PCM reaches WASAPI unchanged when source,
   device and endpoint format match.
4. **Optional format conversion for compatibility** — up/downsampling, bit-depth
   conversion and float32 output, with TPDF dither and saturating rounding. Shared
   mode is never used as a fallback; converted output is labelled explicitly.

### Added in this fork

5. **DLNA / OpenHome output** — SSDP discovery, `ConnectionManager.GetSinkProtocolInfo`
   format negotiation, and OpenHome `Playlist.DeleteAll + Insert + Play` control.
   The renderer pulls a WAV/LPCM stream over HTTP from a server hosted inside the
   Apple Music process. Every track opens a new URL and renegotiates the sample rate
   and bit depth.
6. **File-shaped live stream** — renderers of the `LightningPlayer` class refuse a body
   with no total length. The stream therefore announces `Content-Length`, honours
   `Accept-Ranges: bytes`, and answers `Range: bytes=N-` with `206 + Content-Range`.
   Audio is still produced in real time; the declared size is an upper bound.
7. **True track duration** — a small helper reads Windows media session (SMTC) timeline
   data so the declared length and the renderer's progress bar match the song.
   Falls back to `[dlna] stream_seconds` when no timeline is available.
8. **Forced Hi-Res tier** — `force_hires=1` raises the requested tier to 20 (up to
   24-bit/192 kHz) even when Apple Music does not detect an external DAC. The strict
   filter still picks the *highest rate actually present in the item*: a 96 kHz track
   plays at 96 kHz, a 44.1 kHz ALAC track falls to 44.1 kHz, and AAC candidates are
   rejected whenever the item has any lossless variant. Items that only have AAC
   remain playable.
9. **Multi-build quality lock** — two verified CoreMedia profiles with SHA-256
   fingerprints; an unrecognised image disables the lock rather than detouring
   unknown code.
10. **Real-time path hardening** — `VirtualLock` on both audio buffers (with a
    working-set quota lift) and `THREAD_PRIORITY_ABOVE_NORMAL` for the DLNA producer.

## Known limitations

- **The stream is live, not seekable.** Dragging the progress bar on the renderer
  cannot work, because the requested audio has not been produced yet. Seek inside
  Apple Music instead: it rebuilds the audio graph and this fork follows with a new
  URL (about 1–2 s of reconnection).
- **Declared length = track duration + 3 s.** A track ends with the renderer waiting
  on the last few seconds of the declared entity; this is cosmetic.
- The first track after Apple Music starts may fall back to `stream_seconds` if the
  SMTC helper has not published its first timeline reading yet.
- `force_hires=1` with WASAPI exclusive on a DAC limited to 48 kHz may fail to open
  a 96/192 kHz endpoint (no sound). Set `force_hires=0`, or `allow_resampling=1`.
  DLNA output is unaffected: formats are negotiated against the renderer.

## Configuration

The installer writes `%PUBLIC%\AMExclusive\am-exclusive.ini`. Upgrades preserve
values you have already changed.

```ini
[mod]
mode=exclusive
period_100ns=200000
allow_resampling=0
output=wasapi        ; wasapi | dlna
force_hires=1        ; 0 restores Apple's native tier selection

[dlna]
device_udn=          ; filled in by the in-app device picker
device_name=
location=
buffer_ms=1500       ; prebuffer target; larger = more jitter tolerance
format_pref=lpcm
max_bits=24
manual_urls=
pcm_byte_order=big   ; bare audio/L16 and audio/L24 are big-endian per RFC 3551/3190
stream_seconds=600   ; fallback declared length when SMTC has no timeline
follow_track_duration=1
```

## Recommended Apple Music settings

In **Settings > Playback**: turn **Crossfade**, **Sound Enhancer**, **Sound Check**,
**Dolby Atmos** and **Spatial sound** off; turn **Lossless Audio** and **Exclusive Mode**
on; set both lossless quality selectors to **Hi-Res Lossless (ALAC)**. Set **Hardware
buffer** to an integer `1`–`100` ms (default `20` ms; increase if playback stutters) and
press **Apply**. Enable **Allow compatible format conversion for unsupported formats**
only if your endpoint cannot play the original format.

With a separate DAC and amplifier, keep the app, Windows and DAC volumes at maximum
and adjust level only on the amplifier. Lower the hardware volume before applying
these settings for the first time.

## Build and install

### GitHub Actions

Run **Actions → Build Setup → Run workflow** in your own fork (enable Actions first),
then download `AMExclusive-Setup-x64` from Artifacts. The workflow builds Windows x64
Release, verifies the embedded installer payload, and does not publish a Release.
No Apple Music installation is needed on the runner: pinned Windows App SDK / WinUI
metadata comes from NuGet and `Microsoft.UI.Xaml.winmd` is SHA-256 verified. Artifacts
are retained for 14 days.

### Local build

Requires Visual Studio 2022+ MSVC Build Tools, Windows 11 SDK, CMake 3.24+, PowerShell.

```powershell
cd mod
.\scripts\Build-Mod.ps1 -Configuration Release
```

Without Apple Music on the build machine:

```powershell
cd mod
.\scripts\Get-WinUIMetadata.ps1 -OutputDirectory .\build\winui-metadata
.\scripts\Build-Mod.ps1 -Configuration Release -WinUIMetadataDirectory .\build\winui-metadata
```

Installer output: `mod\build\dist\AMExclusive-Setup.exe`.

**Run the installer as administrator.** The companion scheduled task is registered in
the task root folder and a non-elevated process receives Access denied. After installing,
restart Apple Music completely, then choose the output in the in-app panel: **WASAPI
exclusive** for a local DAC, or **DLNA** after scanning for a renderer.

Uninstall with the same setup (option `2`), from **Windows Settings > Apps > Installed
apps**, or via `%PUBLIC%\AMExclusive\Uninstall.cmd`.

### Logs

All logs live in `%PUBLIC%\AMExclusive\`:

| File | Contents |
|---|---|
| `am-exclusive.log` | hook, quality lock, audio pipeline telemetry |
| `am-exclusive-dlna.log` | stream setup, HTTP requests, format negotiation |
| `am-exclusive-broker.log` | SSDP discovery and configuration |

Please attach these when opening an issue — see [CONTRIBUTING.md](CONTRIBUTING.md).

## Development notes

Part of this fork was developed with AI assistance. All changes were reviewed,
compiled, and validated against a physical AURALiC VEGA G2.2 renderer before being
committed; the upstream project's own history contains similar AI co-author credits.

## Acknowledgements

- [AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive) — the
  original project by its contributors, MIT License.
- [LINUX DO community](https://linux.do/) — discussion and feedback for upstream.
- [MinHook](https://github.com/TsudaKageyu/minhook) — Windows API hooking foundation.
- [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue) — bounded SPSC queue on
  the real-time audio path.

Full version, copyright and license information: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
and [NOTICE.md](NOTICE.md).

## License and disclaimer

Project-authored source code is released under the [MIT License](LICENSE). Third-party
components retain their upstream licenses. The original copyright notice and
permission notice are preserved in `LICENSE`; see [NOTICE.md](NOTICE.md) for the list
of modifications made in this fork.

AMExclusive-Openhome is an independent and unofficial open-source project. It is not
affiliated with, endorsed by, or licensed by Apple Inc. Apple Music is a trademark of
Apple Inc. Hooking a third-party closed-source application may be restricted by that
application's licence terms: this project is offered for personal interoperability
use only, and users are responsible for checking applicable law and Apple's terms.

This project is not guaranteed to operate correctly across different software,
hardware, driver, or audio-device configurations. Its use may cause unforeseen
consequences, and users assume the associated risks.

This project does not distribute Apple software, Apple binaries, decrypted media, or
DRM-free reference files, and it does not provide functionality intended to bypass DRM
protection. The version fingerprints in `mod/src/ApplePrivateOffsets.h` are SHA-256
digests of short instruction sequences used only to verify that a loaded image matches
a reviewed build; no Apple code or binary is included in this repository.
