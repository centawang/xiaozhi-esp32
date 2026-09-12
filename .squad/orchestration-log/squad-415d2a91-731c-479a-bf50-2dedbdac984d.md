

<!-- pi-squad:8a4c8024961cb9e5aca70220c7c75b63c930f9f6afc208340d0307049bbd984d -->
## backend — existing-service-capabilities
Mode: read; outcome: completed
Engine: a8bae3c6-4848-4411-97b5-25d298877bb4

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

<!-- pi-squad:3c14e0217b9d20192c1535b68c177cff8aa9831b80f15b1d666e54b52806bd64 -->
## lead — no-cloud-feasibility
Mode: read; outcome: completed
Engine: a8bae3c6-4848-4411-97b5-25d298877bb4

# 笔划功能：不自建云服务时的可行性评估

## 1. 结论

需要先区分两种“无云”含义：

1. **用户不自建后端，但仍使用现有小智远端服务或公开第三方服务**：可以做条件式方案，但仍依赖外部服务能力、稳定性和授权。
2. **设备不依赖任何外部服务**：只能做本地数据、本地入口的收敛版本；当前仓库没有任意汉字的离线 ASR、通用离线中文 TTS 或本地候选排序能力。

当前团队已接受的完整链路是 `server candidates → device select → server data`，并要求双方通过 hello 确认 `stroke_order`、使用 `type:"stroke"`、由服务端负责候选生成/排序/数据可用性过滤和按需笔顺数据下发。**这个完整范围不能仅靠固件完成，必须有远端服务新增并部署 `type:"stroke"` 语义。**

仓库只包含设备端固件，没有小智云端实现；现有协议文档和代码均没有 `stroke` 消息。因而：

> 不能假定现有小智服务会识别、转发或正确响应一个未定义的 `type:"stroke"` 协议，也不能把“WebSocket/MQTT 能承载任意 JSON”误认为“服务端已经支持该业务语义”。

若用户明确不做任何云端改造，最稳妥的产品调整是推出独立的**“笔划离线演示版”**：CoreS3 上通过明确的触摸入口，从内置、已授权、已版本化的小规模汉字集合中选择并演示；不承诺自然语言候选、ASR 置信度排序、全量《通用规范汉字表》覆盖或在线按需取数。

## 2. 当前代码证据

- `main/Kconfig.projbuild` 的默认 OTA 地址是 `https://api.tenclass.net/xiaozhi/ota/`；`main/ota.cc` 从 OTA 响应取得 MQTT/WebSocket 配置。仓库没有该服务的后端实现，无法据此证明线上服务支持任何未文档化扩展。
- `docs/websocket.md` 列出的现有业务消息包括 hello、listen、abort、STT、TTS、LLM、MCP、system、alert 和可选 custom，没有 stroke。
- WebSocket 与 MQTT 收到非 hello JSON 后都会交给 `OnIncomingJson`，但 `main/application.cc` 对未知类型只记录 `Unknown message type`；`main` 中未发现 StrokeOrder 或 stroke 消息实现。
- `Protocol` 只有 listen、abort、wake word、MCP 等公开发送方法；`SendText` 是受保护的传输实现，当前没有 `SendStroke...` 语义。
- 两个 hello 发送端目前只公布 MCP、AEC、glyph push 等能力；`WebsocketProtocol::ParseServerHello` 和 `MqttProtocol::ParseServerHello` 均未读取服务端 `features`，所以“双方确认 stroke_order”本身也尚需固件实现。
- `custom` 默认关闭，且当前只把 payload 打印为系统聊天文本，不具备候选选择、请求关联、取消、重试或数据返回语义。

## 3. 方案比较

| 方案 | 是否需要新增 `type=stroke` | 能否只改固件 | 主要结论 |
|---|---:|---:|---|
| 复用现有小智服务完成原定完整流程 | **需要** | 否 | 服务端必须确认能力、生成候选、处理选择并下发数据；当前线上支持无证据，不能假定 |
| 仅复用现有 STT，候选和数据本地化 | 不一定 | 部分可以 | 可做窄化实验，但 STT 无已文档化置信度/最终态字段，服务端仍可能继续 LLM/TTS，无法保持原定候选质量和交互时序 |
| MCP/LLM 生成候选，本地 Assets 提供数据 | 可不新增 wire type | **条件式** | 依赖现有服务是否会发现并正确调用新工具；适合实验，不等于零远端依赖，也不满足原定确定性排序 |
| 设备直接调用公开第三方笔顺 API | 不需要小智 stroke | 固件可写客户端，但运行依赖第三方 | 技术上可做 PoC；未选定且未验证授权、格式、SLA，生产风险高 |
| 小规模授权数据放 Assets | 不需要 | 是 | 最可控的无云数据方案；需要改变“仅会话内存、不持久化”的既有边界 |
| 纯本地演示模式 | 不需要 | 是 | 推荐的无云首版；入口、候选集合、朗读和支持范围都要收敛 |

### 3.1 复用现有小智远端服务

若维持已接受的产品范围，远端至少要实现：

- 在 hello 中明确确认 `features.stroke_order=true`；
- 从语音意图生成、去重和排序最多 6 个候选，并按可靠数据可用性过滤；
- 发送 `type:"stroke"` candidates；
- 接收 select/retry/cancel/error，按 request/session ID 幂等处理；
- 从有明确来源、版本和授权的数据源取得选中字的轮廓与中线；
- 在 8 秒限制内返回受限 version=1 矢量数据；
- 保证 WebSocket 与 MQTT 控制面的相同语义。

固件端 JSON 传输路径具备承载能力，只能减少底层传输改动，**不能消除服务端业务实现**。在服务端未确认能力时，按已接受的能力协商规则必须隐藏入口，不能“先发一个 stroke 试试看”。

一个较弱的混合方案是继续使用现有 `stt.text`，在设备已明确进入笔划模式时由固件解析“某字怎么写”，再查本地数据。这不要求新消息类型，但存在三项硬限制：当前 STT 消息没有文档化的候选置信度或 final 标记；设备不知道服务端何时会开始 TTS；同音候选的字频与 ASR 排序也不在设备上。因此它只能作为实验入口，不能宣称实现原定候选规则。

### 3.2 通过 MCP/LLM 获取候选

当前设备是 MCP Server，后端是 MCP Client：后端发现工具后调用 `tools/call`。因此可以设想新增一个受能力保护的工具，例如让 LLM 传入一个目标汉字或一个最多 6 字的候选字符串；设备立即打开本地候选页并返回“已展示”。这条路在 wire 层可以继续使用现有 `type:"mcp"`，不必新增 `type:"stroke"`。

但它仍有明显边界：

- MCP 属性当前只支持 bool、int、string，不支持候选数组或结构化笔画数据；虽然可用字符串临时承载，但必须另做严格解析。
- `tools/call` 在 Application 主任务执行并形成一次响应，不能阻塞 60 秒等待用户点击；点击后的通知若要由后端继续处理，线上服务仍需理解新通知语义。
- LLM 可能从文本推测候选，但没有证据表明它能获得原始 ASR 置信度并按“ASR 置信度 + 常用字频”稳定排序。
- LLM 不能作为可靠笔顺数据源；设备仍必须只接受本地授权数据或可信数据服务，不能让模型生成笔画。
- 仓库文档描述了通用 MCP 工具发现/调用，但线上服务是否自动发现任意新工具、是否允许模型调用、提示词是否会触发，均不在仓库证据范围内。

所以 MCP 更适合**“官方服务不改 wire protocol的条件式入口实验”**：LLM 只提供目标字建议，设备用本地授权清单过滤；必须有触摸入口兜底，并经过真实线上端到端验证后才能开放。它不是原定异步三阶段协议的等价替代。

### 3.3 直接使用公开第三方笔顺 API

固件已有 HTTP 客户端抽象和 GET/POST 示例，技术上可以在独立任务中请求第三方 API，并设置超时、响应大小上限和取消逻辑；不需要小智服务新增 `type:"stroke"`。

但可行的前提很严格：

- API 必须允许嵌入式设备直接调用，并有清晰的数据版权、商用授权、版本和大陆规范依据；
- 返回数据必须能确定性转换成已接受的“0–1024 整数坐标、每笔封闭轮廓 + 有序中线”格式；任意 SVG、脚本、外部资源、GIF 或逐帧图不能直接接受；
- 认证密钥放在固件中容易被提取；若需隐藏密钥、做格式转换或缓存，通常又需要自建网关，违背“不实现云服务”的前提；
- 还要处理 TLS、限流、配额、服务下线、响应漂移、隐私和离线失败；
- 笔顺 API 只解决选中字数据，不自动解决语音意图和候选排序。

仓库没有引用或验证任何可满足上述条件的公开 API。因此该方案只能列为探索性 PoC，不应作为首发生产依赖。

### 3.4 将 fixture 或字库放入 Assets

这是最接近“固件独立完成”的数据架构：

- `Assets::GetAssetData()` 可从 mmap 的 assets 分区按名取得数据；
- 默认 assets 构建脚本支持 extra files，自定义 assets 也可作为本地文件烧录；
- CoreS3 没有覆盖默认 flash 配置，当前默认是 16 MB flash，`partitions/v2/16m.csv` 给 assets 8 MB；该空间还要与字体、emoji、唤醒模型等共享；
- 建议使用一个或少数几个带索引的有界二进制/JSON 数据包，而不是数千个散文件，并记录数据集版本、字符覆盖、来源和授权。

小规模、经过授权的字符集在架构上可行，但仍需用真实数据测量包体、峰值 heap 和动画性能。不能仅凭 8 MB 分区断言完整字库一定可装下。

必须区分：

- **fixture**：只用于测试、演示和 CI，按照已有团队决策不得当作发布字库；
- **产品内置子集**：必须有可靠来源、版本和发布授权，可作为无云产品数据；
- **完整字库**：容量、字体覆盖、授权和更新方式都尚未验证。

把产品数据放进 Assets 会改变此前“服务端按需下发、只在当前会话内存保存、不持久化 Flash”的边界，需要明确作为新的产品模式，而不是悄悄混用。Assets 的在线更新也是整包下载；完全无服务时只能随固件/烧录包发布更新。

### 3.5 仅做本地演示模式

纯本地模式可以由固件完成以下闭环：

`明确触摸入口 → 内置分类/候选页 → 本地索引查字 → 校验受限矢量数据 → 田字格动画 → 暂停/继续/重播/逐笔/返回/退出`

它仍可复用已规划的 `StrokeOrderController`、`StrokeOrderView`、受限解析器和取消/幂等规则，但不产生远端 request ID，也不公布远端 `stroke_order` 能力。

需要收敛的产品承诺：

- 不使用普通单字对话自动触发；首版只提供明确的屏幕入口；
- 候选来自固定、可见的本地支持集合，不宣称按 ASR 置信度排序；
- 仅演示内置且已授权的汉字；无数据时明确提示“不支持”，不从字体猜笔顺；
- 当前仓库的离线语音能力是唤醒词/固定命令，不是任意汉字 ASR，因此不承诺离线自然语言入口；
- 通用中文朗读依赖远端 TTS；纯离线版应默认静音，或仅为很小的内置集合附带已授权录音。已有规则允许朗读失败不阻止动画；不要把远端 TTS 伪装成离线能力；
- 若要求设备从首次冷启动开始完全离线可用，还需让本地入口不依赖 OTA/激活完成，因为当前正常启动流程会联网取得配置。

## 4. 推荐的分层产品路线

### A. 无云可交付首版（推荐）

命名和定位为“笔划离线演示版”或“内置汉字演示”：

- 仅支持 M5Stack CoreS3；先完成 FT6336 到 LVGL pointer input 的坐标接入，并保持 Overlay 关闭时短触 `ToggleChatState` 的原行为；
- Kconfig 默认关闭，独立本地入口，不发送或等待 `type:"stroke"`；
- 内置一个规模受控、已授权、已版本化的数据子集；fixture 只用于开发；
- 数据选择后复制当前汉字所需的最小数据到会话内存，UI 行为继续满足暂停、重播、逐笔和立即取消；
- 默认静音或为极小集合附录音；
- 在 UI 明示支持范围，不承诺《通用规范汉字表》全覆盖。

### B. MCP 辅助入口（可选实验）

在 A 的基础上注册一个只接受单个汉字或受限候选字符串的 MCP 工具，LLM 只负责建议，设备以本地数据清单做最终过滤。必须满足：

- 线上服务真实完成 initialize、tools/list、tools/call 的端到端验证；
- 工具调用立即返回，不等待点击；
- 模型未调用或给出无效字符时回退到本地入口；
- 不把模型生成内容当作可靠笔顺数据；
- 不宣称 ASR 置信度排序。

若线上服务不能自动使用该工具，则该方案不能被算作“固件单独完成”。

### C. 完整产品（保留原架构，等待服务端）

自然语言触发、最多 6 个高质量候选、服务端数据过滤和按需下发继续使用已接受的 `type:"stroke"` ADR。固件可先实现 UI、解析器和本地 fixture 测试，但发布入口必须保持关闭，直到服务端明确部署能力并在 hello 中确认。

不建议把公开第三方 API 作为 A 与 C 之间的默认桥梁；除非先完成授权、格式和可靠性验证，否则它只是把“自建云风险”替换成“不可控外部依赖”。

## 5. 哪些是固件可以独立完成的

可以独立完成：

- CoreS3 坐标触摸接入、Overlay 触摸独占和原短触回归保护；
- StrokeOrderController/View、田字格动画、暂停/继续/重播/逐笔/取消；
- 受限矢量格式解析、容量边界、UTF-8/单字校验、会话生命周期；
- 本地 Assets 数据索引、授权子集演示、离线重播；
- 明确的本地触摸入口；
- 可选的第三方 HTTP 客户端或 MCP 工具端代码，但不包括外部服务是否可用。

不能由固件单独保证：

- 现有小智服务识别新的 stroke 业务消息；
- 服务端 hello 能力确认；
- 基于 ASR 置信度和字频的候选排序；
- 可靠、已授权、持续维护的全量笔顺数据服务；
- LLM 必然调用新 MCP 工具或理解点击通知；
- 任意汉字的离线 ASR 和通用中文 TTS。

## 6. 风险与验证说明

本次严格只读，未修改文件，未执行 shell、构建、单元测试或真机测试。结论来自对 `main/application.cc`、`main/protocols/*`、`main/mcp_server.*`、`main/assets.*`、`scripts/build_default_assets.py`、`main/Kconfig.projbuild`、分区表、CoreS3 板级实现、协议文档和三份 stroke ADR 的读取与检索。

进入实现前仍需验证：真实线上 MCP 行为、官方服务是否计划支持 stroke、候选与笔顺数据授权、真实数据包大小、CoreS3 assets 剩余容量、冷启动离线入口、触摸回归、30 FPS/heap/WDT/audio 指标，以及 WebSocket/MQTT 的一致性。
