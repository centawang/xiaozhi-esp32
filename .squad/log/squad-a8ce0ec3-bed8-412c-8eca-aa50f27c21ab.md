

<!-- pi-squad:cb1ef0d5425d25461a1e4fe9ed00bd8f30ad2da1a254927e1c4f21ca3dfc8008 -->
# squad-a8ce0ec3-bed8-412c-8eca-aa50f27c21ab
completed; 1/1 successful jobs.
- stroke-voice-autostop-revision: 已完成 stroke-voice-autostop 的 M1/M2 修订并写入报告：`/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/dc0a382f-5464-4566-a178-b03442c7a248/stroke-voice-autostop-revision.md`。

实现要点：`AudioService::EnableVoiceProcessing` 攩为可观察的 bool 结果；Stroke 启动严格按 fence→send→enable/running→MarkListeningStarted→Speak；stroke 和 generation-0 各失败路径均关闭 channel、reset streaming、回 Idle，普通/wake re-listen 恢复唤醒词；新增 production-used `listening_start_policy.h` seam；wake re-listen send 失败后不再执行 decoder/popup；保留既有 mode/session/fence/WS/MQTT/500 字行为。

验证：red 阶段 2 项定向测试中 production seam 约束按预期失败 1 项；green 阶段完整 host suite 118 项通过、1 项既有条件性 skip；clang-format 19.1.7 scoped dry-run 通过；`git diff --check` 通过；ESP-IDF v6.0.2 在仓库外全新副本以 CoreS3 + `CONFIG_STROKE_ORDER_LOCAL=y` clean build 成功。App 为 2,909,840 bytes，SHA-256 `c2adb51d0907af68d44026adbc901e3136f7629074c81afdd5379cadbf6eb4cd`；assets 为 2,877,806 bytes，SHA-256 `6d403211684fda973fa415b92aa04e3a557bcfa6896563d5f454bc78c87a4df0`。未 commit/stage/reset/clean，未 zip 发布、未烧录。
