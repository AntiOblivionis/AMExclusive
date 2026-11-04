# AMExclusive-Openhome

### 让 Windows 版 Apple Music 输出到 WASAPI 独占设备 / OpenHome 网播

[English](README.md) · [上游项目](https://github.com/AntiOblivionis/AMExclusive) · [更新日志](CHANGELOG.md) · [署名声明](NOTICE.md)

> **非官方分支。** 本仓库是 [AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive)
> （MIT License）的派生项目，由本仓库维护者独立维护，与 Apple Inc. 及上游作者均无关联，
> 也未获得其许可或背书。`amexclusive.tgwgroup.ltd` 是**上游项目**的官网，不是本分支的官网。

## 本分支增加了什么

上游项目提供 WASAPI 独占输出和强制最高音质无损流。本分支完整保留这些能力，并新增
**DLNA / OpenHome 输出**，让同一路捕获到的 PCM 可以送往网络播放器，而不只是本机 DAC。

| | 上游 | 本分支 |
|---|---|---|
| WASAPI 独占输出 | ✅ | ✅（保持不变） |
| 强制最高音质无损 | ✅ 单一 CoreMedia 版本 | ✅ 两个已验证版本，不匹配则安全降级 |
| 无外接 DAC 时强制高解析档 | — | ✅ `force_hires=1` |
| DLNA / OpenHome 网播输出 | — | ✅ |
| 每首歌新链接 + 真实曲长 | — | ✅ 通过 Windows 媒体会话获取 |
| 音频缓冲锁页 + 生产者线程提权 | — | ✅ |

## 开发与测试环境

- Windows 11 x64、Visual Studio 2022 MSVC Build Tools、CMake 3.24+、PowerShell 5.1
- **本分支实测**：Apple Music `1.1792.39682.0` / CoreMedia `1.2 (3365.13)` x64
- **沿用上游已验证环境**：Apple Music `1.6.4.90` / 包框架 `1.1540.23042.0` x64
- 实测网播：AURALiC VEGA G2.2（`LightningPlayer`，OpenHome），
  播放 `WAV 96 kHz / 24-bit / 双声道`，`converted=0`（无重采样、无位深转换）

质量锁依赖与具体 Apple Music 构建绑定的私有镜像指纹。遇到其他版本时会**安全降级**：
播放与 DLNA 输出照常工作，但音质档位由 Apple Music 自行决定。查看
`am-exclusive.log` 中是否出现 `Highest ALAC quality filter matched build profile`
即可确认是否生效。其他版本与设备不保证可用。

## 功能

### 沿用上游

1. **WASAPI 独占输出** —— 直接向选定端点提交音频，绕开 Windows 共享模式混音路径。
2. **强制最高音频规格** —— 拒绝 Apple Music 的低带宽降级，等待最高可用无损流，
   不自动回退到 AAC。
3. **Bit-perfect 输出** —— 音源、设备与端点格式匹配时，整数 PCM 原样提交给 WASAPI。
4. **可选格式转换以兼容设备** —— 升／降采样、位深转换、float32 输出，使用 TPDF
   抖动与饱和舍入；共享模式永不作为兜底，转换输出会明确标注。

### 本分支新增

5. **DLNA / OpenHome 输出** —— SSDP 发现、`ConnectionManager.GetSinkProtocolInfo`
   格式协商、OpenHome `Playlist.DeleteAll + Insert + Play` 控制。网播通过 HTTP 从
   Apple Music 进程内的服务器拉取 WAV/LPCM 流。每首歌开启新 URL，并重新协商采样率与位深。
6. **文件形态的实时流** —— `LightningPlayer` 一类网播遇到没有总长度的响应体会直接报
   "不支持此类格式"。因此本分支声明 `Content-Length`、支持 `Accept-Ranges: bytes`，
   对 `Range: bytes=N-` 返回 `206 + Content-Range`。音频仍然是实时产生的，声明长度只是上限。
7. **真实曲目时长** —— 由辅助程序读取 Windows 媒体会话（SMTC）时间线，使声明长度与
   网播进度条与歌曲一致；拿不到时间线时回退到 `[dlna] stream_seconds`。
8. **强制高解析档** —— `force_hires=1` 把请求档位提升到 20（最高 24-bit/192 kHz），
   即使 Apple Music 没有检测到外接 DAC。严格过滤器仍只取"条目里实际存在的最高采样率"：
   96 kHz 曲目走 96 kHz，只有 44.1 kHz ALAC 就落到 44.1 kHz；只要条目里有无损版本，
   AAC 候选就被剔除。条目本身只有 AAC 时仍可播放。
9. **多版本质量锁** —— 两个已验证 CoreMedia profile，使用 SHA-256 指纹；镜像不匹配时
   直接关闭质量锁，不会去 hook 未知代码。
10. **实时链路加固** —— 两级音频缓冲 `VirtualLock`（配合工作集配额提升），
    DLNA 生产者线程提升到 `THREAD_PRIORITY_ABOVE_NORMAL`。

## 已知限制

- **实时流不可拖动进度条。** 网播端拖动会请求尚未产出的音频，无法实现。请在
  Apple Music 里拖动：它会重建音频图，本分支会跟着换 URL 重新推送（约 1~2 秒重连）。
- **声明长度 = 曲目时长 + 3 秒。** 播完时网播会停在最后几秒等待，属显示问题。
- Apple Music 启动后的第一首歌，如果 SMTC 辅助程序还没吐出第一条时间线，会回退到
  `stream_seconds`（默认 600 秒）。
- `force_hires=1` 配合 WASAPI 独占、而本机 DAC 最高只支持 48 kHz 时，96/192 kHz 曲目
  可能打不开端点（表现为没声音）。请设 `force_hires=0`，或打开 `allow_resampling=1`。
  DLNA 输出不受影响：格式会按网播上报的能力协商。

## 配置

安装程序会写入 `%PUBLIC%\AMExclusive\am-exclusive.ini`。升级安装会保留你改过的值。

```ini
[mod]
mode=exclusive
period_100ns=200000
allow_resampling=0
output=wasapi        ; wasapi | dlna
force_hires=1        ; 设为 0 恢复 Apple 原生档位选择

[dlna]
device_udn=          ; 在应用内扫描设备后自动填写
device_name=
location=
buffer_ms=1500       ; 预缓冲目标，调大可容忍更多网络抖动
format_pref=lpcm
max_bits=24
manual_urls=
pcm_byte_order=big   ; 裸 audio/L16、audio/L24 按 RFC 3551/3190 为大端
stream_seconds=600   ; 拿不到 SMTC 时间线时的兜底声明长度
follow_track_duration=1
```

## Apple Music 推荐设置

在 **设置 > 播放** 中：关闭 **交叉渐入渐出**、**声音增强器**、**音量平衡**、
**杜比全景声**、**空间音效**；开启 **无损音频** 和 **独占模式**；无损音频下方两项
规格都设为 **高解析度无损（ALAC）**。**硬件缓冲区** 填 `1`–`100` 毫秒整数
（默认 `20` 毫秒，播放有问题可增大）并点击 **确定**。仅在设备无法播放原规格时
开启 **设备不支持原规格时允许兼容格式转换**。

使用独立 DAC 与放大器时，把应用内音量、Windows 输出音量、DAC 输出音量保持最大，
只在放大器上调音量。首次应用这些设置前，请先降低硬件音量，避免突然出现过高声压。

## 编译与安装

### GitHub Actions

在自己的 fork 中打开 **Actions → Build Setup → Run workflow**（先启用 Actions），
成功后从 Artifacts 下载 `AMExclusive-Setup-x64`。工作流只手动触发，编译 Windows x64
Release 并校验安装器内嵌载荷，不自动发布 Release。构建机不需要安装 Apple Music：
固定版本的 Windows App SDK / WinUI 元数据来自 NuGet，并在编译前用 SHA-256 校验
`Microsoft.UI.Xaml.winmd`。构建产物保留 14 天。

### 本地编译

需要 Visual Studio 2022 或更新版本的 MSVC Build Tools、Windows 11 SDK、
CMake 3.24+、PowerShell。

```powershell
cd mod
.\scripts\Build-Mod.ps1 -Configuration Release
```

构建机没有 Apple Music 时：

```powershell
cd mod
.\scripts\Get-WinUIMetadata.ps1 -OutputDirectory .\build\winui-metadata
.\scripts\Build-Mod.ps1 -Configuration Release -WinUIMetadataDirectory .\build\winui-metadata
```

安装器输出：`mod\build\dist\AMExclusive-Setup.exe`。

**请以管理员身份运行安装程序。** 伴生程序需要把计划任务注册到任务根目录，
非提权进程会收到 Access denied。安装完成后**完全退出并重新打开 Apple Music**，
然后在应用内面板选择输出：本机 DAC 选 **WASAPI 独占**，网播选 **DLNA** 并先扫描设备。

卸载：再次运行 Setup 选 `2`，或从 **Windows 设置 > 应用 > 已安装的应用** 卸载，
也可运行 `%PUBLIC%\AMExclusive\Uninstall.cmd`。

### 日志

日志位于 `%PUBLIC%\AMExclusive\`：

| 文件 | 内容 |
|---|---|
| `am-exclusive.log` | hook、质量锁、音频链路 telemetry |
| `am-exclusive-dlna.log` | 开流、HTTP 请求、格式协商 |
| `am-exclusive-broker.log` | SSDP 发现与配置 |

提 Issue 时请附上日志，见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 开发说明

本分支部分改动在 AI 辅助下完成。所有改动均经过人工审阅、编译，并在实体
AURALiC VEGA G2.2 网播上验证后才提交；上游项目自身的提交历史中也有类似的
AI 协作署名。

## 致谢

- [AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive) —— 原项目及其贡献者，MIT License。
- [LINUX DO 社区](https://linux.do/) —— 上游开发测试过程中的交流与反馈。
- [MinHook](https://github.com/TsudaKageyu/minhook) —— Windows API Hook 基础能力。
- [rigtorp/SPSCQueue](https://github.com/rigtorp/SPSCQueue) —— 实时音频路径上的有界 SPSC 队列。

完整版本、版权与许可证信息见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
与 [NOTICE.md](NOTICE.md)。

## 协议与声明

项目自行编写的源码使用 [MIT License](LICENSE)，第三方组件继续遵循各自的上游许可证。
原版权声明与许可声明在 `LICENSE` 中原样保留；本分支的改动清单见 [NOTICE.md](NOTICE.md)。

AMExclusive-Openhome 是独立且非官方的开源项目，与 Apple Inc. 没有任何关联，
也未获得其许可或背书。Apple Music 是 Apple Inc. 的商标。对第三方闭源应用进行
hook 可能受该应用软件许可条款限制：本项目仅供个人互操作性使用，使用者需自行确认
是否符合当地法律及 Apple 的相关条款。

本项目不保证在不同软件、硬件、驱动程序或音频设备条件下正常运行。使用本项目可能造成
不可预知的后果，使用者应自行承担相关风险。

本项目不分发 Apple 软件、Apple 二进制文件、解密后的媒体或 DRM-free 参考文件，
也不提供用于绕过 DRM 保护的功能。`mod/src/ApplePrivateOffsets.h` 中的版本指纹只是
短指令序列的 SHA-256 摘要，用于确认已加载镜像是否为审阅过的构建；本仓库不包含
任何 Apple 代码或二进制文件。
