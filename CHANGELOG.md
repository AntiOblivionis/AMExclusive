# Changelog

All notable changes to this fork are documented here. The baseline is the
upstream [AntiOblivionis/AMExclusive](https://github.com/AntiOblivionis/AMExclusive)
`main` branch at the time of the fork (2026-11-04).

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.4.6-preview] - 2026-11-07

### Added

- **Selectable transport modes for network playback.** The panel's stream
  selector now offers three modes, persisted as `[dlna] stream_format`:
  - `wav` / `flac-file` — the existing file-shaped streams, unchanged. Both
    remain the default and the universal fallback.
  - `live` — a close-delimited FLAC live stream: real FLAC frames with
    `STREAMINFO total_samples = 0`, **no** `Content-Length` and **no**
    `Transfer-Encoding`, served over **one stable URL for the whole listening
    session**. Each track boundary closes the connection cleanly, which an
    OpenHome playlist treats as end-of-item and auto-advances (the alias is
    re-pointed at the next track's ring, so the address never expires). Range
    is not advertised (`DLNA.ORG_OP=00`), the DIDL item is
    `object.item.audioItem.audioBroadcast`, and a late/reconnecting GET attaches
    at the oldest retained byte instead of being refused. This removes the
    stale-URL 410 storm class of failure at the root, and matches the framing
    foo_out_upnp and AirConnect prefer for realtime PCM. Verified playing on a
    VEGA G2.2 (`Playlist.TransportState = Playing`, stable pull).
    Track boundaries are switched by the control point itself
    (`Playlist.Insert` + `Playlist.SeekId`): a live body has no length, so the
    renderer would otherwise never advance, and closing it produces a resume
    Range rather than an end-of-item. Metadata, duration and sample rate are
    therefore correct per track; the previous session is retired for 5 s and
    then aborted, so the renderer can finish moving without seeing a reset.
- `DlnaTarget` now carries `openHomeProduct` (and `openHomeReceiver`, unused
  after the Songcast removal); the control client gained Product
  (`SourceXml`/`SourceIndex`/`SetSourceIndex`) and Receiver actions.
- `DlnaStreamServer` gained live-mode aliases (`SetAliasSession` /
  `RemoveAlias`) and a close-delimited response path; `DlnaStreamSession`
  gained `NewLiveReader` for late joiners.

### Changed

- `DlnaStreamShape` gained `FlacLive`; FLAC file and live shapes now share the
  encoder path (the live shape simply omits the fixed total).
- A leftover `stream_format=songcast` in an old ini is treated as `wav`.

### Fixed

- **Track boundaries no longer produce EOFs or resets at all.** The live
  stream is never sealed at a track end: LightningPlayer treats an EOF as a
  broken connection and resumes with `Range: bytes=<end>-`, which either wedges
  the stream or makes it give up. `EndOfStreamComplete` is now a bookkeeping
  event for the runtime (2 s grace, never waiting on the renderer to drain),
  and a live `Close` keeps the HTTP server and alias alive across the runtime's
  Close+Open, so a format change produces a new playlist item instead of a
  reset. Retired sessions are still aborted after 5 s, and switching back to a
  file shape cleans them up. Regression-tested end to end.
- **Live + `Range` wedged the stream (and blocked the next format change).**
  After a reconnect LightningPlayer sends `Range: bytes=<n>-`. A live body has
  no finite representation, so answering 200 from the current head made the
  device stop reading and hold the connection: the ring filled, the producer
  back-pressured, and the runtime's end-of-stream drain never completed, so a
  pending 192 kHz rebind was never applied (third track silent). Live now
  answers `416` with `Content-Range: bytes */<current extent>`, which makes the
  renderer re-GET from the start; every send also has a 15 s timeout so a
  non-reading peer cannot pin the producer. Verified: 416 -> re-GET -> `fLaC`
  from byte 0 -> full track.
- **The start of a live track was being eaten.** LightningPlayer probes by
  reading ~128 KB *at stream rate* (about 3.3 s at 320 kbps) and then reconnects;
  that probe's cursor advanced the ring's retained head, so the real GET started
  after the probe bytes. No reclaim now happens during a reader's first five
  seconds, so the real GET always begins at byte 0. Verified end to end with a
  simulated probe: every one of four consecutive tracks starts with `fLaC` and
  delivers the full entity.
- **Live track changes are queued promptly.** The SMTC metadata wait at a track
  boundary is capped at 300 ms for live (500 ms for the title-change poll)
  instead of 2.5-3 s: a live stream ends with a clean EOF, so the renderer's own
  buffer is the only cover while the next playlist item is being inserted.

### Removed

- **Songcast (OHU push) experimental mode.** It was implemented and tested, but
  the VEGA G2.2 firmware on hand advertises `Receiver:1` while `ProtocolInfo` is
  empty, `SetSender` hangs (both `ohu://` and `ohm://`), no Join/Listen is ever
  sent, and probing restarts the device's UPnP service (port change / HTTP 502).
  With no working receiver to validate against, the mode, its `OhuSender`
  implementation and the panel entry were removed rather than shipped broken.

### Verified

- Release build clean for hook / audiohost / UI / DLNA self test.
- Protocol-level self test against a local OpenHome renderer: `live` returns
  200 with `audio/x-flac`, no Content-Length, no chunked encoding,
  `contentFeatures` `OP=00`, start byte `fLaC` after a probe, and a clean EOF
  (`reason=eos`) after the track; `wav` and `flac-file` still pass unchanged.
- Four consecutive live tracks (4 s each, 5 s pull buffer, 1.0x realtime source)
  auto-advanced through Playlist.Insert with no fallback: four distinct session
  GUIDs, every track starting at `fLaC`, full byte count, clean EOF.
- **P0 fix - ring reclaim deadlock.** A renderer that stops reading used to
  leave `ReclaimLocked()` with no active reader, so `Append()` waited forever
  on a full ring: capture froze, `AdvanceToNextTrack()` was never reached and
  `Stop()`'s `JoinProducer()` hung. `Append()` now drops the oldest bytes for
  live sessions when no reader is active (broadcast semantics) and honours a
  cancellable flag. `Stop()` requests cancellation, tries a 1 s graceful join,
  and only then aborts and joins again. `ProducerMain` signals its exit through
  a condition variable; `ResetAppendCancel()` clears the flag on resume.
- **P1 fix - live-edge `Range`.** `416` with `bytes */<extent>` declared the
  resource finished and made the renderer give up. A range whose offset is at
  `produced` now attaches a reader that waits for new bytes; an offset inside
  the retained window is aligned down to a recorded FLAC frame start and
  answered `206` with `Content-Range: bytes <start>-/*` and no Content-Length.
  An offset behind the window gets `416` with `bytes */*`. Each connection
  registers exactly one reader and the range is parsed without exceptions.
- **P2 fix -** file-shape EOS parking sleeps 50 ms instead of spinning;
  `Close()` and `SetupStream()` prune expired retired sessions, so rings and
  alias tokens no longer accumulate after the producer exits.
- **Reproduction harness.** `tools/dlna_reconnect_renderer.py` mimics
  LightningPlayer: 128 KB probe, real GET, deliberate close at N bytes, then
  `Range: bytes=<received>-`. On the unfixed build a 45 s test hit the deadlock
  (`timeout` 124); after the fixes the same test PASSes with `206` at the
  aligned frame boundary and continues to EOF. Basic wav/live/multi-track
  regressions also pass with no BEX64.
- **AM playlist simulation.** `tools/run_am_playlist_sim.py` plus the
  `playlist` mode of `DlnaSelfTest.cpp` reproduce Apple Music's real behaviour:
  per-track decode format, 1.0x PCM, EOF -> Stop+Close -> re-Open of the SAME
  sink, and an OpenHome DeleteAll on graph teardown. A 44.1/16 -> 192/24 run
  PASSes: renderer STREAMINFO reads 44100/16 then 192000/24, both tracks get a
  frame-aligned `206` resume, one stable URL/alias across the format change,
  no 416, no deadlock, no BEX64.
- **P0 fix - format-change rebind (confirmed on real hardware).** A
  44.1/16 -> 192/24 playlist used to leave the second track silent: the
  runtime's `FinalizeEndOfStreamDrainLocked()` waited forever on
  `coordinator_.EndOfStreamComplete()` because a live session is never sealed
  and Apple tears down the graph without delivering a decoder `S_FALSE`.
  `graphBound_` stayed true, so `TryBindPendingGraphLocked()` never applied the
  192 kHz successor. The drain now has a 3 s deadline; a real two-track replay
  shows `finalize-run` -> `pending-apply` -> `bound a=192000`, a new
  `open device rate=192000 bits=24` and continuous capture to track end.
  Lightweight `v2 rebind` diagnostics are emitted at rebind decision points.
- **Still open:** the 416 branch (`offset < oldest`) has not been exercised by
  the harness; the "32 MiB renderer read-buffer ceiling" hypothesis is
  unproven (two observed counts differ by 1.9 MB); real-device 44.1 -> 192
  playback has not been retested.
- Rate matrix, verified by parsing STREAMINFO from the delivered stream:
  `44100/16`, `96000/24` and `192000/24` each deliver the requested rate and
  depth, start at `fLaC`, and end cleanly. A sequential 44.1 -> 96 change
  (separate sink opens, as the runtime does on a format change) works the same
  way at both rates.

## [0.4.5] - 2026-11-06

### Fixed

- **曲目边界后永久静音（Apple Music 1.7）**。1.7.0.156 / 包
  `1.1792.39942.0` 起，曲目自然结束时不再 `AudioConverterDispose` +
  `AudioConverterNew`，而是跨曲目复用同一个 P0 converter。旧实现在 EOF 排空
  后退役整个捕获世代并 `coordinator_.Disable()`，依赖下一次 converter 重建
  重新绑定；复用之后该前提不成立，状态机永久停在 Disabled：
  `capturedFrames` 继续增长、`submittedFrames` 停止，网播 renderer 反复请求
  过期 URL（HTTP 410）。现在 EOF 收尾时若该 converter 仍有效且仍是 P0 解码
  器，主动重新武装并开启新世代；`OpenEndpoint` 仍要求队列有真实 PCM 才打开
  输出，真停止时不会凭空开流。
- 本地文件 seek 判定不再使用不可靠的队列深度启发式（`[local] reservoir_ms`
  会让自然曲尾也命中），未匹配栈按自然尾部保守处理：不丢数据、不静音。

### Verified

- Apple Music 文件版本 `1.7.0.156` / 包 `1.1792.39942.0`；CoreMedia 质量锁
  指纹与 `1.1792.39682.0` 完全一致，无需新增 profile。

## [0.4.3] - 2026-11-05

### Fixed

- **DLNA 自动连播（同格式）从第二首起无声**。0.4.2 用 playerItem 变化驱动
  换曲，但 Apple Music 在连播同格式曲目时会复用整条解码管线，playerItem
  不会重建，换曲信号根本不触发（实测曲目边界无任何 converter 事件：PCM 持续
  流入、旧网播流已 complete、新流永远不建立，队列积压到手动切歌为止）。
  改写为在 DLNA 输出内部按曲目边界滚动会话：
  - **精确边界**：切换点用 SMTC 报告的真实曲目时长。此前用「时长 +3 秒」的
    声明长度作切换点，每首都会把下一首开头并入上一首，连播越久偏移越大
    （第 N 首开头已被吃掉 3×N 秒）。+3 秒只保留在 HTTP 声明里防止截尾。
  - **温和接续**：下一首以 OpenHome `Playlist.Insert(AfterId=当前曲目)` 排入
    播放列表，不 Stop、不 DeleteAll、不重发 Play。设备放完当前曲自然接续，
    不再丢弃已缓冲的尾部，也不必重新探测+预缓冲——此前表现为换曲后约
    5 秒才出声。
  - **兜底**：排队后 25 秒设备仍未来拉流，则强制 Play；纯 AVTransport 设备
    回退到完整 Stop+Insert+Play 流程。
  - 跨边界的 PCM 帧写入新会话，不丢不重；SMTC 时长不准只影响设备显示的
    曲目边界，字节流始终连续。中途断连也会在下一曲边界自动换会话重推。

### Changed

- 网播「拉流缓冲」新增 **1 秒** 档（1 / 3 / 5 / 8 秒）。设备冷启动仍建议
  不低于 3 秒；1 秒档主要缩短自动连播/首曲的起播等待。

## [0.4.2] - 2026-11-05

### Fixed

- **同格式连播第二首无声**：曲目切换现在由 Apple 的 playerItem 变化驱动，
  强制重开音频图与网络流（此前只在格式变化时重绑，Apple 复用解码管线时
  网播第二首必然无声）。本地播放同样每曲重新对齐缓冲。
- 首个正式版：v0.4.x 全部功能自本版脱离 preview。

## [0.4.1-preview] - 2026-11-05

### Removed

- **DLNA 断流自动恢复（BytesServed 停滞检测）整体移除**。
  大缓冲网播设备（LUMIN 等）会预读几十 MB 后暂停拉流、转为播放缓冲，
  停滞本身就是它们的正常节奏；服务端无法从连接层面区分"自己节奏拉流"
  与"真卡死"。8 秒检测在 LUMIN 上于 3–4 分钟（一个预读窗口）误触发
  restart，导致播放中断。真卡死时用户听不到声音，面板的
  「重开当前曲目」按钮保留作为手动恢复入口。

## [0.4.0-preview] - 2026-11-05

### Added

- **HiFi player mode** (opt-in, default off): a separate
  `am-exclusive-audiohost.exe` owns the endpoint and drains a cross-process
  shared-memory ring, so WASAPI exclusive / ASIO and all scheduling tuning run
  outside the Apple Music process. The hook only moves canonical Q31 PCM.
  - Panel: Output → Local → **Playback engine** (`AM player` / `HiFi player`),
    persisted as `[mod] player_mode` in `am-exclusive.ini`.
  - Transport: 16 MB SPSC ring (up to 2,000,000 frames, ~10.4 s at 192 kHz)
    plus a fixed-size named-pipe control channel. No UDP, no retransmission.
  - Automatic fallback: when the host is absent or refuses the request,
    `RemoteSink` falls back to the in-process WASAPI/ASIO sink.
  - The engine reads `[local]` itself, so timer resolution, dedicated core,
    priority boost and large pages are configured in the process that actually
    owns the audio threads.
- New build target `am-exclusive-audiohost.exe`: the installer publishes it and
  the broker starts it at boot (single instance, idle until the hook connects).

### Notes

- PCM only, up to the endpoint's format. DSD is not supported; Apple Music does
  not provide a DSD source here.
- The HiFi path has **not been validated on real hardware** yet. `AM player`
  mode is the 0.3.2 path unchanged.

## [0.3.2-preview] - 2026-11-05

### Fixed

- **"Save & apply" closed Apple Music.** `PipeThread` (a background IPC thread)
  wrote the output-status `TextBlock` inline when it received `RestartResult` or
  `DeviceAddResult`. XAML objects have thread affinity, so C++/WinRT raised
  `RPC_E_WRONG_THREAD`; the exception escaped and the process terminated through
  `std::terminate` → `abort` (`ucrtbase`, `0xc0000409`). Saving triggers
  `RestartCurrent` → `RestartResult`, which is why the app closed about a second
  later rather than immediately. Both paths now marshal through the window
  dispatcher and swallow WinRT failures.
  Verified against `AppleMusic.exe.22768.dmp`: stack `ucrtbase!abort` ←
  `ucrtbase!terminate` ← `am_exclusive_ui` (`throw_hresult`).

### Changed

- The local-tuning ini write no longer runs inside the output-save path. The
  three switches persist on their own `Toggled` / `SelectionChanged` events,
  with a suppression flag while the card is built and a catch-all around the
  writer. The Save button path is the 0.3.0 flow again.

### Note

- **v0.3.1-preview carries the crash above; use 0.3.2-preview instead.**

## [0.3.1-preview] - 2026-11-05

### Added

- **Local playback tuning for WASAPI / ASIO**, off by default (an empty or
  disabled `[local]` section reproduces 0.3.0 behavior exactly):
  - **Deep reservoir** (`reservoir_ms`, 0.1–30 s): the capture queue grows with
    the target and the software pump water marks chase it. The cutover gate
    deliberately stays at 0.1 s, so playback never deadlocks when Apple Music
    can only decode a little ahead.
  - **System timing**: `NtSetTimerResolution` (0.5 ms by default), a dedicated
    core for the WASAPI render thread, the software pump and the ASIO
    `bufferSwitch` callback (applied once on the driver-owned thread), and
    Windows dynamic priority boost disabled.
  - **Large pages**: the capture queue can use `MEM_LARGE_PAGES` after
    requesting `SeLockMemoryPrivilege`; any failure falls back to the ordinary
    allocator and `VirtualLock` unchanged.
- **Panel controls**: Output → Local → *Local playback tuning* — master switch,
  memory reservoir (0.1 / 0.5 / 2 / 5 / 10 s) and system timing. The save path
  writes `[local]` and restarts the current track, reusing the existing
  `SetOutputTarget` flow; no protocol change.
- New header-only `audio_v2/LocalPlaybackTuning.h` and a documented `[local]`
  section in `am-exclusive.ini`.

### Changed

- The local staging queue's 240-block headroom now follows the actual capture
  queue capacity, so a larger reservoir is no longer capped by the staging
  layer. With the default 256-block queue the limit is still exactly 240.

### Verified

- Release build clean: hook / ui / broker / setup plus both
  `EXCLUDE_FROM_ALL` self-tests (`am_exclusive_drain_probe`,
  `am_exclusive_dlna_selftest`).
- First real run (Realtek speakers, 44.1 kHz/24-bit exclusive): zero underruns,
  zero dropped frames, `bufferedFrames` ≈ 58.6k ≈ 1.33 s (was ~0.1 s).
  Apple Music appears able to decode about 1.3–1.4 s ahead, not a whole track.
- `largePages=0` on that machine (privilege or contiguous memory unavailable);
  the queue silently used the ordinary allocator.
- Panel layout has not been visually verified yet.

### Not included

- Kernel Streaming output (direct KS pins).
- System-wide Throttle / Hibernate (suspending other processes, switching power
  schemes, blocking input).
- A separate playback process or the dual-PC UDP boundary.

The DLNA / OpenHome path is untouched by every item above.

## [0.3.0-preview] - 2026-11-05

### Added

- **The output panel is split into two tabs**: *Local playback* (WASAPI or ASIO)
  and *Network playback* (OpenHome/DLNA). Exactly one output is active; saving
  persists it in `am-exclusive.ini` and applies it immediately by restarting
  the current track, so Apple Music never has to be closed. The tab and both
  local selections are restored on the next launch.
- **WASAPI endpoint list** and **ASIO driver list** are enumerated from the
  machine (all active `eRender` endpoints via `IMMDeviceEnumerator`, all
  drivers registered under `HKLM\SOFTWARE\ASIO`; 32-bit-only drivers are shown
  but disabled). A WASAPI selection of "follow Apple Music default device"
  keeps the previous behaviour.
- **Native ASIO output** (`AudioSinkKind::Asio`): `AsioExclusiveSink` loads the
  selected driver by CLSID, follows the track sample rate with
  `setSampleRate`, and fills the driver-owned double buffer from
  `bufferSwitch`. Derived from the MIT-licensed AMExclusiveASIO /
  AM-ASIO-MAD work; see `THIRD_PARTY_NOTICES.md`.

### Changed

- `SetOutputTarget` now carries a full output profile: kind 0/1/2, the WASAPI
  endpoint id or ASIO CLSID, and a display name. The broker persists
  `[mod] output`, `output_tab`, `wasapi_device`, `asio_driver` and their names,
  and re-broadcasts the whole profile when the ini changes.
- Verified with an in-process mock ASIO driver: Open / Start / 20 bufferSwitch
  callbacks / Stop / Close, 10,240 frames submitted, zero underruns, identical
  left/right samples. WASAPI/ASIO enumeration was verified on a real machine.

## [0.2.3] - 2026-11-05

### Fixed

- **File-shaped FLAC played as full-scale noise.** `FlacEncoder` was fed the
  canonical Q31 samples the WAV path packs (a 24-bit sample is `value >> 8`,
  i.e. left-justified in 32 bits) but wrote their low `bitsPerSample` bits, so
  every sample reached the renderer scrambled. The encoder now shifts each
  sample down to the stream depth itself, which makes the FLAC payload
  bit-identical to the WAV payload for the same PCM. Confirmed by decoding a
  captured entity: the decoded samples match the source exactly (a 12000-level
  440 Hz tone decodes as 12000, not 3072000), with no ffmpeg errors.
- **The final frame of a verbatim stream announced the wrong block size.** The
  zero-padding that keeps every frame the same size ran *after* the frame header
  had been written, so the last frame declared 256 frames while carrying 4096.
  Padding now happens before the header. A captured 40 s entity walks cleanly:
  469 frames, all `blockCode = 0xC`, all CRC-16 correct, ending exactly at the
  declared `Content-Length`.
- `Server:` response header still reported `0.2.1` in the 0.2.2 build.

## [0.2.2] - 2026-11-04

### Added

- **A second wire shape for the DLNA stream**, selectable from the AME panel
  and persisted as `[dlna] stream_format`. `wav` stays the default and is
  byte-for-byte unchanged. Only the two shapes that actually play on tested
  hardware are offered: WAV file and FLAC file.
  The chunked variants that were tried first (`flac`, `live`) were removed
  outright: a VEGA G2.2 reads one chunk, drops the connection, retries, and
  gives up, and offering them in the panel produced a silent "no sound" that
  looked like a fault in this app rather than a renderer limitation.
  - `flac-file` — file-shaped FLAC (the one offered in the panel).

  New `mod/src/audio_v2/FlacEncoder.{h,cpp}`: fixed block size 4096,
  independent channels, fixed predictors order 0–4, RICE2 residuals,
  CRC-8/CRC-16, 16/20/24-bit, plus a verbatim mode with constant frame size.
  Verified against ffmpeg: decoded output is bit-identical to the source at
  24/48k, 24/96k and 16/44.1k; compressed ratio 21–37% of raw PCM.
- DIDL `<res>` now also carries `sampleFrequency`, `bitsPerSample` and
  `nrAudioChannels`, which strict renderers cross-check against the container
  and the HTTP `Content-Type`.
- `DLNA.ORG_OP` now follows the shape: `01` only for the seekable file shape,
  `00` for FLAC and live, so a renderer is never promised byte ranges that the
  server will not honour.
- **`flac-file` — FLAC served as an ordinary file.** Verbatim subframes give
  every frame an identical size, so `FlacEncoder::VerbatimSize()` can compute
  the exact entity length before the first sample exists; the response then
  carries a real `Content-Length`, `Accept-Ranges: bytes` and `206` replies.
  No compression (bandwidth is within 1% of WAV), but it is the only way to
  satisfy a renderer that refuses an unbounded body while also refusing WAV.
  Verified on a VEGA G2.2: `state=Playing`, `filetype=FLAC`, `48000`, `24-bit`,
  position advancing. The same device rejects the chunked FLAC shape outright.
- DIDL `<res>` gained `sampleFrequency`, `bitsPerSample` and `nrAudioChannels`.
- HTTP responses now carry `Date` (required by RFC 7231 for 1.1) and a
  `Server` banner; `contentFeatures.dlna.org` gained `DLNA.ORG_PN=WAV`; the
  `transferMode.dlna.org` header uses the conventional lower-case spelling.
- The WAV MIME follows the renderer: when it advertises `audio/x-wav` or
  `audio/wave` but not `audio/wav`, that spelling is used instead.

### Fixed

- Chunked shapes now start every HTTP connection at the **oldest retained
  byte** instead of the newest. `NewReader` began at `produced_` because the
  file shape always reconnects with a byte range; a chunked stream has no
  ranges, so a renderer that probes and then opens a second connection read
  zero bytes and stopped. Measured: probe drained 60 KB, playback connection
  got an immediate end-of-stream.
- `FlushEncoderTail` emits the encoder's final partial block. Without it the
  last up-to-4096 frames of every FLAC track were dropped and the stream ended
  mid-frame.
- FLAC STREAMINFO declares `min_block = max_block = 4096`. Reporting a smaller
  minimum made renderers treat the stream as variable-block.
- FLAC advertises `audio/x-flac`, not the registered `audio/flac`. Measured on
  a VEGA G2.2: its ConnectionManager lists exactly `http-get:*:audio/x-flac:*`
  and drops a stream that announces the standard spelling.
- Chunked bodies go out in 8 KB pieces rather than 64 KB: a renderer that
  decodes while receiving stalls on a full 64 KB burst.

### Verified

Measured against a physical AURALiC VEGA G2.2 (`LightningPlayer`, OpenHome):

| shape | result |
|---|---|
| `wav` | plays, 24-bit/48 kHz, device pulls the whole entity |
| `flac` | **rejected** — device reads one chunk, drops the connection, retries, gives up |
| `live` | **rejected** — same behaviour |

The renderer itself does support FLAC: the same 10 s 24-bit FLAC served as a
**file** (Content-Length + Range) plays with `state=Playing`, `48000`, `24`.
So the limitation is that this device only accepts file-shaped bodies, not the
codec. FLAC and live remain available for renderers that accept chunked
transfer (Lumin and most software stacks); the VEGA G2.2 stays on `wav`.

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
