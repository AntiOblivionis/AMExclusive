# v0.4.0-preview — HiFi 播放器模式 · 网播全面优化

## 概述

新增「HiFi 播放器」模式：所有输出（WASAPI 独占 / ASIO / DLNA 网播）都可以由一个独立的
引擎进程承载，Apple Music 只负责解码与 PCM 搬送。默认关闭，切换在面板内完成。

## 新增

### HiFi 播放器模式（默认关闭）
- 面板：输出方式 → 本地播放 → **播放模式**（AM 播放器 / HiFi 播放器），持久化到
  `[mod] player_mode`。
- HiFi 模式下，独立的 `am-exclusive-audiohost.exe` 拥有全部输出路径：
  - WASAPI 独占 / ASIO
  - DLNA/OpenHome 网播（SOAP 控制 + HTTP 流服务器 + FLAC 编码整体迁入）
  - 定时器精度、专属核心、大页、进程优先级等调度优化全部在引擎进程内生效
- 引擎进程：高优先级（High）、音频线程 MMCSS Pro Audio（实时类 18）、共享环锁页、
  大页阶梯分配、单实例、崩溃自动重启（带退避）。
- 传输：16 MB 跨进程 SPSC 共享环（Q31 交错，192k 下约 10 秒）+ 命名管道控制通道。
  环使用 SEC_LARGE_PAGES 大页分配，失败自动回退普通页并整段预驻留 + 锁页。
- 协议版本握手：引擎与 hook 版本不匹配时拒绝并回退进程内 sink，不会产生静音或坏数据。
- 断流自动恢复（网播）：设备停止拉流 8 秒后自动重开当前曲目（每曲最多 3 次）。

### 网播（DLNA）优化
- 面板「网播播放」新增**拉流缓冲**（3 / 5 / 8 秒），直接写 ini，下一曲生效。
  部分设备（AURALiC VEGA G2.2）要求 ≥3 秒预缓冲，否则停在探测阶段不拉流。
- 流缓冲（ring）与拉流缓冲解耦：`[dlna] ring_seconds`（默认 8 秒），设备短暂停拉
  不再反压 Apple Music 的解码队列。
- 网络层：关闭 Nagle 小包攒批、QoS 优先标记（DSCP EF）。
- DLNA.ORG_CI 标记修正：服务器端转码的内容必须标 CI=0（CI=1 是"经连接管理器转码"
  的含义，vg2.2 见到 CI=1 会停止消费）。
- 曲目真实时长获取加固（SMTC 快照 900ms → 2500ms），192kHz 曲目不再声明错误的
  Content-Length。

### 面板
- 网播页新增「拉流缓冲」下拉。
- 本地播放页新增「播放模式」下拉（HiFi 模式）。

## 修复
- 保存输出设置时 broker 会用本地缓冲值覆盖面板写的 buffer_ms（1500），现在改为
  重读 ini。
- DLNA.ORG_CI 标记错误（见上）。
- SMTC 时长快照超时过短（见上）。
- 0.3.2 的全部修复包含在内。

## 实测（AURALiC VEGA G2.2，192kHz/24bit）
- 453 MB 声明长度全程连续拉流，无断流无重连。
- submittedFrames 以 192 kHz 实时速率增长，环形缓冲水位毫秒级。
- 引擎进程内存约 22 MB（High 优先级），Apple Music 进程不再承载任何音频设备逻辑。

## 兼容性说明
- 拉流缓冲低于 3 秒的选项已移除（设备门槛）。
- 网播仍是 IPv4、单设备、不支持 Seek（拖进度条）。
- DLNA 的 HiFi 路径未经其他品牌设备验证；AM 播放器模式行为与 0.3.2 完全一致。

## 校验
| 文件 | 大小 | SHA-256 |
|---|---|---|
| `AMExclusive-Setup-0.4.0-preview-20261105.exe` | 3,566,592 B | `4CED56044B9028D9613690A318E997D38D797BB86270EE9263244C11D0147FD2` |
| `AMExclusive-0.4.0-preview-20261105.zip` | 1,589,992 B | `2D3F5961321093BCC0EA1FA56FD6200F6731EA1619DB863C13F48E7E34EDAC51` |

## 致谢
ASIO 输出派生自 [yeluo53/AMExclusiveASIO](https://github.com/yeluo53/AMExclusiveASIO) 与 [magua521/AM-ASIO-MAD](https://github.com/magua521/AM-ASIO-MAD)（均为 MIT），详见 THIRD_PARTY_NOTICES.md。
