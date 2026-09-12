# Accepted decisions

Only accepted proposals and explicit user directives belong here.

<!-- pi-squad:f45e18e41e37444c75b7297de781c806fd80c6b28a398a90b2bf1663d3526bfc -->
## User directive — 2026-09-12T04:13:16.481Z

所有工程类成员（Lead、Frontend、Backend、Tester）执行任务时统一使用 grok-4.6 模型，thinking level 设为 high。

<!-- pi-squad:f96e76e93092ae529acc060854206728394c7b3bb2e7dc7f4207e630aa371f92 -->
## User directive — 2026-09-12T04:13:53.516Z

模型设置中的“engineer”专指角色名称为 engineer 的 Frontend、Backend、Tester；他们统一使用 grok-4.6，thinking level 为 high。Lead 与其他顾问成员维持默认模型。此条澄清取代先前把 Lead 也算入工程类成员的表述。

<!-- pi-squad:56dadcde0fe0f6655296d362c494f3f4c907aaf8c5031958acd824e669eeec2d -->
## User directive — 2026-09-12T04:23:15.316Z

笔划功能第一轮产品决策已由用户全部接受：功能名为“笔划”，领域术语统一为汉字、候选字、笔画、笔顺、笔顺演示，代码模块建议名 StrokeOrder；首期只做笔顺演示，不做临摹判定，支持自动播放、暂停/继续、重播、逐笔前进和返回候选页；可通过明确的笔划模式入口或服务端识别自然语言意图启动，不自动拦截普通单字对话；最多展示 6 个候选字，按 ASR 置信度与常用字频排序，小屏可降为 3 至 4 个，无可信候选时提示重说；首期仅支持具备受支持显示屏与点选输入能力的板卡，其他板卡不暴露入口，核心能力保持可选。

<!-- pi-squad:70e8bf7ace4a505bc8658089397e7dbc0982b839c550f6cbbf0dd93297d8f36e -->
## User directive — 2026-09-12T04:24:17.275Z

笔划功能第二轮产品决策已由用户全部接受：首期只支持《通用规范汉字表》中的简体汉字，繁体、生僻字和异体字不自动替换；笔划模式接受单字、某字、某怎么写、某词的某等表达，若解析出多个目标字则要求重说；点击候选字后立即进入演示，以返回候选页作为撤销；演示结束后停留在完成状态，可重播、逐笔查看、返回或退出，不自动监听下一个字；普通新对话可中断并退出，高优先级系统事件立即中断；已完整加载的数据可在断网后演示；候选列表只展示具备可靠笔顺数据的汉字，全部不可用时明确提示，不从字体轮廓猜测笔顺。

<!-- pi-squad:d9b55ceb658d09cf458884743c30d0240fb0b7d31e6bb5014af8a57b1d039176 -->
## User directive — 2026-09-12T04:26:06.054Z

Lead 和 Tester 执行任务时统一使用 gpt-5.6-sol 模型，thinking level 设为 xhigh。此设置优先于此前关于 Tester 使用 grok-4.6:high 的配置。

<!-- pi-squad:08deca76aa25b1775521e9f286a5df62c56e6640e72846286a4a25f60b5b4acc -->
## User directive — 2026-09-12T04:27:02.542Z

笔划功能第三轮产品决策已由用户全部接受：演示中以浅色完整字形作为参照，已完成笔画显示深色，当前笔画使用主题强调色动态绘制并显示起笔标记；首期固定显示田字格；默认使用中速，单笔时长按路径长度设上下限，笔画之间短暂停顿；开始演示时朗读一次汉字，动画过程不逐笔播报，朗读失败不阻止动画且支持静音；暂停、继续和重播必须幂等，逐笔操作设短防抖且不建立无界队列，返回和退出立即取消动画；被其他功能中断后不自动恢复，下次重新开始。

<!-- pi-squad:9ae539b0f7afdbabae4e4dbb7f7776f9d325c72c31197d55c04dcf991bd1dc1a -->
## User directive — 2026-09-12T04:33:53.613Z

笔划功能第四轮架构决策已由用户全部接受：首期笔顺数据在用户选字后由服务端按需下发，只在当前笔划会话内存中保存，不打包完整字库或持久化到 Flash；候选生成、去重、按 ASR 置信度和字频排序及数据可用性过滤由服务端负责，设备仅做字符合法性、去重和最多 6 个的有界验证；使用独立 StrokeOrderController 管理 Hidden、Candidates、Loading、Animating、Paused、Completed、Error 等 UI 子状态，不扩展 DeviceStateMachine；增加默认关闭的显式坐标输入能力，仅彩色 LVGL 且注册 pointer input 的板卡声明和公布支持；候选出现后停止监听并进入 Idle，但保留协议控制通道和独立页面，最长等待 60 秒，点击、唤醒词、按键、新会话、超时或高优先级事件按规则完成选择或取消。对应 ADR 为 docs/adr/0001-stroke-order-feature-boundaries.md。

<!-- pi-squad:770c7e27dff8627c5ceb9cee7c09b69989c01ad12c1f0d49f12e3423673950e1 -->
## User directive — 2026-09-12T04:36:32.834Z

笔划功能第五轮中 Q24-Q26 已由用户接受推荐：使用传输无关的 type=stroke 共享 Protocol 消息，并保持 WebSocket 与 MQTT 控制面语义一致，仅在能力成立时通过 hello 宣布 stroke_order；可靠笔顺数据必须有明确授权和版本并符合大陆规范，服务端保存来源与版本，测试 fixture 不得作为发布字库；点击候选后 100ms 内显示加载反馈，单次请求最长 8 秒，失败可手动重试或返回，不无限重试、不猜测，所有响应匹配当前 request/session ID，迟到响应丢弃，完整数据加载后可断网重播。Q27 用户选择 M5Stack Core2，但当前仓库没有该板卡实现，因此验收板决策需进一步确认。

<!-- pi-squad:69be3c93274ce4ff00585d312d1bba232e4027b6e63d8bdca4d5ad1e95ccc8db -->
## User directive — 2026-09-12T04:37:29.721Z

笔划功能首期真机验收基准改为仓库现有的 M5Stack CoreS3，构建身份为 m5stack-core-s3，路径 main/boards/m5stack/core-s3/。该板当前通过定时器轮询 FT6336 并把短触映射为 ToggleChatState，尚未注册 LVGL pointer input；笔划实现必须先完成坐标式 LVGL 输入接入，并确保非笔划场景的原有短触交互不回归。此前关于新增 M5Stack Core2 作为验收板的分支不再采用。

<!-- pi-squad:7a8fb1c57de23f91c81ca647c04a0ca3208fff0841b46d2c51e8ba63ba62be5e -->
## User directive — 2026-09-12T04:38:03.319Z

笔划功能第六轮决策已由用户接受：验收板 Q27 选择现有 M5Stack CoreS3；stroke 协议采用 server candidates → device select → server data 的按需阶段，cancel/error 均带 request ID；重复选择、重试和重复数据按 request/session ID 幂等，取消或被替代后的迟到响应丢弃；首版协议 version=1，未知 action/version、无效 UTF-8、缺失字段或超限内容只终止当前笔划请求，不关闭普通对话连接；只有设备与服务端都确认 stroke_order 能力时才开放入口，断线后仅允许继续播放已完整加载的数据。

<!-- pi-squad:a0f281f42ecd85485bffaf9cde8a9e380df14f256c108f80ccaf77a08c8b9192 -->
## User directive — 2026-09-12T04:57:22.592Z

笔划功能第七轮决策已由用户全部接受：单字数据使用受限且版本化的归一化矢量格式，每笔含封闭笔画轮廓和用于动态揭示的有序笔画中线，坐标为 0–1024 整数，不接受任意 SVG、脚本、外部资源、逐帧图片或字体轮廓推断；CoreS3 候选页采用最多 6 字的 2×3 大按钮网格，演示页约 200×200 田字格加右侧控制区，触摸目标不小于约 44×44，通用 StrokeOrderView 负责布局；笔划 Overlay 打开时独占触摸，关闭时未被控件消费的短触继续 ToggleChatState，每个触摸序列最多触发一次动作；CoreS3 目标约 30 FPS、持续下限约 20 FPS、点击反馈 100ms 内，100 次会话无持续 heap 下降、timer 泄漏、WDT、音频 underrun 或触摸失效。对应 ADR 为 docs/adr/0003-use-bounded-vector-stroke-data.md。

<!-- pi-squad:28e0ceb613a095f012b5a3488c949ecad42d8860893cf67c51f134c3e3c2e243 -->
## User directive — 2026-09-12T05:10:47.408Z

用户接受 Hanzi Writer Data 原型方案：原型从固定 commit 的 chanind/hanzi-writer-data（上游 skishore/makemeahanzi）选择小规模汉字子集，使用每字 strokes 轮廓与 medians 中线，在主机端转换为设备受限二进制格式并放入 CoreS3 Assets；保留数据来源、commit、Arphic Public License、修改说明和许可证文件，转换脚本及衍生数据按许可证要求处理；不得直接打包完整 all.json，也不得仅凭该项目自述就宣称已获官方逐字认证；正式商业发布前需完成授权与笔顺准确性审核。此决定限定为原型数据路线，未来完整联网版的 type=stroke 架构暂保留。

<!-- pi-squad:ebf85154c0626f83a694c6cdc6648a233d8a60944a7e7e3f65f10cb12184e4d0 -->
## User directive — 2026-09-12T05:11:42.091Z

用户明确要求开始实现笔划功能，并接受第九轮 Q41-Q45 的全部推荐以及后续未决问题默认采用协调者推荐：首期为 CoreS3 本地优先可用版本，未来保留 type=stroke 联网扩展；进入明确笔划模式后复用现有远端 STT，收到有效文本即停止本轮监听并由设备本地处理，不解析 LLM/TTS 回答；候选以 STT 识别字优先，并从本地受支持集合按拼音和固定常用度补足最多 6 个，不宣称 ASR 置信度排序；原型先收录约 500 个常用简体字；仅在 CoreS3 坐标触摸和有效本地字库存在时开放本地功能，新的语音选字仍要求普通远端 ASR 在线。用户授权后续问题按推荐方案继续。

<!-- pi-squad:4663771eadfc11150da7c69856a378411a98fe6be9e19cec6a37005432f1faab -->
## User directive — 2026-09-12T05:13:51.538Z

用户要求 Squad 持续迭代笔划功能，直到产出可烧录到 M5Stack CoreS3 的可用固件；后续未决问题默认采用协调者推荐。执行过程必须持续完成实现、独立审查、测试、CoreS3 构建和残余硬件风险报告，不因中间阶段完成而停止。

<!-- pi-squad:f80d052b32fc4e72f0bb1246d6449126495d423f4b678e3f4c9d972374ac578c -->
## User directive — 2026-09-12T11:36:20.789Z

用户完成了 M5Stack CoreS3 真机烧录反馈：设备上可见并可使用 SO 入口。用户要求继续扩展为约 500 个汉字的版本，并要求可通过语音选字。后续仍按协调者推荐推进；WebSocket 可使用现有远端 STT，MQTT 在没有可靠 per-open identity 前继续安全降级，不伪造语音会话归属。

<!-- pi-squad:b9864af037f6b580bc4a288c38f63f43d4bac77f34876bf70138dea95145d9ef -->
## User directive — 2026-09-12T13:45:20.016Z

500 字固件真机反馈：SO 可点击，但点击后直接显示本地候选，没有进入语音选字。当前代码 InitializeProtocol 在 OTA 同时有 MQTT/WebSocket 时优先 MQTT，而 MQTT stroke voice capability 明确为 false，因此该症状与 MQTT 本地降级路径一致。下一修复优先让启用本地笔划的构建在两种配置同时存在时选择 WebSocket，只有 MQTT 时继续安全降级。
