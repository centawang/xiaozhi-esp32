

<!-- pi-squad:7e7e921fe219d16ed8d38d20589a4a0806d78a1135e2b9e7f7de4991769a0d5b -->
# squad-fe3cbec4-026d-4e3a-8d3f-fd757e774993
completed; 2/2 successful jobs.
- stroke-speak-timeout-diagnosis: # Stroke Speak→Timeout diagnosis (read-only)

**Symptom:** WebSocket-priority 500-char CoreS3 build. SO click shows overlay **Speak**, speech yields no candidates, ~15s later overlay **Timeout**. USB app logs unavailable.

**What Timeout actually is.** Overlay `Speak` is `StrokeOrderView::RenderAwaitingSpeech()` at `StartVoiceSessionFromMain`, *before* hello/bind/listen. Overlay `Timeout` is *only* `StrokeAbortReason::SpeechTimeout` → `ShowSpeechTimedOutFromMain()`. Coordinator `kSpeechTimeoutMs = 15000` fires on 1 Hz `MAIN_EVENT_CLOCK_TICK` while `Phase::AwaitingSpeech`. That phase is entered only by `BindOpenedChannel` after a valid hello `session_id`. FailStroke/identity aborts show notifications (`Stroke voice unavailable` / `Stroke session identity error`) and close the overlay — **not** the Timeout page.

**Observed UI therefore already implies:** hello bind succeeded (valid WS session id); round stayed `AwaitingSpeech` for ~15s; **no** `CommitStrokeStt`; **no** FailStroke abort processed.

No files modified. No tests run.

---

## End-to-end (code)

1. SO → `RequestStartStrokeRound` → `BeginStrokeRoundFromMain` (Idle).
2. If `StrokeVoiceRoutingAvailable()` (`SupportsCorrelatedSessionOpen` && `SupportsStrokeVoiceRouting`) and `stroke_voice_transport_available_`: close any old channel, `DrainStreamingAudio()`, `BeginRound` (phase Starting, **speech timer starts now**), overlay Speak, `QueueListeningRequest(generation)`.
3. Else local 3-candidate fallback — **not this symptom**.
4. `HandleStartListeningRequest`: Idle + channel closed → Connecting, `MarkConnecting`, `Schedule(ContinueOpenAudioChannel(ManualStop, gen))`.
5. `ContinueOpenAudioChannel`: `ReserveAudioChannelOpenAttempt`, blocking `WebsocketProtocol::OpenAudioChannel` (≤10s hello), bind `identity->Get()`, `SetListeningMode(ManualStop)` → Listening.
6. `StartListeningAudio`: `CanStartListening` / `MarkListeningStarted` (**resets speech timer**), `SendStartListening(mode=manual)`, `EnableVoiceProcessing(true)`.
7. Uplink: AudioInputTask → AFE `HandleVoiceResult` (S3, `CONFIG_USE_AUDIO_PROCESSOR`) → encode → `MAIN_EVENT_SEND_AUDIO` → `Protocol::SendAudio` (not generation-gated).
8. Downlink JSON: WS `OnData` passes STT **as raw JSON** (hello identity is **not** stamped). `OnIncomingJson` `ProbeJsonString(session_id)` → `CaptureRoute(Stt)`. Match + AwaitingSpeech → `InterceptStrokeStt` → `CommitStrokeStt` → `FinishStrokeListening` (stop+close) → local candidates. Missing/mismatch id while round active → **FailStroke** (not Timeout). Matching non-STT (TTS/LLM/Audio) → **Drop**.
9. `FinishStrokeListening` is the only stroke path that `SendStopListening`.

WS hello identity is bound per open-attempt; MQTT still `SupportsStrokeVoiceRouting=false`. Stroke local builds prefer WS when both configs exist.

---

## Hypotheses (probability order)

### H1 — Manual listen deadlock (highest)
Stroke *always* uses `kListeningModeManualStop`. Device waits for `type=stt` before sending listen **stop**. Public auto-VAD servers typically emit final STT on stop (manual) or on VAD end (auto). Ordinary chat uses AutoStop/Realtime and works; stroke never stops → no STT → 15s Timeout.

**Predict:** Status bar LISTENING while overlay Speak; ~15s after listen-start (or ~15s after Speak if connect is fast); ordinary wake-word STT still works; WS shows `listen/start mode=manual` + Opus, **no** `type=stt`; no `listen/stop` until timeout close.

**Defect:** `ContinueOpenAudioChannel` / `HandleStartListeningRequest` hard-code ManualStop; VAD only drives LED; product wants “valid text then stop locally” which AutoStop already provides.

**Falsify:** WS capture of `type=stt` during Speak; or overlay candidates after speech in this build.

### H2 — Uplink never armed; timer from BeginRound (high if Timeout is ~15s from SO, not from mic)
`speech_started_ms_` starts at `BeginRound` (Speak). `MarkListeningStarted` is the only reset. If `StartListeningAudio` never succeeds, Speak lasts exactly ~15s with no mic.

**Predict:** Status bar stuck CONNECTING or returns STANDBY; no Opus uplink; Timeout ≈15s from SO; ordinary chat still listens.

**Defects:** Speak is shown before bind/listen (too coarse); `StartListeningAudio` skipped if `IsAudioProcessorRunning()` already true (no `SendStartListening`); `CanStartListening` failure → `AbandonCancelledStrokeListening` (NewNormalSession, **not** Timeout) — so H2 needs skip-without-abandon.

**Falsify:** Status bar LISTENING, or WS Opus during Speak.

### H3 — Server replies without interceptable STT (medium)
Matching-session TTS/LLM/Audio is **Drop**, not FailStroke. If server skips `type=stt` and only TTS/LLM, round stays AwaitingSpeech → Timeout. Incoming TTS audio also Dropped (silent).

**Predict:** LISTENING + Speak; WS `tts`/`llm` with hello session_id, **no** `stt`; no TTS heard; then Timeout. No “Stroke voice unavailable”.

**Defect:** Intercept is STT-only; no fallback from `tts.sentence_start` text; Drop is silent.

**Falsify:** No tts/llm during Speak, or a `type=stt` present.

### H4 — Empty/failed uplink despite Listening (medium-low if chat works)
Listening + manual still needs Opus. Failures: `EnableVoiceProcessing` no-op, 120ms warmup only (not 15s), encode/send drain on `SendAudio` false, AFE fetch discarded on control generation, CoreS3 mic mute.

**Predict:** LISTENING + Speak; WS listen start **without** Opus (or SendAudio errors); chat on same firmware also deaf ⇒ hardware; chat OK + stroke deaf ⇒ stroke-only drain/reset.

**Defect:** `DrainStreamingAudio()`=`ResetStreamingState()` disables VP and clears send queue at round start; if VP not re-enabled, silent uplink. Send path ignores stroke generation (OK if VP on).

**Falsify:** Opus frames while Speak, or chat equally deaf.

### H5 — STT arrives but identity routing drops it (low **for Timeout UI**)
WS does **not** stamp hello identity onto JSON. `CaptureRoute` requires STT `session_id` == bound hello id. Missing/invalid → FailStroke → overlay abort + notification, **not** Timeout. Mismatch → same. Only Drop+stay AwaitingSpeech if id matches but kind/phase wrong (covered by H3).

**Predict if this were the bug:** notification, overlay closes, **not** Timeout. **Already weak vs reported UI.**

**Defect (real, different symptom):** `websocket_protocol.cc` stamps `session_id` on binary audio only; JSON relies on server field. Docs: hello session_id is optional; STT examples include it.

**Falsify:** Timeout page with no identity notification (reported) falsifies H5 as the cause of *this* screen.

---

## Minimal on-screen stage (no USB)

Replace static `Speak` with a 1-line probe updated on the existing 1 Hz clock while `AwaitingSpeech` (do **not** block main/audio; LVGL lock only):

`Speak L# A#### J#/#/# T##`

| Token | Meaning |
|---|---|
| L0/L1 | `MarkListeningStarted` done |
| A | Opus packets actually `SendAudio`'d this round (saturate 9999) |
| J | stt seen / Drop / FailStroke |
| T | seconds since `speech_started_ms_` |

Keep `Timeout` as today; optionally prefix `TO L# A# J#` so the last probe survives abort.

**How to read the next flash**

- `L0 A0 T15` → H2
- `L1 A0 T15` → H4
- `L1 A>0 J0/0/0 T15` → H1 (manual, no server JSON)
- `L1 A>0 J0/>0/0 T15` → H3 (TTS/LLM dropped)
- `J#/#/>0` then notification not Timeout → H5

Status bar CONNECTING vs LISTENING is a free discriminator for H2 vs H1/H3/H4.

## Host seam (no device log)

Add a copyable `StrokeVoiceDebugSnapshot` {generation, phase, listen_started, listen_mode, send_frames, stt_seen, drop_count, fail_count, elapsed_ms} filled from Application + coordinator (test doubles already in `stroke_round_integration_harness.cc`).

Harness cases:
1. Manual listen + Opus, no stop → no CommitStrokeStt, Speech timeout at 15s (H1).
2. Same + AutoStop fake STT → Intercept (H1 fix).
3. Bind + no `StartListeningAudio` → timeout with listen_started=false (H2).
4. Matching `tts` without `stt` → Drop, timeout (H3).
5. STT JSON without session_id → FailStroke, not timeout (H5).

Do not treat fixture STT as a release lexicon. No CoreS3 build/flash in this read-only pass.
- stroke-speak-timeout-test-plan: # SO Speak 15 秒 Timeout：紧凑定位与测试计划

> 只读结论：本次未修改文件，也未运行测试、固件构建或真机命令。`116 tests` 是对当前 `scripts/tests/*.py` 中 `def test_` 的静态盘点，不是本次执行结果。

## 1. 先给结论

当前的 `Speak -> 约 15 秒 -> Timeout` 只能证明：本轮最终进入了 `StrokeRoundCoordinator::Phase::AwaitingSpeech`，并且 15 秒内没有成功提交匹配当前 generation/session 的 STT；它不能证明已经调用或成功发送 `StartListening`，也不能证明麦克风音频已经上行。

更强的一点是：生产代码中 Speech timeout 只在 `AwaitingSpeech` 触发，而该阶段由有效的 `BindOpenedChannel()` 建立。因此，**单纯的 WebSocket/channel open 返回失败通常不会产生这个 15 秒 Timeout**：连接失败或 10 秒 server-hello 超时会走 `StartFailed`/network error 并提前退出。反复稳定出现完整 15 秒 Timeout 时，排查优先级应移到：

1. `StartListeningAudio()` 是否真的执行；
2. `listen/start` 是否真正发送成功；
3. 是否产生并成功发送 Opus 上行包；
4. 服务端是否产生 STT；
5. STT 的 `session_id` 是否匹配、是否被当成 retired/unknown/missing 而拒绝。

但屏幕上的 `Speak` 本身不能作为 open 成功证据：`StartVoiceSessionFromMain()` 在排队打开通道之前就渲染 `Speak`。

## 2. 当前真实路径与可观察性

实际调用链如下：

```text
SO entry click
 -> BeginStrokeRoundFromMain
 -> BeginRound
 -> StartVoiceSessionFromMain -> 屏幕显示 Speak
 -> QueueListeningRequest
 -> Idle -> Connecting
 -> WebsocketProtocol::OpenAudioChannel
 -> server hello + exact session_id
 -> BindOpenedAudioChannel / BindOpenedChannel -> AwaitingSpeech
 -> Listening state
 -> StartListeningAudio
 -> MarkListeningStarted
 -> Protocol::SendStartListening
 -> AudioService::EnableVoiceProcessing(true)
 -> mic -> audio engine -> Opus -> send queue
 -> Application MAIN_EVENT_SEND_AUDIO -> WebsocketProtocol::SendAudio
 -> server STT JSON
 -> CaptureRoute -> CommitStrokeStt -> candidates
```

当前诊断盲点：

| 关口 | 当前已有证据 | 当前缺口 |
|---|---|---|
| 选到 WebSocket | 启动日志有 `selected=websocket`、`correlated_open=1 stroke_voice=1` | `WebsocketProtocol::Start()` 只返回 true，不建立连接；这些日志不证明 WS 可用 |
| channel open | WS 日志有 `Connecting...`、成功 hello 后的 `Session ID`；hello 最长等待 10 秒 | SO 页面仍只显示 `Speak`；`StartFailed` 本身没有专属状态 |
| StartListening | fake harness 会统计一次 `start_listen` | 生产代码无日志；`Protocol::SendStartListening()` 返回 `void`，内部 `SendText()` 的 bool 被丢弃 |
| 音频产生/上行 | `AudioService` 内部有 input/encode 统计字段 | 无公开快照、无 SO 每轮计数；`Application` 对 `SendAudio(false)` 只清空余包，没有定位日志 |
| STT 收到/路由 | missing/wrong 当前会提前 abort 并提示；coordinator 已有严格路由 | 被 SO 拦截的 STT 没有普通 STT 的 `>> text` 日志；retired session 是静默 Drop，仍可能最后 Timeout |
| 15 秒超时 | coordinator 精确检查 15000 ms；屏幕显示 `Timeout` | `AwaitingSpeech` 同时覆盖“已 bind 未 Start”和“已 Start 等 STT”，无法归因 |

另一个关键点：`MarkListeningStarted()` 只重置 speech deadline，并不切换到独立 phase。因此，现有 coordinator 状态本身无法区分“通道已 bind，但 `StartListeningAudio()` 从未执行”。

## 3. 现有 116 项测试盘点

静态计数为：

| 文件 | 测试方法数 | 主要范围 |
|---|---:|---|
| `test_build.py` | 65 | 构建脚本、板卡/Kconfig/CMake/变体 |
| `test_build_default_assets.py` | 2 | 默认字体资产元数据 |
| `test_protocol_selection.py` | 3 | MQTT/WS 选择矩阵和 capability source-shape |
| `test_stroke_order.py` | 24 | SOB1、转换、边界、C++ store harness |
| `test_stroke_order_pinyin.py` | 13 | 500 字、SPY1、来源与 C++ harness |
| `test_stroke_order_ui.py` | 9 | controller/lifecycle/session/round/audio-generation harness |
| **合计** | **116** |  |

与本问题直接相关的最好证据是 `stroke_round_integration_harness.cc`，它已经覆盖：

- fake WebSocket 成功 open 后 `start_listen == 1`、fake microphone enabled；
- Speech timeout 在 15000 ms 不提前并按统一 abort 清理；
- missing/wrong session ID 对 STT/TTS/LLM/audio 为 fail-closed；
- retired STT 被 Drop；
- matching STT 只提交一次；
- MQTT 本地候选降级及普通会话旁路；
- open/cancel/close 的 generation fence 竞态。

但它没有覆盖本次定位所需的关键差异：

- fake `Open()` 总是成功，没有“普通 WS 失败”和“SO fresh-channel open 失败”的对照；
- 没有“open/bind 成功，但刻意不调用 `StartListeningAudio()`”的分支；
- fake microphone enabled 被当成音频链路成功，没有模拟 encode/send packet 数；
- 没有 `SendStartListening()` 的真实返回结果；
- 没有执行生产 `WebsocketProtocol`、生产 `AudioService`、CoreS3 ES7210/I2S 或真实服务端；
- 没有验证诊断文字实际出现在 LVGL 屏幕上。

所以 116 项即使全部通过，也不能区分本次五类故障。

## 4. 最小反馈信号：一行、每 generation 固定有界

建议不扩展 `DeviceStateMachine`，只为当前 SO generation 保存一个固定大小诊断快照：

```text
open_ok / hello_id_bound
listen_called / listen_send_ok
encoded_packets
send_attempts / send_ok / send_fail
stt_seen / stt_route_reason
```

只记录计数、布尔值、generation/open-attempt ID 和单调时间；不记录语音内容、token、URL 或完整 session ID。若需和服务端关联，只输出 session ID 的短哈希，不在屏幕显示原值。

### 屏幕最小状态

保留标题 `Speak`，下面只增加一行 ASCII 状态，避免字体依赖：

```text
O1 I1 L1 A12 S0
```

- `O`：channel open 成功；
- `I`：hello session ID 已验证并绑定；
- `L`：`listen/start` 已实际发送成功；
- `A`：本 generation 成功交给 WebSocket 的二进制包数，显示 `99+` 封顶；
- `S`：STT 路由结果，`0` 未见、`M` match、`X` wrong/missing/invalid、`R` retired/stale。

只在状态首次变化时刷新，不能每个音频包重绘。网络/音频线程只更新原子或固定快照；LVGL 更新仍经主任务/显示锁。

### 终态文案

| 条件 | 最小终态 | 含义 |
|---|---|---|
| 普通 fresh WS 对照也失败 | `Normal WS failed` | 共享网络、凭据、服务端或普通音频链路问题，先停止归因 SO |
| 普通对照成功，SO `O0` | `SO open failed` | SO 新通道未打开/未收到 hello |
| `O1 I0` | `SO hello ID invalid` | socket/hello 成功，但用于绑定的 identity 缺失或非法 |
| `O1 I1 L0` | `Listen not started` | 状态事件或 `StartListeningAudio()` 未到达 |
| `L` 已调用但发送失败 | `Listen send failed` | 不能误报为“已经监听”；当前 void API 看不到此事实 |
| `L1 A0` | `No audio uplink` | listen 已发送，但本轮没有成功的二进制上行 |
| `A>0` 且 `S=X/R` | `STT ID rejected` | 已收到 STT，但 identity 缺失、错误或 retired；显示子类型 |
| `A>0 S0` 到 15 秒 | `STT no response` | 已上行音频但设备没见到 STT；这是服务端/ASR/协议响应问题，不能伪称 session-id 被拒绝 |
| `S=M` | 立即进入候选页 | 正向控制；不得再触发该轮 speech timeout |

`SendStartListening()` 当前返回 `void`。最低限度可以只标记“调用过”，但要区分“未调用”和“调用了但发送失败”，更可靠的最小改法是让它返回底层 `SendText()` 的 bool，并同时验证 WebSocket 与 MQTT 的普通路径。

音频也至少分两级：`encoded_packets` 与 `send_ok`。这样 `encoded=0` 指向麦克风/AFE/Opus，`encoded>0 && send_ok=0` 指向 WS send；仅用“mic enabled”不能证明上行。

## 5. 最小测试矩阵

### 5.1 Host 可自动化

建议在现有 `stroke_round_integration_harness.cc` 中增加一个表驱动 failure matrix，并由现有 Python wrapper 或一个新增的单一 unittest 方法执行：

| Case | 注入 | 必须断言 |
|---|---|---|
| H0 正向 | normal WS 成功；SO open + valid ID + listen send + 1 个 audio send + matching STT | 候选态；无 timeout |
| H1 普通 WS 失败 | 普通 fresh open 返回 false | 分类为 `Normal WS failed`，不归因 SO |
| H2 SO open 失败 | 普通对照成功；SO open false 或 `IsAudioChannelOpened=false` | `SO open failed`；start/audio 均为 0；coordinator 不留活动轮次 |
| H3 未 StartListening | open/bind 成功，跳过 `StartListeningAudio()`，推进单调时钟 | `O1 I1 L0 A0`；终态 `Listen not started` |
| H4 无音频上行 | start send 成功、capture 标记开启，但不产生包；再测 encode 有包而 send 全失败 | 分别定位 capture/encode 与 transport send，终态均属于 `No audio uplink` |
| H5 STT ID 拒绝 | 注入 missing、非法、valid-wrong、retired 和 exact-match ID | missing/wrong 立即 fail；retired 记录 `S=R` 且不误提交；match 只提交一次 |
| H6 服务端无 STT | `send_ok>0`，完全不注入 STT，推进到 15 秒 | `STT no response`，不能分类成 ID reject |

同时增加/抽取一个纯 host JSON 测试：open 返回 `sid-B` 后，`listen/start` JSON 必须携带完全相同的 `sid-B`，且发送失败可被上层观察。当前测试只验证选择 helper 和 source 文本，没有验证这条数据契约。

这些测试可在 host 证明分类器、时序、generation 隔离和路由决策；不能证明真实 CoreS3 有音频包。

### 5.2 必须真机或“CoreS3 + 可控服务端”验证

以下事实 host fake 无法替代：

- OTA 下发的真实配置、TLS/鉴权、DNS 和 `WebSocket::Connect()`；
- server hello 是否携带有效且每轮独立的 session ID；
- CoreS3 ES7210/I2S 是否采到 PCM，AFE 是否输出，Opus 是否编码；
- 二进制帧是否真的离开设备并被服务端收到；本地 `Send()` 返回 true 只证明本地栈接受；
- 服务端是否收到 `listen/start`、是否启动 ASR、返回 STT 的 ID 是否和 hello 相同；
- LVGL 诊断行、触摸、状态切换与 15 秒终态是否正确。

可控服务端最小场景：

1. 不回 hello：应在约 10 秒得到 `SO open failed`，不能到 Speech Timeout；
2. 回有效 hello，记录是否收到 `listen/start` 与二进制包，不回 STT：应区分 `L0`、`A0`、`A>0 S0`；
3. 回 matching STT：必须进入候选页；
4. 回 missing/wrong/retired session ID：必须显示对应 `STT ID rejected` 子类型，且不得把错误 STT用于当前候选。

## 6. 现在不改代码也能执行的最短真机循环

1. **冷启动先看启动日志**：必须是 `selected=websocket`、`correlated_open=1`、`stroke_voice=1`。这些只证明选择和 capability，不证明连接。
2. **先做普通 WS fresh-channel 对照**：不要先进入 SO；普通聊天说同一个短词（建议“一”），要求屏幕出现用户文本且串口出现普通 STT 的 `>>`。若失败，先处理普通 WS/音频/服务端。
3. **回 Idle 后立即做 SO**：同距离、同音量说“一”；同时录屏、保存串口和服务端同一时间窗日志。
4. **按证据停在最早失败关口**：
   - 无 `Connecting to websocket server`/server hello：open；
   - 有 hello 但服务端无 `listen/start`：当前代码无法仅靠设备日志区分“未调用”和“发送失败”；
   - 服务端有 start、无 binary：音频上行；
   - 有 binary、服务端发了 STT：比较 hello/listen/STT 三者 session ID；
   - 有 binary、服务端没发 STT：服务端/ASR 无响应，不是已证实的 device session reject。

当前无改动时，屏幕只能提供：`Speak`、`Timeout`、missing identity 的 `Stroke voice unavailable`、unknown valid identity 的 `Stroke session identity error`。其中 retired ID 是静默 Drop。因此，要彻底区分五类，至少需要上述一行状态或服务端日志。

## 7. 验收标准

- 冷启动普通 WS 对照和 SO 使用同一服务端、同一短语，并强制各自 fresh channel；
- 每个 SO generation 只产生一份有界诊断摘要，旧 generation 事件不得修改新页面；
- open 失败不得等到 15 秒后才显示通用 Timeout；
- `L1` 必须表示 start frame 发送成功，而不只是函数被调用；
- `A>0` 必须统计成功的 binary send，不能只统计 mic enabled 或 queue 入队；
- 每个收到的 STT 都记录 match/missing/wrong/retired 路由结果；原始 session ID 不上屏、不进公开日志；
- matching STT 后取消 speech timeout并进入候选；
- host failure matrix 通过、现有 116 项回归通过、CoreS3 IDF 6.0.2 构建通过；
- 真机至少完成普通 WS 正向、SO matching 正向、no-hello、wrong-ID 四个场景，并保留屏幕、串口和服务端三方时间线。

## 8. 本次验证说明

本次使用只读文件枚举、文本检索和源码阅读；没有 shell/test/build/flash 权限，因此没有执行 `python3 -m unittest discover -s scripts/tests -v`，也没有烧录 CoreS3。116 是静态方法数；所有动态结果仍需后续执行者实际验证。
