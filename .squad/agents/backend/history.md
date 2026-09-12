# Backend: project knowledge

做一个新的功能，叫“笔划”。用户说一个汉字的单字，找几个备选，用户点击屏幕选择。用动画演示正确的笔画顺序。

<!-- pi-squad:b065092a971c6e49b48744ed276e4540699da0edb375937cc993be8f888cc6e0 -->
## 2026-09-12T04:21:17.656Z — repo-runtime

# Runtime: voice, protocol, state, and stroke-feature extension points

Read-only analysis of `/Users/mandyw/git/xiaozhi-esp32`. No files modified; no tests run. Scope: `main/audio`, `main/protocols`, `main/application.*`, `main/device_state_machine.*`, `main/mcp_server.*`, display glyph path, and docs (`websocket.md`, `mqtt-udp.md`, `mcp-protocol.md`, `glyph-push.md`, `main/audio/README.md`).

## 1. Voice input → device behavior

End-to-end uplink:

```
Mic → AudioCodec → AudioInputTask (prio 8)
  → one AudioEngine (Afe on S3/P4/S31, Lite otherwise)
    → wake-word event  OR  16 kHz mono PCM
  → audio_encode_queue_ (max 2; drop oldest)
  → OpusCodecTask (prio 2, 60 ms Opus)
  → audio_send_queue_ (max ~40 / 2.4 s; drop oldest)
  → MAIN_EVENT_SEND_AUDIO on Application main loop (prio 10)
  → Protocol::SendAudio
```

Wake / listen / speak control is not in the audio tasks. AudioService only raises bits (`MAIN_EVENT_WAKE_WORD_DETECTED`, `MAIN_EVENT_SEND_AUDIO`, `MAIN_EVENT_VAD_CHANGE`, `MAIN_EVENT_PLAYBACK_DRAINED`). Application::Run() owns session policy.

Typical session:
1. Idle: voice processing off, wake-word on.
2. Wake or button → Connecting → `OpenAudioChannel()` (hello handshake, may block ~10 s) → Listening.
3. Listening: `SendStartListening(mode)` then `EnableVoiceProcessing(true)`. Auto mode waits for playback drain (`pending_listening_start_`) so TTS is not truncated.
4. Server `type:stt` → copy text (+ optional `glyph_push`) → `Schedule` → `display->SetChatMessage("user", ...)`.
5. Server `type:tts state=start` → Speaking; downlink Opus accepted only in this state. `state=sentence_start` shows assistant text. `state=stop` → Listening (auto/realtime) or Idle (manual).
6. Abort / channel close → Idle; Idle **clears chat messages**.

Listening modes (`Protocol`): `auto`, `manual`, `realtime` (needs AEC). Default is AutoStop if AEC off, else Realtime. Device vs server AEC are mutually exclusive at Kconfig.

Downlink:
```
WS binary or MQTT UDP → OnIncomingAudio
  → only if GetDeviceState()==Speaking
  → decode queue (max ~40; non-blocking drop from net callback)
  → OpusCodecTask → playback queue → AudioOutputTask (prio 4) → speaker
```
Local OGG (`PlaySound`) also uses the decode queue (blocking push). EnableVoiceProcessing resets the decoder, which is why popup sound is deferred until after listening start.

## 2. WebSocket vs MQTT/UDP abstraction

`Protocol` is transport-neutral. Shared JSON verbs live in `protocol.cc`: listen start/stop/detect, abort, MCP wrap, hello `features` + `text_font` via `AddTextFontCapabilities`. Both transports must stay in this layer; do not add session semantics only in `WebsocketProtocol`.

Transport choice (`Application::InitializeProtocol`): OTA MQTT config → `MqttProtocol`; else WebSocket config → `WebsocketProtocol`; else MQTT. MQTT `Start()` connects the broker immediately. WebSocket `Start()` is a no-op until `OpenAudioChannel()`.

| | WebSocket | MQTT + UDP |
|---|---|---|
| Control | WS text JSON | MQTT publish/subscribe |
| Audio | WS binary Opus | UDP AES-CTR Opus |
| Hello | `transport: websocket`, version from NVS | `transport: udp`, version 3 |
| Session open | Connect + hello, wait 10 s | MQTT hello → UDP endpoint/key/nonce, wait 10 s |
| Close | drop socket (no goodbye) | optional `goodbye`; server goodbye must not ping-pong |
| Audio framing | v1 raw; v2 `BinaryProtocol2`+timestamp; v3 `BinaryProtocol3` | 16 B header + AES-CTR; seq anti-replay |
| Timeout | 120 s since last incoming | same `Protocol::IsTimeout` |

Incoming JSON: parse on the **network callback**, dispatch `hello`/`goodbye` in the transport, everything else to `Application` `OnIncomingJson`. Types handled today: `tts`, `stt`, `llm`, `mcp`, `system` (`reboot`), `alert`, optional `custom` (`CONFIG_RECEIVE_CUSTOM_MESSAGE`). Unknown types are logged and dropped. `cJSON` is deleted after the callback; **never capture `cJSON*` across `Schedule()`**.

MCP is JSON-RPC 2.0 inside `{type:mcp, payload:...}`. Device is the MCP server. `tools/call` is `Schedule`d onto the main task. Tool properties are only bool/int/string. `tools/list` paginates at ~8000 bytes. User-only tools need `withUserTools=true`.

`glyph_push` is **missing CJK bitmap glyphs** for LVGL text, attached to `stt` / `tts.sentence_start`. It is not stroke-order data. Limits: v=1, bundle/size/bpp must match hello, ≤64 glyphs, ≤64 KiB decoded bitmaps. Rejected payloads still show the text.

## 3. State machine and thread scheduling

States: Unknown, Starting, WifiConfiguring, Idle, Connecting, Listening, Speaking, Upgrading, Activating, AudioTesting, FatalError. Legal edges are strict (`device_state_machine.cc`). Same-state is a no-op. Invalid transitions log and fail. **There is no UI/selection state.** Idle→Speaking is legal (manual); Listening→Idle and Speaking→Idle/Listening are the chat exits. FatalError is terminal.

Listeners run in the `TransitionTo()` caller. Application only sets `MAIN_EVENT_STATE_CHANGED`; actual side effects run later in `HandleStateChangedEvent` on the main loop (display, wake/voice enable, deferred listen). Do not do heavy work in a state listener.

Threading rules (must keep):
- **Main loop must not block** and must not starve audio. `OpenAudioChannel()` already blocks up to 10 s on a scheduled main-task continuation — do not add more blocking (HTTP, animation busy-wait, LVGL snapshot) there.
- Cross-thread UI/protocol/state mutations: `Application::Schedule()` or event bits. Buttons already use `ToggleChatState` / `StartListening` / `StopListening` bits.
- Network callbacks (WS `OnData`, MQTT `OnMessage`, UDP audio) are not the main task. Copy, then Schedule. MCP `ParseMessage` runs on the net thread until `DoToolCall` schedules.
- Display: `DisplayLockGuard` (LVGL lock, 30 s timeout). LVGL input callbacks are not the main task; send selection back via `Schedule`.
- Audio: input prio 8, output 4, opus 2, main 10. Encode/send queues drop oldest instead of blocking the engine. Failed `SendAudio` **drains the rest of the send queue** to avoid codec deadlock.
- Incoming TTS audio is dropped unless Speaking. Do not expect stroke-narration audio to play in Idle/Listening.
- `PlaySound` / decoder reset race: ResetDecoder in EnableVoiceProcessing and Speaking entry.

## 4. Network data handling

- Validate before mutating live objects (glyph_push is the template: version, bounds, base64 length, total size).
- WS v2/v3 currently copies `payload_size` bytes **without checking against frame `len`** — untrusted length risk.
- WS `cJSON_ParseWithLength` is used without a null check before `cJSON_GetObjectItem`.
- MQTT UDP: type 0x01, length match, seq ≤ last dropped, AES-CTR under `crypto_mutex_`; send path holds `channel_mutex_`.
- `SendMcpMessage` concatenates payload as raw JSON inside a larger object; payload must already be JSON.
- MCP replies and broadcasts are always scheduled on main.
- Idle on channel close clears the chat UI; any in-progress picker would vanish unless it is a separate overlay or the session is kept open.

## 5. “笔划” extension points (single-char candidates + stroke playback)

Product intent (from member memory): user speaks one Hanzi → several candidates → tap to confirm → animate correct stroke order.

**Not present today.** STT is display-only. No candidate widget, no stroke protocol, no Display touch API. `glyph_push` only fills missing font bitmaps. `custom` messages dump JSON as a system bubble. Touch exists only on some boards (LVGL indev / board timers), not on `Display`.

Recommended split:

1. **Server (ASR + lexicon)** — homophones and stroke data do not belong in the audio engine. After a single-character STT, send candidates + stroke commands. Advertise a hello `features` flag (same pattern as `glyph_push` / `mcp`) so no-LCD and old firmware stay silent.

2. **Shared JSON in `Application::OnIncomingJson`** (preferred over transport-specific code and over MCP for the picker). Parse and bound on the net thread, copy into `Schedule`. Do **not** use `CONFIG_RECEIVE_CUSTOM_MESSAGE` as the product path. MCP is a weaker fit: no array properties (would be a JSON string), `tools/call` is one-shot on main, selection is async. If MCP is used at all, show UI immediately, return, then `SendMcpMessage` notification on tap. Board `InitializeTools()` is for board I/O, not this core UX.

3. **UI on `LcdDisplay` overlay**, not `SetChatMessage`. Chat bubbles are not hit-targets; Idle `ClearChatMessages()` would wipe them; OLED/emote/NoDisplay cannot host a picker. Gate on LCD + touch. Preview-image overlay is the closest existing pattern (`SetPreviewImage`). Keep animation on an LVGL timer, not the main loop or audio tasks. Stroke payloads need the same class of limits as glyph_push (count, decoded bytes, version). Compact stroke commands beat SVG/GIF on MQTT and RAM.

4. **Selection uplink** — new device→server JSON on the existing listen/MCP session (channel must stay open). LVGL click → `Schedule` → `Protocol::SendText` / `SendMcpMessage`. Do not send from the LVGL thread. Do not invent a new `DeviceState` unless Idle-clearing or listen/TTS transitions fight the overlay; a dedicated state requires updating every legal edge. Prefer overlay while Listening/Speaking, pause mic if taps must not become new STT (`EnableVoiceProcessing(false)` without closing the channel).

5. **Playback** — visual first. If TTS names the character, it still requires Speaking and the existing audio queues. Do not stall OpusCodecTask with canvas work.

## 6. Key paths and risks

Critical paths: `AudioService` queues → `Application::Run` send/wake/state → `Protocol` hello/listen/audio → `OnIncomingJson` STT/TTS/MCP → `Schedule` display/state.

Risks for 笔划:
- Main-task blocking (hello already 10 s).
- Capturing `cJSON*` after delete.
- Idle clearing UI; audio channel timeout 120 s while user thinks.
- No-touch / OLED / no-display boards.
- Stroke payload size vs MQTT and internal RAM (non-PSRAM glyph cache is per-message only).
- LVGL animation vs audio CPU on C3/C6.
- Putting logic in WebsocketProtocol only (MQTT users break).
- Reusing glyph_push for strokes (wrong data model).
- Auto-listen turning a tap-era utterance into a new STT.
- WS binary `payload_size` not bounds-checked (pre-existing).
- MCP 8 KB list / string-only args.

Validation still needed (not done): physical LCD+touch board, both WS and MQTT, and a no-display build to prove feature gating.

- Uplink is Mic→AudioInputTask→one AudioEngine→OpusCodecTask→send queue→Application MAIN_EVENT_SEND_AUDIO→Protocol::SendAudio; downlink Opus is accepted only in kDeviceStateSpeaking.
- Protocol is the shared JSON/MCP/hello-capability layer; WebsocketProtocol carries JSON+Opus on one socket, MqttProtocol splits MQTT control and AES-CTR UDP audio. Session JSON must not be added to only one transport.
- Application::Run is the only legal place for session/UI/protocol mutation; network and LVGL callbacks must copy data and Schedule() or set event bits. OpenAudioChannel already blocks the scheduled main task up to 10s for hello.
- DeviceStateMachine has no UI/selection state; Idle clears chat via ClearChatMessages. glyph_push is bitmap-font fallback on stt/tts.sentence_start, not stroke-order data.
- McpServer tools are bool/int/string only, tools/call runs on the main task, and tools/list paginates near 8000 bytes; board InitializeTools is for board I/O, not core UX.

<!-- pi-squad:8a4c8024961cb9e5aca70220c7c75b63c930f9f6afc208340d0307049bbd984d -->
## 2026-09-12T05:00:39.791Z — existing-service-capabilities

# 现有远端服务对「笔划」的能力评估

**范围：** 只读分析 `/Users/mandyw/git/xiaozhi-esp32`。未改文件、未跑测试、未探测线上 xiaozhi.me。问题：在不自行实现云服务的前提下，现有远端能否支撑候选字生成、用户选择回传、笔顺数据获取。

**总判断：不能按已接受产品/ADR 交付。** 现有公共服务没有 `stroke_order` / `type=stroke` / 笔顺字库；设备也没有可复用的候选/笔顺通道。最多只能用现有 ASR+LLM+TTS 做口头讲解，或日后由合作服务端扩展协议。

---

## 1. 现有远端拓扑（仓库可连接）

| 通道 | 来源 | 现有用途 |
|---|---|---|
| 默认 OTA HTTP | `CONFIG_OTA_URL` = `https://api.tenclass.net/xiaozhi/ota/`，可被 NVS `wifi.ota_url` 覆盖 | POST 设备信息，换激活码、server_time、firmware.url、**mqtt/websocket 接入点** |
| 官方会话云 | README：固件默认接 [xiaozhi.me](https://xiaozhi.me)；控制台配模型/智能体 | 流式 ASR + LLM + TTS；云侧 MCP（家居/知识/邮件等，在服务端而非本仓库） |
| WebSocket | OTA `websocket` 段写入 NVS：`url`/`token`/`version` | JSON 控制 + 二进制 Opus |
| MQTT+UDP | OTA `mqtt` 段：endpoint/凭证/topic | MQTT JSON 控制 + UDP AES-CTR Opus |
| 开源兼容服务 | README 列出 xinnan-tech/xiaozhi-esp32-server 等 | **不在本仓库**；接上仍是现有 hello/stt/tts/mcp，不会 magically 出现笔划 |

传输选择（`Application::InitializeProtocol`）：OTA 有 mqtt → MQTT；否则有 websocket → WS；否则 MQTT。两端共享 `Protocol` JSON，hello 由各传输组包。

设备 hello `features` 现仅：`mcp`（恒 true）、可选 `aec`、`glyph_push`（由 assets 字体元数据决定）。**无 `stroke_order`。** 服务端 hello 只解析 `transport`/`session_id`/`audio_params`，不回读或确认能力位。

---

## 2. 会话 JSON：候选 / 选择 / 笔顺

`Application::OnIncomingJson` 已知 `type`：

| type | 方向 | 现有语义 | 对笔划 |
|---|---|---|---|
| `stt` | 下行 | 单字段 `text` + 可选 `glyph_push` → 聊天气泡 | **无 n-best、无候选数组、无意图** |
| `tts` | 下行 | start/stop/sentence_start；start 才进入 Speaking 并收下行音频 | 可复用「朗读一次汉字」，但必须 Speaking；Idle/Listening 丢弃 Opus |
| `llm` | 下行 | `emotion` 表情 | 无关 |
| `mcp` | 双向 | 设备是 MCP **server**；`tools/call` Schedule 到主任务 | 无笔划工具；属性仅 bool/int/string；list ~8000B |
| `system` | 下行 | 仅 `reboot` | 不可用 |
| `alert` | 下行 | 状态/文案/表情 + 振动音 | 最多当错误提示，不是候选页 |
| `custom` | 下行 | `CONFIG_RECEIVE_CUSTOM_MESSAGE` **默认 n**；把 payload 打成 system 气泡 | **无稳定产品语义**（ADR 0002 已否决） |
| 未知 | 下行 | `Unknown message type` 后丢弃 | 现网即使发 `stroke` 设备也不处理 |

上行（`Protocol` 公开/半公开）：`hello`、`listen` start/stop/detect、`abort`、`mcp`（`SendMcpMessage`）、MQTT `goodbye`。**`SendText` 为 protected**，应用层没有通用自定义 JSON 发送 API。选择回传没有现成动词。

`glyph_push`（`docs/glyph-push.md`）：给缺字补 LVGL 位图，挂在 `stt` / `tts.sentence_start`；v=1，≤64 字、≤64KiB。**不是笔顺矢量，不可当字库。**

---

## 3. HTTP / 静态资源：没有笔顺 API

仓库里的 `CreateHttp` 用途：

1. **OTA CheckVersion / Activate** — 激活与接入配置，不承载业务 JSON。多余字段被忽略。
2. **固件 GET** — `firmware.url` 或 MCP `self.upgrade_firmware`。
3. **Assets GET** — NVS `assets.download_url`（MCP `self.assets.set_download_url` 写入，启动时下载）。内容是 `assets.bin`：唤醒模型、CBIN 字体、emoji/GIF/EAF、背景主题。**解析器不认识笔顺。** ADR 0001/0003 禁止整库进 Flash、禁止逐帧图/SVG/字体轮廓。
4. **预览图 GET** — user-only `self.screen.preview_image`，整图进 LVGL，非矢量笔顺。
5. **快照 POST / 相机 Explain POST** — 上行图片，视觉问答，与写字无关。
6. **板级 dictation POST** — Stick-S3 把 STT 纯文本 POST 到本机 helper URL，非公共笔顺服务。

**没有**可配置的通用 JSON HTTP 客户端、汉字查询 REST、或按字下载矢量笔顺的机制。OTA 规范外链飞书 wiki，仓库内不实现笔划字段。

---

## 4. MCP 为什么带不了首期协议

方向是 **云 → 调设备工具**，不是设备向云要字库。

内置工具：音量/亮度/主题/状态/拍照；user-only：系统信息、重启、OTA、截图、预览图、assets URL。无 `self.stroke.*`。

限制与 ADR 0002 一致：无 array/object 属性；`tools/call` 一次调用、主任务执行，不能同步等点击；候选/中线只能塞进超长 string，且受 ~8KB list 与会话超时（默认 120s）约束。云侧 MCP（README「知识搜索」）若存在，也只进 LLM 文本/TTS，到不了候选 Overlay。

---

## 5. 四类判定

### A. 现有公共服务已明确支持 — 无（对笔划三件事）

xiaozhi.me / tenclass OTA 已明确的是：激活、WS/MQTT 地址、ASR、LLM、TTS、设备 MCP、glyph_push、OTA/assets、控制台配模型。SUPPORT.md 云服务范围是验证码/激活/智能体/声纹/克隆，**无笔划。**

可顺便复用、但不是笔划协议：单字 STT 文本；TTS 朗读（需 Speaking）；缺字位图；官方 LLM **口头**讲笔顺（会幻觉，违反「不猜测、可靠授权数据」）。

### B. 协议可扩展但必须服务端配合 — 是，且这是产品正路

已接受 ADR：`type=stroke` 共享 Protocol；hello `stroke_order` 双方确认才开放；server candidates → device select → server data；cancel/error 带 request ID；WS 与 MQTT 语义一致。

现状：设备不声明、不解析、不发送；服务端 hello 不确认能力。开源兼容服同样要改服务端。把官方/开源服当黑盒接入 **不够**。

`custom` 不是扩展点（默认关、只显示文本）。在 stt 上塞额外字段当前会被忽略（除 glyph_push）。

### C. 设备端可直接复用 — 仅管道与约束，不含水

可复用：`OnIncomingJson` + `Schedule`；`SendMcpMessage` 线程模型；hello `features` 模式；`SendStopListening` / Idle 仍保通道（MQTT 尤其）；`CreateHttp` 模式（若将来有授权 HTTP 字源）；TTS 一次朗读；glyph_push 显示候选汉字形。

不可当实现：STT 当候选列表；glyph_push 当笔顺；assets/GIF 当演示；MCP 标量工具当选字协议；`custom` 当产品通道。

### D. 完全不可用（相对已接受首期）

- 官方/仓库内 **笔顺数据面** 与授权字库
- ASR n-best / 同音字排序 / 按「有可靠笔顺」过滤（ADR 规定服务端做，设备只做 ≤6 有界校验）
- 选字后 8s 内按需矢量（轮廓+中线，0–1024）
- 设备泛化 HTTP 查字
- 不改固件就接收 `type=stroke`

---

## 6. 不开发云端时的最小可行替代

按已接受决策，首期 **不是** 设备本地字库方案。不写云时只有降级，不能称为产品 MVP。

| 方案 | 做什么 | 能否满足首期 | 主要限制 |
|---|---|---|---|
| **0. 正路：合作现有云加 `stroke`** | 官方或已部署的 xiaozhi 兼容服实现 candidates/select/data | 能，但 **需要服务端开发**（本题排除） | 双方 hello `stroke_order`；授权矢量数据 |
| **1. 口头笔顺（唯一零云改动）** | 用户说「某怎么写」，走现有 ASR+LLM+TTS | **否** | 无候选点选、无动画、LLM 可能编造笔顺、无法保证《通用规范汉字表》与授权数据 |
| **2. 设备内嵌极小 fixture** | 固件/测试夹具若干字，STT 当唯一候选 | **否**（且违 ADR） | 无同音排序；禁止用测试 fixture 当发布字库；禁止整库 Flash；同音/多候选做不到 |
| **3. 设备 HTTP 拉第三方公开笔顺** | 自写 GET + 解析 Make Me a Hanzi 等 | **否** | 无现成客户端；源多是 SVG/graphics，禁任意 SVG；许可/大陆规范未验证；无 ASR n-best；等于新数据面 |
| **4. assets.bin GIF/EAF 每字一动画** | 主题包塞逐帧 | **否** | ADR 禁逐帧图；分区/RAM；难暂停/逐笔/主题色；不能按需 |
| **5. 自建开源服并改协议** | 跑 xiaozhi-esp32-server 等 | 能做产品，但 **就是实现云** | 不在本仓库；仍要数据授权 |

**建议：** 固件按 ADR 做 `StrokeOrderController` + `type=stroke`，把云当作硬依赖；在服务端未宣布 `stroke_order` 前 **不暴露入口**（已接受决策）。不开发云则只能做口头降级，不要伪装候选+动画。

---

## 7. 关键限制（实现时仍成立）

- 通道 120s 空闲超时；候选页最多等 60s 且停听进 Idle，必须保控制面（MQTT 长连更自然，WS 随会话）。
- Idle 清聊天；笔划必须独立 Overlay。
- 主循环不可阻塞；禁止在 net/LVGL 线程发协议或持有 `cJSON*`。
- TTS 朗读失败不得挡动画；无音频则静音演示。
- 无触摸/OLED/无屏板不声明能力。
- 迟到响应按 request/session ID 丢弃 — 现网完全没有这些字段。

---

## 8. 证据（只读）

- `README.md`：xiaozhi.me、开源服务列表、云侧 MCP 能力描述
- `docs/websocket.md`、`docs/mqtt-udp.md`、`docs/mcp-protocol.md`、`docs/glyph-push.md`、`docs/mcp-usage.md`
- `docs/adr/0001-stroke-order-feature-boundaries.md`、`0002-use-shared-protocol-for-stroke-order.md`、`0003-use-bounded-vector-stroke-data.md`
- `main/application.cc` OnIncomingJson / InitializeProtocol / SendMcpMessage / CheckAssetsVersion
- `main/protocols/protocol.{h,cc}`、`websocket_protocol.cc`、`mqtt_protocol.cc`、`text_glyph_payload.cc`
- `main/ota.{h,cc}`、`main/assets.{h,cc}`、`main/mcp_server.{h,cc}`、`main/Kconfig.projbuild`（OTA_URL、RECEIVE_CUSTOM_MESSAGE default n）
- `.github/SUPPORT.md` 官方云服务范围

**未验证：** 线上 xiaozhi.me 是否私下发未知 JSON（设备也会丢）；开源服 fork 是否已有笔划（本仓库无代码/文档）。物理板与真机协议未测。

- 官方默认可连接面是 OTA https://api.tenclass.net/xiaozhi/ota/ 下发的 xiaozhi.me WebSocket/MQTT 会话：ASR+LLM+TTS、设备 MCP、glyph_push、固件/assets，文档与代码均无 stroke_order/笔顺字库。
- OnIncomingJson 只处理 tts/stt/llm/mcp/system(reboot)/alert/可选 custom；未知 type 丢弃。stt 仅为单 text + 可选 glyph_push 位图，无 n-best 候选。
- 设备上行公开 JSON 只有 hello/listen/abort/mcp（及 MQTT goodbye）；Protocol::SendText 为 protected，应用层没有选字回传动词。
- HTTP 仅用于 OTA/激活、固件 GET、assets.bin GET、预览图 GET、快照/相机 POST；assets 只含字体/表情/唤醒词/主题，解析器不接受笔顺矢量。
- MCP 是设备 server、属性仅 bool/int/string、tools/list 约 8KB；无 self.stroke 工具，且不适合异步点选。CONFIG_RECEIVE_CUSTOM_MESSAGE 默认关闭且只把 payload 显示为 system 文本。

<!-- pi-squad:3b4dfdff5e5d747400ad006c70c78410cf2c94cb76e06cc1dad1308077efd88a -->
## 2026-09-12T05:12:08.675Z — stroke-local-foundation

Implemented the CoreS3 local-first 笔划 data foundation without touching unrelated uncommitted stick-s3/emoji work and without importing hanzi-writer-data. Added host converter scripts/convert_stroke_order.py (pinned local checkout + charset + commit → deterministic stroke_order.bin + provenance manifest), SOB1 bounded parser in main/stroke_order/, default-off CONFIG_STROKE_ORDER_LOCAL (CoreS3-only CMake compile), host tests (16 stroke + 83 discover OK), and license/ADR docs that keep type=stroke as a future networked path. Residual: C++ parser not executed on host/device, clang-format missing, no 500-char dataset, no UI/pointer/protocol.

- Existing uncommitted work was stick-s3 emoji/dictation plus CMake/build_default_assets local-emoji path; stroke work only added a 5-line CONFIG_STROKE_ORDER_LOCAL source append to CMakeLists.txt and left build_default_assets.py untouched.
- SOB1 v1 is little-endian magic SOB1 with header CRC of bytes 0..23, strictly sorted index entries of codepoint/offset/length/crc, and per-character closed outline + median polylines in integer 0..1024 y-down; Bind validates header+index only and LoadCharacter failure must not clobber prior valid state.
- Host conversion must take an explicit local hanzi-writer-data directory, charset, and hex commit; it accepts only MLQCZ path commands, flattens Q/C with de Casteljau tol 0.25/depth 8, rejects network paths/SVG extras, and must not vendor all.json.
- CONFIG_STROKE_ORDER_LOCAL defaults n and depends on BOARD_TYPE_M5STACK_CORE_S3, so non-CoreS3/no-data builds do not compile stroke_order_store.cc. The store never heap-allocates; views point into a caller-owned blob.
- Host verification in this environment: python3 -m unittest scripts.tests.test_stroke_order -v (16 OK) and python3 -m unittest discover -s scripts/tests -v (83 OK). clang-format was not installed; idf.py was not run; Apple clang++ could not find C++ stdlib headers so the device parser was not executed.

<!-- pi-squad:64b9c00d7c7ea3400465bdda2e9cc05e456bf7d7853789881d4e5f311c641829 -->
## 2026-09-12T07:51:24.555Z — stroke-stt-local-candidates

Implemented CoreS3 local-first 笔划 STT session: Idle entry schedules a main-task voice session (AwaitingSpeech overlay + existing StartListening), intercepts STT only when pending, parses one Hanzi by Unicode code point, SetCandidates (recognized char first, store-only, dedup, max 6; smoke 一/人/口 correctly shows 1 with no fake homophones), then local playback. No type=stroke. Host 98/98 (kept prior 96). Isolated IDF v6.0.2 m5stack-core-s3 CONFIG_STROKE_ORDER_LOCAL=y build produced /tmp/xiaozhi-stroke-stt-local-candidates-build/xiaozhi.bin 2,886,016 bytes (30% free). Report: subagent-artifacts/.../stroke-stt-local-candidates.md. Residual: real ASR + CoreS3 e2e.

- BindStore no longer auto-lists the whole SOB1 library; production candidates come only from bounded SetCandidates/SetCandidatesFromPrimary, so the three-char smoke corpus correctly yields one candidate when there is no homophone provider.
- Stroke voice is an independent StrokeOrderSession (token + AwaitingSpeech/Candidates/NoMatch/TimedOut/Error) with lifecycle listen-hold; DeviceStateMachine is unchanged and Connecting/Listening are the only extra device classes allowed while awaiting speech.
- OnIncomingJson must copy STT text before Schedule; intercept is decided by an atomic pending token on the net thread and re-checked on main so late/duplicate STT cannot mutate LVGL, while ordinary stt/glyph_push/chat bubbles stay on the original path.
- Follow-up TTS/LLM of the current stroke round is dropped via awaiting||suppress (covers stt→tts races) and cleared on Cancel so the next normal conversation is not swallowed; StopActiveListeningRound stops mic upload without closing the control channel.
- Host-testable ParseStrokeOrderUtterance counts Unicode code points (not UTF-8 bytes) and accepts 单字/某字/某怎么写/某词的某 while rejecting empty, ASCII, emoji, invalid UTF-8, overlong, ambiguous multi-char, and CJK not in the local store.

<!-- pi-squad:063f156fd5f28d289d1371d744e30f652d7f47d413b6409d80e9d5906ccb545f -->
## 2026-09-12T11:36:43.572Z — stroke-500-candidate-design

# 500-character local pinyin candidate design

Read-only. No files changed; no tests run. Minimum upgrade of `StrokeOrderNullHomophoneProvider` to a ~500-char local pinyin index on the existing `StrokeOrderCandidateProvider` seam. Does not implement `type=stroke`, parse LLM/TTS, claim ASR confidence, or enable MQTT voice. 500-glyph SOB1 packaging is a sibling prerequisite; SPY1 covers exactly the packaged charset (3-char smoke now, ~500 later).

## Current firmware

- `AppendHomophones` writes extras only; Null provider returns 0.
- `SetCandidatesFromPrimary`: primary first, then extras. `FilterCandidateLocked`: U+4E00..U+9FFF, in SOB1, unique, loadable, max 6 (`kMaxCandidateInputs`=24).
- `ApplyVoiceUtteranceLocked` requires `Contains(parsed)` + Null provider → unsupported STT char cannot show in-library homophones.
- MQTT `SupportsStrokeVoiceRouting()==false`; fallback hardcodes 一/人/口, never opens audio. WS is the only correlated STT path.
- STT bounded on net callback (≤96 B UTF-8); fill after `CommitStrokeStt` on main/LVGL. Converter does **not** ingest hanzi-writer-data dictionary (LGPL risk). SOB1 is Arphic graphics only.
- `GetAssetData("stroke_order.bin")` aliases mmap (`ZZ` stripped); `BindStore` nothrow-copies ≤1 MiB; unbind before munmap. Asset names ≤31 chars.

## 1. Pinyin source / input

Do **not** read pinyin from hanzi-writer-data.

Source: reviewed UTF-8 TSV derived offline from Unicode Unihan `kMandarin` (`kHanyuPinyin` only if kMandarin missing). Unicode license. Generator never networks. Do not vendor Unihan in firmware.

File `scripts/stroke_order/data/pinyin_table.tsv` + NOTICE (Unihan tag, not-a-release):

```
# UTF-8, BOM stripped, # comments
# char <TAB> rank <TAB> pinyin[ pinyin...]
# rank unique uint16, 1=most common, frozen, not ASR
# pinyin /^[a-z]{1,6}[1-5]$/  轻声=5
一	1	yi1
衣	48	yi1
行	20	xing2 hang2
```

Rules: one U+4E00..U+9FFF scalar; table **equals** charset; rank dense 1..N (do not use SOB1 codepoint order as frequency); keep all polyphone readings; any mismatch/invalid syllable → convert error, no partial output. ~500 list = reviewed 常用简体 ∩ pinned hanzi-writer-data.

## 2. Offline host generator

```
python3 scripts/convert_stroke_pinyin.py \
  --charset charset.txt --pinyin-table scripts/stroke_order/data/pinyin_table.tsv \
  --sob1 /path/stroke_order.bin --output-dir /outside/checkout/out/
```

No network/URL/UNC/`all.json`. Validate charset + TSV + SOB1 (`validate_blob`, every char loadable). Strip tones for grouping only (`yi1`/`yi4`→`yi`); device stores group ids, not strings. Emit `stroke_pinyin.bin` + deterministic `stroke_pinyin.manifest.json` (Unihan tag, SHA-256s, `not_a_release_library`). Atomic staging like smoke packager.

Smoke: also emit SPY1 for 一/人/口 (not homophones: yi/ren/kou) so primary-only tests stay valid. Homophone tests use **synthetic** SOB1+TSV, not upstream glyphs. CMake adds `stroke_pinyin.bin` to `STROKE_ORDER_ASSET_OUTPUTS`. Keep Arphic with SOB1; pinyin NOTICE separate.

## 3. Device SPY1 (do not extend SOB1)

Asset `stroke_pinyin.bin`. LE, CRC-32/ISO-HDLC.

```
Header 32B: magic SPY1, ver=1, flags bit0=tone-stripped,
  char_count, group_count, char_index_off=32,
  group_index_off=32+char_count*12, header_crc(0..23), body_crc
CharIndex[n] 12B, codepoint strictly increasing:
  u32 cp, u16 rank unique 1..n, u8 readings 1..8, u8 0,
  u32 off → u16 group_id[readings] increasing
GroupIndex[g] 12B, group_id increasing:
  u16 id, u16 members 1..32, u32 off → u32 cp[members] rank-sorted, u32 0
```

Limits (C++ ↔ Python lockstep): file ≤64 KiB; chars/groups ≤1024; readings/char ≤8; members/group ≤32; overflow-checked offsets; unknown version fails bind, previous state kept; no alloc; binary search. ~500 chars ≈8–20 KiB.

`StrokeOrderPinyinIndex` parser; `StrokeOrderPinyinProvider` non-owning index. `AppendHomophones`: lookup primary (0 if missing); union group ids; walk members by rank; skip primary + dups; write ≤cap; do **not** test SOB1 (filter remains glyph gate).

## 4. Policy: polyphone, dedup, primary first, rank, max 6

```
ordered=[primary]  # exact STT first
append AppendHomophones(primary, cap=6-len)  # rank-sorted extras
SetCandidates(ordered)  # store filter, stop at 6
```

| Case | Result |
|---|---|
| In SOB1, no peers | 1 candidate |
| In SOB1 + peers | primary + rank fill ≤6 |
| Polyphone 行 | union groups, primary first |
| Dup across groups | one slot |
| Outside SPY1 and SOB1 | NoMatch |
| Outside SOB1, in SPY1 | extras only |
| All filtered | NoMatch |
| SPY1 unbound | primary only (today) |

**View:** drop `Contains()` short-circuit; gate on `SetCandidatesFromPrimary`/`OpenCandidates`. Parse Ambiguous/NoTarget→NoMatch; InvalidUtf8/TooLong already aborted on net thread.

**MQTT fallback:** if SPY1 bound, `TopRanked(6)` ∩ store; else smoke 一/人/口. Never open audio / mint session id.

## 5. UTF-8 / memory / lifecycle

TSV UTF-8 on host only. Device uses u32 cps + u16 group ids. STT still `StrokeOrderParse` (96 B / 32 cps).

Rebind nothrow-copies SPY1 ≤64 KiB then may drop mmap. Query on stack (`extra[6]`, `groups[8]`). Glyphs still `CopyCandidateGlyph` from owned SOB1.

```
UnApply → SuspendAssets → Unbind SPY1+SOB1 → munmap
Apply → copy/validate SOB1 (entry requires it) → copy/validate SPY1
  SPY1 fail: log, Null provider, do not hide entry
```

Entry: SOB1 + LVGL pointer + Idle. AppendHomophones only after CommitStrokeStt; index immutable; net thread must not walk SPY1 or keep `cJSON*`.

## 6. WebSocket voice (routing unchanged)

SO → fresh WS channel + hello session_id → listen → `type=stt` snapshot InterceptStrokeStt → copy UTF-8 → Schedule → CommitStrokeStt → FinishStrokeListening (stop/drain/close/Idle) → Parse → SetCandidatesFromPrimary(pinyin) → Candidates 60s. No LLM/TTS, no `features.stroke_order`, no WS-only JSON.

## 7. MQTT fail-closed voice

Keep `SupportsCorrelatedSessionOpen`/`SupportsStrokeVoiceRouting` false. No nonce, no forged identity. No-voice → local candidates + `当前连接仅支持点选候选字`. Ordinary MQTT STT stays generation-0 PassNormal.

## 8. Test matrix (host first)

P1 TSV UTF-8/BOM/dup/bad syllable fail; P2 charset≠TSV≠SOB1 fail; P3 handwritten SPY1 CRC/unaligned/unknown ver/overflow; P4 bind fail preserves prior; P5 smoke 一 → 1 cand; P6 synthetic homophones primary+rank+max6+dedup; P7 polyphone union; P8 extra primary still one slot; P9 outside both → NoMatch; P10 outside SOB1 in SPY1 → extras; P11 unbound → primary only; P12 parse statuses; P13 WS session_id; LLM/TTS dropped; P14 MQTT no open/listen, TopRanked/smoke, ordinary STT; P15 SOB1 ok+SPY1 corrupt → entry stays; P16 Suspend unbinds both; P17 CMake atomic outputs; P18 converter no urllib/all.json. CoreS3 later: WS homophone, MQTT tap, 100-session heap/WDT.

## 9. Expected file changes (not done)

New: `stroke_order_pinyin.h/.cc`; `scripts/stroke_order/pinyin.py`; `convert_stroke_pinyin.py`; `data/pinyin_table.tsv`+NOTICE; `stroke_order_pinyin_harness.cc`; `handwritten_spy1_v1.hex` + synthetic homophone fixture.

Edit: candidates.h docs; view bind SPY1 + drop Contains + MQTT TopRanked; smoke packager; CMake/Kconfig; test_stroke_order*.py; session/controller harness; stroke-order-data.md; README; constants.py SPY1 limits.

Do not change: DeviceStateMachine, hello `stroke_order`, MQTT voice flags, SOB1 layout, StrokeRoundCoordinator routing.

Risks: 500-glyph size vs 1 MiB is packaging; Unihan ≠ 笔顺 certification; tone-strip over-recalls; charset-only index → 库外/繁体 STT = NoMatch, no 繁简 guess.

- StrokeOrderNullHomophoneProvider is a no-op; SetCandidatesFromPrimary puts the STT primary first then extras, and FilterCandidateLocked keeps U+4E00..U+9FFF, in-bound SOB1, unique, loadable, max 6.
- ApplyVoiceUtteranceLocked currently requires Contains(parsed.codepoint) plus NullHomophoneProvider, so an unsupported STT character cannot surface in-library homophones.
- WebSocket is the only correlated stroke-STT path; MQTT SupportsStrokeVoiceRouting is false and degrades to a local candidate page without opening audio or forging a session id.
- The SOB1 converter explicitly does not ingest hanzi-writer-data dictionary/pinyin (possible LGPL); pinyin must be a separate licensed table and a separate device asset.
- stroke_order.bin is mmap-aliased then nothrow-copied (≤1 MiB) and unbound before munmap; a sibling SPY1 blob can follow the same lifecycle without extending SOB1.
- Stroke STT is UTF-8 bounded on the network callback (≤96 bytes); candidate fill runs only after CommitStrokeStt on main/LVGL; LLM/TTS are dropped and type=stroke is not used.

<!-- pi-squad:7f6dc396d1c7bf4099a94aa1ac1573c9d2ed6391638582a2dbae116f4546b266 -->
## 2026-09-12T12:17:40.093Z — stroke-500-assets-pinyin-retry

Completed the timed-out stroke-500-assets-pinyin work in place (no rollback/commit). SPY1 C++ parser is bounded (LE/unaligned, CRC, overflow, ≤64KiB, rank/readings/groups/members); missing/corrupt SPY1 degrades to exact-only without unbinding SOB1. Controller/View use the provider (primary first, polyphone union, rank, dedup, store/loadable, max 6; out-of-store primary may still yield homophones). MQTT uses TopRanked(6) and never opens voice. Firmware extra-files now consume the pre-generated 500 pack (SOB1 1,046,788 / SPY1 18,152) mapped to stroke_order.bin/stroke_pinyin.bin plus selection/coverage/source locks/manifests/SHA256SUMS/APL/Unicode/NOTICE. ESP owned copies are PSRAM-only (MALLOC_CAP_SPIRAM|8BIT) with no DRAM fallback. Host suite 110/110. Isolated ESP-IDF v6.0.2 CoreS3 CONFIG_STROKE_ORDER_LOCAL=y build produced xiaozhi.bin 2,907,712 bytes (SHA-256 911a0615…) and generated_assets.bin 2,877,288 bytes; ~30% app partition free. Not flashed; type=stroke untouched.

- Prototype SOB1 is exactly 1,046,788 bytes (1,788 under the unchanged 1 MiB kMaxFileBytes=1048576); SPY1 is 18,152 bytes under the 64 KiB cap. Generation must fail closed rather than raise those limits.
- Device SOB1/SPY1 owned copies allocate with heap_caps_malloc(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT) only. Failure hides SO or degrades SPY1 to exact-only; there is no internal-DRAM fallback. Host tests use new[].
- Ordinary CoreS3 builds package the reviewed prototype_500 fixture offline via package_stroke_order_prototype.py (14 extra-files, names ≤31 chars, atomic publish). CMake no longer uses the 3-char smoke packager for firmware.
- WebSocket STT candidates: recognized char first, polyphone union, rank order, dedup, store/loadable filter, max 6. MQTT local pages use AppendTopRanked(6) and never open stroke voice. Corrupt/missing SPY1 must not unbind SOB1.
- Official 《通用规范汉字表》 PDF SHA-256 is af85c706a53d3b3bbad818bcce7415ac9a2284ea14f79fe7f54ce1248a7bdac9. The 500 pack is not official, not commercial, not manually 500-reviewed; HWD dictionary/LGPL is not ingested; Unihan readings use UNICODE-LICENSE.txt.

<!-- pi-squad:d7c2bcf6c2c7006f5fd36d4eb4dbf42699358f37665b0caa1a5a322ff56db47b -->
## 2026-09-12T13:45:37.698Z — stroke-websocket-preference

Fixed CoreS3 500-char SO skipping voice: extracted SelectProtocolTransport and, only for CONFIG_STROKE_ORDER_LOCAL, prefer WebSocket when OTA has websocket even if MQTT is also present. MQTT-only still MQTT; non-stroke builds keep MQTT-first. MQTT SupportsStrokeVoiceRouting remains false. TDD: host harness red on dual-config+stroke-pref (returned MQTT), then green. Host suite 116 tests OK (1 skip). clang-format 19.1.7 and git diff --check passed. Isolated ESP-IDF v6.0.2 CoreS3 CONFIG_STROKE_ORDER_LOCAL=y clean build produced xiaozhi.bin 2,908,944 B SHA-256 f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a and generated_assets.bin 2,877,806 B SHA-256 6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0 (500-char assets, not_a_release_library). No flash, no 500-data edits, unrelated uncommitted work preserved. Hardware gate: serial must show selected=websocket and stroke_voice=1; if websocket_config=0 the device will still local-fallback and the next check is whether OTA only ships MQTT.

- Application::InitializeProtocol previously chose MqttProtocol whenever HasMqttConfig() was true, so dual MQTT+WebSocket OTA configs never enabled stroke voice.
- MqttProtocol still reports SupportsCorrelatedSessionOpen=false and SupportsStrokeVoiceRouting=false; WebsocketProtocol reports true/true. StrokeVoiceRoutingAvailable requires both.
- SelectProtocolTransport in main/protocols/protocol_selection.h is the production seam: stroke-local builds prefer WebSocket iff has_websocket_config; otherwise original MQTT-first including empty-config MQTT fallback.
- Isolated ESP-IDF v6.0.2 m5stack-core-s3 CONFIG_STROKE_ORDER_LOCAL=y build at /private/tmp/xiaozhi-stroke-websocket-preference-build: xiaozhi.bin 2908944 bytes sha256 f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a; generated_assets.bin 2877806 bytes sha256 6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0; stroke_order.bin 1046788 bytes / 500 chars.
- This firmware cannot prove live OTA includes a WebSocket config. If serial shows websocket_config=0 selected=mqtt stroke_voice=0, local candidates after SO are still the safe MQTT degrade.

<!-- pi-squad:90b36fb5bd515d9ffb1a07b58f8d6b8ab48c97f8bb49c81d99d8b9fbf30124bf -->
## 2026-09-12T14:24:12.022Z — stroke-speak-timeout-diagnosis

# Stroke Speak→Timeout diagnosis (read-only)

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

- Overlay Speak is shown in StartVoiceSessionFromMain before OpenAudioChannel/bind/listen; overlay Timeout is only SpeechTimeout after 15s in coordinator Phase::AwaitingSpeech (kSpeechTimeoutMs), which requires a successful BindOpenedChannel.
- Stroke voice listen always sends Protocol listen start with kListeningModeManualStop; FinishStrokeListening is the only stroke path that SendStopListening, and it runs only after CommitStrokeStt. Ordinary chat uses AutoStop or Realtime.
- WebsocketProtocol stamps hello session_id onto incoming binary audio packets only. OnIncomingJson STT/TTS/LLM routing uses ProbeJsonString(session_id) from the server JSON. Missing/mismatch during an active round is FailStroke (notification), not Timeout. Matching non-STT is Drop.
- speech_started_ms_ is set in BeginRound (same moment as Speak) and only reset by MarkListeningStarted in StartListeningAudio. CheckTimeouts does not require listen-start. Clock tick is 1s.
- Stroke local builds set prefer_websocket_for_stroke_voice; WebsocketProtocol SupportsStrokeVoiceRouting/SupportsCorrelatedSessionOpen are true; MqttProtocol both false. MAIN_EVENT_SEND_AUDIO is not stroke-generation gated; DrainStreamingAudio/ResetStreamingState disables voice processing and clears send queues.

<!-- pi-squad:9ffe3045e40bf26f25fdfe673efa796dc502c911fcf65c4c1a4ae552005a3eee -->
## 2026-09-12T14:32:38.319Z — stroke-voice-autostop

Fixed CoreS3 SO Speak≈15s Timeout: stroke StartListening now uses AutoStop/VAD instead of hardcoded ManualStop. TDD helper went red on always-ManualStop, then green (gen0 ManualStop, stroke AutoStop). Application uses ListeningModeForStartGeneration on first-open and existing-channel branches; GetDefaultListeningMode unchanged. Overlay shows Connect until listen/start+mic succeed, then Speak. Speech timeout starts at MarkListeningStarted after successful send. SendStartListening returns bool; send failure never shows Speak. Host 117 tests OK (1 skip). clang-format 19.1.7 and git diff --check passed. Isolated IDF v6.0.2 CoreS3 CONFIG_STROKE_ORDER_LOCAL=y clean build: xiaozhi.bin 2909536 B sha256 ec03a329…, generated_assets.bin 2877806 B sha256 6d403211… (500-char assets unchanged). Not flashed. Next gate: SO shows Connect then Speak; speech yields candidates within 15s.

- HandleStartListeningRequest/ContinueOpenAudioChannel previously hardcoded kListeningModeManualStop for stroke generations; FinishStrokeListening only SendStop after STT. Public manual ASR waits for stop before final STT, producing a 15s AwaitingSpeech Timeout with no CommitStrokeStt/FailStroke.
- ListeningModeForStartGeneration is the production seam: generation 0 stays ManualStop (press-to-talk); nonzero stroke generations use AutoStop. GetDefaultListeningMode (ordinary chat auto/realtime) is independent and unchanged.
- Speak must not be shown at StartVoiceSessionFromMain. Connect is the pre-listen page; Speak is only after StartListeningAudio passes the fence, SendStartListening succeeds, voice processing is enabled, and MarkListeningStarted runs.
- speech_started_ms_ must stay 0 until MarkListeningStarted. BeginRound/bind/connecting must not consume the 15s speech budget. CheckTimeouts requires speech_started_ms_ != 0.
- Protocol::SendStartListening now returns SendText's bool on the shared Protocol layer (both transports). Stroke send failure fences/aborts and never shows Speak. MQTT SupportsStrokeVoiceRouting remains false.
