

<!-- pi-squad:e0058f15e9e4fea0e79f8d3cb662d08ad5177b1a5ff1ce3a4b05df3e471856c3 -->
# squad-2930f30a-6fcc-43b7-9c8e-42feccdffa06
completed; 1/1 successful jobs.
- stroke-final-core-s3-2000-review: # stroke-final-core-s3-2000 独立最终审计

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
