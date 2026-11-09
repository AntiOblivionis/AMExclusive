<p align="center">
  <img src="assets/orion-logo.svg" alt="Orion" width="480">
</p>

<h1 align="center">Orion · AppleMusic-OpenHome</h1>

<p align="center">
  <b>让 Windows 上的 Apple Music 成为一台「专业级」Hi-Fi 播放器</b><br>
  Make Apple Music on Windows a “Pro” Hi-Fi player
</p>

<p align="center">
  <a href="#中文">中文</a> ·
  <a href="#english">English</a> ·
  <a href="#打赏支持">打赏支持 / Donations</a>
</p>

---

## 中文

### 这是什么

Orion（AppleMusic-OpenHome）是一个面向 Windows 版 Apple Music 的开源项目。它直接从
Apple 自己的解码器里取出 PCM，自己提交给设备，绕过 Windows 混音器，提供四条
bit-perfect 输出通路：

- **WASAPI 独占** —— 本机输出端点，按曲目原始采样率独占打开
- **ASIO** —— 任意已注册的 ASIO 驱动，采样率逐曲跟随
- **OpenHome** —— 网播设备，带播放列表控制，支持连播无间隙滚动
- **DLNA / UPnP** —— 文件形态的 WAV / FLAC 流，声明真实字节数，网播能显示
  正确时长并在缓冲窗口内拖动

同时强制 Apple Music 请求最高可用无损规格而不是悄悄降级，把它变成一台在受支持
硬件与配置下可以做到 bit-perfect 的 Hi-Fi 播放器。

### 下载

最新版本与安装包见 **[Releases](https://github.com/dxdxdxlm123/AppleMusic-OpenHome/releases/latest)**。
安装包未做代码签名，运行前请核对发布说明里给出的 SHA-256。

### 开发与测试环境

- Windows 11 x64
- Windows 版 Apple Music 文件版本 `1.6.4.90`（初始开发版本）与 `1.7.0.156`（已重新验证）
- 已验证包/框架版本 `1.1540.23042.0`、`1.1792.39682.0`、`1.1792.39942.0` x64

本项目仅在上述开发与测试版本中完成验证，不保证在其他 Apple Music 版本上的运行效果。

### 特性

1. **WASAPI 独占输出**  
   以独占模式直接向当前 Windows 输出设备提交音频，绕过常规共享模式下的 Windows 音频混音路径。

2. **强制最高音频规格**  
   去除 Apple Music 原生“低带宽自动降低音频规格”的逻辑，在最高可用无损规格尚未就绪时继续加载等待，而不是自动回退到更低音频规格。

3. **Bit-perfect 输出**  
   在受支持的播放路径中保持源音频规格，并将整数 PCM 直接提交给 WASAPI；当音源、输出设备与目标格式匹配时，可实现 bit-perfect 播放。

4. **可选采样率与位深转换以兼容设备**

   开启兼容开关后，先尝试原采样率的无损输出；设备不支持时，允许升／降采样、位深转换及 float32 输出，保留 WASAPI 独占和现有音质锁定。例如，44.1 kHz/24-bit 和 96 kHz/24-bit 音源都可转换至仅支持 48 kHz/16-bit 的设备。每个输出格式都必须通过驱动的独占支持检查与初始化，不回退到共享模式。探测覆盖常见整数容器、float32、WAVEFORMATEX／EXTENSIBLE 格式声明；设备的混音采样率也会作为候选，不能直接当作独占支持的证据。

   在 44.1 kHz 及以上（音源更低时以原采样率为准），优先保留音源精度，再尝试 float32／24-bit、16-bit；同一精度组先原采样率，再从最近的更高采样率尝试，最后从高到低尝试更低采样率。更低带宽的输出作为最后选择。升采样使用带限插值抑制频谱镜像，降采样抑制混叠；降低位深使用 TPDF 抖动和饱和舍入。兼容模式也允许本地 int32 音频通过 Apple 解码后的 float32 数据播放，不宣称恢复原始 int32 的 bit-perfect。转换后的输出会明确标注。关闭开关时保留现有无损格式策略与 PCM 路径。

### 推荐设置

在 Apple Music 的 **设置 > 播放** 中：

- 关闭 **交叉渐入渐出**。
- 关闭 **声音增强器**。
- 关闭 **音量平衡**。
- 开启 **无损音频**。
- 将无损音频下方的两项音频规格均设为 **高解析度无损（ALAC）**。
- 关闭 **杜比全景声**。
- 开启 **独占模式**。
- 在 **硬件缓冲区** 中输入 `1`–`100` 毫秒的整数并点击 **确定**。该数值会在下一首歌曲打开独占输出时应用。默认值为 `20` 毫秒，遇到播放问题时可尝试增加数值。
- 设备无法播放原规格时，可开启 **设备不支持原规格时允许兼容格式转换**，下次打开歌曲时生效。
- 关闭 **空间音效**。

使用独立 DAC 和放大器时，将 Apple Music 应用内音量、Windows 输出音量和 DAC 输出音量保持在最大值，只通过放大器调整实际播放音量。使用 DAC/AMP 一体机时，将 Apple Music 应用内音量和 Windows 输出音量保持在最大值，通过 DAC/AMP 一体机的硬件音量控制调整实际播放音量。首次应用这些设置前，请先降低放大器或 DAC/AMP 一体机的硬件音量，避免突然出现过高声压。

### 面板：本机输出与网播输出

Orion 面板有两个标签页：**本机播放**（WASAPI 输出端点或 ASIO 驱动，列出本机已注册的
全部设备／驱动）和 **网播播放**（OpenHome／DLNA 渲染器）。同一时刻只有一个输出处于
活动状态。保存后选择会写入 `am-exclusive.ini`，并立即通过重开当前曲目生效，无需重启
Apple Music。ASIO 是独占的：Apple Music 播放期间其他程序不能同时使用同一驱动。

### 编译与安装

#### 使用 GitHub Actions 编译

1. 打开 [Actions → Build Setup](https://github.com/dxdxdxlm123/AppleMusic-OpenHome/actions/workflows/build-setup.yml)。编译自己的修改时，先 Fork 仓库，并在自己的仓库中启用 Actions。
2. 点击 **Run workflow**，选择分支并运行。
3. 成功后，在该次运行的 **Artifacts** 中下载 **AMExclusive-Setup-x64**，解压得到 `AMExclusive-Setup.exe`。

工作流仅手动触发，编译 Windows x64 Release 并检查安装器内嵌文件，不自动发布 Release。构建机不需要安装 Apple Music：工作流会从 NuGet 下载固定版本的 Microsoft Windows App SDK / WinUI 元数据包，并在编译前使用固定 SHA-256 校验 `Microsoft.UI.Xaml.winmd`。安装 Orion 的电脑仍需安装 Apple Music。构建产物保留 14 天，下载需登录 GitHub。

#### 本地编译

从源码编译需要 Visual Studio 2022 或更新版本的 MSVC Build Tools、Windows 11 SDK、CMake 3.24 或更新版本和 PowerShell。默认情况下，构建脚本会从本机安装的 Windows 版 Apple Music 中读取所需 WinUI 元数据：

```powershell
cd mod
.\scripts\Build-Mod.ps1 -Configuration Release
```

构建机没有安装 Apple Music 时，也可以先准备独立元数据目录再编译：

```powershell
cd mod
.\scripts\Get-WinUIMetadata.ps1 -OutputDirectory .\build\winui-metadata
.\scripts\Build-Mod.ps1 -Configuration Release -WinUIMetadataDirectory .\build\winui-metadata
```

安装器输出位置：

```text
mod\build\dist\AMExclusive-Setup.exe
```

安装或升级时，运行 `AMExclusive-Setup.exe` 并选择 `1`。随后打开 Apple Music，进入 **设置 > 播放**，开启 **独占模式**。

卸载时，运行同一个 Setup 并选择 `2`，或在 **Windows 设置 > 应用 > 已安装的应用** 中卸载 **AMExclusive**（当前版本注册的应用名仍沿用旧标识，见下文「关于命名」）。

### 已知限制

- 仅在 [开发与测试环境](#开发与测试环境) 列出的 Apple Music 版本上验证过。Orion 通过向
  Apple Music 进程注入 DLL 并挂钩其解码回调工作，Apple Music 自身更新后挂钩偏移可能失效。
- 安装包**未做代码签名**，首次运行会被 Windows SmartScreen 拦截，需要手动确认并核对
  SHA-256。
- ASIO 输出是独占的，同一 ASIO 驱动不能与其他程序同时使用。
- 网播的 `live`（FLAC 实时流）仍属 preview 语义：跨采样率连播组合（如 96→44.1、192→96）
  未逐一实测，单曲 10 分钟以上的长播放待实测，`416`（`offset < oldest`）分支未在回归靶子
  中实测。默认仍是 `wav`（文件形态），遇到无声或断流时切回 `wav` 即恢复上一版行为。
- 不保证在不同软件、硬件、驱动程序或音频设备条件下正常运行。

完整的逐版本限制记录见 [RELEASE_NOTES.md](RELEASE_NOTES.md)。

### 关于命名

本项目名为 **Orion**，仓库名为 **AppleMusic-OpenHome**（Orion 是产品名，AppleMusic-OpenHome
是仓库名）。它派生自上游 [AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive)，
但**不再沿用 AMExclusive / AME 作为本项目名称**，词标也重新设计为猎户座星座图形加几何字母
造型，不复用上游的 logo 文件与字母轮廓，以免被误认为上游项目本身或它的官方续作。文档、说明
与仓库元信息统一使用 Orion。

为了让既有安装和配置文件继续可用，以下**技术标识暂时保留旧名**，它们只是文件名／产物名，
与项目名无关，后续版本会另行规划迁移：

| 保留的旧标识 | 位置 | 说明 |
|---|---|---|
| `am-exclusive.ini` | `%PUBLIC%\AMExclusive\` | 配置文件，改名会让既有设置失效 |
| `am-exclusive.log` / `am-exclusive-dlna.log` / `am-exclusive-broker.log` | `%PUBLIC%\AMExclusive\` | 日志文件 |
| `AMExclusive-Setup.exe` | 安装器产物 | 由构建目标产生 |
| `AMExclusive-Setup-x64` | Actions 产物名 | 由工作流上传步骤决定 |
| `AMExclusive` | Windows 已安装应用列表 | 安装器注册的应用名 |

### 致谢

本项目的起点和主要代码基础来自上游
[AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive)。**衷心感谢原作者
[AntiOblivionis](https://github.com/AntiOblivionis)** 的开创性工作，以及把项目以 MIT
许可证开放出来的决定 —— 没有它就没有 Orion。Orion 是在它的 WASAPI 独占输出之上，增加了
DLNA / OpenHome 网播输出、格式协商、控制点与流媒体服务等一系列能力。上游全部提交历史都完整
保留在本仓库中，贡献名单见 [CONTRIBUTORS.md](CONTRIBUTORS.md)。

感谢 [LINUX DO 社区](https://linux.do/) 在项目开发与测试过程中提供的交流、支持与反馈。

本项目使用了以下开源项目：

- [MinHook](https://github.com/TsudaKageyu/minhook) — 提供 Windows x64/x86 API Hook 基础能力。
- [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue) — 提供音频实时路径使用的单生产者、单消费者有界队列。

感谢上述项目及其贡献者。完整版本、版权和许可证信息见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

### 协议与声明

项目自行编写的源码使用 [MIT License](LICENSE)。第三方组件继续遵循其各自的上游许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 与 [NOTICE.md](NOTICE.md)。

Orion 是一个独立且非官方的开源项目，与 Apple Inc. 没有任何关联，也未获得其许可或背书。Apple Music 是 Apple Inc. 的商标。

本项目不保证在不同软件、硬件、驱动程序或音频设备条件下能够正常运行。使用本项目可能造成不可预知的后果，使用者应自行承担相关风险。

本项目不分发 Apple 软件、Apple 二进制文件、解密后的媒体或 DRM-free 参考文件，也不提供用于绕过 DRM 保护的功能。

---

## English

### What it is

Orion (AppleMusic-OpenHome) is an open-source project for Apple Music on Windows. It taps
PCM straight out of Apple's own decoder and submits it to the device itself,
bypassing the Windows mixer, with four bit-perfect output paths:

- **WASAPI exclusive** — the local endpoint, opened exclusively at the track's own rate
- **ASIO** — any registered ASIO driver, sample rate followed per track
- **OpenHome** — network renderers with playlist control (gapless auto-advance)
- **DLNA / UPnP** — file-shaped WAV or FLAC stream with a real byte count, so the
  renderer shows the correct duration and can seek inside the burst window

It also forces Apple Music to request the highest available lossless tier instead
of silently downgrading, turning it into a Hi-Fi player that can reach bit-perfect
output on supported hardware and configurations.

### Download

The latest build and installers are in
**[Releases](https://github.com/dxdxdxlm123/AppleMusic-OpenHome/releases/latest)**.
The installer is not code-signed; verify the SHA-256 published in the release notes
before running it.

### Development and Test Environment

- Windows 11 x64
- Apple Music for Windows file version `1.6.4.90` (original development build) and `1.7.0.156` (re-verified)
- Verified package/framework builds `1.1540.23042.0`, `1.1792.39682.0` and `1.1792.39942.0` x64

This project has only been validated with the development and test versions listed above. Its behavior on other Apple Music versions is not guaranteed.

### Features

1. **WASAPI exclusive output**  
   Sends audio directly to the selected Windows output device in exclusive mode, avoiding the normal shared-mode Windows audio mixing path.

2. **Forced highest audio quality**  
   Removes Apple Music's native low-bandwidth quality downgrade behavior and keeps waiting for the highest available lossless stream instead of automatically falling back to a lower audio specification.

3. **Bit-perfect output**  
   Preserves the source sample format through the supported playback path and submits integer PCM directly to WASAPI, allowing bit-perfect playback when the source, device, and supported output format match.

4. **Optional sample-rate and bit-depth conversion for device compatibility**

   The optional compatibility switch first tries lossless output at the original rate, then permits up/downsampling, bit-depth conversion and float32 output while retaining WASAPI exclusive output and the existing quality lock. For example, both 44.1 kHz/24-bit and 96 kHz/24-bit sources can use a 48 kHz/16-bit-only endpoint. Each output format must pass the driver's exclusive support check and initialization; shared-mode output is never a fallback. The probe includes common integer containers, float32, alternate WAVEFORMATEX/EXTENSIBLE declarations and the device's mix rate as a candidate only.

   Among rates at or above 44.1 kHz (or the source rate if lower), source precision is preferred, then float32/24-bit, then 16-bit. Within each precision group, try the original rate, the nearest higher rates, then descending lower rates. Lower-bandwidth outputs are the last resort. Upsampling uses a band-limited interpolation filter to suppress spectral images; downsampling suppresses aliasing. Depth reduction uses TPDF dither and saturating rounding. Compatibility mode also accepts local int32 audio through Apple's decoded float32 stream without claiming original int32 bit-perfect recovery. Converted output is labelled explicitly. With the switch off, the existing lossless format policy and PCM path are unchanged.

### Recommended Settings

In Apple Music, open **Settings > Playback** and use the following settings:

- Turn **Crossfade** off.
- Turn **Sound Enhancer** off.
- Turn **Sound Check** off.
- Turn **Lossless Audio** on.
- Set both audio-quality selectors below Lossless Audio to **Hi-Res Lossless (ALAC)**.
- Turn **Dolby Atmos** off.
- Turn **Exclusive Mode** on.
- Enter an integer from `1` through `100` milliseconds in **Hardware buffer** field and select **Apply**. The setting is used when exclusive output opens for the next song. The default is `20` ms; try a higher value if encountering playback problems.
- Enable **Allow compatible format conversion for unsupported formats** if your device cannot play the original format. The setting applies when the next song opens.
- Turn **Spatial sound** off.

With a separate DAC and amplifier, keep the Apple Music app volume, Windows output volume, and DAC output volume at maximum, and adjust listening volume only on the amplifier. With an integrated DAC/amp, keep the Apple Music app volume and Windows output volume at maximum, and adjust listening volume using the DAC/amp hardware control. Before applying these settings for the first time, lower the amplifier or DAC/amp hardware volume to avoid an unexpectedly high sound level.

### The panel: local and network output

The Orion panel has two tabs: **Local playback** (WASAPI endpoint or ASIO driver,
listing every device/driver registered on the machine) and **Network playback**
(OpenHome/DLNA renderer). Exactly one output is active. Saving persists the
choice in `am-exclusive.ini` and applies it immediately by restarting the
current track - no Apple Music restart is needed. ASIO is exclusive: no other
application may use the same driver while Apple Music is playing.

### Build and Install

#### Build with GitHub Actions

1. Open [Actions → Build Setup](https://github.com/dxdxdxlm123/AppleMusic-OpenHome/actions/workflows/build-setup.yml). To build your own changes, fork the repository and enable Actions in your fork first.
2. Select **Run workflow**, choose the branch, and run it.
3. When the run succeeds, download **AMExclusive-Setup-x64** from **Artifacts** and unzip it to obtain `AMExclusive-Setup.exe`.

The workflow runs only when manually triggered. It builds Windows x64 Release and checks the embedded installer payload; it does not publish a GitHub Release. No Apple Music installation is needed on the runner: it downloads pinned Microsoft Windows App SDK / WinUI metadata packages from NuGet and verifies `Microsoft.UI.Xaml.winmd` against a pinned SHA-256 before building. Apple Music is still required on the PC where you install Orion. Artifacts are retained for 14 days; downloading them requires signing in to GitHub.

#### Build locally

To build from source, install Visual Studio 2022 or newer MSVC Build Tools, a Windows 11 SDK, CMake 3.24 or newer, and PowerShell. By default, the build script reads the required WinUI metadata from the locally installed Apple Music package:

```powershell
cd mod
.\scripts\Build-Mod.ps1 -Configuration Release
```

Apple Music does not need to be installed on the build machine if you provide standalone metadata instead:

```powershell
cd mod
.\scripts\Get-WinUIMetadata.ps1 -OutputDirectory .\build\winui-metadata
.\scripts\Build-Mod.ps1 -Configuration Release -WinUIMetadataDirectory .\build\winui-metadata
```

The installer will be generated at:

```text
mod\build\dist\AMExclusive-Setup.exe
```

To install or upgrade, run `AMExclusive-Setup.exe` and choose `1`. Then open Apple Music, go to **Settings > Playback**, and enable **Exclusive mode**.

To uninstall, run the same setup and choose `2`, or uninstall **AMExclusive** from **Windows Settings > Apps > Installed apps** (the registered app name still uses the legacy identifier - see “About the name” below).

### Known limitations

- Only the Apple Music versions listed under
  [Development and Test Environment](#development-and-test-environment) are verified. Orion
  works by injecting a DLL into the Apple Music process and hooking its decode callback, so a
  Apple Music update can invalidate the hook offsets.
- The installer is **not code-signed**; SmartScreen will block the first run. Verify the
  SHA-256 published in the release notes.
- ASIO output is exclusive - the same ASIO driver cannot be shared with another application.
- The `live` FLAC transport for network playback is still preview-grade: cross-sample-rate
  auto-advance combinations (96→44.1, 192→96) are not individually tested, long single tracks
  over 10 minutes are pending, and the `416` (`offset < oldest`) branch has not been exercised
  by the regression harness. The default remains `wav` (file-shaped); switching back to `wav`
  restores the previous release's behaviour.
- Correct operation across arbitrary software, hardware, driver or audio-device combinations
  is not guaranteed.

The full per-release list is in [RELEASE_NOTES.md](RELEASE_NOTES.md).

### About the name

The product is called **Orion**; the repository is **AppleMusic-OpenHome** (Orion is the project
name, AppleMusic-OpenHome is the repository name). It is derived from the upstream
[AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive), but it no longer uses
AMExclusive / AME as its own name, and the wordmark is a fresh design - an Orion constellation
figure plus geometric letterforms that do not reuse the upstream logo file or its letter outlines -
so nobody mistakes this repository for the upstream project or its official successor. All
documentation and repository metadata use Orion.

To keep existing installs and settings working, the following **technical identifiers still carry
the legacy name**. They are file and artifact names, not the project name; migrating them is a
separate, later task:

| Legacy identifier kept | Where | Why |
|---|---|---|
| `am-exclusive.ini` | `%PUBLIC%\AMExclusive\` | Config file - renaming it would drop existing settings |
| `am-exclusive.log` / `am-exclusive-dlna.log` / `am-exclusive-broker.log` | `%PUBLIC%\AMExclusive\` | Log files |
| `AMExclusive-Setup.exe` | installer artifact | Produced by the build target |
| `AMExclusive-Setup-x64` | Actions artifact name | Set by the workflow upload step |
| `AMExclusive` | Windows installed-apps list | Name registered by the installer |

### Acknowledgements

This project starts from, and is largely built on, the upstream
[AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive).
**Sincere thanks to its author, [AntiOblivionis](https://github.com/AntiOblivionis)**, for the
original work and for releasing it under the MIT license — without it there would be no Orion.
Orion adds the DLNA / OpenHome network playback path, format negotiation, the control point and
the stream server on top of upstream's WASAPI-exclusive output. The complete upstream commit
history is preserved in this repository; the contributor list is in
[CONTRIBUTORS.md](CONTRIBUTORS.md).

Thanks to the [LINUX DO community](https://linux.do/) for the discussion, support, and feedback provided during the development and testing of this project.

This project uses the following open-source projects:

- [MinHook](https://github.com/TsudaKageyu/minhook) — provides the Windows x64/x86 API hooking foundation.
- [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue) — provides the bounded single-producer, single-consumer queue used on the real-time audio path.

Thanks to these projects and their contributors. Complete version, copyright, and license information is available in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

### License and Disclaimer

Project-authored source code is released under the [MIT License](LICENSE). Third-party components retain their respective upstream licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [NOTICE.md](NOTICE.md).

Orion is an independent and unofficial open-source project. It is not affiliated with, endorsed by, or licensed by Apple Inc. Apple Music is a trademark of Apple Inc.

This project is not guaranteed to operate correctly across different software, hardware, driver, or audio-device configurations. Its use may cause unforeseen consequences, and users assume the associated risks.

This project does not distribute Apple software, Apple binaries, decrypted media, or DRM-free reference files, and it does not provide functionality intended to bypass DRM protection.

---

## 打赏支持

如果 Orion 让你多听了几年好歌，欢迎请作者喝一杯 —— 你的打赏就是继续更新的动力。
**点击下面的二维码即可放大**，长按或截图后扫码均可。

If Orion has been useful to you, a small tip keeps the project alive.
**Click a code below to enlarge it**, then scan or long-press to pay.

<table align="center">
  <tr>
    <td align="center" valign="top">
      <img src="assets/donate-wechat.png" alt="微信赞赏码 WeChat reward code" width="180"><br>
      <b>微信赞赏 · WeChat</b><br>
      <sub>点击放大 / Click to enlarge</sub>
    </td>
    <td width="48"></td>
    <td align="center" valign="top">
      <img src="assets/donate-alipay.jpg" alt="支付宝收款码 Alipay QR code" width="180"><br>
      <b>支付宝 · Alipay</b><br>
      <sub>点击放大 / Click to enlarge</sub>
    </td>
  </tr>
</table>

<p align="center">
  <sub>没有弹出浮层时，点下面的文字可在新标签页打开原图：
  <a href="assets/donate-wechat.png">微信赞赏码</a> ·
  <a href="assets/donate-alipay.jpg">支付宝收款码</a></sub>
</p>

打赏完全自愿，与项目功能无关，不构成任何形式的服务承诺。
Tipping is entirely optional, grants no support entitlement, and does not affect the MIT license terms.
