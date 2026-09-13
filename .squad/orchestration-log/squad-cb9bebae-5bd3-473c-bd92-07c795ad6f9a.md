

<!-- pi-squad:df90a450f9494264fe12c6216e6eb88d385f54edb73d4d12e8049b6ecbc35ca4 -->
## tester — stroke-final-core-s3-2000-retry
Mode: write; outcome: completed
Engine: a206edd8-6367-473d-b46c-ba4ad32dbcbd

已完成 `stroke-final-core-s3-2000` 原地重试验证，详细报告写入：`/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/a206edd8-6367-473d-b46c-ba4ad32dbcbd/stroke-final-core-s3-2000-retry.md`。

结论：APPROVED。当前源码与 `/private/tmp/xiaozhi-stroke-2000-final-build` 保留构建图/运行时快照一致：1,095 个含项目依赖的 Ninja target 全部 VALID，1,095 个项目编译对象无缺失且无源码更新，当前审计源 14 个 payload 与 build runtime 逐字节一致，包内 8 个关键构建产物与 build 目录逐字节一致。因此未重跑 clean build，也未重建 package/ZIP。

产物：目录 `dist/m5stack-core-s3-stroke-2000-prototype/`（56 文件、104,430,898 bytes）；ZIP `dist/m5stack-core-s3-stroke-2000-prototype.zip`（38,449,270 bytes，SHA-256 `7eb600eba44509ccb3c6666323d6cd884172e5447482c74121a4a2c74aec7773`）。app/assets/merged 分别为 2,914,784 / 7,568,207 / 15,956,815 bytes，SHA-256 分别为 `a54a0e4d...697da`、`54453b52...83dd0`、`9618a195...29268`。

实际复核：保留全量 host 127 tests（126 pass、1 条外部固定 checkout 条件性 skip）；本轮快速 33 tests（32 pass、同一 skip）；clang-format 19.1.7 与 `git diff --check` 通过。顶层/runtime/audit checksum 分别 55/55、14/14、28/28；ZIP CRC、路径安全、allowlist、解压 checksum 和零差异 diff 全通过。禁止文件名及可读文本秘密模式扫描均为 0。

merged 五段 offset 均逐字节匹配；NVS/phy 区间全 `0xFF`；esptool v5.3.1 对 bootloader/app/merged 的 ESP32-S3、DIO、80 MHz、16 MB、checksum/hash 验证通过。merged 末端未越过 16 MB。2000 字 runtime 恰好 15 文件；8 个 SOB1 共 2000 字/5,800,492 bytes，SCB1 SHA-256 `91779189...19e4`，SPY1 SHA-256 `4597a2a7...6399`；generated assets 含 39 项且 15 runtime 项逐字节一致，assets 余量 820,401 bytes ≥ 256 KiB。

准确首选烧录命令（在 package 根目录）：`python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 --before default-reset --after hard-reset write-flash @flash_args`。该集合不写 NVS/phy；merged 从 0x0 写入会清除 NVS，两种方式必须二选一。

未修改功能源码或 dist，未 commit/stage/reset/clean。残余风险是本轮未真机烧录，仍需验证 WebSocket 动态 Connect→Speak→合法 session STT→Candidates 链、MQTT 本地降级、PSRAM/8 分片 mmap、触摸/动画/音频和 100 次长稳；2000 字仍是未完成商用授权及逐字笔顺审核的技术原型。
