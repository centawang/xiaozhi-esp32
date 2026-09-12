

<!-- pi-squad:ad9a2117562e55497552a4998a44afff13b576f980f5c61c003ae6c5b45545fc -->
## tester — stroke-local-foundation-rereview-retry
Mode: review; outcome: completed
Engine: 4bd164a0-12c4-4c30-824f-5cadde8f46fe

# stroke-local-foundation 修订复审

**Verdict: APPROVED（仅限 local data foundation 范围）**

本次是独立重试复审；未沿用此前因 stream 中断而未产生 verdict 的复审结果。审查了当前真实工作区中的 parser、converter、validator、测试、fixture、Kconfig/CMake 与文档，并核对了 Lead 保留的真实命令输出及 `/tmp` 中仍存在的构建/转换产物。按要求，本 Reviewer **未修改文件，也未自行运行 shell、测试或构建**。

原拒绝项 B1/B2、M1–M5 及 m1–m4 均已达到本阶段关闭条件。没有发现阻止 UI 阶段开始依赖该基础层的新 blocker。

## 原拒绝项关闭情况

### B1 — CLOSED：不再把编码字节别名为 `Point*`

- `main/stroke_order/stroke_order_store.h:79-98` 的公开接口只给出 `outline_count()`、`median_count()`、`GetOutlinePoint()`、`GetMedianPoint()`；两个底层地址是私有 `const uint8_t*`。
- `main/stroke_order/stroke_order_store.cc:92-110` 按索引逐字节显式 little-endian 解码到调用者拥有的 `Point` 值。
- 当前 `main/stroke_order` 中没有 `reinterpret_cast<...Point>`、编码区 `Point*` 或 `sizeof(Point)` 序列化依赖；整数宽度与四字节点编码有静态断言。
- parser 的校验路径也通过 `ReadU16`/`ReadPoint` 解码，不依赖宿主端序或地址对齐。

### B2 — CLOSED：生产 C++ parser 已真实编译、链接并执行

- `scripts/tests/test_stroke_order.py:553` 编译独立 executable，命令同时包含 `scripts/tests/stroke_order_store_harness.cc` 与生产文件 `main/stroke_order/stroke_order_store.cc`，并启用 `-fsanitize=undefined -fno-sanitize-recover=all`。
- 保留输出显示手工执行的同一 harness 返回 `stroke_order_store_harness: PASS`。
- `scripts/tests/stroke_order_store_harness.cc` 覆盖：Python 生成物、独立 golden、真实三字 corpus、`buffer + 1` 未对齐输入、header/index/record 损坏、错误 record CRC、index/record codepoint 不一致、截断/超大文件、offset/length/data-size 溢出、character/stroke/point count 超限、点边界与越界访问，以及 Bind/Load 失败保状态。
- 另有 ESP-IDF Xtensa 编译器 `-fsyntax-only` 成功证据，以及下述完整 CoreS3 固件构建证据。

### M1 — CLOSED：provenance 已绑定真实 checkout 与输入 bytes

- `scripts/stroke_order/convert.py:48, 441` 只接受完整 40-hex commit。
- `_verify_git_checkout()` 核对 git top-level、`HEAD^{commit}`、传入 commit、`remote.origin.url`，并要求 `git status --porcelain=v1 --untracked-files=all` 为空。
- `_verify_source_file_at_head()` 对每个选中的相对路径执行 `git show HEAD:path`，要求其为 HEAD 中 tracked blob，并比较 HEAD bytes 与工作区读取 bytes 的 SHA-256。
- manifest 记录 verified HEAD、实际 origin、`checkout_clean: true`、每文件路径/SHA-256、排序后的 aggregate SHA-256 以及二进制 SHA-256。实际 `/tmp/stroke-smoke-out/stroke_order.manifest.json` 与实现一致。
- 测试覆盖短 SHA、错误 HEAD、dirty checkout、错误 origin，以及被 ignore 但未进入 HEAD 的伪装输入。

### M2 — CLOSED：已有独立 oracle 与跨实现验证

- `handwritten_sob1_v1.hex` 是固定 84-byte SOB1 字节串，不由 converter 在测试时生成；Python 测试还锁定其完整 hex。
- Python 与真实 C++ CRC 实现都验证标准 known answer：`CRC32("123456789") == 0xCBF43926`。
- golden 固定并重算 header/index/record CRC：`0x7ce14721`、`0x6e6cb232`、`0x3bdfc874`，且包含坐标边界 0/1024。
- C++ harness 同时解析手工 golden 与 Python converter 输出，因此不再仅依赖共享 Python constants/validator 的锁步自证。

### M3 — CLOSED：SOB1 index 已有完整 CRC 保护

- v1 仍为 32-byte header，但 offset 28 已统一定义为 `index_crc32`，覆盖完整 `[index_offset, data_offset)`。
- packer 在 `scripts/stroke_order/convert.py` 写入 `crc32(index)`；Python validator 在解析 entries 前验证；C++ `Bind()` 在遍历或暴露 index 前验证。
- header、Python struct、C++、文档、golden 与 corruption tests 对 CRC 字段和范围一致；单 record CRC 继续在 `LoadCharacter()` 时校验。

### M4 — CLOSED：charset/path/pack 边界已收紧

- charset 与 pack 层都限制为单个 U+4E00..U+9FFF 字符，明确拒绝 ASCII、emoji、surrogate、扩展/兼容区、多字符和路径分隔符。
- 直接目录与 `data/` 布局均在 `resolve(strict=True)` 后做根目录归属检查；有 symlink escape 测试。
- `pack_characters()` 已从 package `__all__` 移除，同时自身强制容器、数量、字符/codepoint 一致性、去重、stroke/point 数量、整数坐标范围及闭合轮廓，不能绕开 converter 前置校验生成违反 parser 契约的 blob。

### M5 — CLOSED（基础层范围）：真实上游 smoke corpus 已落地

- 保留命令输出显示 GitHub remote HEAD 为 `68d10a4b21150cae5e1ebbd223eed289cf32d90c`，随后从该 origin clone、detached checkout；当前 `/tmp/hanzi-writer-data-smoke/.git/HEAD` 与 origin config 仍对应此值。
- 仓库只保存 一、人、口 三个真实 JSON；保留 `cmp` 输出为 `verbatim-match`，并记录固定 SHA-256。
- 完整 `ARPHICPL.TXT` 已纳入，测试锁定 SHA-256 `5590533436c70f10f2f524ee61456238c290175c6662fbe1c700b5f038a6d328`；`NOTICE.md` 记录 repo、commit、用途、原始路径、逐文件哈希、aggregate、转换修改和非发布字库边界。
- 实际 clean checkout 转换得到 2668-byte blob，SHA-256 `9750f7ad01958f48df78102e8facd5183b58646f4350084aa05c03fba18b225f`；Python validator 与 C++ harness 均加载三字及其 outline/median 点。
- 尚无视觉 renderer/golden；这不阻塞纯数据基础层，但必须在 UI 阶段用截图或真机确认真实轮廓与 median 的缩放、方向和对齐，不能把此次批准解释为视觉效果已验收。

### Minor — CLOSED

- **m1**：实现拒绝 URL/UNC spelling，文档明确无法判断普通路径底层是否为网络挂载，不再过度承诺。
- **m2**：JSON `NaN`/`Infinity` 由 `parse_constant` 拒绝，median/path/device conversion 均检查 `math.isfinite()`，错误进入受控 `ConvertError`/`PathError`。
- **m3**：`pack_characters()` 不再顶层导出，且自身执行完整格式不变量校验。
- **m4**：保留证据显示安装并实际使用 `clang-format version 19.1.7`；对三个 C++ 文件执行 `-i` 后 `--dry-run -Werror` 成功。最终 `git diff --check` 亦为 exit 0。

## 验证证据

Lead 保留的最终命令结果：

- `python3 -m unittest scripts.tests.test_stroke_order -v`：**24 tests / OK**；其中测试真实编译并运行 C++ harness。
- 独立手工 harness 命令：**`stroke_order_store_harness: PASS`**，带 UBSan。
- `python3 -m unittest discover -s scripts/tests -v`：最终 **91 tests / OK**。
- ESP-IDF Xtensa parser syntax compile：exit 0。
- 隔离 CoreS3 构建：`IDF_VER="v6.0.2"`，临时 sdkconfig 明确为 `CONFIG_IDF_TARGET="esp32s3"`、`CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y`；`build.ninja` 与 `.ninja_log` 都证明生产 parser object 进入并完成构建。
- 构建日志生成 `xiaozhi.bin`，大小 `0x2ba350`（2,859,856 bytes），最小 app partition `0x3f0000`，剩余 `0x135cb0`（31%）；`xiaozhi.bin`、ELF、map 与 flash args 仍存在于隔离 `/tmp/xiaozhi-stroke-core-s3-build`。
- `clang-format 19.1.7 --dry-run -Werror` 与 `git diff --check`：成功。
- 最终工作区证据仍列出既有 Stick-S3、emoji、`scripts/build_default_assets.py`、partition、`.squad` 等未提交项；根目录 `sdkconfig`、`sdkconfig.old`、`build/` mtime 保持构建前值，构建状态写入 `/tmp`。没有 commit/stage/reset/clean 证据。

## UI 阶段必须遵守的 API 约束

1. 只能通过 `GetOutlinePoint()` / `GetMedianPoint()` 取得解码值；不得恢复 typed pointer、reinterpret cast、宿主端序读取或对 mmap 对齐的假设。
2. blob 由调用方持有；其内存必须覆盖 `StrokeOrderStore` 及所有 `StrokeView` 的使用期。不得在 blob 释放、unmap、替换、`Unbind()` 或重新 `Bind()` 后继续使用旧 view。
3. `StrokeOrderStore` 明确非线程安全；加载、页面切换和动画访问必须串行化，不能让后台 callback 与 LVGL/render 同时读写 store 状态。
4. 必须检查 `Bind`、`LoadCharacter`、`GetStroke` 和点访问器的 bool 结果。`Bind()` 只验证 header/index；动画前必须让 `LoadCharacter()` 成功，以完成目标 record CRC 与结构校验。
5. Bind/Load 失败保留先前成功状态；UI 不得把“调用失败”误解为“状态自动清空”，取消/切换会话时应显式 `Unbind()` 或替换自身会话引用。
6. 坐标契约为 0..1024、y-down，只在 View 中缩放到田字格；设备端不得重新解析 SVG。
7. `CONFIG_STROKE_ORDER_LOCAL` 仍默认关闭且仅依赖 CoreS3；在有效资产 mmap、坐标触摸与 UI 控制器完整接入前不得公开入口或能力。

## 约 500 字发布数据仍需满足的条件

- 使用明确、版本化且人工审核的《通用规范汉字表》简体字符清单；U+4E00..U+9FFF 只是结构边界，不是简体或规范笔顺证明。
- 对发布数据使用真实 clean checkout、完整固定 commit、匹配 origin、HEAD tracked-byte 校验；保留每文件 SHA-256、aggregate SHA-256 与最终 binary SHA-256。aggregate 是内容指纹，不应替代独立审核 allow-list。
- 正式商业发布前完成授权/法务复核与逐字大陆规范笔顺准确性审核；不得把 upstream 自述或三字 smoke 当作官方逐字认证。
- 按 APL 要求随产品/分发物提供完整许可证、显著修改说明，并确认衍生数据可获取/再分发方式符合许可证；三字 fixture 不能冒充发布字库。
- 对约 500 字实际产物重新验证 parser 上限、单字点数、总文件大小、assets 分区和 CoreS3 固件余量，记录相对基线的 flash/heap 增量。
- UI 阶段增加真实字形视觉 golden 或真机视觉 smoke，并完成 CoreS3 asset mount/mmap、触摸、动画、取消/重播及 100 次稳定性测试；当前批准不等同于真机或发布数据批准。

## 结论边界

批准该 artifact 作为 `stroke-local-foundation`，允许后续 UI/CoreS3 集成按上述 API 契约推进。此次批准不覆盖 UI、触摸路由、`type=stroke` 协议、约 500 字库、商业许可结论或真机运行。
