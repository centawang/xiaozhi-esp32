

<!-- pi-squad:a42304c17f798248e41474b91813adacc30e6a705a2554ad0c99567842eb7723 -->
# squad-bc056c65-eea5-4e22-b4cb-7fbc8f7bd8af
completed; 1/1 successful jobs.
- stroke-2000-sharded-library-review: # stroke-2000-sharded-library 独立审查

## Verdict: APPROVED

在所审工作树和保留证据范围内，未发现阻止生成最终 2000 字 CoreS3 烧录包并进入真机验收的 blocker、major 或 minor 缺陷。该批准只覆盖 **M5Stack CoreS3 技术原型**；不表示字库已获官方逐字认证、商业授权或真机性能验收通过。

本轮严格只读：读取了真实源码、fixture、构建目录及 Lead 的原始保留日志；**未修改文件，未运行 shell、测试或构建**。以下测试结果均明确标注为对作者留存输出的核验，而不是本审查者重新执行。

## 严重度结论

### Blocker

- 无。

### Major

- 无。

### Minor

- 无已证实代码缺陷。
- 非缺陷但仍未关闭的验证项：没有 CoreS3 真机上的冷启动全量校验时延/WDT、触摸、帧率、音频 underrun、OTA assets suspend/rebind 和 100 次会话 heap/timer 稳定性数据；详见末尾真机重点。

## 审查证据与判定

### 1. selection-2000、coverage 及 8×250 shard

通过。

- `scripts/tests/fixtures/stroke_order/prototype_2000/selection-2000.csv` 实际从 rank/official number `0001` 连续到 `2000`，首字 `一/U+4E00`、末字 `胶/U+80F6`；`charset-2000.txt` 对应同一顺序。
- `stroke_order.cov.json` 声明且由 `verify_source()` 深度核对 `character_count=2000`、`ok_count=2000`、`error_count=0`、每条记录 `status=ok`，并核对 `(codepoint, official_rank, shard)` 与 selection 完全一致。
- `package_stroke_order_2000.py::verify_source()` 不只信任 SHA 文件：它加载 selection、charset、SCB1、全部 SOB1、SPY1 和 coverage；验证每个 codepoint 只出现一次，每片 250 字，rank 分片为 1–250、251–500……1751–2000，并逐字核对 `rank/shard/local_index`。
- 八片实际固定大小为 `443020, 603800, 680236, 723504, 771996, 816964, 849284, 911688` B，总计 `5,800,492` B；每片均小于 1 MiB。
- 作者保留的完整再生成输出显示：固定 HWD、transcription、官方 PDF、Unihan 输入重新生成后，`file_count 29 expected_count 29 exact_match True`。

### 2. SCB1 唯一映射、hash/CRC 与损坏拒绝

通过。

- Python `scripts/stroke_order/catalog.py` 与 C++ `main/stroke_order/stroke_order_catalog.{h,cc}` 对齐为 SCB1 v1：32-byte header、12-byte entry、40-byte shard descriptor，分别校验 header/body CRC32。
- 两端均检查 canonical offsets、精确文件尾、reserved 字段、Basic-CJK 范围、严格递增 codepoint、唯一且连续 rank、唯一 shard name、每 shard 完整且唯一 local index、rank range/count、shard size 上限及总大小。
- `StrokeOrderController::ValidateCatalogShardsLocked()` 逐片检查 descriptor size、整片 CRC、SOB1 bind，并对每个 local index 调用 `LoadCharacter()`；后者再校验单字 record CRC、笔画/点数/坐标及完整消费记录。因此 C++ rebind 实际遍历并解析全部 2000 字，而不是只读取 header/index。
- Python 测试覆盖 body CRC、重复 codepoint、错误 offset、错误 shard；C++ harness 还覆盖重复 local index、未终止 asset name、错误 total、trailing byte、missing/swapped shard、SCB1/SPY1 corruption。
- 固定 catalog 实际为 24,352 B，SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`。

### 3. SPY1 2048/63618/最大 group 与无截断

通过。

- Python `pinyin_constants.py` 和 C++ `stroke_order_pinyin.h` 均为 `MAX_CHARACTERS=2048`、`MAX_GROUPS=2048`、`MAX_READINGS_PER_CHARACTER=8`、`MAX_GROUP_MEMBERS=32`、`MAX_FILE_BYTES=65536`。
- fixture 和测试确认 SPY1 为 63,618 B，距 64 KiB 上限 1,918 B；2000 characters、1047 groups、最大 group 29 members。
- `union_readings()` 对超过 8 readings 直接抛错；`pack_spy1()` 对 group 超过 32 members 直接抛错，不做切片或静默截断。
- Python/C++ validator 都验证 canonical payload、双向 character↔group reciprocal membership、rank 一致、CRC、reserved、排序、重复、越界与 trailing bytes。
- 固定 SPY1 SHA-256 为 `4597a2a7134d4d40e5b025b339d3491bbcaf05f75f7485dee4db7555429b6399`。

### 4. Controller 单 shard lease、mmap 生命周期与 owned glyph

通过。

- `StrokeOrderShardSource` 明确定义 `AcquireShard/ReleaseShard`；`ScopedShardView` 在所有成功/失败返回路径上 RAII release。
- 生产 `AssetsStrokeOrderShardSource` 直接返回当前 assets mmap 中的文件视图，不复制 5.8 MiB corpus；descriptor 将单片限制在 1 MiB，固定最大片为 911,688 B。
- Controller 只常驻拥有 24,352 B SCB1 copy 和 63,618 B SPY1 copy。候选及当前 glyph 均经 `CopyGlyphLocked()` 解码到 `std::vector` owned cache；生产代码不公开 borrowed `StrokeView`，旧 getter 仅在 `STROKE_ORDER_TESTING` 下存在。
- 所有生产 shard 访问均由 Controller mutex 包围；调用点来自持有 display/LVGL serialization 的 View。`Assets::UnApplyPartition()` 必须先成功执行 `StrokeOrderView::SuspendAssets()`，再 munmap；若拿不到 display lock，会拒绝 unmap。
- Suspend 顺序为关闭 overlay/清除 View glyph cache，再同步 unbind Controller 与 SPY1；Controller 清除 shard source、catalog/store、候选和 loaded glyph。
- Rebind 先清空旧代，再依次拥有/验证 SPY1、复制/验证 catalog、逐片全量验证，最后逐个比对 SCB1 与 SPY1 的 2000 个 codepoint/rank；任何一步失败都会再次统一 suspend，SO 入口保持隐藏。
- C++ harness 的保留输出为 `stroke_order_sharded_harness: PASS acquires=27 max_active=1 max_bytes=911688`，且覆盖跨 shard 候选、选 shard 7、owned glyph 在 suspend 后仍有效、完整 rebind、缺片/交换片/损坏/合法但 rank 不匹配的 SPY1 fail-closed。

### 5. Runtime pack、closed checksum、asset 名和 CMake

通过。

- `COPY_MAP` 固定生成恰好 15 个 runtime 文件：`so00.bin`…`so07.bin`、`stroke_cat.bin`、`stroke_pinyin.bin`、`runtime.json`、`ARPHICPL.TXT`、`UNICODE-LICENSE.txt`、`NOTICE.md`、`SHA256SUMS`。
- selection、charset、source lock、coverage、SPY1/per-shard manifests 均仅留在源码 fixture，不进 runtime pack。实际 clean-build `stroke_order_assets/` 列表与上述 15 项一致。
- `verify_checksum_directory()` 要求目录文件集合与期望集合完全相等、全是普通非 symlink 文件；`SHA256SUMS` 必须对其余 14 项各覆盖一次、不得重复/自引用，并逐项重算 SHA-256。
- 运行时名均不超过 assets 表允许的 31 UTF-8 bytes；catalog 中 shard 名与实际 `soNN.bin` 一致。
- `main/CMakeLists.txt` 声明全部 15 outputs、全部 fixture inputs、packager 及其直接/间接 Python 模块依赖。保留的最终 `build.ninja` 记录证实这些 outputs/dependencies 真实进入构建图。
- 最终 `generated_assets.bin` 解析证据显示共 39 个 assets，outer additive checksum 正确，其中上述 15 个 stroke 文件的 size/SHA-256 与 runtime package 一致。

### 6. 8 MiB 容量门禁及 local emoji 保留

通过。

- `build_default_assets.py` 在发布输出前执行：`total_size < max_output_bytes` 且 `max_output_bytes-total_size >= min_free_bytes`；失败发生在 `os.replace()` 前，因此不会覆盖既有最终 image。边界测试保留了旧 output，并实际得到 `262143 < 262144` 的拒绝结果。
- CMake 从真实 assets partition 读取 `size`，stroke build 固定传入 `--max_output_bytes <partition-size> --min_free_bytes 262144`。
- IDF clean build 的 8 MiB assets partition为 8,388,608 B，`generated_assets.bin` 为 7,568,207 B，余量 820,401 B，明显高于 256 KiB。
- `get_emoji_collection_path()` 仍优先选择 `main/assets/emoji/<collection>`；CMake 仍保留 Stick-S3 的 `DEFAULT_EMOJI_COLLECTION otto-gif-135` 及本地 emoji dependency glob。未发现本 artifact 覆盖/删除用户 local emoji 工作。该保持性为静态源码验证，本轮没有另跑 Stick-S3 固件构建。

### 7. 500/STT/session 安全回归

通过。

- 旧 `prototype_500` 与对应 generator/package/harness 测试仍在；最终 host suite 中 500 selection、coverage、SOB1/SPY1、provenance 和 packager 项均通过。
- 当前 production 保持 local-stroke 构建优先 WebSocket；WebSocket 声明 correlated open/stroke voice，MQTT 明确两项均为 false。
- MQTT 分支只进入本地 TopRanked candidates，不打开 stroke audio channel、不启动 microphone；WebSocket 才进入相关联的远端 STT。
- `StrokeRoundCoordinator` 仍以非零 generation、精确 server session id、open-attempt identity、cancel fence 和 route epoch 拦截/拒绝迟到、错 session、缺 session 和跨轮消息；失效 stroke start 不会降级成 generation-0 普通监听。
- 相关 production-shaped C++ harness、AutoStop、audio processor postcondition、async close/cancel fence 及 ordinary chat bypass 均包含在最终 127 项通过结果中。

### 8. 验证记录可信度

通过。

- 保留日志中有较早的失败/`skipped=1` 中间尝试，也记录了一次临时 CMake 拼写错误；这些不能作为最终证据。之后错误已修复，并有全新 `/private/tmp/xiaozhi-stroke-2000-retry-src-20260913-012457` source snapshot 和独立 build dir 的成功结果。
- 最终 host run：`Ran 127 tests in 23.055s`，结尾为裸 `OK`，无 skip。
- 最终专项 C++ harness：Clang、`-Wall -Wextra -Werror -fsanitize=undefined`，PASS。
- 最终静态检查：`clang-format version 19.1.7` dry-run 通过；`py_compile` 无错误；`git diff --check` 返回 0。
- 最终 IDF build：ESP-IDF v6.0.2，`CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y`、`CONFIG_FLASH_DEFAULT_ASSETS=y`、`CONFIG_SPIRAM_MODE_QUAD=y`、`partitions/v2/16m.csv`；日志编译了 catalog/assets/pinyin/controller/view，最终 `Project build complete`。
- 产物：
  - `xiaozhi.bin`: 2,914,800 B，SHA-256 `48fbf45cddc2a87ecf7e38cb10b21cd1326b88cdeca37a06371c79461c75b7d0`；app partition 余 1,213,968 B（29%）。
  - `generated_assets.bin`: 7,568,207 B，SHA-256 `54453b5232408dfb8cf8dadbde9dd5d93a1bdb90f213df69d7ff075210783dd0`；assets partition 余 820,401 B。
  - `bootloader.bin`: 16,672 B，SHA-256 `4dcdf209fed6c50e2b175c0fdb47ceab00cbe829423f158657748f2d295151c3`。
  - `partition-table.bin`: 3,072 B，SHA-256 `4811619cacae08ef2e0e71b7220c6033a346ca5da7ca179082408c963ef530b5`。

## 许可与产品边界

通过，且必须继续保持当前限制。

- Runtime 包携带完整 `ARPHICPL.TXT`、`UNICODE-LICENSE.txt` 与 NOTICE。
- `stroke_order.src.json` 固定记录政府 PDF hash/size、HWD commit、transcription commit/hash/origin；NOTICE 明确 transcription aid 仓内无显式许可证、未做双人逐页 PDF 核对、未做逐字笔顺人工审核。
- 文档、Kconfig 和 runtime manifest 均明确 `prototype`、`not_a_release_library`、`not_commercially_reviewed`、`not_official_certification`，未宣称官方逐字认证或可商业发布。

## 放行范围

**允许**基于当前已审源码状态生成最终 2000 字 CoreS3 包并烧录到 M5Stack CoreS3 做硬件验收。若源码、fixture、CMake、分区或依赖发生变化，应重新运行完整 host suite 与 IDF 6.0.2 clean build，并重新记录 app/assets hashes 和余量。

本批准不等于商业发布批准，也不等于真机验收完成。

## 真机验收重点

1. 冷启动记录 assets outer checksum、SPY1/SCB1/8 shards 全量验证总耗时，确认无 task-WDT、启动卡死或长时间黑屏，SO 入口仅在验证完成后出现。
2. 逐片验证 rank 边界字：1/250、251/500、501/750、751/1000、1001/1250、1251/1500、1501/1750、1751/2000；再测一个实际跨 shard 同音候选组，确认候选、选字、动画字形一致。
3. WebSocket：点击 SO 后必须进入 Connect→Speak→STT→Candidates；迟到/错 session STT、TTS、LLM、audio 必须丢弃或终止当前笔划轮次，不能落入普通对话。
4. MQTT-only：不得声称或启动笔划语音；只能安全进入本地候选页，普通 MQTT 对话保持正常。
5. 触摸：overlay 独占；每个触摸序列只触发一次；退出后未被 SO 控件消费的短触仍执行原 `ToggleChatState`；覆盖边缘、长按、连点和拖出按钮。
6. 动画：暂停/继续/重播幂等、逐笔防抖、返回候选、退出和新普通会话立即取消 timer；目标约 30 FPS、持续不得低于约 20 FPS。
7. 在 Candidates、Animating、Paused、Completed 各状态执行 assets 更新/失败重绑/电源休眠，确认 overlay 同步关闭、SO fail-closed、没有旧 mmap 访问；成功更新后可完整 rebind。
8. 连续至少 100 次语音选字→跨 shard 候选→播放→返回/退出，记录 free heap、largest free block、PSRAM、timer/task 数、音频 underrun、触摸失效与 WDT；不得出现单调泄漏。
9. 烧录前核对最终 build 的 board/partition/Kconfig，并重算 `xiaozhi.bin`、`generated_assets.bin`、bootloader、partition table hash；烧录后确认 assets 位于 `0x800000` 且实际分区表为 8 MiB assets。
