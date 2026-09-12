

<!-- pi-squad:ac01254997647b26c6b6d069346163576a3bd75f3216fae2b88914df7e7dae90 -->
# squad-b22e2157-dc96-41a4-aeeb-d3f604a45a3f
completed; 1/1 successful jobs.
- stroke-ui-cores3-review: # stroke-ui-cores3 独立审查

**Verdict: REJECTED**

当前实现已经具备可编译的 CoreS3 三字触摸演示骨架，专项/全量 host 测试和 IDF 6.0.2 构建证据也真实存在；但资产 mmap 生命周期和跨线程 generation 防护仍有可导致悬空访问或休眠后重建 UI 的 blocker。逐笔防抖、触摸坐标快照、暂停/完成态定时器、休眠入口恢复也有明确功能或回归问题。因此不能批准进入 STT 集成。

本审查只读取真实工作区、保留的作者 session/output 及 `/tmp/xiaozhi-stroke-ui-cores3-build` 产物；**未修改文件，也未自行运行 shell、测试、构建或真机操作**。

## Blocker

### B1. `StrokeOrderController` 可继续持有已被 OTA assets 更新解除映射的 `stroke_order.bin` 指针

**依据：**

- `main/stroke_order/stroke_order_view.cc:75-85` 仅在 `!controller_->is_ready()` 时调用 `GetAssetData("stroke_order.bin")` 和 `BindStore()`。
- `main/application.cc:69-70` 在启动 UI 后首次 Attach；`main/application.cc:418-421` 在 `Assets::Apply()` 后再次 Attach。
- `main/assets.cc:537-538` 在下载新 assets 前调用 `UnApplyPartition()`；`main/assets.cc:207-214` 由 `esp_partition_munmap()` 解除原 mmap 并清空索引。
- `StrokeOrderStore` 保存调用者拥有的 blob 地址；controller 的 `is_ready()` 不随 Assets mmap 失效自动变化。

如果启动时旧 assets 中已有合法 `stroke_order.bin`，第一次 Attach 会绑定其 mmap 地址。随后 assets 下载解除映射，但 controller 仍可能保持 ready；第二次 Attach 因 `is_ready()==true` 跳过重绑。后续 `Contains/LoadCharacter/GetStroke` 会解引用失效地址，而且即使新 assets 已删除笔顺文件，入口 gate 也可能错误地继续成立。下载与可点击入口还可能并发，使候选快照在解除映射期间读取旧地址。

**要求：** assets 解除映射前必须先使 Stroke UI 不可进入、停止 timer、清空所有 view 缓存并 `Unbind()`；新 assets 完成映射后必须无条件按新文件重新验证/绑定，不能以旧 `is_ready()` 代替资产世代检查。更稳妥的方案是显式 asset lease/generation 或 controller 拥有有界拷贝。必须增加“旧文件存在→OTA 更新/删除/损坏→迟到点击”的生产路径测试。

### B2. `generation_` 本身存在 C++ 数据竞争，且检查与 UI/Controller 变更不是同一临界区，不能可靠拦截迟到任务

**依据：**

- `main/stroke_order/stroke_order_view.h:105` 将 `generation_` 定义为普通 `uint32_t`。
- `main/stroke_order/stroke_order_view.cc:809-867` 在 LVGL callback 中读取它，在 `Application::Schedule` 的 main task lambda 中再次读取；该 lambda 的检查发生在取得 `DisplayLockGuard` 之前。
- `main/stroke_order/stroke_order_view.cc:155-160,267-285` 在 overlay 生命周期路径修改 generation；这些写与 main-task 裸读没有统一 C++ 同步。
- `main/stroke_order/stroke_order_view.cc:121-133` 的 power-save 路径先在 display lock 内 DestroyOverlay，释放锁后才 `controller_->Exit()`。
- `OverlayDeleted()`（`stroke_order_view.cc:872-887`）在外部删除路径没有 bump generation。

LVGL task、Application main task、ESP timer task是不同执行上下文。普通整数跨线程读写构成数据竞争。即使简单改成 atomic，现有流程仍有 TOCTOU：scheduled lambda 可先通过 generation 检查并修改 controller，随后阻塞等 display lock；power-save 删除 overlay 后释放锁，旧 lambda 再取得锁并按旧动作重建页面，最后 power-save 才把 controller 设为 Hidden，留下 view/controller 不一致状态。Shutdown 也有同类迟到任务风险。

**要求：** generation 检查、controller 状态变更和对应 view 呈现必须在一个明确串行化方案内完成；例如全部转到 Application main task，再在持有 display lock 后检查原子/受锁 generation，并以一致锁顺序完成 controller cancel 与 UI teardown。外部 `LV_EVENT_DELETE` 也必须使待执行任务失效。需要可控调度 harness 覆盖 click→power-save/shutdown/page rebuild 的各个交错点。

## Major

### M1. 真机逐笔前进的 120 ms 防抖时钟在 Paused 状态不会前进

- `main/stroke_order/stroke_order_controller.cc:123-132` 用 `now_ms_`/`last_step_ms_` 判定防抖。
- `Tick()` 仅是 `now_ms_` 的生产来源（`stroke_order_controller.cc:201-205`）。
- `AnimTimerCb()` 只在 Animating 调用 Tick；Paused 只重绘（`stroke_order_view.cc:929-947`）。

第一次 `StepForward()` 会把多笔字置为 Paused，此后 `now_ms_` 停止，后续逐笔点击会永久落在同一个防抖窗口而被拒绝。若第一次点击恰好发生在 `now_ms_==0`，又因 0 被当作“未设置”哨兵，反而可能绕过防抖。现有 harness 通过 `SetNowMsForTest()` 人工推进时间，掩盖了生产 UI 路径问题。应使用真实单调时钟，或让时间基准在 Paused 时继续推进，并增加“不注入测试时间、暂停后等待再逐笔”的测试。

### M2. 三个独立 atomic 不能提供一致的 `(pressed,x,y)` 快照

- `main/boards/m5stack/core-s3/m5stack_core_s3.cc:134-136` 分别声明 x、y、pressed atomic。
- poll 在 `:245-248` 依次 relaxed-store x/y，再 release-store pressed。
- LVGL read 在 `:285-288` acquire-load pressed 后分别 relaxed-load x/y。

这能避免普通数据竞争，也使某次 release 前的坐标可见，但手指持续按下时下一轮 poll 可能在两个 relaxed load 之间更新坐标，LVGL 因而读到新 x + 旧 y 的撕裂组合，可能误命中相邻候选或控制。应把 x/y（最好连同 pressed/sequence）打包为单个 atomic snapshot，或用 sequence-lock/临界区保证一致快照。

### M3. Paused 和 Completed 页面仍每 33 ms 全量重绘 200×200 canvas

`main/stroke_order/stroke_order_view.cc:929-947` 在 Paused/Completed 每次 timer tick 都调用 `RedrawCanvas()` 和 `UpdateControlLabels()`，但状态没有变化；timer 也未暂停/删除。虽然未发现每帧 vector 大分配，但会持续清屏并重画田字格和全部笔画，造成不必要的 CPU、总线和功耗负担，可能影响音频任务。静态状态应只在状态/主题变化时重画并暂停 timer；Resume/Replay 时再恢复。

### M4. 从 overlay 进入休眠后，入口会永久隐藏且保留不可见的触摸消费区

- `EnsureOverlay()` 只给 entry 加 `HIDDEN`（`stroke_order_view.cc:228-263`），没有清 entry hit rect。
- `OnPowerSave(true)` 只 DestroyOverlay + controller Exit，`on=false` 直接返回（`:121-133`）。
- `DestroyOverlay()`（`:267-285`）既不 ShowEntry，也不 ClearEntryHitRect。
- controller Hidden 时 `ShouldConsumePointer()` 仍回退到 entry rect（`stroke_order_controller.cc:261-266,487-491`）。

因此若睡眠发生在 overlay 打开时，唤醒后 entry 仍隐藏；其旧坐标区域却继续吞掉普通短触，形成聊天触摸死区。若睡眠发生在 overlay 未打开时，entry 又会在睡眠界面继续可点，行为不一致。应在睡眠时明确隐藏且清 gate，在唤醒后按资产/indev/设备状态重新展示；不得在低功耗状态启动 overlay。

### M5. Overlay 没有与普通会话/启动状态建立中断边界

`EntryClicked()`（`stroke_order_view.cc:809-823`）不检查 `Application::GetDeviceState()`；当前 Application 对 StrokeOrder 只有两处 Attach（`main/application.cc:69-70,420-421`），没有在普通新会话、wake word、连接/监听/说话、高优先级系统事件中取消 overlay。入口甚至在 `kDeviceStateStarting` 阶段即可显示。结果可能是在启动/配网或普通语音会话背后继续显示并独占触摸，与“普通新对话/高优先级事件中断笔划、不影响普通聊天”的既定边界不符。进入 STT 集成前必须先定义并实现 Idle/Starting/Listening/Speaking/升级/休眠的入口与取消规则。

### M6. 93 项测试没有覆盖本次最危险的 UI/触摸并发路径

`scripts/tests/test_stroke_order_ui.py` 只有两个测试；生产 C++ harness 确实编译并运行 controller/store，但不编译或模拟 `stroke_order_view.cc`、CoreS3 pointer indev、Application::Schedule、asset munmap、LVGL timer/delete callback。当前测试通过不能发现 B1、B2、M1-M5。需要最少加入 controller 生产时钟测试、触摸 snapshot/sequence harness，以及可替换 LVGL/Assets 调度层的生命周期测试。

## Minor

### m1. APL/NOTICE 是未声明的 custom-command 副产物

- `scripts/package_stroke_order_smoke.py:120-135` 写出 bin、manifest、`ARPHICPL.TXT`、`NOTICE.md`。
- `main/CMakeLists.txt:1050-1070` 只把 `stroke_order.bin` 声明为 OUTPUT/依赖；其余三个文件既不是 OUTPUT 也不是 BYPRODUCTS。

干净构建确实打包了全部文件，但增量构建中若 bin 已存在而 NOTICE/APL 被删除，或脚本在写完 bin 后复制许可证失败，下一次 Ninja 可能不重跑打包命令，随后生成缺少许可文件的 assets。应声明全部输出/BYPRODUCTS，并采用临时目录后原子发布或失败清理。

### m2. `DisplayLockGuard` 的失败仍会 Unlock

本次静态检查未发现 StrokeOrderView 自身的嵌套 guard：Attach/Shutdown/OnPowerSave/HandleControl/HandleStatePresentation 各取一次，私有 helper 不再重复取锁，这是改进。但 `main/display/display.h` 的 guard 在 `Lock()` 失败后仍继续执行并在析构时 Unlock；作者也已承认这是既有 core 风险。当前 30 秒超时通常不会发生，但后续应修复 guard，让失败可检测且绝不解锁未持有的锁。

## 已验证为正确/有实证的部分

1. **20 ms I2C 路径与触摸序列基本结构：** FT6336 仅在 `PollTouchpad()` 读取，timer 周期为 20 ms（CoreS3 `:234-248,291-308`）；LVGL callback 不做 I2C。press edge 固定 `sequence_consumed_`，release 同时检查 consumed、exclusive、acted 和 `<500 ms`，所以代码意图上 overlay/entry 消费整次序列、普通短触只 Toggle 一次、长按不触发短触（`:251-275`）。上述坐标一致性与真机抖动仍未通过。
2. **renderer 借用边界：** 当前动画页在 `stroke_order_view.cc:322-346` 使用 `CopyLoadedGlyph()` 生成 owned cache；逐帧绘制不保留 controller mutex 外的 `StrokeView`。候选页只在 stack-local snapshot store 生命周期内立即解码（`:288-319,371-396`）。
3. **user_data/跳过候选：** candidate/control user_data 指向固定成员数组，不指向 vector 元素；候选先 reserve/fill，再建按钮；display slot 和 controller index 分离（`:384-413,825-844`），跳过坏 glyph 后不会错选。control ids 同样稳定（`:455-475`）。
4. **布局与分配：** `stroke_order_layout.h:9-27,36-91` 固定 320×240；候选格约 96×96，控制为 88×44，entry 52×44；田字格位于 `(8,20)`、200×200，控制区位于 x=224..311、最后按钮 y=192..235，无明显越界。canvas RGB565 约 80 KiB，仅页面建立时创建并在 overlay teardown 释放（view `:418-482,267-285`）；vector 解码分配发生在页面切换，不在逐帧路径。
5. **资产 gate/名称与三字范围：** Kconfig 默认关闭且仅 CoreS3、非 Emote；运行时名称和 CMake 名称均为 `stroke_order.bin`。打包器固定并校验 一/人/口、commit、文件 hash、APL hash，明确 `not_a_release_library`。保留构建目录中存在 2668-byte bin、6900-byte APL、2377-byte NOTICE 和 manifest。
6. **timer/canvas 正常主动销毁顺序：** `DestroyOverlay()` 先 bump generation、停止 timer、清 widget 指针、删除 overlay，最后销毁 canvas buffer；delete callback 不释放 buffer，主动路径未见明显 double-free。
7. **构建身份：** `main/boards/m5stack/core-s3/config.json` 仍为 type/name `m5stack-core-s3`；没有新增板卡 identity 或改变 pin 配置。

## 作者验证证据核验

保留日志证明作者实际执行过：

- clang-format **19.1.7**；对列出的本阶段 C++ 执行 `-i`，核心 Stroke/UI/board/harness 的 `--dry-run -Werror` 返回 0。
- 专项 `python3 -m unittest scripts.tests.test_stroke_order_ui scripts.tests.test_stroke_order -v`：**26 tests, OK, 4.923s**。
- `python3 -m unittest discover -s scripts/tests -v`：**93 tests, OK, 7.438s**。
- `test_stroke_order_ui.py` 用 C++17、`-Wall -Wextra -Werror`、UBSan 编译真实 `stroke_order_controller.cc` + `stroke_order_store.cc` 并执行 harness。
- ESP-IDF **v6.0.2**，隔离目录 `/tmp/xiaozhi-stroke-ui-cores3-build`，配置头实际含 `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=1`、`CONFIG_STROKE_ORDER_LOCAL=1`；构建日志显示 `Project build complete`。
- `/tmp/xiaozhi-stroke-ui-cores3-build/xiaozhi.bin`：2,879,248 bytes (`0x2bef10`)；app partition `0x3f0000`，余 `0x1310f0`（30%）。ELF 39,486,896 bytes，map 24,403,675 bytes，`generated_assets.bin` 1,678,548 bytes；flash_args 同时列出 app 与 assets。
- 并未烧录或启动真机。作者构建后的复合“收集信息”命令曾因 grep 返回 code 1，但这发生在已成功固件构建之后，不推翻上述 build success。

## 工作区保护核验

作者起止 `git status --short --branch` 证据都保留了原有 Stick-S3 `config.json`/`m5stack_stick_s3.cc`、dictation/tool、emoji、`partitions/v2/8m_single_app.csv`、`.squad`、`CONTEXT.md` 等未提交项；没有 commit/stage/reset/clean 证据。本审查也未修改任何文件。但多项功能文件仍是 untracked，后续提交前必须精确选择路径，禁止 `git add -A`，以免混入用户工作。

## 重审前必须完成

1. 修复 asset mmap/rebind 生命周期和 generation/调度串行化两个 blocker。
2. 修复生产逐笔时钟、原子触摸快照、静态状态 timer、sleep/wake entry gate，并实现普通会话/系统事件取消规则。
3. 为上述交错场景增加生产 C++/UI harness；重跑 clang-format 19、专项 26、完整 suite、IDF 6.0.2 CoreS3 隔离构建。
4. **在这些修复获独立复审前，不应开始 STT 集成。**

## 真机仍必须验证

- CoreS3 实际 FT6336 坐标方向、边缘/间隙/拖动/长按/触点抖动，以及每序列只执行一次动作。
- entry/overlay 与普通短触 Toggle、启动配网、wake word、新会话、来电/系统事件、sleep/wake 的互斥和取消。
- 一/人/口候选与完整笔顺，暂停/继续/逐笔/重播/返回/退出；逐笔等待超过 120 ms 后可连续工作。
- 动画目标约 30 FPS、持续下限约 20 FPS；Paused/Completed CPU 应回落。
- 缺失/损坏 asset、assets OTA 替换/删除以及迟到点击不得崩溃或错误保留入口。
- 100 次会话的 free heap/largest block、timer 数、WDT、音频 underrun、触摸失效；确认 80 KiB canvas 与 glyph cache 的真实 PSRAM/heap 行为。
