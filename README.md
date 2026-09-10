<p align="center">
  <img src="assets/ame-logo.svg" alt="AME." width="480">
</p>

# AMExclusive

### Make Apple Music on Windows a “Pro” Hi-Fi Player

[中文](README.zh-CN.md)

## Description

AMExclusive (AME) is an open-source project designed specifically for Apple Music on Windows. It adds WASAPI exclusive audio output and forces Apple Music to use the highest available audio quality, with the goal of turning Apple Music into a more capable Hi-Fi player that can achieve bit-perfect output on supported hardware and configurations.

## Development and Test Environment

- Windows 11 x64
- Apple Music for Windows file version `1.6.4.90`
- Verified package/framework build `1.1540.23042.0` x64

This project has only been validated with the development and test versions listed above. Its behavior on other Apple Music versions is not guaranteed.

## Features

1. **WASAPI exclusive output**  
   Sends audio directly to the selected Windows output device in exclusive mode, avoiding the normal shared-mode Windows audio mixing path.

2. **Forced highest audio quality**  
   Removes Apple Music's native low-bandwidth quality downgrade behavior and keeps waiting for the highest available lossless stream instead of automatically falling back to a lower audio specification.

3. **Bit-perfect output**  
   Preserves the source sample format through the supported playback path and submits integer PCM directly to WASAPI, allowing bit-perfect playback when the source, device, and supported output format match.

## Recommended Settings

In Apple Music, open **Settings > Playback** and use the following settings:

- Turn **Crossfade** off.
- Turn **Sound Enhancer** off.
- Turn **Sound Check** off.
- Turn **Lossless Audio** on.
- Set both audio-quality selectors below Lossless Audio to **Hi-Res Lossless (ALAC)**.
- Turn **Dolby Atmos** off.
- Turn **Exclusive Mode** on.
- Turn **Spatial sound** off.

With a separate DAC and amplifier, keep the Apple Music app volume, Windows output volume, and DAC output volume at maximum, and adjust listening volume only on the amplifier. With an integrated DAC/amp, keep the Apple Music app volume and Windows output volume at maximum, and adjust listening volume using the DAC/amp hardware control. Before applying these settings for the first time, lower the amplifier or DAC/amp hardware volume to avoid an unexpectedly high sound level.

## Build and Install

To build from source, install Visual Studio 2022 or newer MSVC Build Tools, a Windows 11 SDK, CMake 3.24 or newer, PowerShell, and Apple Music for Windows.

```powershell
cd mod
.\scripts\Build-Mod.ps1 -Configuration Release
```

The installer will be generated at:

```text
mod\build\dist\AMExclusive-Setup.exe
```

To install or upgrade, run `AMExclusive-Setup.exe` and choose `1`. Then open Apple Music, go to **Settings > Playback**, and enable **Exclusive mode**.

To uninstall, run the same setup and choose `2`, or uninstall **AMExclusive** from **Windows Settings > Apps > Installed apps**.

## Acknowledgements

Thanks to the [LINUX DO community](https://linux.do/) for the discussion, support, and feedback provided during the development and testing of this project.

This project uses the following open-source projects:

- [MinHook](https://github.com/TsudaKageyu/minhook) — provides the Windows x64/x86 API hooking foundation.
- [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue) — provides the bounded single-producer, single-consumer queue used on the real-time audio path.

Thanks to these projects and their contributors. Complete version, copyright, and license information is available in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License and Disclaimer

Project-authored source code is released under the [MIT License](LICENSE). Third-party components retain their respective upstream licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

AMExclusive is an independent and unofficial open-source project. It is not affiliated with, endorsed by, or licensed by Apple Inc. Apple Music is a trademark of Apple Inc.

This project is not guaranteed to operate correctly across different software, hardware, driver, or audio-device configurations. Its use may cause unforeseen consequences, and users assume the associated risks.

This project does not distribute Apple software, Apple binaries, decrypted media, or DRM-free reference files, and it does not provide functionality intended to bypass DRM protection.
