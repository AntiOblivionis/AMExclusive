# v0.4.6-preview — FLAC 实时流 / 曲目边界与设备重连修复（2026-11-07）

> **这是 PREVIEW，不是稳定版。** 协议级回归、AM 播放列表模拟，以及**真机两首
> 44.1/16 → 192/24 自动连播**均已通过。播放列表内若混用更多采样率组合，尚未
> 逐一实测。若遇到无声、断流或歌名不更新：把面板「流式」切回 **WAV（文件形态）**
> （下一曲生效）即恢复 0.4.5 的行为；必要时重新安装 0.4.5，两个版本都无需先卸载。

## 本版变更

| 类别 | 内容 |
|---|---|
| 新增 | 面板「流式」三档：`wav`（默认）/ `flac-file` / `live`（FLAC 实时流） |
| 新增 | live：close-delimited FLAC、无 Content-Length、一次会话一个稳定 URL；曲目边界由控制点 `Insert + SeekId` 主动切项，每首歌名/时长正确 |
| 移除 | Songcast（OHU 推送）实验模式：VG2.2 的 `Receiver:1` 是空壳（`SetSender` 超时、从不 Join）。旧 ini 的 `songcast` 按 `wav` 处理 |
| 修复 | 开头被吃掉：探测连接前 5 秒不回收 ring，正式 GET 从第 0 字节开始 |
| 修复 | 长播放整链路冻结：设备不读时 live 丢弃最旧数据（广播语义），`Append` 可取消，`Stop` 先 1 秒优雅 join 再强制收尾 |
| 修复 | 设备重连：live-edge `Range` 回 `206` 并按 FLAC 帧起点对齐续流；越界 416 改为 `Content-Range: bytes */*`，不再宣布资源结束 |
| 修复 | **跨采样率自动连播第二首无声**：Apple Music 换 44.1→192 会拆旧图，runtime 之前等一个永不完成的 EOF 排空，把 192k 的重绑一直挂起；现在排空有 3 秒上限 |
| 修复 | file 形状 EOS 空转、退休会话与 ring 泄漏 |
| 工具 | `tools/dlna_reconnect_renderer.py`、`tools/run_am_playlist_sim.py`（可复现的回归靶子） |
| 诊断 | 主日志新增 `v2 rebind` 前缀的重绑状态行（仅切换点输出，已节流） |

## 验证结果（本次打包前全部通过）

| 用例 | 结果 |
|---|---|
| Release 全量构建（7 个 target） | 零错误；Setup / hook 的 FileVersion = `0.4.6.0` |
| `AMExclusive-Setup.exe --extract-test` | 通过，12 个嵌入资源校验并清理 |
| wav 44.1/16、live 44.1/16、live 连播 3 首 | PASS |
| live 96/24、192/24 | PASS，STREAMINFO rate/bits 与请求一致 |
| live 206 重连（30 s） | PASS，`206 + Content-Range: bytes <帧起点>-/*` |
| AM 播放列表模拟 44.1/16 → 192/24 | PASS，两首 STREAMINFO 正确、均 206 续流、无 416 / 死锁 / BEX64 |
| Windows 错误报告 | 回归期间无 selftest 崩溃事件 |
| **真机 44.1/16 → 192/24 两首自动连播** | **PASS**：`finalize-run`(3016ms) → `pending-apply` → `bound a=192000 d=24`；dlna `open device rate=192000 bits=24`、第二首 `fLaC`、后续多次 `206` 续流；`capturedFrames` 6.2M→63.8M 持续到曲终 |

## 安装

1. 退出 Apple Music（安装器会自动停，但手动退更稳妥）
2. 管理员运行 `AMExclusive-Setup-0.4.6-preview-20261107.exe`，选「1 安装/升级」
3. 重新打开 Apple Music → 面板「网播播放」→「流式」选择模式（下一曲生效）

默认仍是 `WAV（文件形态）`，升级不会改变你现有的 ini 设置。

## 校验

安装包**未做代码签名**，运行前请核对：

| 文件 | 大小 | SHA-256 |
|---|---|---|
| `AMExclusive-Setup-0.4.6-preview-20261107.exe` | 3,627,008 B | `3155AD21069D19E27E1E61F41E437945DAB523470BAC87F37CB6CB6D2E804C37` |
| `AMExclusive-0.4.6-preview-20261107.zip` | 1,616,725 B | `31A4571C65E956086187B4E5C20C81CFD7A683F9E6E8128325CA25EC929B743A` |

zip 内为上述 exe 与 `使用说明.txt`，CRC 校验通过。

## 已知限制

- **真机 44.1/16 → 192/24 自动连播曾失败，现已修复并复验通过。**
  13:26 那轮第二首静音的根因是 `AudioCoreRuntime::FinalizeEndOfStreamDrainLocked()`
  等一个永不完成的 `coordinator_.EndOfStreamComplete()`（live 不 seal session，
  AM 拆图后 producer 不再收到 `S_FALSE`），`graphBound_` 一直为真，192k 的
  `pendingGraphBind_` 永不落地。现在 EOF 排空有 3 秒上限，超时强制 finalize。
  14:41–14:51 重放验证：`finalize-run` → `pending-apply` → `bound a=192000`，
  第二首完整出流。
- `416`（`offset < oldest`）分支已实现但未在回归靶子中实测
- 「设备约 32 MiB 读缓冲上限」只是待验证假说，两次现场字节数相差 1.9 MB
- 早期 `206` 尝试触发过 BEX64 / 0xC0000409，已回退并重写，根因未定位（无 WER dump）；本次全量回归未再复现
- 更多采样率组合（如 96→44.1、192→96）未逐一实测；单曲 >10 分钟长播放待实测

## 许可

MIT；派生改动见 `THIRD_PARTY_NOTICES.md` / `NOTICE.md`。上游
[AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive)。
