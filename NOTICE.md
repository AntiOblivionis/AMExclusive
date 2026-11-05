# Notice

## Upstream

This repository is a fork of
**[AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive)**.

- Upstream license: **MIT License**
- Upstream copyright: `Copyright (c) 2026 AMExclusive contributors`

The upstream `LICENSE` text is reproduced verbatim in this repository's
[`LICENSE`](LICENSE), with a separate copyright line added for the derivative work
below. [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) is likewise retained
unchanged. The MIT copyright notice and permission notice also accompany every
binary installer produced from this tree.

The `assets/` directory (including `assets/ame-logo.svg`) comes from the upstream
repository and remains credited to its authors. This fork does not claim ownership
of the upstream name or logo and does not present itself as the upstream project.

## Derivative work

`AMExclusive-Openhome` is maintained independently by
[dxdxdxlm123](https://github.com/dxdxdxlm123). Changes relative to upstream:

### New subsystems

| Area | Files (under `mod/src/`) |
|---|---|
| OpenHome / DLNA control point | `DlnaDiscovery.{h,cpp}`, `UpnpXml.{h,cpp}`, `audio_v2/DlnaControlClient.{h,cpp}` |
| DLNA audio sink and HTTP stream server | `audio_v2/DlnaSink.{h,cpp}`, `audio_v2/DlnaStreamServer.{h,cpp}`, `audio_v2/DlnaLog.{h,cpp}` |
| Sink abstraction shared by WASAPI and DLNA | `audio_v2/AudioSink.h` |
| Locked memory for real-time buffers | `audio_v2/MemoryLock.h` |
| DLNA self-test renderer client | `mod/tests/DlnaSelfTest.cpp` |

### Modified subsystems

| Area | Files |
|---|---|
| Forced Hi-Res tier + multi-build quality lock | `HookDll.cpp`, `LosslessQualityLock.cpp`, `ApplePrivateOffsets.h` |
| Media session track-duration helper (`watch` mode) | `MediaSessionControl.cpp` |
| DLNA output wiring, IPC and UI | `AudioCoreCoordinator.{h,cpp}`, `AudioCoreRuntime.{h,cpp}`, `Broker.cpp`, `UiAdapter.cpp`, `IpcProtocol.h`, `AudioErrors.h`, `HookDll.cpp` |
| Queue locking, PCM packing, endpoint policy | `audio_v2/PcmQueue.h`, `audio_v2/PcmTypes.h`, `audio_v2/WasapiExclusiveSink.{h,cpp}` |
| Installer preserves DLNA configuration on upgrade | `mod/scripts/Install-Mod.ps1` |
| New configuration keys and build targets | `mod/config/am-exclusive.ini`, `mod/CMakeLists.txt` |

A change-by-change summary is in [`CHANGELOG.md`](CHANGELOG.md).

### Development notes

Part of this fork was developed with AI assistance. Every change was reviewed,
compiled, and validated against a physical AURALiC VEGA G2.2 renderer before
being committed. The upstream commit history likewise contains AI co-author
credits.

## Third-party components

| Component | License | Use |
|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) v1.3.4 | BSD-style (see notices) | Windows API hooking |
| [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue) | MIT | Bounded SPSC queue on the real-time audio path |
| Windows App SDK / WinUI metadata | Microsoft licence terms | Downloaded from NuGet at build time only; never redistributed |

Full licence texts: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Apple software

No Apple software, Apple binaries, Apple source code, decrypted media, or
DRM-free reference files are included in this repository. The constants in
`mod/src/ApplePrivateOffsets.h` are relative virtual addresses, structure
offsets and SHA-256 digests of short instruction sequences, used solely to
verify that a locally loaded image matches a build that was reviewed. They are
interoperability and version-safety fingerprints, not copies of Apple code.
