<p align="center">
  <img src="assets/ame-logo.svg" alt="AME." width="480">
</p>

# AMExclusive

### Make Apple Music on Windows a “Pro” Hi-Fi Player

[English](README.md)

## 描述

AMExclusive（AME）是一个专为 Windows 版 Apple Music 设计的开源项目，旨在为 Apple Music 提供 WASAPI 独占音频输出以及强制最高音频规格输出的能力，并以此将 Apple Music 优化为一款在受支持硬件与配置下可实现 bit-perfect 输出的“专业”Hi-Fi 音乐播放器。

## 开发与测试环境

- Windows 11 x64
- Windows 版 Apple Music 文件版本 `1.6.4.90`
- 已验证包/框架版本 `1.1540.23042.0` x64

本项目仅在上述开发与测试版本中完成验证，不保证在其他 Apple Music 版本上的运行效果。

## 特性

1. **WASAPI 独占输出**  
   以独占模式直接向当前 Windows 输出设备提交音频，绕过常规共享模式下的 Windows 音频混音路径。

2. **强制最高音频规格**  
   去除 Apple Music 原生“低带宽自动降低音频规格”的逻辑，在最高可用无损规格尚未就绪时继续加载等待，而不是自动回退到更低音频规格。

3. **Bit-perfect 输出**  
   在受支持的播放路径中保持源音频规格，并将整数 PCM 直接提交给 WASAPI；当音源、输出设备与目标格式匹配时，可实现 bit-perfect 播放。

## 推荐设置

在 Apple Music 的 **设置 > 播放** 中：

- 关闭 **交叉渐入渐出**。
- 关闭 **声音增强器**。
- 关闭 **音量平衡**。
- 开启 **无损音频**。
- 将无损音频下方的两项音频规格均设为 **高解析度无损（ALAC）**。
- 关闭 **杜比全景声**。
- 开启 **独占模式**。

在 Windows 声音设置中，关闭所用输出设备的 **空间音效**。

使用独立 DAC 和放大器时，将 Apple Music 应用内音量、Windows 输出音量和 DAC 输出音量保持在最大值，只通过放大器调整实际播放音量。使用 DAC/AMP 一体机时，将 Apple Music 应用内音量和 Windows 输出音量保持在最大值，通过 DAC/AMP 一体机的硬件音量控制调整实际播放音量。首次应用这些设置前，请先降低放大器或 DAC/AMP 一体机的硬件音量，避免突然出现过高声压。

## 编译与安装

从源码编译需要 Visual Studio 2022 或更新版本的 MSVC Build Tools、Windows 11 SDK、CMake 3.24 或更新版本、PowerShell，以及本机已安装 Windows 版 Apple Music。

```powershell
cd mod
.\scripts\Build-Mod.ps1 -Configuration Release
```

安装器输出位置：

```text
mod\build\dist\AMExclusive-Setup.exe
```

安装或升级时，运行 `AMExclusive-Setup.exe` 并选择 `1`。随后打开 Apple Music，进入 **设置 > 播放**，开启 **独占模式**。

卸载时，运行同一个 Setup 并选择 `2`，或在 **Windows 设置 > 应用 > 已安装的应用** 中卸载 **AMExclusive**。

## 协议与声明

项目自行编写的源码使用 [MIT License](LICENSE)。第三方组件继续遵循其各自的上游许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

AMExclusive 是一个独立且非官方的开源项目，与 Apple Inc. 没有任何关联，也未获得其许可或背书。Apple Music 是 Apple Inc. 的商标。

本项目不保证在不同软件、硬件、驱动程序或音频设备条件下能够正常运行。使用本项目可能造成不可预知的后果，使用者应自行承担相关风险。

本项目不分发 Apple 软件、Apple 二进制文件、解密后的媒体或 DRM-free 参考文件，也不提供用于绕过 DRM 保护的功能。
