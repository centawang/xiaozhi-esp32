

<!-- pi-squad:5091aab2f4fd0c2d0daca418d0c447a5dbc5f13e910a0694d1a4272179be739d -->
# squad-4e34d1c8-58b6-47ab-af33-b7870ca0ace7
completed; 1/1 successful jobs.
- stroke-final-core-s3-500-review: # stroke-final-core-s3-500 独立最终审计

## 结论

**REJECTED**

固件与数据产物本身具有充分的“可烧录”离线证据：ESP-IDF v6.0.2 全新外部目录构建成功、核心单元全部编译、镜像 offset/容量/逐字节比较正确、esptool 校验有效、目录和 ZIP 完整性均通过。拒绝原因不是二进制失效，而是包内首选烧录说明对 NVS 的影响不准确，可能导致用户在未预期的情况下丢失配网、激活等持久状态；用户要求“实际可烧录且说明准确才 approved”，因此当前包不能批准。

## 严重级别

### Blocker

无。

### Major

#### M1. 首选 merged 烧录会覆盖/清空 NVS，但 README 暗示只有显式 `erase-flash` 才会清除 NVS

证据：

- `dist/m5stack-core-s3-stroke-500-prototype/README.zh-CN.md:44-50` 将 `0x0 merged-binary.bin` 标为首选方式。
- 同文件 `:60` 写道“只有需要清除原配网/NVS 状态时才先执行” `erase-flash`，容易使用户认为首选 merged 烧录会保留现有 NVS。
- 实际分区表 `/private/tmp/xiaozhi-stroke-500-final-build/partition-table.decoded.csv` 显示 NVS 位于 `0x9000`、大小 16 KiB。
- 此 merged 是从 `0x0` 开始、长 `11,266,414` bytes 的 raw 合并文件。所用 esptool v5.3.1 的 `merge_bin()` 在输入镜像间以 `0xFF` 填充空洞：`.../site-packages/esptool/cmds.py:2966-2968`。因此 partition table 结束后的 `0x9000..0xcfff` NVS 区间在 merged 文件中是填充数据，而不是“未包含、烧录时跳过”的洞。
- 同一 esptool 的 `write_flash()` 按单个输入文件的连续 `address + image_size` 计算擦除区间：`cmds.py:1374-1388`。从 `0x0` 写入这个 merged 文件会擦除覆盖到约 `0xabefff` 的连续扇区，包含 NVS（以及未单独提供 payload 的其他间隙/OTA1 区域）。
- `manifest.json:73` 仍把该文件称为 `preferred_merged_flash_image`；`:294` 的 `nvs_partition_payload_included=false` 虽说明没有携带凭据，但没有说明 merged 的 `0xFF` padding 会擦除设备上已有 NVS。

影响：按照当前“首选”命令烧录会导致已配置设备丢失 Wi‑Fi、激活等 NVS 状态并需要重新配置，与 README 的擦除说明不一致。这是用户可见的数据丢失/操作风险。

修复要求：

1. README 必须明确说明 `0x0 merged-binary.bin` 是连续 raw 镜像，会擦除其覆盖范围内的现有 NVS；不能再表述为只有先执行 `erase-flash` 才会清除 NVS。
2. 如希望保留配网/激活状态，应把 `@flash_args` 多文件方式作为保留 NVS 的首选方式；若仍将 merged 作为首选，必须醒目标为“会重置 NVS，适用于全新/允许重新配网的设备”。
3. manifest 同步记录 merged 的 NVS 擦除语义。更新 README/manifest 后重建顶层 `SHA256SUMS` 与 ZIP，并重新做目录、ZIP CRC、解压后 checksum 校验。固件二进制本身无需因本问题重编译。

### Minor

无独立 minor。

## 已通过的核对项

### 1. Host tests 与唯一 skip

- 最终保留日志 `/private/tmp/xiaozhi-stroke-500-host-tests.log`：`Ran 113 tests in 14.518s`，`OK (skipped=1)`，即 112 pass、0 failure、0 error。
- 唯一 skip 是 `test_production_selection_api_is_pinned_and_rebuilds_committed_outputs`；源码 `scripts/tests/test_stroke_order_pinyin.py:243-267` 显示它只在没有设置 `STROKE_TRANSCRIPTION_JSON` 时 skip，条件正是缺少外部 clean pinned transcription checkout。它不是设备功能测试的条件禁用。
- 非 skip 证据确实存在：`/private/tmp/stroke-500-revision-host-final2.log` 中同一测试为 `ok`，整套 `Ran 113 tests in 13.639s / OK`；`stroke-500-revision-host.log` 和 `...host-final.log` 也记录同项 `ok`。
- `/private/tmp/stroke-500-revision-generate.log` 记录真实重生成 500 字输出：SOB1 `1,046,788` bytes / `3ca8...6f3b`，SPY1 `18,152` bytes / `a976...998`；仍保留的生成目录包含完整 14 文件。
- 因此最终那 1 个 skip 仅源于外部 pinned checkout 未注入；已有非 skip 的 pinned 重建与 committed-output 对比证据，不掩盖运行时功能。

### 2. ESP-IDF v6.0.2 clean build

- `configure.log` 显示在仓库外 `/private/tmp/xiaozhi-stroke-500-final-build` 配置，`SDKCONFIG` 也位于该目录；target 为 `esp32s3`，IDF 为 `v6.0.2`。
- `sdkconfig` 验证：`CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y`、`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`、`CONFIG_SPIRAM=y`、`CONFIG_SPIRAM_MODE_QUAD=y`、`CONFIG_CAMERA_GC0308=y`。
- `build.log` 完成 2214-step build 并出现 `Project build complete`。日志确认编译：Application、AudioService、WebSocketProtocol、MqttProtocol、M5Stack CoreS3 board、StrokeOrderStore、StrokeOrderController、StrokeRoundCoordinator、StrokeOrderPinyinIndex、StrokeOrderView。
- 构建执行 `Packaging reviewed 500-character stroke-order prototype`，处理 14 个 extra files；保留的 offset 检查日志确认 generated-assets 文件表含全部 14 个 basename。
- `clang-format` 保留证据为 19.1.7，16 个 tracked changed-line 文件与 18 个新增完整文件通过；`git diff --check` 日志为空，表示通过。

### 3. Flash 布局、容量与镜像

保留产物和 checksum 一致：

| 镜像 | offset | 大小 | SHA-256 |
|---|---:|---:|---|
| bootloader | `0x0` | 16,672 | `1cf7897c69aaf046202e438fad724d692f701a343c85b15cefd0b0420cccce51` |
| partition table | `0x8000` | 3,072 | `4811619cacae08ef2e0e71b7220c6033a346ca5da7ca179082408c963ef530b5` |
| OTA data | `0xd000` | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| app | `0x20000` | 2,908,800 | `a49373c7a38329023b850a45e0c51426d60236dd5b0b8337e9498df38b52af62` |
| assets | `0x800000` | 2,877,806 | `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0` |
| merged | `0x0` | 11,266,414 | `4a22047b840bd3459cb1ad4fa158567a1e3be2927afd23413df64eca09964fbb` |

- 分区表为 NVS `0x9000/16K`、otadata `0xd000/8K`、phy `0xf000/4K`、OTA0 `0x20000/4032K`、OTA1 `0x410000/4032K`、assets `0x800000/8M`。
- app 结束于 `0x2e6280`，低于 OTA0 末端 `0x410000`，余 `1,219,968` bytes（29.55%）。
- assets/merged 结束于 `0xabe96e`，低于 16 MB 末端 `0x1000000`，余 `5,510,802` bytes；无越界。
- `/private/tmp/xiaozhi-stroke-500-final-build/offset-byte-compare.log` 对 `0x0`、`0x8000`、`0xd000`、`0x20000`、`0x800000` 五段均为 PASS，并确认 14/14 stroke assets 出现在 generated-assets 表。
- esptool v5.3.1 的 bootloader/app/merged image-info 均识别 ESP32-S3、DIO、80 MHz、16 MB；bootloader/merged checksum `0xd6` valid，app checksum `0xde` valid，validation hash 均 valid。merged 的 image-info 只能直接识别开头 bootloader，其他段由五段 byte compare 补证。

### 4. 目录、ZIP 与 checksum

- 真实目录 `dist/m5stack-core-s3-stroke-500-prototype/` 存在，共 26 个普通文件；`stroke_order/` 实际列出 14 个文件。
- 顶层 `SHA256SUMS` 有 25 项，覆盖除自身外全部普通文件，并包含内层 `stroke_order/SHA256SUMS`；保留目录校验日志 25 项全为 OK。
- 内层 `stroke_order/SHA256SUMS` 有 13 项，覆盖 14 文件集合中除自身外的全部文件；保留内层日志全为 OK。
- ZIP CRC 日志显示 26 个文件均 OK，结尾为 `No errors detected`；解压到 `/private/tmp/xiaozhi-stroke-500-final-unzip/` 后再次执行顶层 checksum，25 项全为 OK。
- ZIP：`dist/m5stack-core-s3-stroke-500-prototype.zip`，23,929,854 bytes，保留 SHA-256 证据为 `9fd6dfcb7e04ece3732381385ef4a4620b721f00806c3c8227216e0e60c86a5a`。

### 5. 500 字数据与许可边界

- `selection-500.csv` 实际含连续编号 0001–0500；host 测试验证 500 行唯一字符及 codepoint 一致。
- `stroke_order.cov.json` 为 `character_count=500 / ok_count=500 / error_count=0 / complete=true`；`stroke_pinyin.cov.json` 为 `character_count=500 / ok_count=500 / complete=true`。
- SOB1：magic/version `SOB1/1`，1,046,788 bytes，SHA-256 `3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`，距 1 MiB 上限仅 1,788 bytes。
- SPY1：magic/version `SPY1/1`，18,152 bytes，SHA-256 `a97666f18fa8e5a5e586f774dac044c08ba26b40decb4644e12c6886a5cbc998`，距 64 KiB 上限 47,384 bytes。
- 完整 `ARPHICPL.TXT` 和 Unicode License v3 均在包内。`NOTICE.md` 清楚披露 HWD 固定 commit、转换/修改方式、APL、Unicode 16.0.0、无明确 license 的 transcription checkout 仅作原型转录辅助、非官方数字附件、未双人逐页核对、非正式/商用字库且外部分发前仍需法律与笔顺准确性审核。边界说明合格。

### 6. 凭据、协议说明、dirty source 与硬件边界

- 包文件集合不含 sdkconfig、NVS payload、私钥或临时日志；`flash_args` 也没有 NVS 镜像。因此没有发现被打包的设备凭据。注意这与 merged 烧录会以 padding 擦除设备现有 NVS 是两个不同问题。
- README 的行为说明与源码一致：WebSocket 显式支持 correlated open/stroke voice，STT 目标字优先并由 SPY1 同音候选补足到最多 6 个；MQTT 两项 capability 均为 false，转入本地 `AppendTopRanked` 候选，不宣称 MQTT voice selection。
- README 和 manifest 均明确披露 dirty worktree、base commit `8836458c95d52b7e1335837315ec4b000e581e53` 不能单独复现二进制，以及打包前 48 个 dirty paths。
- README/manifest 均明确本轮未真机。仍未验证 CoreS3 的 PSRAM 启动与分配、FT6336 坐标/旋转、SO 入口、overlay 独占和原短触兼容、真实 WebSocket STT/session、MQTT 降级、FPS、音频、断线/超时及 100 次会话稳定性。

## 本次审计执行说明

本审计遵守只读要求：只读取真实 dist、源码、构建目录和保留日志；**未运行 shell、未重跑测试/构建、未烧录、未修改文件**。上述运行结果均来自逐项读取的保留日志，不冒充本审计新执行结果。
