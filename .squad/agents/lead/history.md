# Lead: project knowledge

做一个新的功能，叫“笔划”。用户说一个汉字的单字，找几个备选，用户点击屏幕选择。用动画演示正确的笔画顺序。

<!-- pi-squad:ba5b0de6229f9f32cab7bc172e3a4adea046ebcbf8892d8342302ecf6635f12e -->
## 2026-09-12T04:21:17.656Z — repo-architecture

# xiaozhi-esp32 仓库整体功能与架构（只读分析）

## 1. 分析范围与结论

本次重点阅读了 `README.md`、`AGENTS.md`、`main/main.cc`、`main/application.*`、`main/device_state_machine.*`、`main/CMakeLists.txt`、`main/Kconfig.projbuild`，并追踪了 Board、网络、音频、协议、显示、MCP、OTA、Assets 和构建脚本等必要关联实现。

总体上，这是一个基于 ESP-IDF 的多芯片、多板型语音助手固件：**核心业务代码只依赖抽象 Board/Display/Protocol/AudioService，一次构建通过 Kconfig 与 CMake 只选入一个具体板级实现**。运行时以 `Application` 的 FreeRTOS 事件循环为中心，以 `DeviceStateMachine` 约束对话和设备状态；音频、网络和协议回调通过事件位或 `Application::Schedule()` 汇入主任务。

## 2. 产品功能概览

依据 `README.md` 与实现，可归纳为：

- **语音对话**：麦克风采集、Opus 上行、服务端 ASR/LLM/TTS、下行 Opus 播放；支持传统流式链路和 Realtime 模式。
- **唤醒与打断**：ESP-SR 离线唤醒词，支持自定义唤醒词；唤醒后可上传唤醒词音频，并可在说话/聆听期间打断。
- **AEC/VAD**：S3/P4/S31 可走 AFE 引擎，支持设备侧或服务端 AEC；无 AEC 时默认 auto-stop，有 AEC 时默认 realtime。
- **网络**：Wi-Fi、以太网、USB RNDIS、ML307/EC801E 或 NT26 4G，以及部分板子的 Wi-Fi/4G 切换。
- **传输**：WebSocket；或 MQTT 控制面 + AES-CTR 加密 UDP 音频面。
- **显示与交互**：无屏、OLED、LVGL LCD、Emote 动画屏；状态栏、聊天文本、表情、主题、通知、动态字形推送、相机预览等。
- **设备能力**：音量、背光、LED、相机、电池、电源管理、按键/旋钮，以及板级自定义能力。
- **MCP**：设备作为 MCP Server 暴露音量、亮度、主题、相机等工具；板级还能注册专属工具。云端 MCP 能力不在本仓库实现。
- **运维**：首次联网配置（热点或 BluFi）、设备激活、OTA、资源包下载、版本校验、NVS 持久化。
- **国际化与硬件矩阵**：README 当前声明 38 种界面语言、138 个 Board 目录、171 个发布变体，覆盖 ESP32/C3/C5/C6/S3/P4/S31。

## 3. 启动与运行主流程

### 3.1 启动

1. `main/main.cc::app_main()` 初始化 NVS；遇到页耗尽或版本不兼容时擦除并重建 NVS。
2. 获取 `Application` 单例。其构造函数创建主事件组、1 秒时钟定时器，并根据 Kconfig 确定 AEC 初值。
3. `Application::Initialize()` 首次调用 `Board::GetInstance()`，通过链接期选中的 `create_board()` 构造具体 Board；板级构造函数通常在此初始化 I2C/SPI、显示面板、触摸、按键、codec、相机、电源和板级 MCP 工具。
4. 状态从 `Unknown` 进入 `Starting`；显示执行 `SetupUI()`，随后显示 User-Agent/板型信息。
5. 从 Board 取得 `AudioCodec`，初始化并启动 `AudioService`；发送队列、唤醒词、VAD、播放耗尽等回调只置主事件位。
6. 注册状态变化监听器、启动状态栏时钟、注册通用与 user-only MCP 工具。
7. 注册 Board 网络事件回调并调用 `StartNetwork()`；网络连接异步进行。
8. `Application::Run()` 将主任务优先级设为 10，永久等待并处理事件组。

### 3.2 配网、激活与协议初始化

- Wi-Fi 无已保存 SSID 或连接超时后，`WifiBoard` 进入 `WifiConfiguring`，启动热点配置或 BluFi。
- 网络连接事件到达主循环后，若当前是 `Starting`/`WifiConfiguring`，转为 `Activating` 并启动独立 activation task。
- activation task 顺序为：
  1. 创建 `Ota`；
  2. 检查并按需下载/应用 Assets；
  3. 请求 OTA 服务，提交系统、分区、Board 身份等信息，处理时间、激活挑战、协议配置和新固件；
  4. 有新固件时升级并重启；
  5. 根据 OTA 响应选择 `MqttProtocol` 或 `WebsocketProtocol`，两者都没有时回退 MQTT；若两种配置都存在，代码优先 MQTT；
  6. 注册协议回调并 `Start()`。
- 完成后主循环处理 `ACTIVATION_DONE`：进入 `Idle`、释放 OTA 对象、切低功耗、显示版本并播放成功音。

### 3.3 一次典型对话

- **按键/外部开始**：`ToggleChatState()` 或 `StartListening()` 只置事件位。
- **Idle → Connecting**：若音频通道未开，主循环先改状态，再调度 `OpenAudioChannel()`，避免状态 UI 被阻塞式连接覆盖。
- **Connecting → Listening**：通道建立后设置 ListeningMode；`HandleStateChangedEvent()` 启动语音处理、配置唤醒检测并发送 listen/start。
- **上行音频**：`AudioService` 的 Mic → AudioEngine → PCM encode queue → Opus task → send queue；主循环逐包调用 `Protocol::SendAudio()`。
- **服务端事件**：STT 更新用户文本；TTS start 进入 `Speaking`；TTS sentence_start 更新字幕；仅 Speaking 状态下的下行音频进入解码/播放队列；LLM 更新表情；MCP 进入 `McpServer`。
- **TTS stop**：manual 模式回 Idle；auto/realtime 回 Listening。auto 模式会等播放队列真正耗尽再启用上行，避免尾音截断。
- **唤醒词**：Idle 时编码唤醒缓存，经过 Connecting 打开通道并发送唤醒事件；Speaking/Listening 时发送 abort、清上行残留并重新监听。
- **断网/关闭**：会话状态下关闭音频通道；关闭回调被调度到主任务，清 UI 并回 Idle。

### 3.4 主事件循环职责

`Application::Run()` 串行处理错误、网络、激活、状态变化、播放耗尽、用户操作、音频发送、唤醒词、VAD、调度队列和时钟更新。外部任务需要改变应用/UI/协议状态时，应使用事件位或 `Application::Schedule()`；状态统一通过 `Application::SetDeviceState()`。

## 4. 设备状态机

`main/device_state.h` 定义：Unknown、Starting、WifiConfiguring、Idle、Connecting、Listening、Speaking、Upgrading、Activating、AudioTesting、FatalError。

`main/device_state_machine.cc` 的实际合法边为：

- Unknown → Starting
- Starting → WifiConfiguring / Activating
- WifiConfiguring → Activating / AudioTesting
- AudioTesting → WifiConfiguring
- Activating → Upgrading / Idle / WifiConfiguring
- Upgrading → Idle / Activating
- Idle → Connecting / Listening / Speaking / Activating / Upgrading / WifiConfiguring
- Connecting → Idle / Listening
- Listening → Speaking / Idle
- Speaking → Listening / Idle
- FatalError 无出边；同状态转换视为成功 no-op

状态使用 atomic 保存；监听器列表有 mutex，回调在 `TransitionTo()` 调用者上下文执行。Application 的监听器不直接改 UI，只置 `MAIN_EVENT_STATE_CHANGED`，最终由主循环刷新 Display、LED 和音频开关。

值得注意：当前合法边中没有任何状态能进入 `FatalError`；另外 `docs/websocket.md` 的示意图包含 Starting → AudioTesting/FatalError，与代码不一致，应以状态机实现为准。

## 5. 核心模块边界

| 模块 | 边界与责任 |
|---|---|
| `main/main.cc` | ESP-IDF 入口、NVS 初始化，启动 Application。 |
| `main/application.*` | 顶层编排：事件循环、状态驱动、网络/协议生命周期、激活、对话、主任务调度。 |
| `main/device_state_machine.*` | 唯一的运行状态合法性检查与观察者通知。 |
| `main/audio/` | codec I/O、AudioEngine、AFE/Lite 引擎、唤醒/VAD/AEC、Opus 编解码和有界队列；不负责协议语义。 |
| `main/protocols/` | 共享消息语义和传输抽象；WebSocket 与 MQTT/UDP 实现连接、握手、JSON/音频承载。 |
| `main/boards/common/` | `Board` 抽象及 Wi-Fi、4G、双网、RNDIS/以太网、按键、背光、电源等复用实现。 |
| `main/boards/**/` | 具体引脚、面板/触摸/codec 初始化、板级 MCP 与硬件组合。核心层不应引用具体 Board 或其 `config.h`。 |
| `main/display/`、`main/led/` | 无屏/OLED/LCD/Emote UI 与 LED 抽象；`LcdDisplay` 拥有 LVGL 布局，`LvglDisplay` 提供锁、主题、状态栏、字形缓存等。 |
| `main/mcp_server.*` | JSON-RPC 2.0 工具发现/调用；工具调用被调度到 Application 主任务，响应通过 Protocol 发回。属性类型目前仅 bool/int/string。 |
| `main/ota.*`、`main/assets.*`、`main/settings.*` | OTA/激活与远端传输配置、资源分区映射/下载、NVS 配置。 |
| `main/CMakeLists.txt`、`main/Kconfig.projbuild`、`scripts/build.py` | 板型、芯片、功能、字体、语言、资源及发布变体的构建期选择。 |

音频任务边界尤其清晰：输入、输出和 Opus 各有任务，S3/P4/S31 的 AFE 还有 fetch task；队列均有明确上限，主事件循环不承担 PCM 处理。

## 6. Board 工厂与配置选择链

完整链路为：

`main/boards/<board>/config.json` → `scripts/build.py` → `main/Kconfig.projbuild` → `main/CMakeLists.txt` → 选中 Board 源文件及本地 `config.h` → `DECLARE_BOARD(...)` → `Board::GetInstance()`。

具体过程：

1. `config.json` 定义兼容性敏感的 `type`、芯片 `target`、可选 `manufacturer`，以及一个或多个 `builds`；每个 build 的 `name` 是发布/OTA 变体身份，`sdkconfig_append` 放板级差异。
2. `scripts/build.py` 枚举并校验配置、身份唯一性和 target；通过显式 `CONFIG_BOARD_TYPE_*=y` 或扫描 CMake 的 BOARD_DIR 映射解析 Kconfig symbol，并合并语言、唤醒词、显示样式、AEC、配网等用户选项。
3. 脚本生成 `build/xiaozhi-build.sdkconfig.defaults`，用 `idf.py -DIDF_TARGET=... -DSDKCONFIG_DEFAULTS=... -DBOARD_NAME=<build.name> reconfigure` 配置，再 build/merge-bin。
4. `Kconfig.projbuild` 的 `choice BOARD_TYPE` 保证只选一个、并按 `IDF_TARGET_*` 限制；同文件还定义 Assets、语言、屏型/样式、唤醒、AEC、配网和相机等能力。
5. `main/CMakeLists.txt` 将唯一 `CONFIG_BOARD_TYPE_*` 映射到 `BOARD_DIR`，同时选择字体、emoji、目标专属音频引擎和条件源文件；它只 glob `boards/${BOARD_DIR}` 下的 `.c/.cc`，并加入公共/core 源。
6. CMake 从该目录 `config.json` 读取 `BOARD_TYPE`/manufacturer，将 build.py 传入的 `BOARD_NAME` 与它们编译为宏；未传 name 时回退 type。
7. 具体 Board 文件用 `DECLARE_BOARD(Class)` 定义唯一全局 `create_board()`；`Board::GetInstance()` 用该函数懒创建单例。一次构建若选入零个或多个工厂都会失败，因此选中目录最终必须恰有一个有效工厂。
8. Board 基类对 Display/LED/Camera/Backlight/Battery 提供可空或 No-op 默认值；网络、AudioCodec、功耗等由具体 Board/网络基类实现。

板型身份参与 OTA，不能通过修改已有板子的引脚来复用身份；应新增独立 Board 或独立 release variant。

## 7. “笔划”功能的可能扩展点

### 7.1 建议的逻辑分层

可形成以下链路：

`现有语音上行 → 后端 ASR/意图识别 → 候选字命令 → StrokeController → LVGL 候选页 → 点击 → 选择通知/数据请求 → 笔画数据 → StrokeView 动画`

- **后端/协议入口**：现有 STT 只显示文本，不负责意图；候选生成更适合后端。设备可通过 MCP 工具或新增共享 `stroke` 消息接收候选。
- **控制器**：建议新增独立 `StrokeController`，负责请求 id、候选、当前字、动画阶段和取消，不把解析/动画继续堆入 `Application`；Application 只负责注册入口与主任务调度。
- **显示层**：候选按钮和动画应放在通用 LVGL/LCD 层，例如独立 `StrokeView`，而不是复制到每个 Board。具体 Board 仍只负责把触摸控制器注册为 LVGL pointer input。
- **数据层**：可从服务端按“选中字符”下发压缩路径，或放入 Assets。现有 `Assets::GetAssetData()` 能按名称 mmap 资源，且资源包可更新；但 4M/8M/16M 分区中的 assets 分别约 1M/2M/8M，完整汉字笔画库容量与授权尚未验证。

### 7.2 协议选择

两条可行路径：

1. **MCP 工具 + 异步通知**：注册如 `self.stroke.present_candidates`，后端/模型调用后立即返回“已展示”，用户点击再由 `Application::SendMcpMessage()` 发送 `notifications/stroke_selected`。优点是天然复用两种传输和能力发现；限制是当前 MCP Property 只支持 bool/int/string，结构化候选/路径需要扩展 schema，或暂以受限 JSON string 承载；工具调用不能阻塞等待点击。
2. **共享 Protocol 消息**：新增 `type:"stroke"`，由专用解析器处理 candidates/path/cancel，并在 `Protocol` 添加 selection 发送语义。若采用此路，必须同时验证 WebSocket 与 MQTT 控制面，并在两种 hello 的 `features` 中一致声明能力。

不建议直接复用当前 `custom` 消息：它受 Kconfig 控制，现实现只把 payload 打印到系统聊天文本，没有稳定交互语义。

### 7.3 UI、触摸与动画约束

- 仓库目前**没有通用触摸能力接口**：`Board`/`Display` 都不报告 touch capability；各触摸板在板级代码中注册 LVGL input，通用 UI 也尚无 clickable 业务控件。需要新增显式能力（如 `Board::HasTouchscreen()` 或 Display capability），避免把所有 LVGL/LCD 板误判为可用。
- MVP 应先限定“彩色 LVGL + 触摸”板型；无屏、OLED、Emote、无触摸 LCD 的降级行为需产品定义。
- 候选字符显示可复用现有 `glyph_push` 与动态字形缓存，避免基础字体缺字；笔画路径本身需要新的严格解析器。
- 动画应使用 LVGL timer/animation 或绘制任务增量更新，禁止在 Application 主循环/音频任务内延时循环；限制候选数、笔画数、每笔点数、总 payload 和内存，并校验 Unicode 单字、数值范围和版本。
- LVGL 点击回调不应直接改变 Application/Protocol；应复制最小数据后调用 `Application::Schedule()`。动画对象创建/销毁必须在 LVGL 锁或 LVGL 任务上下文。
- 对 MVP，笔划更像独立的 UI 子状态（Hidden/Candidates/Animating/Error），不宜直接扩充音频状态机；否则必须定义与 Listening/Speaking、唤醒打断、断网、OTA 的全部转移。现有 Idle 处理会清聊天区，因此笔划页最好是独立 modal/view，并明确何时关闭。
- 必须用 request/session id 拒绝过期点击；在断网、协议重置、新请求、返回/超时和 OTA 时停止 timer、释放路径与 UI 对象。

### 7.4 构建落点

- 新通用源加入 `main/CMakeLists.txt`，并以 Kconfig/组件条件保护；不要假定 PSRAM、触摸或大屏存在。
- 若只支持部分板型，`Kconfig.projbuild` 应表达依赖；如需在线构建可配置，还要同步 `scripts/build.py` 的语义选项暴露与校验。
- 若用本地笔画资源，需要同步 `scripts/build_default_assets.py`/Assets index 和各分区容量策略，而不是手工修改生成的资源文件。

## 8. 关键文件索引

- 产品与规则：`README.md`、`AGENTS.md`
- 入口/编排：`main/main.cc`、`main/application.h`、`main/application.cc`
- 状态：`main/device_state.h`、`main/device_state_machine.*`
- Board 工厂：`main/boards/common/board.*`
- 网络基类：`main/boards/common/wifi_board.*`、`ml307_board.*`、`nt26_board.*`、`dual_network_board.*`
- 音频：`main/audio/README.md`、`main/audio/audio_service.*`、`main/audio/engines/`
- 协议：`main/protocols/protocol.*`、`websocket_protocol.*`、`mqtt_protocol.*`
- UI：`main/display/display.*`、`lvgl_display/`、`lcd_display.*`、`oled_display.*`、`emote_display.*`
- 字形：`main/protocols/text_glyph_payload.*`、`main/display/lvgl_display/dynamic_glyph_cache.*`
- MCP：`main/mcp_server.*`、`docs/mcp-protocol.md`
- 激活/资源：`main/ota.*`、`main/assets.*`、`main/settings.*`
- 选择链：`main/Kconfig.projbuild`、`main/CMakeLists.txt`、`scripts/build.py`、各 Board 的 `config.json`/`config.h`
- 示例触摸板：`main/boards/waveshare/esp32-s3-touch-lcd-4b/`

## 9. 不确定项与需确认事项

1. 首批支持的具体 Board、最低分辨率、横竖屏和是否必须电容触摸。
2. 候选由 LLM、词典还是同音字服务生成；候选数量、排序和置信度规则。
3. 笔画数据来源、格式、字体/字形标准、授权、在线/离线要求和完整库大小。
4. 采用 MCP 还是新 Protocol 消息；后端是否可同步改造和灰度发布。
5. 用户点击时 TTS 是否仍播放、是否暂停监听、是否允许唤醒/按键打断、何时退出笔划页。
6. 无触摸/无屏/OLED/Emote 的产品降级方案。
7. 动画的速度、逐笔高亮、描摹轮廓、重播/上一步/返回等验收标准。
8. `FatalError` 当前不可达及协议文档状态图与代码不一致是否需要另行治理；这不是实现笔划的前置条件，但扩展状态机时不能照搬文档。

## 10. 验证说明

本次为严格只读分析，未修改任何文件。仅使用文件读取、查找和内容检索核对实现；未执行 shell、构建、单元测试或硬件测试。若进入实现阶段，至少应运行 `scripts/tests`，构建代表性的 S3/C6/P4 触摸板与无触摸/OLED/无屏/Emote 路径，并分别验证 WebSocket、MQTT/UDP、真实触摸坐标/旋转、动画帧率、堆内存和对话打断。

- `Application` 是唯一顶层运行编排器：外部回调以事件位或 `Schedule()` 汇入永久主循环，状态变化通过 `DeviceStateMachine` 约束。
- 一次构建通过 Kconfig/CMake 只 glob 一个 `BOARD_DIR`；该目录中的 `DECLARE_BOARD` 定义 `create_board()`，供 `Board::GetInstance()` 懒创建唯一具体 Board。
- Board 发布身份分为 config.json 顶层 `type` 与 build `name`；build.py 将 name 作为 `BOARD_NAME`，身份进入系统信息与 OTA，具有兼容性意义。
- 音频管线使用独立输入、输出、Opus 任务和有界队列；S3/P4/S31 选 AFE 引擎，其余目标选 Lite 引擎。
- WebSocket 与 MQTT/UDP 共享 `Protocol` JSON 语义；新增共享业务消息必须同时维护和验证两个传输路径。
- 当前 MCP 工具调用会被调度到 Application 主任务并立即形成响应；其属性模型只支持 bool、int、string，不原生支持候选/路径数组。
- 当前通用 `Board`/`Display` 接口没有触摸能力查询；触摸控制器由各 Board 独立注册到 LVGL，通用显示 UI 中没有现成的候选点击业务控件。
- `main` 中未发现现有“笔划/笔画/stroke/hanzi/candidate”功能实现；现有动态字形推送只解决字符显示，不提供笔顺路径。
- 实际状态机没有进入 `FatalError` 的合法边，且 AudioTesting 只能从 WifiConfiguring 进入；协议文档中的部分示意边与代码不一致。
- 资源分区随常见 4M/8M/16M 布局约为 1M/2M/8M，能 mmap/在线替换资源，但完整汉字笔画库能否容纳尚无证据。

<!-- pi-squad:3c14e0217b9d20192c1535b68c177cff8aa9831b80f15b1d666e54b52806bd64 -->
## 2026-09-12T05:00:39.791Z — no-cloud-feasibility

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

- 当前仓库没有 stroke/StrokeOrder 固件实现；三份 stroke ADR 已存在，但 `main/application.cc` 仍将未知 JSON 类型仅记录为警告，现有协议文档也未定义 `type:"stroke"`。
- WebSocket 与 MQTT 控制面都能把非 hello JSON交给同一回调，但 `Protocol` 没有 stroke 发送语义；通用承载能力不等于服务端已实现 stroke 业务。
- 当前 WebSocket/MQTT hello 只公布 MCP、AEC、glyph push 等能力，两个 `ParseServerHello` 都不读取服务端 features；双方确认 `stroke_order` 尚未实现。
- 设备 MCP Server 的输入属性类型只有 bool、int、string；工具调用在 Application 主任务执行并在回调返回后响应，不适合阻塞等待异步用户选择。
- Assets 支持 mmap 后按名读取文件，也支持默认 extra files 或自定义 assets 烧录；CoreS3 采用默认 16 MB 配置，对应 v2 分区表中的 8 MB assets 分区。
- CoreS3 的 FT6336 当前由 20 ms 定时器轮询，释放后的短触直接调用 `ToggleChatState()`，尚未注册 LVGL pointer input。
- 当前离线语音实现聚焦唤醒词和固定命令；普通 STT、LLM 与通用 TTS 均经远端协议到达设备，仓库中没有任意汉字的本地 ASR/TTS 实现。
- 默认 OTA 地址和运行时传输配置指向仓库外的服务；由于后端代码不在仓库内，不能从设备端代码推断线上服务支持未定义消息或任意新 MCP 业务。

<!-- pi-squad:9a8bf9e1f66a5df0ad579f5401fd9a26c503e8e0a1237876494d7a42a1bf8da6 -->
## 2026-09-12T05:34:15.895Z — stroke-local-foundation-revision

已完成 `stroke-local-foundation` 的原位修订，并将完整交付报告写入 `/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/4bc75096-f914-413c-8ac5-0ad9df82b84c/stroke-local-foundation-revision.md`。

修复了 Tester 的 B1/B2、M1–M5 及列出的 minor：StrokeView 改为私有 byte view + 显式 little-endian 点访问器；SOB1 v1 header 原 reserved 字段改为 index CRC；新增实际编译运行的 C++ harness、独立手写 golden 与 CRC known answer；provenance 强制完整 commit、clean checkout、origin、HEAD tracked blob 核验、逐文件/聚合 SHA-256；charset/path/pack/NaN/Infinity 边界收紧；引入固定真实 upstream commit 的 一、人、口 三字 smoke corpus、完整 APL 和 notice。

验证均成功：专项 24/24、全 host suite 91/91、独立 C++ harness PASS（UBSan，覆盖未对齐输入与损坏/溢出/保状态）、ESP-IDF xtensa parser syntax compile PASS、ESP-IDF v6.0.2 下隔离 `m5stack-core-s3` + `CONFIG_STROKE_ORDER_LOCAL=y` 全量构建 PASS，生成 2,859,856-byte `xiaozhi.bin`。clang-format 19.1.7 格式化及 dry-run PASS。未实现 UI、触摸或协议；未 commit/stage/reset/clean；现有 Stick-S3、emoji、CMake、Assets、`.squad` 等未提交工作仍保留。

- SOB1 v1 当前使用 32-byte header 的 offset 28 字段存放完整 index byte range 的 CRC-32/ISO-HDLC；单 record CRC 仍用于按需加载校验。
- `StrokeOrderStore::StrokeView` 不再暴露序列化区 `Point*`；点坐标通过 `GetOutlinePoint`/`GetMedianPoint` 显式按 little-endian 解码，任意未对齐 blob 已在 UBSan host harness 中验证。
- converter 现在只接受完整 40-hex commit，并离线核对本地 checkout HEAD、clean 状态、origin，以及每个选中文件在 HEAD 中的原始 bytes；manifest 记录逐文件 SHA-256 和确定性 aggregate SHA-256。
- 真实 smoke corpus 固定到 chanind/hanzi-writer-data commit `68d10a4b21150cae5e1ebbd223eed289cf32d90c`，仅含 一、人、口；JSON 与 upstream byte-identical，完整 APL SHA-256 为 `5590533436c70f10f2f524ee61456238c290175c6662fbe1c700b5f038a6d328`。
- 本机 `/usr/bin/clang++` 默认缺失可发现的 libc++ include path；加入 `$(xcrun --show-sdk-path)/usr/include/c++/v1` 后可成功构建并运行 host harness。
- 本机已有 ESP-IDF checkout 为 v6.0.2；隔离 `/tmp` sdkconfig/build 确认 `CONFIG_IDF_TARGET="esp32s3"`、`CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y` 并完成固件构建。
- 仓库 `.clang-format` 需要 clang-format 19 语法；已有 16.0.6 不支持 `AlwaysBreakAfterReturnType: ExceptShortType`，临时安装的 19.1.7 可正确格式化和检查。

<!-- pi-squad:bb4f492d339b892796452cfb27f8134234d605e8cac2c989f19ac081d1e74fe9 -->
## 2026-09-12T07:15:17.093Z — stroke-ui-cores3-revision

已原地完成 `stroke-ui-cores3` 修订并将完整报告写入 `/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/44bc0a4c-7b74-431c-a7c3-062043ba21b1/stroke-ui-cores3-revision.md`。B1/B2、M1–M6、m1 已逐项修复，m2 也已修复：controller 改为最多 1 MiB 的 owned blob；Assets 在 munmap 前同步 suspend/unbind、Apply 后无条件 rebind；LVGL 点击改为 LVGL task 内直接串行执行并移除 generation/Schedule TOCTOU；引入生产 lifecycle gate；逐笔改用显式单调毫秒；CoreS3 改为单个 packed atomic touch snapshot 并抽取 sequence tracker；静态状态暂停/删除 timer；power-save、DeviceState、alert、shutdown 和外部 delete 均取消并正确 gate UI；四个资产产物全部声明为 CMake OUTPUT，打包使用 staging + 原子目录发布；DisplayLockGuard 记录持锁状态、仅持锁才 Unlock，锁失败 fail-fast。未实现远端 STT/type=stroke，未 commit/stage/reset/clean，原有 Stick-S3/emoji/.squad 等工作仍保留。

验证：clang-format 19.1.7 dry-run、git diff --check、Python py_compile 均通过；专项 29/29、全 host 96/96 通过；实际删除 NOTICE 后增量构建触发重打包并恢复；ESP-IDF v6.0.2 在全新隔离目录 `/tmp/xiaozhi-stroke-ui-cores3-final-build-44bc0a4c` 以 `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y` 完成 2212-step 全量构建。最终 `xiaozhi.bin` 2,880,480 bytes，app 分区剩余 1,248,288 bytes（30%）。bin/elf/map/flash_args/generated_assets.bin 均保留在该目录。未做真机烧录、触摸/帧率/heap/音频长稳验证。

- `StrokeOrderController` 现在持有并验证最多 1 MiB 的 SOB1 私有副本；渲染路径不再依赖 Assets mmap 生命周期。
- 资产更新生产顺序已固定为 `SuspendAssets -> entry/timer/cache/controller 清理 -> munmap -> remap/Apply -> 无条件 GetAssetData/Bind -> gate 重评估`；Download 在 suspend 失败时拒绝 unmap。
- StrokeOrder LVGL 点击不再进入 Application::Schedule；点击与外部 lifecycle 操作统一由 LVGL/display lock 串行，锁序为 display/LVGL lock 后 controller mutex。
- CoreS3 touch 通过单个 `atomic<uint32_t>` 发布 pressed/x/y coherent snapshot，20 ms I2C poll 与 LVGL read 之间不存在分离坐标撕裂。
- `StepForward(uint64_t monotonic_ms)` 使用显式生产单调时间和独立有效位，时间 0 不再是哨兵；Paused 超过 120 ms 可继续逐笔。
- Paused/Completed timer 会暂停，Error/候选/退出/delete 会删除 timer；Resume/Replay 会恢复或重建。
- entry 只在资产有效、pointer 存在、surface 有效、设备 Idle 且未睡眠/中断时显示；所有非 Idle 状态均取消 overlay。
- CMake 的 stroke_order.bin、manifest、APL、NOTICE 均为声明输出；删除 NOTICE 的真实增量构建已证明 Ninja 会重新打包。
- 最终可烧录目录为 `/tmp/xiaozhi-stroke-ui-cores3-final-build-44bc0a4c`，其中 `xiaozhi.bin` SHA-256 为 `33c7c2778d062a83b9b9418d488f52bac575a2860c1582bc647704623b6fd981`。
- 当前实现仍是 一/人/口 本地 smoke demo；没有远端 STT 或 type=stroke，且尚无 CoreS3 真机验证。

<!-- pi-squad:41eff5e2b683f028545e3451e584980e52e112a2acf1318e560b1a5a53f665d2 -->
## 2026-09-12T09:28:23.445Z — stroke-stt-local-candidates-finalize

已完成 `stroke-stt-local-candidates` 同协议终态收尾，完整报告写入 `/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/34cb5385-b3da-4234-bf3e-094d696f186b/stroke-stt-local-candidates-finalize.md`。

静态复核结果：Tester 的 B1–B3、M1–M4 与两项代码 minor 均有 production code 闭环。`StrokeRoundCoordinator` 以 mutex 统一管理 generation/phase/route epoch/channel identity；监听 generation 贯穿 start、blocking open、bind 与启麦；STT/TTS/LLM/audio 按 incoming session ID 路由，旧 snapshot/旧 channel 输出丢弃，新 normal channel 可通过；所有主要取消源汇入 main-task abort；候选页有 X 与 60 秒超时；stroke STT 在网络线程先有界探测和 UTF-8 校验；`ResetStreamingState()` 清 send/decode/playback 等队列并以 capture/playback generation 阻止 in-flight 工作回灌。fake integration harness 链接真实 coordinator，覆盖旧 STT→取消→新 session、stale continuation、统一 abort、旧输出丢弃、新普通会话通过、15s/60s timeout 和 100 次 identity 生命周期。

验证：保留 `/tmp/stroke-revision-host.log` 显示 98/98、OK；本轮专项 `scripts.tests.test_stroke_order_ui` 7/7、OK。`git diff --check` PASS；clang-format 19.1.7 对本 artifact 的变更行以及新增 StrokeOrder/harness C++ 文件 PASS。为避免扩大 diff，没有整文件格式化历史文件；全工作区整文件检查仍会报告 8 个含既有风格债或无关 Stick-S3 工作的文件。本轮手工缩减了 CoreS3 中明显的纯格式 churn。

在既有 build dir 上完成非 clean 增量复验：ESP-IDF v6.0.2、CoreS3、`Project build complete`。当前 `xiaozhi.bin` 为 2,899,296 bytes，SHA-256 `418fef0b37ba6f9ead414737287c0c41e29648d4304c95ecc265f3458439eec7`；app partition 4,128,768 bytes，余 1,229,472 bytes（29.78%）。报告中记录了 bin/elf/map/flash_args/generated_assets 的完整路径、大小、SHA-256、sdkconfig 与 flash offsets。

本轮未 commit、stage、reset 或 clean；`.squad`、Stick-S3、emoji、分区及其他用户未提交工作均保留。残余风险仅限尚未完成的 CoreS3 真机触摸/音频/长稳和真实 WebSocket/MQTT 服务端验证、MQTT 极端延迟 hello 缺少客户端 nonce 的服务端时序假设，以及当前仅三字 smoke corpus。

- `StrokeRoundCoordinator` 是当前本地笔划语音轮的传输无关代际闸门；route snapshot 同时携带 route epoch、generation 和有界 session ID，并在 main 执行前复核。
- WebSocket 以每个 socket 的 identity 与 channel generation 标记音频/关闭事件；MQTT/UDP 在 hello 后把不可变 session ID 捕获到 UDP callback，并对延迟 goodbye 再核对当前 ID。
- 当前 stroke 路由启用后采用 fail-closed：active stroke 收到缺失/非法 session ID 会终止该轮；retired stroke ID 的 STT/TTS/LLM/audio 均丢弃；新普通 channel 必须绑定不同的有效 ID才能通过。
- `AudioService::ResetStreamingState()` 会停止 voice processing、清 encode/send/decode/playback/testing/timestamp 队列，并用 capture/playback generation 阻止已取出的旧工作重新入队。
- 当前构建配置为 ESP-IDF v6.0.2、esp32s3、M5Stack CoreS3、16MB flash、`partitions/v2/16m.csv`、`CONFIG_STROKE_ORDER_LOCAL=y`、quad 80MHz PSRAM。
- 当前可烧录 app 位于 `/private/tmp/xiaozhi-stroke-stt-local-candidates-revision-build/xiaozhi.bin`，大小 2,899,296 bytes，SHA-256 为 `418fef0b37ba6f9ead414737287c0c41e29648d4304c95ecc265f3458439eec7`。
- 现有本地笔顺资产仍仅含 `一/人/口` 三字 smoke corpus，homophone provider 为 no-op，尚不是约 500 字发布资源。

<!-- pi-squad:3e8bca9a5d129717e90085eb0f27238b62698b25a5ec760aa8e2ae6c47f80a26 -->
## 2026-09-12T10:59:14.826Z — stroke-final-core-s3-smoke

已完成最终 `stroke-final-core-s3-smoke` artifact，未修改功能源码，未 commit/stage/reset/clean。报告已写入 `/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/b39f7e68-7994-4eee-9178-edc47124f997/stroke-final-core-s3-smoke.md`。

验证结果：
- `python3 -m unittest discover -s scripts/tests -v`：100/100 PASS。
- `git diff --check`：构建前后均 PASS。
- clang-format 19.1.7：既定 artifact scope（tracked 改动行 + 全部新增 StrokeOrder/audio helper/host harness）PASS。
- ESP-IDF v6.0.2：在全新仓库外目录 `/private/tmp/xiaozhi-stroke-final-core-s3-build` 完成 2213-step `m5stack-core-s3` / `esp32s3` / 16MB / `CONFIG_STROKE_ORDER_LOCAL=y` build；真实编译 Application、AudioService、WebSocket、MQTT、CoreS3 和全部 StrokeOrder `.cc`。
- `generated_assets.bin` 确认包含一/人/口三字 smoke、APL、NOTICE 和数据 manifest。
- `idf.py merge-bin` 成功；merged 镜像从 `0x0` 烧录，大小 10,067,156 bytes，SHA-256 `ee74246d94aacbd0645d6daedc39bd461547a99dd7d8e68d3604f06764a9da95`。esptool 验证 ESP32-S3、DIO、80m、16MB、checksum/hash valid；各 flash offset 与多文件产物逐字节一致。
- app 大小 2,903,072 bytes，`0x3f0000` app 分区余量 1,225,696 bytes（29.69%）。
- 包目录和 ZIP 均已校验；解压后 `SHA256SUMS` 全部通过且与原目录无差异。包内无 sdkconfig、credential/secret、NVS key 文件或临时日志。

产物：
- `/Users/mandyw/git/xiaozhi-esp32/dist/m5stack-core-s3-stroke-smoke/`
- `/Users/mandyw/git/xiaozhi-esp32/dist/m5stack-core-s3-stroke-smoke.zip`
- ZIP：21,509,614 bytes；SHA-256 `f70fe2a62fe72192838f87baa978dff9b244ec5d300581468cb466b7ba682f39`

仓库根 `sdkconfig` 和既有 `build/` 的构建前后哈希未变，worktree status fingerprint 也未变；`dist/` 由仓库 `.gitignore` 忽略。未进行真机烧录或硬件验证，README 与 manifest 已明确标注。

- 当前三字 CoreS3 smoke 的 flash layout 为：bootloader `0x0`、partition table `0x8000`、OTA data `0xd000`、app `0x20000`、assets `0x800000`；merged raw image 从 `0x0` 写入。
- 当前 clean build 的 `xiaozhi.bin` 为 2,903,072 bytes（`0x2c4c20`），最小 app partition 为 4,128,768 bytes（`0x3f0000`），剩余 1,225,696 bytes（29.69%）。
- 当前 `generated_assets.bin` 为 1,678,548 bytes，构建日志明确纳入 `stroke_order.bin`、`stroke_order.manifest.json`、`ARPHICPL.TXT` 与 `NOTICE.md`；corpus 严格为一/人/口，SOB1 SHA-256 为 `9750f7ad01958f48df78102e8facd5183b58646f4350084aa05c03fba18b225f`。
- ESP-IDF v6.0.2 / esptool v5.3.1 对 bootloader、app 和 merged 镜像报告 ESP32-S3、16MB、DIO、80m，checksum 与 validation hash 有效。
- ZIP 解压后的 16 个文件与目录产物逐项一致；APL、NOTICE、manifest、app、assets、bootloader、partition、OTA、merged、ELF、map 和 flash_args 均在包内。
- 构建仍有 managed component 的既有 Kconfig `default False/default 0` notification 和第三方 legacy I2C EOL pragma，但无编译、链接或分区失败。
- 本 artifact 来自 base commit `8836458c95d52b7e1335837315ec4b000e581e53` 上的 dirty worktree；该 commit 单独不能复现最终二进制。
- 成功构建和主机测试不构成 CoreS3 真机验证；触摸、真实 STT/session routing、音频、FPS、heap/timer/WDT 长稳仍未验证。

<!-- pi-squad:245303cdcd63281bc0e6f5e09f73af71155e3647eeedefc24e8bc35f7e8b9d9c -->
## 2026-09-12T11:36:43.572Z — stroke-500-data-plan

# 笔划：从三字 smoke 升级到约 500 个常用简体字的数据实施计划

## 结论

技术上可沿用现有 SOB1，不必先改格式：当前上限是 1024 字、单字 16 KiB、整库 1 MiB，500 字位于字符数上限内。真正的前置缺口不是解析器，而是**可审计的 500 字选择文件、固定上游的实测覆盖率、拼音/常用度旁表、容量实测和逐字准确性审查**。当前仓库只能证明固定 commit 上“一/人/口”三字可转换；不能据此宣称 500 字覆盖。

## 1. 500 字清单的可审计来源与固定版本

### 1.1 推荐的规范来源与确定规则

- 规范源固定为国务院公布的《通用规范汉字表》，文件身份固定为 **国发〔2013〕23号，2013-06-05**。
- 目标集合固定为其**一级字表编号 0001–0500**，共 500 个字符；一级字表顺序同时作为本地“固定常用度”顺序。它是稳定的产品选择规则，不使用会变化的网页热榜，也不宣称是 ASR 置信度排名。
- 实施时必须从政府官方发布件取得原始 PDF/附件，保存来源 URL、取得日期、原文件名、字节数和 SHA-256。当前仓库没有该快照或哈希，本计划不编造哈希。
- 由两人独立核对编号、字符和码点；OCR 只能辅助，不得作为权威文本。差异必须回看官方版面解决。
- 首版不静默“跳过失败字并从 0501 以后补齐”。编号 0001–0500 是固定基线；若上游缺字或转换失败，应阻断阶段验收并记录原因。这样版本之间不会悄悄改变“500 字”的含义。

### 1.2 应形成的审计产物

1. `selection-500.csv`：`rank, official_id, character, codepoint, official_page, form_note`；500 行，官方顺序。
2. `charset-500.txt`：从 CSV 机械生成，一字一行，仅作为 converter 输入。
3. `source-lock.json`：官方文件身份/SHA-256、hanzi-writer-data repo/commit、生成脚本所在的干净 xiaozhi commit、脚本 SHA-256、转换参数。
4. `coverage-500.json`：逐字列出 source path/hash、存在性、解析结果、大小、笔画数、审查状态和失败原因。
5. `SHA256SUMS`：绑定以上文件、SOB1、manifest、许可证和 NOTICE。

现有 converter 会按 Unicode 码点排序写 SOB1 和 manifest，因此**会丢失官方频序**；`selection-500.csv`/紧凑 rank 旁表必须单独保留，不能从 SOB1 index 顺序反推常用度。

## 2. 与《通用规范汉字表》简体范围的关系

- 设备 allow-list 只接受 `selection-500.csv` 中的精确 Unicode 标量；STT 返回繁体、异体或其他字时不做 OpenCC 式自动替换，按“不支持/请重说”处理。
- 500 字全部来自一级字表主表，因而属于大陆现代通用规范用字范围；但“主表成员”不等于“每个字都是由繁体简化而来”。其中会有传承字。产品文案宜称“通用规范汉字（简体中文范围）”，不要声称 500 个都是简化字形。
- 当前 converter 只检查 U+4E00..U+9FFF、UTF-8 和结构限制，**不会检查是否属于《通用规范汉字表》或是否为简体/异体**。规范范围校验必须在 charset 生成/审计层完成。
- Unicode 码点一致也不能证明轮廓采用大陆规范字形；hanzi-writer-data 的轮廓与笔顺仍需逐字审查。
- 《通用规范汉字表》解决“收哪些规范字/字形范围”，不等于逐字笔顺认证。商用笔顺核对需另用经法务和语言专家确认的大陆笔顺规范（例如经确认版本的《现代汉语通用字笔顺规范》或其现行替代标准）。

## 3. hanzi-writer-data 固定 commit 覆盖率

### 3.1 固定版本

沿用已批准且当前 smoke 已锁定的：

- repo：`https://github.com/chanind/hanzi-writer-data`
- commit：`68d10a4b21150cae5e1ebbd223eed289cf32d90c`
- graphics upstream：`skishore/makemeahanzi`
- graphics license：Arphic Public License

`scripts/convert_stroke_order.py` 已要求 40 位 commit 等于 HEAD、origin 匹配、工作区完全干净，并逐个验证所选 JSON 是 HEAD 中的 tracked blob；manifest 会记录每文件 SHA-256 和聚合摘要。500 字生成必须走这个入口，而不是把 `package_stroke_order_smoke.py` 扩成 500 个硬编码 fixture。

### 3.2 当前可证明的覆盖

- 仓库只包含 `scripts/tests/fixtures/stroke_order/upstream_smoke/data/` 下“一/人/口”三个上游 JSON，并固定了三者 SHA-256、聚合 SHA-256、完整 APL 和 NOTICE。
- `dist/.../stroke_order.bin` 也是三字、2668 bytes。
- 仓库没有 500 字 charset、固定官方源、500 字 coverage report 或该 commit 的完整 checkout。

因此对拟定 500 集的覆盖率目前是**未测量/不可报告**；只能说三字 smoke 为 3/3。不能把“hanzi-writer-data 通常字很多”当作 500/500 证据。

### 3.3 覆盖率口径与门槛

覆盖预检应一次收集全部结果，而不是 converter 第一个错误即退出：

- `file_coverage = tracked JSON 存在数 / 500`
- `conversion_coverage = 通过 JSON、路径、轮廓、medians、点数和大小限制的字数 / 500`
- `prototype_review_coverage = 完成原型抽检/问题处置数 / 500`
- `commercial_review_coverage = 完成逐字双人审核并关闭差异数 / 500`

数据阶段门槛建议为 file 和 conversion **500/500**；任何缺失、未知 SVG 命令、轮廓/median 不匹配、点数超限或单字超 16 KiB均阻断，不自动猜测、降采样或换字。若整库超过 1 MiB，应停下来做新的格式/压缩 ADR 或调整明确的产品范围，不能只放宽设备上限。

## 4. 原型与商用准确性边界

### 原型可接受

- 明确标注“原型数据/非发布字库”，manifest 保持 `not_a_release_library: true`。
- 保留完整 APL、上游 repo/commit、每文件摘要及修改说明：曲线 flatten、坐标取整、Y 轴翻转、SOB1 序列化。
- 只使用 `strokes` 和 `medians`；不摄入 hanzi-writer-data/Make Me a Hanzi 的词典文本，避免把未评估的 LGPL/其他 copyleft 词典材料带入。
- 自动验证 500/500 可解析、可加载、闭合轮廓、坐标/笔画/点数有界；再对高笔画、弯折多、易混形、方向敏感、边界尺寸等风险样本做人工视觉审查。
- 不宣传“官方逐字认证”“适合教学考试”或“100% 符合大陆笔顺”。

### 商用发布前必须增加

- 法务确认 APL 对固件、衍生二进制、下载包、NOTICE 和源码/修改说明的义务；官方清单和拼音源也分别审查授权。
- 500/500 逐字核对规范字形、笔画数、笔顺、每笔方向/起止点、轮廓与 median 对应关系；至少一名合格语言/书写规范审校者加独立复核者签名。
- 保存逐字证据、问题单、修订来源和 reviewer/version；修改后的字不能继续冒充“verbatim upstream”。
- 商用 gate 通过前不得仅把 manifest 标志改成 release。若上游数据无法通过，应改用有明确商用授权且准确性可追责的数据源。

## 5. 生成、打包、Flash 与 PSRAM 预算

### 5.1 生成链

`官方表锁定件 → 双人核对 selection-500.csv → charset-500.txt → clean pinned HWD checkout → 现有 converter → Python validate_blob(load_all) → C++ 全字加载 harness → provenance/coverage/package`。

转换参数继续固定为 SOB1 v1、0–1024、Y-down、de Casteljau tolerance 0.25/max depth 8。输出应先在 staging 完整验证后原子发布。普通固件构建不得联网；建议提交一个经审查的版本化 500 原型包（SOB1、manifest、locks、APL、NOTICE、checksums），CMake 只验证并装包。不要 vendor `all.json` 或整个第三方 checkout。

语音候选还需独立紧凑旁表。当前生产实现使用 `StrokeOrderNullHomophoneProvider`，只显示识别出的受支持字；500 字 SOB1 本身不会自动产生同音候选。建议固定 Unicode 16.0.0 `Unihan_Readings.txt` 的 `kMandarin`（锁定 `Unihan.zip` SHA-256并审查 Unicode license），再以人工 override 处理多音字；输入读音域可覆盖一级字表 3500 字，输出候选严格限制在本地 500 字。常用度按官方编号，主字优先、去重、最多 6。不得改用 HWD 词典。实现时还要移除调用点对“不在 500 内的 STT 主字”提前拒绝的限制，否则无法用其拼音补足受支持同音字。

### 5.2 Flash 预算

当前已记录的 CoreS3 smoke 构建（不是本次新测试）：

- assets partition：8,388,608 B
- generated assets：1,678,548 B；余 6,710,060 B
- 三字 SOB1：2,668 B
- app：2,903,072 / 4,128,768 B；余 1,225,696 B

建议预算：

| 项目 | 目标 | 硬门槛 |
|---|---:|---:|
| `stroke_order.bin` | ≤ 768 KiB | ≤ 1 MiB（现有格式硬限制） |
| 500 文件 manifest | ≤ 128 KiB | 失败即调查异常膨胀 |
| 拼音/rank 旁表 | ≤ 64 KiB | 仅结构数据，不含词典释义 |
| charset/coverage/license/NOTICE | ≤ 32 KiB | 来源与许可证不得省略 |
| 整个增量包 | 约 ≤ 992 KiB | ≤ 1.22 MiB |
| 最终 `generated_assets.bin` | ≤ 3 MiB | 且必须小于实际 8 MiB partition |

按最坏包预算替换当前约 14 KiB smoke 附件，最终 assets 约不超过 2.95 MiB，仍约有 5.4 MiB 分区余量。必须以实际 pack 结果为准；`build_default_assets.py` 中虽有 `assets_size: 0x400000`，简化 pack 路径未见明确容量拒绝，因此 CI 必须自行比较产物与 partition CSV。extra-files 会按 basename 扁平复制，新增文件必须避免重名和 32-byte 名称截断。

### 5.3 PSRAM/heap 预算

`Assets` 先 mmap Flash，但 `StrokeOrderController::BindStore()` 会把**整份 SOB1 再复制到 `new[]`**；500 字不是零 RAM。CoreS3 构建记录只证明 `CONFIG_SPIRAM=y`，没有记录运行时可用 PSRAM。

- 持久数据目标：SOB1 ≤768 KiB，加读音旁表后 ≤832 KiB；硬上界约 1.06 MiB。
- 候选页最多缓存 6 个解码字；按单字 16 KiB 上限约 96 KiB。
- 200×200 RGB565 canvas 约 80 KiB；加当前字、vector/LVGL 开销，临时预算 256 KiB。
- 笔划功能总增量 heap 暂按目标 ≤1.1 MiB、硬上界约 1.35 MiB验收。

plain `new[]` 是否确实落到外部 RAM 必须真机用 heap caps/指针属性证明；不能只根据 SPIRAM 配置推断。实施时优先显式使用 PSRAM-capable allocator，并设内部 DRAM 回归门槛。若 bind 后 PSRAM 不足，不应改成内部 RAM 兜底；应隐藏入口并记录错误，或另做 mmap 生命周期/按字加载设计。

## 6. 分阶段实施与验收

### P0：来源冻结

- 取得并哈希官方发布件；双人生成/复核 0001–0500 CSV。
- 锁定 HWD commit、干净的 converter commit、APL/NOTICE、建议的 Unihan 版本。
- 验收：500 行、500 个唯一标量、全部 U+4E00..U+9FFF、官方编号连续；无自动繁简转换；所有 lock/checksum 可复算。

### P1：覆盖和容量探测

- 在 clean detached HWD checkout 做 500 字预检和正式转换，两次独立输出必须 byte-identical。
- 产出逐字 coverage、笔画数/点数/record size 分布、top-20 最大字和最终大小。
- 验收：file/conversion 500/500；Python 全量 load；SOB1 ≤768 KiB目标且绝不超过 1 MiB。

### P2：500 原型包与自动化

- 建立非 fixture 的版本化数据包；保留三字 fixture 继续做小而快的单元测试。
- 扩展 host test：selection 与官方锁一致、manifest 500 文件、C++ 依次 load 500 字、随机/全量 CRC、损坏记录隔离、构建输出提取后 SHA 对等、asset 大小门槛和 basename 冲突。
- 验收命令应包括 `python3 -m unittest scripts.tests.test_stroke_order -v`、UI focused suite 和 `python3 -m unittest discover -s scripts/tests -v`。

### P3：语音候选数据

- 生成 versioned 读音/rank 旁表和有界 provider；支持多音字、主字优先、去重、只返回 store 内字、最多 6。
- 验收：500 支持字至少有一个已审读音；典型同音/多音/繁体输入/不支持字测试；WebSocket STT 精确选字可用。MQTT 继续遵守当前无可靠 per-open identity 的安全降级，不伪造语音归属。

### P4：CoreS3 构建与真机

- ESP-IDF v6.0.2 从空的仓库外目录构建 `m5stack-core-s3`，确认 `CONFIG_STROKE_ORDER_LOCAL=y`、16 MiB Flash、`partitions/v2/16m.csv`、assets 中各文件 hash/size。
- 真机覆盖：首页入口、触摸、500 字直接选择测试入口、分层抽样语音、暂停/继续/逐笔/重播/返回/中断、断网后已加载播放。
- 性能门槛沿用已批准目标：约 30 FPS、持续不低于约 20 FPS、反馈 <100 ms；100 次会话无持续 heap 下降、timer 泄漏、WDT、audio underrun 或触摸失效。记录 bind 前后内部/外部 heap、最小余量及 blob 实际内存区域。

### P5：商用 gate

- 完成法务与 500/500 双人准确性审核；关闭全部差异；生成新的签名 manifest/审核报告。
- 在此之前产物名称、文档和 UI 均保持“原型/非官方认证”边界。

## 当前测试证据与缺口

本次严格只读，仅使用文件读取/检索；没有 shell，未运行测试、转换、构建或硬件验证。仓库中的 `dist/m5stack-core-s3-stroke-smoke/manifest.json` 记录过一次 ESP-IDF v6.0.2 构建和 100/100 host tests，但产物来自 dirty worktree且明确未做硬件验收，只能作为历史证据。

现有测试充分覆盖 SOB1 边界、CRC、provenance、三字真实 fixture、controller/lifecycle/session 和原子 smoke 打包；尚无 500 charset/coverage、规范表成员校验、500 全量 C++ load、拼音 provider、最终 asset 容量、PSRAM 归属或 500 真机长稳测试。上述缺口全部应作为升级验收项，而不是由三字通过结果外推。

- 当前 SOB1 的经 Python/C++ 同步锁定限制是：最多 1024 字、48 笔/字、256 outline 点/笔、64 median 点/笔、16 KiB/字和 1 MiB/文件；500 字不需要先扩大字符数上限。
- 当前固定真实上游证据只有 hanzi-writer-data commit `68d10a4b21150cae5e1ebbd223eed289cf32d90c` 上的一、人、口三文件及其 SHA-256；仓库没有 500 字 charset 或覆盖报告。
- `pack_characters()` 和 manifest 都按码点排序，因此不会保留 charset 的官方频序；常用度排名必须使用独立、受审计的旁表。
- 现有 converter 只校验 Basic CJK、结构、路径、数值和容量，不校验《通用规范汉字表》成员、简繁属性或大陆规范字形。
- 当前语音候选生产路径使用 `StrokeOrderNullHomophoneProvider`；增加 500 字 SOB1 只扩大精确命中范围，不会自动生成同音候选。
- 当前 controller 会从 assets mmap 复制整份 `stroke_order.bin` 到自有 `new[]`；Store 本身不分配，但 500 字会形成接近 SOB1 大小的持久 heap 占用。
- 记录的三字 CoreS3 构建中 assets 为 1,678,548/8,388,608 bytes，SOB1 为 2,668 bytes；Flash 容量很充足，PSRAM 的实际可用量和分配归属尚无真机证据。
- 现有 `package_stroke_order_smoke.py` 是三字 fixture 专用且以硬编码摘要打包；500 字应使用 clean pinned checkout 的通用 converter 和独立版本化数据包。
- `build_default_assets.py` 会按 basename 扁平复制 extra-files，名称可能覆盖，且简化 pack 路径未见以 partition 大小做显式拒绝；500 包需要新增 CI 容量与冲突检查。
- 当前测试覆盖三字转换/解析/UI 生命周期，但没有 500 全量、规范范围、拼音数据、PSRAM 或真机容量/性能验证。

<!-- pi-squad:3b1c8f7e2096d91573bfa6da01a8e0fafc13977aba74aea1fc450f4d01b2aa39 -->
## 2026-09-12T12:42:13.275Z — stroke-500-assets-pinyin-revision

已完成 `stroke-500-assets-pinyin` 原地修订，完整报告已写入 `/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/e836592b-b14b-416f-af4e-e9eceef3402c/stroke-500-assets-pinyin-revision.md`。已关闭 B1、M1–M4 与 CSV minor：最终 14 文件包按映射后名称重建并严格验证 SHA256SUMS；transcription 固定 origin/完整 HEAD/tracked path/blob hash/clean/workspace bytes；C++/Python SPY1 同步 canonical payload 和双向 membership/rank 校验；corruption 测试重算 CRC 并真实覆盖 `TestOnlyBindView(buffer+1,size)`、offset/count/reserved/semantic/overlap/u32 overflow；NOTICE 增加 2026-09-12、converter/schema version 及法务边界；CSV 实际解析 codepoint。500 字未变，coverage 500/500；SOB1 1,046,788 bytes（余 1,788），SPY1 18,152 bytes。验证：专项 13/13、最终全 host 113/113、clang-format 19.1.7、py_compile、git diff --check 均通过；ESP-IDF v6.0.2 在全新仓库外 build 目录完成 CoreS3 `CONFIG_STROKE_ORDER_LOCAL=y` 2214-step 构建并在最终 packager 修改后重建 generated_assets。未烧录、未打 dist、未执行 commit/stage/reset/clean。

- 最终映射后包恰有 14 个文件；其 SHA256SUMS 含 13 个非自身条目，清单 SHA-256 为 dbba16e352851188de77545c45db9eaeb1577e7e4261d6ae95c7b3ab115fb70a，实际 `shasum -a 256 -c` 13/13 通过。
- 固定 transcription 为 origin `https://github.com/leonsilicon/table-of-general-standard-chinese-characters`、HEAD `f9786a82be6e1672bdc60f85760a9e4a3791d1f1`、tracked path `table-of-general-standard-chinese-characters.json`、blob SHA-256 `a9f0a21fb83a84dd695eaeec7b77743a6099ca559768c8275ae0c22c827917d0`。
- 重生成后 selection 仍为 500 个唯一 Basic CJK、编号 0001–0500；SOB1 SHA-256 `3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`，SPY1 SHA-256 `a97666f18fa8e5a5e586f774dac044c08ba26b40decb4644e12c6886a5cbc998`。
- SPY1 v1 的实现与文档现在要求 canonical 紧密 payload：char-order readings 后接 group-order members 至 EOF，并要求 char→group 与 group→char 双向引用及 rank 一致；C++ 校验没有新增动态分配。
- ESP-IDF v6.0.2 CoreS3 产物：xiaozhi.bin 2,908,800 bytes / SHA-256 `911bebfc91cefe3a1e4c7e0e5308d380eca90e119182924bfc6f1d16d367a479`；generated_assets.bin 2,877,806 bytes / `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`；app 分区余量 1,219,968 bytes。
- 与保留的相同 sdkconfig 三字构建相比，500 字 generated_assets.bin 精确增加 1,199,258 bytes；两份 sdkconfig SHA-256 均为 `4fe18823c9183f2b4065af37a2d2cec89499858cd95208fb91887dd03a8e441e`。
- 最终全 host 测试为 113/113 PASS，覆盖现有异步取消、音频 generation 与 round route 安全回归；本修订未修改 STT/session/transport 生产路径。
- 本轮没有真机烧录；PSRAM 分配失败、约 1.065 MiB owned-copy 的启动/碎片、100 次会话、触摸/FPS/WDT/音频以及真实 WebSocket/MQTT 行为仍需 CoreS3 验证。

<!-- pi-squad:7fb1cb1b93d3cc0a195fd23f5bc1e36f2b6eb3e82266b66a0e445be6f297bc27 -->
## 2026-09-12T13:12:11.092Z — stroke-final-core-s3-500

已完成最终 artifact `stroke-final-core-s3-500`，报告写入 `/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/2848bf38-4c29-42e5-abc4-56a59b3d4dc8/stroke-final-core-s3-500.md`。未修改功能源码，未 commit/stage/reset/clean，原三字 dist 包保留。完整 host tests 共 113 项，结果 OK（112 pass、1 expected conditional skip、0 failure/error）；clang-format 19.1.7 artifact scope 与 `git diff --check` 通过。ESP-IDF v6.0.2 在全新仓库外目录 `/private/tmp/xiaozhi-stroke-500-final-build` 完成 esp32s3/m5stack-core-s3/16MB/CONFIG_STROKE_ORDER_LOCAL=y 的 2214-step 构建，日志确认 Application、AudioService、WebSocket、MQTT、CoreS3、store/controller/coordinator/pinyin/view 均编译，并生成且嵌入 14 个 500 字资产文件。最终目录为 `dist/m5stack-core-s3-stroke-500-prototype/`，ZIP 为 `dist/m5stack-core-s3-stroke-500-prototype.zip`（23,929,854 bytes，SHA-256 `9fd6dfcb7e04ece3732381385ef4a4620b721f00806c3c8227216e0e60c86a5a`）。merged 镜像 11,266,414 bytes，SHA-256 `4a22047b840bd3459cb1ad4fa158567a1e3be2927afd23413df64eca09964fbb`；app SHA-256 `a49373c7a38329023b850a45e0c51426d60236dd5b0b8337e9498df38b52af62`；assets SHA-256 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`。esptool image-info、五段 offset byte compare、目录总 SHA256SUMS、ZIP CRC 与 ZIP 解压后总 SHA256SUMS 均通过。未烧录，等待独立 review。

- 当前 500 字 SOB1 为 1,046,788 bytes，SHA-256 `3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`，距 1 MiB 格式上限仅余 1,788 bytes。
- 当前 500 字 SPY1 为 18,152 bytes，SHA-256 `a97666f18fa8e5a5e586f774dac044c08ba26b40decb4644e12c6886a5cbc998`，距 64 KiB 格式上限余 47,384 bytes。
- CoreS3 500 字构建的 app 为 2,908,800 bytes，4,128,768-byte app 分区剩余 1,219,968 bytes（29.55%）；assets 为 2,877,806 bytes，8 MiB assets 分区剩余 5,510,802 bytes。
- 最终包内有 26 个普通文件；顶层 SHA256SUMS 覆盖除自身外全部 25 个文件，stroke_order 内层 SHA256SUMS 覆盖其余 13 个资产文件，目录和 ZIP 解压后均验证通过。
- WebSocket 笔划模式使用现有远端 STT，并以识别字优先、本地拼音及固定常用度补足最多 6 个同音候选；MQTT 因无可靠 per-open identity，仅显示本地 SPY1 TopRanked 候选且不启用语音选字。
- 本轮没有真机烧录；PSRAM 初始化、FT6336 坐标/旋转、SO/候选/动画、真实协议交互、目标帧率及 100 次会话稳定性仍需硬件验证。

<!-- pi-squad:79292e9fdf7a098450a6433a1577729e63f4c86bd7c3d1b60e96704b2842ed1b -->
## 2026-09-12T14:12:46.634Z — stroke-final-core-s3-500-capability-doc-fix

已完成 `stroke-final-core-s3-500` package 修订，且仅改动 dist 内 `README.zh-CN.md`、`manifest.json`、顶层 `SHA256SUMS` 和 ZIP。文档/manifest 现明确 `selected=websocket` 与初始化时 `correlated_open=1`/`stroke_voice=1` 是 `protocol_->Start()` 前的静态 transport capability，不能证明 connection、audio open、hello session 或 STT；真机验收必须点击 SO 后实际进入 Listening、收到带合法 `session_id` 的 STT 并显示候选。已删除错误字段 `stroke_voice_one_requires_correlated_websocket_open`，加入所要求的机器字段，并保留 WebSocket 优先、MQTT 本地降级、NVS 烧录边界及 500 字原型许可说明。新 ZIP 为 `dist/m5stack-core-s3-stroke-500-prototype.zip`，SHA-256 `6cb9ecfe8b3eadedb14a4cee3f90633a5df87c0bf199998184ac37ba2ee91a10`，大小 24,167,765 bytes。验证通过：顶层 25/25、内层 13/13、ZIP CRC、解压后 checksum 和递归 diff；README/manifest 外 23/23 条目 hash 不变。未重编、未烧录，未 commit/stage/reset/clean。完整报告已写入指定输出路径。

- 初始化日志 `selected=websocket`、`correlated_open=1`、`stroke_voice=1` 在 `protocol_->Start()` 前产生，只能证明静态 transport capability；现有静态日志不足以证明 WebSocket 动态语音选字链路成功。
- 该 package 的动态语音选字验收证据必须覆盖 SO 后实际进入 Listening、带本轮合法 `session_id` 的 STT，以及候选字显示。
- 修订后 ZIP SHA-256 为 `6cb9ecfe8b3eadedb14a4cee3f90633a5df87c0bf199998184ac37ba2ee91a10`，大小 24,167,765 bytes；解压内容与 dist 目录完全一致。
- README/manifest 之外的 23 个 package 文件 hash 逐项保持不变；其中 `xiaozhi.bin` 为 `f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a`，`generated_assets.bin` 为 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`。

<!-- pi-squad:800b4db67836a2233c0eb13f36a321b37ef5e87e47f3b038c3f2f28f0fc68533 -->
## 2026-09-12T14:59:58.409Z — stroke-voice-autostop-revision

已完成 stroke-voice-autostop 的 M1/M2 修订并写入报告：`/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/dc0a382f-5464-4566-a178-b03442c7a248/stroke-voice-autostop-revision.md`。

实现要点：`AudioService::EnableVoiceProcessing` 攩为可观察的 bool 结果；Stroke 启动严格按 fence→send→enable/running→MarkListeningStarted→Speak；stroke 和 generation-0 各失败路径均关闭 channel、reset streaming、回 Idle，普通/wake re-listen 恢复唤醒词；新增 production-used `listening_start_policy.h` seam；wake re-listen send 失败后不再执行 decoder/popup；保留既有 mode/session/fence/WS/MQTT/500 字行为。

验证：red 阶段 2 项定向测试中 production seam 约束按预期失败 1 项；green 阶段完整 host suite 118 项通过、1 项既有条件性 skip；clang-format 19.1.7 scoped dry-run 通过；`git diff --check` 通过；ESP-IDF v6.0.2 在仓库外全新副本以 CoreS3 + `CONFIG_STROKE_ORDER_LOCAL=y` clean build 成功。App 为 2,909,840 bytes，SHA-256 `c2adb51d0907af68d44026adbc901e3136f7629074c81afdd5379cadbf6eb4cd`；assets 为 2,877,806 bytes，SHA-256 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`。未 commit/stage/reset/clean，未 zip 发布、未烧录。

- `AudioService::Stop()` 会为唤醒输入任务设置 `AS_EVENT_AUDIO_PROCESSOR_RUNNING`，所以仅检查 event bit 会把已停止服务误判为 processor running；`IsAudioProcessorRunning()` 必须同时检查 `service_stopped_ == false`。
- 生产启动顺序现已由 `EvaluateListeningStartResult()` 这一 host-testable seam 与 `Application::StartListeningAudio()` 共同约束；非零 generation 失败映射为 stroke abort/fenced abandon，generation 0 失败映射为普通 Idle 恢复。
- 最终 host suite 实际结果为 118 tests passed、1 个需要外部 pinned transcription checkout 的既有测试 skipped。
- ESP-IDF v6.0.2 CoreS3 clean build 在 `/tmp/xiaozhi-stroke-autostop-idf602-20260912-230839` 成功，确认 `CONFIG_STROKE_ORDER_LOCAL=y`；app partition 仍有 0x129970 bytes（30%）余量。
- 底层 `AudioEngine::EnableVoiceProcessing()` 仍是 void；当前可观察成功边界是 AudioService 正在运行、engine 初始化成功、且 running event bit 已置位。真机麦克风/VAD 与真实网络竞态仍不能由 host/build 证明。

<!-- pi-squad:70bbbe819456be9fcc01a46a410d3bbf1910c70dba482969d6f0e4d81f9cf85e -->
## 2026-09-12T17:08:18.982Z — stroke-2000-sharded-library-retry

已在现有 partial 上原地完成 `stroke-2000-sharded-library`，完整报告写入 `/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/aa39ef10-4f7a-424f-b1c3-f9354028c267/stroke-2000-sharded-library-retry.md`。实现了严格 SCB1 Python/C++ 校验、2000 字与 8 shard 唯一映射、事务式 catalog+8 shards+SPY1 rebind、单一有界 shard lease、owned glyph cache、同步 suspend 失效、SPY1 2048 上限/63,618 B 与无静默截断、最小 runtime pack、闭合 hash/原子发布/容量门禁及相应文档测试。未 commit/stage/reset/clean，未烧录或生成 dist；Stick-S3、local emoji、`.squad` 与其他用户工作均保留。最终以固定输入重生成的 29 个 fixture 文件逐 SHA-256 完全一致；host 全量 127 tests 全通过且无 skip；clang-format 19.1.7 dry-run、py_compile、git diff --check 均通过；仓库外 ESP-IDF 6.0.2 CoreS3 clean build 成功。固件 `xiaozhi.bin` 2,914,800 B（SHA-256 `48fbf45cddc2a87ecf7e38cb10b21cd1326b88cdeca37a06371c79461c75b7d0`）；assets 7,568,207 B（SHA-256 `54453b5232408dfb8cf8dadbde9dd5d93a1bdb90f213df69d7ff075210783dd0`），8 MiB 分区余量 820,401 B，满足至少 256 KiB。

- 2000 字 corpus 恰为 8×250 个字符，SOB1 shard 总计 5,800,492 B，最大 shard 911,688 B；SCB1 为 24,352 B，SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`。
- SPY1 v1 实测 63,618 B，距 64 KiB 上限余 1,918 B，含 2000 characters、1047 groups、最大 group 29 members；Python/C++ character/group validator 上限均为 2048。
- Production rebind 只有在 SPY1、SCB1、全部 8 shards 及两者的 2000 个 codepoint/rank 映射全部一致时才就绪；缺失、损坏、交换或部分更新均 fail closed。
- Controller 的 shard source 采用 Acquire/Release lease；实际 UBSan C++ harness 观测 `max_active=1`、`max_bytes=911688`，生产路径不保留跨锁或跨 asset generation 的 StrokeView。
- Runtime stroke package 恰含 15 个文件：8 shards、catalog、pinyin、runtime manifest、APL、Unicode license、NOTICE、闭合 SHA256SUMS；selection/charset/coverage/source/per-shard manifests 不进入 runtime pack。
- ESP-IDF 6.0.2 外部 clean build 的完整 generated assets 为 7,568,207 B，8 MiB assets 分区余量 820,401 B；最终 image 内 15 个 stroke 文件的 size/hash 与 runtime package 一致。
- 当前工作树同时存在用户的 Stick-S3、emoji、`.squad` 和 8m partition 工作；本任务没有回滚或清理这些内容，并保留了共享 CMake/build_default_assets 中的既有 local emoji hunk。
