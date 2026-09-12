# Tester: project knowledge

做一个新的功能，叫“笔划”。用户说一个汉字的单字，找几个备选，用户点击屏幕选择。用动画演示正确的笔画顺序。

<!-- pi-squad:5c2e64a7d326b8569a3ce3b522730a233728d37e8e82213e365ef7bc1989cca7 -->
## 2026-09-12T04:21:17.656Z — repo-validation

# repo-understanding-validation：构建、板卡变体与“笔划”验证体系

## 1. 结论摘要

- 仓库根目录没有 `config.json`；板卡发布配置分布在 `main/boards/**/config.json`。它们与 `scripts/build.py`、`main/Kconfig.projbuild`、`main/CMakeLists.txt` 共同决定板卡、芯片目标、固件身份和变体。
- 当前 GitHub Actions 的主矩阵使用 ESP-IDF **v6.0.2**。push 到 `main` 时动态编译全部适用于 6.0.2 的变体；PR 根据改动路径选择。`main/display/**`、`main/application.*`、其他非板卡 `main/**` 或 `main/boards/common/**` 的改动会选择全部 6.0.2 变体，因此“笔划”若落在公共 UI/应用层，CI 会自然触发全矩阵。
- ESP32-S31 不进入 6.0.2 动态矩阵；工作流另有 IDF 6.1 固定矩阵，当前明确编译 `esp32-s31-function-coreboard-1` 与 `esp32-s31-korvo-1` 两个 preview 变体。
- `scripts/tests` 只有两个 Python 文件，静态盘点共有 **66 个 `unittest` 测试方法**，重点验证构建脚本、变体元数据、Kconfig/CMake 一致性和默认资产元数据；没有设备 C++ 逻辑、LVGL 渲染、触摸、动画、协议或硬件功能自动测试。
- `.github/workflows/build.yml` 只做 host unittest、编译/链接、`merge-bin` 和上传 `merged-binary.bin`；没有烧录、启动、UI 截图、触摸、音频、内存/帧率、固件大小阈值或硬件回归。
- `docs/esp-idf-6-migration.md` 是历史快照而非当前矩阵真值：它记录 138 个目录、171 个变体、IDF 6.1 仅增加一个 S31 变体，但当前源码和 workflow 已有两个 S31 变体，且还存在文档表中未反映的配置。因此任何精确数量应在对应 IDF 环境中以 `python scripts/build.py --list-boards --json` 重新生成。

## 2. Board variant 构建方式

### 配置模型

每个 `main/boards/<board>/config.json` 的核心字段为：

- `type`：兼容性敏感的上报板卡族标识；发布后应保持稳定。
- `target`：ESP-IDF 芯片目标，目前源码覆盖 `esp32`、`esp32c3`、`esp32c5`、`esp32c6`、`esp32s3`、`esp32p4`、`esp32s31`。
- `builds[].name`：兼容性敏感的固件变体名/OTA 身份。
- `builds[].sdkconfig_append`：该变体的 Kconfig 覆盖，例如屏幕型号、闪存/分区、摄像头、AEC、P4 silicon revision。
- 可选字段：`manufacturer`、`idf_version`、顶层 `preview`、`build_options`、`legacy_names`。其中本次阅读到的 `build.py` 使用前四类；`legacy_names` 存在于配置中，但不参与所读到的构建选择或产物命名流程。

`type` 和 `name` 只能由小写字母、数字、点和连字符组成。构建脚本会检查：上报 type/name/产物名唯一性、manufacturer 与目录一致性、target 存在、Kconfig symbol 可解析、CMake 中能找到该板卡目录。多变体示例包括：

- `bread-compact-esp32`：`bread-compact-esp32`（128x64）与 `bread-compact-esp32-128x32`。
- `espressif/esp32-p4-function-ev-board`：Rev<3 的 `esp32-p4-function-ev-board` 与 Rev>=3 的 `esp32-p4x-function-ev-board`。
- `m5stack/corep4`：IDF `<6.0` 选择 `m5stack-corep4`，IDF `>=6.0` 选择 `m5stack-corep4x`。
- 两个 S31 配置均要求 `idf_version >=6.1` 且启用 `preview`。

### 规范命令

```bash
source /path/to/esp-idf-v6.0.2/export.sh
idf.py --version
python3 scripts/build.py --list-boards
python3 scripts/build.py --list-boards --json
python3 scripts/build.py <board-directory> --name <build.name>
```

多变体在交互终端可选择；非交互环境必须传 `--name`。附加用户选项包括：

```bash
python3 scripts/build.py <board> --name <variant> \
  --language zh-CN \
  --wake-word nihaoxiaozhi \
  --build-options-json '{"display_style":"default"}'
```

还可用 `--list-languages`、`--list-wake-words` 查询有效值；后者要求 ESP-SR managed component 已解析。`--zip` 才会生成 `releases/v<version>_<name>.zip`，默认只生成 `build/merged-binary.bin`。

### 实际构建流程

1. 检测当前 IDF 版本并按 `idf_version` 过滤变体。
2. 解析板卡 Kconfig symbol；将语言、唤醒词和语义构建选项合并到 `sdkconfig_append`，后写入者覆盖前值。
3. 仅在已有 CMake cache 的 target 不同时执行 `idf.py fullclean`。
4. 将旧 `sdkconfig` 移为 `sdkconfig.old`，生成 `build/xiaozhi-build.sdkconfig.defaults`。
5. 一次 `idf.py ... reconfigure` 同时传入 target、defaults 和 `BOARD_NAME`，随后校验 Kconfig 是否接受用户选项。
6. 执行 `idf.py build`、`idf.py merge-bin`，可选 ZIP。

注意：脚本会修改本地 `sdkconfig`、`sdkconfig.old`、`build/` 和可能的 `.vscode/settings.json`；不可把已有 build 目录当作另一 target 的可靠状态。

## 3. 自动测试与 CI 现状

### `scripts/tests`

`test_build.py` 的 64 个方法覆盖：

- IDF 版本表达式、P4/P4X 命名、manufacturer 前缀和变体身份唯一性。
- `config.json` type/name 合法性、语言/具体唤醒词不得固化到板卡配置、闪存/分区默认值不应重复。
- 改动路径到受影响板卡的选择；公共代码选全矩阵、文档改动选空矩阵。
- BOARD_TYPE 菜单排序、Kconfig 与 CMake symbol 对齐、各 target 默认板卡。
- target 切换、reconfigure 参数、旧 sdkconfig 备份、preview、stage marker、merge/ZIP。
- 语言与唤醒词发现/校验，显示型号/样式、多行消息、AEC、BluFi、摄像头镜像等语义选项。
- 非交互多变体选择、CLI 参数转发、板卡相对 include 存在性。

`test_build_default_assets.py` 的 2 个方法只验证默认字体资产 index 中 charset/size/bpp/bundle 元数据及缺失 bundle 的错误。

这些测试大量 mock `idf.py` 调用；即使全部通过，也不代表任何固件完成编译，更不代表 UI 或硬件正确。

仓库另有 `docker/firmware-builder/test_firmware_builder.py`（构建产物/manifest、失败日志、OSS 重试与输入校验），但唯一 workflow 没有执行它。

### GitHub Actions 矩阵

- `prepare`：IDF 6.0.2 容器中运行 `python -m unittest discover -s scripts/tests -v`。
- push：`build.py --list-boards --json`，构建所有适用于 6.0.2 的变体。
- PR：比较 merge commit 两个 parent，传给 `build.py --select-changed`。
  - 公共 `main/**`、`main/boards/common/**`、`components/**`、`partitions/**`、`sdkconfig.defaults*` 和若干全局构建文件 => 全矩阵。
  - 某个板卡目录 => 仅该板卡所有变体。
  - docs-only => 动态固件矩阵为空，但 host tests 仍运行。
- 每个变体独立 job，`fail-fast: false`，上传 `build/merged-binary.bin`。
- 两个 S31 变体在独立 IDF 6.1 job 中始终构建，不依赖动态选择结果。
- 没有 IDF 5.5 CI lane。迁移文档中的 5.5.4 结果是历史本地验证，不能视为持续回归保障。

## 4. 当前/历史代表性矩阵与“笔划”推荐矩阵

迁移文档可作为历史证据：IDF 6.0.1 曾有 157 个变体全绿；文档还列出 ESP32/C3/C5/C6/S3/P4 的代表构建及剩余分区空间，其中低余量例子约为 11%–17%。但该记录明确不等于当前全矩阵或硬件验证。

“笔划”建议先跑以下快速代表矩阵，再由 CI 跑公共改动触发的完整矩阵：

| 目标/变体 | 覆盖目的 |
|---|---|
| ESP32 `m5stack/atommatrix-echo-base` | 明确无显示板；验证功能可裁剪/降级，不能引入无条件 LVGL/touch 依赖 |
| ESP32 `bread-compact-esp32` 与 `bread-compact-esp32-128x32` | 低资源、OLED 128x64/128x32、无触摸布局与禁用策略 |
| ESP32-C5 `waveshare/esp32-c5-touch-lcd-1.69` | RISC-V、触摸 LCD、PSRAM 路径 |
| ESP32-C6 `waveshare/esp32-c6-touch-amoled-2.16` | RISC-V、触摸 AMOLED、现有默认/多行 UI 配置 |
| ESP32-S3 `bread-compact-wifi-lcd` | 通用非触摸彩屏，验证可见但不可点击设备的降级 |
| ESP32-S3 `waveshare/esp32-s3-touch-lcd-4.3c` | 主功能触摸彩屏路径、较大分辨率与旋转/校准 |
| ESP32-S3 `espressif/esp-vocat` | `CONFIG_USE_EMOTE_MESSAGE_STYLE` 与 expression assets，防止显示样式条件编译回归 |
| ESP32-S3 `waveshare/esp32-s3-epaper-1.54-v1/v2` | 4MB/8MB、电子纸；验证明确禁用或低刷新降级，避免逐帧动画造成刷新风暴/残影 |
| ESP32-P4 `espressif-esp32-p4-function-ev-board` 与 `...p4x...` | 大屏触摸、Rev<3/Rev>=3 两条 silicon 配置、资源与 hosted Wi‑Fi |
| ESP32-S31 `espressif-esp32-s31-korvo-1`（IDF 6.1） | preview target、触摸与摄像头 build defaults |

若“笔划”改变公共 `Protocol` 消息语义，还应额外选择至少一个 Wi‑Fi/WebSocket 路径和一个 MQTT/UDP/4G 路径构建并做端到端协议测试；仅编译 UI 板卡不够。

## 5. “笔划”功能所需测试

### 可自动化的纯逻辑/数据测试

1. **单字解析**：trim 后恰好一个允许的汉字 code point；拒绝空串、多字、标点、ASCII、无效 UTF‑8。明确繁体、CJK 扩展区和 variation selector 的产品策略，代码按 code point/grapheme 而不是字节计数。
2. **候选生成**：0/1/上限个候选；同音多候选、去重、稳定排序、正确字包含规则；超上限截断；无数据时可恢复提示。
3. **笔画数据**：以已确定版本和许可的数据源建立 golden corpus，覆盖一画字、点/提/折/钩、多部件、简繁和高笔画数。校验 stroke 数、顺序、路径非空、坐标有限且在画布范围、格式版本/checksum。
4. **恶意/损坏输入**：负数或超大 stroke/point 数、NaN/溢出坐标、截断/重复/未知字段、超大 payload；必须有显式 size 上限且不能越界或无限分配。
5. **动画状态机**：初始空白、按序逐笔、最后完成；暂停/重播/退出（若提供）；新语音请求、第二次点击、会话中断、睡眠、断网时旧 timer/callback 被取消，不再访问已销毁 LVGL 对象。
6. **触摸映射**：候选中心、边界、候选间空隙、屏外坐标、旋转/镜像、校准偏移、连点/长按/抖动、多点；一次手势最多提交一次选择。
7. **并发与调度**：语音/网络 callback 不直接修改应用/UI；应用状态变化经 `Application::Schedule()`/合法状态机路径；LVGL 操作持有 display lock；不阻塞主循环和音频任务。
8. **资产测试**：若数据打包到固件，验证生成器、index、缺字、损坏包、4/8/16/32MB 分区和增量大小；若数据来自服务端，验证超时、离线、重试、过期响应以及 WebSocket 与 MQTT/UDP 一致性。

### UI/设备测试

- 用 LVGL simulator 或截图 golden（仓库当前没有此设施）验证候选不重叠、不裁剪、选中态明确，以及每个关键帧只增加预期的一笔。
- 真机至少覆盖 S3 触摸 LCD、C6/C5 触摸小屏、P4 大屏；检查横竖屏、镜像、触摸边缘、动画流畅度、背光/睡眠恢复。
- OLED、无显示、非触摸 LCD 和电子纸必须有明确策略：首版建议不暴露可点击“笔划”，而不是显示一个无法完成的界面。电子纸若要支持，应改为逐步/静态页，不采用连续高帧率动画。
- 连续执行至少 100 次“识别→候选→选择→播放→退出”，观察 free heap、largest free block、timer/task 数，不得出现单调泄漏、看门狗、音频 underrun 或触摸失效。

## 6. 边界条件与回归风险

- **显示抽象风险**：`main/display` 没有统一 touch API；触摸由各板卡直接通过 LVGL port 注册。公共功能不能假设所有 `LvglDisplay` 都可触摸，应通过真实 input capability 或 feature gate 判定。
- **多显示实现**：存在 NoDisplay、OLED、普通 LVGL LCD、自定义 LVGL display、电子纸和 emote style。多个板卡重写 `SetupUI()`；不能假设标准容器总存在，也不能在 display 构造阶段过早创建 UI。
- **资源风险**：动态路径、候选字符串、汉字字体/笔画资产会增加 flash、assets partition、heap 和 PSRAM 压力；历史代表板已有约 11% 的 app 分区余量。无 PSRAM/4MB/8MB 变体最容易暴露问题。
- **生命周期风险**：动画 timer 与异步网络/ASR 响应可能晚于页面销毁；重复进入功能可能泄漏 LVGL object、timer、字体或数据缓存。
- **音频/主循环风险**：路径解析或逐帧重建对象若在主任务/音频任务执行，会造成卡顿、看门狗或播放/采集丢帧。
- **兼容风险**：修改 `Protocol` 契约只验证一个 transport、修改 device state 绕过状态机、网络 cJSON ownership 错误，都会影响现有聊天/重连/OTA。
- **样式/语言风险**：default、WeChat、emote、多行模式条件不同；中文动态 glyph 或字体 fallback 失败会出现候选方框。语言选择虽可配置，但“笔划”应明确仅对中文输入启用，其他 locale 不应破坏构建。
- **CI 盲区**：全矩阵绿只说明编译/链接和 merge 成功；触摸坐标、动画顺序、帧率、硬件启动和音频并未验证。
- **文档漂移**：迁移文档的矩阵计数已与 workflow/config 不一致，不能把其中 170/171 当作当前验收数量。

## 7. 推荐验收标准

1. **范围明确**：首发支持的显示/触摸 capability、汉字 Unicode 范围、候选上限、数据源和离线行为写入规格；不支持设备不展示不可操作入口，并保持原语音助手功能正常。
2. **正确性**：选定的 golden corpus 中候选与权威数据一致；用户选择后 stroke count、顺序和最终字形逐项一致；0/1/多候选及无数据均有确定结果，无崩溃。
3. **交互**：数据已就绪后候选界面在 500 ms 内出现，点击后 100 ms 内有可见反馈；重复/边界点击不误选、不重复启动；新请求/退出能可靠取消旧动画。
4. **稳定性**：100 次循环无崩溃、WDT、音频 underrun 或单调 heap 下降；动画期间主 UI、音频和网络仍可响应。具体 heap/帧率基线应在选定真机上记录并纳入后续回归。
5. **构建**：`python3 -m unittest discover -s scripts/tests -v` 通过；上述快速矩阵通过；公共 `main/**` 改动对应的 IDF 6.0.2 完整动态矩阵和两个 IDF 6.1 S31 job 全绿；没有未评审的板卡身份、分区或 sdkconfig 改动。
6. **容量**：记录每个代表变体的 app/asset 增量；任何分区不得溢出。建议若剩余空间降至 10% 以下或相对基线出现显著增长，必须专项评审或裁剪数据，而不是仅以“编译成功”放行。
7. **硬件**：S3、C5/C6、P4 至少各一块适用触摸设备完成启动、联网、语音输入、候选点击、完整动画、打断、睡眠恢复和重连；无显示/OLED/电子纸设备完成降级回归。
8. **协议（若涉及）**：WebSocket 与 MQTT/UDP 均完成正常、超时、断线、恶意 payload 和过期响应测试，输入有大小/数量上限且 cJSON ownership 正确。

## 8. 本次验证说明

本任务按只读模式完成。实际执行的是文件读取、文件枚举与文本检索；**未运行 shell 命令、Python unittest、`idf.py` 或任何固件构建，也未修改文件**。因此上面的测试数量和行为是源码静态盘点结果，不是本次运行结果。后续执行者应实际运行：

```bash
python3 -m unittest discover -s scripts/tests -v
source /path/to/esp-idf-v6.0.2/export.sh
idf.py --version
python3 scripts/build.py --list-boards --json
python3 scripts/build.py <board> --name <variant>
```

S31 代表构建需切换到 IDF 6.1。

- 仓库根目录不存在 `config.json`；板卡发布配置位于 `main/boards/**/config.json`，并通过 type/target/builds.name/sdkconfig_append 描述兼容身份和变体。
- `scripts/build.py` 会按活动 ESP-IDF 版本过滤变体，非交互多变体构建要求 `--name`，执行 reconfigure、build、merge-bin，默认不打 ZIP。
- `.github/workflows/build.yml` 的动态主矩阵使用 ESP-IDF v6.0.2；公共 `main/**` 或 `main/boards/common/**` 改动会选择全部适用变体，板卡私有改动只选择该板卡，docs-only 改动不选择动态固件。
- 当前 workflow 另用 ESP-IDF 6.1 固定构建两个 S31 preview 变体：`esp32-s31-function-coreboard-1` 和 `esp32-s31-korvo-1`。
- `scripts/tests` 静态盘点有 66 个 unittest 方法，覆盖构建工具和资产元数据，但没有笔划业务、LVGL 渲染、触摸、动画、协议或硬件功能测试。
- 唯一 GitHub workflow 不运行 `docker/firmware-builder/test_firmware_builder.py`，也没有 IDF 5.5、烧录启动、性能或硬件测试 lane。
- `docs/esp-idf-6-migration.md` 的 170/171 变体数字是历史快照，已与当前两个 S31 配置及 workflow 不一致；当前精确矩阵必须由对应 IDF 环境中的 `build.py --list-boards --json` 生成。
- 触摸注册存在于各板卡实现中，`main/display` 的公共 Display/LvglDisplay 接口未提供统一 touch capability；笔划不能仅凭“有显示”假设可点击。

<!-- pi-squad:1a4a58ec6703cdfa1142a466ef998c827ba7db6e599ae7b81fc9812906919f64 -->
## 2026-09-12T05:29:19.761Z — stroke-local-foundation-review

# stroke-local-foundation 独立审查

**Verdict: REJECTED**

当前 Python 原型、SOB1 字段定义、CoreS3 条件接入和文档边界具备良好基础，但设备侧核心解析 API 存在原始字节别名/对齐/端序问题，并且该 C++ 文件从未成功编译或执行。现有 16 项专项测试及 83 项全量 host 测试只能证明 Python 路径和既有 host 测试通过，不能支持把当前 `StrokeOrderStore` API 交给 UI 层依赖。

本审查按要求只读取真实工作区文件和保留的作者命令输出；**未修改文件，也未自行运行 shell、测试或构建**。

## Blocker

### B1. `StrokeView` 直接把序列化字节当作 C++ `Point` 对象，违反安全解码边界

- 文件：`main/stroke_order/stroke_order_store.h` 的 `Point`/`StrokeView`；`main/stroke_order/stroke_order_store.cc` 的 `ParseCharacter()`。
- 具体依据：记录头和计数都通过 `ReadU16`/`ReadU32` 显式按 little-endian 解码，但点数组却使用：
  - `reinterpret_cast<const Point*>(record + after_header)`
  - `reinterpret_cast<const Point*>(record + outline_end)`
  随后直接访问 `outline[p].x/y`，并把这些指针返回 UI 调用者。
- 风险：
  1. `Bind()` 接受任意 `const uint8_t*`，没有保证基址满足 `Point` 对齐；
  2. 文件字节中没有正式构造出的 `Point` 对象，直接按 `Point*` 解引用有对象生命周期/严格别名风险；
  3. `Point` 的布局和 `sizeof(Point)==4` 没有静态保证；
  4. 文件声明为 little-endian，但点坐标按宿主原生端序读取，与头/index 的显式端序处理不一致。
- 这不是单纯测试缺口，而是 UI 即将依赖的公开 API 设计问题。应改为保存字节视图并通过 `ReadU16` 按索引读取点，或使用安全复制解码；不得向调用方暴露指向编码字节的 `Point*`。

### B2. 设备 C++ parser 没有任何成功编译或运行证据

- 文件：`main/stroke_order/stroke_order_store.cc`、`scripts/tests/test_stroke_order.py`、`main/CMakeLists.txt`。
- 作者证据显示：
  - `clang-format` 以 code 127 失败；
  - 两次 `clang++ -fsyntax-only` 均因找不到 `<cstddef>` 失败；
  - 未运行 `idf.py` 或 CoreS3 构建；
  - `CONFIG_STROKE_ORDER_LOCAL` 默认关闭，因此 83 项 Python host suite 不会编译该 C++ 源文件。
- `scripts/tests/test_stroke_order.py::test_cpp_constants_match_python` 只用正则读取头文件中的数值常量，不编译或调用 C++。
- 因而目前没有证据证明 C++ 代码可通过 ESP-IDF 编译，更没有证据证明真实 C++ `Bind`/`LoadCharacter`、CRC、恶意 offset、失败保状态和未对齐输入行为正确。缺失核心实现证据不能视为通过审查。

## Major

### M1. `source_commit` 是未验证的自由文本，manifest 不能证明来源

- 文件：`scripts/stroke_order/convert.py` 的 `_require_commit()`、`convert_dataset()`、`build_manifest()`。
- `_require_commit()` 只接受 7–40 位十六进制；它不确认数据目录是 git checkout，不执行离线 `git rev-parse HEAD`，不检查 dirty tree，也不记录所选 JSON 的内容哈希。测试甚至使用虚构的 40 个 `a`。
- `build_manifest()` 随后原样把该值写成来源 commit。因此任意目录都可被标成任意 commit，且 7 位短 SHA 也不是长期唯一 pin。
- 在“固定 commit + provenance”要求下这是实质缺口。至少应要求完整 commit，并将 checkout HEAD/dirty 状态或选中字形文件的确定性内容哈希绑定到 manifest。

### M2. Python 生成器和 Python validator 高度锁步，可能共同接受同一格式错误

- 文件：`scripts/stroke_order/convert.py`、`scripts/stroke_order/unpack.py`、`scripts/stroke_order/constants.py`、`scripts/tests/test_stroke_order.py`。
- generator 在输出后调用同包的 `validate_blob()`；pack/unpack 共享同一 constants 模块、相同 struct 格式和相同字段解释。专项测试主要是“生成器输出 → Python mirror 解析器”。
- 没有手工定义且独立核算的 SOB1 golden bytes，没有标准 CRC known-answer vector，也没有 Python 输出交给 C++ parser 的跨实现测试。常量正则测试只覆盖部分数值，不覆盖字段顺序、CRC 范围、点端序和对象读取方式。
- 因此 16/16 不能排除生成器和 validator 以同样方式写错字段、端序或 CRC 范围的锁步假阳性。

### M3. index 元数据不在任何 CRC 保护范围内，`Bind()` 可接受被破坏但结构仍合法的候选索引

- 文件：`main/stroke_order/stroke_order_store.cc::Bind()`、`scripts/stroke_order/unpack.py::_parse_header_and_index()`、`docs/stroke-order-data.md`。
- header CRC 只覆盖 bytes `0..23`；record CRC 只覆盖 character record。整个 index 的 `codepoint/offset/length/crc` 没有 checksum。
- `Bind()` 也不比较 index codepoint 与 record header codepoint，并明确忽略 `rec_crc`。例如单独把一个 index codepoint 改成另一个仍合法且保持排序的值，`Bind()`、`GetCodepointAt()` 和 `Contains()` 可先成功，直到 `LoadCharacter()` 才因 record codepoint 不符而失败。
- 这不形成越界读取，因为 offset/length 连续性和范围检查较完整，但会让候选索引在加载前呈现损坏数据。若 `Bind()` 被定义为“验证 header+index”，应增加 index/global metadata CRC，或明确收紧契约并保证候选枚举不会信任未校验元数据。

### M4. charset 未限制为汉字，且原始字符直接进入文件路径

- 文件：`scripts/stroke_order/convert.py::load_charset()`、`_character_json_path()`。
- `load_charset()` 只要求一枚非 surrogate Unicode code point；ASCII、emoji、路径分隔符都能通过，不符合首期“《通用规范汉字表》简体汉字”的产品边界。
- `_character_json_path()` 直接执行 `data_dir / f"{char}.json"`。字符 `/` 在 POSIX 上会形成绝对路径 `/.json`；Windows 路径分隔符/UNC 也有类似边界风险。虽然这是本地工具，但它违反了“只从显式数据目录读取所选字形”的边界。
- 应验证受支持汉字清单，并对解析后的路径做 `resolve()` 后的目录归属检查；至少显式拒绝路径分隔符和非目标字符。

### M5. 没有真实上游样本，无法证明命令子集、坐标变换和限制适配固定版本数据

- 文件：`scripts/tests/fixtures/stroke_order/README.md`、`scripts/tests/test_stroke_order.py`、`scripts/stroke_order/path.py`。
- fixture README 明确说明所有输入都是自造矩形/简单曲线；没有来自固定 commit 的一个真实、经许可保留的样本。
- 当前代码对 `M/L/Q/C/Z`、递归深度 8、容差 0.25、`1024-y`、每笔 256/64 点限制在合成输入上自洽，但测试没有证明真实 `hanzi-writer-data` 的 path 语法、坐标基线、复杂曲线和平坦化后点数能通过，也没有视觉 golden 证明轮廓与 median 对齐。
- 这不要求现在导入 500 字，但 UI 阶段前至少需要一个小规模、来源和许可清楚的真实 smoke corpus，否则 UI 可能建立在只对矩形有效的数据假设上。

## Minor

### m1. “拒绝网络路径”的实现范围比报告表述窄

- 文件：`scripts/stroke_order/convert.py::_require_local_dir()`。
- 它只拒绝匹配 `scheme://` 的字符串；UNC 路径、网络挂载目录等只要 `Path.is_dir()` 为真就会接受。当前实现确实不会主动联网，但“拒绝所有 network paths”的作者表述过强，应收窄文档或补充平台路径检查。

### m2. 非有限数字不能得到受控的转换错误

- 文件：`scripts/stroke_order/path.py::convert_median_points()`、`_to_device_points()`；`scripts/stroke_order/convert.py::main()`。
- Python `json.loads` 默认可接受 `NaN`/`Infinity`；代码没有 `math.isfinite()`，随后 `round(nan/inf)` 会抛出 `ValueError`/`OverflowError`，而 CLI 只捕获 `ConvertError`/`PathError`。结果会 fail closed，但以 traceback 而非明确验证错误退出。

### m3. 对外导出的 `pack_characters()` 本身可产生不合法 blob

- 文件：`scripts/stroke_order/__init__.py`、`scripts/stroke_order/convert.py::pack_characters()`。
- 该函数被公开导出，但自身不拒绝重复 codepoint 或超过 1024 个字符；只有 `convert_dataset()` 的前置检查和后置 `validate_blob()` 才拦截。直接调用公开函数可拿到不满足 SOB1 parser 约束的 bytes。应在 pack 层也强制格式不变量，或不把它作为公共 API。

### m4. 格式检查未完成

- 文件：`main/stroke_order/stroke_order_store.h/.cc`。
- `clang-format` 未安装，实际格式检查没有运行。作者后来只对 `.cc` 做了行长扫描；这不等价于仓库 `.clang-format` 的完整校验。该项本身是 minor，但必须与成功的 CoreS3/C++ 编译一起在复审前补齐。

## 已确认的正面结果

1. **SOB1 基本字段一致**：Python struct、C++ 常量和文档均为 32-byte header、16-byte index、8-byte record header；header/index/record 数值字段按小端定义。header CRC 的实现范围在三处一致为前 24 bytes；record CRC 覆盖完整 record。
2. **整数范围检查总体扎实**：C++ 使用 `AddU32`/`MulU32`、1 MiB 文件上限、16 KiB record 上限、连续 offset 和 `data_end==size`；Python mirror 也有 u32 检查。除 typed point aliasing 外，未发现直接 offset/length 越界路径。
3. **失败保状态的 C++ 控制流静态上成立**：`Bind()` 只在全部 header/index 验证完成后提交成员；`LoadCharacter()` 先解析到局部 `local_strokes/local_count`，成功后才替换已加载字符。Python mirror 的对应测试通过；但仍需真实 C++ 测试。
4. **Python 路径限制基本清楚**：只实现 `M/L/Q/C/Z` 及相对形式，要求 `Z` 闭合，曲线使用确定性 de Casteljau，输出去除连续重复点并做 0..1024 检查。合成测试覆盖未知命令、未闭合、Q/C 和 y 翻转。
5. **确定性设计合理**：字符按 codepoint 排序，manifest `sort_keys=True` 且无时间戳，合成输入的二进制和 manifest 跨临时目录一致。
6. **CoreS3 gating 正确且保留原改动**：`main/Kconfig.projbuild` 中配置默认 `n` 且 `depends on BOARD_TYPE_M5STACK_CORE_S3`；CMake 仅在 `CONFIG_STROKE_ORDER_LOCAL` 时追加 parser。作者前后 diff 证据显示原有 stick-s3 emoji 两处 CMake hunk仍保留，`scripts/build_default_assets.py` 未被本任务改写。
7. **许可证/fixture 边界目前清楚**：仓库没有导入真实 `hanzi-writer-data` 或 `all.json`；fixture README 明确是合成数据且不是发布字库；文档说明 Arphic 许可、修改说明和商业发布前授权/笔顺审核要求。因尚未分发上游图形，当前缺少 APL 许可证全文不是现阶段侵权证据，但任何真实数据进入资产前必须补齐许可证和 notices。

## 作者测试证据核验

- `python3 -m unittest scripts.tests.test_stroke_order -v`：保留日志显示最终 **Ran 16 tests / OK**。
- `python3 -m unittest discover -s scripts/tests -q`：保留日志显示最终 **Ran 83 tests / OK**。
- 这 83 项包含上述 16 项；不是额外 83 项 stroke 测试。
- `clang-format`：**未运行成功**，`command not found`。
- 两次 host `clang++`：**均失败**，`fatal error: 'cstddef' file not found`。
- ESP-IDF/CoreS3 build、C++ parser harness、真实数据转换、设备运行：**均未执行**。

结论：16/83 证据足以说明 Python 合成路径没有打破既有 host suite，但不足以支持进入依赖当前 parser API 的 UI 集成阶段。

## 复审前最低门槛

1. 去掉序列化字节到 `Point*` 的直接别名，使用对齐与端序安全的点访问 API，并测试非对齐 blob。
2. 建立真正独立的 C++ host harness：读取 Python 生成 blob和手工 golden，覆盖 Bind/Load 成功、CRC、大小/offset/count 溢出、失败保状态、未对齐输入及全部边界值。
3. 在 ESP-IDF v6.0.2 下以 `CONFIG_STROKE_ORDER_LOCAL=y` 成功编译 `m5stack-core-s3`；运行仓库 clang-format 检查。
4. 增加独立 SOB1 golden/known-answer CRC，避免 Python pack/unpack 锁步。
5. 将 provenance 绑定到完整 commit 和实际输入内容，限制 charset/path 边界。
6. 在带许可证/notices 的小规模真实上游 corpus 上做转换、C++ 加载和轮廓/median 视觉 smoke；500 字完整发布集仍须另行审核。

- SOB1 的 Python/C++/文档字段大小目前一致：32-byte header、16-byte index、8-byte character header；header CRC 覆盖 bytes 0..23，record CRC 覆盖完整 record。
- C++ Bind 使用 checked u32 加乘、连续 record offset、data_end==file size 和大小上限；LoadCharacter 先写局部结果后提交，因此失败保状态的控制流静态成立。
- StrokeView 当前把编码字节 reinterpret_cast 为 const Point*；API 未保证对齐、对象生命周期、Point 布局或 little-endian 点解码。
- 最终作者日志确有 16 项 stroke Python 测试和包含它们的 83 项全量 host 测试通过；C++ parser 没有被这些测试编译或执行。
- 作者的 clang-format 命令因工具不存在失败，两次 clang++ 语法编译均因缺少 cstddef 失败；未执行 IDF/CoreS3 构建。
- source_commit 目前只做 7–40 位 hex 正则校验，不核对 checkout HEAD、dirty 状态或输入文件哈希。
- Python generator 与 validator 共用 constants/struct 解释；测试没有独立 SOB1 golden、标准 CRC 向量或 Python→C++ 交叉执行。
- Kconfig 默认关闭且依赖 BOARD_TYPE_M5STACK_CORE_S3；CMake 只在 CONFIG_STROKE_ORDER_LOCAL 时加入 parser，保留日志证明既有 stick-s3 emoji CMake hunks仍存在。
- 当前仓库没有真实上游字形数据；测试 fixture 全为自造矩形/曲线，文档明确其不是发布字库。
- 许可证文档清楚区分 Arphic 图形、字典许可风险和合成 fixture；真实字形进入资产前仍缺实际许可证全文/notices、固定来源验证和准确性审核。

<!-- pi-squad:ad9a2117562e55497552a4998a44afff13b576f980f5c61c003ae6c5b45545fc -->
## 2026-09-12T06:13:41.773Z — stroke-local-foundation-rereview-retry

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

- `StrokeOrderStore::StrokeView` 当前只持有私有 byte views，并通过 `GetOutlinePoint`/`GetMedianPoint` 显式按 little-endian 解码到值；生产代码没有序列化字节到 `Point*` 的 typed alias。
- SOB1 v1 的 32-byte header 在 offset 28 存放完整 index byte range 的 CRC-32/ISO-HDLC；Python packer/validator、C++ Bind、文档和手工 golden 对该范围一致。
- 独立 84-byte golden 固定 header/index/record CRC 为 `0x7ce14721`/`0x6e6cb232`/`0x3bdfc874`，Python 与 C++ 均验证标准 `123456789 -> 0xCBF43926` CRC known answer。
- Host harness 直接链接 `main/stroke_order/stroke_order_store.cc`，以 UBSan 执行 Python 生成物、手工 golden、真实三字 corpus、未对齐输入、损坏/溢出输入及 Bind/Load 失败保状态；保留输出为 PASS。
- Converter 要求完整 40-hex commit、clean git checkout、匹配 origin，并逐文件比较工作区 SHA-256 与 `git show HEAD:path` bytes；manifest 记录逐文件、aggregate 与 binary SHA-256。
- 真实 smoke corpus 固定到 `chanind/hanzi-writer-data` commit `68d10a4b21150cae5e1ebbd223eed289cf32d90c`，包含 verbatim 一/人/口 JSON、完整 APL 和 NOTICE；三字转换输出为 2668 bytes。
- 保留最终结果为专项 24/24、全 host suite 91/91、Xtensa syntax compile、clang-format 19.1.7 dry-run 和 ESP-IDF v6.0.2 CoreS3 隔离全量构建全部成功。
- 隔离 build 的 sdkconfig 为 esp32s3/CoreS3/STROKE_ORDER_LOCAL=y，生产 parser object 出现在 build graph 和 `.ninja_log`；app binary 为 2,859,856 bytes，分区剩余 31%。
- 当前 artifact 未实现或验证 UI、触摸、协议、真实 CoreS3 烧录运行，也未提供约 500 字发布库；这些不属于本次基础层批准范围。
- Lead 的最终状态证据保留了既有 Stick-S3、emoji、assets、partition 与 `.squad` 未提交项，并保持根目录 sdkconfig/build 时间戳；本 Reviewer 未自行运行 shell 或测试。

<!-- pi-squad:db275b2aff17003699483d36d76e842040015c844862aff057a047871dc0e31c -->
## 2026-09-12T07:07:00.238Z — stroke-ui-cores3-review

# stroke-ui-cores3 独立审查

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

- `StrokeOrderView::Attach()` only rebinds stroke data when the controller is not ready, while an Assets download explicitly unmaps the prior LVGL asset partition; a previously ready controller can therefore retain a stale mmap pointer across an asset update.
- `generation_` is a plain uint32_t read by LVGL callbacks and Application main-task lambdas and written during overlay teardown; the check occurs before acquiring the display lock, so it is both a C++ data race and a TOCTOU guard.
- The production debounce clock only advances through `StrokeOrderController::Tick()`, and the LVGL timer calls Tick only while Animating; after a multi-stroke StepForward enters Paused, the debounce time does not advance.
- CoreS3 polls FT6336 every 20 ms and keeps I2C out of the LVGL read callback, but separate atomic x and y values do not provide one coherent coordinate snapshot during continuous movement.
- A clean retained IDF 6.0.2 CoreS3 build with `CONFIG_STROKE_ORDER_LOCAL=y` succeeded and produced a 2,879,248-byte app with 30% app-partition headroom plus generated assets containing the smoke bin, APL, and NOTICE.
- The retained host run completed 93/93 tests; its production C++ harness covers controller/store under UBSan, but there is no automated execution of StrokeOrderView, LVGL callback lifecycle, CoreS3 touch routing, or Assets mmap replacement.
- The renderer copies the selected glyph through `CopyLoadedGlyph()` and keeps no controller-owned StrokeView across the mutex; candidate StrokeViews are decoded immediately from a stack-local store into owned vectors.
- Candidate and control LVGL user_data point to fixed member arrays; separate dense display slots and source controller indexes preserve selection correctness when a candidate glyph is skipped.
- The fixed 320x240 geometry keeps the 200x200 grid and five 88x44 controls within bounds and makes all declared targets at least 44 pixels, with no per-frame vector allocation found.
- When power save tears down an open overlay, the entry remains hidden while its controller hit rectangle remains active; the bottom-left entry region can silently consume ordinary chat taps after wake.

<!-- pi-squad:b418bd22c0dd59e8fdd09f1222aba99cc050b12ee888d3a0be09276c417e31ba -->
## 2026-09-12T07:43:47.054Z — stroke-ui-cores3-rereview

# stroke-ui-cores3 修订复审

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

- `StrokeOrderController::BindStore()` 现在先清旧状态，拒绝空/零长度/超过 1 MiB 的 blob，以 nothrow 分配私有副本并在副本上验证；候选和动画渲染使用 owned decoded vectors。
- 生产资产替换顺序是 StrokeOrder suspend/unbind 后才由 strategy munmap；`Assets::Apply()` 每次尝试后都调用 `RebindAssets()`，而 Rebind 会先废弃旧状态再重新 GetAssetData/Bind。
- StrokeOrder 点击、timer 和 delete callback 在 LVGL task 内直接处理；外部生命周期路径使用 display lock，统一锁序为 display/LVGL lock 后 controller mutex，CoreS3 poll 不反向取 display lock。
- CoreS3 使用一个 release/acquire `atomic<uint32_t>` 发布 pressed/x/y，并在生产 poll 路径复用 `StrokeOrderTouchSequence`；Xtensa 构建成功，但该 atomic 并非编译期 always-lock-free。
- `StepForward` 接收显式 monotonic milliseconds并用独立有效位处理时间 0；Paused/Completed 会暂停 timer，Candidates/Error/退出/delete 会删除 timer。
- 生产 lifecycle gate 由 view 实际复用，限制 entry/overlay 到 assets、pointer、surface、Idle、awake、未中断条件均成立时。
- host 96/96 和 IDF 6.0.2 CoreS3 全量构建有保留日志与产物证据；本复审只读，没有重新执行命令。
- 最终固件目录含 2,880,480-byte `xiaozhi.bin`，app 分区剩余 1,248,288 bytes（30%），但没有任何真机启动、交互、性能或稳定性证据。

<!-- pi-squad:e2a0678bfacf102ce9598af8b6bd07da985509e5703ab54e400daafa75f9802b -->
## 2026-09-12T08:16:21.033Z — stroke-stt-local-candidates-review

# stroke-stt-local-candidates 独立审查

**Verdict: REJECTED**

本次只读检查了真实工作区、作者保留的运行日志和 `/tmp/xiaozhi-stroke-stt-local-candidates-build`；未修改文件，未自行运行 shell、测试或构建。快乐路径的 parser、候选过滤、UI 接入与构建产物有实证，但会话代际、取消后的监听清理和 follow-up 抑制存在核心竞态，当前不能作为可烧录可用固件放行。

## Blocker

### B1. STT 截获不是一个原子的“phase + token”快照，迟到旧 STT 可以被新 session 盖上新 token 并接受

**依据：**

- `main/stroke_order/stroke_order_view.cc:67-76` 的 `PeekInterceptStt()` 先单独读 `session_.intercepts_stt()`，随后再单独读 `session_.token()`。
- `main/stroke_order/stroke_order_session.h:45-58` 的 `BeginAwaitingSpeech()` 先写 phase 等字段，最后才 release-store 新 token；`:99-110` 的 `Cancel()` 同样分开写 phase/token。
- `main/application.cc:635-646` 把 `stroke_intercept` 和当时读到的 token 捕获进 main task；只要 `stroke_intercept` 为真，就无条件走 stroke handler 并 `return`。

因此至少有两类真实竞态：

1. 网络线程读到旧 session 的 `AwaitingSpeech` 后，LVGL/main 执行 `Cancel()` 并开始新 session，网络线程随后读到**新 token**；旧轮 STT 被盖成新 token，`AcceptStt()` 会接受，新 session 会“偷”旧 STT。
2. 网络线程读到 awaiting 后发生取消，拿到取消后的 token；main 复核会拒绝，但 Application 仍无条件 return，原本应走普通 chat 的 STT 也可能被吞掉。

即使把两个 atomic 的 memory order 加强，也不能解决“STT 到达时才套用当前 token”的根本问题；必须把请求代际绑定到开始监听/连接 continuation，并提供可证明的旧轮隔离策略。`scripts/tests/stroke_order_session_harness.cc` 只测试“手工保存 first token，Cancel 后再 AcceptStt(first)”；没有经过生产 `PeekInterceptStt()`，也没有 cancel→Begin→旧 STT 的交错测试，不能证明作者声称的“新 session 不偷旧 STT”。

### B2. 取消会清掉 `listen_request_`，但 Start/Connect 的复核仅在该标志仍为 true 时执行；取消后的连接仍可能进入 Listening，且 alert/asset/power/delete 只清 UI、不停麦

**依据：**

- `StrokeOrderSession::Cancel()`（`stroke_order_session.h:99-110`）把 `listen_request_` 清为 false。
- `Application::HandleStartListeningEvent()`（`application.cc:860-895`）只在 `HasStrokeListenRequest() && !ShouldContinueStrokeListen()` 时拒绝；取消后前半条件已经为 false，于是 stale `MAIN_EVENT_START_LISTENING` 被当成普通请求继续执行。
- `Application::ContinueOpenAudioChannel()`（`application.cc:828-857`）也只在 `HasStrokeListenRequest()` 为 true 时复核。若 `OpenAudioChannel()` 最多阻塞约 10 秒期间取消，返回时标志为 false，函数仍会 `SetListeningMode(mode)`。
- 该 guard 实际上几乎不可触达：request 为 true 时 phase 正是 AwaitingSpeech；Cancel/Accept/Timeout 又都会先把 request 清掉。
- `StrokeOrderView::OnInterruption()`、`OnPowerSave()`、`SuspendAssets()`、外部 `OverlayDeleted`/`EntryDeleted`（`stroke_order_view.cc:143-160, 204-236, 1241-1287`）只执行 session/controller/view 清理，没有安排 `StopActiveListeningRound()`。用户点 X 的路径会 schedule stop，但 alert 路径会从网络回调直接取消 UI 后让 Connecting/Listening 继续。

影响是高优先级 alert、资产/表面失效或连接中的取消可以留下持续监听/上传；作者报告中“hello 返回后若 listen-request 已取消则回 Idle”的说法与实际条件相反。这直接违反不得卡住 Listening/持续上传 mic 的要求。

### B3. TTS/LLM 抑制没有 round 身份；timeout/cancel 后旧回答可泄漏到 UI 和扬声器

**依据：**

- `StrokeOrderSession::drops_assistant_output()` 只是 `awaiting_speech || suppress_`（`stroke_order_session.h:37-40`），没有 token/session/turn 参数。
- `CheckTimeout()` 明确把 `suppress_` 清 false（`:84-96`），`Cancel()` 也清 false（`:99-110`）。
- TTS start、sentence_start 和 LLM lambda 都在**main 执行时**查询当前全局 bool（`application.cc:582-620, 656-665`），并没有捕获消息到达时的 stroke generation。
- 下行音频仅按 `DeviceState == Speaking` 放入解码队列（`application.cc:540-543`）。所以 timeout/cancel 清 suppression 后，迟到的旧 `tts start` 可进入 Speaking，后续音频会播放；已排队的旧 TTS/LLM lambda 也可在取消后污染普通 UI。

反方向同样无法证明“只抑制当前 round”：suppression 为真时会丢弃所有 assistant 输出。当前做法只是用取消立即清 bool 来避免吞下一轮，但代价正是放过旧轮迟到回答。这里需要真实 turn/代际隔离或明确的旧轮排空/终止协议，而不是无身份的全局布尔值。

## Major

### M1. 候选页没有屏幕退出控件，也没有已接受的 60 秒候选等待超时

`RenderCandidates()`（`stroke_order_view.cc:625-677`）只创建候选按钮，没有 X；`OnMainClockTick()`（`:313-338`）只在 `awaiting_speech()` 时工作。候选页和从动画返回后的候选页可无限期保持全屏 exclusive touch。Wake/toggle 能从外部取消，但仅触屏用户无法退出；这不满足已接受的候选页最长等待 60 秒，也使“无隐藏触摸死区/可退出”不完整。相比之下 AwaitingSpeech、NoMatch、TimedOut 的 `RenderStatusPage()` 确实有 X，NoMatch/TimedOut 也有 R（`:797-854, 1078-1107`）。

### M2. STT 文本没有在网络线程做有界验证后再 Schedule

`application.cc:625-638` 直接 `std::string(text->valuestring)`，还先解析 glyph payload，之后才 schedule；96-byte/32-codepoint/UTF-8 校验直到 main 的 `ApplyVoiceUtteranceLocked()` 才发生（`stroke_order_view.cc:860-890`、`stroke_order_parse.h:162-253`）。因此超长或无效文本会先在网络线程无界扫描/分配，违反“先复制并验证，再 Schedule”的输入边界要求。普通 STT 需要保留原行为，但 stroke pending 分支仍应使用有界探测/复制。

### M3. `StopActiveListeningRound()` 保留控制通道这一点正确，但没有清空已排队上行音频

`application.cc:772-785` 在 Listening 下调用 `SendStopListening()`、关闭 voice processing 并回 Idle，确实不关闭控制通道；但它不清 `audio_send_queue_`。`Application::Run()` 仍会在 `MAIN_EVENT_SEND_AUDIO` 中发送已有 packet。仓库已有 wake-word 路径通过循环 `PopPacketFromSendQueue()` 清残留（`application.cc:931-935`），说明这里存在同类残余上传窗口。Connecting 分支的 stale continuation 问题已列为 B2。

### M4. 98 项 host 通过并不包含 Application/Protocol 时序回归；所谓普通聊天测试只是源码字符串断言

- `scripts/tests/test_stroke_order_ui.py:207-224` 只断言源码包含 `PeekInterceptStt`、`HandleVoiceSttFromMain`、`AddTextGlyphs`、user bubble 等字符串。
- session harness 不链接 `Application`、Protocol、AudioService 或 LVGL，也不模拟网络/main/LVGL 三线程。
- 未覆盖：分裂 phase/token 读取、cancel→new session→旧 STT、stale start event、阻塞 OpenAudioChannel 后取消、alert 后停麦、timeout/cancel 后迟到 TTS、上行队列清理、普通聊天实际 glyph/chat 输出。

所以 inactive 时普通 STT 的源码分支结构确实保留，但不能据此宣称普通聊天、glyph_push、TTS/audio 已完成回归。

## Minor

1. `StrokeOrderController::SetCandidates()`（`stroke_order_controller.cc:88-97`）只限制输出最多 6 个，若调用者传入巨大 `count` 且前面全无效，会扫描到 `count`；生产 `SetCandidatesFromPrimary()` 当前最多传 6 个，故现路径有界，但公开 API 自身不是严格有界。
2. `StrokeOrderSession::CheckTimeout()` 用 `now_ms < started` 直接判超时（`stroke_order_session.h:84-95`）；32-bit 毫秒约 49.7 天回绕附近会提前超时。应用应使用无符号差值的回绕语义。
3. 作者报告称“专项 31 tests, OK”，保留日志实际显示先有一次 31 项失败，修复后单独重跑 1 项通过，再运行全量 98 项通过。最终代码的 31 项确实包含于全量绿灯中，但没有一条最终独立 31 项命令输出。

## 已验证正确的部分

- 入口点击从 LVGL callback 通过 `Application::Schedule()` 到 `StartVoiceSessionFromMain()`；该窄接口、STT main handler 和 clock tick 都在修改 lifecycle/controller/LVGL 时持 `DisplayLockGuard`，未发现这条快乐路径中的裸 LVGL 访问。
- `Connecting`/`Listening` 的 listen-hold 与其他 DeviceState 的 view 取消规则已实现；channel close 在仍 AwaitingSpeech 时会 schedule 取消并回 Idle；reboot 会 Shutdown、关 channel、停 AudioService。问题在于取消与音频 continuation 没有组成同一代际事务。
- `stroke_order_parse.h` 按 code point 解码，拒绝非法/过长 UTF-8、ASCII、emoji，支持单字、某字、某怎么写、某词的某；多内容汉字判 Ambiguous。`ApplyVoiceUtteranceLocked()` 再以 store Contains 过滤，不存在的“天”进入 NoMatch。
- `SetCandidatesFromPrimary()` 把识别字放第一位；controller 只保留 CJK basic、store 中可加载、去重的候选，输出最多 6。当前 no-op provider 不制造同音字，三字 smoke 对“一”只显示“一”。BindStore 不再自动展示全库。
- 非截获分支仍执行 `AddTextGlyphs(glyphs,bpp)` 和 `SetChatMessage("user",...)`；固件未加入/宣布 `type=stroke`。但 B1 可造成错误截获，因此不能称“完全保持”。

## 作者验证证据复核

- 保留日志确认最终 `python3 -m unittest discover -s scripts/tests -v`：**Ran 98 tests in 9.882s, OK**。本审查未重跑。
- 保留日志确认 clang-format 19.1.7 dry-run 最终输出 `clang-format-ok`。
- `/tmp/xiaozhi-stroke-stt-local-candidates-build` 实际存在；`project_description.json` 标明 `git_revision: v6.0.2`、target `esp32s3`；sdkconfig 实际含 `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y`。
- 构建日志实际到 2212 steps、`Project build complete`，size check 为 `0x2c0980`（2,886,016 bytes），app partition `0x3f0000`，余 `0x12f680`（30%）。compile_commands 实际含 application、CoreS3 board、store/controller/view。
- `xiaozhi.bin`、ELF、map、flash_args、generated_assets.bin 均存在；作者保留的大小/hash清单与报告一致。
- 作者前后保留的 `git status --short` 中 Stick-S3、emoji、Assets/CMake 及既有 stroke 文件仍在，最终没有 staged/deleted 项；这支持“保留未提交工作”，但只读审查不能证明未曾执行过 reset/clean。
- 没有烧录、真实远端 ASR、普通聊天、触摸、音频、heap/FPS/WDT 真机证据。

## 重新送审最低条件

1. 给 stroke start/connect/STT 建立可一致读取、可贯穿 continuation 的 request generation；增加真实 `PeekInterceptStt` 交错测试，证明 cancel 后旧 STT 不生效、新 session 不接收旧 STT、拒绝时不会误吞普通 STT。
2. 所有取消源汇聚到 main-task abort：取消 UI/token，并按 captured generation 停止或作废 pending connect/listen；alert、asset suspend、power、external delete 都必须覆盖。
3. 让 follow-up 抑制绑定具体 round，覆盖 timeout/cancel 后旧 TTS/LLM、下一普通 round、下行音频三个方向；不能用清/置一个全局 bool 在“漏旧回答”和“吞新回答”之间取舍。
4. 候选页补可见退出及 60 秒超时；`StopActiveListeningRound()` 清理待发送音频；网络线程对 stroke STT 做有界验证/复制。
5. 增加 Application+fake Protocol/AudioService 的确定性集成测试，再重跑 host、clang-format、IDF 6.0.2 CoreS3 clean build，并完成真机 ASR/取消/断线/普通聊天/音频泄漏/长稳回归。

- 真实代码的 `PeekInterceptStt()` 分开读取 voice phase 与 token；STT token 是消息到达时从当前本地 session 取的，不是服务端 round 身份，因此不能隔离 cancel 后迟到 STT 与随后新 session。
- `Cancel()` 会清 `listen_request_`，而 `HandleStartListeningEvent()` 和 `ContinueOpenAudioChannel()` 仅在该标志仍为 true 时复核取消；取消后的 stale start/connect continuation 仍可能进入 Listening。
- TTS/LLM 抑制只依赖无 round 身份的 awaiting/suppress 布尔状态；timeout 和 cancel 都会清 suppression，迟到旧 TTS 可随后进入 Speaking，且下行音频只以 Speaking 状态为门。
- Stroke STT 快乐路径确实先复制 `std::string` 再 Schedule 到 main，并在 main 的 display lock 内复核 token、解析、更新 controller/view；但长度与 UTF-8 校验发生在 Schedule 之后。
- UTF-8 parser 按 Unicode code point 处理，支持单字、某字、某怎么写、某词的某，拒绝非法/过长、ASCII、emoji 和歧义多字；本地不存在字符由 store 检查后进入 NoMatch。
- 候选生产路径是识别字优先、store-only、去重、最多 6；当前 null homophone provider 不补无关字，三字 smoke 不会伪装同音候选。
- NoMatch/TimedOut 页面有 R 与 X；候选页没有 X，且 clock tick 只处理 AwaitingSpeech，所以候选页没有已接受的 60 秒退出。
- 作者保留日志证明最终全量 host 98/98 通过；实际 IDF v6.0.2 CoreS3 构建完成，app 2,886,016 bytes、30% 分区余量，构建目录和固件产物仍存在。
- 现有新增测试没有链接 Application/Protocol/AudioService；普通 STT 回归测试只是源码字符串存在性检查，未覆盖生产并发和音频时序。
- 本审查为只读；未修改工作区，未运行 shell、测试、构建或真机操作。

<!-- pi-squad:d67da18a962c7b636a51119ab58699beeac70faf36cf49954276468cce7a42bc -->
## 2026-09-12T09:44:36.616Z — stroke-stt-local-candidates-rereview

# stroke-stt-local-candidates 复审

**Verdict: REJECTED**

本次按只读要求检查了当前真实工作区、原审查结论、Lead 终态报告、保留的 host/专项/IDF 日志与构建目录；未修改文件，也未自行运行 shell、测试或构建。

大部分原问题已实质修复：`StrokeRoundCoordinator` 的路由快照、旧响应隔离、候选 X/60 秒、15 秒等待、网络线程有界 STT、队列清理、编译链接和产物证据均成立。但原 B2 的“取消后不得启动 stale listening”仍有真实窗口，而且当前 fake harness 用同步 `AbortRound()` 代替了生产中的异步 pending-abort/event 路径，未覆盖这个窗口；MQTT hello 也仍不能把响应绑定到具体 open 尝试。因此现在还不能进入最终三字固件打包/烧录准备。

## Blocker

### B2 仍未完全关闭：部分取消源可在 blocking open 返回后先发送 stale `listen/start`、启麦，再于下一轮主循环取消

生产代码的改进对直接调用 `RequestAbortStrokeRound()` 的来源有效：`ContinueOpenAudioChannel()` 在阻塞 `OpenAudioChannel()` 前后检查 `CanContinueOpen(generation)` 和 `IsStrokeAbortPending(generation)`。X、alert、asset suspend、power-save、surface delete 等会设置该 pending abort，因而可在 open 返回时被拦截。

但并非所有已宣称的取消源都会在来源处设置 `stroke_abort_pending_`：

- `ToggleChatState()`、普通 `StartListening()`、`StopListening()` 只设置各自 event bit；
- wake-word callback 只设置 `MAIN_EVENT_WAKE_WORD_DETECTED`；
-收到 reboot 时只 `Schedule([this]{ Reboot(); })`。

如果这些事件在 main task 阻塞于 `OpenAudioChannel()` 时到达，post-open 检查看不到它们，仍会绑定 stroke channel 并 `SetListeningMode()`。下一轮 event loop 中，`MAIN_EVENT_STATE_CHANGED` 又先于 toggle/start/stop/wake 处理；`HandleStateChangedEvent()` 会调用 `StartListeningAudio()`。该函数只检查 coordinator 的 generation/channel/phase，不检查 pending abort 或其他取消 event，于是可能先执行 `SendStartListening()` 和 `EnableVoiceProcessing(true)`，随后才由对应事件调用统一 `AbortStrokeRound()`。

此外，即使是 X 等 pending-abort 来源，也存在 post-open 最后一次检查之后、`StartListeningAudio()` 之前到达的窄窗口，因为 `RequestAbortStrokeRound()` 本身不先使 coordinator generation 失效，而 `StartListeningAudio()` 不检查 pending abort。

这不再是原实现那种无限期卡住 Listening，但仍会在用户已取消或新会话/唤醒已到达后短暂启动麦克风并可能上传旧轮音频，未满足原 B2 和本次明确要求的 stale-start/blocking-open 边界。

**修复条件：**所有异步取消/替换来源必须先发布同一 generation 的取消栅栏，且 `StartListeningAudio()` 在发送 `listen/start` 与启麦前检查该栅栏；或让 coordinator 提供线程安全的 cancel-request 状态。测试必须按生产方式“只置 pending/event、main 仍阻塞”，不能在 fake open 回调中直接同步执行 `AbortRound()`。

## Major

### M1. MQTT open 没有请求代际或 nonce，迟到旧 hello 可被下一次 fresh open 接受

WebSocket 路径有每个 socket 独立的 `WebsocketSessionIdentity` 和 `channel_generation_`，旧 socket 的 data/disconnect 会被拒绝。MQTT 路径则只有 `waiting_for_server_hello_`：每次 `OpenAudioChannel()` 清空 ID、置 true 并等待；`ParseServerHello()` 会接受等待窗口内任意合法 UDP hello，没有客户端 open generation/nonce 可核对。

因此，第一次 open 超时或被取消且尚未绑定 session 时，其迟到 hello 若落入第二次 open 的等待窗口，会被当作第二次 open 的 `opened_session_id_` 和 UDP 密钥。该 ID 尚未进入 coordinator 的 retired 集合，`BindOpenedChannel(new_generation, old_session_id)` 可以成功，随后旧 STT 可能被当成新轮 STT。这正是 fresh-channel/旧轮隔离所要避免的情况。Lead 报告已把它列为服务端时序假设，但没有真实 MQTT 契约或端到端证据证明该假设成立，故不能据此批准。

**修复条件：**MQTT hello 必须回显可关联具体 open 请求的 nonce/generation，或存在可验证且文档化的服务端串行唯一响应契约并有真实端到端证据。

### M2. fake integration harness 虽真实链接 production coordinator，但没有覆盖生产 pending-abort/Application/AudioService 时序

`scripts/tests/test_stroke_order_ui.py` 的 round 测试确实编译并链接 `main/stroke_order/stroke_round_coordinator.cc`，这一点成立。它覆盖了旧 STT capture→cancel→new、旧 TTS/LLM/audio drop、新 normal 全类别通过、15s/60s、channel close、若干 abort reason 与 100 次 identity bind/close。

但 `scripts/tests/stroke_round_integration_harness.cc` 自行重写了 `FakeApplication`、`FakeProtocol`、`FakeAudioService`，未链接生产 `Application`、两种 transport 或 `AudioService`。关键的 blocking-open 用例在 `during_open` 中直接调用 fake `Abort()`，立即执行 production coordinator 的 `AbortRound()`；生产代码在 main 阻塞时只能设置 pending flag/event，coordinator 尚未失效。故该测试绕过并无法发现上述 B2 窗口。fake audio 也只是清两个整数，不能验证真实 capture/playback generation。

原 M4 因此只能算部分关闭：coordinator 自身的确定性测试已具备，但 Application event ordering、pending cancellation 和真实 AudioService 回灌边界仍缺少可执行测试。

## 原项逐项结论

### B1 — coordinator 层关闭；MQTT hello 端到端例外见 M1

- `generation_`、`phase_`、`route_epoch_`、current/stroke channel identity 和 retired IDs 全部由同一个 mutex 管理。
- `CaptureRoute()` 一次复制 bounded session ID、decision、epoch、generation；`CommitStrokeStt()` 与 `RevalidateNormal()` 再核对 epoch、generation、phase 和 identity。
- 已捕获的旧 STT 在 cancel/new generation 后无法 commit；旧 stroke STT 不会回落到普通 chat 分支。
- queued normal TTS start/stop/sentence、STT 和 LLM callback 均在 main 执行前 revalidate；audio 在入 decode queue 前 revalidate。

### B2 — 部分关闭，仍为本次 blocker

- generation 已贯穿 `QueueListeningRequest`、`HandleStartListeningRequest`、`ContinueOpenAudioChannel`、bind、`active_listening_generation_` 和 `StartListeningAudio`。
- fresh stroke 确实先 retire/close 旧 channel、reset streaming，再 `BeginRound()`；stroke 不复用已有 channel。
- `AbortStrokeRound()` 确实先使 coordinator route 失效，并在 `stroke_audio_route_mutex_` 下 `ResetStreamingState()`，之后 stop/abort、清 listening 状态、close channel、撤 overlay、回 Idle。
- 各来源最终会汇入统一 abort；但上文所述 main 被 blocking open 占用时，wake/new-normal/stop/reboot 等不会提前发布 abort pending，且最后启麦点无 pending 检查，故“所有来源均无 stale start”不成立。

### B3 — 关闭到现有 session-ID 契约

- 不再使用无身份 suppress bool；matching stroke ID 的 TTS/LLM/audio 均 Drop，retired ID 后续仍被隔离。
- 有合法新 ID 的 normal channel 绑定后，STT/TTS/LLM/audio 均可 `PassNormal`；普通 STT 的 glyph/chat side effects 保留。
- active stroke 上缺失/非法/未知 ID 为 `FailStroke` 并终止当前轮；inactive 且 strict routing 后的无 ID消息被 Drop，不关闭普通连接。真实服务必须持续提供每 channel 可区分的 ID。

### M1（原项）— 已关闭

`RenderCandidates()` 有 44×44 的 X；`kCandidateTimeoutMs=60000`，首次进入及从演示返回候选页都会重置候选计时。harness 校验 60 秒边界。

### M2（原项）— 已关闭

stroke STT 使用 `ProbeJsonString(..., 96)` 做有界 NUL 探测，在网络 callback 中先执行 UTF-8/长度解析，仅有界有效内容才构造 `std::string` 并 Schedule。普通 STT 未被错误施加该 96-byte 策略。

### M3（原项）— 代码层关闭，运行时仍需真机

`AudioService::ResetStreamingState()` 禁用 voice processing，递增 capture/playback generation，清 encode/send/decode/playback/testing/timestamp queues；已被 codec task 取出的 encode/decode 工作在回队前分别核对 generation。`AbortStrokeRound()` 与 `FinishStrokeListening()` 在 route mutex 下执行 reset，防止通过 route revalidation 的旧下行包在 drain 后入队。已经交给硬件 codec 的单个 PCM frame 无法撤回，属于已披露真机边界。

CoreS3 使用的 `AfeAudioEngine` 另有 `control_generation_` 和 deferred buffer reset，避免 disable/re-enable 后采用旧 blocked fetch；但现有 host fake 没有执行这些生产路径。

### M4（原项）— 部分关闭，见 Major M2

真实 coordinator 已进入 harness，关键 route 规则大多覆盖；但 production Application pending/event ordering 与 production AudioService 并未链接，不能把 fake 结果表述成完整 Application/Protocol/Audio 时序回归。

### 原 minor — 已关闭

- `SetCandidates()` 将输入扫描限定到 `kMaxCandidateInputs == 24`，输出仍最多 6。
- coordinator deadline 使用 `uint64_t` monotonic ms，`ElapsedAtLeast()` 先检查 `now >= started`，消除了原 32-bit 回绕问题。
- 当前证据清楚区分保留的全量 98 项与本轮专项 7 项。

## 其他 minor / 残余风险

1. retired identity 只保存最近 16 个。更老的有效旧 ID 在 active stroke 中不再识别为 retired，而会作为 unknown ID 触发 `FailStroke`、中止当前轮；这是安全失败而非误接收，但不完全等同“所有迟到响应只丢弃”。100 次 harness 循环只验证 distinct normal ID 可反复 bind/close，没有在第 100 轮重放第 1 轮 STT/TTS/audio。
2. 全文件 clang-format dry-run 的保留结果是 30/38 pass、8 fail；失败包含历史文件风格债和明确无关 Stick-S3 工作。artifact scoped changed-lines 检查与新增 StrokeOrder/harness 整文件检查通过，`git diff --check` 通过。因此不把全文件失败列为功能 blocker，但报告必须继续使用“scoped pass”，不能简写成整个工作区 clang-format 全绿。
3. 当前只含 `一/人/口` 三字 smoke corpus、no-op homophone provider；它适合三字工程验证，不是约 500 字发布资源。

## WebSocket/MQTT identity 核对

- WebSocket：每次 open 建立独立 identity，并用 atomic channel generation 拒绝旧 socket data/disconnect；binary audio 标记该 socket hello ID，close 回传同一 ID。
- MQTT：control JSON 由消息自带 ID 进入同一 Application route gate；UDP callback 捕获创建时的 `channel_session_id`；goodbye 在 schedule 前后核对 current ID；close 回传关闭前复制的 ID。
- 两者在已成功绑定 channel 后的 control/audio/close 路由语义一致。差异是 MQTT hello 没有 per-open correlation，故不能宣称 fresh-open 语义完全一致。

## 验证与产物证据

本 Reviewer 没有运行命令。读取到的保留证据为：

- `/tmp/stroke-revision-host.log`：`Ran 98 tests in 9.972s`，`OK`。
- `/tmp/stroke-finalize-targeted.log`：7 tests，4.384s，`OK`；包含真实 coordinator harness。
- clang-format 19.1.7：artifact changed lines PASS；新增 StrokeOrder/harness 全文件 PASS；全 changed/untracked C++ 为 30 pass / 8 fail，失败为历史/无关文件；`git diff --check` PASS。
- `/tmp/xiaozhi-stroke-stt-local-candidates-finalize-incremental.log`：ESP-IDF v6.0.2，30-step CoreS3 增量 build，编译 Application、WebSocket、MQTT、CoreS3、StrokeOrder view 并最终 `Project build complete`。先前同 build dir 日志还显示 AudioService 被重编译。
- build sdkconfig：esp32s3、`CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y`、16MB、`partitions/v2/16m.csv`、quad 80MHz PSRAM。
- 当前可见 `flash_args` 包含 bootloader `0x0`、partition `0x8000`、OTA data `0xd000`、app `0x20000`、assets `0x800000`。
- 保留 shell 证据记录 `xiaozhi.bin` 2,899,296 bytes，SHA-256 `418fef0b37ba6f9ead414737287c0c41e29648d4304c95ecc265f3458439eec7`；app partition 4,128,768 bytes，余 1,229,472 bytes（29.78%）。该 hash 是读取作者日志所得，本次未自行重算。

这些证据证明当前源能在既有 CoreS3 build dir 增量编译链接，并产生可供后续使用的多文件 flash set；不证明启动、触摸、麦克风取消、真实 ASR、WebSocket/MQTT 或长期稳定性。

## 大 diff 与用户工作保护

保留的最终状态显示 21 个 tracked 文件有改动（约 1200 insertions/118 deletions）及多组 untracked 文件，staged names 为空，`git diff --check` 通过。CoreS3 的明显格式 churn 已缩减；剩余大改动主要是已批准 foundation/UI/STT/protocol/audio 链路。

明确无关的 Stick-S3、emoji、`partitions/v2/8m_single_app.csv`、`.squad/`、`CONTEXT.md` 等仍存在且未 staged/deleted；没有看到 Lead 删除或覆盖它们的证据。只读审查不能证明历史上从未执行 reset/clean，也不能把整个 dirty worktree视为一个可直接提交的原子 patch。后续必须只选择经审查的 stroke/CoreS3/protocol/audio/asset 路径，继续排除这些用户工作。

## 放行结论

**不批准进入最终三字固件打包/烧录准备。** 先关闭 blocking-open 后的异步取消栅栏并补生产形状的回归测试；同时解决或以可验证服务端契约封闭 MQTT late-hello 关联问题。修复后再用当前三字 assets 重跑专项、全量 host、artifact scoped format、CoreS3 增量或 clean build，然后才可进入烧录准备。

即使后续代码复审通过，仍必须在真实 WebSocket 与 MQTT/UDP 上分别验证 hello/session ID、STT/TTS/LLM/audio/close、取消/重连/超时，并在 CoreS3 真机验证 FT6336 坐标、overlay 独占与普通短触回归、音频残留、15s/60s、100 次会话 heap/timer/WDT/underrun；这些不能由当前 host/build 证据替代。

- `StrokeRoundCoordinator` 已把 phase、generation、route epoch、current/stroke channel identity 和 retired IDs 放在同一 mutex 下，并要求主任务按快照重新验证。
- 生产 `ContinueOpenAudioChannel()` 仅能提前看到 `stroke_abort_pending_`；wake/toggle/start/stop/reboot 等事件在 main 阻塞 open 时不会设置该标志，且 `StartListeningAudio()` 不检查 pending abort。
- WebSocket 用 per-socket identity 和 channel generation 隔离旧 callback；MQTT hello 只有 `waiting_for_server_hello_`，没有 per-open nonce/generation correlation。
- 当前 fake integration test 确实链接 production `stroke_round_coordinator.cc`，但 FakeApplication/FakeAudioService 是重写；blocking-open 用例同步调用 AbortRound，不复现生产 pending-event 时序。
- `ResetStreamingState()` 递增 capture/playback generation 并清六类队列；encode/decode 回队前有 generation 检查，已交给 codec 的一个输出 frame 仍只能在真机界定。
- 候选页有 44×44 X，候选 deadline 60 秒，speech deadline 15 秒；返回候选页重置候选计时。
- stroke STT 在网络回调先做 96-byte 有界探测和 UTF-8/长度解析，随后才复制并 Schedule；`SetCandidates()` 最多扫描 24 项。
- 保留日志记录全量 host 98/98、专项 7/7 和 IDF 6.0.2 CoreS3 增量 build complete；本次未重新执行。
- 当前 build 的 app 为 2,899,296 bytes、分区余 1,229,472 bytes，flash set 包含 app 与 generated assets；三字 corpus 仅为一/人/口。
- 最终工作区状态无 staged/deleted 项，Stick-S3/emoji/8m partition 等无关未提交工作仍在；全文件 format 不是全绿，只有 artifact scoped changed lines 与新增文件检查通过。

<!-- pi-squad:f4e03bf98829886cac2414f66205317ac8e3b5975257280a4c7c4ed9c03212cf -->
## 2026-09-12T10:24:43.727Z — stroke-stt-local-candidates-final-review

# stroke-stt-local-candidates 最终复审

**Verdict: REJECTED**

本次按只读要求检查了真实工作区源码、当前 host harness、作者保留的测试/格式/构建日志以及隔离 CoreS3 构建目录；**未修改文件，也未自行运行 shell、测试或构建**。

除异步取消覆盖仍有缺口外，其余指定复审项基本成立：WebSocket/MQTT capability 与 MQTT 本地三字降级正确，`AudioService` 已真实接入 generation gate，100 轮 retired overflow 用例已正确清理，99/99、专项 8/8、scoped format、diff check 和 IDF 6.0.2 CoreS3 构建证据均存在；先前关闭的 B1/B3、候选上限、15s/60s timeout 与有界 STT 输入未见回退。

但是，“**所有异步 cancel 入口都在 event/schedule 前发布 generation-bound fence**”并不成立。真实 transport close 路径仍能重现上一轮 B2 所描述的 stale `listen/start`/启麦窗口，因此本轮不能批准进入最终三字固件打包或最终审计。

## Blocker

### B2 仍未完全关闭：异步 audio-channel close 在 `Schedule` 前没有发布 cancel fence

真实代码证据：

- `main/application.cc` 的 `protocol_->OnAudioChannelClosed(...)` 先调用 `StrokeRoundCoordinator::CaptureChannelClose(...)`，随后直接 `Schedule(...)`；即使 snapshot 的 decision 是 `AbortStroke`，callback 线程也没有先调用 `PublishCancelFence(close.generation)` 或 `RequestAbortStrokeRound(...)`。
- `Application::Run()` 固定先处理 `MAIN_EVENT_STATE_CHANGED`，后处理 `MAIN_EVENT_SCHEDULE`。
- `ContinueOpenAudioChannel()` 虽然在 blocking `OpenAudioChannel()` 前后检查 fence/pending/`CanContinueOpen()`，`StartListeningAudio()` 也在发送前检查 fence/pending/`CanStartListening()`/`MarkListeningStarted()`，但 transport close callback 没有发布 fence，所以这些 gate 看不到该取消。

存在明确时序：

1. stroke open 成功并完成 bind，`SetListeningMode()` 发布 state-change event；
2. 远端随即断开，`OnAudioChannelClosed` 捕获 `AbortStroke`，但只发布 scheduled task；
3. 下一轮主循环先执行 `HandleStateChangedEvent()`；
4. `StartListeningAudio()` 看到 generation/channel/phase 仍有效，执行 `SendStartListening()` 和 `EnableVoiceProcessing(true)`；
5. 随后才执行 scheduled close callback 并 abort。

这正是上一 review 要消除的“取消已到达但仍短暂启麦/发送旧轮 start”窗口。更窄的窗口也仍存在：若 disconnect 发生在 post-open `IsAudioChannelOpened()` 检查之后、`BindOpenedAudioChannel()` 之前，coordinator 尚无已绑定 channel，`CaptureChannelClose()` 可返回 `Ignore`，随后主线程仍可 bind 并进入 listening。

同一“所有入口”不变量还未覆盖以下 source-side 路径：

- `Board::SetNetworkEventCallback` 的 `Scanning`/`Disconnected` 只置 `MAIN_EVENT_NETWORK_DISCONNECTED`；
- `Protocol::OnNetworkError` 只置 `MAIN_EVENT_ERROR`；
- 公开且标注 thread-safe 的 `ResetProtocol()` 直接 `Schedule(...)`，到 main task 才 abort；
- `RequestStartStrokeRound()` 可在 main task 中替换已有 round，但请求入口本身不发布当前 generation fence。

其中部分路径凭当前事件处理顺序通常会先 abort，风险低于 audio-channel close，但它们仍不满足本轮明确要求的 source-before-event/schedule 约束。

**关闭条件：**至少让 correlated transport close 在 callback 线程、排队前对其确切 generation 发布 fence，并处理 close 落在 open 完成与 bind 之间的身份/代际窗口；同时逐项盘点 network disconnect/error、protocol reset、round replacement 等真正会取消/替换 round 的异步入口。修复后，最终 start gate 必须能观察到这些入口发布的 fence，且不得把旧 generation 转成 generation 0 普通监听。

## Major

### M2 仍为部分关闭：production-shape harness 没有覆盖所有真实异步入口和上述 close 排程顺序

`scripts/tests/stroke_round_integration_harness.cc` 已有正确的核心形状：

- `during_open` 调用 `PublishCancelEvent()`，只执行 `coordinator.PublishCancelFence(...)` 并设置 pending 字段，没有同步 `AbortRound()`；
- `ContinueOpen()` 的 post-open gate 或 `StartListeningAudio()` 的最终 gate消费 fence；
- 覆盖 Toggle、Start、Stop、WakeWord、Reboot、Alert、AssetSuspend、PowerSave、SurfaceDeleted 九类 fake source；
- 覆盖 post-open 到 start 之间发布 fence，断言不发 start、不开麦。

但 fake source 枚举没有 ChannelClosed、NetworkDisconnected、NetworkError、ResetProtocol/round replacement；`FakeProtocol::Close()` 的 callback 也是同步计数逻辑，没有模拟生产中的“close callback 只 Schedule、state-change 先于 schedule”顺序。因此“覆盖每类入口”以及防止真实窄窗口的证据仍不足，且现有 8/8 通过无法发现上述 blocker。

## 已通过的指定核对项

### 1. 已接入 fence 的原有入口与 gate

- `ToggleChatState()`、普通 `StartListening()`、`StopListening()`、wake-word callback、`WakeWordInvoke()`、system reboot、`Alert()` 均在各自 event/schedule 前调用 `PublishStrokeCancelFence()`。
- X、asset suspend、power-save、surface delete 等经 `RequestAbortStrokeRound()`，该函数先 `PublishCancelFence(expected_generation)`，再写 bounded pending slot 和设置 abort event。
- `ContinueOpenAudioChannel()` 在 blocking open 前后均检查 fence、pending 和 generation/phase；`BindOpenedChannel()` 自身也拒绝 fenced generation。
- `StartListeningAudio()` 在 `SendStartListening()`/`EnableVoiceProcessing(true)` 前检查 fence、pending、`CanStartListening()` 和 `MarkListeningStarted()`。

这些实现关闭了上一 review 明确列出的 Toggle/Start/Stop/Wake/Reboot/X 等窗口，但不能覆盖上面的 transport close 缺口。

### 2. Protocol capability 与 MQTT 本地降级

真实头文件确认：

- `Protocol` 两项 capability 默认 `false/false`；
- `WebsocketProtocol` 为 `true/true`；
- `MqttProtocol` 为 `false/false`；
- `Application::StrokeVoiceRoutingAvailable()` 要求两项同时为 true，没有依赖 concrete transport 或 `dynamic_cast`。

`BeginStrokeRoundFromMain()` 在 MQTT 上不会调用 stroke `QueueListeningRequest()`，而是进入 `BeginLocalStrokeCandidatesFromMain()`；`StrokeOrderView::StartLocalCandidateSessionFromMain()` 使用固定 `{0x4E00, 0x4EBA, 0x53E3}`，即“一/人/口”，并进入 Candidates。fake harness 断言 `opens==0`、`start_listen==0`、mic off。普通 MQTT 的 no-stroke/no-ID STT 与 generation-0 normal bind + 带 ID STT 也有回归断言。

因此上一 review 的 MQTT late-hello 风险没有被错误宣称已修复，而是通过 capability **禁止 MQTT stroke voice open** 实现设备侧 fail-closed。真实 MQTT 普通聊天仍缺少本次端到端运行证据，列入残余限制。

### 3. AudioService generation gate

- `main/audio/audio_service.h` 的生产 `AudioService` 持有 `AudioStreamGenerationGate stream_generation_`。
- capture task 入队记录 `capture_generation`，Opus encode 完成后仅在 `AcceptCapture(...)` 成立时回到 send queue。
- decode 取包时记录 `playback` generation，解码完成后仅在 `AcceptPlayback(...)` 成立时回到 playback queue。
- `ResetStreamingState()` 在 queue mutex 下调用 `BumpStreaming()` 并清 encode/send/decode/playback/testing/timestamp queues；`ResetDecoder()` 单独 bump playback。
- `audio_stream_generation_harness.cc` 验证 bump 后旧 encode/decode token 均拒绝且 stale queue 被清；round integration 也验证 abort/reset 后旧 token不能回灌。

这是“生产代码静态接线 + host-testable gate”的有效证据；host harness 没有实例化完整 ESP-IDF `AudioService`，但本项要求的实际接线可由源码直接核实。已经提交给硬件 codec 的 PCM 帧仍无法撤回。

### 4. 100 轮 retired overflow

当前 harness：

- 连续 100 次 stroke begin/open/listen/abort 后确认无 active round；
- 第 101 轮使用新 ID 后重放第 1 轮 ID，STT 与 TTS/LLM/audio 均为 `FailStroke`，STT commit 失败；
- 随后显式 `AbortOnMain()` 并调用仅用于测试隔离的 `EnsureIdle()`；确认 coordinator inactive、channel closed；
- 之后 100 个新 generation-0 normal ID 均能 bind/close。

该项满足“溢出旧 ID 在 active stroke 中 fail-closed，清理后 normal bind 恢复”。需要准确表述：abort 后对旧 ID 的一次无 channel `CaptureRoute()` 为 `Drop`，并不证明这个已从 16 槽 ring 淘汰的 ID 永远不能再次用于 `BindOpenedChannel(0, old_id)`；当前实现实际允许不在 retired ring 中的有效 ID成为新的 normal channel identity。

### 5. B1/B3、候选/timeout/bounded input 无回归

- B1：coordinator 的 generation/phase/channel identity/route epoch/cancel fence 均受同一 mutex 保护；capture snapshot 在 commit/revalidate 时重新核对 epoch、generation、phase 和 session identity。
- B3：stroke TTS/LLM/audio 被 drop，retired ID 被隔离；普通绑定 channel 的 STT/TTS/LLM/audio 仍走 `PassNormal`，queued normal 输出不能跨入 stroke round。
- 候选：`SetCandidates()` 最多扫描 24 个输入，去重/数据可用性过滤后最多输出 6 个。
- timeout：语音 15 秒、候选 60 秒，使用 `uint64_t` monotonic millisecond 与非下溢比较；返回候选页会重新调用 `MarkCandidates()`。
- bounded input：stroke STT 先以 `ProbeJsonString(..., 96)` 有界查找 NUL，再做 UTF-8/最多 32 code point 解析，只有有效 bounded 内容才构造 `std::string`。

## 验证证据

本 Reviewer 未运行命令；以下均为读取到的保留证据：

- `/tmp/stroke-stt-retry-targeted.log`：`Ran 8 tests in 3.811s`，`OK`。
- `/tmp/stroke-stt-retry-host.log`：`Ran 99 tests in 10.577s`，`OK`。
- 作者会话输出：clang-format 19.1.7 对列出的 application/audio/protocol/coordinator/harness 范围均为 `FORMAT_OK`；`git diff --check` 为 `DIFF_CHECK_OK`。这只是 scoped 证据，不代表整个 dirty worktree 全部格式化。
- `/tmp/xiaozhi-stroke-stt-local-candidates-fence-incremental.log`：IDF v6.0.2 CoreS3 构建重编译了 protocol、Application、AudioService、CoreS3、coordinator/view，最终 `Project build complete`。
- 最后一次 harness-only 收尾后的增量 build 无 source rebuild；隔离 `sdkconfig` 仍明确为 `esp32s3`、`CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y`、16MB、`partitions/v2/16m.csv`。
- 当前隔离目录存在 `xiaozhi.bin`、`xiaozhi.elf`、`xiaozhi.map`、`flash_args`、`generated_assets.bin`；保留哈希/大小为：
  - `xiaozhi.bin` 2,900,880 bytes，SHA-256 `81d92a03b2c3f39b7b8c7ee4c930dc13d9d6cb38ddc944e741630a29de6ebd92`
  - `xiaozhi.elf` 40,022,980 bytes，SHA-256 `84247ab6b2e14d1e7b66cf0b6c8f45181d4fab38f1d83e0ce909439d75f61393`
  - `xiaozhi.map` 24,523,474 bytes，SHA-256 `e4bbb91859f6760cd96bdf688eb0d796a2e8e39878064c31a4f0ef30f814d6ad`
  - `flash_args` 203 bytes，SHA-256 `928ffa71cfb830d3c13e28cea1491354655f13052a52c6a501b8e59d4973d6db`
  - `generated_assets.bin` 1,678,548 bytes，SHA-256 `4d52ca7fafc7d9ce65779a435357c0c8b12346b852de2da59bed7eb753cd4e90`
- 构建日志显示 app `0x2c4390` / partition `0x3f0000`，剩余 `0x12bc70`（30%）；`flash_args` 含 bootloader `0x0`、partition `0x8000`、OTA data `0xd000`、app `0x20000`、assets `0x800000`。

## Minor / 证据表述

1. 作者报告中“abort 后同一旧 ID Drops，不能作为 normal bind”的后半句证据过强：harness 只验证无当前 channel 时 `CaptureRoute(old_id)==Drop`，没有验证同一 old ID 的 normal bind 会被拒绝；16 槽淘汰后实际并无永久 deny-list。当前要求的 active-round fail-closed 与 fresh normal bind 恢复仍成立。
2. 最新 firmware build 是基于已有隔离目录的增量验证；相关 production source 的确在前一份同目录 fence build 中重编译，最后一轮只改 host harness。它不是 clean build，也不是烧录/启动证明。

## 真机与真实服务限制

即使修复 blocker 并通过下一轮代码复审，仍需明确保留：

- 尚未烧录 M5Stack CoreS3；FT6336 坐标、overlay 独占、普通短触 `ToggleChatState` 回归、X/点选/15s/60s、帧率、100 次 heap/timer/WDT/audio-underrun 仍需真机。
- 未连接真实 WebSocket 或 MQTT/UDP 服务；session ID、断线/close 时序、取消后无短暂上传、普通 MQTT 聊天均未端到端验证。
- MQTT late hello 的 transport 关联问题仍存在；当前安全边界是 MQTT 永远不启用 stroke voice，只提供本地“一/人/口”。
- corpus 仍只有三字 smoke 数据，并非约 500 字发布字库；homophone provider 仍是 no-op。
- generation gate 不能撤回已经交给硬件 codec 的单帧 PCM。

## 放行结论

**不批准**进入最终三字固件打包和独立最终审计。先修复 transport/network/protocol-reset 等异步取消入口的 generation fence，尤其是 `OnAudioChannelClosed` 的 open/bind/start 窄窗口，并让 production-shape harness 覆盖这些真实入口与事件顺序；随后重跑专项 8/8、全量 host、scoped format/diff 与 IDF 6.0.2 CoreS3 构建，再做最终复审。

- `Application::OnAudioChannelClosed` 当前只捕获 close snapshot 后 `Schedule`，没有在排队前发布 generation cancel fence；`Application::Run` 又先处理 STATE_CHANGED 后处理 SCHEDULE，因此存在先 `StartListeningAudio`、后 scheduled abort 的真实窗口。
- 当前 production-shape harness 的 during_open 确实只发布 fence+pending，不同步 AbortRound，并覆盖九类 fake source及 post-open/start 窗口；但未覆盖 ChannelClosed、NetworkDisconnected/Error、ResetProtocol 或真实 close schedule 顺序。
- Protocol capability 的真实实现为 WebSocket true/true、MQTT false/false；MQTT 笔划入口进入固定一/人/口本地候选且不调用 stroke voice open。
- AudioService 真实持有并使用 AudioStreamGenerationGate：capture/encode 与 playback/decode 分别携带 generation，ResetStreamingState 同时 bump 两侧并清队列，旧 in-flight 工作在回灌前被拒绝。
- Retired session ring 固定为 16；第 101 个 active stroke 收到已淘汰的第 1 个 ID 时走 FailStroke。显式 abort/EnsureIdle 后 fresh generation-0 normal bind 可恢复。
- 保留日志证明专项 8/8、全量 99/99、scoped clang-format 19、git diff check 及 IDF 6.0.2 CoreS3 构建成功；本 Reviewer 未自行执行命令。

<!-- pi-squad:98445d67d3c576a60e68bced4229f6b1cab227d289b3c5a6dedbf1ef081e2d14 -->
## 2026-09-12T10:31:08.806Z — stroke-stt-async-close-fence

已完成 `stroke-stt-local-candidates` 的限定修订，并将完整报告写入 `/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/100f2b8b-c1da-426b-b1cc-c163634652f1/stroke-stt-async-close-fence.md`。WebSocket 的既有 per-socket generation 现在作为预留 `open_attempt_id` 从 open 前贯穿至 close callback，并由 Application 的 opening/bound 单槽映射到确切 stroke generation；close callback 先发布 fence，再只调度 main-task abort，覆盖 open 成功到 bind 之间的窗口且不按当前 token 猜测。Network Scanning/Disconnected、Protocol OnNetworkError、ResetProtocol、RequestStartStrokeRound replacement 也均在 event/schedule 前发布 fence；最终 listening gate 与 fence publisher 串行化并在同步发送错误后、启用采集前复查。MQTT stroke voice capability 仍为 false/false。

测试已扩展到 ChannelClosed、NetworkDisconnected、NetworkError、ResetProtocol、ReplaceRound，并模拟 `STATE_CHANGED` 先于 scheduled abort；还覆盖 open-return-to-bind close identity 和 replacement 不降级 generation 0。专项 2 项通过、stroke UI 9 项通过、全量 host 100 项通过；clang-format 19.1.7 scoped check 与 `git diff --check` 通过。ESP-IDF v6.0.2 仓库外隔离 CoreS3 `CONFIG_STROKE_ORDER_LOCAL=y` 全量后增量构建通过；最终 `xiaozhi.bin` 2,903,072 bytes，SHA-256 `c9f7adf5a386e46d35aa973d4b8573f2ddcb5c6ca0f855623a9479bea3d065fa`。未 commit/stage/reset/clean，未覆盖既有未提交工作。

- WebSocket 的 `channel_generation_` 可在 blocking open 前预留，并作为 exact `open_attempt_id` 随该 socket 的 disconnect callback 回传；Application 因而能覆盖 open 完成但尚未 bind 的竞态窗口。
- MQTT/UDP 继续报告 `SupportsCorrelatedSessionOpen()==false` 与 `SupportsStrokeVoiceRouting()==false`，close identity 的 attempt ID 为 0，未伪造 nonce。
- 生产 Run 顺序会在 `MAIN_EVENT_SCHEDULE` 前处理 `MAIN_EVENT_STATE_CHANGED`；因此异步 cancel 必须在 callback/source 线程先发布 fence，不能只依赖稍后执行的 abort。
- 最终 host suite 当前为 100 项且全部通过；新增 source-shape 测试同时约束真实 Application/WebSocket/MQTT 源码和 C++ production-shape harness。
- ESP-IDF v6.0.2 CoreS3 隔离构建确认 `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y`，最终 app 分区剩余 0x12b3e0 bytes（30%）。

<!-- pi-squad:55f26946efd199d72b02dd9da89780265a486f30487ff2945b51d73305145d3f -->
## 2026-09-12T11:11:43.553Z — stroke-final-core-s3-smoke-review

# stroke-final-core-s3-smoke 最终独立审计

## Verdict: APPROVED

该交付物具备完整、相互一致的可烧录文件集，离线构建/测试/镜像/打包证据足以支持“可烧录的 CoreS3 三字 smoke 固件”结论；包内说明没有把离线构建冒充真机验证，也没有把三字原型冒充 500 字或发布字库。

- **Blocker：无**
- **Major：无**
- **Minor：无阻止交付的问题**
- **边界：**批准的是可烧录 artifact，不是真机功能验收或商用字库发布批准。

本次严格只读，未修改文件、未运行 shell、未重跑测试或构建。审计依据为真实 `dist/` 文件、源码相关配置，以及 run `d758539f-d07f-42f7-b44a-1a4d7f2d6487` 中保留的实际命令输出。

## 逐项核对

### 1. 当前源码 host、diff 与格式证据：通过

保留输出显示：

- `python3 -m unittest discover -s scripts/tests -v` → `Ran 100 tests in 13.216s`、`OK`，即 **100/100 PASS**。
- `git diff --check` 在打包前及打包后均无输出并报告 PASS。
- clang-format **19.1.7** 的最终成功重试报告：`FORMAT_DIFF_OK tracked=16`、`FORMAT_FULL_OK new=15 harnesses=6`。覆盖 tracked 功能改动行，以及全部新增 `main/stroke_order/*.{h,cc}`、`audio_stream_generation.h` 和 6 个 host harness。此前一次因 macOS shell 不支持 `mapfile` 而退出 127，但后续替代命令成功，最终报告没有把失败尝试冒充通过。

### 2. 全新仓库外 IDF 构建与功能编译：通过

保留命令先删除并重建 `/private/tmp/xiaozhi-stroke-final-core-s3-build`，随后使用：

- ESP-IDF **v6.0.2**；
- `IDF_TARGET=esp32s3`；
- `BOARD_NAME=m5stack-core-s3`；
- build-dir 内独立 `SDKCONFIG`；
- `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`；
- `CONFIG_STROKE_ORDER_LOCAL=y`；
- 16 MB flash、`partitions/v2/16m.csv`、quad PSRAM、CoreS3 GC0308 配置。

实际从空目录完成 **2213-step** build，末尾为 `Project build complete`。构建日志和 `compile_commands.json` 均确认编译：

- `application.cc`
- `audio/audio_service.cc`
- `protocols/websocket_protocol.cc`
- `protocols/mqtt_protocol.cc`
- `boards/m5stack/core-s3/m5stack_core_s3.cc`
- `stroke_order_store.cc`
- `stroke_order_controller.cc`
- `stroke_round_coordinator.cc`
- `stroke_order_view.cc`

构建前后 worktree status fingerprint、仓库根 `sdkconfig` SHA-256 和既有 `build/` 聚合 SHA-256 相同，说明隔离构建未替换仓库根构建状态。

### 3. Flash layout、flash_args 与 merged 镜像：通过

真实 `flash_args` 为：

```text
--flash-mode dio --flash-freq 80m --flash-size 16MB
0x0 bootloader/bootloader.bin
0x8000 partition_table/partition-table.bin
0xd000 ota_data_initial.bin
0x800000 generated_assets.bin
0x20000 xiaozhi.bin
```

解析后的实际分区为：

| 内容 | offset | 容量/文件大小 |
|---|---:|---:|
| bootloader | `0x0` | 16,672 B |
| partition table | `0x8000` | 3,072 B |
| NVS | `0x9000` | 16 KiB |
| OTA data | `0xd000` | 8 KiB |
| phy_init | `0xf000` | 4 KiB |
| ota_0 / app | `0x20000` | `0x3f0000` |
| ota_1 | `0x410000` | `0x3f0000` |
| assets | `0x800000` | 8 MiB |

`idf.py merge-bin` 实际调用 esptool raw merge，生成 `0x999cd4` / **10,067,156 B** 的 `merged-binary.bin`，明确 ready to flash at `0x0`。保留的 `dd`/`cmp` 证据显示 bootloader、partition、OTA、app、assets 五个区域在各自 offset 全部 `MATCH`；镜像末尾恰为 `0x800000 + 1,678,548 = 0x999cd4`，小于 16 MiB，剩余 **6,710,060 B**。

### 4. esptool image 校验与 app 余量：通过

esptool v5.3.1 对 bootloader、app 及最终目录中的 merged 镜像执行了 `image-info`：

- 芯片：ESP32-S3；
- flash：DIO / 80 MHz / 16 MB；
- bootloader checksum 与 validation hash valid；
- app checksum 与 validation hash valid；
- bootloader 与 app 均标识 ESP-IDF v6.0.2；
- app 标识 XiaoZhi 2.4.2；
- app 内 ELF SHA 与包内 `xiaozhi.elf` SHA 一致。

`xiaozhi.bin` 为 **2,903,072 B (`0x2c4c20`)**；最小 app 分区为 **4,128,768 B (`0x3f0000`)**；余量 **1,225,696 B (`0x12b3e0`) / 29.69%**。

### 5. 目录、ZIP、SHA256SUMS、manifest 与报告：通过

真实目录中有 16 个文件；`SHA256SUMS` 覆盖除自身外全部 15 个文件。保留验证对原目录和 ZIP 解压目录分别执行 checksum，所有条目均 `OK`；`unzip -t` 报告 `No errors detected`，解压目录与原目录 `diff -qr` 无差异。

关键摘要一致：

- `merged-binary.bin`：`ee74246d94aacbd0645d6daedc39bd461547a99dd7d8e68d3604f06764a9da95`
- `xiaozhi.bin`：`abafd8edf5b7eb1d73fe51ba37f5eb787475e2d79ae3019396aaebc389b9f7c8`
- `generated_assets.bin`：`4d52ca7fafc7d9ce65779a435357c0c8b12346b852de2da59bed7eb753cd4e90`
- `stroke_order.bin`：`9750f7ad01958f48df78102e8facd5183b58646f4350084aa05c03fba18b225f`
- `manifest.json`：`57bed70c73d935eaabc157ab9449af20c9ed2b6192cdcee7407b7ff98d8c986b`
- `SHA256SUMS` 自身：`70e945ba1fcfd92c069f8d28f403d05002d8d85c71fe0a0a049a6e1830333d3c`
- ZIP：**21,509,614 B**；`f70fe2a62fe72192838f87baa978dff9b244ec5d300581468cb466b7ba682f39`

`manifest.json` 中的文件大小、SHA、offset、容量、版本和验证状态与 `SHA256SUMS`、保留输出及 Lead 报告一致。

### 6. stroke_order.bin 来源、许可与范围：通过

包内包含：

- `stroke_order/stroke_order.bin`
- `stroke_order/stroke_order.manifest.json`
- `licenses/ARPHICPL.TXT`
- `NOTICE.md`

manifest/NOTICE 明确固定来源 `chanind/hanzi-writer-data` commit `68d10a4b21150cae5e1ebbd223eed289cf32d90c`，列出一/人/口三个原始 JSON 路径及 SHA-256、上游 `skishore/makemeahanzi`、Arphic Public License、曲线展平/整数化/y 轴翻转等修改。实际 corpus 为且仅为 **一、人、口**，并显式设置 `not_a_release_library=true`、`not_500_characters=true`、`not_a_commercial_or_release_library=true`。构建日志还确认这四个文件被纳入 `generated_assets.bin`。

### 7. 凭据、NVS key 与临时文件：通过

最终文件集由显式 allow-list 生成并验证；真实目录只有上述 16 个交付文件，没有 `sdkconfig*`、`nvs.bin`、credential/secret/token 文件或 `.log`。对文本 manifest/README/NOTICE/license 未发现秘密赋值。固件中可检出的 `BEGIN ... PRIVATE KEY` 字样对应链接进来的 mbedTLS PEM 解析器格式常量，map 将其归因到 `tf-psa-crypto/.../pkparse.c`，不是随包附带的私钥材料。

### 8. README 烧录与使用说明：通过

`README.zh-CN.md`：

- 明确要求两种烧录方式二选一，未混用 merged 与 multi-file；
- merged 命令从 `0x0` 写入，并固定 ESP32-S3 / DIO / 80m / 16MB；
- multi-file 使用包内相对路径 `@flash_args`，与 IDF 自动生成命令一致；
- 擦除被标为可选，并警告会清除配网和持久数据；
- monitor 命令参数经保留的 `idf_monitor.py --help` 接口核对；
- WebSocket 流程准确写为 SO → 远端 STT → 本地候选 → 动画；
- MQTT 因不提供所需 per-open session 关联，准确写为 SO → 本地一/人/口候选，不发起笔划语音选字。

源码也支持该描述：WebSocket 的 `SupportsCorrelatedSessionOpen()` / `SupportsStrokeVoiceRouting()` 为 true，MQTT 两者为 false；无安全语音路由时 `BeginStrokeRoundFromMain()` 进入本地候选降级。

### 9. dirty worktree 与复现边界：通过

README 和 manifest 均显著披露：base commit 为 `8836458c95d52b7e1335837315ec4b000e581e53`，`dirty_worktree=true`，最终固件包含已审查但未提交的 StrokeOrder 改动，因此 base commit 单独不能复现二进制。保留输出也实际记录了该 HEAD 和 dirty 状态，没有把 commit 当作完整可复现来源。

### 10. 真机边界：通过

README、manifest 和报告一致标记 `hardware_validation_performed=false`，并明确未烧录 CoreS3。它没有声称触摸、真实服务、音频、性能或稳定性已通过。

## 面向用户的最终产物

- 首选 ZIP：`/Users/mandyw/git/xiaozhi-esp32/dist/m5stack-core-s3-stroke-smoke.zip`
- 解压/直接目录：`/Users/mandyw/git/xiaozhi-esp32/dist/m5stack-core-s3-stroke-smoke/`
- ZIP SHA-256：`f70fe2a62fe72192838f87baa978dff9b244ec5d300581468cb466b7ba682f39`

进入解压后的 `m5stack-core-s3-stroke-smoke/` 后，首选先校验 hash，再烧录 merged 镜像：

```sh
source /path/to/esp-idf-v6.0.2/export.sh
export PORT=/dev/cu.usbmodemXXXX
shasum -a 256 -c SHA256SUMS
python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 merged-binary.bin
```

不要在同一次操作中再追加 `@flash_args`；若选择 multi-file，则只运行 README 中的 `write-flash @flash_args` 方案。

## 必须继续报告的硬件残余风险

1. 尚未验证真实 CoreS3 的 FT6336 坐标、旋转、SO 点击、overlay 独占触摸及非笔划短触 `ToggleChatState` 回归。
2. 尚未验证真实 Wi‑Fi/激活、WebSocket 服务端 STT `session_id` 关联，以及目标部署的 MQTT 配置。
3. 尚未验证麦克风、扬声器、音频启停，以及暂停/继续/逐笔/重播/返回/退出、断线、超时和中断。
4. 尚未测量约 30 FPS、持续下限约 20 FPS，也未完成 100 次会话的 heap/timer/WDT/audio-underrun 长稳。
5. 不同 CoreS3 批次、USB 串口枚举、自动复位和供电条件仍可能影响烧录或运行。
6. dirty worktree 仍是长期复现风险；本包可由哈希识别，但 base commit 本身不能重建它。

- `stroke-final-core-s3-smoke` 的已验证 flash set 为 bootloader `0x0`、partition table `0x8000`、OTA data `0xd000`、app `0x20000`、assets `0x800000`；merged raw 镜像从 `0x0` 烧录。
- 最终 app 为 2,903,072 bytes，`0x3f0000` app 分区剩余 1,225,696 bytes（29.69%）；merged 为 10,067,156 bytes，未超过 16 MiB。
- ESP-IDF v6.0.2 的 2213-step 空目录构建实际编译了 Application、AudioService、WebSocket、MQTT、CoreS3 与全部四个 StrokeOrder `.cc`。
- 最终包有 16 个文件；ZIP 解压检查无错误，解压文件与原目录一致，ZIP SHA-256 为 `f70fe2a62fe72192838f87baa978dff9b244ec5d300581468cb466b7ba682f39`。
- 三字 SOB1 corpus 仅含一/人/口，SHA-256 为 `9750f7ad01958f48df78102e8facd5183b58646f4350084aa05c03fba18b225f`，并附固定上游 commit、APL、NOTICE 与非发布范围声明。
- 该 artifact 来自 commit `8836458c95d52b7e1335837315ec4b000e581e53` 的 dirty worktree；commit 单独不可重建该二进制。
- 离线构建与镜像校验均通过，但没有真实 CoreS3 烧录、触摸、服务、音频、FPS 或长稳验证。

<!-- pi-squad:5838318f481dee35d97fb434d1a9beedf073e9fd764ee0b47cbbf5c78b1eb8c9 -->
## 2026-09-12T12:33:56.641Z — stroke-500-assets-pinyin-review

# stroke-500-assets-pinyin 独立审查

**Verdict: REJECTED**

实现的主体路径已经相当完整：真实 500 字 SOB1/SPY1 已生成并进入 CoreS3 构建，候选逻辑、MQTT 本地降级、WebSocket STT、PSRAM owned-copy、缺失/损坏 SPY1 降级和 IDF 6.0.2 构建均有正面证据。但当前固件包内的 `SHA256SUMS` 与实际运行时文件名不一致，不能自校验；此外转录来源锁定、SPY1 C++ 语义校验及其 corruption/overflow/unaligned 测试仍有明显缺口。因此不能批准为最终 500 字可烧录交付。

## Blocker

### B1. 打包后的 `SHA256SUMS` 引用不存在的文件，14 文件固件包不是自洽可验证包

- `scripts/package_stroke_order_prototype.py:37-51` 将 `stroke_order.sob1` 重命名为 `stroke_order.bin`、将 `stroke_pinyin.spy1` 重命名为 `stroke_pinyin.bin`，但同时把 `SHA256SUMS` 原样复制；复制循环见同文件 `:135-136`。
- 原始 `scripts/tests/fixtures/stroke_order/prototype_500/SHA256SUMS` 中列出的仍是 `stroke_order.sob1` 和 `stroke_pinyin.spy1`。
- 实际保留构建目录 `/private/tmp/xiaozhi-stroke-500-pinyin-build/stroke_order_assets/` 中只有映射后的 `.bin` 两文件，没有 `.sob1/.spy1`，却保留上述原始 `SHA256SUMS`。
- 结果是对最终 14 文件目录运行标准 checksum 校验会因两个“missing file”失败；这直接破坏任务要求的离线包完整性、名称映射和 SHA256 审计链。现有 `test_prototype_packager_is_offline_and_copies_reviewed_files` 只检查文件存在和文件名长度，没有对**打包后目录**执行 checksum 校验，因此 110/110 没有发现此问题。

**修复要求：**打包阶段按 `COPY_MAP` 重写/重新生成目标目录的 `SHA256SUMS`（至少列 `stroke_order.bin`、`stroke_pinyin.bin`），并增加对最终 14 文件目录逐项校验、无多余/缺失条目的测试；随后重建 `generated_assets.bin`。

## Major

### M1. 转录 JSON 与所声明 commit 没有由生成器绑定，来源锁可被任意本地 JSON + 任意 40 位字符串伪造

- `scripts/stroke_order/select.py:170-205` 的 `official_source_lock()` 只验证 `transcription_commit` 是 40 位十六进制，然后记录任意 `table_json` 的 SHA-256；它不验证该文件来自 `TRANSCRIPTION_REPO`、checkout HEAD 等于该 commit、worktree clean、文件受 Git 跟踪且字节等于 `HEAD`。
- 这与 `scripts/stroke_order/convert.py` 对 HWD checkout 的 HEAD/origin/clean/tracked-blob 强校验不对称。
- 当前保留生成证据确实显示使用了准备好的 leonsilicon checkout，HEAD 为 `f9786a82be6e1672bdc60f85760a9e4a3791d1f1`，且当前 source lock 记录 JSON SHA-256 `a9f0a21f…`; 因而本审查**没有证据表明当前 500 行被补字或来自错误输入**。但仓库内生成流程本身不能保证“与声明转录源一致”，测试也只验证行数、唯一性、连续编号、首字和末编号，并未对 pinned 输入重建/比对。

**修复要求：**像 HWD 一样验证转录 checkout 的 origin、完整 HEAD、clean 状态和 tracked blob，或把权威的转录 JSON SHA-256 固定成代码常量并强制匹配；测试应从该 pinned 输入重建 selection 并与仓库文件逐字节比较。

### M2. C++ SPY1 验证弱于 Python `load_all=True`，可接受语义不一致但 CRC 正确的索引

- `main/stroke_order/stroke_order_pinyin.cc:140-306` 校验了 LE 字段、CRC、计数、范围、rank 唯一、字符排序、组内排序及各自 offset/length，但在完成两个循环后直接提交 bound 状态。
- Python 参考验证器在 `scripts/stroke_order/pinyin.py:512-524` 额外验证：每个字符引用的每个 group 必须含该字符且 rank 相同；每个 group member 必须对应 char index 中相同 rank 的字符。
- 因此，只要重新计算 body CRC，一个 group member 指向不存在字符、rank 不一致、或字符引用一个不含自己的 group 的 SPY1 可以通过 C++ `Bind()`，但会被 Python `validate_spy1(..., load_all=True)` 拒绝。这不是内存越界，但会让候选集合/排序与格式语义不一致。
- C++ 还只要求 readings/member 区域落在 `index_end..size`，不约束两类 payload 的规范布局或互不重叠。若格式有意允许别名，应在格式文档明确；否则应收紧。

**修复要求：**让设备校验至少达到 Python 双向 membership/rank 一致性；增加 CRC 重算后的语义损坏 fixture。

### M3. 现有 SPY1 “unaligned/overflow”测试没有实际命中所声称的解析分支

- `scripts/tests/stroke_order_pinyin_harness.cc:114-119` 把 unaligned 源指针交给公开 `Bind()`；但 `Bind()` 先 `memcpy` 到 `new[]`/PSRAM owned buffer，再调用 `BindView()`，所以这没有验证解析器直接处理未对齐基址。头文件已暴露 `TestOnlyBindView()`，测试却未使用。
- 同 harness `:130-136` 把 `char_count` 改成 `0xffffffff`，但没有重算 header CRC，因此 C++ 会先在 `Crc32(data,24)` 失败，未进入乘法/offset overflow 检查。Python 同名测试也有相同问题。
- 没有看到 CRC 合法条件下针对 `group_index_offset`、`readings_offset`、`members_offset`、乘法/加法 overflow、重叠 payload、reserved、group/member 边界的系统性 corruption case。

**修复要求：**使用 `TestOnlyBindView(unaligned+1, size)`；每次修改结构字段后重算正确 CRC，使测试确实走到目标拒绝分支；为所有关键 offset/count/semantic invariant 建立表驱动 corruption 测试。

### M4. APL 修改告知写了“如何修改”，但未写“何时修改”

- 随包 `ARPHICPL.TXT:29-34` 第 2(a) 条明确要求 modified file 的 prominent notice 说明 **how and when** changed。
- `prototype_500/NOTICE.md:24-30` 说明了 flatten、round、flip-y、clamp、downsample 等“how”，但没有转换日期或版本化变更时间；manifest 为保持确定性也不含时间字段。
- 完整 APL 确实随包提供，衍生文件和转换器也在仓库中；本项不是对授权结论的法律裁决，但在最终对外打包/发布前应由项目法务确认并补齐修改时间/版本告知。

## Minor

1. `scripts/stroke_order/select.py:145-162` 读取 CSV 时保留文本 `codepoint`，却把 `codepoint_int` 直接设为 `ord(character)`；`verify_selection_rows()` 因而不会发现 CSV 的 `codepoint` 列与汉字不符。当前读取到的 500 行看起来一致，但自动测试存在盲点。
2. 保留报告给出了最终 `generated_assets.bin` 大小 2,877,288 bytes，却没有给出与前一三字资产构建在相同配置下的严格增量；容量本身充足，但“增量”尚未被量化。
3. PSRAM 分配失败目前只有静态代码路径和日志设计证据，没有 ESP 上的失败注入；约 1.065 MiB SOB1 + 18 KiB SPY1 双 owned-copy 的启动成功、heap 碎片和长期稳定仍只能由真机确认。

## 逐项核对

1. **selection-500：结构通过，来源闭环有 M1。** 实际 `selection-500.csv` 为 500 行，rank/`official_number` 为 1/0001 到 500/0500，字符均在 U+4E00..U+9FFF 且测试验证唯一；`charset-500.txt` 顺序一致。生成器只截取 tier1 前 500，HWD 任一失败即终止，不从 0501 补字。保留生成证据显示使用了声明 checkout；但生成器未自行锁定该 checkout。
2. **HWD/SOB1：通过。** `stroke_order.cov.json` 为 500/500、0 errors、commit `68d10a4b…`、clean；manifest 包含 500 个源文件路径/hash。生成记录为 SOB1 **1,046,788 bytes**、SHA-256 `3ca8fb08…`，距 1 MiB 1,788 bytes；Python 和 C++ harness 均全量 load 500。Python/C++ 上限仍分别为 1,048,576，没有提高；record 受 16 KiB、48 strokes、256/64 points 限制。
3. **来源/许可：大体通过，APL 告知见 M4。** 官方 PDF URL、100,606,660 bytes、SHA-256 `af85c706…`；无许可证转录辅助的 repo/commit/hash和限制均披露；HWD/APL、Unicode 16.0.0/Unicode License v3 齐全；converter 只提取 `strokes`/`medians`，没有打包 dictionary/all.json；文档明确 not official、not commercial、not manually 500-reviewed。
4. **SPY1：基础安全通过，完整格式验证/测试见 M2/M3。** Python/C++ 均显式 LE 字节读、CRC、u32 add/mul/range、64 KiB、1024 chars/groups、8 readings、32 members；实际 SPY1 18,152 bytes、500 chars。手写 golden 与 CRC known-answer 存在。
5. **拼音/候选：通过。** Python 从 kMandarin primary 加安全 kHanyuPinyin union，规范化为字母+1..5；provider 多音 union、按 rank/codepoint 排序和去重；controller 把 primary 放首位，再做 Basic CJK、SOB1 contains、真实 `LoadCharacter`、去重和最多 6 的过滤。库外 primary 可由 provider seam 提供本地候选；固定 500 SPY1 自身只认识其 500 字，因此任意库外字通常没有同音结果，这是当前数据范围而非 controller 早拒绝。
6. **传输分支：通过。** `websocket_protocol.h` 宣布 correlated open/stroke voice；`ApplyVoiceUtteranceLocked()` 使用 `pinyin_provider_`。MQTT 两能力均 false；`Application::BeginStrokeRoundFromMain()` 直接进入本地候选，`StartLocalCandidateSessionFromMain()` 使用 `AppendTopRanked(6)`，不会调用 `OpenAudioChannel()`。
7. **owned copy/lifecycle：代码通过，真机仍待验。** `stroke_order_alloc.h` 在 ESP 只申请 `MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT`，无 DRAM fallback；SOB1 失败隐藏入口，SPY1 失败降级。View suspend 先失效 UI 再 unbind 两份 copy；rebind 先 SOB1 后可选 SPY1。坏/缺 SPY1 不清除成功的 SOB1。
8. **离线/CMake：除 B1 外通过。** packager无网络，映射两个运行时名；CMake 声明 14 个 output 和对应 depends，使用 staging/backup/rename，构建日志显示 `Processed 14 extra files`。所有 basename ≤31。
9. **验证证据：构建通过，但本审查未重跑。** 作者保留日志显示最终 host suite **110/110 OK（11.531s）**、clang-format 19.1.7 scoped check、`git diff --check` PASS。IDF v6.0.2 新建仓库外目录完成 2214-step CoreS3 build，实际编译 store/controller/pinyin/view/coordinator。
10. **STT/session 与用户工作：未见回退。** 当前 WebSocket/MQTT capability 与 generation/session/fence 路径仍在；本轮没有 `type=stroke`。初始状态记录了既有大量未提交工作，作者未 commit/stage/reset/clean；仓库 `sdkconfig` 前后 SHA-256 都是 `0fbe5276…`。本审查没有 shell 权限，不能独立执行最终 `git diff`，但未发现破坏既有工作证据。

## 构建产物证据

保留并实际读取的构建目录：`/private/tmp/xiaozhi-stroke-500-pinyin-build`。

- IDF `v6.0.2`，target `esp32s3`，`CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`，`CONFIG_STROKE_ORDER_LOCAL=y`
- PSRAM quad/80 MHz/heap malloc；16 MiB flash，`partitions/v2/16m.csv`
- `xiaozhi.bin`: 2,907,712 bytes，SHA-256 `911a0615e51cba206e62611ff227bac0b1992fc1c211eaf056e04580bece6a9d`
- `generated_assets.bin`: 2,877,288 bytes，SHA-256 `c005ca8bcaec1896fbb66784dd024844fc5f3f359d3fa0c877d025bdf7bfabee`
- `xiaozhi.elf`: 40,147,184 bytes，SHA-256 `b05eb476dc425b9c526c4b086868c796466df62f472ccca6264d74f4f48fb18b`
- `xiaozhi.map`: 24,555,960 bytes，SHA-256 `9783bae4a0f64226e8383c58b256dba5fa04cb660264901aed01d9681e4e51ce`
- `flash_args`: 203 bytes，SHA-256 `928ffa71cfb830d3c13e28cea1491354655f13052a52c6a501b8e59d4973d6db`
- app partition 0x3f0000，剩余 0x12a1c0 = 1,221,056 bytes（日志约 30%）
- flash offsets：bootloader 0x0、partition 0x8000、OTA 0xd000、app 0x20000、assets 0x800000

## 本审查实际执行说明

按只读限制，本审查只执行了文件读取、目录枚举和文本检索；**没有运行 shell、unittest、clang-format、git 或 idf.py，也没有修改文件**。上面的测试/构建结果均来自保留作者日志，并通过读取真实源码、fixture 和保留构建目录交叉核对；不能表述为本 Reviewer 亲自重跑。

## 修复后复审/烧录前条件

1. 修复最终目录 `SHA256SUMS` 的重命名映射并增加端到端 checksum 测试。
2. 绑定 transcription JSON 到 pinned repo/commit/blob，重生成并逐字节比对 selection/charset/SOB1/SPY1/manifests/source locks。
3. 补 C++ SPY1 双向 membership/rank 校验与真正命中 unaligned/overflow/offset 分支的 corruption 测试。
4. 补齐/法务确认 APL 修改日期与分发告知，继续保留 prototype/not-release/not-commercial/not-manually-reviewed 标记。
5. 再跑 110+ host tests、clang-format 19、diff-check，并从空 build dir 重建 CoreS3；重新记录所有 hash/容量。
6. 真机重点：启动日志确认两份 copy 均 `ptr_in_psram=1`；覆盖 PSRAM 不足 fail-closed、500 字抽样与高笔画字动画、WebSocket 语音多音候选、MQTT 绝不开 voice、坏/缺 SPY1 仍可用 SOB1、assets suspend/rebind、100 次会话 heap/timer、触摸/FPS/WDT/音频 underrun。修复前不应把当前 build 当作最终可发布/可交付固件。

- `prototype_500/selection-500.csv` 当前包含 500 行唯一 Basic CJK，编号 0001–0500；保留生成证据使用 leonsilicon 转录 checkout commit f9786a82be6e1672bdc60f85760a9e4a3791d1f1，且生成器在 HWD 不完整时拒绝从后续编号补字。
- 当前 SOB1 为 1,046,788 bytes、SHA-256 3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b，保持 1 MiB 上限；HWD coverage 记录为 pinned commit 68d10a4b21150cae5e1ebbd223eed289cf32d90c 的 500/500。
- 当前 SPY1 为 18,152 bytes、500 chars，Python/C++ 常量均保持 64 KiB、1024 chars/groups、8 readings/char、32 members/group；设备解析使用显式 little-endian 字节读取。
- 固件打包把 `.sob1/.spy1` 重命名为 `.bin`，却原样复制仍引用旧文件名的 SHA256SUMS；实际 14 文件目标目录不存在旧文件名，因而该 checksum 清单不能在最终包中直接校验。
- C++ SPY1 BindView 没有实现 Python load_all 验证器已有的字符↔组成员双向一致性和 rank 一致性检查。
- ESP 上 SOB1/SPY1 owned copy 仅使用 MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT；View 的 SPY1 绑定失败不会解除已经成功绑定的 SOB1。
- MQTT 声明不支持 correlated/stroke voice，点击入口走 TopRanked 本地候选且不打开音频通道；WebSocket 声明支持并在 STT 后使用 pinyin provider。
- 保留证据显示 host 110/110 通过，IDF v6.0.2 CoreS3 全量构建通过；xiaozhi.bin 为 2,907,712 bytes，generated_assets.bin 为 2,877,288 bytes，app 分区剩余 1,221,056 bytes。

<!-- pi-squad:808fa68b45de73e36849cf3e333fe8391c9a2f02f21e9392c773e3b6367e01a0 -->
## 2026-09-12T13:06:18.684Z — stroke-500-assets-pinyin-rereview

# stroke-500-assets-pinyin 复审报告

**Verdict: APPROVED**

本次按只读要求核对了真实工作区中的最终 `prototype_500`、打包脚本、transcription selection、Python/C++ SPY1 parser、专项测试、CMake 接入、NOTICE/文档，以及 Lead run `27a915ed-8598-4435-bcb5-fe32537f49ee` 的保留命令输出。原 B1、M1–M4 和 CSV minor 均已闭环，未发现新的阻断问题。

**批准范围：允许继续生成最终 500 字技术原型固件包并烧录到 M5Stack CoreS3。** 此批准不等于商业发布、数据授权或 500 字逐字笔顺准确性认证；烧录后仍须完成下列真机重点验证。

## 严重度

- **Blocker：无。**
- **Major：无。**
- **Minor：无新的代码缺陷。** 剩余事项均是真机验证或法律/内容审核边界，不阻止内部原型打包与烧录。

## 原问题逐项复核

### B1 — 映射后 14 文件包与 `SHA256SUMS`：关闭

- `scripts/package_stroke_order_prototype.py:24-58` 定义 14 个源/目标文件，其中 `.sob1/.spy1` 映射为运行时 `stroke_order.bin`、`stroke_pinyin.bin`。
- `verify_checksum_directory()`（约 66–102 行）要求目录实际集合严格等于 13 个 payload 加 `SHA256SUMS`，拒绝 missing、extra、重复清单项、malformed/path 型名称、自身引用、非普通文件和符号链接。
- 打包流程在复制映射后的 13 个 payload 后重新生成清单，再在 staging 发布前验证；不再复制引用旧 `.sob1/.spy1` 名称的源清单（约 164–174 行）。
- 真实保留目录 `/private/tmp/xiaozhi-stroke-500-revision-20260912-205811/build/stroke_order_assets/` 当前可见恰好 14 个文件；其清单含 13 条，引用 `.bin` 名称且不含自身。
- 保留日志显示严格验证为 `validated 14 final files; 13 non-self checksum entries`，随后 `shasum -a 256 -c SHA256SUMS` 的 13 项全部 `OK`；映射后清单自身 SHA-256 为 `dbba16e352851188de77545c45db9eaeb1577e7e4261d6ae95c7b3ab115fb70a`。
- `scripts/tests/test_stroke_order_pinyin.py:477-544` 在真实临时目标目录检查完整 14 文件集合及 `.bin` 名称，并分别构造 missing、extra、duplicate、self 四类失败用例。

### M1 — transcription provenance：关闭

- `scripts/stroke_order/select.py:27-36` 固定 origin、40 位 HEAD、tracked path 和 JSON SHA-256：
  - origin：`https://github.com/leonsilicon/table-of-general-standard-chinese-characters`
  - HEAD：`f9786a82be6e1672bdc60f85760a9e4a3791d1f1`
  - path：`table-of-general-standard-chinese-characters.json`
  - SHA-256：`a9f0a21fb83a84dd695eaeec7b77743a6099ca559768c8275ae0c22c827917d0`
- `_verify_transcription_checkout()`（82–134 行）验证文件位于 checkout 内且路径精确匹配、origin 匹配、HEAD 精确匹配、`git show HEAD:<path>` 存在且与 workspace bytes 完全相同、固定 SHA-256 相同，并用 `status --porcelain=v1 --untracked-files=all` 要求工作树完全 clean。
- 生产 API `select_official_500(table_json)`（162 行）不再接收调用者自报的 transcription commit，始终走上述固定值；可参数化 helper 为私有测试 seam。
- `stroke_order.src.json` 实际记录 `checkout_clean: true`、完整 commit、固定 path/hash、origin，以及“无显式许可证、仅作 prototype transcription aid、不是官方数字附件或商业授权来源”的边界。
- 测试（`test_stroke_order_pinyin.py:185-266`）覆盖 wrong HEAD、wrong origin、untracked/dirty、workspace mutation，并在设置真实 pinned checkout 环境变量时重建 selection/charset 后逐字节比较。保留最终测试日志中该用例是 `ok` 而非 skipped；重生成日志还显示 14 个文件全部 `MATCH`，其中 selection/charset byte-identical。

### M2 — C++/Python SPY1 canonical payload 与双向 membership/rank：关闭

- C++ `StrokeOrderPinyinIndex::BindView()` 在 `main/stroke_order/stroke_order_pinyin.cc:203-314` 从 index 末尾维护唯一 `payload_cursor`：readings 必须按 char-index 顺序紧密排列，随后 members 必须按 group-id 顺序紧密排列，并要求最终 cursor 精确等于 EOF。因此 alias、gap、overlap、trailing/unreferenced bytes 都会失败。
- C++ 从约 318 行起执行两向语义校验：char 引用的 group 必须含同 codepoint/同 rank；group member 必须存在于 char index、rank 相同且 char 反向引用该 group。char ranks 也在约 204–228 行强制唯一。
- Python `validate_spy1()` 在 `scripts/stroke_order/pinyin.py:438-547` 使用相同 u32 checked math、canonical cursor 和双向 membership/rank 规则；生成器及 `StrokePinyinBlob.bind()` 均以 `load_all=True` 调用。
- 文档 `docs/stroke-order-data.md:239-248` 已将 canonical 紧密布局、双向一致性、overflow-safe、unaligned 输入明确为 SPY1 v1 契约。
- C++ 新校验没有引入 heap 容器或无界分配；除既有 `Bind()` 的 bounded owned blob 外，只使用固定上限栈数组和受 `1024 chars / 8 readings / 1024 groups / 32 members / 64 KiB` 限制的循环与二分查找。

### M3 — unaligned、CRC-valid corruption、overflow 测试：关闭

- C++ harness 在 `scripts/tests/stroke_order_pinyin_harness.cc:141` 直接调用 `TestOnlyBindView(unaligned.data() + 1, golden.size())`，确实绕过 owned-copy 对齐并验证不对齐视图。
- mutation 表覆盖 char/group count、group/readings/members offset、reading/member count、header/char/group/member reserved、重复 rank、readings/member overlap、offset add overflow、char→group 不一致、member 缺失、rank 不一致；每个结构修改后分别调用 `RefreshHeaderCrc` 或 `RefreshBodyCrc`，不会只停在 CRC 前置检查。
- 另构造 canonical 仍成立但 group→char 缺反向引用的 payload，以及带 trailing byte 的 payload，均重算 body CRC 后拒绝。
- `TestOnlyAddU32`/`TestOnlyMulU32` 直接暴露生产实现给测试，表驱动覆盖正常值、u32 add overflow、u32 mul overflow；Python 也使用并测试 `u32_add/u32_mul`。
- host C++ harness 使用 C++17、`-Wall -Wextra -Werror` 和 UBSan，合法 114-byte handwritten golden、unaligned view、corruptions 及完整 500 字 SOB1/SPY1 都在保留日志中通过。

### M4 — NOTICE 日期、版本和法律边界：关闭

实际 `prototype_500/NOTICE.md:28-44` 明确包含：

- 修改日期 `2026-09-12`；
- converter `stroke-order-converter/1`；
- schema `SOB1 v1 + SPY1 v1`；
- flatten、round、y-flip、clamp、downsample 的转换说明；
- prototype / not a release / not official certification / not commercially reviewed；
- transcription aid 无显式许可证、尚未完成双人逐页和 500 字逐字审核；
- 外部或商业分发前仍需 APL 法务审核，NOTICE 不构成法律意见或授权结论。

`docs/stroke-order-data.md:45-53, 178-206` 同步记录这些边界，完整 `ARPHICPL.TXT` 与 `UNICODE-LICENSE.txt` 均在 fixture 和最终映射包中。

### CSV minor — codepoint 文本真实验证：关闭

- `load_selection_csv()`（`select.py:252-283`）现在解析 `U+` 十六进制文本为整数，检查格式、单字符和 `ord(character)` 一致，再写入 `codepoint_int`。
- `verify_selection_rows()`（约 194–230 行）再次独立解析并比对文本 codepoint。
- `test_selection_csv_rejects_mismatched_text_codepoint` 实际篡改首行 codepoint 并断言拒绝；最终测试通过。

## 500 字数据与容量不变量

真实 fixture 目录恰有 14 个文件；`selection-500.csv` 可见编号从 0001 连续到 0500，charset 顺序相同。源码测试及保留日志确认 500 个唯一 Basic CJK：

- selection SHA-256：`2ae037c4ecf9cbaa4fcba62284bb333a523d6abb9ce6894fc004234b34400f02`
- charset SHA-256：`41fffff9dd68473ce15fae52d40042271a6311ff573ffa1ecc405e5d0de77b58`
- HWD coverage：500/500，0 error；pinyin coverage：500/500
- SOB1：1,046,788 bytes，SHA-256 `3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`；上限仍为 1,048,576 bytes，余 1,788 bytes
- SPY1：18,152 bytes，SHA-256 `a97666f18fa8e5a5e586f774dac044c08ba26b40decb4644e12c6886a5cbc998`；上限仍为 65,536 bytes，余 47,384 bytes

这些 selection/charset/SOB1/SPY1 hash 与原审查记录一致；本次未替换字符集合或放宽格式上限。manifest、fixture `SHA256SUMS` 和保留 `shasum` 输出对 SOB1/SPY1 hash 相互一致。

相同 sdkconfig（双方 SHA-256 均为 `4fe18823c9183f2b4065af37a2d2cec89499858cd95208fb91887dd03a8e441e`）下，三字 `generated_assets.bin` 为 1,678,548 bytes，500 字版为 2,877,806 bytes，净增 1,199,258 bytes。

## 保留验证证据

本复审没有运行 shell、测试或构建；以下是对 Lead 保留日志的核验，并已与当前源码/fixture/保留构建目录交叉检查：

1. 固定输入重新生成返回 500 characters、SOB1/SPY1 上述大小与 hash；随后 14/14 fixture 文件逐一显示 `MATCH`。
2. 专项 suite 为 13/13 PASS；最终 symlink hardening 后又运行完整 suite，结果 `Ran 113 tests in 13.639s`、`OK`，其中包含同一 13 项专项测试及 `async_cancel_sources_fence`、`audio_stream_generation_rejects_stale_inflight_work`、`round_route_fake_protocol_audio_and_ordinary_bypass`。
3. `clang-format 19.1.7 --dry-run -Werror` 对三份改动 C++ 文件通过；相关 Python 文件 `py_compile` 通过；`git diff --check` 返回 `FINAL_STATIC_CHECKS_OK`。注意该工作树有大量既有未跟踪文件，因此 `git diff --check` 本身不能覆盖未跟踪文件，但专项 formatter/compiler/tests 已覆盖本修订的 C++/Python 实现。
4. IDF 环境明确为 ESP-IDF v6.0.2；在新建仓库外路径 `/private/tmp/xiaozhi-stroke-500-revision-20260912-205811/build` 完成 CoreS3 2214-step build。配置实际含 `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y`、`CONFIG_SPIRAM_MODE_QUAD=y`、16 MiB flash 与 `partitions/v2/16m.csv`。
5. 首次 clean build 后，最后一次 packager symlink hardening 又触发 `[4/7] Packaging reviewed...` 和 `[6/7] Building default assets.bin`；日志显示 `Processed 14 extra files`、`generated_assets.bin` 2,877,806 bytes、最终 `Project build complete`。
6. 最终产物证据：`xiaozhi.bin` 2,908,800 bytes / SHA-256 `911bebfc91cefe3a1e4c7e0e5308d380eca90e119182924bfc6f1d16d367a479`；`generated_assets.bin` 2,877,806 bytes / `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`；app 分区余 1,219,968 bytes，assets 分区余 5,510,802 bytes。`flash_args` 包含 app `0x20000` 和 assets `0x800000`。

## Session / transport 回归

本轮修订路径限定在 packager/generator/select/SPY1/parser/tests/docs/fixture，没有修改 `main/application.*`、`main/audio/*` 或 `main/protocols/*` 的生产逻辑。最终 113/113 中相关安全回归均通过；当前源码仍明确要求 WebSocket 同时支持 correlated open 和 stroke voice，而 MQTT 的 `SupportsCorrelatedSessionOpen()`、`SupportsStrokeVoiceRouting()` 均为 false。没有发现 session identity、cancel fence、stale audio generation、普通对话 bypass 或 MQTT no-voice 的回退。

## 真机重点与剩余边界

烧录后重点检查：

1. 冷启动时 SOB1/SPY1 两份 owned copy 均实际落在 PSRAM；记录 free/largest block，验证约 1,064,940 bytes 数据复制不会造成启动失败或严重碎片。
2. 注入低 PSRAM、缺失/损坏 SPY1：必须 fail closed、不得回落内部 DRAM；SPY1 失败仅退化为 exact-only/fallback，SOB1 与普通助手保持可用；覆盖 Assets suspend/rebind。
3. 抽样一画字、高笔画字、折/钩/多部件字，以及多个多音字；确认识别字优先、候选去重/按 rank、最多 6 个，笔顺与最终字形人工核验。
4. 真正 WebSocket STT 验证 round identity、断线和迟到回包隔离；MQTT 只显示本地 TopRanked 页面且绝不打开 voice。
5. 验证 overlay 独占触摸、每个触摸序列最多一次动作、关闭后原短触恢复；动画目标约 30 FPS、持续不低于约 20 FPS，且无 WDT、音频 underrun。
6. 连续至少 100 次进入/语音选字/播放/中断/退出，确认 heap、largest block、timer/task 无持续下降或泄漏。

最终结论仅适用于 **CoreS3 本地 500 字技术原型的打包与烧录验证**。该包不是官方逐字认证字库，尚未完成 500 字人工笔顺审核；transcription aid checkout 无显式许可证。任何对外或商业分发前仍须完成 Arphic Public License、transcription aid 和 Unicode 材料的法务审核。

- 最终映射后的 CoreS3 stroke-order 资产目录恰有 14 个文件；其 `SHA256SUMS` 对另外 13 个文件各引用一次，使用 `.bin` 运行时名称且不自引用，保留 `shasum -c` 输出为 13/13 OK。
- 生产 selection 路径固定 transcription origin、40 位 HEAD、tracked JSON path 和 SHA-256，并验证 workspace bytes 等于 HEAD blob、工作树含 tracked/untracked 均 clean；真实 pinned 输入重建的 selection/charset 与 fixture byte-identical。
- SPY1 v1 的 Python 与 C++ validator 都要求 readings 后接 members 的无 gap/overlap/alias/trailing canonical payload，并验证 char↔group 双向 membership 和 rank 一致；C++ 新语义检查没有引入动态容器。
- C++ harness 直接用 `TestOnlyBindView(buffer+1,size)` 覆盖未对齐输入；结构 corruption 在重算相应 CRC 后覆盖 count、offset、reserved、overlap、双向语义与 u32 add/mul overflow。
- 当前 prototype 保持 500 个唯一 Basic CJK，HWD 和 pinyin coverage 均为 500/500；SOB1 为 1,046,788 bytes、SPY1 为 18,152 bytes，分别保持 1 MiB 与 64 KiB 上限。
- 保留日志的最终 host suite 为 113/113 PASS；IDF v6.0.2 CoreS3 新 build 成功，最终 packager 修改后再次重建 14-file `generated_assets.bin`。
- 最终 `xiaozhi.bin` 为 2,908,800 bytes、SHA-256 `911bebfc91cefe3a1e4c7e0e5308d380eca90e119182924bfc6f1d16d367a479`；`generated_assets.bin` 为 2,877,806 bytes、SHA-256 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`。
- 本修订未触碰 session/audio/transport 生产路径，且最终安全回归覆盖 cancel fence、stale audio generation、round route、普通对话 bypass 和 MQTT no-voice。
- 当前数据包明确是 prototype、非官方逐字认证且未完成商业法务审核；transcription aid checkout 无显式许可证。

<!-- pi-squad:33bd83c8a4fc2d82dca4fe10b9df28db2deb222707bbb4a9d2da571c8e18270d -->
## 2026-09-12T13:22:51.441Z — stroke-final-core-s3-500-review

# stroke-final-core-s3-500 独立最终审计

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

- 最终 113 项 host suite 为 112 pass、1 conditional skip；该 skip 只因缺少 `STROKE_TRANSCRIPTION_JSON`，而保留的 `stroke-500-revision-host-final2.log` 已证明同一测试在 pinned checkout 下非 skip 且整套 113/113 通过。
- ESP-IDF v6.0.2 外部构建目录完成 2214-step CoreS3 构建，配置启用 16 MB flash、PSRAM、GC0308 与 `CONFIG_STROKE_ORDER_LOCAL`，并实际编译全部指定核心单元。
- 最终包有 26 个普通文件；顶层 checksum 覆盖另外 25 个文件，内层 14 文件集合的 checksum 覆盖另外 13 个文件；目录、ZIP CRC 和解压后 checksum 均有通过证据。
- SOB1 为 1,046,788 bytes、SHA-256 `3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`，距 1 MiB 上限仅 1,788 bytes；SPY1 为 18,152 bytes、SHA-256 `a97666f18fa8e5a5e586f774dac044c08ba26b40decb4644e12c6886a5cbc998`。
- ZIP 的保留 SHA-256 为 `9fd6dfcb7e04ece3732381385ef4a4620b721f00806c3c8227216e0e60c86a5a`；merged 的 SHA-256 为 `4a22047b840bd3459cb1ad4fa158567a1e3be2927afd23413df64eca09964fbb`。
- esptool raw `merge_bin` 用 `0xFF` 填充输入镜像之间的空洞，而 `write_flash` 对单个 merged 文件按连续长度擦除；因此从 `0x0` 写入当前 11,266,414-byte merged 镜像会覆盖位于 `0x9000` 的 NVS 分区。
- 包内没有 NVS payload、sdkconfig、私钥或临时日志；“未携带凭据”不等于“merged 烧录会保留设备原有 NVS”。
- WebSocket 路径启用关联式语音选字；MQTT/UDP 明确禁用 stroke voice 并使用本地 TopRanked 候选，README 对运行行为的这部分描述与源码一致。

<!-- pi-squad:923d7bd14b893d234d29507cf6ec2e65775752aca04bdd42adc49cac2492900b -->
## 2026-09-12T13:27:45.885Z — stroke-final-core-s3-500-doc-fix

已完成 `stroke-final-core-s3-500` 同一 package artifact 的限定修订，仅改动 README、manifest、顶层 SHA256SUMS 与 ZIP；未修改源码/固件二进制，未重编译，也未 commit/stage/reset/clean。README 现将 `write-flash @flash_args` 设为已配置设备保留 NVS 的首选，明确两种方式严格二选一，并说明 merged 从 0x0 连续写入时会以 0xFF padding 清除 0x9000–0xcfff 的 Wi-Fi/激活/NVS，`erase-flash` 不是唯一清 NVS 方式。manifest 已增加对应机器字段、用途和风险。最终 SHA-256/大小：README `b75dd1d60d5cb1d3de66c335b6a92d409f85a6640d4485c39d0515593ba7a364` / 8,149 B；manifest `5fb0f547e8a4e63a7efcecb7a4d92907e0ff1f8725d04d646c6468e8b1727635` / 17,303 B；SHA256SUMS `d51b79a25e9932ae1c430fc1eb8dcf9b950dc94091dd11ffac0cc565cc593990` / 2,299 B；ZIP `bdbc8c589db291d4ace0de49e093dab87079fb5d6e955c36a104227cf410371e` / 23,931,458 B。实际验证：目录 checksum 25/25 OK；`unzip -t` 26/26 OK；解压后 checksum 25/25 OK；`diff -qr` identical；修改前后其余 23/23 文件 hash 不变；merged NVS 范围 16,384/16,384 字节均为 FF；JSON 与语义断言通过。完整报告已写入指定输出路径。残余风险：未做本轮真机烧录；多文件方式虽不主动写 NVS/phy，仍改写 bootloader、分区表、OTA data、app 与 assets，仍存在布局不兼容、断电和旧持久状态兼容风险。修正后的首选命令为 `python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 --before default-reset --after hard-reset write-flash @flash_args`。

- `merged-binary.bin` 的 SHA-256 仍为 `4a22047b840bd3459cb1ad4fa158567a1e3be2927afd23413df64eca09964fbb`，大小 11,266,414 字节；其 `0x9000:0xd000` 全部为 0xFF，因此从 0x0 写入会清除既有 NVS。
- `flash_args` 仅列出 0x0 bootloader、0x8000 partition table、0xd000 OTA data、0x20000 app、0x800000 assets，不含 0x9000 NVS 或 0xf000 phy_init 输入。
- 顶层 SHA256SUMS 有 25 条并覆盖目录内除自身外全部文件；ZIP 有 26 个文件条目，解压内容与源目录逐字节一致。
- 修改前后所有 23 个非授权变更文件 hash 完全一致，包括全部八个 `.bin` 固件/资产镜像与 `xiaozhi.elf`。

<!-- pi-squad:5ec1fc510e55c79a7d61951225500205c3879910eea085d8eb6dc634661edef5 -->
## 2026-09-12T13:56:30.983Z — stroke-websocket-preference-review

# stroke-websocket-preference 独立审查

**Verdict: APPROVED**

本审查只读完成：读取了真实工作区源码、测试 harness、500 字资产元数据，以及作者 run `d85b3d30-db77-4438-b4a7-79f6f57e2766` 的保留命令输出；我没有修改文件，也没有自行运行 shell、测试或构建。代码实现与保留的 red/green、host suite、格式检查和 IDF 构建证据一致，可以进入 500 字包重打与 M5Stack CoreS3 真机多文件烧录阶段。

## Findings

### Blocker

无。

### Major

无。

### Minor

1. **范围说明：仓库原有 WebSocket 连接日志仍打印 URL。** 本次新增的 `Application::InitializeProtocol` 两条诊断日志只打印 MQTT/WS 配置是否存在、stroke preference、所选 transport 及两个 capability bit，不打印 endpoint/token，满足本 artifact 对新增日志的要求。但 `main/protocols/websocket_protocol.cc:237` 既有日志仍为 `Connecting to websocket server: %s` 并传入 `url.c_str()`；它不是本轮新增或修改的代码，也不影响本轮选择/session 正确性。若“不泄露 endpoint”被解释为全仓库日志策略，而非本轮新增选择日志的约束，建议在对外收集串口日志前另行脱敏。

## 代码审查

### 1. `SelectProtocolTransport` 矩阵完整且策略正确

`main/protocols/protocol_selection.h:18-29` 的实际顺序为：

1. `prefer_websocket_for_stroke_voice && has_websocket_config` → WebSocket；
2. 否则有 MQTT → MQTT；
3. 否则有 WebSocket → WebSocket；
4. 都无 → MQTT fallback。

`scripts/tests/protocol_selection_harness.cc:26-43` 覆盖三项布尔输入的全部 8 种组合：

| MQTT | WS | stroke pref | 结果 |
|---|---|---|---|
| 1 | 1 | 1 | WebSocket |
| 1 | 1 | 0 | MQTT |
| 1 | 0 | 1/0 | MQTT |
| 0 | 1 | 1/0 | WebSocket |
| 0 | 0 | 1/0 | MQTT |

因此：

- 只有 `CONFIG_STROKE_ORDER_LOCAL` 令 preference 为 true；WS 可用时优先 WS。
- 非 stroke 构建 preference 为 false，双配置仍保持历史 MQTT-first。
- MQTT-only、WS-only 和 no-config fallback 均保持预期行为。

### 2. Application 确实走生产 helper

`main/application.cc:9` 引入 `protocol_selection.h`；`Application::InitializeProtocol()` 在 `main/application.cc:595-617` 读取两个 OTA capability，按 `#if CONFIG_STROKE_ORDER_LOCAL` 设置 preference，调用 `SelectProtocolTransport()`，并依据返回值构造唯一的 `WebsocketProtocol` 或 `MqttProtocol`。无配置时原有 `using MQTT` 警告仍保留。

新增选择日志只含：

- `mqtt_config` / `websocket_config` 布尔值；
- `stroke_local_pref`；
- `selected` 的固定枚举名；
- `correlated_open` / `stroke_voice` capability bit。

作者构建后的 `xiaozhi.bin` 中也检出了这两条格式串。`InitializeProtocol` 新增代码没有读取或打印 endpoint/token。

### 3. capability 与 session 安全边界未放宽

真实源码仍为：

- `MqttProtocol::SupportsCorrelatedSessionOpen() == false`；
- `MqttProtocol::SupportsStrokeVoiceRouting() == false`；
- `WebsocketProtocol` 对两者均返回 true；
- `Application::StrokeVoiceRoutingAvailable()` 仍要求两个 capability 同时为 true。

作者保留的精确 edit patch显示，本轮对 `application.cc` 只增加 include 并替换 `InitializeProtocol` 的 transport 选择/诊断片段；没有修改之后的音频、STT 或 session routing，也没有修改 MQTT/WebSocket capability 和协议实现。

现有安全路径仍成立：WebSocket 使用单调 `channel_generation_` 预留 open attempt，把该次 hello 的 session identity 固定捕获到回调；Application 对 stroke open 要求非零 attempt ID，绑定返回的 session ID，对缺失/错误/过期 identity 执行 abort。选择 WebSocket 后失败不会转而伪造 MQTT identity，也没有新增 WS→MQTT 动态回退。

### 4. red-capable 测试确实先红后绿

保留会话按时间顺序证明：

- 修复前先写入 MQTT-first 的生产 helper 与同一 harness；
- 执行：
  `python3 -m unittest scripts.tests.test_protocol_selection.ProtocolSelectionTest.test_production_helper_matrix_and_stroke_websocket_preference -v`
- 结果为 exit 1，唯一 harness 失败：
  `FAIL: both configs with stroke preference must select websocket, not mqtt`
  `protocol_selection_harness: FAIL (1)`；
- 随后才把 helper 改为 WS-preference 并接入 Application；
- 执行 `python3 -m unittest scripts.tests.test_protocol_selection -v`，结果 `Ran 3 tests ... OK`。

该测试直接编译并执行生产 `protocol_selection.h`，不是仅复制一份测试逻辑；旧的双配置+stroke preference 行为确实会被它捕获。

## 验证证据

### Host tests

作者保留输出：

```text
python3 -m unittest discover -s scripts/tests -v
Ran 116 tests in 15.386s
OK (skipped=1)
```

即 115 pass、1 个有条件的预期 skip（需要 `STROKE_TRANSCRIPTION_JSON`）、0 fail/error。输出包含新增 3 项 protocol-selection 测试以及既有 stroke round/session 集成测试。

### Format / diff

保留命令与结果：

- clang-format 版本：`19.1.7`；
- 对 `main/protocols/protocol_selection.h`、`main/application.cc`、`scripts/tests/protocol_selection_harness.cc` 执行 format 与 `--dry-run -Werror`：`FORMAT_OK`；
- scoped `git diff --check`：`DIFF_CHECK_OK`；
- 后续全工作区 `git diff --check`：exit 0。

### ESP-IDF v6.0.2 CoreS3 clean build

作者先删除并重建仓库外目录 `/private/tmp/xiaozhi-stroke-websocket-preference-build`，使用独立 sdkconfig 和 ESP-IDF v6.0.2。实际配置包含：

- target `esp32s3`；
- `BOARD_NAME=m5stack-core-s3`；
- `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`；
- `CONFIG_STROKE_ORDER_LOCAL=y`；
- 16 MiB flash、`partitions/v2/16m.csv`、quad PSRAM。

结果为 2214-step fresh build，真实编译 `application.cc`、MQTT、WebSocket、CoreS3 board 和全部 stroke-order 源文件，完成链接、镜像生成及分区检查：

```text
xiaozhi.bin binary size 0x2c6310 bytes
Smallest app partition 0x3f0000 bytes
0x129cf0 bytes (30%) free
Project build complete
```

### 500 字资产与固件 hash

构建日志确认执行 `Packaging reviewed 500-character stroke-order prototype (not a release library)`，manifest 为 `character_count=500`、`not_a_release_library=true`，且来源 commit 为 `68d10a4b21150cae5e1ebbd223eed289cf32d90c`。

- `xiaozhi.bin`: 2,908,944 bytes, SHA-256 `f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a`
- `generated_assets.bin`: 2,877,806 bytes, SHA-256 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`
- `stroke_order.bin`: 1,046,788 bytes, SHA-256 `3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`
- `stroke_pinyin.bin`: 18,152 bytes, SHA-256 `a97666f18fa8e5a5e586f774dac044c08ba26b40decb4644e12c6886a5cbc998`

工作区 fixture 的 `SHA256SUMS` 与构建输出中的 stroke/pinyin hash 一致。

## 放行与真机边界

**允许重打 500 字原型包，并允许在 M5Stack CoreS3 上按多文件布局烧录进行真机验证。** 保留构建给出的布局为 bootloader `0x0`、partition table `0x8000`、OTA data `0xd000`、`xiaozhi.bin` `0x20000`、`generated_assets.bin` `0x800000`；实际操作仍应以同一构建目录的 `flash_args` 为准。该 500 字资产仍标记为 prototype / `not_a_release_library`，本批准不等于商业发布授权或逐字官方认证。

烧录后的关键验收日志应为：

```text
mqtt_config=1 websocket_config=1 stroke_local_pref=1 selected=websocket
correlated_open=1 stroke_voice=1
```

随后点击 SO 应进入语音等待/远端 STT，而不是直接显示本地候选。

**若线上 OTA 没有下发 WebSocket 配置，设备仍会安全本地降级：** `websocket_config=0` 时 helper 选择 MQTT，capability 为 false/false，`BeginStrokeRoundFromMain()` 会进入 `BeginLocalStrokeCandidatesFromMain()`。这属于设计行为；此时应检查 OTA 服务端配置，不应放宽 MQTT capability 或伪造 per-open session identity。

未完成且不能由本次只读审查替代的验证：真实 OTA 是否下发 WebSocket、真机 WebSocket 连接/STT、触摸交互及音频长稳。

- `SelectProtocolTransport` 的 8 格输入矩阵已由编译运行生产头文件的 host harness 全覆盖；只有双配置且 stroke preference=true 时改变历史行为为 WebSocket，其余组合保持 MQTT-first、WS-only 或 MQTT fallback。
- `Application::InitializeProtocol` 仅在 `CONFIG_STROKE_ORDER_LOCAL` 下把 WebSocket preference 设为 true，并真实使用 helper 的返回值构造协议对象；新增诊断日志只包含配置存在性、transport 名和 capability bits。
- MQTT 的 correlated-open/stroke-voice capability 仍为 false/false，WebSocket 仍为 true/true；`StrokeVoiceRoutingAvailable()` 继续取两者逻辑与，既有 generation/session 校验和失败 abort 路径未被本 artifact 修改。
- 保留运行证据显示 red 测试在旧 MQTT-first 双配置路径上以唯一预期断言失败，修复后 3 项专项测试和 116 项全量 host suite 通过（1 expected skip）。
- ESP-IDF v6.0.2 在全新仓库外目录完成 CoreS3 + `CONFIG_STROKE_ORDER_LOCAL=y` 的 2214-step 构建；`xiaozhi.bin` 为 2,908,944 bytes，SHA-256 `f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a`。
- 500 字 `generated_assets.bin` 为 2,877,806 bytes，SHA-256 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`；manifest 明确为 500 字且 `not_a_release_library=true`。
- 若 OTA 没有 WebSocket 配置，本地 stroke 构建仍选择 MQTT 并直接显示本地候选；这是安全降级，不应通过放宽 MQTT session capability 来规避。
- 仓库既有 `WebsocketProtocol::OpenAudioChannel` 会记录 WebSocket URL；本 artifact 的新增日志不记录 endpoint/token，但若要求全局隐藏 endpoint，应另行脱敏该既有日志。

<!-- pi-squad:b435f389a196dc0f90cc2eb42e88ca9df4ebdad33250e811a9b861e5fb70e96f -->
## 2026-09-12T14:01:53.060Z — stroke-final-core-s3-500-websocket-refresh

已完成 `stroke-final-core-s3-500` dist-only 刷新，未改功能源码、未重编译、未烧录，也未执行 commit/stage/reset/clean。使用 approved clean build `/private/tmp/xiaozhi-stroke-websocket-preference-build` 更新 app/ELF/map/bootloader/merged，并核对 flash_args、partition、OTA、assets。README/manifest 已补充双配置优先 WebSocket、真机期望 `selected=websocket`/`stroke_voice=1`、OTA `websocket_config=0` 时安全 MQTT 本地降级，以及保留 NVS 的 `@flash_args` 与 merged 清除 NVS 说明。最终 ZIP SHA-256 `dc6524722341f93cb5335117caa10abba383ba2603722853097fa203edb46a15`（24,165,514 bytes）；merged `e23e291907d14d14a1ee847b340793cf009f0d8ad4650d88d05b5ef8e1ccb039`（11,266,414 bytes）；app `f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a`（2,908,944 bytes）。完整报告已写入 `/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/73eff2b9-edef-4b77-82e1-0937485db697/stroke-final-core-s3-500-websocket-refresh.md`。

- 指定 build 的 `project_description.json` 为 ESP-IDF v6.0.2、target `esp32s3`；sdkconfig 含 `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y` 与 `CONFIG_STROKE_ORDER_LOCAL=y`。
- Approved app SHA-256 为 `f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a`，大小 2,908,944 bytes。
- `generated_assets.bin` SHA-256 仍为 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`，大小 2,877,806 bytes；`stroke_order/` 的 14 个数据/许可文件刷新前后 hash 全部不变。
- esptool v5.3.1 image-info 验证包内 bootloader、app、merged 为 ESP32-S3 / DIO / 80 MHz / 16 MB，镜像 checksum 与 validation hash 有效。
- merged 在 0x0、0x8000、0xd000、0x20000、0x800000 的内容分别与 bootloader、partition、OTA、app、assets 逐字节一致；0x9000–0xcfff 是 0xFF，故从 0x0 写 merged 会清除 NVS。
- 顶层 SHA256SUMS 25/25、内层 SHA256SUMS 13/13 均通过；`unzip -t`、`zip -T`、解压后 checksum 与 `diff -qr` 全部通过，目录和 ZIP 均有 26 个普通文件。
- 保留 NVS 的首选命令是从解压目录执行 `python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 --before default-reset --after hard-reset write-flash @flash_args`；该参数集不含 NVS/phy_init 镜像。
- 本次未进行真机验证；双配置 WebSocket 选择、`stroke_voice=1`、MQTT fallback、PSRAM、触摸和长期稳定性仍需硬件验证。

<!-- pi-squad:b92b8cbd512613713cce75a14d6d6a77fb5561c3284a758373d0414c5351730d -->
## 2026-09-12T14:15:23.976Z — stroke-final-core-s3-500-capability-doc-rereview

# stroke-final-core-s3-500 capability 文档最终只读复审

## Verdict: APPROVED

未发现阻断项。当前 `dist/m5stack-core-s3-stroke-500-prototype/README.zh-CN.md`、`manifest.json`、两级校验表及保留的打包验证证据满足本轮要求。

**在本次复审范围内，以下精确 ZIP 标记为 `stroke-final-core-s3-500` 最终 500 字原型包：**

- 路径：`dist/m5stack-core-s3-stroke-500-prototype.zip`
- SHA-256：`6cb9ecfe8b3eadedb14a4cee3f90633a5df87c0bf199998184ac37ba2ee91a10`
- 大小：`24,167,765` bytes
- ZIP 条目：30（26 个普通文件、4 个目录条目）

这里的“最终”仅指本轮 500 字原型 package artifact；不表示完成商业发布授权或 CoreS3 动态真机验收。

## 1. capability 说明核对

- README 现明确双配置、本地笔划构建选择 `selected=websocket`，并明确初始化日志中的 `selected=websocket`、`correlated_open=1`、`stroke_voice=1` 在 `protocol_->Start()` **之前**产生，只表示静态 transport capability。
- README 明确这些字段不能证明 WebSocket connection、audio channel open、hello 返回本轮合法 `session_id` 或 STT 成功；现有两行初始化日志不足以作为动态验收证据。
- 当前源码也支持该表述：`main/application.cc` 先在约 607、620 行打印选择与 capability，再在约 997 行调用 `protocol_->Start()`；`WebsocketProtocol` 的两个 capability 方法静态返回 true，而 MQTT 对应方法静态返回 false。
- README 的真机标准已改为：点击 SO 后实际进入 Listening，收到携带本轮合法 `session_id` 的 STT，并显示候选字；若使用串口日志，必须提供覆盖动态链路的证据。
- `manifest.json` 已删除错误字段 `stroke_voice_one_requires_correlated_websocket_open`。当前真实文件中无该字段。
- `manifest.json` 新增并正确设置：
  - `logged_before_protocol_start=true`
  - `static_capability_only=true`
  - `does_not_prove_connection=true`
  - `does_not_prove_open=true`
  - `does_not_prove_session=true`
  - `does_not_prove_stt=true`
  - `existing_static_logs_sufficient=false`
  - `so_enters_listening=true`
  - `stt_has_valid_session_id=true`
  - `candidates_displayed=true`
- README/manifest 仍说明不修改既有 WebSocket URL 日志，并要求分享日志前脱敏 endpoint/query、token、签名和设备标识等信息。

## 2. 必须保留的行为与边界

- **WebSocket 优先：** `manifest.json` 保留 `local_stroke_dual_config_preference="websocket"`；源码 `SelectProtocolTransport()` 在启用本地笔划且有 WebSocket 配置时优先 WebSocket。
- **MQTT 安全降级：** README 与 manifest 均保留 OTA 无 WebSocket 时选择 MQTT、`stroke_voice=false`、直接使用本地 `SPY1 TopRanked` 候选且不宣称 MQTT 语音选字。
- **NVS 烧录边界：** README 与 manifest 均将多文件 `@flash_args` 标为已配置设备保留 NVS 的首选；真实 `flash_args` 仅列 bootloader、partition table、OTA data、assets 和 app，不含 NVS/`phy_init`。README/manifest 同时明确从 `0x0` 写入 merged raw image 会以 `0xFF` 覆盖 `0x9000–0xcfff` 并清除已有 Wi-Fi、激活信息及其他 NVS 数据。
- **500 字与许可边界：** README、manifest、`stroke_order/NOTICE.md` 和 source-lock 文件继续明确：500 字仅为原型，不是正式发布字库、不是官方逐字认证，也未完成商用授权与逐字笔顺准确性审核；Hanzi Writer Data 固定 commit、Arphic Public License、Unicode 16.0.0/Unicode License v3 及正式发布前审核要求均保留。

## 3. 完整性与不变性证据

当前真实顶层 `SHA256SUMS` 含 25 个被校验文件，内层 `stroke_order/SHA256SUMS` 含 13 个被校验资产文件。关键不变 hash 为：

- `xiaozhi.bin`：`f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a`
- `generated_assets.bin`：`6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`
- `merged-binary.bin`：`e23e291907d14d14a1ee847b340793cf009f0d8ad4650d88d05b5ef8e1ccb039`
- SOB1 `stroke_order.bin`：`3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`
- SPY1 `stroke_pinyin.bin`：`a97666f18fa8e5a5e586f774dac044c08ba26b40decb4644e12c6886a5cbc998`

保留的作者终端输出（run `520f3fc7-b4cd-43aa-8a64-c774d6fb787d`）显示修订后实际执行并通过：

- 顶层 checksum：`TOP_RESULT=25/25`
- 内层 checksum：`INNER_RESULT=13/13`
- README/manifest 之外条目：`UNCHANGED_NON_DOC_HASHES=23/23`
- ZIP CRC：30 个条目全部 `OK`，`No errors detected`
- ZIP 解压后：`EXTRACTED_TOP_RESULT=25/25`、`EXTRACTED_INNER_RESULT=13/13`
- 解压目录与当前 dist：`EXTRACTED_DIFF=IDENTICAL`
- 解压普通文件数：26

保留的修订前后 checksum diff 仅改变 README 与 manifest 两个被校验条目；顶层 `SHA256SUMS` 随之更新。当前真实文件中的 README hash 为 `ab4463176d8480ec34d264ae7bef9d18ea354a2987f707203c1e5e677eb533c5`，manifest hash 为 `f574a87985cf452105390485c2e1d021e35753b099afd8ba8f8777fe0336f4c9`。

## 4. 本次复审方式与剩余风险

本复审严格只读：实际进行了文件读取、目录枚举、文本检索和源码顺序核对；**未运行 shell、checksum、解压、测试、编译或烧录，也未修改文件**。ZIP hash/大小、CRC、解压 checksum/diff 和 23/23 不变性结论来自已核对的保留终端输出，而不是本复审重新执行。

剩余风险已在包内如实保留：本轮没有 CoreS3 真机烧录；WebSocket connection、audio open、hello 合法 `session_id`、STT、候选显示、PSRAM、触摸、动画、FPS、heap/timer、WDT、audio underrun 和 100 次会话长稳仍未完成动态验收。初始化静态 capability 日志不能消除这些风险。

- `main/application.cc` 在创建 transport 后、调用 `protocol_->Start()` 前打印 `selected` 与 `correlated_open/stroke_voice`；WebSocket capability 方法静态返回 true，MQTT 静态返回 false，因此这些初始化字段不是连接、open、session 或 STT 成功证据。
- 最终候选 ZIP 的精确身份为 SHA-256 `6cb9ecfe8b3eadedb14a4cee3f90633a5df87c0bf199998184ac37ba2ee91a10`、大小 24,167,765 bytes；保留验证记录显示 30 个 ZIP 条目 CRC 全部通过且解压内容与 dist 目录一致。
- 修订后的顶层校验覆盖 25 个普通文件、内层校验覆盖除自身外 13 个资产文件；保留验证记录分别为 25/25 和 13/13，通过。
- README/manifest 之外 23 个顶层被校验条目的 hash 保持不变；app、generated assets 和 merged image 的 SHA-256 分别为 `f6ec2f...32f2a`、`6d4032...a4df0`、`e23e29...cb039`。
- 多文件 `flash_args` 不含 NVS 或 `phy_init`，是保留既有 NVS 的首选；从 `0x0` 写 merged image 会清除 NVS 范围，即使包中没有设备导出的凭据。
- 500 字资产明确保持原型边界：不是官方逐字认证或发布字库，未完成商用授权与逐字准确性审核，正式外部/商业发布仍需法律与数据审核。

<!-- pi-squad:c4b83c6af14e4a02f3de13a92ff472456487acb7cd68a841ee8260c105bd68ec -->
## 2026-09-12T14:24:12.022Z — stroke-speak-timeout-test-plan

# SO Speak 15 秒 Timeout：紧凑定位与测试计划

> 只读结论：本次未修改文件，也未运行测试、固件构建或真机命令。`116 tests` 是对当前 `scripts/tests/*.py` 中 `def test_` 的静态盘点，不是本次执行结果。

## 1. 先给结论

当前的 `Speak -> 约 15 秒 -> Timeout` 只能证明：本轮最终进入了 `StrokeRoundCoordinator::Phase::AwaitingSpeech`，并且 15 秒内没有成功提交匹配当前 generation/session 的 STT；它不能证明已经调用或成功发送 `StartListening`，也不能证明麦克风音频已经上行。

更强的一点是：生产代码中 Speech timeout 只在 `AwaitingSpeech` 触发，而该阶段由有效的 `BindOpenedChannel()` 建立。因此，**单纯的 WebSocket/channel open 返回失败通常不会产生这个 15 秒 Timeout**：连接失败或 10 秒 server-hello 超时会走 `StartFailed`/network error 并提前退出。反复稳定出现完整 15 秒 Timeout 时，排查优先级应移到：

1. `StartListeningAudio()` 是否真的执行；
2. `listen/start` 是否真正发送成功；
3. 是否产生并成功发送 Opus 上行包；
4. 服务端是否产生 STT；
5. STT 的 `session_id` 是否匹配、是否被当成 retired/unknown/missing 而拒绝。

但屏幕上的 `Speak` 本身不能作为 open 成功证据：`StartVoiceSessionFromMain()` 在排队打开通道之前就渲染 `Speak`。

## 2. 当前真实路径与可观察性

实际调用链如下：

```text
SO entry click
 -> BeginStrokeRoundFromMain
 -> BeginRound
 -> StartVoiceSessionFromMain -> 屏幕显示 Speak
 -> QueueListeningRequest
 -> Idle -> Connecting
 -> WebsocketProtocol::OpenAudioChannel
 -> server hello + exact session_id
 -> BindOpenedAudioChannel / BindOpenedChannel -> AwaitingSpeech
 -> Listening state
 -> StartListeningAudio
 -> MarkListeningStarted
 -> Protocol::SendStartListening
 -> AudioService::EnableVoiceProcessing(true)
 -> mic -> audio engine -> Opus -> send queue
 -> Application MAIN_EVENT_SEND_AUDIO -> WebsocketProtocol::SendAudio
 -> server STT JSON
 -> CaptureRoute -> CommitStrokeStt -> candidates
```

当前诊断盲点：

| 关口 | 当前已有证据 | 当前缺口 |
|---|---|---|
| 选到 WebSocket | 启动日志有 `selected=websocket`、`correlated_open=1 stroke_voice=1` | `WebsocketProtocol::Start()` 只返回 true，不建立连接；这些日志不证明 WS 可用 |
| channel open | WS 日志有 `Connecting...`、成功 hello 后的 `Session ID`；hello 最长等待 10 秒 | SO 页面仍只显示 `Speak`；`StartFailed` 本身没有专属状态 |
| StartListening | fake harness 会统计一次 `start_listen` | 生产代码无日志；`Protocol::SendStartListening()` 返回 `void`，内部 `SendText()` 的 bool 被丢弃 |
| 音频产生/上行 | `AudioService` 内部有 input/encode 统计字段 | 无公开快照、无 SO 每轮计数；`Application` 对 `SendAudio(false)` 只清空余包，没有定位日志 |
| STT 收到/路由 | missing/wrong 当前会提前 abort 并提示；coordinator 已有严格路由 | 被 SO 拦截的 STT 没有普通 STT 的 `>> text` 日志；retired session 是静默 Drop，仍可能最后 Timeout |
| 15 秒超时 | coordinator 精确检查 15000 ms；屏幕显示 `Timeout` | `AwaitingSpeech` 同时覆盖“已 bind 未 Start”和“已 Start 等 STT”，无法归因 |

另一个关键点：`MarkListeningStarted()` 只重置 speech deadline，并不切换到独立 phase。因此，现有 coordinator 状态本身无法区分“通道已 bind，但 `StartListeningAudio()` 从未执行”。

## 3. 现有 116 项测试盘点

静态计数为：

| 文件 | 测试方法数 | 主要范围 |
|---|---:|---|
| `test_build.py` | 65 | 构建脚本、板卡/Kconfig/CMake/变体 |
| `test_build_default_assets.py` | 2 | 默认字体资产元数据 |
| `test_protocol_selection.py` | 3 | MQTT/WS 选择矩阵和 capability source-shape |
| `test_stroke_order.py` | 24 | SOB1、转换、边界、C++ store harness |
| `test_stroke_order_pinyin.py` | 13 | 500 字、SPY1、来源与 C++ harness |
| `test_stroke_order_ui.py` | 9 | controller/lifecycle/session/round/audio-generation harness |
| **合计** | **116** |  |

与本问题直接相关的最好证据是 `stroke_round_integration_harness.cc`，它已经覆盖：

- fake WebSocket 成功 open 后 `start_listen == 1`、fake microphone enabled；
- Speech timeout 在 15000 ms 不提前并按统一 abort 清理；
- missing/wrong session ID 对 STT/TTS/LLM/audio 为 fail-closed；
- retired STT 被 Drop；
- matching STT 只提交一次；
- MQTT 本地候选降级及普通会话旁路；
- open/cancel/close 的 generation fence 竞态。

但它没有覆盖本次定位所需的关键差异：

- fake `Open()` 总是成功，没有“普通 WS 失败”和“SO fresh-channel open 失败”的对照；
- 没有“open/bind 成功，但刻意不调用 `StartListeningAudio()`”的分支；
- fake microphone enabled 被当成音频链路成功，没有模拟 encode/send packet 数；
- 没有 `SendStartListening()` 的真实返回结果；
- 没有执行生产 `WebsocketProtocol`、生产 `AudioService`、CoreS3 ES7210/I2S 或真实服务端；
- 没有验证诊断文字实际出现在 LVGL 屏幕上。

所以 116 项即使全部通过，也不能区分本次五类故障。

## 4. 最小反馈信号：一行、每 generation 固定有界

建议不扩展 `DeviceStateMachine`，只为当前 SO generation 保存一个固定大小诊断快照：

```text
open_ok / hello_id_bound
listen_called / listen_send_ok
encoded_packets
send_attempts / send_ok / send_fail
stt_seen / stt_route_reason
```

只记录计数、布尔值、generation/open-attempt ID 和单调时间；不记录语音内容、token、URL 或完整 session ID。若需和服务端关联，只输出 session ID 的短哈希，不在屏幕显示原值。

### 屏幕最小状态

保留标题 `Speak`，下面只增加一行 ASCII 状态，避免字体依赖：

```text
O1 I1 L1 A12 S0
```

- `O`：channel open 成功；
- `I`：hello session ID 已验证并绑定；
- `L`：`listen/start` 已实际发送成功；
- `A`：本 generation 成功交给 WebSocket 的二进制包数，显示 `99+` 封顶；
- `S`：STT 路由结果，`0` 未见、`M` match、`X` wrong/missing/invalid、`R` retired/stale。

只在状态首次变化时刷新，不能每个音频包重绘。网络/音频线程只更新原子或固定快照；LVGL 更新仍经主任务/显示锁。

### 终态文案

| 条件 | 最小终态 | 含义 |
|---|---|---|
| 普通 fresh WS 对照也失败 | `Normal WS failed` | 共享网络、凭据、服务端或普通音频链路问题，先停止归因 SO |
| 普通对照成功，SO `O0` | `SO open failed` | SO 新通道未打开/未收到 hello |
| `O1 I0` | `SO hello ID invalid` | socket/hello 成功，但用于绑定的 identity 缺失或非法 |
| `O1 I1 L0` | `Listen not started` | 状态事件或 `StartListeningAudio()` 未到达 |
| `L` 已调用但发送失败 | `Listen send failed` | 不能误报为“已经监听”；当前 void API 看不到此事实 |
| `L1 A0` | `No audio uplink` | listen 已发送，但本轮没有成功的二进制上行 |
| `A>0` 且 `S=X/R` | `STT ID rejected` | 已收到 STT，但 identity 缺失、错误或 retired；显示子类型 |
| `A>0 S0` 到 15 秒 | `STT no response` | 已上行音频但设备没见到 STT；这是服务端/ASR/协议响应问题，不能伪称 session-id 被拒绝 |
| `S=M` | 立即进入候选页 | 正向控制；不得再触发该轮 speech timeout |

`SendStartListening()` 当前返回 `void`。最低限度可以只标记“调用过”，但要区分“未调用”和“调用了但发送失败”，更可靠的最小改法是让它返回底层 `SendText()` 的 bool，并同时验证 WebSocket 与 MQTT 的普通路径。

音频也至少分两级：`encoded_packets` 与 `send_ok`。这样 `encoded=0` 指向麦克风/AFE/Opus，`encoded>0 && send_ok=0` 指向 WS send；仅用“mic enabled”不能证明上行。

## 5. 最小测试矩阵

### 5.1 Host 可自动化

建议在现有 `stroke_round_integration_harness.cc` 中增加一个表驱动 failure matrix，并由现有 Python wrapper 或一个新增的单一 unittest 方法执行：

| Case | 注入 | 必须断言 |
|---|---|---|
| H0 正向 | normal WS 成功；SO open + valid ID + listen send + 1 个 audio send + matching STT | 候选态；无 timeout |
| H1 普通 WS 失败 | 普通 fresh open 返回 false | 分类为 `Normal WS failed`，不归因 SO |
| H2 SO open 失败 | 普通对照成功；SO open false 或 `IsAudioChannelOpened=false` | `SO open failed`；start/audio 均为 0；coordinator 不留活动轮次 |
| H3 未 StartListening | open/bind 成功，跳过 `StartListeningAudio()`，推进单调时钟 | `O1 I1 L0 A0`；终态 `Listen not started` |
| H4 无音频上行 | start send 成功、capture 标记开启，但不产生包；再测 encode 有包而 send 全失败 | 分别定位 capture/encode 与 transport send，终态均属于 `No audio uplink` |
| H5 STT ID 拒绝 | 注入 missing、非法、valid-wrong、retired 和 exact-match ID | missing/wrong 立即 fail；retired 记录 `S=R` 且不误提交；match 只提交一次 |
| H6 服务端无 STT | `send_ok>0`，完全不注入 STT，推进到 15 秒 | `STT no response`，不能分类成 ID reject |

同时增加/抽取一个纯 host JSON 测试：open 返回 `sid-B` 后，`listen/start` JSON 必须携带完全相同的 `sid-B`，且发送失败可被上层观察。当前测试只验证选择 helper 和 source 文本，没有验证这条数据契约。

这些测试可在 host 证明分类器、时序、generation 隔离和路由决策；不能证明真实 CoreS3 有音频包。

### 5.2 必须真机或“CoreS3 + 可控服务端”验证

以下事实 host fake 无法替代：

- OTA 下发的真实配置、TLS/鉴权、DNS 和 `WebSocket::Connect()`；
- server hello 是否携带有效且每轮独立的 session ID；
- CoreS3 ES7210/I2S 是否采到 PCM，AFE 是否输出，Opus 是否编码；
- 二进制帧是否真的离开设备并被服务端收到；本地 `Send()` 返回 true 只证明本地栈接受；
- 服务端是否收到 `listen/start`、是否启动 ASR、返回 STT 的 ID 是否和 hello 相同；
- LVGL 诊断行、触摸、状态切换与 15 秒终态是否正确。

可控服务端最小场景：

1. 不回 hello：应在约 10 秒得到 `SO open failed`，不能到 Speech Timeout；
2. 回有效 hello，记录是否收到 `listen/start` 与二进制包，不回 STT：应区分 `L0`、`A0`、`A>0 S0`；
3. 回 matching STT：必须进入候选页；
4. 回 missing/wrong/retired session ID：必须显示对应 `STT ID rejected` 子类型，且不得把错误 STT用于当前候选。

## 6. 现在不改代码也能执行的最短真机循环

1. **冷启动先看启动日志**：必须是 `selected=websocket`、`correlated_open=1`、`stroke_voice=1`。这些只证明选择和 capability，不证明连接。
2. **先做普通 WS fresh-channel 对照**：不要先进入 SO；普通聊天说同一个短词（建议“一”），要求屏幕出现用户文本且串口出现普通 STT 的 `>>`。若失败，先处理普通 WS/音频/服务端。
3. **回 Idle 后立即做 SO**：同距离、同音量说“一”；同时录屏、保存串口和服务端同一时间窗日志。
4. **按证据停在最早失败关口**：
   - 无 `Connecting to websocket server`/server hello：open；
   - 有 hello 但服务端无 `listen/start`：当前代码无法仅靠设备日志区分“未调用”和“发送失败”；
   - 服务端有 start、无 binary：音频上行；
   - 有 binary、服务端发了 STT：比较 hello/listen/STT 三者 session ID；
   - 有 binary、服务端没发 STT：服务端/ASR 无响应，不是已证实的 device session reject。

当前无改动时，屏幕只能提供：`Speak`、`Timeout`、missing identity 的 `Stroke voice unavailable`、unknown valid identity 的 `Stroke session identity error`。其中 retired ID 是静默 Drop。因此，要彻底区分五类，至少需要上述一行状态或服务端日志。

## 7. 验收标准

- 冷启动普通 WS 对照和 SO 使用同一服务端、同一短语，并强制各自 fresh channel；
- 每个 SO generation 只产生一份有界诊断摘要，旧 generation 事件不得修改新页面；
- open 失败不得等到 15 秒后才显示通用 Timeout；
- `L1` 必须表示 start frame 发送成功，而不只是函数被调用；
- `A>0` 必须统计成功的 binary send，不能只统计 mic enabled 或 queue 入队；
- 每个收到的 STT 都记录 match/missing/wrong/retired 路由结果；原始 session ID 不上屏、不进公开日志；
- matching STT 后取消 speech timeout并进入候选；
- host failure matrix 通过、现有 116 项回归通过、CoreS3 IDF 6.0.2 构建通过；
- 真机至少完成普通 WS 正向、SO matching 正向、no-hello、wrong-ID 四个场景，并保留屏幕、串口和服务端三方时间线。

## 8. 本次验证说明

本次使用只读文件枚举、文本检索和源码阅读；没有 shell/test/build/flash 权限，因此没有执行 `python3 -m unittest discover -s scripts/tests -v`，也没有烧录 CoreS3。116 是静态方法数；所有动态结果仍需后续执行者实际验证。

- 静态盘点当前 `scripts/tests` 六个 Python 文件共有 116 个 `test_*` 方法：65+2+3+24+13+9。
- 生产 Speech timeout 只在 `StrokeRoundCoordinator::Phase::AwaitingSpeech` 触发；该 phase 由成功且 identity 有效的 `BindOpenedChannel()` 建立。
- `StrokeOrderView::StartVoiceSessionFromMain()` 在排队打开音频通道之前就渲染 `Speak`，所以看到 Speak 不等于 channel 已打开或麦克风已启动。
- `MarkListeningStarted()` 不创建独立 phase，只重置 15 秒起点；现有状态无法区分“已 bind 未 StartListening”和“已 Start 正在等 STT”。
- `Protocol::SendStartListening()` 当前返回 void，丢弃底层 `SendText()` 的 bool；`Application` 对 `SendAudio(false)` 清空剩余队列但没有 SO 级诊断。
- 现有 round harness 覆盖成功 start、15 秒边界、missing/wrong fail-closed、retired Drop 和 matching commit，但不执行生产 WebSocket、AudioService、CoreS3 麦克风或真实服务端。
- 活动 SO 期间 missing/非法或未知有效 session ID 会提前 abort；retired session ID 会静默 Drop，并可能让当前轮最后仍显示 Timeout。
- WebSocket `Start()` 不建立连接；启动时 `selected=websocket` 和 capability 日志只证明选择，不证明普通 WebSocket 语音链路可用。

<!-- pi-squad:7a009925ad0a73cdbb103d0130d21cefa58dd445d113c93bd233ba7b9787b1f0 -->
## 2026-09-12T14:55:17.898Z — stroke-voice-autostop-review

# stroke-voice-autostop 独立审查

**Verdict: REJECTED**

Happy path 的 AutoStop 根因修复基本成立，且作者保留的测试、格式化与 CoreS3 构建证据可信；但“只有真正启用 voice processing 后才显示 Speak/启动 15 秒计时”以及共享 `SendStartListening(bool)` 的普通调用方失败恢复仍未满足。当前仍有可重现为“Listening/Speak 但没有采音或无法继续”的失败路径，因此暂不授权重打 app/package 或烧录。

## Blocker

无。

## Major

### M1. `Speak` 和 speech timeout 在未确认 voice processing 真正启用时仍会开始

- `main/application.cc:1878` 仅调用 `audio_service_.EnableVoiceProcessing(true)`；随后 `main/application.cc:1887-1888` 调用 `MarkListeningStarted(...)`，并在 `main/application.cc:1907` 调用 `ShowListeningFromMain(...)`。
- 但 `AudioService::EnableVoiceProcessing` 返回 `void`；`main/audio/audio_service.cc:684-686` 在 `InitializeAudioEngine()` 失败时直接返回，不设置 `AS_EVENT_AUDIO_PROCESSOR_RUNNING`。成功标志实际直到 `main/audio/audio_service.cc:697` 才设置。
- 因此初始化失败时，生产代码仍会启动 15 秒 speech timer 并显示 `Speak`，但没有有效采音，最终仍可能落到本修复要消除的假 `Speak → Timeout`。
- `scripts/tests/stroke_round_integration_harness.cc:469` 直接把 `microphone_enabled=true`，没有 voice-enable 失败注入，因而现有 117 项测试无法捕获此路径。

**必须修复：** 在 `EnableVoiceProcessing(true)` 后、`MarkListeningStarted` 前验证 `IsAudioProcessorRunning()`（或把 enable API 改成返回成功状态）；失败时停止/关闭本次 stroke channel、回收 generation、关闭 Connect 页面且不得显示 Speak或启动 speech timer。增加 `enable_voice_ok=false` 的生产语义 harness。

### M2. `SendStartListening(bool)` 的普通调用方失败恢复不完整

- 共享层修改本身正确：`main/protocols/protocol.h:83` 返回 `bool`，`main/protocols/protocol.cc:75-87` 原样返回虚函数 `SendText` 的结果；WebSocket 与 MQTT 都通过各自 `SendText` 兼容该实现。
- stroke generation 的失败分支会 abort，符合要求。
- 但 `main/application.cc:1853-1867` 对 generation 0 的失败最终只是 `return`。若该入口是在 processor 尚未运行时进入 Listening，设备状态仍停留 `kDeviceStateListening`，采音未启用，也没有 close/Idle 恢复。
- `main/application.cc:1656-1662` 的另一调用点只记录 warning，之后仍执行 decoder/popup/wake-word 操作。
- 不能假设失败总会触发 network-error fence：`main/protocols/websocket_protocol.cc:83-86` 在 websocket 不存在或已断开时直接返回 false；`main/protocols/mqtt_protocol.cc:170-173` 在 publish topic 为空时也直接返回 false。

**必须修复：** 为 generation 0 和 wake-word re-listen 明确定义失败恢复，至少关闭失效 channel、回到 Idle/恢复 wake word，且不得继续表现为已监听；补充两类调用点的 send-failure 测试。否则“共享 Protocol 及调用方正确处理 bool”和普通路径不回归尚不能成立。

## Minor

### m1. ManualStop 闭环测试是拆分证据，不是一个完整的旧生产路径回归

- 保留日志证明 helper 在“所有 generation 都返回 ManualStop”时确实 RED：编译成功、运行退出 1，并出现 4 个 AutoStop 断言失败。
- fake server 也证明 manual 无 stop 不出 final STT，并在 `MarkListeningStarted+15s` 超时；AutoStop/VAD 正向可进入 Candidates。
- 但 `FakeApplication::StartListeningAudio` 在 `scripts/tests/stroke_round_integration_harness.cc:461` 会重新用 helper 选 AutoStop；manual case 随后在 941-942 行直接重置 fake server/记录值为 ManualStop。故闭环语义与 production seam 是两个拼接测试，而不是旧生产 wiring 自然走到 timeout 的单一回归。

这不否定当前根因，但建议让 fake application 接收已由 open/existing-channel 分支选定的 mode，以便旧硬编码真正让同一集成用例失败。

## 已核对通过的部分

1. `ListeningModeForStartGeneration`：generation 0 明确为 ManualStop，非 0 为 AutoStop；enum 值由 `application.cc:29-34` 的 static_assert 与 `ListeningMode` 对齐。
2. `GetDefaultListeningMode()` 保持 AEC off→AutoStop、AEC on→Realtime，普通 ToggleChat 仍调用该函数。
3. `HandleStartListeningRequest` 只计算一次 `start_mode`，首次 open continuation、已有 channel 以及 speaking 分支均使用该值；函数内不再硬编码 ManualStop。
4. UI 正常路径为 SO 点击后 `StartVoiceSessionFromMain` 渲染 Connect，成功 send、enable 调用、Mark 后才由 `ShowListeningFromMain` 渲染 Speak。
5. `BeginRound` 把 `speech_started_ms_` 清零；只有 `MarkListeningStarted` 写入时间；`CheckTimeouts` 还要求该值非零，连接阶段不消耗 15 秒。
6. fake server 覆盖 manual 等 stop、AutoStop VAD 返回 matching STT、send failure 不显示 Speak，以及 MQTT local fallback 不发送 listen/start。
7. MQTT 仍声明 `SupportsCorrelatedSessionOpen=false`、`SupportsStrokeVoiceRouting=false`；WebSocket 为 true/true。

## 作者验证证据核对

本 Reviewer 按只读限制只读取真实代码与保留日志，**未运行 shell、测试、format 或构建，也未修改文件**。

保留日志显示：

- RED：helper 固定 ManualStop 时 harness 编译 exit 0、运行 exit 1，4 个预期断言失败。
- GREEN：mode 与 round harness 均 PASS。
- 全量 host：`Ran 117 tests in 14.958s`，`OK (skipped=1)`。
- `clang-format version 19.1.7`，`FORMAT_DRY_RUN=0`；`git diff --check` 为 `DIFF_CHECK=0`。
- ESP-IDF v6.0.2、仓库外新建目录 `/private/tmp/xiaozhi-stroke-voice-autostop-build`，CoreS3 + `CONFIG_STROKE_ORDER_LOCAL=y` 完成 2214-step 构建；日志明确为 `Project build complete`。
- app：`xiaozhi.bin` 2,909,536 bytes，SHA-256 `ec03a329fbbb42543740a4df51eb2eeaf1b22d193f1af6c53a485280cc3c93da`；app 分区剩余 `0x129aa0`（30%）。
- assets：`generated_assets.bin` 2,877,806 bytes，SHA-256 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`；`stroke_order.bin` 1,046,788 bytes，SHA-256 `3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`；manifest 为 500 字、`not_a_release_library=true`。这两个 assets hash 与前一版 500 字构建保留证据一致。

## 放行条件

修复 M1、M2 并补失败路径测试后，重跑 117+ host suite、clang-format/diff check 与同配置 CoreS3 clean build。独立复审通过后，才可重打 app/package 并执行真机 `Connect → Speak → 15 秒内候选` 验证；本轮 REJECTED 不授权该重打与烧录步骤。

- `ListeningModeForStartGeneration(0)` 当前返回 ManualStop，所有非零 stroke generation 返回 AutoStop；`GetDefaultListeningMode()` 仍独立保持 AEC off→AutoStop、AEC on→Realtime。
- 生产路径在 `BeginRound` 清零 `speech_started_ms_`，仅 `MarkListeningStarted` 写入起点；`CheckTimeouts` 要求起点非零，因此连接阶段不计入 15 秒 speech timeout。
- `Protocol::SendStartListening` 已返回底层虚函数 `SendText` 的 bool；WebSocket 和 MQTT 均通过共享实现发送 listen/start，MQTT 的 stroke voice capability 仍为 false。
- `AudioService::EnableVoiceProcessing(true)` 是 void，并可能在 `InitializeAudioEngine()` 失败时直接返回而不置 processor-running 位；当前 Application 未验证该结果便标记 listening started 和显示 Speak。
- 保留证据显示 117 项 host 测试通过（1 skip）、clang-format 19.1.7 与 diff check 通过、IDF v6.0.2 CoreS3 clean build 成功；500 字 generated assets hash仍为 `6d403211...a4df0`。

<!-- pi-squad:8ff166b9c7a935a1a94bb1084f5debbd0f05bd79b3345d18bef3dcd8487a8b5a -->
## 2026-09-12T15:14:40.583Z — stroke-voice-autostop-rereview

# stroke-voice-autostop 修订复审

## Verdict: REJECTED

M2、启动失败闭环、production-used policy seam、ManualStop 负向闭环和 AutoStop 正向闭环基本已关闭；作者保留的 host/format/build 证据也可核验。但 **M1 的 engine/event postcondition 仍未真正实现**，因此当前不能放行重打 app/package 或烧录验收。

## 未关闭项

### Major — `AudioService::EnableVoiceProcessing()` 的 bool 仍只证明 service event，不证明 AudioEngine 已启用/停用

真实代码已有可用于后置条件的 engine 查询接口：

- `main/audio/audio_engine.h` 声明 `virtual bool IsVoiceProcessingEnabled() const = 0;`。
- `main/audio/engines/afe_audio_engine.cc` 通过 `kVoiceProcessingEnabled` event bit 实现该查询。
- `main/audio/engines/lite_audio_engine.cc` 通过 `voice_processing_enabled_` 实现该查询。

但修订后的 `main/audio/audio_service.cc:681-713`：

1. enable 分支调用 void 的 `audio_engine_->EnableVoiceProcessing(true)`；
2. 只复查 `service_stopped_`；
3. **没有检查 `audio_engine_->IsVoiceProcessingEnabled()`**；
4. 随即自行设置 `AS_EVENT_AUDIO_PROCESSOR_RUNNING`，再返回 `IsAudioProcessorRunning()`。

同时 `main/audio/audio_service.h:131-134` 的 `IsAudioProcessorRunning()` 只检查：

- `service_stopped_ == false`；
- `AS_EVENT_AUDIO_PROCESSOR_RUNNING` 已置位。

它同样不检查 engine 的真实 enabled 状态。disable 分支也只是调用 void disable、清 service event，然后以 `!IsAudioProcessorRunning()` 返回成功；即使 engine 没有真正关闭，该返回值仍会是 true。

因此当前 bool 是由 AudioService 自己刚设置/清除的 event 推导出来的，不是要求中的 **service_stopped + engine + event** 可观察后置条件。若 engine enable 拒绝、失效或 no-op，`StartListeningAudio()` 仍会看到 enable=true/running=true，继续 `MarkListeningStarted()`、显示 Speak 并启动 15 秒 timeout。这正是本轮要求关闭的 M1 风险。

测试没有发现该问题：`scripts/tests/stroke_round_integration_harness.cc` 的 fake 独立提供 `enable_result` 与 `expose_running_after_enable`，能证明 policy 对失败结果的处理，但不能证明真实 `AudioService` 会从真实 AudioEngine 状态产生失败结果；`scripts/tests/test_stroke_order_ui.py:455-487` 的静态约束只查找 `return IsAudioProcessorRunning();`，没有要求生产代码调用 `IsVoiceProcessingEnabled()`。

## 已核验通过的部分

### Stroke 启动顺序与失败闭环

`main/application.cc:1839-1973` 的生产顺序已是：

1. generation/fence/phase gate；
2. `SendStartListening(listening_mode_)`；
3. 再次检查 fence；
4. `EnableVoiceProcessing(true)`；
5. `IsAudioProcessorRunning()`；
6. policy disposition；
7. `MarkListeningStarted()`；
8. `ShowListeningFromMain()`，随后才显示 Speak。

send/enable/running 失败在 `MarkListeningStarted()` 前处理。非 fenced stroke 会先 `PublishCancelFence()`，再走 `AbortStrokeRound(...StartFailed)`；该路径会 `ResetStreamingState()`、关闭 channel、取消 overlay 并回 Idle。因此，只要 AudioService 能诚实报告 engine 失败，就不会显示 Speak，也不会写入 `speech_started_ms_` 或启动 15 秒等待。

### generation 0 与 wake re-listen 恢复

`RecoverOrdinaryListeningStartFailure()` 会清 pending/generation/popup，reset streaming，关闭并退休普通 channel，回 Idle，并立即调用 `EnableWakeWordDetection(true)`。

该恢复路径已接入：

- generation 0 的 send 失败；
- generation 0 的 enable/running 失败；
- generation 0 的 `MarkListeningStarted()` 失败；
- Listening 状态 wake-word re-listen 的 send 失败。

wake re-listen 的失败分支在 `ResetDecoder()` 和 popup 前 return，满足“不继续 decoder/popup 且恢复 wake”的要求。`GetDefaultListeningMode()` 仍保持 AEC off→AutoStop、否则 Realtime；显式 generation 0 仍由 `ListeningModeForStartGeneration(0)` 选择 ManualStop。

### Policy seam 与闭环测试

`main/protocols/listening_start_policy.h` 被真实 `Application::StartListeningAudio()` include 并调用，也被 integration harness 调用。policy 对 send/enable/running 任一失败映射为 generation 0 RecoverOrdinary、stroke AbortStroke、已 fenced stroke AbandonFencedStroke。

ManualStop 负向闭环现在通过 `ContinueOpenWithMode(...ManualStop)` 在 start 前注入旧 wiring；fake server 自然收到 ManualStop，VAD 不产生 final STT，并在 `MarkListeningStarted()` 后 15 秒进入 speech timeout，没有再事后篡改 server mode。独立 AutoStop 正向用 production helper 选择模式，验证 VAD final STT 后进入 Candidates。

## 作者证据复核

本复审只读检查真实文件及保留日志，未运行 shell、测试或构建，也未修改文件。保留日志可核验：

- Red：2 项定向测试运行，`Ran 2 tests`，`FAILED (failures=1)`；production seam 静态约束失败，而更新后的 integration harness 通过。
- Green：最终 `python3 -m unittest discover -s scripts/tests -v` 为 `Ran 118 tests in 17.234s`、`OK (skipped=1)`；另有 11/11 定向 UI suite 通过。
- 格式：clang-format 19.1.7 对 6 个 scoped C/C++ 文件执行 `--dry-run -Werror`，命令成功且仅输出版本；同一证据链中的 `git diff --check` 成功无报错。
- 构建：仓库外 `/tmp/xiaozhi-stroke-autostop-idf602-20260912-230839` 使用 ESP-IDF v6.0.2、CoreS3、`CONFIG_STROKE_ORDER_LOCAL=y` 完成 build 和 merge-bin；`xiaozhi.bin` 为 2,909,840 bytes，SHA-256 `c2adb51d0907af68d44026adbc901e3136f7629074c81afdd5379cadbf6eb4cd`，app 分区余量 `0x129970`（30%）。
- 500 assets：`generated_assets.bin` 为 2,877,806 bytes，SHA-256 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`；该大小和 hash 与修订前的 stroke-voice-autostop 构建及更早 500 字固件证据一致，确认 assets 未变。
- 临时构建目录没有 `releases/`，未执行发布打包或烧录。

## 放行条件

修复 M1 后需再次复审：

1. enable 后、设置 service running event 前检查 `audio_engine_->IsVoiceProcessingEnabled()`；失败时关闭 engine、清 event、返回 false。
2. `IsAudioProcessorRunning()` 的真值应同时包含 service 未停止、engine 已初始化且存在、engine enabled、service event 已置位。
3. disable 的成功语义也必须确认 engine disabled 且 service event cleared，不能仅因本层 event 被清除就返回 true。
4. 增加能约束真实 AudioService 对 engine/service-event 不一致状态处理的测试或生产源码约束，而不只测试 fake policy。
5. 重跑完整 host suite、clang-format/diff check 和 IDF 6.0.2 CoreS3 clean build；再次确认 500 assets hash 不变。

在上述条件完成并复审通过前，**不批准重打 app/package，也不批准进入烧录验证**。

- `AudioEngine` 公共接口已经提供 `IsVoiceProcessingEnabled() const`，AFE 与 Lite 两个实现都能报告各自的 voice-processing enabled 状态。
- 当前 `AudioService::IsAudioProcessorRunning()` 只检查 `service_stopped_` 和 `AS_EVENT_AUDIO_PROCESSOR_RUNNING`，未纳入 AudioEngine enabled 状态。
- 当前 `AudioService::EnableVoiceProcessing(true)` 调用 void engine enable 后不检查 `IsVoiceProcessingEnabled()`，而是自行置 service event，因此可能把 engine 未启用误报为成功。
- `Application::StartListeningAudio()` 已按 fence→send→enable/running→MarkListeningStarted→ShowListening 顺序执行，且 stroke start 失败发生在计时和 Speak 前。
- generation 0 send/enable/running 失败与 wake re-listen send 失败均已接入 reset、close、Idle、wake-word 恢复路径；wake 失败不会继续 decoder reset 或 popup。
- `EvaluateListeningStartResult()` 同时被生产 `Application` 与 host harness 使用；ManualStop 负向闭环不再事后覆盖 fake server mode。
- 保留日志显示最终 host suite 118 tests 通过、1 skip；clang-format 19.1.7 scoped dry-run 和 `git diff --check` 通过；IDF 6.0.2 CoreS3 clean build 成功。
- 本次构建的 500 assets 为 2,877,806 bytes、SHA-256 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`，与修订前证据一致。

<!-- pi-squad:c76adaabc02f89aa3f7b33a77b73c86caecf319629465a7980099be76a32e739 -->
## 2026-09-12T15:59:38.671Z — stroke-voice-autostop-engine-postcondition-review

# stroke-voice-autostop engine postcondition 最终复审

## Verdict: APPROVED

未发现阻断项或重大问题。Frontend 对最后 M1 的修订满足本次验收条件；可以进入用户指定的 **CoreS3 仅刷 app 后进行 Connect → Speak → AutoStop STT 真机验证** 阶段。

本复审为只读：我直接检查了当前工作树真实源码、测试源码、仓库外构建快照及保留的命令输出；**未修改文件，也未自行运行 shell、测试、格式化或构建**。

## 1. AudioService engine 后置条件：通过

真实 `main/audio/audio_service.cc` 已形成闭环：

- `EnableVoiceProcessing(true)` 先完成 engine 初始化与 decoder/resampler 准备，再调用 `audio_engine_->EnableVoiceProcessing(true)`；随后在设置 `AS_EVENT_AUDIO_PROCESSOR_RUNNING` 之前实际调用 `audio_engine_->IsVoiceProcessingEnabled()`，并通过 `VoiceProcessingEnableMayPublishEvent(...)` 判断是否允许发布 service event。
- engine 拒绝 enable，或此时 service 已 stopped 时，会再次调用 engine disable、清除 service event 并返回 `false`；不会把 engine 拒绝误写成成功 event。
- `IsAudioProcessorRunning()` 同时要求：service 未 stopped、engine initialized、engine present、真实 `engine->IsVoiceProcessingEnabled()` 为 true、service event 为 true。`service_stopped_.load()` 在 engine/event 快照之后读取，`Stop()` 用来唤醒 input task 的 event bit 不会被解释成 running。
- disable 分支先调用 `audio_engine_->EnableVoiceProcessing(false)`，再读取真实 `IsVoiceProcessingEnabled()`。若 engine 仍 enabled，则返回 `false`；service 仍运行时恢复/保持 event，使状态仍可诊断为 running，而不是用 `!IsAudioProcessorRunning()` 伪报停止成功。
- 正常 disable 先确认 engine 已 false，再清 event，并以 `VoiceProcessingDisableSucceeded(engine_enabled, event_running)` 要求“engine false 且 event false”才返回 true。

我同时检查了真实 engine：

- `AfeAudioEngine` 同步置/清 `kVoiceProcessingEnabled`，`IsVoiceProcessingEnabled()` 读取该 event bit。
- `LiteAudioEngine` 使用 `std::atomic<bool> voice_processing_enabled_`，enable/disable 同步写入，查询同步读取。

并发 `Stop()` 后 service event 可作为 input-task wake bit 保留，但此时 `service_stopped_` 使 running 恒为 false；这不属于成功 event，也不构成伪报。

## 2. Production helper 与四类不一致测试：通过

`main/audio/audio_processor_postcondition.h` 不是测试副本：`audio_service.cc` 直接 include 并调用其中四个 helper：

- `AudioProcessorIsRunning`
- `VoiceProcessingEnableMayPublishEvent`
- `VoiceProcessingDisableKeepServiceEvent`
- `VoiceProcessingDisableSucceeded`

`scripts/tests/audio_processor_postcondition_harness.cc` 使用同一个生产头文件和可拒绝 enable/disable 的 `FakeVoiceEngine`，覆盖了要求的四类不一致：

1. engine false、即使尝试把 service event 视作 true，running 仍 false；真实 enable 驱动还验证 event 不发布。
2. engine true、service event false，running false。
3. service stopped、engine true 且 event/wake bit true，running false。
4. disable 后 engine 卡在 true，disable 返回 false、live service event 保持、running 不被伪报为 false。

另有成功 disable 用例要求 engine false、event false、running false。`scripts/tests/test_stroke_order_ui.py` 还约束真实 `AudioService` 必须 include/use helper、真实查询 engine，并检查 enable 查询位于 SetBits 前、disable 查询位于 engine disable 后。Application integration fake 仅验证上层策略，没有被当作 engine 后置条件的替代证据。

## 3. Application 顺序与失败恢复：通过

真实 `Application::StartListeningAudio()` 保持以下顺序：

1. generation/cancel fence/abort/`CanStartListening` 最终门；
2. `SendStartListening(listening_mode_)`；
3. stroke 在 send 后再次检查 fence；
4. `EnableVoiceProcessing(true)`；
5. `IsAudioProcessorRunning()`；
6. `EvaluateListeningStartResult(...)`；
7. 仅在 Proceed 后 `MarkListeningStarted(...)`；
8. 仅在 Mark 成功后 `ShowListeningFromMain(...)`，即 Connect → Speak。

因此 send、enable 或 running 任一失败均不会 Mark，也不会显示 Speak：

- 非零 stroke generation 走 cancel fence + `AbortStrokeRound(...StartFailed)` 或 fenced abandon 清理。
- generation 0 走 `RecoverOrdinaryListeningStartFailure()`；该函数清 pending/generation/popup，`ResetStreamingState()`，关闭 channel，回 Idle，并立即恢复 wake-word detection。
- Listening 状态的 wake re-listen 在 `SendStartListening` 失败时立即调用同一 ordinary recovery 并 return；`ResetDecoder()`、popup 与 wake detector 重启只在 send 成功分支执行。

`listening_start_policy.h` 对 send/enable/running 三个结果统一判定；integration harness 覆盖 stroke send 失败、stroke enable 失败、enable 返回 true 但 running false、generation-0 send/enable 失败、wake re-listen 失败以及成功 AutoStop/VAD STT。

## 4. 保留验证证据：一致

### Host

Frontend 最终收尾实际执行：

```text
python3 -m unittest discover -s scripts/tests -v
Ran 119 tests in 16.683s
OK (skipped=1)
```

其中明确包含并通过：

- `test_audio_processor_postcondition_binds_engine_and_service_event`
- `test_start_result_policy_is_used_by_production_failure_paths`
- `test_round_route_fake_protocol_audio_and_ordinary_bypass`

唯一 skip 是依赖外部 `STROKE_TRANSCRIPTION_JSON` 的可选重建测试。收尾过程中曾有一次把类名写成 `StrokeOrderUiTests` 的定向命令，因类名不存在而仅发生 test-discovery error；随后完整 119 项 discovery 成功执行并覆盖正确类 `StrokeOrderUiTest`，不影响最终结果。

### Format / diff

- 保留记录显示相同源码由 clang-format 19.1.7 执行 scoped `-i` 后再执行 `--dry-run -Werror`，退出成功且无格式错误，覆盖 helper、`audio_service.{h,cc}` 和 postcondition harness。
- 最终收尾再次执行 `git diff --check`，证据为 `git-diff-check-exit:0`。
- 注意 `git diff --check` 本身不检查未跟踪文件；但新增 C++ helper/harness 已被上述直接 clang-format 检查、host harness 编译及 IDF 编译覆盖，因此不构成放行缺口。

### IDF 6.0.2 CoreS3 clean build 快照

复用的仓库外 clean build 位于：

`/private/tmp/xiaozhi-stroke-autostop-engine-pc-20260912-232736/build`

我直接核对到：

- `project_description.json`: `git_revision=v6.0.2`、`target=esp32s3`。
- `build/config/sdkconfig.h`: `CONFIG_BOARD_TYPE_M5STACK_CORE_S3=1`、`CONFIG_STROKE_ORDER_LOCAL=1`。
- `compile_commands.json`: `BOARD_NAME="m5stack-core-s3"`，并包含 `main/audio/audio_service.cc` → `audio_service.cc.obj`。
- build 日志包含 `Successfully generated assets.bin`、`Successfully created ESP32-S3 image`、生成 `xiaozhi.bin`，以及 app 分区检查通过。
- 收尾保留的逐文件 SHA 对比显示 `audio_processor_postcondition.h`、`audio_service.{h,cc}`、postcondition harness、`test_stroke_order_ui.py`、`application.{h,cc}`、`listening_start_policy.h`、`main/CMakeLists.txt` 均与 clean-build 快照 `SAME`。

产物证据：

| 产物 | 字节 | SHA-256 |
|---|---:|---|
| `xiaozhi.bin` | 2,910,176 | `669e3b63169af57b56bb74714f369e64be9c42df16e8c149120fb032a9d55cbe` |
| `generated_assets.bin` | 2,877,806 | `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0` |

app 大小为 `0x2c67e0`，最小 app partition `0x3f0000`，剩余 `0x129820`（30%）。500 字 fixture manifest 仍声明 `character_count: 500`；`generated_assets.bin` 的大小/SHA 与既有 500 字固件证据一致，资产未变化。

## 5. 放行范围与剩余风险

本次批准允许按用户条件进行 **仅刷 app** 的 CoreS3 真机验收。构建的 `flasher_args.json` 将 app 与 assets 分开（app `xiaozhi.bin`，assets `generated_assets.bin`）；仅刷 app 的前提是目标设备已保有上述相同 500-assets 内容和相同分区布局。不要重刷/覆盖 assets。

真机仍需实际确认：

- SO 入口后 Connect → Speak；
- 用户说字后 AutoStop/VAD 返回匹配 STT 并进入候选；
- 失败/断线能退出而不假显示 Speak；
- 无 audio underrun、WDT、持续 heap 下降；
- overlay 关闭后原有短触交互不回归。

Host harness 没有实例化完整 FreeRTOS/AFE `AudioService`；当前 AFE/Lite enable 是同步的，但若未来 engine 改为异步，现有同步 postcondition 需要重新评估。这些是后续硬件/演进风险，不阻止本次 app-only 真机验证。

- `AudioService::EnableVoiceProcessing(true)` 在 service event SetBits 前查询真实 `AudioEngine::IsVoiceProcessingEnabled()`；engine 拒绝或 service 已 stopped 时会 disable、清 event 并返回 false。
- `AudioService::IsAudioProcessorRunning()` 合取 service 未 stopped、engine initialized、engine present、真实 engine enabled 与 service event；Stop 设置的 wake bit不会单独构成 running。
- Disable 成功要求 engine 查询为 false且 service event 已清；engine 卡在 enabled 时返回 false，并在 live service 中保持/恢复 event，因此不会伪报停止。
- 生产 `audio_processor_postcondition.h` 被 `audio_service.cc` 直接使用；专用 harness 覆盖 engine false/event true、engine true/event false、stopped service 以及 stuck disable 四类不一致。
- `Application::StartListeningAudio()` 的生产顺序仍为 fence→send→enable→running→policy→Mark→Speak；stroke、generation 0 和 wake re-listen 失败均有对应清理/恢复路径。
- 保留的最终 host 证据为 119 tests、16.683s、OK、skipped=1；一次错误类名的定向 discovery 失败随后被成功完整 discovery 覆盖。
- 保留的 CoreS3 build 是 IDF v6.0.2/ESP32-S3、`CONFIG_STROKE_ORDER_LOCAL=1`，并实际编译 `audio_service.cc`；关键源码与该 clean-build 快照逐文件 SHA 一致。
- CoreS3 app 为 2,910,176 bytes，SHA-256 `669e3b63169af57b56bb74714f369e64be9c42df16e8c149120fb032a9d55cbe`；assets 为 2,877,806 bytes，SHA-256 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`，与既有 500 字资产证据一致。
