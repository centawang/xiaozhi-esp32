

<!-- pi-squad:03fcb2a3885a7492be2150329c6110dd7dfead49b81acf2bbcbaef571a18261f -->
# squad-eaeb61cb-7567-4406-b8ac-bef62f75481d
completed; 1/1 successful jobs.
- stroke-ui-cores3-rereview: # stroke-ui-cores3 修订复审

## Verdict: APPROVED

本次对真实工作区源码和保留的作者验证日志进行了只读复审。原审查中的 **B1、B2、M1–M6、m1、m2 均已达到本阶段放行条件**。没有遗留 blocker 或 major。批准范围仅是 CoreS3 本地三字 smoke UI 基础及其可烧录构建，不代表远端 STT、`type=stroke`、约 500 字字库或真机体验已经验收。

本复审未修改文件，也未自行运行 shell、测试或构建；测试结论来自逐项核验后的保留日志和构建目录，不冒充本次重新执行。

## Blocker

无。

## Major

无。

## Minor / 后续注意项

1. **外部 LVGL delete 后的资源回收可更彻底。** `StrokeOrderView::OverlayDeleted()` 会停 timer、清 widget 指针和 glyph vectors、退出 controller 并使 lifecycle surface 失效，但不会当场销毁独立的 `canvas_buf_`；`EntryDeleted()` 则停止 timer并隐藏 overlay。该 buffer 会在后续 `RebindAssets()`、`SuspendAssets()`、正常 `DestroyOverlay()` 或 `Shutdown()` 中释放，因此当前没有继续绘制或 UAF 路径，但一次孤立的外部 delete 可能暂时保留约 80 KiB。建议后续增加安全的 deferred cleanup 或真实 screen teardown 测试。
2. **host harness 是生产核心逻辑覆盖，不是完整 LVGL/Assets 集成执行。** controller harness 编译运行真实 `stroke_order_controller.cc`/`stroke_order_store.cc`；lifecycle harness 直接使用生产 `stroke_order_lifecycle.h` 和 `stroke_order_touch_input.h`。但它没有在 host 上执行 `stroke_order_view.cc`、`Assets::Download/UnApplyPartition/Apply`、真实 `esp_partition_munmap`、LVGL delete/timer callback 或 FT6336 I2C。相关 glue 已由源码检查和 ESP-IDF 编译/链接覆盖，运行时仍属于真机边界。
3. **`StrokeOrderController::GetStroke()` 仍返回借用 controller-owned blob 的 `StrokeView`。** 头文件已明确标为 host-test helper，生产 renderer 使用 `CopyLoadedGlyph()`/`CopyCandidateGlyph()`，所以当前路径安全；下一阶段不得把 `GetStroke()` 当作跨锁、跨任务或跨 Unbind 的稳定接口，最好以后用测试宏隐藏。
4. **DisplayLockGuard 采用全局 fail-fast 策略。** `main/display/display.h` 现在记录 `acquired_`、仅在持锁时 `Unlock()`，锁失败直接 `std::abort()`，因此不会再发生未持锁 UI 访问或错误 unlock；代价是 30 秒锁超时会重启/终止，而不是返回错误。该选择满足本轮安全要求，但尚无锁超时注入和真机长稳证据。

## 原问题逐项核对

### B1 — controller-owned blob、上限、失败状态与 Assets 替换链：通过

- `StrokeOrderStore::kMaxFileBytes` 为 1,048,576；`StrokeOrderController::BindStore()` 在分配前拒绝 null、零长度和超限输入。
- `BindStore()` 使用 `new (std::nothrow)` 创建私有副本，在副本上重新 `Bind()`，并在 `RefreshCandidatesLocked()` 中逐候选执行 record CRC/body/load 校验。
- 每次 bind 尝试开始即清除旧 playback、store、owned blob、候选、selection 和 ready 状态；分配、格式或候选验证失败不会保留旧可用状态。
- 候选和演示渲染分别走 `CopyCandidateGlyph()` 与 `CopyLoadedGlyph()` 的 owned decoded vectors；生产 view 不再保存 Assets mmap 上的点视图。
- `Assets::UnApplyPartition()` 在 strategy `UnApplyPartition()`/`esp_partition_munmap()` 前同步调用 `StrokeOrderView::SuspendAssets()`；该调用隐藏 entry、清 hit rect、删除 overlay/timer/cache，并 `controller_->Unbind()`。锁失败时 `DisplayLockGuard` fail-fast，因而 unmap 不会在未成功串行化的情况下继续。
- `Assets::Apply()` 不论 strategy apply 成败都会调用 `StrokeOrderView::RebindAssets()`。`RebindAssetsLocked()` 先 suspend/cancel/unbind，再重新 `GetAssetData("stroke_order.bin")`、复制、验证和 bind；删除、损坏或超限 replacement 保持 entry 隐藏。
- controller harness 覆盖源 blob 清零/释放后继续读取、suspend/unbind 后拒绝迟到 open、损坏替换不保留旧 ready、超限拒绝和合法替换恢复。实际 Assets mmap/remap 未在 host 执行，见 Minor 2。

### B2 — UI 串行、锁序、delete/interrupt/shutdown：通过

- `stroke_order_view.cc` 已无 `Application::Schedule()` 点击跳转和普通 `generation_`。Entry、candidate、control、timer 和 delete callback 在 LVGL task 中直接串行处理 controller 与 view。
- 外部 device-state、power-save、asset、alert 和 shutdown 入口均先取得 `DisplayLockGuard`；共同锁序为 **display/LVGL lock → controller mutex**。
- CoreS3 的 20 ms poll 只调用 controller 查询并访问自己的 touch sequence，不在持 controller mutex 时获取 display lock，未发现反向锁序。
- `StrokeOrderLifecycle` 是生产 view 实际使用的 gate。外部 overlay/entry delete 会令 surface invalid、停止 timer、取消 controller、清 hit rect；迟到点击不能重新打开，只有显式 Attach/Rebind 才能 rebuild。
- `OnInterruption()` 与 `Shutdown()` 在同一 display 临界区取消 session；shutdown 还清 controller/store/blob。正常主动销毁路径先停 timer，再删 overlay，最后释放 canvas buffer，重复调用可容忍。

### M1 — StepForward 单调时间：通过

- 接口已变为 `StepForward(uint64_t monotonic_ms)`；生产 control callback传入 `esp_timer_get_time()/1000`。
- `has_last_step_ms_` 与 `last_step_ms_` 分离，时间 0 是合法首样本；倒退或小于 120 ms 的样本被拒绝。
- harness 覆盖 0 ms 首次操作、0/119 ms 拒绝以及 Paused 后超过阈值继续逐笔。

### M2 — packed atomic touch snapshot 与 sequence tracker：通过

- CoreS3 只用一个 `std::atomic<uint32_t>` 发布 pressed/x/y；poll 端 release-store，LVGL indev 端 acquire-load，持续移动不会混读两个时刻的 x/y。
- release 保留最后坐标，x 有 15-bit 上限；真实 `PollTouchpad()` 使用生产 `StrokeOrderTouchSequence` 管理 press edge、消费状态、长按阈值、release overlay gate 和每序列最多一个 legacy toggle。
- harness 覆盖 encode/decode、移动、release、边界、普通短触、已消费触摸、长按、overlay-on-release 和重复 release。
- 作者记录 Xtensa 上 `is_always_lock_free` 为 false，已删除不成立的静态断言；ESP timer callback 是任务上下文，最终 IDF 构建已成功链接该 atomic 实现。实时开销仍需真机量测。

### M3 — Paused/Completed/Error timer：通过

- `SyncAnimTimer()` 仅在 `Animating` 创建或恢复 33 ms timer；Paused/Completed pause。
- Candidates、Error、exit、interrupt、power-save、asset suspend、delete 和 shutdown 经 `StopAnimTimer()`/`DestroyOverlay()` 删除 timer。
- Pause/Step 只立即重绘一次；Resume/Replay 恢复或重建 timer；自然完成帧绘制后暂停。

### M4 — sleep/wake gate：通过

- `LvglDisplay::SetPowerSaveMode()` 在 `CONFIG_STROKE_ORDER_LOCAL` 下调用 `StrokeOrderView::OnPowerSave()`；CoreS3 的 PowerSaveTimer 生产 callback 已走该 virtual 方法。
- sleep 设置 lifecycle sleeping，退出 controller session，删除 overlay/timer/cache，隐藏 entry 并清 hit rect。
- wake 重新读取当前 DeviceState 和 pointer indev，并与 assets/surface gate 一起决定是否恢复 entry；睡眠期间 `TryOpenOverlay()` 失败。

### M5 — DeviceState / alert gate：通过

- `Application::SetDeviceState()` 在成功状态设置后同步通知 StrokeOrder view；非 Idle 统一 cancel/hide，点击入口和控件时还再次读取原子 DeviceState。
- Starting、WifiConfiguring、Connecting、Listening、Speaking、Upgrading、Activating、AudioTesting 等生产转换均不能保留 overlay；普通按键和 wake word 通过既有状态转换中断。
- `Application::Alert()` 显式调用 `OnInterruption()`；`DismissAlert()` 在仍为 Idle 时重新评估；`Reboot()` 调用 `Shutdown()`。
- 没有扩展 DeviceStateMachine，也没有提前开放语音笔划状态。

### M6 — 生产 harness：通过，但覆盖边界必须如实表述

- `scripts/tests/stroke_order_controller_harness.cc` 编译真实 controller/store，覆盖 blob ownership、上限、坏/好替换、候选复制、迟到动作与注入单调时钟。
- `scripts/tests/stroke_order_lifecycle_harness.cc` 直接编译生产 lifecycle/touch helper，覆盖 click→sleep、click→non-Idle、interrupt、suspend/corrupt/valid rebind、external delete、shutdown/reinitialize，以及 packed snapshot/sequence。
- 这满足“抽取最小生产逻辑且生产代码实际复用”的要求，但不是并发调度器、LVGL simulator 或真实 Assets/硬件测试；不能据此宣称真机交错和 mmap 已动态验证。

### m1 — 四个 CMake 输出与 staging：通过

- `main/CMakeLists.txt` 将 bin、manifest、APL、NOTICE 全部列入 `STROKE_ORDER_ASSET_OUTPUTS`，作为 custom-command `OUTPUT` 和 default-assets dependencies。
- `package_stroke_order_smoke.py` 在同一父目录创建 staging，验证四文件完整后以 rename 发布；异常会清 staging，并在旧目录已移走时恢复 backup。
- 保留日志证明作者删除隔离 build 的 NOTICE 后再次构建，Ninja 输出 `Packaging 一/人/口...`，NOTICE 恢复为 2,377 bytes，构建完成。

### m2 — DisplayLockGuard：通过

- guard 保存 acquisition 状态、禁止复制，只在 `acquired_` 时 Unlock；不再错误释放未持有锁。
- 锁失败日志后 fail-fast，不继续执行未加锁 UI。CoreS3 全量构建包含该头文件变更。

## 验证证据核验

- `/tmp/xiaozhi-stroke-ui-cores3-host-tests-final.log`：`Ran 96 tests in 11.595s`，最终 `OK`。专项 29 项的最终相关实现也包含在这次全量 96 项中。
- 作者 run 日志记录 clang-format **19.1.7**、最终 `git diff --check`/clang-format dry-run 为 `PASS`，以及 Python `py_compile` 通过。
- `/tmp/xiaozhi-stroke-ui-cores3-final-clean-build.log`：从 CMake 配置开始的 ESP-IDF 构建，目标 `esp32s3`；末尾生成 image、执行 size check并显示 `Project build complete`。
- 构建目录 `sdkconfig` 实际含 `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y` 与 `CONFIG_STROKE_ORDER_LOCAL=y`；`compile_commands.json` 含 CoreS3 board、store、controller、view 四条真实 Xtensa 编译项。
- 最终 app 为 `0x2bf3e0` / 2,880,480 bytes；最小 app 分区 `0x3f0000`，剩余 `0x130c20` / 1,248,288 bytes（30%）。保留 `xiaozhi.bin`、ELF、map、flash_args 和 1,678,548-byte `generated_assets.bin`；flash_args 含 assets `0x800000`。
- 构建目录四个 stroke asset 均存在；manifest 固定 commit、三字符和 `not_a_release_library=true`，NOTICE 含完整来源、hash、许可与修改说明。
- 作者运行前后 `git status --short` 证据均保留原有 Stick-S3、emoji、`.squad`、分区等未提交路径；最终只额外出现本修订预期的 Assets/display/lifecycle 变更。日志中没有 commit/stage/reset/clean，未见用户工作被删除或回滚。

## 下一阶段远端 STT 可依赖的接口边界

可以依赖：

- `StrokeOrderController::BindStore()` 的“有界私有副本 + 失败清旧状态”语义，以及 `Unbind()`/`is_ready()`。
- controller 的线程安全状态操作：`OpenCandidates()`、`SelectCandidate()`、`Pause()`、`Resume()`、`Replay()`、`StepForward(monotonic_ms)`、`BackToCandidates()`、`Exit()`、`Shutdown()`。
- 渲染数据复制接口 `CopyCandidateGlyph()`、`CopyLoadedGlyph()`；不得跨锁持有 store byte view。
- `StrokeOrderView` 现有 `Attach/RebindAssets/SuspendAssets/OnDeviceStateChanged/OnPowerSave/OnInterruption/Shutdown` 的 display-lock 串行和 lifecycle gate 语义。
- CoreS3 packed pointer snapshot 与 one-action touch sequence 作为坐标输入基础。

不得依赖：

- `GetStroke()` 返回值在 controller mutex 释放、Unbind 或 asset 更新后继续有效；它仅是 host-test helper。
- 网络/STT callback 直接操作 LVGL，或仅调用 controller `OpenCandidates()` 就假定页面会刷新。当前 view 没有公开的“提交 ASR 结果并原子呈现候选”入口；下一阶段应新增一个由主任务调度、内部取得 display lock并一次完成 session/gate/controller/view 更新的窄接口。
- 当前三字 smoke 集合提供拼音补全、约 500 字覆盖、服务端 `type=stroke`、request/session ID 或能力协商；这些都尚未实现。

## 尚未验证的真机边界

- 未烧录或启动实体 M5Stack CoreS3。
- FT6336 实际坐标方向、旋转/镜像、边缘命中、拖动/长按及非笔划短触回归未验证。
- 真实 LVGL delete/timer 时序、assets 下载后的 munmap/remap、sleep/wake、alert/普通会话中断和重启未动态验证。
- 点击反馈 100 ms、目标约 30 FPS/持续下限约 20 FPS、atomic 实时开销、音频 underrun、WDT 未测。
- 100 次会话后的 free heap、largest block、timer 数和触摸稳定性未测；1 MiB 上限 blob 加 80 KiB canvas 和 glyph vectors 的峰值 RAM 未压测。
- 当前仅为一/人/口 smoke 数据；正式字库授权、准确性和约 500 字容量不在本次批准范围。
