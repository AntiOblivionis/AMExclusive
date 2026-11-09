# Contributing

Thanks for your interest in improving Orion. This is a small,
hardware-sensitive project: **good bug reports are worth more than speculative
patches.**

## Before you open an issue

1. Check the [Known limitations](README.md#known-limitations) section first.
   Renderer-side seeking, a 3-second tail on the progress bar, and the first-track
   SMTC fallback are known behaviour.
2. Reproduce on a supported build. The quality lock is tied to specific Apple
   Music / CoreMedia versions; on other builds it deliberately disables itself.
3. **Attach logs.** Without them most reports cannot be diagnosed. Logs live in
   `%PUBLIC%\AMExclusive\`:

   | File | What to look for |
   |---|---|
   | `am-exclusive.log` | `matched build profile`, `quality lock`, telemetry (`droppedFrames`, `underruns`) |
   | `am-exclusive-dlna.log` | `format decided`, `http 200/206`, `Range`, `device started pulling` |
   | `am-exclusive-broker.log` | SSDP discovery, `location=` (renderer port changes on reboot) |

   Also state: Windows version, Apple Music version, output mode (`wasapi`/`dlna`),
   renderer model and firmware, and whether the same symptom appears with
   `force_hires=0`.

4. **Scrub personal data.** Logs and configuration may contain your renderer UDN,
   LAN IP addresses and track names. Redact anything you do not want public.

## Development setup

Requirements: Visual Studio 2022+ MSVC Build Tools, Windows 11 SDK, CMake 3.24+,
PowerShell 5.1. Build instructions are in the [README](README.md#build-and-install).

```powershell
cd mod
.\scripts\Get-WinUIMetadata.ps1 -OutputDirectory .\build\winui-metadata
.\scripts\Build-Mod.ps1 -Configuration Release -WinUIMetadataDirectory .\build\winui-metadata
```

Useful checks before a PR:

- `cmake --build <dir> --config Release` succeeds with no errors.
- The setup payload passes its own `--extract-test` check.
- If you touched the quality lock, confirm the fail-closed path still logs clearly
  on an unrecognised CoreMedia image.
- If you touched the DLNA HTTP layer, verify `200 / 200 / Range: bytes=44- → 206`
  against a renderer (or a local test renderer).

## Pull requests

- Keep one logical change per PR and describe the hardware you tested on.
- Do not bundle unrelated formatting changes with functional changes.
- Update `CHANGELOG.md` and, when behaviour changes, both READMEs.
- Say explicitly whether a change affects bit-perfect output. Format conversions
  must remain labelled and must never silently fall back to shared mode.
- Upstream-only fixes (WASAPI path, installer, CI) are usually better sent to
  [AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive)
  first; this fork will pick them up.

## What must never be committed

- Apple software, Apple binaries, WinMD metadata, or generated WinRT projection
  headers from an Apple Music installation.
- Any decrypted media, DRM-free reference file, or audio sample.
- Real device configurations containing a UDN or LAN address, personal logs,
  ETL/PCAP captures, and build output or binaries.
- Code whose purpose is to bypass DRM or extract decrypted streams. This project
  deliberately stops at decoded PCM produced by the player itself.

## Security issues

Do not open a public issue. See [SECURITY.md](SECURITY.md).

## Conduct

All participation is governed by the [Code of Conduct](CODE_OF_CONDUCT.md).

## Licence

By contributing you agree that your contribution is licensed under the
[MIT License](LICENSE) and that you have the right to submit it. The upstream
copyright notice stays intact.
