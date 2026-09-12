

<!-- pi-squad:3bbfd4caa1dd39c92a315e12be519939a39c626aa2c7f271e4f7897edbe03e66 -->
# squad-fb7f592a-7613-4816-bacd-4d2544bc6cbe
completed; 1/1 successful jobs.
- stroke-ui-cores3-revision: 已原地完成 `stroke-ui-cores3` 修订并将完整报告写入 `/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/44bc0a4c-7b74-431c-a7c3-062043ba21b1/stroke-ui-cores3-revision.md`。B1/B2、M1–M6、m1 已逐项修复，m2 也已修复：controller 改为最多 1 MiB 的 owned blob；Assets 在 munmap 前同步 suspend/unbind、Apply 后无条件 rebind；LVGL 点击改为 LVGL task 内直接串行执行并移除 generation/Schedule TOCTOU；引入生产 lifecycle gate；逐笔改用显式单调毫秒；CoreS3 改为单个 packed atomic touch snapshot 并抽取 sequence tracker；静态状态暂停/删除 timer；power-save、DeviceState、alert、shutdown 和外部 delete 均取消并正确 gate UI；四个资产产物全部声明为 CMake OUTPUT，打包使用 staging + 原子目录发布；DisplayLockGuard 记录持锁状态、仅持锁才 Unlock，锁失败 fail-fast。未实现远端 STT/type=stroke，未 commit/stage/reset/clean，原有 Stick-S3/emoji/.squad 等工作仍保留。

验证：clang-format 19.1.7 dry-run、git diff --check、Python py_compile 均通过；专项 29/29、全 host 96/96 通过；实际删除 NOTICE 后增量构建触发重打包并恢复；ESP-IDF v6.0.2 在全新隔离目录 `/tmp/xiaozhi-stroke-ui-cores3-final-build-44bc0a4c` 以 `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y` 完成 2212-step 全量构建。最终 `xiaozhi.bin` 2,880,480 bytes，app 分区剩余 1,248,288 bytes（30%）。bin/elf/map/flash_args/generated_assets.bin 均保留在该目录。未做真机烧录、触摸/帧率/heap/音频长稳验证。
