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

<!-- pi-squad:bf53a20b2408a1a458ff76c21f1320c61c1f67d561f4497d19aaf3a142792d07 -->
## 2026-09-13T02:57:16.075Z — stroke-interaction-improvements-rai-final-review

# Independent final review — `stroke-order-interaction-improvements`

## Verdict: REJECTED

**Release status:** not approved.

- Blocker: 0
- Major: 3
- Minor: 0
- Release-evidence caveat: 1

I reviewed the live shared-checkout production sources and task-relevant harnesses directly. I also inspected the retained raw `git status`/diff output, test output, sdkconfigs, and build logs. I did **not** edit, stage, commit, push, or rerun tests: this review had read/grep access but no shell execution capability.

## Release-blocking findings

### Major 1 — `Stop()` does not fence or drain capture-side work, so stale microphone audio can survive a Stop→Start cycle

**Evidence:**

- `main/audio/audio_service.cc:252-261` makes `Stop()` bump only `stream_generation_.playback`; it clears the encode/decode/playback/testing queues but does not bump the capture generation, clear `audio_send_queue_`, or clear capture-side timestamps.
- An encode task already popped by `OpusCodecTask` can finish after `Stop()`. At `main/audio/audio_service.cc:617-632`, send admission checks only `AcceptCapture(task->capture_generation)`. Because `Stop()` did not bump capture and this branch does not check `service_stopped_`, the old microphone packet can be placed back into `audio_send_queue_` and `NotifySendQueueAvailable()` can fire after Stop.
- The testing branch at `main/audio/audio_service.cc:634-636` unconditionally refills `audio_testing_queue_` after a successful in-flight encode, also without checking stop or generation.
- Even without the race, `Stop()` never clears already queued `audio_send_queue_` packets.
- This is observable in a real restart path: `Application::UpgradeFirmware()` calls `audio_service_.Stop()` at `main/application.cc:2092` and, on failure, calls `audio_service_.Start()` at `:2103-2107`. Old capture packets can therefore cross the service pause and later be sent. This is both a lifecycle regression and a privacy issue.

The retained generation harness does not cover production in-flight **encode** across `Stop()`; its stop model covers ordinary decode admission only. A release fix needs a capture-side stop fence/drain (normally bump streaming/capture under the queue lock, clear send/timestamp/testing state, and reject post-stop send/testing completion) plus an executable Stop→in-flight-encode→Start regression test.

### Major 2 — cancel-fenced candidate/control touches can still mutate state and request sound during the deferred-abort window

**Evidence:**

- Async cancellation intentionally publishes a source-side fence before deferred main/LVGL cleanup, e.g. `main/application.cc:1110-1138` and the channel-close path around `:721-729`.
- `StrokeOrderView::CandidateClicked` at `main/stroke_order/stroke_order_view.cc:1350-1375` checks session/current generation and phase, but not `coordinator_->HasCancelFence(presented_generation_)`.
- `StrokeOrderView::ControlClicked` at `main/stroke_order/stroke_order_view.cc:1379-1394` does not check presented generation, coordinator currentness, or cancel fence at all; a successful `HandleControlLocked` immediately calls `RequestTouchFeedback()`.
- Coordinator mutators such as `MarkLocalPlayback` also validate generation/phase but not the cancel fence (`main/stroke_order/stroke_round_coordinator.cc:296-306`).

Consequently, after a network/replacement/cancel source has made the displayed round semantically stale but before the scheduled abort acquires the display lock and tears down the overlay, an old candidate, pause/step/replay/back/exit action can still be accepted and beep. That violates the explicit requirement that stale/rejected touches remain silent. The existing touch test checks call counts/source shape, and the lifecycle harness has no coordinator cancel-fence interleaving.

Both candidate and control paths need a fail-silent fence/current-session gate immediately before mutation, with revalidation before requesting feedback if mutation can race cancellation. Add a deterministic fence→touch→deferred-abort test.

### Major 3 — the new common synchronous shutdown contract is not bounded on all affected codec paths

**Evidence:**

- `AudioService::JoinWorkersIfAny()` waits forever for worker exit bits at `main/audio/audio_service.cc:1121-1139` and then polls until suspension.
- `NoAudioCodec::Write` calls `i2s_channel_write(..., portMAX_DELAY)` at `main/audio/codecs/no_audio_codec.cc:237`.
- `NoAudioCodecSimplexPdm::Read` calls `i2s_channel_read(..., portMAX_DELAY)` at `main/audio/codecs/no_audio_codec.cc:372`.
- A worker blocked in either call cannot observe `service_stopped_`, publish its exit bit, or be joined. `Stop()` does not first perform a codec-side cancellation that can safely unblock these calls.

The reviewed CoreS3 path is better: its `esp_codec_dev` I2S implementation uses a 1000 ms wait, and AFE fetch is bounded to 100 ms at `main/audio/engines/afe_audio_engine.cc:423-430`. The retained Lite ESP32-C3 build uses ES8311, not the unbounded NoAudio/PDM paths. Thus the representative builds do not establish the repository-wide `AudioService::Shutdown()` guarantee introduced by this common change. If release scope is strictly the supplied CoreS3 image, this is a non-CoreS3 residual; it still prevents approving the common lifecycle contract as requested.

## Requirements that pass source review

- Fixed timing is implemented by `kStrokeDurationMs = 1000` and `kGapMs = 160` (`main/stroke_order/stroke_order_controller.h:60-62`). `StrokeOrderController::Tick` consumes reveal/gap overflow through a bounded phase loop (`main/stroke_order/stroke_order_controller.cc:380-438`). `AnimTimerCb` uses monotonic `esp_timer_get_time()` and carries sub-millisecond remainder (`main/stroke_order/stroke_order_view.cc:1476-1508`), so ordinary delayed timer callbacks do not introduce per-tick drift.
- Pause/continue, step, replay, back, and exit controls remain instantiated and clickable on the animation page. Controller methods accept the required animating/paused states; back and exit synchronously stop/destroy animation timing through the view path. Step retains a 120 ms bounded debounce and no action queue.
- Accepted entry/candidate/control paths request one feedback handoff; invalid indices, invalid state, failed selection, and debounced steps do not call feedback. The cancel-fence race above is the uncovered exception.
- `TryPlayUiFeedback` returns immediately after a failed `try_to_lock` before protected generation/queue/in-flight reads (`main/audio/audio_service.cc:984-1007`). UI admission uses the two-slot fixed ring and a service-owned immutable PCM vector, with no per-request PCM copy/allocation.
- UI admission rejects when ordinary decode is queued or in flight. The owning `unique_ptr` is moved only on successful ordinary insertion; capacity failure retains ownership. Sustained UI requests cannot refill a slot while ordinary decode is pending/in flight.
- Feedback admission does not reset the decoder or bump stream generation.
- Final destruction ordering is otherwise coherent on the inspected CoreS3 path: `Application::~Application()` calls `AudioService::Shutdown()` before deleting the Application event group (`main/application.cc:94-108`); service shutdown performs Stop→service joins→engine shutdown→callback detach (`main/audio/audio_service.cc:268-289`); the destructor repeats shutdown before engine/event/codec destruction.
- AFE shutdown sets its fence, joins bounded-fetch processing, shuts down CustomWakeWord, joins native encoding, then clears callbacks (`main/audio/engines/afe_audio_engine.cc:211-235`). Static wake encoder tasks signal only after final owner access and are deleted after reaching suspended state. Null task handles avoid impossible waits after creation failure.
- `CustomWakeWord::Shutdown()` is repeatable and joins its optional encoder before callback/resource destruction (`main/audio/wake_words/custom_wake_word.cc:167-175`).
- LiteAudioEngine has no independent worker; after the AudioService input worker joins, it stops/detaches WakeNet, clears output under its mutex, and clears callbacks (`main/audio/engines/lite_audio_engine.cc:46-61`). Sequential Start-after-final-Shutdown is rejected by the service shutdown fence.

No additional concrete CoreS3 worker-exit, fixed-slot ownership, callback-after-engine-destruction, or task-create-null-handle defect was found in the inspected source. The three findings above nevertheless prevent release approval.

## Retained validation evidence (not rerun by this reviewer)

The raw retained run output shows:

- Focused lifecycle/admission/lock/timing/UI selection: **7/7 passed** in 5.914 s.
- Full host suite: **131 run, 130 passed, 1 skipped** in 27.981 s. The skip is the optional clean pinned transcription fixture.
- Scoped clang-format, Python compile, and feature diff checks reported success. Whole-worktree `git diff --check` still reported unrelated trailing whitespace in `.squad/agents/tester/history.md`.
- ESP-IDF **v6.0.2** feature-on CoreS3 clean build directory: app `0x2c98f0`, 29% partition free; merged image SHA-256 `79edf86bde35ce32030d7e356de5b555e6e96e4bc190cb334be417ca7c56ae51`.
- Feature-off CoreS3: app `0x2bd470`, SHA-256 `7fcc1a3bf0af9cb4a980675832dfdf9ffa5ba7bf05f9caefe6e9c7c542632ab4`.
- Representative Lite ESP32-C3: app `0x25d6f0`, SHA-256 `be59322f12e1f7348c1d7d6de360ed6e5bac821154a3f711c49dd35522a6476a`.
- Build logs confirm the expected feature-on/off configs and compilation of AFE/CustomWakeWord versus Lite paths.

These successful tests/builds do not exercise either Major 1 or Major 2, and the shutdown harness is a host-side state model rather than execution of the production FreeRTOS join code.

## Artifact scope/provenance

The curated logical artifact file set excludes `.squad/**`, Stick-S3 sources, emoji assets, `partitions/v2/8m_single_app.csv`, `main/CMakeLists.txt`, and `scripts/build_default_assets.py`; retained `git diff --cached --name-only` was empty. However, the actual shared checkout remained dirty in all of those paths, and all three build logs show CMake sourcing `/Users/mandyw/git/xiaozhi-esp32`. Therefore the logical diff is separated, but the supplied firmware hashes were **not produced from an artifact-only clean source checkout**. In particular, the merged image includes generated assets from the dirty shared source. I cannot certify from this evidence that the flash image is bitwise free of unrelated CMake/default-assets effects. After fixes, rebuild from an isolated clean base plus only the reviewed artifact.

## Missing tests and residual hardware risks

Required before approval:

1. Production-shaped in-flight encode across Stop, including already queued send data, testing encode completion, Stop→Start, no callback/no stale packet.
2. Coordinator cancel fence published, then candidate and each animation control clicked before deferred abort; assert no mutation and no feedback request.
3. Actual FreeRTOS lifecycle/failure injection for never-started, partial task creation, already-stopped, repeated Shutdown, and shutdown during in-flight UI output; the current harness only models phases.
4. Bounded I/O shutdown test or finite codec I/O contract for affected NoAudio/PDM boards, if the common change is released repository-wide.
5. Artifact-only rebuild and hash from an isolated clean checkout.

Physical CoreS3 validation remains outstanding: actual 1000 ms visual cadence under LVGL load; pause/step/replay/back/exit responsiveness during rendering; single-fire touch behavior; feedback latency/loudness and TTS overlap; codec/DMA shutdown latency; 100-session heap/timer/WDT/audio-underrun behavior. No physical task-creation-failure or shutdown stress was retained.

- `AudioService::Stop()` currently bumps only playback generation and does not clear `audio_send_queue_`; an in-flight send encode checks only the unchanged capture generation before requeueing.
- StrokeOrder candidate/control callbacks do not consult the coordinator cancel fence; control callbacks do not validate presented/current generation before mutating state and requesting feedback.
- The fixed reveal/gap implementation uses 1000 ms and 160 ms constants, consumes delayed-tick overflow, and the LVGL timer derives elapsed time from the monotonic microsecond clock with carried remainder.
- The inspected CoreS3 codec path has 1000 ms I2S vendor waits and AFE fetch is bounded to 100 ms, but common NoAudio/PDM codec paths still contain `portMAX_DELAY` I/O while service join waits indefinitely.
- Retained evidence shows focused 7/7 and full 130-pass/1-skip results plus successful IDF 6.0.2 CoreS3 feature-on/off and Lite C3 builds; these were retained author executions, not rerun in this read-only review.
- The logical artifact path list excludes Stick-S3/emoji/8m/CMake/default-assets changes, but the retained firmware was built from the still-dirty shared source checkout rather than an artifact-only clean checkout.

<!-- pi-squad:ca4b0a20283506a8b0bd21bf2ba5c65295d0a8337a704af6340864a3ab9491ee -->
## 2026-09-13T04:02:29.092Z — stroke-interaction-improvements-systems-review

# Independent strict review — `stroke-order-interaction-improvements`

## Verdict: **REJECTED**

No Blocker; **3 Major** and **2 Minor** findings. I inspected the actual isolated source under `/tmp/stroke-systems-isolated`, corresponding shared-tree files, the frozen patch, manifests, and retained logs. I made no edits and, because only read tools were available, ran no tests/builds/hashes.

## Findings

### Major 1 — pre-stop AFE PCM can be relabeled as new capture after Stop→Start

**Files:** `main/audio/audio_service.cc:153-176,715-733`; `main/audio/engines/afe_audio_engine.cc:423-450,487-520`.

`AudioService::Start()` joins only the service input/output/Opus tasks. It does not join or obtain a quiescence acknowledgement from the separately running AFE processing task before clearing `service_stopped_`. The AFE worker validates `control_generation_` before entering `HandleVoiceResult()`, but the generation is not carried to `output_callback_`.

A valid interleaving remains: old AFE work passes the checks at lines 423-450 and is descheduled inside/before `HandleVoiceResult`; Stop fences/drains the service; Start joins service workers, disables engine controls, resets codec cancellation, and reopens service admission; a new listening start sets the service processor bit; then the old AFE handler resumes at lines 516/520. `PushTaskToEncodeQueue()` sees a running/current processor and stamps the **current** capture generation at `audio_service.cc:733`. Old PCM can therefore be accepted and sent as new-generation audio.

The retained CoreS3-on sdkconfig has `CONFIG_USE_AUDIO_PROCESSOR=y`, so this is the primary firmware path. `audio_capture_stop_harness.cc` models an in-flight Opus encode, not the production AFE producer/callback, and cannot prove this boundary.

### Major 2 — production AFE VAD state has a cross-task data race

**Files:** `main/audio/engines/afe_audio_engine.h:72`; `main/audio/engines/afe_audio_engine.cc:282-292,487-503`.

`is_speaking_` is a plain `bool`. The application/control task writes it at line 290 while the AFE processing task reads/writes it at lines 495-503. No mutex, atomic, or task-owned deferred reset synchronizes these accesses. This is C++ undefined behavior on the dual-core target and can also produce a stale VAD callback after voice processing is disabled. The retained TSAN harnesses do not compile or execute production `AfeAudioEngine`.

### Major 3 — CoreS3/K10 read errors are reported as complete microphone frames

**Files:** `main/audio/audio_codec.cc:17-21`; `main/boards/m5stack/core-s3/cores3_audio_codec.cc:234-238`; `main/boards/dfrobot/df-k10/k10_audio_codec.cc:178-183`; retained dependency `managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c:713-717`.

The new base `InputData()` exact-size check works only when the virtual `Read()` returns the true count. CoreS3 and K10 call `esp_codec_dev_read()` through `ESP_ERROR_CHECK_WITHOUT_ABORT` and then unconditionally return the requested `samples`. The locked ESP-IDF 6 dependency returns `ESP_CODEC_DEV_DRV_ERR` when its finite 1000 ms I2S read fails, but these codecs discard that result. `InputData()` consequently accepts a partial/zero-filled frame and forwards it to AFE. This violates microphone-frame validity on the actual CoreS3 path and is source-demonstrable, not hardware-only.

### Minor 1 — changed direct-output return semantics remain inconsistent

**Files:** the three changed LilyGO codecs at `tcamerapluss3_audio_codec.cc:152-162`, `tcircles3_audio_codec.cc:132-142`, and `tdisplays3promvsrlora_audio_codec.cc:141-151`, plus `df-k10/k10_audio_codec.cc:192-210`.

The LilyGO methods ignore `WriteI2s()` status/moved bytes and always return the requested count. K10 returns the number of duplicated `int32_t` entries, so a full write reports twice the input sample count. `AudioCodec::OutputData()` currently discards this return, limiting impact, but partial/error semantics are not coherent and timeout truncation is silent.

### Minor 2 — transient coordinator contention can permanently disarm Candidate/Exit

**File:** `main/stroke_order/stroke_order_view.cc:1322-1367`.

`CandidateClicked()` and Exit disarm their LVGL target before calling the intentionally nonblocking `TryUiAction`. A benign try-lock failure leaves the control non-clickable with no re-arm, even when no cancel fence exists. Disarm after successful admission or re-arm non-cancel rejection.

## Prior Major disposition

1. **Stop→Start capture fencing/drain — NOT RESOLVED.** The queue-side work is correct: Stop publishes stopped and bumps both generations under `audio_queue_mutex_`, clears encode/send/decode/testing/timestamps/playback, wakes waiters, and post-Opus send/testing admission validates stopped+capture generation under that lock. Codec cancellation is reset only after service-worker join. But the live AFE producer is not fenced/acknowledged, so old PCM/VAD callbacks can cross restart and be relabeled.
2. **Cancel-fenced UI — RESOLVED for the required controller/coordinator/abort/beep invariant.** `TryUiAction()` holds the coordinator lock, validates exact generation, active phase, transition, and cancel fence before invoking the mutation; `StrokeOrderApplyUiAction()` validates the exact presentation session inside that critical section. Candidate, pause/continue, step, replay, back, exit, and retry routes use it. A second feedback admission suppresses sound when cancellation wins after mutation but before feedback. Fence-first rejection produces no controller/coordinator mutation, abort request, or beep. Exit uses the bounded two-entry handoff. Minor 2 remains a usability issue.
3. **Finite codec I/O — NOT FULLY RESOLVED.** `AudioCodecIo` correctly uses 240-byte chunks, 200 ms per driver wait, cancellation between chunks, and termination on error, impossible moved count, or zero progress. Common NoAudio/PDM and all four changed direct-I2S codec files use the wrappers, and no direct `portMAX_DELAY` remains there. Direct microphone reads reject incomplete frames. However, CoreS3/K10 vendor-read failures are falsely accepted as full frames, and changed output methods do not consistently report partial/error results.

## Other behavior rechecked

- Fixed 1000 ms reveal and separate 160 ms gap are present; `Tick()` carries elapsed time across phases with a bounded loop.
- Controller pause/resume/replay are idempotent; step has 120 ms debounce and no queue.
- CoreS3 touch uses one atomic packed sample and one-action-per-press/release sequencing; consumed/exclusive touches do not fall through to legacy toggle.
- The 40 ms/1600 Hz tone is generated once. UI admission is try-lock, allocation-free, bounded to two fixed slots, rejects while ordinary decode is pending/in flight, and preserves ordinary `unique_ptr` ownership on capacity failure.
- Failed feedback try-lock returns before protected generation/queue reads.
- Final shutdown ordering is substantially sound: Application shuts AudioService before deleting its event group; service workers join before engine shutdown; AFE/custom wake encoder workers signal, park, and join before callback detach and owned-state destruction. This final-shutdown result does not repair restart quiescence.
- Lite-path shutdown is covered structurally after the service input producer joins. CoreS3 compiles the affected AFE path.

## Isolation, binary, and build provenance

Retained audit evidence reports base HEAD `7b455c74149d401e88fa729306391c10d9e73f0b`, an exact **43/43** patch path set, shared/isolated/clean-HEAD-apply source matches against `SOURCE-SHA256SUMS`, and artifact-scoped `git diff --check` success. The patch SHA-256 is reported as `a304b1c2b6c6a8a4e55f5af5d5a12173d436f3856bcf41f15908208c17f4e8bd`.

The path list excludes dirty Stick-S3/dictation work, emoji assets, the 8m partition, `main/CMakeLists.txt`, `build_default_assets.py`, and Squad state. `isolation-audit.log` reports those tracked exclusions unchanged/others absent.

`stroke_cat.bin` is necessary: unchanged `main/CMakeLists.txt:1053-1082` names it as a prototype source dependency and packaged runtime output. The binary patch contains a complete 24,352-byte literal. Retained audits report SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4` in shared, isolated, and clean-HEAD apply trees, matching committed `prototype_2000/SHA256SUMS` and `runtime.json`. Metadata consistently identifies SCB1 v1, 2,000 characters, eight shards, and prototype/non-commercially-reviewed status.

Retained evidence reports all **63/63** saved payload hashes valid (CoreS3-on 15; four other variants 12 each), dependency audits of 9,300 files and the complete PDM set of 10,629 files with zero mismatches, and builds rooted at `/private/tmp/stroke-systems-isolated` (the `/tmp` worktree path) with ccache disabled. These are retained author results; this reviewer could not independently recompute hashes.

## Retained validation (not reviewer-run)

- Full host log: **134 tests**, **OK**, no skip marker; 32.866 s.
- Focused TSAN log: **3 tests**, **OK**.
- ESP-IDF v6.0.2 clean build/merge logs: CoreS3 feature-on/off, bread-compact ESP32 NoAudio+Lite, doit-s3-aibox PDM+AFE, and DF-K10 direct-I2S+AFE all complete.
- Feature-on image: `/tmp/stroke-systems-evidence/cores3-on/merged-binary.bin`, 15,956,815 bytes, SHA-256 `87cb40ea427534730c0e4f51530adeaad558b2bdd3a68df870c80e7d05058dbc`; app SHA-256 `8227d03f06861314a6c5063d46d340c2fb89977ec9eb7a5532b245a310cf8ece`; assets SHA-256 `54453b5232408dfb8cf8dadbde9dd5d93a1bdb90f213df69d7ff075210783dd0`.
- The canonical CoreS3 ZIP was later overwritten by feature-off validation; only the separately retained `cores3-on/merged-binary.bin` is the feature-on image.

The green tests do not execute the production AFE restart interleaving or CoreS3 `esp_codec_dev_read()` failure path.

## Missing validation and physical residuals

Before re-review: add a deterministic AFE callback/VAD Stop→Start interleaving test; test actual synchronized `is_speaking_` ownership; inject CoreS3/K10 read timeout/error results; compile the three changed LilyGO targets; rerun host/TSAN/build/hash evidence.

Hardware-only work remains: CoreS3 repeated Stop→Start and missing-clock stress; animation controls and 20/30 FPS under load; feedback audibility/latency and ordinary-audio priority; capture/playback/wake/VAD/AEC; 100+ sessions for heap/timer/task leaks, WDT, underrun, and dead touch. Commercial licensing and per-character accuracy review remain outstanding for this explicitly technical prototype.

- AudioService Stop/Start joins only service input/output/Opus tasks; the AFE processing task remains live and its output callback carries no capture generation.
- The retained CoreS3-on sdkconfig enables CONFIG_USE_AUDIO_PROCESSOR, making the AFE producer path relevant to the primary image.
- AfeAudioEngine::is_speaking_ is a plain bool accessed from the control and AFE processing tasks without synchronization.
- CoreS3AudioCodec and K10AudioCodec discard esp_codec_dev_read errors and return the requested count, defeating the base exact-frame check.
- The artifact path manifest has 43 entries and explicitly includes the required 24,352-byte stroke_cat.bin; retained evidence reports its hash matches SHA256SUMS/runtime metadata.
- Retained evidence records 134 host tests, three TSAN helper tests, five clean builds/merges, and 63 payload hashes; no validation was rerun in this review.

<!-- pi-squad:28334d4dc1be505437b6680ac2b2893c37278e9278b9e628e69f22550e2fc0fc -->
## 2026-09-13T05:01:37.473Z — stroke-interaction-improvements-visual-review

# stroke-order-interaction-improvements — independent final strict review

## Verdict: **REJECTED**

- Blocker: 0
- Major: 1
- Minor: 0

The frozen visual-only artifact is narrow, builds successfully in the retained evidence, and its cancel-fence, visual-feedback, timeout-recovery, scope-reduction, and catalog-provenance work are otherwise credible. It nevertheless has a reproducible source-level elapsed-time loss across a benign coordinator lock miss followed by Pause. The task explicitly makes any timing defect reject-worthy.

## Major finding

### M1 — pending active elapsed time is discarded when Pause follows a missed animation tick

**Files/lines:**
- `main/stroke_order/stroke_order_view.cc:609-630`
- `main/stroke_order/stroke_order_view.cc:1217-1251`
- `main/stroke_order/stroke_order_view.cc:1262-1297`
- `main/stroke_order/stroke_order_view.cc:1443-1477`
- `main/stroke_order/stroke_order_ui_action.h:43-45`

`AnimTimerCb` correctly leaves `last_anim_tick_us_` and `anim_tick_remainder_us_` unchanged when `TryUiAction` misses the coordinator lock, so a later successful timer callback can catch up. However, Pause does not settle that pending elapsed interval. `StrokeOrderApplyUiAction` changes the controller directly from Animating to Paused, after which `HandleControlLocked` calls `HandleStatePresentationLocked`; `SyncAnimTimer` then zeros both the baseline and remainder for every non-Animating state without first applying elapsed time through `controller_->Tick`.

A feasible sequence is:
1. An accepted animation tick establishes baseline `t0`.
2. One or more timer callbacks miss the coordinator try-lock, so controller progress remains at its old value while the baseline intentionally remains `t0`.
3. The lock becomes free and a Pause click is dispatched before the next successful animation timer callback.
4. Pause is admitted, but no `Tick(now - t0)` occurs; `SyncAnimTimer` clears the baseline/remainder.
5. Resume starts a fresh baseline, permanently losing the active interval between `t0` and Pause.

Thus a stroke can require more than 1000 ms of active wall-clock playback, and benign coordinator contention can lose elapsed time. Even without contention, up to one timer interval between the last tick and Pause is dropped. This conflicts directly with requirements 1 and 4.

**Exact disposition:** do not accept or flash this frozen revision as final. Before re-review, settle pending monotonic elapsed time inside the same exact-generation/fence-protected admission that performs Animating→Paused, or otherwise preserve it across Pause while excluding time spent paused. Add a deterministic test for: accepted tick/baseline → forced benign tick lock miss → lock release → Pause before another timer tick → Resume, asserting that total active elapsed still reaches the stroke/gap boundaries at exactly 1000/160 ms. Re-freeze and re-hash the patch and rerun both host suites and CoreS3 feature-on/off builds.

## Requirement review

### 1. Fixed timing and controller carry — rejected only for M1

The controller itself is sound: `stroke_order_controller.h:59-62` defines 1000 ms reveal, 160 ms gap, 120 ms step debounce, and 33 ms tick; `stroke_order_controller.cc:386-439` consumes carry through stroke/gap phases and bounds work to `2 * kMaxStrokesPerCharacter + 2`. Equality boundaries, final-stroke completion, pause no-op ticks, replay, step, and `UINT32_MAX` are covered in `stroke_order_controller_harness.cc`. M1 is in the view-level wall-clock handoff to Pause, not in the controller loop.

### 2. Visual-only pressed feedback — pass by source inspection, hardware residual remains

`StyleControl` in `stroke_order_view.cc:72-89` uses only LVGL `LV_STATE_PRESSED`: a 30% text/background mix, text-colored border, and 3 px pressed border. Candidate and animation-control creation both call `StyleControl`; dispatch remains `LV_EVENT_CLICKED`. No feedback timer/queue was introduced. Searches of the actual stroke-order sources found no `PlaySound`, tone, feedback event, or audio request; the six removed audio/lifecycle helper headers are absent. Actual visibility/contrast in both themes remains a physical CoreS3 check.

### 3. Controls during animation — pass apart from M1's Pause timing

All five animation controls remain present and clickable. The production dispatcher covers pause/continue, step, replay, back, and exit; controller methods retain replay/pause/resume idempotence and 120 ms step debounce. Back renders candidates synchronously; Exit immediately stops the animation timer and fences/queues the existing abort. Candidate and Exit disarm only after successful admission, while other controls remain armed. CoreS3's unchanged touch-sequence gate continues to suppress the legacy short-touch action once an overlay/entry consumes the physical sequence.

### 4. Exact generation/session, fence safety, and locks — safety pass; elapsed-contention clause fails via M1

`StrokeRoundCoordinator::TryUiAction` (`stroke_round_coordinator.h:143-166`) uses a nonblocking coordinator lock and rejects generation 0, wrong generation, inactive phase, raised cancel fence, and invalid phase transitions before invoking mutation. `stroke_order_ui_action.h:20-73` revalidates the presentation session under that admission. Candidate/control callbacks and ticks enter through this gate; fence-first paths do not mutate controller/session/coordinator or issue Application start/abort requests. Action-first mutation and phase transition linearize under one coordinator lock, with rendering and Application calls after release.

No lock-cycle or reentrant coordinator call was found: the UI order is display/LVGL serialization → nonblocking coordinator try-lock → controller mutex; the mutation callback does not call Application or coordinator methods; Application abort releases coordinator/routing locks before entering the view/display path. The retained TSAN harness exercises fence-first, action-first, exact identity, and click contention. It does not cover M1.

### 5. Speech-timeout recovery and active error controls — pass by source inspection

`ShowSpeechTimedOutFromMain` (`stroke_order_view.cc:408-425`) now closes the retired round through `CancelSessionLocked(false)`, restores entry eligibility, and then uses ordinary `ShowNotification`; it does not create a generation-0 R/X page or add an admission exception. The unchanged Application path first retires the exact generation in `AbortStrokeRound`, closes the prior overlay, returns device state to Idle when needed, then invokes the timeout notification. Active NoMatch/Error pages still route Retry/Back/Exit through nonzero exact-generation admission. A stale old control cannot act on a fresh generation.

## Scope and artifact audit

The inspected frozen patch contains exactly 12 logical paths and matches the supplied scope inventory: six production files, all under `main/stroke_order/`; three test files; one document; `.gitignore`; and `stroke_cat.bin`. `final-scope-audit.log` records the restored Application/audio/codec/other-board files and confirms no isolated diff in `main/application.*`, `main/audio`, `main/boards`, `main/CMakeLists.txt`, or `scripts/build_default_assets.py`. The isolated feature-off compile database contains zero stroke-order translation units, so no feature-off/non-CoreS3 production behavior change is introduced by this patch's source scope.

The `.gitignore` change negates only `scripts/tests/fixtures/stroke_order/prototype_2000/stroke_cat.bin` beneath the existing `*.bin` rule; it does not expose other binaries. The binary patch declares 24,352 bytes. The actual fixture's `runtime.json` records size 24,352 and SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`; committed `SHA256SUMS` records the same digest; unchanged CMake names this exact catalog as a required source dependency. The frozen evidence manifest records patch SHA-256 `23fd6980cdfcff85910ab7ce590bc88f311d647bbb500551dfbc9b9a0352198a`, and shared/isolated source-hash verification logs report all 12 paths `OK`. These are retained checksum records; this reviewer had no shell/hash execution facility and did not independently recompute them.

## Test audit and blind spots

The retained deterministic tests are useful but miss the rejecting sequence:
- `stroke_order_ui_fence_harness.cc:171-211` proves rejected clicks remain armed under contention, but it does not model `last_anim_tick_us_`, the LVGL timer callback, or Pause's call to `SyncAnimTimer`.
- `test_stroke_interaction_systems.py:62-68` only lexically asserts that the timer baseline assignment occurs after `TryUiAction`; it cannot detect the separate baseline reset performed by Pause presentation.
- Direct controller tests call `Tick` with supplied deltas, so they do not test transfer of elapsed wall time from the view to the controller.
- LVGL pressed-state rendering and real touch timing are source assertions/build coverage only, not executed host UI tests.

A deterministic view-clock/accounting seam is required to prevent recurrence. Hardware-only follow-up should still verify light/dark pressed contrast, touch latency and all controls during 30 FPS drawing, timeout-to-entry behavior, 100-session heap/timer/WDT stability, and ordinary audio regression.

## Retained validation inspected (not reviewer-run)

- Shared host suite: 132/132 PASS, no skips, 40.357 s.
- Isolated host suite: 132/132 PASS, no skips, 30.229 s.
- Focused stroke UI: 12 PASS; focused interaction/TSAN: 5 PASS.
- clang-format 19.1.7 dry-run and isolated/artifact diff checks: PASS.
- ESP-IDF v6.0.2 CoreS3 clean feature-on build, merge, and ZIP: PASS; app 2,917,920 bytes with 1,210,848 bytes free; assets 7,568,207 bytes with 820,401 bytes free.
- CoreS3 clean feature-off build, merge, and ZIP: PASS; zero stroke-order compilation units.
- Final feature-on merged image retained SHA-256: `6602c2ea033927dc75e1d0c4009f9ae76253e3d7459372b05a437cee1d16cc53`.
- Dependency verification: 72 manifests / 10,629 files / zero mismatches.

I performed no test/build/hash command, edit, staging, commit, push, merge, or flash in this read-only review. The successful retained tests and builds do not override M1 because the relevant interleaving is absent from them.

- The controller's fixed-duration loop uses 1000 ms per stroke, a separate 160 ms gap, and a phase bound larger than the maximum work needed for a validated 48-stroke glyph.
- The view commits its animation-clock baseline only after a successful coordinator-admitted tick, but `SyncAnimTimer` clears that baseline whenever the controller is not Animating.
- A Pause action does not apply pending wall-clock elapsed before changing the controller to Paused; therefore a timer try-lock miss followed by Pause can permanently discard active reveal time.
- The UI admission path rejects zero/wrong generations, inactive phases, cancel-fenced rounds, and invalid playback/candidate transitions before mutation, and revalidates the presentation session inside the coordinator lock.
- The final logical patch has 12 paths; its six production changes are confined to `main/stroke_order`, with no Application, audio, codec, board, CMake, or default-assets logical diff.
- The sole unignored binary is the exact prototype catalog path; retained runtime/SHA256SUMS data identify it as 24,352 bytes with SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`.
- Retained evidence records two no-skip 132-test host passes and clean CoreS3 feature-on/off ESP-IDF v6.0.2 builds; these were inspected but not rerun by this reviewer.

<!-- pi-squad:1e89dc570fd3ea427d299c9f6a784f5e9ea7f053172732082bdec47d882bda60 -->
## 2026-09-13T05:34:33.677Z — stroke-interaction-improvements-animation-review

# Final independent strict review — `stroke-order-interaction-improvements`

## Verdict: APPROVED

I found **no release-blocking source defect** in the frozen 12-file visual-only artifact.

- Blocker: **0**
- Major: **0**
- Minor: **1 test-evidence limitation only**; it is not a production-source defect.

Approval is for the reviewed technical-prototype artifact and matching CoreS3 firmware candidate. It is not physical-hardware acceptance, commercial-data approval, or per-character stroke-order certification.

## Prior timing Major: RESOLVED

The prior loss of pending active elapsed time on Pause is repaired correctly.

1. `main/stroke_order/stroke_order_ui_action.h:10-57` defines one display/LVGL-owned `StrokeOrderAnimationClock`. `Settle()` at lines 36-47:
   - computes monotonic delta only for an ordered sample;
   - carries fractional microseconds in `[0,999]`;
   - advances the baseline only after admission;
   - saturates whole milliseconds at `UINT32_MAX` rather than wrapping;
   - calls the bounded controller `Tick()` once;
   - synchronizes terminal/paused state afterward.
2. `StrokeOrderApplyAnimationTick()` at `stroke_order_ui_action.h:61-73` performs the exact nonzero generation, current presentation session, active coordinator phase, and cancel-fence admission before touching the clock or controller.
3. Pause at `stroke_order_ui_action.h:115-130` settles elapsed while the controller is still Animating and while the same coordinator admission remains held, then performs Animating→Paused. If settlement reaches Completed/Error, it returns an admitted result for presentation and does not call invalid `Pause()`.
4. Resume retains the fractional remainder: `Sync(Paused)` clears only the running baseline, while `Sync(Animating)` starts a fresh baseline without clearing the remainder. Paused wall time is therefore excluded. The post-action code at lines 159-164 deliberately resets timing for replay/step/back/exit/candidate/retry, but not for Pause/Resume.
5. `main/stroke_order/stroke_order_view.cc:599-629` resets only on explicit timer teardown and otherwise calls `anim_clock_.Sync`. A running clock is not rebased by presentation sync. `HandleControlLocked` at lines 1214-1261 and `AnimTimerCb` at lines 1439-1464 pass microsecond samples to the same production seam; duplicate timer arithmetic is gone.
6. Candidate/new-playback presentation intentionally invokes `StopAnimTimer()` before constructing the new animation page, then establishes the visible playback baseline through `SyncAnimTimer()`. Replay starts a new baseline at its admitted action timestamp; back, exit, abort, overlay deletion, and new-session teardown clear the clock. Pause presentation preserves the remainder, and Resume presentation cannot rebase the baseline already established by the admitted Resume action.
7. `main/stroke_order/stroke_order_controller.cc:380-436` consumes reveal/gap phases with a bounded loop. For the valid maximum of 48 strokes, the 98-iteration cap exceeds the at-most-95 iterations needed to finish all 48 reveals and 47 gaps, so a saturated delta cannot leave valid glyph elapsed stranded. The fixed reveal and gap constants remain 1000 ms and 160 ms (`stroke_order_controller.h:60-63`).

I found no elapsed loss or double counting across timer miss→Pause→Resume, no paused-wall-time inclusion, no invalid Pause after completion, and no wraparound in the validated large-timestamp path.

## Admission, concurrency, and stale-action review

`main/stroke_order/stroke_round_coordinator.h:143-166` uses `std::try_to_lock`, then rejects zero/wrong generation, inactive phase, raised cancel fence, and illegal phase transitions before invoking the mutation. The coordinator lock remains held across clock settlement, controller/session mutation, and the optional coordinator phase transition. Thus:

- fence-first and replacement-first actions cannot mutate controller/session/clock;
- action-first settlement is linearized before a later fence;
- a benign contention miss changes neither clock baseline nor remainder;
- the LVGL control remains armed after a failed admission;
- timer callbacks and Pause use the same gate;
- rendering and Application abort/start calls occur only after `TryUiAction` releases the coordinator lock.

The effective lock order is LVGL/display ownership → nonblocking coordinator try-lock → controller mutex. I found no reverse path that holds the coordinator mutex while acquiring the display lock: `Application::AbortStrokeRound` retires the coordinator round and releases its route/coordinator locks before `StrokeOrderView::AbortFromMain`. Timer/action mutation callbacks do not call Application or re-enter coordinator methods. Controller work is bounded, and no queue or retry loop was added.

## Reconfirmed requirements

- **Visual-only feedback:** `main/stroke_order/stroke_order_view.cc:77-89` applies LVGL `LV_STATE_PRESSED` background, border color, and 3 px border styling through the shared `StyleControl`. Candidate and playback controls use that style. No sound or audio feedback path appears in the 12-file patch.
- **Controls during animation:** Pause/continue, step, replay, back, and exit remain clickable on the animation page. Dispatch is only `LV_EVENT_CLICKED`; no repeated press handler or unbounded action queue exists.
- **Exact stale/fence behavior:** candidate, control, and animation tick paths all use the exact presented generation plus current `StrokeOrderSession` and coordinator fence admission. Candidate and Exit disarm only after success.
- **Benign contention:** rejected try-lock attempts do not disarm controls or change state/clock; later touches/ticks can retry normally.
- **Timeout lifecycle:** `ShowSpeechTimedOutFromMain` closes the already-retired session/overlay and reevaluates the SO entry, then shows a notification. It does not construct an unusable generation-0 retry page.
- **Controller timing:** every reveal is fixed at 1000 ms, the 160 ms gap is separate, delayed ticks carry remainder, paused ticks are inert, and large deltas are bounded.
- **Scope:** the frozen patch contains exactly the 12 paths listed in `/tmp/stroke-animation-evidence/artifact-files.txt`. Inspection of every `diff --git` entry confirms there are no Application, audio, codec, board, CMake, or default-assets changes. The final isolated status lists those same 12 paths.
- **Catalog:** `.gitignore:18-20` adds only the exact `prototype_2000/stroke_cat.bin` exception. `runtime.json:2-12` identifies the 24,352-byte SCB1 catalog, SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`, 2000 characters, and prototype/non-commercial flags. `SHA256SUMS` records the same digest. `NOTICE.md` retains the pinned Hanzi Writer Data commit, APL and Unicode provenance, transcription caveat, and explicit non-release/non-commercial limitations.

## Deterministic harness assessment

`scripts/tests/stroke_order_ui_fence_harness.cc:62-86` creates real coordinator mutex contention with condition-variable barriers. `TestPauseClock` at lines 89-231 covers:

- accepted tick, forced timer miss, then Pause before another accepted tick;
- 600.600 ms + 399.399 ms fractional carry and the next 1 µs crossing exactly 1000 ms;
- 159.999 ms gap and the next 1 µs crossing exactly 160 ms;
- settlement entering a gap, crossing a gap, and completing the final glyph;
- `UINT64_MAX`-scale elapsed and equal/backward samples;
- Pause admission miss followed by a later retry with unchanged baseline/remainder;
- fence-first and presentation-session-stale rejection;
- action-first Pause followed by fenced Resume;
- replay/back clock reset.

The harness uses the actual production clock/action/tick helpers, controller, session, and coordinator rather than a copied timing model. Mutation-sensitivity evidence shows that removing Pause settlement fails the 600 ms assertion and clearing paused remainder fails the 600 µs assertion.

### Minor — test/model limitation, non-blocking

`scripts/tests/stroke_order_ui_fence_harness.cc:89-231` invokes the production helpers directly and manually calls `clock.Sync` to emulate presentation; it does not compile or execute `StrokeOrderView` against a live LVGL scheduler. `scripts/tests/test_stroke_interaction_systems.py:39-77` couples the real view to the helper using source-shape assertions, and the ESP-IDF build compiles the actual view, but neither proves real LVGL timer ordering, deletion callbacks, or FT6336 scheduling dynamically. Static inspection of `SyncAnimTimer`, control callbacks, timer callback, and teardown found their wiring correct, so this is not a source blocker. A future LVGL integration/fake-timer test would strengthen regression coverage.

## Retained validation evidence reviewed

These results are **retained author-run evidence**, not commands rerun by this reviewer:

- focused interaction/TSAN suite: **5 passed**;
- focused UI suite: **12 passed**;
- shared full host suite: **132 passed, no skips**;
- clean HEAD + frozen patch isolated suite: **132 passed, no skips**;
- additional production helper/controller/coordinator harness under UBSAN: both PASS markers, no sanitizer failure;
- mutation sensitivity: both deliberately damaged clock variants failed at the expected assertions;
- clang-format 19.1.7 scoped dry-run and artifact `git diff --check`: pass;
- dependency audit: **72 manifests / 10,629 files / zero mismatches**;
- clean artifact-only ESP-IDF **v6.0.2** CoreS3 feature-on and feature-off builds, with fullclean between variants, successful image creation, partition checks, merge-bin, and ZIP creation;
- feature-on compiled seven stroke units; feature-off compiled zero;
- feature-on app/assets sizes: 2,918,080 / 7,568,207 bytes, with 1,210,688 / 820,401 bytes spare;
- feature-on ZIP SHA-256: `9a0de13a42a16676b068d0b1938d73f9866519f8a19f5f77f3aed251d6881650`;
- feature-on merged image SHA-256: `e9327d9ce781d8981a3237d3fc1dd64a145efcdb89dab5e1673e04a6c7921068`;
- feature-off ZIP SHA-256: `dd613614accc9fcb50e368207b14881031b71548fb394e7d74ba1beb1eb2d358`.

The expected frozen patch digest `2686ac777f77fc887d0ca409dbaabbdf1982c4217bac7852f08603ad8609deb5` is recorded in the retained `EVIDENCE-SHA256SUMS`; the 12 source hashes are consistently reported for shared and isolated checkouts. I inspected the complete patch, actual shared source, key isolated source, manifests, and logs. Because this review environment exposes read/search tools only, I did not independently execute tests/builds or recompute digests. The expected negative-test message `assets safety margin too small: 262143 < 262144` appears after the full-suite `OK` and belongs to the intentional boundary-rejection test, not to either firmware build.

## Remaining acceptance gates

- Flash and exercise the feature-on image on M5Stack CoreS3.
- Verify pressed-state contrast in both themes, animation-time touch responsiveness, ≤100 ms perceived feedback, approximately 30 FPS target / 20 FPS sustained floor, rapid Pause/Resume/step/replay/back/exit, and real cancel/new-session races.
- Run the 100-session heap/timer/touch/WDT/audio-underrun stability gate and confirm no button sound plus unchanged ordinary capture/TTS/wake behavior.
- Do not treat the 2000-character corpus as a release or commercial library until the documented APL/transcription/Unicode review and manual per-character stroke-order verification are complete.

- The repaired production seam in `stroke_order_ui_action.h` settles pending elapsed and performs Animating→Paused under one exact-generation/session/cancel-fence `TryUiAction` admission; fence-first or contention-first rejection cannot alter clock or controller state.
- `StrokeOrderAnimationClock` preserves only the `[0,999]` µs active remainder while paused, starts a fresh baseline on Resume, does not rebase a running clock during `SyncAnimTimer`, and resets on terminal/new-playback/teardown actions.
- For the valid 48-stroke maximum, `StrokeOrderController::Tick` needs at most 95 reveal/gap iterations to finish a glyph; its 98-iteration cap therefore remains sufficient for saturated elapsed input.
- The frozen artifact patch enumerates exactly 12 paths and contains no Application, audio, codec, board, CMake, or default-assets logical changes.
- The required SCB1 catalog is 24,352 bytes with recorded SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`; its manifest and NOTICE explicitly retain prototype, provenance, and non-commercial-review limitations.
- Retained final evidence reports 132/132 host tests in both shared and isolated checkouts, focused TSAN and UBSAN passes, clean ESP-IDF v6.0.2 CoreS3 feature-on/off builds, and feature-on ZIP/merged hashes `9a0de13a...81650` / `e9327d9c...21068`.
- The production-shaped host harness directly exercises the shared helper/controller/session/coordinator seam, but actual LVGL timer and FT6336 scheduling remain outside its dynamic coverage.

<!-- pi-squad:db77a8239978d9689d91f21606627ab99e8af79a327d7a151766f75cf8792e32 -->
## 2026-09-13T06:19:34.291Z — stroke-order-sequential-animation-review

# Independent safety/reliability review — sequential stroke visibility

## Verdict

**APPROVED** for the frozen source artifact `stroke-order-interaction-improvements`.

- Blocker: **0**
- Major: **0**
- Minor: **0**

I found no deterministic multi-stroke update, hidden whole-millisecond backlog, timing/control/fence regression, scope mismatch, or provenance inconsistency in the inspected source and retained evidence. The skip/burst defect is **closed at the controller/model and redraw-opportunity level**. Approval does not claim physical LCD visibility; CoreS3 panel validation remains a hardware gate.

## What I independently inspected

Read-only inspection covered the actual shared tree, `/tmp/stroke-sequential-isolated`, the complete author report, the frozen patch, the pre-repair snapshots, repair-only diff, final controller/UI-fence harnesses, Python test drivers, scope/source manifests, TDD logs, sanitizer logs, full-suite logs, clean-build logs, image audits, dependency audits, and package checksum manifests. I made no edits and did not stage, commit, push, flash, or publish anything.

The frozen patch has exactly the required 12 diff headings. The repair-only diff has exactly seven paths: documentation, controller `.cc/.h`, `stroke_order_ui_action.h`, both C++ harnesses, and the interaction Python test. `SOURCE-SHA256SUMS` plus the retained shared/isolated hash audits report all 12 paths as matching. The evidence checksum manifest records the required patch SHA-256 `81252835e03c4ea0116b0cb91405831f762cd2910edc96a098635fb7167da083`; the catalog remains `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`. The isolated status contains exactly those 12 artifact paths, and the scope audit reports zero logical diff under Application, audio, boards, CMake, or default assets. Unrelated shared-tree dirt was excluded rather than reverted.

## TDD and test integrity

The retained author-session chronology shows the exact regression was inserted into `scripts/tests/stroke_order_ui_fence_harness.cc` before production timing was changed, then the focused test command was run. The pre-repair production clock passed the full elapsed value into the production controller, whose old phase loop could traverse the glyph.

The red run used the real `StrokeOrderController`, `StrokeRoundCoordinator`, `StrokeOrderSession`, `StrokeOrderAnimationClock`, and deterministic coordinator contention. It failed with harness return `-6` and:

- `mode=0 done=0->2`
- `mode=1 done=0->2`
- `mode=2 done=0->2`
- `one update completed multiple strokes`

The original assertion remains at `scripts/tests/stroke_order_ui_fence_harness.cc:150-156`: `after <= before + 1`. It is strengthened, not weakened, by requiring stroke index 0 and exact progress 283 after the delayed timer, timer-miss retry, and timer-miss→Pause modes. Thus a no-op or merely suppressed completion cannot game the regression. `AssertAdjacent` at lines 30-39 also bounds every admitted timer settlement and Pause settlement to one adjacent phase snapshot.

Final retained TSAN and UBSAN harness logs both return 0, print all four PASS markers, and retain `mode=0/1/2 done=0->0`. Focused retained results are 6 interaction tests and 12 UI tests passing. Shared and clean isolated full runs each report `Ran 133 tests` and `OK`, with no skips. The mutation probes fail when Pause settlement is removed, fractional remainder is dropped, baseline rebase is removed, or the public `Tick` cap is removed; the last mutation triggers 26 controller-harness failures including prior-partial-frame checks.

## Timing and sequential-visibility analysis

### Bounded clock settlement

`main/stroke_order/stroke_order_ui_action.h:33-48` computes elapsed time without adding an unbounded delta to the prior remainder, advances the baseline to the admitted monotonic sample, retains only `(delta_us % 1000 + old_remainder) % 1000`, and passes at most `kTickMs == 33` to the controller. All excess whole milliseconds are discarded. There is no whole-ms debt field, retry queue, or compensating callback. Equal samples add no time; backwards samples neither rebase nor debit the clock. A successful delayed callback therefore cannot cause later catch-up.

The timer helper at lines 62-77 performs settlement only inside `TryUiAction` after exact nonzero generation, active coordinator phase, cancel-fence, presentation-session, and Animating-state checks. A try-lock miss, stale session, or fence returns before clock/controller mutation.

### Defensive public controller bound

`main/stroke_order/stroke_order_controller.cc:386-443` independently caps public `Tick(dt_ms)` to 33 ms and limits processing to two phase visits. The compile-time condition requires `33 < 160` and `33 < 1000`. Therefore one call may remain in its phase, finish reveal and enter its gap, or finish a gap and enter the next reveal; it cannot cross a second boundary or complete a newly entered reveal. Boundary arithmetic is sound:

- reveal 990 + huge delta becomes completed reveal plus 23 ms of gap;
- gap 150 + huge delta becomes the next reveal at 23 ms;
- exact reveal completion presents the completed stroke at gap 0;
- exact gap completion presents the next current stroke at progress 0;
- repeated huge deltas still require separate calls and redraw opportunities.

Final-stroke completion is permitted only when that final stroke was already partial; single-stroke and multi-stroke cases are covered. Zero and completed-state ticks remain no-ops.

### Partial-frame opportunities and presentation

Candidate selection creates the animation page and redraws progress 0 with the current-stroke start marker. Each admitted timer update then calls `RedrawCanvas()` exactly once at `stroke_order_view.cc:1439-1462`. A gap→next-reveal crossing redraws the new current stroke with its bounded partial progress; a reveal→gap crossing redraws the just-completed stroke. The final harness requires every automatically completed stroke of 一, 人, and 口, under normal 33 ms cadence and adversarial huge deltas, to have an earlier `0 < progress < 1000` snapshot. Repeated public huge deltas require 101 separate settlements to complete 口.

Explicit `StepForward` remains a single debounced stroke action and creates no queue. Replay resets playback and clock timing, then establishes a fresh baseline. The next 1 ms update after Replay yields progress 1 rather than old debt.

## Pause, fences, controls, and lock ordering

`PauseContinue` settles through the same bounded clock while still inside the exact coordinator admission, then pauses. It can cross only one adjacent boundary; if it completes the already-partial final stroke, the action presents Completed rather than forcing an invalid Paused state. Paused wall time is excluded: Paused clears the running baseline but preserves the fractional microsecond remainder; Resume starts a fresh baseline at its admitted timestamp. The harness covers timer miss→Pause, Pause admission miss/retry, fractional carry, paused-time exclusion, reveal/gap boundary cases, final completion, stale session, cancel fence, and fresh generation.

The lock order remains display/LVGL serialization → nonblocking coordinator lock → controller lock. The `TryUiAction` callback does not call Application or another coordinator method; rendering and Application abort/start requests occur only after coordinator lock release. Fence publication takes the coordinator lock and therefore linearizes before or after the whole mutation. Rejected actions are mutation-free and candidate/Exit controls are disarmed only after admission; Pause/Continue, Step, Replay, Back, and Exit remain callable while animation is active. No new timer, retry loop, deferred action queue, or unbounded allocation was introduced.

The artifact remains visual-only. `StyleControl` supplies LVGL `LV_STATE_PRESSED` background/border feedback to candidate and playback controls. No stroke feedback sound, audio event, audio queue, or broad audio/board helper appears in the patch. The timeout path closes the retired overlay and restores a fresh SO-entry retry instead of creating generation-0 controls.

## Retained validation and build evidence

These are **author-run retained results that I inspected**, not commands I reran:

- focused interaction: 6 passed;
- focused UI/controller: 12 passed;
- standalone final TSAN and UBSAN harnesses: return 0;
- shared full host suite: 133/133, no skips, 40.802 s;
- clean isolated full host suite: 133/133, no skips, 40.670 s;
- touched-range clang-format 19.1.7 checks, diff check, and reverse-apply check: pass;
- ESP-IDF v6.0.2 clean CoreS3 feature-on build: success, 7 stroke units, only CoreS3 factory;
- fullclean, then feature-off build: success, 0 stroke units, only CoreS3 factory;
- feature-on app/assets: 2,918,096 / 7,568,207 bytes;
- feature-off app/assets: 2,868,448 / 1,664,169 bytes;
- merged flash slices and app/bootloader image checksums: valid for both variants;
- dependency audits: 72 manifests and 10,629 files with zero mismatches;
- feature-on ZIP SHA-256: `732fd3a1c4ef72114f0c3964470acf22fd3f4d8fa7303f0d5312a965753969ae`;
- feature-on merged SHA-256: `1c627e61ac6f4420d5ff61476976fbbcb93d275e1267e9ef779e461b6a50b2c7`.

Because this review had no shell/test-execution tool, I did not independently rerun tests/builds or recompute cryptographic hashes. I verified source logic directly and cross-checked the retained logs/manifests and actual shared/isolated file contents.

## Hardware residuals — not waived by approval

No CoreS3 was flashed in this run. Host state and redraw tests prove sequential controller states and a redraw opportunity after each admitted settlement; they do not prove that LVGL flushes every intermediate canvas image to the LCD. Under load, canvas invalidation/flush coalescing could still hide a physical frame. The intentionally visible light complete-reference outline can also be perceived as an already completed character.

Physical acceptance must still verify normal and stalled playback in both themes, distinguish reference/current/completed strokes, exercise Pause/Continue/Step/Replay/Back/Exit during animation, test stale-touch/cancel races and contention, and measure touch latency, sustained FPS, heap/timer stability, WDT behavior, and ordinary audio behavior across 100 sessions. These are hardware/presentation gates, not remaining deterministic source defects in the reviewed repair.

- The final production clock rebases every admitted forward sample, discards all excess whole milliseconds, preserves only a sub-millisecond remainder, and credits at most 33 ms; rejected timer/Pause admissions leave clock and controller state unchanged.
- The public controller independently caps Tick to 33 ms and visits at most two phases; with 33 ms below both the 160 ms gap and 1000 ms reveal, one call cannot cross more than one adjacent boundary.
- The exact red regression used production controller/coordinator/session/clock code and real coordinator contention; its original at-most-one-completion assertion remains and final tests additionally require stroke 0 at progress 283.
- Automatic 一/人/口 playback tests require a prior partial current-stroke snapshot before every completion under normal and huge delayed deltas; Replay clears timing debt and Step remains one debounced explicit stroke.
- Retained clean evidence reports 133/133 host tests in both shared and isolated trees, successful ESP-IDF v6.0.2 CoreS3 feature-on/off builds, and the reviewed feature-on ZIP/merged hashes; no physical panel validation was performed.

<!-- pi-squad:2840d07b4187768c0b8ff659b10cf5e29d465305182af500b5a2dc8109ac704e -->
## 2026-09-13T10:02:56.806Z — stroke-animation-speed-review

# Final independent strict review — 150 ms animation calibration

## Verdict

**APPROVED** for `stroke-order-interaction-improvements`.

- Blocker: **0**
- Major: **0**
- Minor: **0**
- Speed regression: **source-correct and covered by the retained production-shaped red/green test**.
- Skip/burst safety: **source-correct; one settlement cannot cross more than one adjacent boundary or complete an unseen next stroke**.

This was read-only. I inspected the actual shared source, frozen patch, author report, preserved before/final sources, test/mutation logs, build/package audits, and provenance files. I made no edits and did not stage, commit, push, flash, or publish. I had no shell/test runner, so I did not rerun tests or independently calculate hashes; all execution/hash results below are clearly identified as retained author evidence and were cross-checked against source and audit-script logic.

## Source correctness

### Speed and clock

- `main/stroke_order/stroke_order_controller.h:60-67` fixes reveal/gap at 1000/160 ms, retains `kTimerPeriodMs=33`, and separately defines `kMaxAnimationAdvanceMs=150`.
- `main/stroke_order/stroke_order_view.cc:608-628` uses 33 ms at both LVGL create/set-period sites. `main/stroke_order/stroke_order_view.cc:1441-1462` routes real timer timestamps through the tested animation helper.
- `main/stroke_order/stroke_order_ui_action.h:36-46` caps the production clock at 150 ms. Each admitted monotonic sample computes elapsed whole ms plus prior sub-ms carry, rebases `last_tick_us_`, retains only `%1000` remainder, and discards excess whole-ms time. Equal samples add no credit; backward samples preserve baseline/remainder. State is limited to running/baseline/sub-ms remainder—no hidden debt or queue.
- `main/stroke_order/stroke_order_controller.cc:386-454` independently caps public `Tick()` at the same 150 ms, preventing alternate callers from bypassing the clock cap.

### Sequential visibility and edges

- `main/stroke_order/stroke_order_controller.cc:394-399` statically requires the cap below both 160 ms gap and 1000 ms reveal and limits processing to two phase visits.
- Starting in a reveal, at most 149 ms can remain for a newly entered gap; starting in a gap, at most 149 ms can remain for a newly entered reveal. Thus one call crosses at most one boundary and cannot traverse a gap and finish an unseen stroke.
- Exact reveal/gap equality transitions are correct; final completion has no synthetic gap. Zero, same-time, backward, and repeated huge inputs expose no whole-ms debt.
- At the cap, a stroke starting at zero shows 150/300/450/600/750/900 before 1000. A stroke entered with 1–149 ms after a gap still receives at least six positive partial presentations before completion.

## TDD integrity and retained results

`/tmp/stroke-speed-evidence/red-before.log` records the exact six-test interaction suite before the production calibration: both TSAN and UBSAN cases failed with return **-6**, while the other four tests passed. Seven rational-6Hz callbacks produced `33/66/99/132/165/198/231`, frame 7 remained `completed=0`, and both failures stopped at the exact assertion retained in `scripts/tests/stroke_order_ui_fence_harness.cc:156-159`: **“first stroke should complete by about 1.17s”**. Preserved prior source under `before/` has both clock/controller capped at 33 ms, consistent with this trace. Chronology is retained evidence, not live-observed by this reviewer.

The assertion remains verbatim and is not weakened. `final-tsan.log` and `final-ubsan.log` return 0 with `150/300/450/600/750/900/1000`; first completion is frame 7 at 1,166,667 us.

The cadence matrix at `stroke_order_ui_fence_harness.cc:126-169` asserts no earlier callback completion, exact final callback/time, and completion no earlier than 1000 ms. Retained results are:

- 33 ms → 1023 ms
- 50 ms → 1000 ms
- 100 ms → 1000 ms
- 150 ms → 1050 ms
- rational 6 Hz → 1166.667 ms
- 5 Hz → 1400 ms

These match `ceil(1000/min(interval,150)) × interval`.

`stroke_order_ui_fence_harness.cc:30-39,73-76,267-307` checks completed-count/phase adjacency for each admitted settlement. Its 一/人/口 matrix covers 33/50/100/150/166.667/200 ms and huge deltas, requires at least six earlier positive partial snapshots per automatic stroke, and records a gap snapshot before every non-final index advance. It also covers single/final strokes, exact edges, zero/near-reveal/near-gap stalls, same/backward timestamps, `UINT64_MAX`, repeated huge deltas, timer miss/retry, miss→Pause, Pause/Resume fractional carry, paused-time exclusion, Replay, Step/debounce, final completion during Pause settlement, fences, stale sessions, and action-first/fence-after ordering.

Mutation evidence is substantive: either layer left at 33 ms fails at frame-7 progress 231; removing the public cap yields 28 controller failures; removing Pause settlement, losing fractional carry, or retaining baseline/backlog also fails. Unmodified TSAN/UBSAN variants return 0.

## Controls, fences, and visual-only scope

- `stroke_order_ui_action.h:62-151` puts timers and controls inside exact nonzero generation/session/cancel-fence admission. Pause settlement and state change share that admission; failed try-lock/stale/fenced actions mutate neither timing nor controller state.
- Replay/non-Pause actions reset and rebase appropriately; Resume retains fractional carry while excluding paused wall time. Step remains a single 120 ms-debounced manual action with no queue.
- `stroke_round_coordinator.h:142-166` rejects contention, wrong generation, inactive phase, and cancel-fenced generations before mutation.
- `stroke_order_view.cc:1214-1359` keeps controls usable during animation and admits before rendering/disarm/abort effects.
- `stroke_order_view.cc:76-89` uses LVGL pressed background/border styles; registrations use `LV_EVENT_CLICKED`. No sound, audio event, feedback queue, or feedback timer exists.
- Timeout closes the retired overlay and restores the ordinary fresh SO entry instead of exposing generation-0 retry controls.

No control/fence/timeout/visual regression requiring rejection was found.

## Artifact, provenance, and retained build evidence

- `EVIDENCE-SHA256SUMS` and `freeze.log` identify `/tmp/stroke-speed-evidence/artifact.patch` as **`c2600b31798f817f364688575901657a4b30df9bd795b2e2b4343ba8de1be1aa`**. Its content is consistent with inspected shared source.
- The manifest has exactly 12 paths. `final-scope-audit.log` reports a clean-HEAD isolated checkout, the exact 12-path set, matching shared/isolated source hashes, successful reverse-apply/whitespace checks, 29 unrelated dirty files unchanged, and no logical diff in Application/audio/board/CMake/default-assets. Calibration itself changed eight existing artifact paths.
- Catalog SHA remains **`91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`**. `runtime.json` records 2000 characters/eight shards. NOTICE/source locks retain Hanzi Writer Data commit `68d10a4b21150cae5e1ebbd223eed289cf32d90c`, Arphic licensing, transcription provenance, and explicit prototype/non-release/non-commercially-reviewed status. This review is not commercial-release legal approval.
- Retained focused results: interaction **6/6**, UI **12/12**, TSAN/UBSAN successful.
- Retained full results: shared **133/133** (74.227 s) and isolated clean-HEAD+patch **133/133** (73.947 s), exit 0, `OK`, no skips.
- Retained clean build evidence: ESP-IDF **v6.0.2**, ESP32-S3/CoreS3, no initial build directory and fullclean between feature-on/off; seven stroke units on, zero off; only CoreS3 board factory/codec; 72 dependency manifests/10,629 files with zero mismatches.
- Feature-on app/assets: **2,918,096 / 7,568,207 bytes**. ZIP: **`d735c48e39159baf5c1af1f88edb0bcb7dfafe26dc0be36db0dde6d2bd598f74`**; merged: **`ac14ea56c9536988814515b94c1c6bbc0f5fb46de9810218c4bea4eb2b42ea07`**. Feature-off build/merge/ZIP also passed. Slice, ZIP payload, partition, and ESP image checksum/validation audits passed.

## Remaining hardware gates

No CoreS3 was flashed for this calibration. Approval covers source and retained evidence, not physical performance. Still required:

1. Measure actual active-reveal duration and confirm every stroke remains visibly sequential.
2. Distinguish the intentional faint full reference outline from completed contours and current accent/start marker in both themes.
3. Confirm LVGL invalidation/panel flush does not coalesce host-proven presentation opportunities.
4. Verify FT6336/control responsiveness and pressed contrast during animation, rapid controls, cancel/new-dialogue races, ordinary audio behavior, sustained FPS, and 100-session heap/timer/WDT stability.

A physical speed, panel-flush, or reference-outline perception failure needs follow-up, but no remaining source defect, early completion, double-boundary transition, backlog, control/fence regression, or scope/provenance mismatch was found.

- The final source separates the 33 ms LVGL scheduling request from a 150 ms animation-credit cap, and both the production clock and public Controller::Tick enforce the cap.
- The clock rebases admitted monotonic samples, retains only sub-millisecond remainder, and discards excess whole milliseconds; equal/backward samples cannot spend hidden debt.
- Because 150 ms is below both the 160 ms gap and 1000 ms reveal and Tick visits at most two phases, one settlement crosses at most one adjacent boundary and cannot finish an unseen next stroke.
- Retained sanitizer traces give first completion at 1023/1000/1000/1050/1166.667/1400 ms for 33/50/100/150/6Hz/5Hz, while the 一/人/口 matrix requires six prior positive partial snapshots per automatic stroke.
- The artifact remains exactly 12 paths with no protected logical diff; the 2000-character catalog hash remains 91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4.
- Retained evidence reports 133/133 host tests in shared and isolated trees and successful ESP-IDF v6.0.2 CoreS3 on/off builds; this read-only reviewer inspected but did not rerun them.

<!-- pi-squad:81c14659f8774df3e95f8cbf8980d61345ad88169a859bc17d7a8f4d1c0f0f05 -->
## 2026-09-13T11:20:31.797Z — stroke-adaptive-duration-review

# stroke-order-interaction-improvements：Option B 最终独立严格复审

## 结论

**APPROVED**。

- Blocker：0
- Major：0
- Minor：0

本批准仅确认冻结逻辑 artifact 中的**自适应时长策略与严格逐笔、不跳笔状态机在源码层面正确**，并确认保留证据与冻结源码相互一致；不等同于 CoreS3 真机 LCD/触摸验收，也不扩大 2000 字技术原型的数据发布或商用许可边界。

## 1. 时长策略与算术：通过

- `main/stroke_order/stroke_order_controller.h:62-87` 定义 `min=480 ms`、`max=600 ms`、整字预算 `5600 ms`、间隔 `160 ms`，并在任何减法、乘法、除法前拒绝 `count==0` 或 `count>StrokeOrderStore::kMaxStrokesPerCharacter`；后者在 `main/stroke_order/stroke_order_store.h:59` 固定为 48。
- 实现准确为 `gap_budget=uint64_t(160)*(N-1)`、饱和 `available`、向下取整 `raw=available/N`、再 clamp 到 `[480,600]`。因此精确表为 N=1–7→600、N=8→560、N=9–48→480。
- 边界正确：N=36 时 gap budget 恰为 5600，available=0；N=37 及以上继续安全饱和为 0，不发生无符号下溢；最大合法 N=48 的乘法远低于 `uint64_t` 上限。0、49、`UINT32_MAX`、`UINT64_MAX` 均在算术前返回 0。
- `main/stroke_order/stroke_order_controller.cc:780-781` 的 `StrokeDurationLocked` 是唯一播放/进度时长 seam：合法 index 从已验证且播放期间不改变的 `loaded_glyph_.size()` 推导同一时长，无效 index 返回 0。自动 Tick、Step、最终状态及 `current_progress_permille()` 均经该 seam；没有几何估算或按路径分支漂移。
- `main/stroke_order/stroke_order_controller.cc:533-549` 先以 `uint64_t` 做 `elapsed*1000`，再除以自适应 duration、clamp 后窄化，已消除旧的毫秒值与 permille 值直接类比及 32 位乘法风险。

## 2. 严格逐笔与 no-skip 保证：通过

- `main/stroke_order/stroke_order_controller.cc:380-444` 对每次公开 Tick 再次限制到最多 150 ms；`static_assert(150<160)` 与 `static_assert(3*150<480)` 位于 `:394-396`。
- 同一函数最多访问两个相邻 phase（`:397-443`）。从 reveal 开始时，单次最多完成该 reveal 并进入但不能退出 160 ms gap；从 gap 开始时，单次最多退出 gap 并给下一 reveal 少于 150 ms。因最短 reveal 为 480 ms，一次 settlement 不能再跨第二个边界、不能跳过 index，也不能完成一个此前不可见的新笔画。
- 每个自动笔画至少有三个更早的正进度 partial presentation snapshots；每个非末笔 gap 必有一次独立 snapshot。显式 Step 仍是用户要求的单笔手动动作，不被伪装成自动播放。
- `main/stroke_order/stroke_order_ui_action.h:36-47` 的生产时钟只结算一次 `min(real elapsed,150 ms)`，把 baseline 重置到已接纳采样并丢弃旧整毫秒 backlog，只保留小于 1 ms 的 fraction；相同/倒退时间不产生信用。`main/stroke_order/stroke_order_ui_action.h:62-74` 让 timer miss/stale/fenced admission 在结算前返回。
- `scripts/tests/stroke_order_ui_fence_harness.cc:248-286` 的独立 oracle 按 count 自行选择 600/560/480，精确断言 phase/index/completed/progress、三次 partial、gap-before-index 及同 timestamp 无债务；duration-aware adjacency 使用 `ceil(1000*150/duration)`，没有旧的 ms==permille 假设。
- `scripts/tests/stroke_order_ui_fence_harness.cc:299-330` 覆盖每个合法 count 1–48、非法 count，以及真实 一/人/口/顺和合成 N=1/2/3/5/7/8/9/10/15/24/36/48，在 33/50/100/150 ms、精确 6 Hz、5 Hz、重复 5 秒 stall 和极大 delta 下运行。源码和保留输出均显示没有 stroke/gap 抑制、重排或追赶式跳笔。

**真实“顺”处置：明确通过。** `scripts/tests/stroke_order_ui_fence_harness.cc:556-587` 读取 U+987A、断言 9 笔，在每次完成时要求至少 3 个先前 partial，并在每次非末笔 index 前要求 gap snapshot；frame 1–37 明确不得 Completed，只有 frame 38 在 6,333,334 us 完成。保留矩阵同时给出 33 ms 时 5.610 s、精确 6 Hz 时 6.333334 s、5 Hz 时 7.6 s。5 Hz 变慢是 no-debt/no-skip 策略的预期结果，不是假硬截止期。

简单字目标也成立：精确 6 Hz 下 一=0.666667 s、人=1.666667 s、口=2.500000 s。

## 3. TDD、真实 catalog 与回归灵敏度：通过

- `red-before.log` 保留了生产修改前同一真实顺速度断言在 TSAN 与 UBSAN 两条测试中均以 return -6 失败，均输出 `shun 6Hz frame38 completed=5 expected=9` 和 `shun should complete at about 6.33s`。这不是 fixed-only 合成字替代。
- 当前 runner 在 `scripts/tests/test_stroke_interaction_systems.py:30-49` 独立解析真实 `stroke_cat.bin`，确认 U+987A 映射到运行时名 `so06.bin`；检查 checked-in `so06.sob1` 的 size、CRC、catalog local index、实际 codepoint 和 9 笔记录，并分别以 thread/undefined sanitizer 编译生产 controller/clock/coordinator/session harness。
- `scripts/tests/stroke_order_ui_fence_harness.cc:554-595` 明确先运行真实 `so06.sob1` 回归；`frame<38` 的断言保留，随后 frame38 才允许 Completed。最终 TSAN/UBSAN 日志均记录 `completed=9 expected=9` 与 PASS。
- `scripts/tests/fixtures/stroke_order/prototype_2000/runtime.json` 记录 catalog SHA-256 `91779189…19e4`、2000 字/8 shard，并记录 so06 为 849,284 bytes、CRC `4f586b0f`、SHA-256 `900cb7ff…0778`；`stroke_order.cov.json` 将顺列为 official rank 1559、shard 6、9 笔。NOTICE/source lock 固定 Hanzi Writer Data commit `68d10a4b…90c`，并继续标记 prototype、非官方逐字认证、未完成商用复核。
- 九个临时 mutation probe 的保留脚本和日志均符合预期失败：fixed1000、fixed600、错误 8 笔时长、clock cap 33、public cap 33、取消 public cap、保留 backlog、漏掉 Pause settlement、丢失 paused fraction。未发现测试只能证明 happy path 的问题。

## 4. Pause/Resume、控件与 fence：通过

- `main/stroke_order/stroke_order_ui_action.h:90-168` 在同一 coordinator admission 内处理 Candidate、Pause/Continue、Step、Replay、Back、Exit、Retry；Pause 先结算至多一个 quantum，Resume 保留 fraction 但排除暂停墙钟时间，Replay/Step/Back/Exit 按语义 reset clock。
- `scripts/tests/stroke_order_ui_fence_harness.cc:397-550` 覆盖 timer miss→retry、timer miss→Pause、fraction carry、paused wall time、最终笔 Pause 完成、Replay reset、Step 120 ms debounce、相同/倒退/`UINT64_MAX` 时间、fence/session stale rejection。
- `main/stroke_order/stroke_round_coordinator.h:143-165` 的非阻塞 exact-generation admission 在 cancel fence、phase 和 generation 校验后才执行 controller/session mutation，成功动作与 coordinator phase 更新线性化；无动作队列。
- `main/stroke_order/stroke_order_view.cc:1214-1256,1313-1359,1439-1462` 显示动画期间按钮仍经同一 action/clock helper 可操作，candidate/Exit 只在成功 admission 后 disarm；timer 不直接调用未受 fence 保护的 controller Tick。
- `main/stroke_order/stroke_order_view.cc:73-90` 使用 LVGL `LV_STATE_PRESSED` 背景/边框视觉反馈；没有 stroke-specific 音效、音频事件或反馈队列。`main/stroke_order/stroke_order_view.cc:408-425` 的 timeout 关闭已退休 overlay 并提示从新的 SO 入口重试，不创建 generation-0 可操作页。

## 5. 冻结范围、构建和保留验证：通过

- 实际 `/tmp/stroke-adaptive-evidence/artifact.patch` 含恰好 12 个 diff path；`artifact-files.txt` 与 isolated changed set 相同。此次 adaptive 修复相对先前 artifact 仅变更 6 个既有路径。`final-scope-audit.log` 记录 Application、audio、boards、CMake、default-assets logical diff 为零，且 shared/isolated 12 个 source hash 均匹配 `SOURCE-SHA256SUMS`。
- 保留 checksum 记录中的 patch SHA-256 为用户期望值 `3f9a45e62be854859e429cea9eafea10aa5740aa7c273204745a9c5e70be72cd`；`SOURCE-SHA256SUMS` 自身记录为 `1a929fa5…b133`，catalog 为 `91779189…19e4`。
- 保留执行结果：focused interaction 7/7（含 TSAN/UBSAN）、UI 12/12；shared 与 clean isolated 全量各 134/134、无 skip；最终单独 TSAN/UBSAN 均 return 0。clang-format scoped check、`git diff --check`、patch reverse-apply 检查通过。
- clean artifact-only ESP-IDF v6.0.2 CoreS3 feature-on/off 构建均成功；on 编译 7 个 stroke translation units、off 为 0。on app/assets 为 2,918,176 / 7,568,207 bytes，均留有分区余量；72 manifests、10,629 dependency files 的保留校验为零 mismatch。
- feature-on ZIP 记录 SHA-256 `16f56b09d9c546054750bca0b1f167911cfe6a0fa470cf0fd397a7a5c2e462e9`，其中唯一 payload 与审计 merged image 一致；merged SHA-256 为 `40d5e0b050bdbfcf7f24bd85feb15fa1752a5ce8716580f7b92ba0d450d45c7a`。flash audit 对 bootloader、partition table、OTA data、assets、app 的 merged offsets 均为 MATCH，app/bootloader image 校验有效。
- on-build 的 `ota_data_initial.bin` 中间副本曾在 fullclean 前漏存；作者没有伪称原文件仍在，而是用同一 IDF 6.0.2 工具规范重建 8192-byte erased image，并与原 merged slice 及 off-build 原文件逐字节比对。该透明 provenance 缺口不影响自适应播放源码或 merged candidate 的一致性。

## 审阅执行边界与残余风险

本次 reviewer 严格只读：实际读取了 shared 与 `/tmp/stroke-adaptive-isolated` 源码、冻结 patch、作者完整报告、测试/构建/audit/checksum 文件；**没有修改、暂存、提交、推送，也没有自行运行 shell、测试、hash 或构建命令**。上述运行结果及哈希是对保留作者证据的审阅，不应表述为 reviewer 重跑；checksum 文件之间及源码内容相互一致，但 reviewer 未用独立命令重新计算摘要。

仍需真机门禁：

1. CoreS3 实际 LVGL callback cadence、LCD flush/FPS 可能合并 host presentation opportunities；必须确认顺及密集字的每一笔在物理屏上可见。
2. 在明暗主题下确认浅色完整参考轮廓不会被误认为已经播放完成，并验证 current accent/start marker 对比度。
3. 用 FT6336 真机验证动画中的 Pause/Continue/Step/Replay/Back/Exit、快速触摸、try-lock 重试及 cancel/new-dialogue 竞态。
4. 验证 100 次会话无持续 heap 下降、timer 泄漏、WDT、触摸失效或音频回归；最终设计没有按钮声音。
5. 精确 6 Hz 是确定性 host workload，不是设备实测 trace；5 Hz 或更差负载允许延长时长而绝不跳笔。
6. 2000 字数据继续只是技术原型；本批准不构成正式商用发布、官方笔顺认证或许可证法律意见。

- 自适应时长由单一 StrokeDurationLocked seam 按已加载 glyph 的总笔画数确定：1–7 笔 600 ms、8 笔 560 ms、9–48 笔 480 ms；0 或大于 48 在算术前返回 0。
- 生产 Tick 同时满足 150<160 和 3*150<480，并最多处理两个相邻 phase；因此一次自动 settlement 至多跨一个边界，每笔至少有三个更早的正进度 snapshot，非末笔 gap 不会被跳过。
- 真实 prototype_2000 catalog 将 U+987A 顺映射到 so06.bin；checked-in so06.sob1 的 catalog size/CRC/local index 可解析为 9 笔，来源记录固定 Hanzi Writer Data commit 68d10a4b21150cae5e1ebbd223eed289cf32d90c。
- 真实顺在确定性 6 Hz host workload 中仅于 frame38（6,333,334 us）完成，frame37 仍未完成；33 ms cadence 为 5.610 s，5 Hz 为 7.6 s，后者体现宁可变慢也不追赶跳笔。
- 保留证据记录 focused interaction 7/7、UI 12/12、shared/isolated full host 各 134/134 无 skip，最终 TSAN/UBSAN return 0；这些是作者保留执行，不是本次只读 reviewer 重跑。
- 冻结 artifact 为 12 个路径，adaptive repair 相对先前 artifact 为 6 个路径；保留 scope audit 显示 Application/audio/boards/CMake/default-assets logical diff 为零。
- feature-on CoreS3 candidate 的保留 SHA-256 为 ZIP 16f56b09d9c546054750bca0b1f167911cfe6a0fa470cf0fd397a7a5c2e462e9、merged 40d5e0b050bdbfcf7f24bd85feb15fa1752a5ce8716580f7b92ba0d450d45c7a；物理 LCD 可见性和触摸时序仍未由 host 证据建立。

<!-- pi-squad:da976fa97e3910eb6f2eee92f9a7e9f07b389b0a4145e01409c8a3d15fb94398 -->
## 2026-09-13T11:47:28.009Z — stroke-adaptive-fast-review

# `stroke-order-interaction-improvements` 最终独立严格复审

## 结论：APPROVED

**Blocker: 0 / Major: 0 / Minor: 0。没有需要列出的 severity/file:line 缺陷。**

批准对象严格限定为冻结补丁 `artifact.patch`（保留证据记录 SHA-256 `d959537e611e207ca772e40769bf0103148480dcfe91cdb5a67630b2bfb79741`）及其对应的 CoreS3 工程候选。真实 **顺** 的加速目标和逐笔不跳过保证均满足：exact 6 Hz 下 frame 29 仍未完成、frame 30 在 **5,000,000 us** 完成；每个自动笔画完成前至少有两个更早的正进度 partial snapshot，八个非末笔均有 gap snapshot，索引、阶段和完成数只按相邻顺序推进。

## 1. 冻结范围与源码一致性

冻结 artifact 恰为 12 路径：`.gitignore`、交互文档、controller `.h/.cc`、view `.h/.cc`、coordinator header、新 `stroke_order_ui_action.h`、controller harness、UI fence harness、Python interaction tests、`prototype_2000/stroke_cat.bin`。本轮 faster repair 只改变其中 6 个既有 artifact 文件；生产变化仅为 controller 的 301/500/4500 常量及相应静态安全断言，未再次改动时钟、控制、fence 或音频范围。

我直接核对了共享树与 `/tmp/stroke-fast-isolated` 中的关键生产源码；保留的 `SOURCE-SHA256SUMS`、`shared-source-hashes-final.log`、`isolated-source-hashes-final.log` 均将 12 路径逐项标为一致。`final-scope-audit.log` 记录 isolated tree 从 clean HEAD `7b455c74149d401e88fa729306391c10d9e73f0b` 应用补丁后恰为这 12 路径，reverse-apply 和 `git diff --check` 通过；`main/application.*`、`main/audio/**`、`main/boards/**`、`main/CMakeLists.txt`、默认 assets 构建脚本的逻辑 diff 为零。没有声音反馈或通用音频生命周期改造。

## 2. TDD 与真实 顺 数据链

- `red-before.log` 证明在生产常量仍为旧值 `480/600/5600` 时，更新后的 frame-30 回归先在 TSAN、UBSAN 两次失败，均为 subprocess return `-6`，输出 `shun 6Hz frame30 completed=7 expected=9`。`red-production-unchanged.log` 记录 red 后 controller `.h/.cc` 与 before snapshot 字节一致；before 源码也确实保留旧常量。
- 实际回归位于 `scripts/tests/stroke_order_ui_fence_harness.cc:569-610`：加载 U+987A，要求 9 笔；所有 frame `<30` 不得 Completed，显式保留 frame29 日志与 frame30 完成断言；`602-603` 再逐笔要求 `partial[index] >= 2`，并要求前八笔全部存在 gap。
- Python 回归 `scripts/tests/test_stroke_interaction_systems.py:30-43,80-87` 验证 catalog entry 指向 `so06.bin`，shard size/CRC 与真实 `so06.sob1` 相符，`entry.local_index` 反查仍为 U+987A，并实际解出 9 笔；它还锁定 frame29=`completed=8, finished=0`、frame30=`completed=9`、357 ms、exact 5,000,000 us、33 ms 下 4,521,000 us、5 Hz 下 6,000,000 us。
- 实际 catalog 为 24,352 bytes、SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`，8 shards/2000 字。U+987A 的 catalog rank 为 1559、shard=6、按生成器的 shard 内 codepoint 排序对应 local index 238；`so06.bin`/`so06.sob1` 为 250 字、849,284 bytes、CRC32 `0x4f586b0f`、SHA-256 `900cb7ff2c288f64b937614969f3f0cff5e6b6cd5a0e421222264e8ee3d20778`。coverage 记录 顺 的 source hash `441d86…fe68`、record 3,608 bytes、stroke_count=9。

## 3. 公式与算术审查

`main/stroke_order/stroke_order_controller.h:62-87` 正确实现：

`gap=uint64_t(160)*(N-1)`；`available=max(0,4500-gap)`；`raw=floor(available/N)`；`D=clamp(raw,301,500)`。

`N==0 || N>48` 在任何减法、乘法、除法前返回 0；48 的上界来自 `stroke_order_store.h:59`。有效范围内所有中间值为 `uint64_t`，无下溢/乘法溢出路径。结果精确为 N=1–7:500、N=8:422、N=9:357、N=10:306、N=11–48:301。`StrokeDurationLocked()` 在 `stroke_order_controller.cc:780-781` 是唯一生产 seam，并按当前加载 glyph 的固定总笔数给每一笔同一时长；progress 在 `:541-549` 先以 `uint64_t` 相乘再除。

## 4. 严格顺序与 no-skip 证明

`stroke_order_controller.cc:394-400` 同时强制 `150 < 160`、`2*150 < 301`，将公开 `Tick()` 的单次 credit 限为 150 ms，并最多访问两个相邻 phase。由此：

1. 从 reveal 开始，最多结束当前 reveal 并进入 gap；剩余量小于 150，不能再越过 160 ms gap。
2. 从 gap 开始，最多结束当前 gap 并进入下一 reveal；剩余量小于 150，不能完成至少 301 ms 的下一笔。
3. 从零进度开始，两次最大 credit 总和仅 300 ms，严格小于最短 reveal 301 ms，所以第三次 settlement 前必然已有两个正进度、非完成 snapshot。
4. gap 本身比 quantum 长，因此每个非末笔在索引推进前必有一次可呈现的 gap snapshot。

`stroke_order_ui_action.h:36-46` 用 `uint64_t` 计算真实 elapsed，只保留亚毫秒余数；成功 admission 时把 baseline 更新到当前样本，旧的整毫秒 backlog 被丢弃。相同/倒退时间不产生虚假 credit。controller 自身再次 cap，因此公开 oversized `Tick(UINT32_MAX)` 也不能绕过限制。

测试 oracle 在 `stroke_order_ui_fence_harness.cc:37-50,240-300` 使用 duration-normalized `ceil(1000*credit/D)` 约束，验证 phase/index/completed 单步相邻、稳定 duration、完整 gap budget、每笔至少两个更早 partial、gap-before-index、同时间重试无 debt。公式对 1–48 全覆盖，并检查 0/49/UINT32_MAX/UINT64_MAX；实际 一/人/口/顺 与合成 N=1/2/3/5/7/8/9/10/11/15/24/36/48 经过真实 parser/controller/clock，在 33/50/100/150 ms、exact 6 Hz、5 Hz、5 秒 stall 和超大 delta 下形成每个 sanitizer 136 runs/1552 自动完成。没有为 stall 伪造 deadline：顺为 4.521 s@33 ms、5.000 s@6 Hz、6.000 s@5 Hz，5 秒和超大 stall 仍只消费每回调 150 ms。

## 5. 控制、暂停与取消 fence

`stroke_order_ui_action.h:60-165` 和 `stroke_round_coordinator.h:136-166` 保持 exact nonzero generation、当前 session、active phase、cancel fence 和非阻塞 try-lock 的同一临界区 admission。Pause 在 admission 内先 settle 一次，再切 Paused；miss 不移动 baseline/remainder；Resume 排除暂停墙钟时间并保留 fraction。Replay 重置进度与时钟，Step 保留 120 ms debounce 且每次只显式前进一笔，无动作队列。final-stroke Pause completion、miss→retry、miss→Pause、fraction carry、Replay、Step、Back、Exit、stale session、replacement generation、timeout、action-vs-fence 线性化均由 harness 覆盖。

真实 LVGL 路径在 `stroke_order_view.cc:1211-1261,1320-1359,1449-1470` 复用上述 action/tick seam；fenced/missed 操作不会进入 render/abort，Candidate/Exit 只在成功 admission 后 disarm。动画中 Pause/Continue、Step、Replay、Back、Exit 仍可点击。`StyleControl` 在 `:73-89` 仅用 `LV_STATE_PRESSED` 做背景/边框视觉反馈；没有声音、音频队列或反馈 timer。

## 6. 保留验证证据

以下是作者保留日志，本次只读 reviewer **没有重新执行**：

- focused interaction：7/7；stroke UI：12/12。
- final TSAN、UBSAN harness 均 return 0；真实 顺 partial 计数为 `2,2,3,2,3,2,3,2,2`，八个 gap 均为 1。
- shared 与 clean-HEAD+patch isolated 全量 host suite 均 134/134、无 skip。
- 十个 mutation probe 全部按预期失败：missing Pause settlement、lost fraction、retained backlog、clock/public 33 ms cap、unbounded public Tick、fixed 1000、fixed 600、prior adaptive、wrong N8；未变异 TSAN/UBSAN controls 均通过。
- clang-format 19.1.7 对本轮 touched ranges 的 dry-run 通过。
- ESP-IDF v6.0.2、esp32s3、M5Stack CoreS3 clean feature-on/off 构建、merge 和 ZIP 成功；on 编译 7 个 stroke units，off 为 0，均仅包含 CoreS3 board factory/codec。on app/assets 为 2,918,176/7,568,207 bytes，分区余量 1,210,592/820,401 bytes。feature-on ZIP SHA-256 `7aa83484a42662d0abb036a7348ff28b50b5085dbb79c2c56b4665074e35e8ce`，merged SHA-256 `0300d70ea10f1de5d250c3bb8ff8e8be90e4506046ecd7224d8204a25d720573`；所有 flash slices 与 flasher args 对齐，app/bootloader image checksum 与 validation hash 有效。依赖审计为 72 manifests/10,629 files/0 mismatch。

## 7. 工具证据说明与剩余硬件门槛

本 reviewer 只有只读文件工具，未独立运行 shell、测试、hash 或构建；补丁 SHA 是从已检查的 `freeze.log`、`EVIDENCE-SHA256SUMS` 和报告交叉核对，而非本次重新计算。host sanitizer 使用 Apple clang 17，不能等价证明 ESP32/FreeRTOS 多核调度。

构建 driver 的异常已如实披露：on/off 构建、merge、ZIP 和各自 `complete` 输出之后，运行中修改 copy list 导致脚本 EOF，原始 final marker 未生成；随后审计实际产物、配置、compile units、ZIP 内容和 flash slices 后才恢复 marker。feature-on 原始 OTA initializer 未在 fullclean 前复制，后用 IDF 6.0.2 生成 canonical 8192-byte erased image，并验证它同时逐字节匹配 feature-on merged image 的 0xd000 slice 与 feature-off 原始 initializer。此瑕疵降低证据流程的整洁度，但没有造成固件/包不一致，故不构成代码或产物拒绝项。

仍未完成 CoreS3 真机烧录验证。**LCD flush 可见性仍是硬件 gate**：host 只能证明 redraw/presentation opportunity，LVGL 在负载下可能合并刷新。必须在真机确认 301 ms 最短笔画仍逐笔肉眼可见、浅色完整参照不会被误认为已播放、动画中所有按钮触摸延迟与 pressed 对比度、取消/新会话竞争、普通音频无回归，以及 100 次会话无持续 heap 下降、timer 泄漏、WDT、underrun 或触摸失效。另，2000 字数据仍明确标为 prototype、非官方认证且未经商业审核；本批准不等于商业发布许可或逐字笔顺认证。

- 冻结生产公式为 D(N)=clamp(floor(max(0,4500-160*(N-1))/N),301,500)，有效 N=1..48，结果依次为 1–7:500、8:422、9:357、10:306、11–48:301；无效 count 在算术前返回 0。
- 公开 Tick 和真实 animation clock 都把每次 advance 限为 150 ms；160 ms gap 与 301 ms 最短 reveal 保证一次 settlement 最多越过一个相邻边界，自动完成前至少存在两个更早正进度 partial snapshot。
- 真实 U+987A 顺 位于 shard 6 / so06.bin，catalog rank 1559、local index 238；so06 为 250 字、849284 bytes、CRC32 0x4f586b0f，顺记录为 9 笔。
- 真实 顺 回归在旧 480–600 ms 策略下先于生产修改于 frame30 失败（completed=7，return -6），新策略下 frame29 为 completed=8/unfinished，frame30 在 5,000,000 us 完成。
- 保留证据显示 focused 7、UI 12、shared/isolated full 134/134、TSAN/UBSAN、十个 mutation probes，以及 ESP-IDF 6.0.2 CoreS3 feature-on/off 构建均达到预期；这些不是本 reviewer 重新执行的结果。
- 最终 feature-on ZIP 的保留 SHA-256 为 7aa83484a42662d0abb036a7348ff28b50b5085dbb79c2c56b4665074e35e8ce，merged image 为 0300d70ea10f1de5d250c3bb8ff8e8be90e4506046ecd7224d8204a25d720573。

<!-- pi-squad:e6f0668ce8065d29886f35c893071436265c7a39af7b0846796b19c387835370 -->
## 2026-09-13T12:31:07.475Z — stroke-start-cue-snap-review

# `stroke-order-interaction-improvements` start-cue/snap 最终独立审查

## 结论：REJECTED

自动计时播放本身的 start-cue → full contour → gap → next cue 状态机满足要求，但最终 artifact 仍有一个可达的 **Blocker**：用户在笔间隔中点击“+1/逐笔”会在一次 UI 动作中越过 gap、进入下一笔并立刻完成该笔，导致该笔的起笔提示从未获得任何 redraw/presentation opportunity。任务明确要求任何可导致跳过 cue/stroke/gap 的源码路径均拒绝，因此不能批准当前冻结 patch 或烧录包。

### Blocker — StepForward 在 gap 中跳过下一笔起笔提示，并可跳过完整 gap 展示

- 位置：`main/stroke_order/stroke_order_controller.cc:301-338`，核心为 `:315-336`；调用入口为 `main/stroke_order/stroke_order_ui_action.h:132-133`。
- `StepForward()` 在 `in_gap_` 为 true 时先于 `:315-325` 清除 gap 并把 `current_stroke_` 加一；随后同一个调用继续执行 `:326`，把刚进入的下一笔直接设置为 150ms 完成；`:327-336` 又会将二笔字直接置为 `Completed`，或在多笔字中继续前进到再下一笔 cue0。
- 可达序列（以“人”两笔为例）：候选选择后 stroke0 cue 已显示；自动 `+160ms` 将 stroke0 完成并进入 gap；此时动画中允许操作“+1”；一次 Step 调用先进入 stroke1，再立刻把 stroke1 完成并把整字置为 Completed。stroke1 的 accent 起笔点从未被 `RedrawCanvas()` 绘制。
- 即使 Step 从普通 cue 调用，`:326-336` 也会在同一次动作里完成当前笔并直接进入下一笔 cue，完全绕过“完成轮廓单独显示的 gap”状态。因此手动路径也不保持要求的 cue/full/gap 相序。
- UI 没有防止此路径：`StrokeOrderApplyUiAction` 对 `Step` 直接调用 `controller.StepForward(now_ms)`，且产品要求动画中按钮可操作。
- 测试遗漏：`scripts/tests/stroke_order_ui_fence_harness.cc:448-453` 只测试从 cue 中 Step；`:630` 后的 contention 测试实际上在 `Tick(250)` 形成 gap 后执行 Step，却只断言点击成功，没有断言 stroke/index/progress 邻接或 cue 已呈现。自动播放 oracle 不调用 Step，故 135 项绿色结果不会发现此缺陷。

## 其余审查结果

### 自动 Tick/clock：通过源码审查

- `stroke_order_controller.h:60-70` 固定 cue 150ms、gap 160ms、最大 credit 160ms、LVGL timer 33ms。
- `stroke_order_controller.cc:380-431` 无循环、递归或 debt 队列；每次 Tick 只处理进入调用时的 phase。cue 边界和 gap 边界均立即 return，并丢弃 surplus；gap 完成只进入下一笔 `progress=0`；最后一笔直接 Completed，不产生末尾 gap。
- `stroke_order_ui_action.h:10-58` 的 clock 对 same/backward timestamp 不造时长，巨大 delta 截断至 160ms，成功 admission 才更新 baseline/remainder；whole-ms backlog 被丢弃，只保留亚毫秒余数。
- Pause 在同一 coordinator admission 内调用相同 `Settle()`，最多跨一个自动边界；暂停时间不计入，Resume 保留 fraction；miss/fence/session rejection 不改 clock/controller。Replay 重置到 stroke0 cue0。上述结论不覆盖前述 StepForward 的独立逻辑缺陷。

### 自动渲染 wiring：通过源码审查

- `stroke_order_view.cc:1073-1095` 只在首个 median 点画 accent 圆形 marker；`:1097-1143` 的 active incomplete pass 不再调用 median/path 渐进绘制。全字 faint reference 与 completed text-color contours 保持独立。
- 全仓 stroke-order C/C++ 搜索未见 `DrawMedianReveal` 残留；冻结前源码快照则明确包含其定义和调用。
- 候选进入动画页经 `RenderAnimationPage()` 创建 canvas 后调用 `RedrawCanvas()`；每个成功 timer settlement 在 `stroke_order_view.cc:1401-1406` 后同步 redraw。gap→next cue 的 Tick 已 return，因此后续 Tick 前存在 cue0 redraw opportunity。Pause/Replay 等已 admission 的控制路径也会 redraw。
- `StrokeOrderStore` 对每笔要求至少 4 个闭合 outline 点和至少 2 个 median 点（`stroke_order_store.h:65-66`，`stroke_order_store.cc:353-401`），坐标限制为 0..1024；空 median/outline 数据 fail closed。marker 经 200×200 canvas、12px inset、176px inner scale 后中心在 12..188，5px 半径仍位于 7..193，不越出 canvas。
- 这些仅证明状态和 canvas 更新机会，不能证明实际 LCD flush 已完成；该项仍是硬件 gate。

### Fence、session、timeout、按压视觉和无声音：未见新增缺陷

- coordinator try-lock 在 exact nonzero generation、active phase、无 cancel fence 时才在锁内执行 controller/session mutation；timer 与 UI action 共用该 admission。
- timeout 路径关闭 retired overlay、清 session/generation 并恢复 SO 入口，不创建 generation-0 可操作页面。
- candidate/control 使用 LVGL `LV_STATE_PRESSED` 背景混色和 3px 边框；事件为 `LV_EVENT_CLICKED`，没有声音、音频队列或新增 audio/Application/board 修改。

## TDD 与保留证据核查

以下均为**作者保留日志/脚本证据的只读核查，不是本 reviewer 重新执行**：

- `red-before.log`：focused 8 项中预期 3 项失败、5 项通过；TSAN 与 UBSAN 两个真实 `so06.sob1` U+987A/顺用例均为 return `-6`，首个 `+160ms` 显示 `stroke=0 progress=420 done=0`，失败在保留的 snap assertion；marker-only 源码回归因发现 `DrawMedianReveal` 失败。`before/` 快照确实包含旧 adaptive/phase-loop 实现和 `DrawMedianReveal`。
- 最终 regression 仍读取实际 `prototype_2000/so06.sob1`，catalog test 校验 U+987A 映射到 runtime `so06.bin`、size/CRC 和 9 strokes。第一/第二/第三个 `+160ms` 断言分别为 stroke0 full+gap、stroke1 cue0、stroke1 full；17 次更新覆盖顺的九笔交替。
- 保留绿色日志：focused systems 8/8、UI 12/12；shared 与 clean-HEAD+patch 各 135/135、无 skip。TSAN/UBSAN 最终 harness 均 return 0。1–48 synthetic counts、真实一/人/口/顺、九种 cadence、stall/huge/same/backward/no-debt、Pause/fraction/miss/fence 均有覆盖。
- 保留精确 timing：顺在 exact 6Hz 为 17 callbacks/2.833334s，5Hz 为 17/3.4s，33ms 为 85/2.805s。七个 mutation probes（progressive duration、cue overflow、gap overflow、missing pause settle、lost fraction、retained backlog、restored median）均被检测。
- catalog/runtime metadata 为 2000 字、8 shards×250；`runtime.json` 将其明确标成 prototype、not a release library、not commercially reviewed、not official certification。catalog retained SHA 为 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`。
- 冻结 patch 内容已读取；`EVIDENCE-SHA256SUMS` 与 `final-freeze.log` 记录 patch SHA `20911785950539351709fe9153cc1d5dfe25fa20f74df095e56dda170a9ca13b`。`final-scope-audit.log` 记录 shared/isolated source 与 source manifest 一致、恰好 12 paths、protected Application/audio/board/CMake/default-assets diff 为零、reverse apply/diff check 通过。
- clean ESP-IDF v6.0.2 CoreS3 retained builds：feature-on 编译 7 个 stroke units，app/assets 2,917,536/7,568,207 bytes；feature-off 0 个 stroke units，2,868,448/1,664,169 bytes，均通过链接和分区检查。on ZIP 记录为 `7e4bf9f1b3a3a82d52500b77d6fcd4157edbbe50e5d6f33f0e43ae9b12add5be`，merged 为 `42f99712f917ea89a82c61c0c9faac8a98e8b7688c5a45967e3aeda739b07293`；flash slice/image audit 与 72 manifests/10,629 dependency files audit 均报告通过。

## 证据限制与硬件残余

本 reviewer 受只读且无 shell/runner 工具限制，没有重新运行测试、构建、hash 或 `git diff`；上述执行结果和摘要值均明确归属于作者的冻结日志。我直接读取并比对了 shared 与 `/tmp/stroke-snap-isolated` 的关键生产/测试源码、完整 patch、before 快照和保留日志，但没有独立计算 SHA。

尚未烧录 CoreS3。即使修复 Blocker，仍须真机验证 panel flush/coalescing 下每个 150ms marker 与 full-contour snap 确实可见、两主题对比度、FT6336 延迟、动画中快速 Pause/Resume/Step/Replay/Back/Exit 与 cancel/new-dialogue race、以及 100-session heap/timer/WDT/touch 稳定性。原型字库的商业许可与逐字笔顺准确性仍未完成正式审核。

## Disposition

**自动 start-cue/snap 算法可接受；完整 artifact 不可接受。** 修复 Step 的 phase-local 行为并增加“在 gap 中按 Step 不得完成未呈现 cue 的下一笔、不得跨两个 phase”的生产回归前，不得使用当前 ZIP/merged image 作为最终候选。

- `StrokeOrderController::Tick` 的自动路径当前严格 phase-local：cue 与 gap 边界都 return 且丢弃 surplus，自动更新不会跨两个边界；最后一笔无 gap。
- `StrokeOrderController::StepForward` 在 gap 中会先进入下一笔，再在同一调用内把该笔完成；二笔字直接 Completed，多笔字继续到再下一笔 cue，因此下一笔 start cue 被完全跳过。
- 现有 Step 回归只验证从 cue 状态逐笔和 debounce；contention 测试虽在 gap 状态调用 Step，却没有验证 phase/index/progress 邻接，因而未捕获跳 cue。
- 实际 SOB1 parser 要求每笔至少 4 个闭合 outline 点、至少 2 个 median 点且坐标在 0..1024；合法 glyph 不会以空 median 进入 marker renderer。
- 保留证据记录 focused 8、UI 12、shared/isolated 135 项全部通过，但这些绿色结果不覆盖可达的 gap→Step 跳 cue 行为。
- 2000 字 runtime metadata 为 8 个各 250 字 shard，并明确标注 technical prototype、非发布字库、未商业审核、非官方认证。

<!-- pi-squad:8f36fd71049aa8745c1642958feaf9c6d4a2992d178dadc68f147c56453b747a -->
## 2026-09-13T13:02:25.955Z — stroke-step-phase-local-review

# Final independent strict review — `stroke-order-interaction-improvements`

## Verdict: APPROVED

**Findings:** Blocker 0 · Major 0 · Minor 0.

The phase-local Step blocker is **resolved**. I found no cue/gap skipping path, double-phase Step, debounce/fence/control regression, automatic timing/render regression, scope defect, or provenance defect in the reviewed artifact.

### Step blocker disposition

- `main/stroke_order/stroke_order_controller.cc:301-337` accepts Step only in Animating/Paused, preserves the 120 ms monotonic debounce, and rejects backwards timestamps. The invalid loaded-glyph/index/final-gap guard at `:310-314` runs before `has_last_step_ms_`/`last_step_ms_` are written at `:316-317`, so impossible states fail closed without consuming debounce.
- Cue-Step at `:327-336` completes only the current stroke. A non-final stroke remains at the same index with progress 1000, `in_gap=true`, gap elapsed zero, and Paused; the final cue alone enters Completed with no trailing gap.
- Gap-Step at `:319-326` clears only the current gap, increments the index exactly once, sets the new cue to elapsed/progress zero, pauses, and returns immediately. It cannot fall through and complete the newly entered cue.
- `main/stroke_order/stroke_order_ui_action.h:132-133,161-166` routes the real Step action through this method and resets/synchronizes the animation clock only after successful admission. `main/stroke_order/stroke_order_view.cc:1185-1199` then performs synchronous state presentation/canvas redraw. Thus every accepted Step has a post-admission redraw opportunity; rejected contention/fence/stale actions do not reset the clock or render.
- Exact repeated progression is therefore cue → same-stroke full/gap → next cue0 → same-stroke full/gap, with index/completed/phase deltas no greater than one. This is also asserted through all phases for real 人, real 顺, and synthetic 1–48-stroke glyphs in `scripts/tests/stroke_order_ui_fence_harness.cc:501-691`, from both Animating and Paused starts.

### Automatic start-cue/snap preservation

- Constants remain cue 150 ms, gap 160 ms, timer request 33 ms, and maximum per-update credit 160 ms in `main/stroke_order/stroke_order_controller.h:62-67`.
- `StrokeOrderController::Tick` at `main/stroke_order/stroke_order_controller.cc:379-426` caps credit, processes only the phase present on entry, discards overflow at either boundary, returns from gap completion at next-cue progress zero, and omits a final gap.
- `StrokeOrderAnimationClock::Settle` at `main/stroke_order/stroke_order_ui_action.h:36-49` rebases accepted samples, retains only sub-millisecond remainder, caps whole-millisecond credit, and does not create catch-up backlog. Equal/backwards samples do not invent elapsed time.
- Rendering at `main/stroke_order/stroke_order_view.cc:1097-1143` draws the light reference glyph, dark completed contours, and only the start marker for an active cue; progressive median rendering is absent. Timer updates use the fenced helper and redraw synchronously at `:1387-1410`.
- Pause/Resume/Replay/Back/Exit, timeout cleanup, click-only dispatch, pressed visual styling, and cancel-fence admission remain consistent with the visual-only requirement. No sound or audio lifecycle integration exists in the stroke-order source, and the retained exact-scope audit reports no Application/audio/board/CMake/default-assets logical diff.

### Retained test and mutation evidence reviewed

- Real pre-fix 人 regressions failed exactly as required: Cue-Step skipped to stroke 1 cue0, and Gap-Step completed stroke 1 (`red-exact.log`). The broader red run had 5 expected Step-related failures while render/source checks passed (`red-focused.log`).
- Final focused run: **22/22 PASS**, no skips (`green-focused.log`).
- Final shared suite: **137/137 PASS**, no skips (`shared-host-full-noskip.log`).
- Fresh clean-HEAD plus frozen-patch isolated suite: **137/137 PASS**, no skips (`isolated-host-full.log`). The asset-margin error text printed after `OK` is output from the suite’s intentional failure-path test, not a suite failure.
- Standalone exact production TSAN and UBSAN harnesses both returned 0 with Step cue/gap/matrix, start-cue/snap, sequential timing, pause clock, and fence tests passing (`final-tsan.log`, `final-ubsan.log`).
- Mutation evidence rejects the old combined Step in both entry modes, a gap fallthrough mutant, and a cue-skip-gap mutant; all four terminate on retained phase-adjacency assertions (`regression-sensitivity.log` and individual mutation logs).
- The real 顺 fixture is mapped through the checked catalog to `so06.sob1` and validated as nine strokes. Retained output shows all 17 alternating cue/full-gap callbacks at 160 ms, plus real 一/人/口/顺 and synthetic 1–48 matrices across multiple delivery intervals and large delays.

### Artifact, scope, and build evidence

- I inspected the actual shared and `/tmp/stroke-step-isolated` implementations and the complete frozen patch. The retained source audit records identical shared/isolated hashes for exactly 12 paths, zero protected logical diff, clean patch checks, and catalog SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`.
- Retained freeze/checksum evidence records frozen patch SHA-256 `07e267b0033890fa04027e358dede492f9ab1e96d211552542390e7b74bab48c`, matching the required value.
- Final CoreS3 feature-on/off builds used ESP-IDF v6.0.2, each followed `idf.py fullclean`, and each retained dependency lock SHA-256 `48195dfe1fc8909561f29d1aa50d2459a222567dc238f6bbb8336f91dd32b3f2`. Dependency audits report 72 manifests/10,629 files with zero mismatches. The earlier resolver-drift pair is isolated under `non-final-dependency-drift/` and explicitly excluded from delivery.
- Both final builds, merged images, partition checks, image validation, merged payload slices, and one-file ZIP equality passed. Feature-on merged SHA-256 is exactly `8997d0e6ec9d98e36c4862a8e4f4aabd6c8b307742b0fabac4a6a4cb1b9e48c9`; feature-off contains zero stroke source units.

### Reviewer execution and evidence caveat

This was a read-only review. I did not edit, stage, commit, push, flash, run tests/builds, or independently execute hashing commands because this assignment exposes file-reading/search tools only. Test/build/hash statements above are explicitly retained evidence that I inspected; source conclusions come from direct inspection of the shared tree, isolated tree, frozen patch, harnesses, and logs.

### Remaining hardware gates

Approval is for the logical/source artifact, not physical display acceptance. No CoreS3 was flashed in this review. Real hardware must still prove that LVGL/panel flushing does not coalesce the required cue, full-contour/gap, and next-cue frames; rapidly repeated Step presses visibly preserve those frames for 人 and 顺; pressed-state contrast works in both themes; touch latency, cancellation/new-dialogue races, Pause/Resume/Replay/Back/Exit, and 100-session heap/timer/WDT stability are acceptable. Normal audio behavior also remains untested on hardware, although this artifact adds no audio behavior. Prototype data licensing and stroke-accuracy limitations remain unchanged.

- `StrokeOrderController::StepForward` is now phase-local: cue completion keeps the same non-final stroke in Paused full/gap state, while gap completion advances once to a Paused next cue at zero progress.
- The Step invalid-state guards precede debounce timestamp writes, and coordinator contention, cancel-fence, stale-generation, or stale-session rejection occurs before controller/clock/view effects.
- Automatic playback uses a 150 ms cue, 160 ms gap, 160 ms credit cap, one starting phase per Tick, discarded boundary overflow, and marker-only active rendering.
- Retained final evidence reports focused 22/22, shared 137/137, isolated 137/137, standalone TSAN/UBSAN passes, and failure of all requested Step mutations.
- The pinned dependency lock hash is `48195dfe1fc8909561f29d1aa50d2459a222567dc238f6bbb8336f91dd32b3f2`; initial dependency-drift outputs are not delivery candidates.
- The frozen patch is recorded as `07e267b0033890fa04027e358dede492f9ab1e96d211552542390e7b74bab48c`, and the final feature-on merged CoreS3 image as `8997d0e6ec9d98e36c4862a8e4f4aabd6c8b307742b0fabac4a6a4cb1b9e48c9`.
- Host tests establish state transitions and synchronous redraw opportunities, not physical LCD flush visibility.

<!-- pi-squad:103dad6d44918f23b77ce1c4d90b3267579188b490d394a0098ddfdcf3f56140 -->
## 2026-09-13T14:12:18.758Z — stroke-first-cue-startup-review

# `stroke-order-interaction-improvements` first-cue startup gate — final independent strict review

## Verdict: **APPROVED**

- Blocker: **0**
- Major: **0**
- Minor: **0**
- Review mode: read-only; no edits, staging, commits, pushes, or publication.

I inspected the actual shared source, the isolated source under `/tmp/stroke-first-cue-isolated`, the complete author report, the frozen patch, the pre-fix snapshots/red harness, and retained sanitizer, mutation, full-suite, scope, dependency, build, image, and checksum logs. The reviewed source agrees at the inspected control points between shared and isolated trees. No first-timer advancement, token loss/rearm defect, skipped later cue, Step/control/fence regression, or scope/provenance mismatch was found.

## Approval basis

### 1. First cue is protected at the source/state and presentation-opportunity level

`main/stroke_order/stroke_order_ui_action.h:10-37` adds one display/LVGL-owned `first_frame_pending_` token. `BeginPlayback()` clears prior running/baseline/fraction state and sets exactly one pending bit. `OnTimer()` consumes that bit, sets `last_tick_us_` to the admitted callback timestamp, clears `remainder_us_`, and returns without calling `Controller::Tick`.

The automatic path in `main/stroke_order/stroke_order_ui_action.h:90-102` reaches `clock.OnTimer(...)` only inside `StrokeRoundCoordinator::TryUiAction`, after exact nonzero generation/current-phase/cancel-fence checks in `main/stroke_order/stroke_round_coordinator.h:139-166`, followed by presentation-session and `Animating` checks. Therefore lock misses, stale generations, stale sessions, fences, and invalid controller state do not consume the token or alter controller/clock state.

For a new animation page, `main/stroke_order/stroke_order_view.cc:813-879` preserves the action-owned token with `StopAnimTimer(false)`, builds the page, draws cue0 with `RedrawCanvas()`, and only then creates/resumes the timer through `SyncAnimTimer()`. The first admitted timer callback at `main/stroke_order/stroke_order_view.cc:1395-1420` performs the gate and redraws the same cue0 state. Thus a first callback delayed by 160 ms or 5 s cannot replace cue0 with the completed first stroke.

For Replay on an existing page, the action establishes a new token, ordinary Sync does not rearm or consume it, and the LVGL callback redraws cue0 before returning. Since click and timer callbacks execute serially on the LVGL task, the timer cannot interleave inside that control callback. This is a valid source-level presentation opportunity, not a claim that an LCD transfer completed.

### 2. Fresh-epoch token transitions are bounded and complete

`main/stroke_order/stroke_order_ui_action.h:124-197` creates the token only after a successful Candidate, Replay, or RetryLoad action. Replay is accepted from Animating, Paused, and Completed by `main/stroke_order/stroke_order_controller.cc:288-305`. Failed RetryLoad and RetryVoice/NoMatch do not arm it. Resume and ordinary Sync do not call `BeginPlayback`.

`main/stroke_order/stroke_order_view.cc:598-607` resets the clock by default on teardown, while only the animation-page rebuild opts out. Back, Exit, overlay deletion, entry deletion, candidate/error/status rendering, asset invalidation, and new-session presentation therefore clear an old token. Exact generation/session/fence admission and the current timer identity check prevent a retired timer turn from advancing or arming a replacement round.

### 3. Pause/Resume and explicit Step remain correct

`StrokeOrderAnimationClock::Settle` at `main/stroke_order/stroke_order_ui_action.h:54-72` returns immediately while pre-arm pending, so Pause credits zero and preserves the token. Syncing Paused suspends the baseline without consuming the token; Resume creates a fresh active baseline but no new token, so the first successful post-resume timer callback remains arm-only. After the token is consumed, the pre-existing fractional-millisecond Pause/Resume behavior remains: paused wall time is excluded, sub-millisecond active remainder is retained, and whole-millisecond stall debt is discarded.

An accepted Step is phase-local in `main/stroke_order/stroke_order_controller.cc:307-338`: cue→same stroke full/gap, gap→next cue at zero, and final cue→Completed. In `main/stroke_order/stroke_order_ui_action.h:185-196`, accepted Step clears the startup token/clock, while rejected Step does not. Its 120 ms debounce and no-queue behavior remain covered. Back and Exit similarly clear the clock.

### 4. Later automatic playback was not rearmed or skipped

`main/stroke_order/stroke_order_controller.cc:379-428` still caps each credit at 160 ms and settles only the phase present on entry: cue completion cannot consume the gap, and gap completion cannot consume the next cue. Cue duration remains 150 ms, gap 160 ms, timer request 33 ms. Equal/backwards samples credit nothing. Only a fresh playback adds one arm-only turn; later strokes are not rearmed.

The retained final TSAN and UBSAN harness logs both return 0 and cover real `一/人/口/顺` plus synthetic 1–48 stroke matrices. Real nine-stroke `顺` is recorded as 18 automatic callbacks at saturated 160 ms, exact 6 Hz, and 5 Hz, and 86 callbacks at 33 ms—exactly one more turn than the prior implementation. The one-stroke case likewise requires arm plus cue settlement.

### 5. TDD and mutation evidence is adequate

The retained pre-fix red evidence is specific and behaviorally meaningful:

- `red-exact.log` records all four required +160 ms/+5 s TSAN/UBSAN cases failing with return `-6` and `stroke=0 progress=1000 done=1 gap=1`.
- The retained assertion is the exact requirement: the first admitted callback must leave stroke0/progress0/done0/not-gap/not-finished.
- `red-harness.cc` models the then-current Candidate → page clock Reset/Sync → delayed first timer sequence; the saved `before/` production clock has no pending token and the saved view uses unconditional `StopAnimTimer()` before timer Sync.
- The final harness keeps the exact assertion and adds the mutable-canvas/delayed-copy model. It proves that the first timer redraw still contains cue0; it correctly does **not** claim a physical display flush.
- `regression-sensitivity.log` and the five individual logs show all required temporary mutants failing with `-6`: token disabled, token consumed before admission, pre-arm Pause clearing it, Resume rearming it, and Sync rearming it.

### 6. Retained regression/build evidence

These are retained author executions that I inspected; I did not rerun them:

- Focused interaction/TSAN/UBSAN/render suite: **12 passed**.
- Existing UI/controller suite: **12 passed**.
- Shared full host suite: **139 passed**, no unittest skips reported.
- Isolated clean-HEAD-plus-patch full host suite: **139 passed**, no unittest skips reported.
- Direct final TSAN and UBSAN production harnesses: return 0.
- Five mutation runs: all rejected by retained assertions.
- Scoped formatting and `git diff --check`: passed in retained logs.
- ESP-IDF version evidence: **v6.0.2**.
- Clean CoreS3 feature-on and feature-off builds, merge, and ZIP steps: passed; feature-on compiled 7 stroke units and feature-off 0.
- Feature-on app/assets sizes: **2,917,584 / 7,568,207 bytes**. Feature-off: **2,868,448 / 1,664,169 bytes**.
- Feature-on ZIP retained SHA-256: `375307e1e441ed0014b422487f0994e0900453ad04ffe8242a2f94145b1de0f8`.
- Feature-on merged image retained SHA-256: `504cbd759ee209e3e952d42e53af489dd73d115df660738c615f48fd565fcc3c`.
- Dependency audit: 72 manifests, 10,629 files, zero mismatches; on/off/isolated locks are recorded byte-identical to the pinned lock.

### 7. Scope and provenance

The frozen patch visibly contains exactly 12 `diff --git` entries, matching `artifact-files.txt`. Its retained freeze/checksum records report SHA-256 `467c07ee8bd1e8db73c44dd14944a6f25330717eaabdb823f90ac671f3f4be4c`, matching the requested artifact identity. `production-repair.diff` contains only the six stated repair files. `final-scope-verification.log` records shared/isolated source hashes matching `SOURCE-SHA256SUMS`, an exact 12-path isolated changed set, reverse-apply and diff checks passing, unrelated dirty files preserved, and no logical diff in Application/audio/boards/CMake/default-assets. The retained catalog digest is `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`.

The patch retains marker-only active rendering, faint reference contours and completed dark contours, LVGL pressed-state visual feedback, and no sound/audio path. Timeout closes the retired overlay and restores the fresh SO-entry retry route. No unbounded action/timer queue or synchronous `lv_refr_now` was introduced.

## Evidence caveat and physical residual risks

This reviewer had no shell/test execution tool and ran **no commands**; checksum, test, sanitizer, mutation, dependency, and firmware-build results above are explicitly retained author evidence, cross-checked by reading the corresponding files and logs rather than independently rerun or rehashed.

Approval is limited to the logical/source and host presentation-opportunity guarantee. The mutable-buffer model and double cue0 redraw do not prove that FT6336/LVGL/ESP LCD completed a physical panel flush before later updates. M5Stack CoreS3 hardware verification remains required for Candidate, Back→new selection, Replay from Animating/Paused/Completed, RetryLoad, and pre-arm Pause/Resume under load. Also still unverified on this candidate: marker/theme contrast, real touch/cancel races, sustained responsiveness/FPS, 100-session heap/timer stability, WDT/audio regressions, and physical package flashing. These are hardware acceptance gates, not grounds to reject the reviewed source gate.

- The reviewed first-cue gate is a single display-owned pending bit: fresh Candidate, Replay, and successful RetryLoad arm it only after coordinator admission; the first admitted timer consumes it without calling Controller::Tick.
- RenderAnimationPage preserves the fresh token across timer recreation, redraws cue0 before timer creation/resume, and default timer teardown still resets the clock on Back, Exit, deletion, status/candidate pages, invalidation, and replacement-session presentation.
- Coordinator generation/active-phase/cancel-fence checks and presentation-session/Animating checks all precede OnTimer, so rejected, stale, fenced, or contended callbacks cannot consume the pending token or mutate playback timing.
- Pre-arm Pause credits zero and preserves pending across Resume; post-arm Pause/Resume retains only sub-millisecond active fraction and excludes paused wall time; accepted phase-local Step supersedes and clears the token.
- Automatic playback remains marker-only with cue150/gap160/cap160 and no backlog; only fresh playback gains one arm-only callback, giving real 顺 18 saturated callbacks or 86 callbacks at 33 ms in retained sanitizer evidence.
- Retained evidence records the exact pre-fix +160 ms/+5 s TSAN/UBSAN failures, final TSAN/UBSAN passes, 12 focused and 12 UI passes, two 139-test no-skip full suites, and five mutation failures.
- The frozen patch has 12 diff entries and retained SHA-256 467c07ee8bd1e8db73c44dd14944a6f25330717eaabdb823f90ac671f3f4be4c; retained audits report no protected logical diff and the required catalog hash unchanged.
- Retained ESP-IDF v6.0.2 evidence records clean CoreS3 feature-on/off builds and packages; the feature-on ZIP and merged-image hashes are 375307e1e441ed0014b422487f0994e0900453ad04ffe8242a2f94145b1de0f8 and 504cbd759ee209e3e952d42e53af489dd73d115df660738c615f48fd565fcc3c.
- The host mutable-buffer regression proves a cue0 presentation opportunity but cannot certify an asynchronous LCD flush; physical CoreS3 visibility remains an explicit hardware gate.
