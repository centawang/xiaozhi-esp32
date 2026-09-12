

<!-- pi-squad:f60a4179116ff260967506f2723092bc3eb56ad28f149454288b1015e93efa81 -->
## rai — stroke-final-core-s3-500-doc-rereview
Mode: review; outcome: completed
Engine: 38be5631-beb9-4a2e-95e8-73e58d4c64d2

# stroke-final-core-s3-500 文档/manifest 独立复审

## 结论

**APPROVED（限定为当前 CoreS3 500 字原型 artifact 的文档、manifest、目录与 ZIP 一致性，以及在已配置 CoreS3 上采用保留 NVS 的多文件烧录路径）**。

- **Blocker：0**
- **Major：0**
- **Minor：0**

此前拒绝项已闭环：README 不再把 merged 镜像作为已配置设备的首选，并已准确说明从 `0x0` 写入连续 merged raw 镜像会清除 NVS；manifest、顶层校验表和 ZIP 均已同步重建。此批准**不等于**对 500 字数据的正式商用发布授权或真机功能验收。

## 只读复审范围与证据说明

本次直接读取了真实工作区中的：

- `dist/m5stack-core-s3-stroke-500-prototype/README.zh-CN.md`
- `manifest.json`
- `flash_args`
- 顶层及 `stroke_order/` 内层 `SHA256SUMS`
- `stroke_order/NOTICE.md`、coverage/source-lock/manifest 文件
- 真实 sibling ZIP（文件存在且为 ZIP/PK 数据）
- Tester run `f58f9d69-359e-44ac-814a-d352f7f4c1ff` 的原始保留执行输出与最终报告
- 上一轮独立拒绝审计及原始打包记录，用于核对修订前基线 hash

遵守任务约束：**没有修改文件、没有运行 shell、没有重跑测试、构建或烧录**。下述 checksum、ZIP 和二进制范围检查是对 Tester 最后一次重建之后保留的原始执行输出的核验，不冒充本次重新执行。

## 1. README 烧录语义：通过

真实 README 当前明确写明：

1. `merged-binary.bin` 是从 `0x0` 开始的连续 raw 镜像，esptool 会擦除并写入其覆盖范围。
2. NVS 位于 `0x9000`–`0xcfff`，共 16,384 字节；merged 在该范围是 `0xFF` padding，因此从 `0x0` 写 merged 会清除既有 Wi‑Fi 配网、激活信息及其他 NVS 数据。
3. 显式 `erase-flash` **不是**清除 NVS 的唯一方式；merged 写入本身即会清除该范围。
4. `write-flash @flash_args` 被列为已配置设备希望保留 NVS 时的首选。
5. 两个 `write-flash` 方式要求严格二选一，不得先后执行，也不得合并输入。
6. README 明确区分“包内不携带导出的凭据/NVS payload”和“烧录是否保留目标设备 NVS”，没有再把两者混为一谈。
7. README 同时保留多文件方式的真实风险：仍会重写 bootloader、partition table、OTA data、app 和 assets，布局不兼容、烧录中断或旧持久状态不兼容仍可能要求恢复。

## 2. `flash_args` 与保留 NVS 路径：通过

真实 `flash_args` 仅包含：

- `0x0 bootloader/bootloader.bin`
- `0x8000 partition_table/partition-table.bin`
- `0xd000 ota_data_initial.bin`
- `0x20000 xiaozhi.bin`
- `0x800000 generated_assets.bin`

它不含 NVS 的 `0x9000` 输入，也不含 `phy_init` 的 `0xf000` 输入。各相邻镜像均按 4 KiB sector 边界与 NVS/phy 分区分离；当前多文件集合不会主动写 NVS 或 `phy_init`。

## 3. manifest 机器字段：通过

真实 `manifest.json` 为有效、完整可读 JSON，机器字段与 README、`flash_args`、分区事实一致：

- `methods_are_mutually_exclusive: true`
- `preferred_flash_method_for_preserving_nvs: "multi-file"`
- `merged_clears_existing_nvs: true`
- NVS range：offset `36864/0x9000`，inclusive end `53247/0xcfff`，exclusive end `53248/0xd000`，size `16384`
- multi-file 的 NVS/phy image 输入与 active write 四项均为 `false`
- multi-file 写入内容及 bootloader/partition/OTA/app/assets 风险均已列出
- merged 为 `write_offset=0`、`continuous_raw_image=true`、NVS range 为 `0xff_padding`
- merged 用途仅为 fresh device 或明确接受 Wi‑Fi/激活/NVS 重置的 complete reflash
- `explicit_erase_flash_is_not_the_only_way_to_clear_nvs: true`
- `credential_absence_does_not_imply_nvs_preservation: true`

`security_and_packaging.nvs_partition_payload_included=false` 也已由说明字段限定为“没有设备导出的 NVS/凭据”，并明确补充 merged 仍会用 FF padding 清除目标设备 NVS。README 在 manifest 文件表中的 hash/size 与顶层校验表一致。

## 4. 二进制与其他不可变文件：通过

Tester 在修订前建立完整 baseline，最终重建后比较除获准变化的 README、manifest、顶层 SHA256SUMS 外的其余文件，保留输出为 **23/23 unchanged**。当前真实顶层校验表仍列出同一组值。主要二进制如下：

- bootloader：`1cf7897c69aaf046202e438fad724d692f701a343c85b15cefd0b0420cccce51`
- generated assets：`6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`
- merged：`4a22047b840bd3459cb1ad4fa158567a1e3be2927afd23413df64eca09964fbb`，11,266,414 bytes
- OTA data：`7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f`
- partition table：`4811619cacae08ef2e0e71b7220c6033a346ca5da7ca179082408c963ef530b5`
- SOB1：`3ca8fb08164a86e10bf150cfbfa006b133c35df00f0a46a5d55c18e5ff726f3b`
- SPY1：`a97666f18fa8e5a5e586f774dac044c08ba26b40decb4644e12c6886a5cbc998`
- app：`a49373c7a38329023b850a45e0c51426d60236dd5b0b8337e9498df38b52af62`
- ELF：`3ff711ddcfbd2100eb60637ac66110c9943ede705d17eab7e3beee07edc76ed2`
- map：`2ace67eb1d9c49b8548bad970cfc9be4b8cb76931c70163dc63dfd3d71ef7e32`

保留的最终检查还直接读取 merged 的 `0x9000:0xd000`，结果为 **16,384/16,384 字节均为 `0xFF`**，支持文档中的 NVS 清除结论。

## 5. SHA256SUMS 与 ZIP：通过

真实顶层 `SHA256SUMS` 当前恰有 **25 项**，逐项覆盖 package 目录内除自身外的全部 25 个普通文件；目录总计 26 个普通文件。最终修订后保留执行输出证明：

- 目录 `shasum -a 256 -c SHA256SUMS`：25/25 `OK`
- `unzip -t`：26 个文件条目全部 `OK`，`No errors detected`
- 解压目录再次校验：25/25 `OK`
- 原目录与解压目录 `diff -qr`：`identical`
- 最终不可变文件对比：23/23 unchanged
- 最终 README/manifest/NVS 语义断言：`OK`

最终文档与封装 hash/大小为：

| 文件 | SHA-256 | 大小 |
|---|---|---:|
| `README.zh-CN.md` | `b75dd1d60d5cb1d3de66c335b6a92d409f85a6640d4485c39d0515593ba7a364` | 8,149 B |
| `manifest.json` | `5fb0f547e8a4e63a7efcecb7a4d92907e0ff1f8725d04d646c6468e8b1727635` | 17,303 B |
| `SHA256SUMS` | `d51b79a25e9932ae1c430fc1eb8dcf9b950dc94091dd11ffac0cc565cc593990` | 2,299 B |
| `m5stack-core-s3-stroke-500-prototype.zip` | `bdbc8c589db291d4ace0de49e093dab87079fb5d6e955c36a104227cf410371e` | 23,931,458 B |

最终校验发生在最后一次 README 同步及原子重建 SHA256SUMS/ZIP **之后**，不是中间版本的结果。

## 6. 500 字、许可、dirty 与未真机边界：无回退

- `stroke_order.cov.json` 仍为 500/500、0 error、complete，同时保留 `prototype`、`not_a_release_library`、`not_official_certification`。
- `stroke_pinyin.cov.json` 仍为 500/500、complete、prototype/not-release。
- APL、Unicode License、NOTICE、charset、selection、SOB1/SPY1 及 source-lock/manifest 的 hash 均在 23/23 不变集合内。
- NOTICE 仍固定 HWD commit `68d10a4b21150cae5e1ebbd223eed289cf32d90c`、仅使用 strokes/medians，并固定 Unicode 16.0.0 Unihan；明确不是官方认证、不是 release 字库、未完成商业审查，外部分发前仍需法律与逐字笔顺准确性审核。
- README 与 manifest 仍明确 `dirty_worktree=true`、base commit 不能单独复现全部二进制、打包前 dirty path count 为 48。
- `hardware_validation_performed=false`；README 和 hardware risk 列表仍明确本轮未烧录、未验证真实 PSRAM、FT6336、WebSocket STT/open identity、FPS、heap/timer/WDT/audio-underrun 及 100 次会话长稳。

因此，本次文档修订没有把原型包装成正式/商用字库，也没有把离线构建验证冒充真机验收。

## 批准的保留 NVS 烧录方式

**允许主协调器烧录当前已配置的 M5Stack CoreS3，但仅批准下面的 multi-file 路径。** 应先进入 package 目录，确认 `$PORT` 指向正确的 ESP32-S3 / 16 MB CoreS3，然后执行：

```sh
cd /Users/mandyw/git/xiaozhi-esp32/dist/m5stack-core-s3-stroke-500-prototype
python -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
  --before default-reset --after hard-reset write-flash @flash_args
```

在这台已配置设备上，**不要**先执行 `erase-flash`，也**不要**再执行 `0x0 merged-binary.bin`；两种写法严格二选一。multi-file 路径保留 NVS/phy 的前提不消除 bootloader、分区表、OTA data、app/assets 改写及断电/布局兼容风险。

## 剩余风险（不阻止本次限定批准）

1. 本轮仍没有真机烧录结果；NVS 保留、启动、PSRAM、触摸、联网、动画和长稳需要烧录后观察。
2. multi-file 会改写 bootloader、partition table、OTA data、app 和 assets；应保证供电稳定并保留恢复手段。
3. 500 字数据仍仅为原型；不得把本批准解释为正式商用分发许可、官方笔顺认证或逐字准确性审核结论。
