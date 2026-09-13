# Rai: project knowledge

做一个新的功能，叫“笔划”。用户说一个汉字的单字，找几个备选，用户点击屏幕选择。用动画演示正确的笔画顺序。

<!-- pi-squad:a35860ffc820bc4a8aba375a51986f33399aa120de849de82273e55b2db15b6b -->
## 2026-09-12T10:51:59.732Z — stroke-stt-async-close-fence-review

# stroke-stt-local-candidates 最终异步 fence 独立安全复审

## 结论

**APPROVED**。允许进入最终 CoreS3 三字（一/人/口）固件打包。

- **Blocker：0**
- **Major：0**
- **Minor：1**（仅回归测试断言强度，不是当前生产代码缺陷）

本次为只读复审：读取了真实工作区源码、测试 harness、最终作者报告和保留的执行日志；遵照任务要求，没有修改文件，也没有运行 shell 或自行重跑测试。

## 关键安全检查

### 1. WebSocket open-attempt 与确切 stroke generation：通过

- `main/protocols/websocket_protocol.cc:106-112` 在阻塞式 open 前通过原子 `channel_generation_` 预留非零 attempt ID。
- `main/application.cc:1424-1434` 先调用 `ReserveAudioChannelOpenAttempt()`，再由 `BindStrokeOpenAttempt(expected_generation, open_attempt_id)` 建立单槽的 attempt→stroke-generation 映射，最后才进入 `OpenAudioChannel(...)`。
- `main/protocols/websocket_protocol.cc:224-233` 的断线回调捕获创建该 socket 时的 `generation` 与独立 `WebsocketSessionIdentity`，并原样回传 `.open_attempt_id = generation`，没有读取回调发生时的当前 stroke token。
- `main/application.cc:663-687` 先用 `MatchStrokeOpenAttempt(info.open_attempt_id)` 精确匹配 opening/bound generation；correlated ID 不匹配时不会退化为当前 generation。匹配后在 transport callback 线程发布 `PublishCancelFence(closing_generation)`，之后才 `Schedule` main-task abort。
- opening 映射跨越 open→bind 窗口，并仅在 open 失败、abort、STT finish 等结束路径按 generation 清理，因此 close 发生在 open 返回到 `BindOpenedAudioChannel` 之间也能命中确切 generation。

### 2. open→bind、bind→start 的 fence 时序：通过

- open 阻塞返回后，`ContinueOpenAudioChannel` 在 bind 前重新检查 cancel fence、phase 与 pending abort；bind 自身也在 coordinator 锁下拒绝已 fenced generation。
- bind 后到 `STATE_CHANGED`/最终 start 之间发生 close 时，close callback 先发布 fence，再排入 scheduled abort。
- 真实 `Application::Run` 顺序为 `MAIN_EVENT_STATE_CHANGED`（`main/application.cc:266`）→ `MAIN_EVENT_STROKE_START`（`:289`）→ `MAIN_EVENT_SCHEDULE`（`:327`）。即使 state change 先消费，最终 start gate 也会先看到 source-side fence。
- harness 在 `scripts/tests/stroke_round_integration_harness.cc:530-575` 覆盖 close 后仅 fence+scheduled abort、先执行 final gate、后清理 scheduled abort，并验证 `start_listen == 0`、麦克风未启用及 replacement 仍生成新的非零 generation。

### 3. 其他异步取消源：通过

均在发布 event/schedule 之前 source-side fence，且 callback 线程没有直接执行新增 LVGL/state mutation：

- Network Scanning：`main/application.cc:158-167`
- Network Disconnected：`main/application.cc:189-193`
- Protocol `OnNetworkError`：`main/application.cc:611-616`
- `RequestStartStrokeRound` replacement：`main/application.cc:1064-1081`
- `ResetProtocol`：`main/application.cc:2066-2071`

这些入口最终都经 `stroke_listening_start_mutex_` 与 start 临界区串行；`PublishCurrentCancelFence()` 原子返回其实际命中的 generation，避免 fence 与随后 abort 命令之间重新读取“当前 token”。

### 4. 最终 StartListeningAudio gate：通过

- `main/application.cc:1784-1841` 在 `stroke_listening_start_mutex_` 下完成 phase/fence/pending-abort 检查与 `MarkListeningStarted`。
- 首次 gate 位于 `SendStartListening` 前；`SendStartListening` 后、`EnableVoiceProcessing(true)` 前在 `:1824-1831` 再次检查 fence，覆盖 `SendText` 同步报错重入 `OnNetworkError` 的情形。
- enable 后还有一次防御性 fence 检查（`:1833-1841`）。所有 Application fence publisher 使用同一递归互斥量；同步同线程错误可重入，其他线程则与整个 start/send/enable 临界区线性化。
- stale explicit generation 始终保留为非零 token；replacement 的 `IsLatestGeneration(expected_generation)` 仅允许旧 round 已被 final gate 清理时继续产生新的非零 generation，没有转换成 generation 0 ordinary listen。

### 5. MQTT/UDP 与普通会话：通过

- `main/protocols/mqtt_protocol.h:35-36` 仍为 `SupportsCorrelatedSessionOpen()==false`、`SupportsStrokeVoiceRouting()==false`。
- MQTT close 回传 `open_attempt_id = 0`，未伪造 correlated nonce；普通 `OpenAudioChannel` 仍接受默认 attempt 0。
- 本地 stroke 分支只建立三字候选，不打开音频 channel、不发送 listen/start；harness 同时验证普通 MQTT STT/normal channel 仍可通过，以及 stroke MQTT 只进入本地 Candidates。

### 6. 既有防线无回退：通过

- session routing：`StrokeRoundCoordinator` 继续用 session ID、generation、route epoch、retired identity 联合提交；missing/wrong/old response 在活动 stroke round 中 fail-closed 或 drop。integration harness 覆盖捕获后 replacement、重复 STT、retired/unknown close 与 100 轮身份寿命。
- AudioService generation：`ResetStreamingState()` 同时 bump capture/playback generation 并清队列；encode/decode 完成后在重新入队前检查 generation。专项 `audio_stream_generation_harness` 保持覆盖。
- bounded STT：`main/application.cc:808-856` 先将 session 限为 96 bytes，再以 `ProbeJsonString(..., StrokeOrderParse::kMaxBytes)` 限制 STT 为 96 bytes，随后做严格 UTF-8/码点解析；无界 `std::string` 分配不发生在 stroke 拦截路径的边界检查之前。
- candidate timeout：`StrokeRoundCoordinator::kCandidateTimeoutMs == 60000`，harness 验证 60 秒前不触发、到点触发并走统一 abort；候选关闭/超时相关 UI suite 保持通过。

## 验证证据

保留日志证明最终修订之后实际执行了：

- 专项 2 项：`OK`
- stroke UI 9 项：`OK`
- 最终全量 host：`Ran 100 tests in 11.285s`，`OK`
- clang-format 19.1.7 scoped dry-run：无报错
- `git diff --check`：无输出
- ESP-IDF v6.0.2、`esp32s3`、`CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y` 的仓库外构建：最终 30-step 增量重编译包含 `application.cc`、WebSocket、MQTT、coordinator 并成功链接/分区检查。
- 最终 `xiaozhi.bin`：2,903,072 bytes，SHA-256 `c9f7adf5a386e46d35aa973d4b8573f2ddcb5c6ca0f855623a9479bea3d065fa`；app 分区余量 `0x12b3e0`（30%）。`xiaozhi.elf`、`.map`、`flash_args` 的报告大小/hash也与保留日志一致。
- 构建目录仍存在，`stroke_order_assets/stroke_order.manifest.json` 明确列出三字 `一/人/口`、`character_count: 3`、固定来源 commit 与 `not_a_release_library: true`。

上述测试和构建是对作者保留证据的核验，不是本只读复审重新执行的结果。

## Minor finding（不阻止批准）

`test_async_cancel_sources_fence_before_deferred_work` 的 source-shape 断言强度仍可提高：

1. 它通过 harness 中的文字与调用顺序模拟 `STATE_CHANGED` 先于 scheduled abort，但没有直接断言真实 `Application::Run` 中 `MAIN_EVENT_STATE_CHANGED` 位于 `MAIN_EVENT_STROKE_START`/`MAIN_EVENT_SCHEDULE` 前。
2. 它对 final gate 使用首个 `HasCancelFence` 的索引进行比较，未明确断言“第二次” `HasCancelFence` 严格位于 `SendStartListening` 与 `EnableVoiceProcessing(true)` 之间。

真实生产源码的两项顺序本次已人工核对正确，production-shape harness 也执行了所需交错，因此这是未来防回归测试精度问题，不构成当前功能或安全 blocker。

## 尚未验证的真实风险

- 未烧录 M5Stack CoreS3，未验证实际 FT6336/LVGL 触摸、显示、音频 DMA、功耗切换、WDT、underrun 或 100 次真机会话 heap/timer 稳定性。
- 未连接真实 WebSocket/STT 服务注入 open/hello 后立即断链、open→bind 断链、bind→start 断链、Wi-Fi scanning/disconnect、同步发送失败与重连风暴；host fake 无法替代具体网络驱动的 callback 生命周期和 FreeRTOS 调度。
- 未做 TSAN 或真实多核压力测试；锁序与线性化仅由源码审查和确定性交错 harness 支持。
- 普通 MQTT 行为只由 host harness 与源码兼容性检查覆盖，未做真实 broker/UDP 端到端回归。
- 三字资源是明确标注的原型 smoke corpus，不是可发布完整字库；正式商用仍需授权与笔顺准确性审核。

- WebSocket open attempt 在 `OpenAudioChannel` 前由原子 channel generation 预留，并由 Application 单槽映射到确切 stroke generation；disconnect callback 回传同一 per-socket attempt ID。
- 真实 `Application::Run` 当前处理顺序是 STATE_CHANGED、随后 STROKE_START、最后 SCHEDULE；source-side fence 因而是阻止 scheduled abort 之前错误启动采集的必要条件。
- `StartListeningAudio` 与所有 Application fence publisher 使用同一递归互斥量，并在 SendStartListening 后、EnableVoiceProcessing 前再次检查 fence。
- MQTT/UDP 的 correlated-open 与 stroke-voice capability 均保持 false；三字 stroke 走本地候选，普通 MQTT channel 仍使用 attempt ID 0。
- 最终保留证据显示 100/100 host tests 通过，ESP-IDF v6.0.2 CoreS3 stroke-enabled 构建成功，最终 bin 为 2,903,072 bytes、SHA-256 c9f7adf5a386e46d35aa973d4b8573f2ddcb5c6ca0f855623a9479bea3d065fa。

<!-- pi-squad:e5cdf7650e4b7e4dcc42e43613c8fe7126ffe71cb7dbe6543b7fa2e0aa76cb94 -->
## 2026-09-12T11:36:43.572Z — stroke-500-license-review

# 500 字笔划原型：数据许可与发布边界只读审查

> 审查性质：工程侧许可证合规边界审查，不替代中国或其他适用法域的正式法律意见。

## 结论

**REJECTED：当前工作区没有足够证据批准“约 500 字原型”对外分发或商用发布。**

这不否定现有三字 smoke 的开发用途。当前可核实产物仍只有 `一/人/口`：

- `scripts/package_stroke_order_smoke.py` 将三个字、三个源文件摘要和固定 commit 写死；
- `main/CMakeLists.txt` 在 `CONFIG_STROKE_ORDER_LOCAL` 下调用的仍是该三字 packager；
- `dist/m5stack-core-s3-stroke-smoke/stroke_order/stroke_order.manifest.json` 的 `character_count` 为 3；
- `main/stroke_order/stroke_order_view.cc` 的本地 fallback 也是三个 codepoint；
- 文档和包内 NOTICE 均明确“not a 500-character or release 字库”。

因此，本审查只能给出 500 字版本的**条件性合规方案**，不能把尚不存在的 charset、拼音索引、500 个源文件摘要和最终二进制视为已审过。

## 已核实的正面基础

1. **图形来源固定**：现有 fixture 声明来自 `chanind/hanzi-writer-data` commit `68d10a4b21150cae5e1ebbd223eed289cf32d90c`，并追溯到 `skishore/makemeahanzi`；三字源路径、逐文件 SHA-256 和 aggregate SHA-256 已写入 NOTICE/manifest。
2. **完整 APL 文本存在**：`scripts/tests/fixtures/stroke_order/upstream_smoke/ARPHICPL.TXT` 是完整 Arphic Public License，代码还固定了其 SHA-256。
3. **当前转换边界较窄**：`scripts/stroke_order/convert.py` 只解析每字 JSON 的 `strokes` 与 `medians`，生成 SOB1；三字 JSON 确实只有这两个字段。文档明确不导入 `all.json`、dictionary text 或完整 checkout。
4. **当前没有拼音/同音字数据**：`main/stroke_order/stroke_order_candidates.h` 是 no-op provider，并明确说明不查询未许可字典；仓库未发现 500 字 charset、拼音索引、HSK 或频率表产物。
5. **现有 smoke packager 会同时生成** `stroke_order.bin`、manifest、`ARPHICPL.TXT` 和 `NOTICE.md`，CMake 将四项都作为原子输出纳入 assets。

## 阻止 500 字发布的主要缺口

### B1. 没有可审的 500 字物料

缺少：明确字符清单、选字方法和来源、约 500 个源 JSON 的逐文件摘要、聚合摘要、最终二进制、拼音/候选索引（如拟加入）、对应 NOTICE、source offer 及逐字笔顺复核记录。现有三字哈希不能外推到另外约 497 字。

### B2. 上游许可链证据不完整

仓库保留了 APL 文本，但没有同时保存固定 commit 下 `hanzi-writer-data` 与 Make Me a Hanzi 的完整许可说明快照，也没有任何 LGPL 文本。`scripts/stroke_order/constants.py` 仅写“dictionary material … **may be LGPL or other copyleft terms**”；这种不确定表述不能作为发布许可结论。

最保守处理是：500 字首包**完全不读取、转换、复制或派生 `dictionary.txt`**，也不把其发音、释义、部件、频率字段间接写进索引。若以后使用，必须先在同一固定 commit 上确认：准确文件、版权所有者、数据的上游来源、准确 LGPL 版本/“only”或“or-later”、完整许可证和 NOTICE；未确认前不得仅写“LGPL”后发布。

### B3. APL 的字面义务尚未按最保守标准闭环

根据包内 `ARPHICPL.TXT`：

- §1：每份复制品必须保留**未修改的** `ARPHICPL.TXT`；
- §2(a)：每个修改文件中应有显著说明，写明**如何以及何时**修改；
- §2(b)：修改物整体须在同一许可条件下向第三方 Freely Available，并能从指定地点或常见交换介质取得；
- §2(c)：若修改字体通常以交互命令运行，需显示版权、无担保、可再分发及查看许可证的方法；对当前资产是否触发有解释空间，保守方案应提供设备内“许可证/关于”入口或等效明显说明；
- §5：不得对接受者行使 APL 权利附加限制；
- §7–8：应保留无担保/损害免责语义。

当前 NOTICE/manifest记录了曲线扁平化、坐标取整和 y 轴翻转，满足了“如何”的一部分，但 `stroke_order.manifest.json` 被刻意设计为无时间戳，NOTICE 也没有每个修改输出的明确修改日期；SOB1 文件本身不含显著修改 notice。总体构建 manifest 的 `created_utc` 不能稳妥替代 APL §2(a) 对修改文件的要求。**发布前应让法务确认 companion NOTICE 是否足够；最保守实现是在 SOB 格式的许可元数据区或与文件不可分割、由哈希绑定的 per-file notice 中记录 changed-at/changed-by/how，并保留独立人类可读 NOTICE。**

§2(b) 也不应只靠一个可能失效的 GitHub 链接。应随下载包附 source/compliance bundle，或提供稳定、无需额外 EULA/账号限制的指定下载地址，至少包含精确选中的原始 JSON、字符清单、转换器版本/源码、构建说明、修改后的 SOB1 与摘要。APL 资产须允许复制、修改和再分发；若整机/固件另有“禁止逆向/禁止再分发”条款，必须明确排除 APL 资产。

### B4. 通用 converter 不是发布 packager

`scripts/stroke_order/convert.py` 只写出 `.bin` 和 manifest；它不会复制 APL、生成满足 §2(a)/(b) 的发布 NOTICE，也不会生成 source bundle。现有四文件闭环只属于三字 `package_stroke_order_smoke.py`。500 字版本若直接调用通用 converter，会漏掉发布必需物，必须有新的 fail-closed packager，并在缺任一许可文件、日期、源摘要或 source offer 时拒绝出包。

### B5. MIT 与 APL 不可混称

仓库代码根许可证是 MIT，但图形及其 SOB1 修改物不能据此标成“MIT-only”。APL §2 的 separate-work/aggregation 文字支持把独立固件代码继续按 MIT 分发，但包、SBOM 和 NOTICE 必须清楚区分：**固件代码 MIT；笔画图形及其派生数据 APL**。不得把根 `LICENSE` 当成覆盖全部 assets 的单一许可证。

## 拼音索引、频率、HSK 与规范汉字清单风险

### 拼音索引

- 若拼音来自 `dictionary.txt`，即使只发布 `汉字→拼音` 的压缩索引而不发布原文，也应先按其派生/改编物处理；不能仅凭“拼音是事实”自行免除许可证、来源、修改和 share/source 义务。
- 若使用其他词典、Unihan、拼音库或在线接口，必须逐一核实**数据文件本身**而非仅代码库的许可证、版本、商业再分发、署名及数据库权利；MIT 代码不当然使随附数据成为 MIT。
- 最保守可落地首版是**不带拼音索引、不扩展同音字**：沿用 no-op provider，由远端 STT 返回的单个汉字作为唯一主候选，经本地 500 字 allow-list 检查后展示。这仍实现“语音选字”，且不会引入 dictionary/拼音数据链。需要多个同音候选时，另立经授权的数据包再审。

### 第三方频率表

词频表通常源自受版权、数据库权、研究用途、署名、非商用或站点条款约束的语料。即使只取排序或前 500，仍可能复制数据库的实质性选择/编排。**不要抓取或合并来源不明词频；不要用未许可排行生成字符集或候选顺序。** 首版可使用项目自行策划、留有作者/审核记录的覆盖清单，并采用显式手工顺序或 codepoint 顺序，不宣称频率排名。

### HSK 表

公开可访问不等于开放再分发；具体 HSK 词表版本、出版物和 “HSK” 标识还可能有版权/商标边界。没有明确许可前，不应打包其表格、复刻其顺序，或把产品标为“HSK 1–N 级覆盖/认证”。

### 《通用规范汉字表》

可把权威版本作为**人工成员资格核验参考**，但不要直接复制官方 PDF、表格版式、排序、级别或注释进入包，也不要把其标题变成官方背书。最保守流程是先由项目团队基于产品需要独立拟定约 500 字清单，再逐字核对是否属于指定版本，并仅保留“核对版本、日期、审核人、结果”的审计记录。U+4E00..U+9FFF 只是结构约束，不证明简体、规范性或正确笔顺。

## 最保守可落地方案

1. 固定仍使用上述 40 位 commit；对约 500 个**显式、项目自拟**字符逐一从 clean checkout 取 `data/<字>.json`，校验 tracked-at-HEAD、逐文件 SHA-256 与 aggregate SHA-256。
2. 只转换 `strokes`/`medians`；明确排除 `dictionary.txt`、`all.json`、释义、拼音、频率、HSK、部件和第三方排行。
3. 不做同音补足；STT 单字为唯一候选。任何“多候选/常用度排序”延后到独立授权数据审查。
4. 将 APL 数据与 MIT 代码作为可识别的独立聚合物：独立文件、独立 license 字段、独立 source bundle；不得用固件 EULA 限制 APL 权利。
5. 对 500 字逐字做大陆规范笔顺人工审核并留审计结果；失败字移出集合，不从字体轮廓或别表自动替换。
6. 在外部下载包、OTA 发布页/合规下载页及设备可访问说明中同时提供许可证、NOTICE 和 source 获取方式。正式商用前再由合格法务或权利人确认 APL 对这种抽取/转换格式及 §2(a) notice 位置的解释。

## 每个分发包必须携带

- 根项目 `LICENSE`（MIT，明确只覆盖相应代码）；
- `licenses/ARPHICPL.TXT`：完整、未修改、固定摘要；
- `THIRD_PARTY_NOTICES.md` 或 `NOTICE.stroke-order.md`：Arphic copyright、两个上游项目、固定 repo/commit、材料范围、排除项、逐项修改的 how/when/by-whom、无担保、再分发权、查看许可证方式、稳定 source URL/包路径；
- `stroke_order/stroke_order.bin` 及与其哈希绑定的 per-file modification notice；
- `stroke_order/stroke_order.manifest.json`；
- `sources/stroke-order-apl-source-<version>.*`，或等效且长期可用的书面 source offer；最保守是包内直接附带精确选中的原始 JSON、charset、转换器版本/源码和复现说明；
- SBOM/文件清单，逐文件标注 MIT、APL 或其他准确许可证；
- 只有实际使用 dictionary/拼音/频率/HSK 数据时，才加入相应的完整许可证、copyright/attribution、源数据和修改说明；未知许可证不得用通用 NOTICE 掩盖。

## manifest 最低字段

- artifact：`name`、精确版本、`distribution_class=prototype-evaluation`、精确 `character_count`、字符/codepoint 清单、`not_official_or_certified=true`；
- source graphics：project/upstream、规范化 repo URL、40 位 commit、clean checkout、APL 名称/标识、license 文件路径与 SHA-256、copyright、每字源路径与 SHA-256、aggregate 算法与摘要；
- excluded inputs：明确记录 `dictionary.txt`、pinyin、frequency、HSK、`all.json` 均未使用；
- selection：项目自拟方法、字符清单文件摘要、规范表核验的版本/日期/审核状态；不得伪装成第三方频率排名；
- conversion/modification：转换器版本或 commit、参数、曲线扁平化、取整、y 翻转、筛选、序列化，`changed_at_utc`、`changed_by`、每个 modified output；
- outputs：路径、大小、SHA-256、逐文件许可证；
- source availability：source bundle 路径/URL、摘要、对应版本，并声明 APL 权利不受额外限制；
- reviews：许可证审查状态、逐字笔顺审核状态、未通过字符；不得把内部工程审查写成官方/法律认证。

## 不得对外作出的声明

- “官方/教育部/国家认证笔顺”“全部逐字符合《通用规范汉字表》”或“官方字库”；
- “最常用 500 字”“按权威词频排序”“HSK 某级完整覆盖/认证”，除非有可审许可、版本与完整性证据；
- “hanzi-writer-data/Make Me a Hanzi 数据是 MIT”“整个固件及所有 assets 都是 MIT”“无 copyleft/无署名义务”；
- “dictionary/LGPL 不适用，因为只取了拼音事实”，或在实际派生后仍声称“未使用 dictionary”；
- “APL 只需放一个链接”“转换后二进制是自有闭源数据”“禁止用户提取、修改或再分发该资产”；
- “可无条件商用”“已完成授权审核”。APL 本身允许收费并不等于现有来源链、修改 notice、source share 和逐字准确性已闭环；
- “完整字库/发布字库”。应表述为“约 500 字、范围明确的实验性支持集合”，并列出准确版本和数量。

## 验证记录与剩余风险

本次严格只读，使用文件读取与仓库搜索核对上述源码、manifest、NOTICE 和完整 APL 文本；搜索未发现 `**/*LGPL*`、500 字 charset、拼音/词频/HSK 数据产物。**未运行 shell、测试、构建或哈希重算，也未联网复核两个上游仓库在固定 commit 的 README/许可历史。** 报告中的现有摘要值来自已读源码、测试断言和 manifest，不是本次独立重算。上游实际权利链、APL 对图形抽取/SOB1 的适用性及 companion notice 是否满足 §2(a)，仍须由法务或权利人确认。

- 当前仓库可核实的真实笔划数据包仍只有一/人/口：packager、CMake、运行时本地 fallback 和 dist manifest 都是三字；没有可审的 500 字 charset 或二进制。
- 现有图形来源固定为 chanind/hanzi-writer-data commit 68d10a4b21150cae5e1ebbd223eed289cf32d90c，并追溯 skishore/makemeahanzi；三字 NOTICE/manifest 保存了逐文件和 aggregate SHA-256。
- 包内 Arphic Public License §1 要求保留未修改许可证，§2(a) 要求每个修改文件显著说明如何及何时修改，§2(b) 要求修改物整体同条件 Freely Available，§5 禁止附加限制。
- 通用 scripts/stroke_order/convert.py 只输出 SOB1 和 manifest；复制 APL 与 NOTICE 的四文件闭环目前仅由三字 smoke packager 实现。
- 仓库根代码许可证是 MIT，但 APL 图形及其派生 SOB1 必须作为独立聚合资产标识，不能被根 MIT LICENSE 覆盖。
- 当前 converter 只读取 strokes/medians；仓库无 LGPL 文本，dictionary 许可仅被描述为可能是 LGPL 或其他 copyleft，不能据此批准 dictionary 或拼音派生物。
- 当前同音候选 provider 是 no-op，仓库未发现拼音、词频或 HSK 数据；维持该边界可避免在首个 500 字原型引入未审数据链。
- 本次为只读静态审查，没有运行测试、构建、哈希重算或外部上游网络验证。

<!-- pi-squad:f60a4179116ff260967506f2723092bc3eb56ad28f149454288b1015e93efa81 -->
## 2026-09-12T13:32:36.594Z — stroke-final-core-s3-500-doc-rereview

# stroke-final-core-s3-500 文档/manifest 独立复审

## 结论

**APPROVED（限定为当前 CoreS3 500 字原型 artifact 的文档、manifest、目录与 ZIP 一致性，以及在已配置 CoreS3 上采用保留 NVS 的多文件烧录路径）**。

- **Blocker：0**
- **Major：0**
- **Minor：0**

此前拒绝项已闭环：README 不再把 merged 镜像作为已配置设备的首选，并已准确说明从 `0x0` 写入连续 merged raw 镜像会清除 NVS；manifest、顶层校验表和 ZIP 均已同步重建。此批准**不等于**对 500 字数据的正式商用发布授权或真机功能验收。

## 只读复审范围与证据说明

本次直接读取了真实工作区中的：

- `dist/m5stack-core-s3-stroke-500-prototype/README.zh-CN.md`
- `manifest.json`
- `flash_args`
- 顶层及 `stroke_order/` 内层 `SHA256SUMS`
- `stroke_order/NOTICE.md`、coverage/source-lock/manifest 文件
- 真实 sibling ZIP（文件存在且为 ZIP/PK 数据）
- Tester run `f58f9d69-359e-44ac-814a-d352f7f4c1ff` 的原始保留执行输出与最终报告
- 上一轮独立拒绝审计及原始打包记录，用于核对修订前基线 hash

遵守任务约束：**没有修改文件、没有运行 shell、没有重跑测试、构建或烧录**。下述 checksum、ZIP 和二进制范围检查是对 Tester 最后一次重建之后保留的原始执行输出的核验，不冒充本次重新执行。

## 1. README 烧录语义：通过

真实 README 当前明确写明：

1. `merged-binary.bin` 是从 `0x0` 开始的连续 raw 镜像，esptool 会擦除并写入其覆盖范围。
2. NVS 位于 `0x9000`–`0xcfff`，共 16,384 字节；merged 在该范围是 `0xFF` padding，因此从 `0x0` 写 merged 会清除既有 Wi‑Fi 配网、激活信息及其他 NVS 数据。
3. 显式 `erase-flash` **不是**清除 NVS 的唯一方式；merged 写入本身即会清除该范围。
4. `write-flash @flash_args` 被列为已配置设备希望保留 NVS 时的首选。
5. 两个 `write-flash` 方式要求严格二选一，不得先后执行，也不得合并输入。
6. README 明确区分“包内不携带导出的凭据/NVS payload”和“烧录是否保留目标设备 NVS”，没有再把两者混为一谈。
7. README 同时保留多文件方式的真实风险：仍会重写 bootloader、partition table、OTA data、app 和 assets，布局不兼容、烧录中断或旧持久状态不兼容仍可能要求恢复。

## 2. `flash_args` 与保留 NVS 路径：通过

真实 `flash_args` 仅包含：

- `0x0 bootloader/bootloader.bin`
- `0x8000 partition_table/partition-table.bin`
- `0xd000 ota_data_initial.bin`
- `0x20000 xiaozhi.bin`
- `0x800000 generated_assets.bin`

它不含 NVS 的 `0x9000` 输入，也不含 `phy_init` 的 `0xf000` 输入。各相邻镜像均按 4 KiB sector 边界与 NVS/phy 分区分离；当前多文件集合不会主动写 NVS 或 `phy_init`。

## 3. manifest 机器字段：通过

真实 `manifest.json` 为有效、完整可读 JSON，机器字段与 README、`flash_args`、分区事实一致：

- `methods_are_mutually_exclusive: true`
- `preferred_flash_method_for_preserving_nvs: "multi-file"`
- `merged_clears_existing_nvs: true`
- NVS range：offset `36864/0x9000`，inclusive end `53247/0xcfff`，exclusive end `53248/0xd000`，size `16384`
- multi-file 的 NVS/phy image 输入与 active write 四项均为 `false`
- multi-file 写入内容及 bootloader/partition/OTA/app/assets 风险均已列出
- merged 为 `write_offset=0`、`continuous_raw_image=true`、NVS range 为 `0xff_padding`
- merged 用途仅为 fresh device 或明确接受 Wi‑Fi/激活/NVS 重置的 complete reflash
- `explicit_erase_flash_is_not_the_only_way_to_clear_nvs: true`
- `credential_absence_does_not_imply_nvs_preservation: true`

`security_and_packaging.nvs_partition_payload_included=false` 也已由说明字段限定为“没有设备导出的 NVS/凭据”，并明确补充 merged 仍会用 FF padding 清除目标设备 NVS。README 在 manifest 文件表中的 hash/size 与顶层校验表一致。

## 4. 二进制与其他不可变文件：通过

Tester 在修订前建立完整 baseline，最终重建后比较除获准变化的 README、manifest、顶层 SHA256SUMS 外的其余文件，保留输出为 **23/23 unchanged**。当前真实顶层校验表仍列出同一组值。主要二进制如下：

- bootloader：`1cf7897c69aaf046202e438fad724d692f701a343c85b15cefd0b0420cccce51`
- generated assets：`6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`
- merged：`4a22047b840bd3459cb1ad4fa158567a1e3be2927afd23413df64eca09964fbb`，11,266,414 bytes
- OTA data：`7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f`
- partition table：`4811619cacae08ef2e0e71b7220c6033a346ca5da7ca179082408c963ef530b5`
- SOB1：`3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`
- SPY1：`a97666f18fa8e5a5e586f774dac044c08ba26b40decb4644e12c6886a5cbc998`
- app：`a49373c7a38329023b850a45e0c51426d60236dd5b0b8337e9498df38b52af62`
- ELF：`3ff711ddcfbd2100eb60637ac66110c9943ede705d17eab7e3beee07edc76ed2`
- map：`2ace67eb1d9c49b8548bad970cfc9be4b8cb76931c70163dc63dfd3d71ef7e32`

保留的最终检查还直接读取 merged 的 `0x9000:0xd000`，结果为 **16,384/16,384 字节均为 `0xFF`**，支持文档中的 NVS 清除结论。

## 5. SHA256SUMS 与 ZIP：通过

真实顶层 `SHA256SUMS` 当前恰有 **25 项**，逐项覆盖 package 目录内除自身外的全部 25 个普通文件；目录总计 26 个普通文件。最终修订后保留执行输出证明：

- 目录 `shasum -a 256 -c SHA256SUMS`：25/25 `OK`
- `unzip -t`：26 个文件条目全部 `OK`，`No errors detected`
- 解压目录再次校验：25/25 `OK`
- 原目录与解压目录 `diff -qr`：`identical`
- 最终不可变文件对比：23/23 unchanged
- 最终 README/manifest/NVS 语义断言：`OK`

最终文档与封装 hash/大小为：

| 文件 | SHA-256 | 大小 |
|---|---|---:|
| `README.zh-CN.md` | `b75dd1d60d5cb1d3de66c335b6a92d409f85a6640d4485c39d0515593ba7a364` | 8,149 B |
| `manifest.json` | `5fb0f547e8a4e63a7efcecb7a4d92907e0ff1f8725d04d646c6468e8b1727635` | 17,303 B |
| `SHA256SUMS` | `d51b79a25e9932ae1c430fc1eb8dcf9b950dc94091dd11ffac0cc565cc593990` | 2,299 B |
| `m5stack-core-s3-stroke-500-prototype.zip` | `bdbc8c589db291d4ace0de49e093dab87079fb5d6e955c36a104227cf410371e` | 23,931,458 B |

最终校验发生在最后一次 README 同步及原子重建 SHA256SUMS/ZIP **之后**，不是中间版本的结果。

## 6. 500 字、许可、dirty 与未真机边界：无回退

- `stroke_order.cov.json` 仍为 500/500、0 error、complete，同时保留 `prototype`、`not_a_release_library`、`not_official_certification`。
- `stroke_pinyin.cov.json` 仍为 500/500、complete、prototype/not-release。
- APL、Unicode License、NOTICE、charset、selection、SOB1/SPY1 及 source-lock/manifest 的 hash 均在 23/23 不变集合内。
- NOTICE 仍固定 HWD commit `68d10a4b21150cae5e1ebbd223eed289cf32d90c`、仅使用 strokes/medians，并固定 Unicode 16.0.0 Unihan；明确不是官方认证、不是 release 字库、未完成商业审查，外部分发前仍需法律与逐字笔顺准确性审核。
- README 与 manifest 仍明确 `dirty_worktree=true`、base commit 不能单独复现全部二进制、打包前 dirty path count 为 48。
- `hardware_validation_performed=false`；README 和 hardware risk 列表仍明确本轮未烧录、未验证真实 PSRAM、FT6336、WebSocket STT/open identity、FPS、heap/timer/WDT/audio-underrun 及 100 次会话长稳。

因此，本次文档修订没有把原型包装成正式/商用字库，也没有把离线构建验证冒充真机验收。

## 批准的保留 NVS 烧录方式

**允许主协调器烧录当前已配置的 M5Stack CoreS3，但仅批准下面的 multi-file 路径。** 应先进入 package 目录，确认 `$PORT` 指向正确的 ESP32-S3 / 16 MB CoreS3，然后执行：

```sh
cd /Users/mandyw/git/xiaozhi-esp32/dist/m5stack-core-s3-stroke-500-prototype
python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
  --before default-reset --after hard-reset write-flash @flash_args
```

在这台已配置设备上，**不要**先执行 `erase-flash`，也**不要**再执行 `0x0 merged-binary.bin`；两种写法严格二选一。multi-file 路径保留 NVS/phy 的前提不消除 bootloader、分区表、OTA data、app/assets 改写及断电/布局兼容风险。

## 剩余风险（不阻止本次限定批准）

1. 本轮仍没有真机烧录结果；NVS 保留、启动、PSRAM、触摸、联网、动画和长稳需要烧录后观察。
2. multi-file 会改写 bootloader、partition table、OTA data、app 和 assets；应保证供电稳定并保留恢复手段。
3. 500 字数据仍仅为原型；不得把本批准解释为正式商用分发许可、官方笔顺认证或逐字准确性审核结论。

- 真实 `flash_args` 只写 0x0 bootloader、0x8000 partition table、0xd000 OTA data、0x20000 app、0x800000 assets，不含 0x9000 NVS 或 0xf000 phy_init 输入。
- `merged-binary.bin` 仍为 11,266,414 bytes、SHA-256 `4a22047b840bd3459cb1ad4fa158567a1e3be2927afd23413df64eca09964fbb`；保留检查确认其 0x9000:0xd000 全部 16,384 字节为 FF。
- 最终顶层 SHA256SUMS 有 25 项，覆盖 26 个普通文件中除自身外的全部文件；最终 ZIP 有 26 个文件条目，保留日志证明 CRC、解压 checksum 与目录 diff 全部通过。
- 文档修订前后除 README、manifest、顶层 SHA256SUMS 外的 23 个目录文件 hash 全部不变；这覆盖八个 `.bin`、ELF、map、许可和全部 500 字资产元数据。
- 500 字原型仍明确为 500/500 complete，但不是 release 字库、不是官方认证、未完成商业授权/逐字准确性审核；来源、APL、Unicode License 与 dirty source 边界没有回退。
- 最终 ZIP 的 SHA-256 为 `bdbc8c589db291d4ace0de49e093dab87079fb5d6e955c36a104227cf410371e`，大小为 23,931,458 bytes。

<!-- pi-squad:b7959f7aafed1424cb20fe47ecd070ee6f31106842dd63198e617acc1355070e -->
## 2026-09-12T14:07:41.668Z — stroke-final-core-s3-500-websocket-refresh-review

# stroke-final-core-s3-500 WebSocket preference 刷新独立复审

## 结论

**REJECTED**

- Blocker：0
- Major：1
- Minor：1（继承的串口隐私风险）

固件输入、二进制刷新、500 字资产不变性、merged offset、NVS 烧录说明、checksum 与 ZIP 完整性证据均通过；但当前 README 与 manifest 对 `stroke_voice=1` 日志的时序/证明力作了与真实代码相反的陈述。该字段在协议初始化、甚至 `protocol_->Start()` 之前由静态 capability 打印，不能证明 WebSocket 音频通道已打开或已取得本轮 correlated session identity。由于本任务明确要求 README/manifest 准确，当前 ZIP 不应作为最终 500 字原型包发布。

本次严格只读：直接读取真实 dist 目录、真实 ZIP 头与路径、外部 build 元数据、sdkconfig、源码、测试/构建日志和打包 run 的保留原始输出；没有修改文件，没有运行 shell、测试、构建、解压或烧录。以下命令结果均明确标注为 Tester 最终写入后保留的执行证据，不冒充本复审重新执行。

## 已通过部分

### 1. 输入 build 已获独立批准

`.squad/orchestration-log/squad-403028f3-70ee-4225-8cfd-215f5d1c7ebe.md` 明确给出 `stroke-websocket-preference` 的 `APPROVED` 结论，并允许进入 500 字包重打与 CoreS3 真机多文件烧录阶段。

直接读取 `/private/tmp/xiaozhi-stroke-websocket-preference-build`：

- `project_description.json`：ESP-IDF `v6.0.2`、target `esp32s3`、app `xiaozhi.bin`、build dir 与指定路径一致；
- `sdkconfig:650/1106/1226`：`CONFIG_IDF_TARGET="esp32s3"`、`CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y`；
- build 日志结尾：`[2213/2214]` 尺寸检查，app `0x2c6310`，分区 `0x3f0000`，余量 `0x129cf0 (30%)`，随后 `Project build complete`；
- host-test 日志：`Ran 116 tests in 15.386s`、`OK (skipped=1)`，即 115 pass、1 expected conditional skip、0 failure/error。

### 2. app/ELF/map/merged 已更新，且来自 approved build

上一版批准基线与当前值不同，确认刷新发生：

| 文件 | 旧 SHA-256 | 当前 SHA-256 | 当前大小 |
|---|---|---|---:|
| `xiaozhi.bin` | `a49373c7a38329023b850a45e0c51426d60236dd5b0b8337e9498df38b52af62` | `f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a` | 2,908,944 |
| `xiaozhi.elf` | `3ff711ddcfbd2100eb60637ac66110c9943ede705d17eab7e3beee07edc76ed2` | `3906ba19dcba7c9a81a337064dbb736856f6b8e5335e1078639155a3ec85a456` | 40,154,248 |
| `xiaozhi.map` | `2ace67eb1d9c49b8548bad970cfc9be4b8cb76931c70163dc63dfd3d71ef7e32` | `7063b425589af4d0b43ad3e0feea239761a3f36137997e9d4fa379560465ae03` | 24,553,497 |
| `merged-binary.bin` | `4a22047b840bd3459cb1ad4fa158567a1e3be2927afd23413df64eca09964fbb` | `e23e291907d14d14a1ee847b340793cf009f0d8ad4650d88d05b5ef8e1ccb039` | 11,266,414 |

Tester 的最终保留输出逐项 `cmp` 通过：package 的 app、ELF、map、flash_args、bootloader、partition table、OTA data、generated assets 与指定 approved build 相同。app 的 `image-info` 所报 ELF SHA 也等于包内 `xiaozhi.elf` SHA。

### 3. 500 字 assets 保持不变

真实 `dist/.../stroke_order/` 仍恰有 14 个普通文件；真实 build 的 `stroke_order_assets/` 列表相同。刷新前后 14 文件 SHA-256 baseline diff 无输出并给出：

`OK all 14 data/license file SHA-256 values unchanged`

当前内层 `SHA256SUMS` 的 13 个被校验文件与 build 中同名清单完全一致；第 14 个文件 `stroke_order/SHA256SUMS` 自身 SHA-256 为 `dbba16e352851188de77545c45db9eaeb1577e7e4261d6ae95c7b3ab115fb70a`。关键资产保持：

- `stroke_order.bin`：`3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`，1,046,788 bytes；
- `stroke_pinyin.bin`：`a97666f18fa8e5a5e586f774dac044c08ba26b40decb4644e12c6886a5cbc998`，18,152 bytes；
- `generated_assets.bin`：`6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`，2,877,806 bytes；
- coverage 文件仍声明 500/500、complete、prototype、`not_a_release_library=true`。

### 4. 双配置 WebSocket preference 与无 WS 降级的功能描述主体正确

真实生产代码：

- `protocol_selection.h`：stroke preference 为真且存在 WebSocket 配置时首先返回 WebSocket；否则保持 MQTT-first/WS-only/no-config fallback；
- `application.cc:594-617`：`CONFIG_STROKE_ORDER_LOCAL` 令 preference 为 true，并按 helper 只构造一个 transport；
- `mqtt_protocol.h`：`SupportsCorrelatedSessionOpen()==false`、`SupportsStrokeVoiceRouting()==false`；
- `websocket_protocol.h`：两项 capability 均为 true；
- `application.cc:1227-1232`：capability 不成立时进入本地候选路径。

因此 README/manifest 对“双配置选择 WebSocket”“`websocket_config=0` 且 MQTT 可用时选择 MQTT、`stroke_voice=0`、使用本地 SPY1 TopRanked 候选、不声称 MQTT 语音选字”的主体说明是正确的。

### 5. NVS 多文件首选与 merged 清 NVS说明保持正确

真实 `flash_args` 仅写：

- `0x0` bootloader
- `0x8000` partition table
- `0xd000` OTA data
- `0x20000` app
- `0x800000` assets

不含 `0x9000` NVS 或 `0xf000` phy_init 输入。README 和 manifest 均把 `write-flash @flash_args` 列为已配置设备保留 NVS 的首选，并明确两种刷写方式二选一；merged 从 `0x0` 写入会用 `0xFF` padding 覆盖 `0x9000–0xcfff`，清除 Wi-Fi、激活及其他 NVS。保留 offset 检查也确认该 16,384-byte 范围全为 `0xFF`。

### 6. 二进制、checksum 与 ZIP 完整性证据通过

Tester 最终写入后保留输出显示：

- merged 五段逐字节匹配：bootloader `0x0`/16,672 bytes、partition `0x8000`/3,072、OTA `0xd000`/8,192、app `0x20000`/2,908,944、assets `0x800000`/2,877,806；
- esptool v5.3.1 对 bootloader、app、merged 报 ESP32-S3 / DIO / 80 MHz / 16 MB，checksum valid、validation hash valid；app 为 `xiaozhi` 2.4.2；
- 顶层 `SHA256SUMS` 25/25 OK；内层 13/13 OK；manifest 24 个 file record 的 size/hash 均匹配；
- `unzip -t` 全部条目 OK，`zip -T` OK；
- 解压后顶层与内层 checksum 全部 OK；原 dist 与解压树 `diff -qr` identical；目录与 ZIP 各 26 个普通文件；
- 直接读取当前真实 ZIP 可见合法 `PK` 文件头及预期顶层目录名。

最终产物：

- ZIP：`dc6524722341f93cb5335117caa10abba383ba2603722853097fa203edb46a15`，24,165,514 bytes；
- merged：`e23e291907d14d14a1ee847b340793cf009f0d8ad4650d88d05b5ef8e1ccb039`，11,266,414 bytes；
- app：`f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a`，2,908,944 bytes。

## Major finding：`stroke_voice=1` 被错误描述为 correlated open 成功证据

真实源码顺序：

1. `Application::InitializeProtocol()` 在 `application.cc:609-617` 只根据选择结果构造 `WebsocketProtocol` 或 `MqttProtocol`；
2. 紧接着 `application.cc:620-622` 调用两个 `Supports...()` 并打印 `Stroke voice availability: correlated_open=%d stroke_voice=%d`；
3. WebSocket header 对两个 capability 无条件返回 true，MQTT header 无条件返回 false；
4. `StrokeVoiceRoutingAvailable()`（`application.cc:1133-1135`）也只检查这两个静态 capability；
5. `protocol_->Start()` 直到同一 `InitializeProtocol()` 的 `application.cc:997` 才执行；真正 `OpenAudioChannel()` 更晚才在 listening 流程调用。

所以选择 WebSocket 后，即使尚未启动协议、尚未联网、open 最终失败、尚无 session identity，这条初始化日志也会打印 `correlated_open=1 stroke_voice=1`。

但真实包内：

- README 第 90 行称 `stroke_voice=1` “应在 WebSocket 音频通道成功打开并取得本轮关联身份后出现”；
- README 第 105 行把“WebSocket 通道就绪后确认 `stroke_voice=1`”列为验收步骤；
- manifest 第 517 行声明 `"stroke_voice_one_requires_correlated_websocket_open": true`。

这些描述不准确，并可能让真机验收把静态 capability 日志误当成 WebSocket 已连接、audio open 已成功、session identity 已关联的证据。`selected=websocket` 和初始化时 `stroke_voice=1` 只证明选中了声明支持该能力的 transport；实际 open/session/STT 仍必须通过后续行为或新增的动态日志验证。

**要求修复：** 不必重编 app；应修订 README 和 manifest，明确该日志是 `protocol_->Start()` 前的静态 capability 诊断，不证明连接/open/session 成功。建议删除或替换 `stroke_voice_one_requires_correlated_websocket_open`，并另行表述“笔划监听真正开始前仍必须成功 correlated open”。随后重算 manifest/README/顶层 SHA256SUMS，重建 ZIP，并重跑 25/25、13/13、ZIP CRC、解压 checksum/diff 与最终 ZIP hash/size 检查。

## Minor finding：继承的 WebSocket URL 日志隐私风险

`main/protocols/websocket_protocol.cc:237` 仍将完整 `url.c_str()` 写入串口日志。它不是本次 package refresh 引入，也不影响 transport 选择正确性；但收集/分享真机串口日志前应脱敏 endpoint 及可能位于 URL 中的 query credential。

## 放行影响与剩余风险

- **当前 ZIP 不批准作为最终 500 字原型包。**
- 本 finding 未发现 app 二进制功能缺陷；若设备已经烧录的确实是 approved app SHA-256 `f6ec...32f2a`，没有因本次文档问题而必须重刷 app，可继续用于受控真机验证。但在修正文档/manifest并重包复审前，不应把当前 ZIP标记为最终包。
- 本轮未做真机烧录/运行。真实 OTA 是否提供 WebSocket 配置、真实 WebSocket open/STT/session identity、PSRAM、FT6336 坐标/旋转、overlay 触摸、音频与 100 次会话稳定性仍未验证。
- 500 字数据仍仅为 prototype / `not_a_release_library`，本审查不构成商用授权或逐字笔顺官方认证。

- 真实 `Application::InitializeProtocol()` 在构造 transport 后立即打印 `Stroke voice availability`，而 `protocol_->Start()` 到该函数末尾才执行；因此 `stroke_voice=1` 是静态 transport capability，不是 WebSocket 连接、audio open 或 correlated session identity 成功证据。
- 当前刷新包的 app/ELF/map/merged SHA-256 分别为 `f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a`、`3906ba19dcba7c9a81a337064dbb736856f6b8e5335e1078639155a3ec85a456`、`7063b425589af4d0b43ad3e0feea239761a3f36137997e9d4fa379560465ae03`、`e23e291907d14d14a1ee847b340793cf009f0d8ad4650d88d05b5ef8e1ccb039`。
- 当前最终 ZIP 的保留 SHA-256/大小为 `dc6524722341f93cb5335117caa10abba383ba2603722853097fa203edb46a15` / 24,165,514 bytes；merged 为 11,266,414 bytes；app 为 2,908,944 bytes。
- 刷新前后 `stroke_order/` 的 14 个数据/许可文件 hash 全部不变；`generated_assets.bin` 保持 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0` / 2,877,806 bytes。
- 真实 `flash_args` 不包含 NVS/phy_init 输入；保留输出确认 merged 的 `0x9000:0xd000` 为 16,384 bytes 全 `0xFF`，所以多文件方式是保留 NVS 的首选，而从 `0x0` 写 merged 会清除 NVS。
- 最终保留证据显示顶层 checksum 25/25、内层 13/13、ZIP CRC、解压 checksum/diff、五段 merged offset compare 与 esptool image validation 均通过；本复审没有自行重跑这些命令。
- 现有 WebSocket 连接日志仍输出完整 URL；分享串口日志前需脱敏可能包含 endpoint/query credential 的内容。

<!-- pi-squad:2dbfded8c79df9187c8e31d1b00413ebd4f43c36971c1f1060813280f99f1505 -->
## 2026-09-12T18:10:25.039Z — stroke-final-core-s3-2000-review

# stroke-final-core-s3-2000 独立最终审计

## 结论

**APPROVED（限定为当前 M5Stack CoreS3 的本地 2000 字技术原型烧录候选）**。

- 烧录阻断项：**0**
- 当前技术原型范围内 Major：**0**
- 对外/商用发布阻断项：**1**（许可与逐字准确性审查尚未闭环，见下文）
- Minor：**2**（调试产物泄露本地路径；仍有一条来源重建测试条件性跳过）

允许主协调器给**当前已配置的 16 MB M5Stack CoreS3**使用包内 `@flash_args` 多文件方式烧录。此批准不是动态 WebSocket 语音链或真机稳定性验收，也不批准把 2000 字数据作为正式、官方或商用字库发布。

## 精确产物

- 目录：`/Users/mandyw/git/xiaozhi-esp32/dist/m5stack-core-s3-stroke-2000-prototype/`
  - 56 个普通文件
  - 总大小：`104,430,898` bytes
  - 文件集合摘要：`ad0c91d63c4627fa2fa83150ab4dc39fca45b9c2b0655d7cbedab79d71f640c0`
- ZIP：`/Users/mandyw/git/xiaozhi-esp32/dist/m5stack-core-s3-stroke-2000-prototype.zip`
  - 大小：`38,449,270` bytes
  - **SHA-256：`7eb600eba44509ccb3c6666323d6cd884172e5447482c74121a4a2c74aec7773`**

关键镜像：

| 文件 | 大小 | SHA-256 |
|---|---:|---|
| `xiaozhi.bin` | 2,914,784 | `a54a0e4db306a42f3451824b5a005bda4baf6910c62c9a728242be0fa95697da` |
| `generated_assets.bin` | 7,568,207 | `54453b5232408dfb8cf8dadbde9dd5d93a1bdb90f213df69d7ff075210783dd0` |
| `merged-binary.bin` | 15,956,815 | `9618a19591d625d355adc0c0a74fb158cd8477e76b75b534fb84aefe19929268` |

## 审计证据

### 1. 源码、构建快照与最新修复

真实保留目录 `/private/tmp/xiaozhi-stroke-2000-final-build` 的证据显示：

- `CMakeCache.txt` 指向当前源码根 `/Users/mandyw/git/xiaozhi-esp32`，target 为 `esp32s3`。
- `sdkconfig` 含 `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y`、`CONFIG_FLASH_DEFAULT_ASSETS=y`、16 MB flash、Quad PSRAM 和 `partitions/v2/16m.csv`。
- `/private/tmp/stroke-final-core-s3-2000-retry-source-consistency.log`：2,004 个 Ninja dependency targets；其中 1,095 个带项目依赖的 targets 全部 `VALID`；1,095 个项目编译对象无缺失、无源文件比对象更新；1,707 个当前构建输入无晚于 app 的文件；14 个审计 runtime payload 与 build snapshot 精确匹配。
- `.ninja_log`、`build.log` 和 `compiled-units-verify.log` 均显示实际编译了 `application.cc`、`audio_service.cc`、`afe_audio_engine.cc`、WebSocket、MQTT、CoreS3 以及 store/catalog/assets/controller/coordinator/pinyin/view。
- 当前源码可见最新逻辑：非零 stroke generation 映射到 AutoStop；AudioService 的 running 条件绑定真实 `AudioEngine::IsVoiceProcessingEnabled()` 与 service event；WebSocket 使用 per-open attempt identity；STT/Audio/TTS/LLM 均按合法 session 路由；CMake 固定打包 `so00.bin`–`so07.bin`、SCB1 与 SPY1。

构建日志从 `[1/2216]` 开始，最后成功生成 app 并显示 `Project build complete`。本重试没有重跑 clean build；依据上述依赖图、对象时间关系和逐字节 runtime/产物比对，没有发现 stale build 信号。需注意这是 dirty-worktree 构建，base commit `a2ba69df3abc3538f03a08b03facd4541a284a78` 单独不足以复现固件。

### 2. Host 与快速测试证据

保留日志实际记录：

- 全量：`python3 -m unittest discover -s scripts/tests -v` → `Ran 127 tests in 27.702s`，126 pass、1 skip、0 failure/error。
- 快速：2000/UI/pinyin 三组 → `Ran 33 tests in 18.335s`，32 pass、同一条 skip、0 failure/error。
- 唯一 skip 是需要设置 `STROKE_TRANSCRIPTION_JSON` 的固定 clean checkout 重建测试；并非运行时功能失败，但意味着本轮没有从外部固定 transcription checkout 独立重建 selection。
- 日志中的测试名明确覆盖 AutoStop、AudioEngine postcondition、WebSocket transport selection、session/route/fence、2000 字八分片、SCB1/SPY1 和资产余量。
- clang-format 19.1.7 artifact scope、changed-lines、Python `py_compile` 与 `git diff --check` 均有通过记录。

本独立审计遵守只读要求，**没有运行 shell、测试、构建或写文件**；以上均是对真实源码、产物和保留日志的核验，不冒充本轮重跑结果。

### 3. 包、ZIP 与完整性

真实目录结构与三个校验表一致：

- 顶层 `SHA256SUMS`：55/55，覆盖除自身外的全部普通文件。
- `stroke_order/runtime/SHA256SUMS`：14/14；runtime 目录恰好 15 文件。
- `stroke_order/audit/SHA256SUMS`：28/28；audit 目录恰好 29 文件。
- 保留日志证明 ZIP CRC 无错误、56 个 member 无重复/绝对路径/`..`/反斜杠/symlink，解压后 checksum 全通过，且与原目录逐文件 byte diff 为零。
- `manifest.json.files` 与 `manifest.json`、顶层 `SHA256SUMS` 组成精确 56 文件 allowlist。
- 禁止文件名扫描未发现 `sdkconfig*`、日志、NVS/phy payload 或 credential/secret 文件；可读文本中的常见私钥头和凭据赋值模式扫描为 0。

### 4. 2000 字 runtime 与 assets

- runtime 恰好 15 文件：8 个 SOB1、SCB1、SPY1、`runtime.json`、APL、Unicode license、NOTICE、内层校验表。
- 8 个 SOB1 每片 250 字，共 2000 字，总计 `5,800,492` bytes；哈希与 runtime/audit/manifest 三处一致。
- SCB1：24,352 bytes，SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`，2000 entries / 8 shards。
- SPY1：63,618 bytes，SHA-256 `4597a2a7134d4d40e5b025b339d3491bbcaf05f75f7485dee4db7555429b6399`，2000 chars / 1,047 groups / 最大 group 29。
- `generated_assets.bin` 文件表含 39 项；15 个 runtime 项全部与 build runtime snapshot 和包内 runtime 文件逐字节一致。
- assets 分区余量 `820,401` bytes，高于要求的 256 KiB。

### 5. 镜像、offset 与 16 MB 边界

保留 byte-compare 证明 merged 的五段与独立文件完全一致：

- `0x0` bootloader：16,672 bytes
- `0x8000` partition table：3,072 bytes
- `0xd000` OTA data：8,192 bytes
- `0x20000` app：2,914,784 bytes
- `0x800000` assets：7,568,207 bytes

esptool v5.3.1 `image-info` 验证 bootloader/app（以及 merged 起始 bootloader）为 ESP32-S3、DIO、80 MHz、16 MB，checksum 与 validation hash 有效；app 标识为 `xiaozhi` 2.4.2 / ESP-IDF v6.0.2，ELF hash 与包内 ELF 一致。

- app 分区余量：`1,213,984` bytes（约 29%）。
- merged 末端 `0xf37b4f`，距 16 MiB 末端余 `820,401` bytes，不越界。
- partition decode 为 NVS `0x9000/16K`、OTA data `0xd000/8K`、phy `0xf000/4K`、两个 4032K app、assets `0x800000/8M`。

### 6. NVS 烧录语义与授权命令

包内 `flash_args` 精确为五段，不含 NVS 或 phy-init：

```text
--flash-mode dio --flash-freq 80m --flash-size 16MB
0x0 bootloader/bootloader.bin
0x8000 partition_table/partition-table.bin
0xd000 ota_data_initial.bin
0x800000 generated_assets.bin
0x20000 xiaozhi.bin
```

在 package 根目录对当前已配置 CoreS3 使用：

```sh
python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
  --before default-reset --after hard-reset write-flash @flash_args
```

该方式不写 `0x9000..0xcfff` NVS 或 `0xf000..0xffff` phy-init，是保留 Wi-Fi/激活配置的首选；但仍会改写分区表与 OTA data。不要加 `erase-flash`。

`merged-binary.bin` 的 NVS/phy 区间均为 `0xFF`；从 `0x0` 写完整 merged 会擦除已有 NVS/phy 数据并要求重新配网/激活。两种烧录方法必须二选一，不能混用。

### 7. 协议边界

- WebSocket 的 `SupportsCorrelatedSessionOpen=true`、`SupportsStrokeVoiceRouting=true` 是静态 transport capability；初始化日志不证明连接或会话已成功。
- 真机验收标准必须是完整的 **Connect → Speak → 携带本轮合法 `session_id` 的 STT → Candidates**。当前包尚无该动态证据。
- MQTT/UDP 两项 capability 均为 false，不伪造 open identity；源码在无安全语音路由时直接使用 SPY1 `AppendTopRanked` 的本地候选，最多 6 个，不打开 stroke 语音 channel。

### 8. 许可与隐私问题

**对外/商用发布阻断：** 当前材料明确是技术原型。APL 和 Unicode License 完整随包，NOTICE 固定 HWD commit、转换日期与修改方式；但字符 selection 使用的第三方 transcription aid 明确“checkout 中无显式许可证”，尚未做双人逐页官方 PDF 核对、APL/Unicode 正式法律审查或 2000 字逐字笔顺准确性审查。故本批准只允许本地研发/真机验证，不批准外部分发或商用发布。

**Minor privacy finding：** 包内 `xiaozhi.map` 可直接读到 `/Users/mandyw/...` 本地绝对路径，README/manifest 还记录 `/private/tmp/xiaozhi-stroke-2000-final-build`。这些不是凭据，不影响烧录，也不推翻“无 NVS/明文密钥命中”，但公开分发前应移除 ELF/map 或使用路径重映射构建，避免泄露构建机用户名和目录结构。

## 真机剩余风险

尚未验证：实际启动、FT6336 坐标/旋转与 overlay 独占、音频 DMA、动态 WebSocket 链、MQTT-only fallback、PSRAM 初始化、8 分片 mmap/lease、动画 FPS、WDT/audio underrun，以及 100 次会话 heap/timer 长稳。烧录前应确认设备确为 16 MB M5Stack CoreS3并备份重要配置。

- 真实 package 目录含 56 个文件；顶层/runtime/audit 校验分别闭合覆盖 55、14、28 个 payload，保留日志还证明 ZIP CRC、路径安全、解压 checksum 与原目录 byte diff 全通过。
- 精确 ZIP 为 `dist/m5stack-core-s3-stroke-2000-prototype.zip`，大小 38,449,270 bytes，SHA-256 `7eb600eba44509ccb3c6666323d6cd884172e5447482c74121a4a2c74aec7773`。
- app/assets/merged 分别为 2,914,784、7,568,207、15,956,815 bytes；assets 与 16 MiB flash 末端余量均为 820,401 bytes。
- 2000 字 runtime 恰好 15 文件；8 个 SOB1 共 5,800,492 bytes，SCB1 hash 为 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`，SPY1 hash 为 `4597a2a7134d4d40e5b025b339d3491bbcaf05f75f7485dee4db7555429b6399`。
- 多文件 `@flash_args` 不覆盖 NVS/phy；merged 从 0x0 写入会以 FF 覆盖并清除这些区域。
- 保留测试证据为全量 127（126 pass、1 conditional skip）和快速 33（32 pass、同一 skip）；本独立审计因只读要求未重新运行测试或 shell。
- WebSocket capability 只是静态声明；动态验收必须观察 Connect→Speak→合法 session STT→Candidates。MQTT capability 保持 false，并降级为本地 TopRanked 候选。
- 包内没有发现 NVS 或明文凭据，但 `xiaozhi.map` 暴露 `/Users/mandyw/...` 构建路径，属于公开分发前应处理的低等级隐私元数据。
