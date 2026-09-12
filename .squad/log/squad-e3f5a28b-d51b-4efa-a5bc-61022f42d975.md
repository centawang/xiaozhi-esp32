

<!-- pi-squad:49f28a8db3c7a6b057d516bb3c6bf0c59585b605f5afb43e44cfe4042f562e92 -->
# squad-e3f5a28b-d51b-4efa-a5bc-61022f42d975
completed; 1/1 successful jobs.
- stroke-final-core-s3-500-websocket-refresh-review: # stroke-final-core-s3-500 WebSocket preference 刷新独立复审

## 结论

**REJECTED**

- Blocker：0
- Major：1
- Minor：1（继承的串口隐私风险）

固件输入、二进制刷新、500 字资产不变性、merged offset、NVS 烧录说明、checksum 与 ZIP 完整性证据均通过；但当前 README 与 manifest 对 `stroke_voice=1` 日志的时序/证明力作了与真实代码相反的陈述。该字段在协议初始化、甚至 `protocol_->Start()` 之前由静态 capability 打印，不能证明 WebSocket 音频通道已打开或已取得本轮 correlated session identity。由于本任务明确要求 README/manifest 准确，当前 ZIP 不应作为最终 500 字原型包发布。

本次严格只读：直接读取真实 dist 目录、真实 ZIP 头与路径、外部 build 元数据、sdkconfig、源码、测试/构建日志和打包 run 的保留原始输出；没有修改文件，没有运行 shell、测试、构建、解压或烧录。以下命令结果均明确标注为 Tester 最终写入后保留的执行证据，不冒充本复审重新执行。

## 已通过部分

### 1. 输入 build 已获独立批准

`.squad/orchestration-log/squad-403028f3-70ee-4225-8cfd-215f5d1c7ebe.md` 明确给出 `stroke-websocket-preference` 的 `APPROVED` 结论，并允许进入 500 字包重打与 CoreS3 真机多文件烧录阶段。

直接读取 `/private/tmp/xiaozhi-stroke-websocket-preference-build`：

- `project_description.json`：ESP-IDF `v6.0.2`、target `esp32s3`、app `xiaozhi.bin`、build dir 与指定路径一致；
- `sdkconfig:650/1106/1226`：`CONFIG_IDF_TARGET="esp32s3"`、`CONFIG_BOARD_TYPE_M5STACK_CORE_S3=y`、`CONFIG_STROKE_ORDER_LOCAL=y`；
- build 日志结尾：`[2213/2214]` 尺寸检查，app `0x2c6310`，分区 `0x3f0000`，余量 `0x129cf0 (30%)`，随后 `Project build complete`；
- host-test 日志：`Ran 116 tests in 15.386s`、`OK (skipped=1)`，即 115 pass、1 expected conditional skip、0 failure/error。

### 2. app/ELF/map/merged 已更新，且来自 approved build

上一版批准基线与当前值不同，确认刷新发生：

| 文件 | 旧 SHA-256 | 当前 SHA-256 | 当前大小 |
|---|---|---|---:|
| `xiaozhi.bin` | `a49373c7a38329023b850a45e0c51426d60236dd5b0b8337e9498df38b52af62` | `f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a` | 2,908,944 |
| `xiaozhi.elf` | `3ff711ddcfbd2100eb60637ac66110c9943ede705d17eab7e3beee07edc76ed2` | `3906ba19dcba7c9a81a337064dbb736856f6b8e5335e1078639155a3ec85a456` | 40,154,248 |
| `xiaozhi.map` | `2ace67eb1d9c49b8548bad970cfc9be4b8cb76931c70163dc63dfd3d71ef7e32` | `7063b425589af4d0b43ad3e0feea239761a3f36137997e9d4fa379560465ae03` | 24,553,497 |
| `merged-binary.bin` | `4a22047b840bd3459cb1ad4fa158567a1e3be2927afd23413df64eca09964fbb` | `e23e291907d14d14a1ee847b340793cf009f0d8ad4650d88d05b5ef8e1ccb039` | 11,266,414 |

Tester 的最终保留输出逐项 `cmp` 通过：package 的 app、ELF、map、flash_args、bootloader、partition table、OTA data、generated assets 与指定 approved build 相同。app 的 `image-info` 所报 ELF SHA 也等于包内 `xiaozhi.elf` SHA。

### 3. 500 字 assets 保持不变

真实 `dist/.../stroke_order/` 仍恰有 14 个普通文件；真实 build 的 `stroke_order_assets/` 列表相同。刷新前后 14 文件 SHA-256 baseline diff 无输出并给出：

`OK all 14 data/license file SHA-256 values unchanged`

当前内层 `SHA256SUMS` 的 13 个被校验文件与 build 中同名清单完全一致；第 14 个文件 `stroke_order/SHA256SUMS` 自身 SHA-256 为 `dbba16e352851188de77545c45db9eaeb1577e7e4261d6ae95c7b3ab115fb70a`。关键资产保持：

- `stroke_order.bin`：`3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`，1,046,788 bytes；
- `stroke_pinyin.bin`：`a97666f18fa8e5a5e586f774dac044c08ba26b40decb4644e12c6886a5cbc998`，18,152 bytes；
- `generated_assets.bin`：`6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`，2,877,806 bytes；
- coverage 文件仍声明 500/500、complete、prototype、`not_a_release_library=true`。

### 4. 双配置 WebSocket preference 与无 WS 降级的功能描述主体正确

真实生产代码：

- `protocol_selection.h`：stroke preference 为真且存在 WebSocket 配置时首先返回 WebSocket；否则保持 MQTT-first/WS-only/no-config fallback；
- `application.cc:594-617`：`CONFIG_STROKE_ORDER_LOCAL` 令 preference 为 true，并按 helper 只构造一个 transport；
- `mqtt_protocol.h`：`SupportsCorrelatedSessionOpen()==false`、`SupportsStrokeVoiceRouting()==false`；
- `websocket_protocol.h`：两项 capability 均为 true；
- `application.cc:1227-1232`：capability 不成立时进入本地候选路径。

因此 README/manifest 对“双配置选择 WebSocket”“`websocket_config=0` 且 MQTT 可用时选择 MQTT、`stroke_voice=0`、使用本地 SPY1 TopRanked 候选、不声称 MQTT 语音选字”的主体说明是正确的。

### 5. NVS 多文件首选与 merged 清 NVS说明保持正确

真实 `flash_args` 仅写：

- `0x0` bootloader
- `0x8000` partition table
- `0xd000` OTA data
- `0x20000` app
- `0x800000` assets

不含 `0x9000` NVS 或 `0xf000` phy_init 输入。README 和 manifest 均把 `write-flash @flash_args` 列为已配置设备保留 NVS 的首选，并明确两种刷写方式二选一；merged 从 `0x0` 写入会用 `0xFF` padding 覆盖 `0x9000–0xcfff`，清除 Wi-Fi、激活及其他 NVS。保留 offset 检查也确认该 16,384-byte 范围全为 `0xFF`。

### 6. 二进制、checksum 与 ZIP 完整性证据通过

Tester 最终写入后保留输出显示：

- merged 五段逐字节匹配：bootloader `0x0`/16,672 bytes、partition `0x8000`/3,072、OTA `0xd000`/8,192、app `0x20000`/2,908,944、assets `0x800000`/2,877,806；
- esptool v5.3.1 对 bootloader、app、merged 报 ESP32-S3 / DIO / 80 MHz / 16 MB，checksum valid、validation hash valid；app 为 `xiaozhi` 2.4.2；
- 顶层 `SHA256SUMS` 25/25 OK；内层 13/13 OK；manifest 24 个 file record 的 size/hash 均匹配；
- `unzip -t` 全部条目 OK，`zip -T` OK；
- 解压后顶层与内层 checksum 全部 OK；原 dist 与解压树 `diff -qr` identical；目录与 ZIP 各 26 个普通文件；
- 直接读取当前真实 ZIP 可见合法 `PK` 文件头及预期顶层目录名。

最终产物：

- ZIP：`dc6524722341f93cb5335117caa10abba383ba2603722853097fa203edb46a15`，24,165,514 bytes；
- merged：`e23e291907d14d14a1ee847b340793cf009f0d8ad4650d88d05b5ef8e1ccb039`，11,266,414 bytes；
- app：`f6ec2f7c99013a28c6163234e268fbb03f5164ad3b233d12d9c15883fcb32f2a`，2,908,944 bytes。

## Major finding：`stroke_voice=1` 被错误描述为 correlated open 成功证据

真实源码顺序：

1. `Application::InitializeProtocol()` 在 `application.cc:609-617` 只根据选择结果构造 `WebsocketProtocol` 或 `MqttProtocol`；
2. 紧接着 `application.cc:620-622` 调用两个 `Supports...()` 并打印 `Stroke voice availability: correlated_open=%d stroke_voice=%d`；
3. WebSocket header 对两个 capability 无条件返回 true，MQTT header 无条件返回 false；
4. `StrokeVoiceRoutingAvailable()`（`application.cc:1133-1135`）也只检查这两个静态 capability；
5. `protocol_->Start()` 直到同一 `InitializeProtocol()` 的 `application.cc:997` 才执行；真正 `OpenAudioChannel()` 更晚才在 listening 流程调用。

所以选择 WebSocket 后，即使尚未启动协议、尚未联网、open 最终失败、尚无 session identity，这条初始化日志也会打印 `correlated_open=1 stroke_voice=1`。

但真实包内：

- README 第 90 行称 `stroke_voice=1` “应在 WebSocket 音频通道成功打开并取得本轮关联身份后出现”；
- README 第 105 行把“WebSocket 通道就绪后确认 `stroke_voice=1`”列为验收步骤；
- manifest 第 517 行声明 `"stroke_voice_one_requires_correlated_websocket_open": true`。

这些描述不准确，并可能让真机验收把静态 capability 日志误当成 WebSocket 已连接、audio open 已成功、session identity 已关联的证据。`selected=websocket` 和初始化时 `stroke_voice=1` 只证明选中了声明支持该能力的 transport；实际 open/session/STT 仍必须通过后续行为或新增的动态日志验证。

**要求修复：** 不必重编 app；应修订 README 和 manifest，明确该日志是 `protocol_->Start()` 前的静态 capability 诊断，不证明连接/open/session 成功。建议删除或替换 `stroke_voice_one_requires_correlated_websocket_open`，并另行表述“笔划监听真正开始前仍必须成功 correlated open”。随后重算 manifest/README/顶层 SHA256SUMS，重建 ZIP，并重跑 25/25、13/13、ZIP CRC、解压 checksum/diff 与最终 ZIP hash/size 检查。

## Minor finding：继承的 WebSocket URL 日志隐私风险

`main/protocols/websocket_protocol.cc:237` 仍将完整 `url.c_str()` 写入串口日志。它不是本次 package refresh 引入，也不影响 transport 选择正确性；但收集/分享真机串口日志前应脱敏 endpoint 及可能位于 URL 中的 query credential。

## 放行影响与剩余风险

- **当前 ZIP 不批准作为最终 500 字原型包。**
- 本 finding 未发现 app 二进制功能缺陷；若设备已经烧录的确实是 approved app SHA-256 `f6ec...32f2a`，没有因本次文档问题而必须重刷 app，可继续用于受控真机验证。但在修正文档/manifest并重包复审前，不应把当前 ZIP标记为最终包。
- 本轮未做真机烧录/运行。真实 OTA 是否提供 WebSocket 配置、真实 WebSocket open/STT/session identity、PSRAM、FT6336 坐标/旋转、overlay 触摸、音频与 100 次会话稳定性仍未验证。
- 500 字数据仍仅为 prototype / `not_a_release_library`，本审查不构成商用授权或逐字笔顺官方认证。
