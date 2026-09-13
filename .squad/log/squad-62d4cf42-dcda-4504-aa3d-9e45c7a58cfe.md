

<!-- pi-squad:07d44744685606380a8c592c5afa251ef2695cf73097d3502071dcff145bb097 -->
# squad-62d4cf42-dcda-4504-aa3d-9e45c7a58cfe
completed; 1/1 successful jobs.
- stroke-adaptive-duration-review: # stroke-order-interaction-improvements：Option B 最终独立严格复审

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
