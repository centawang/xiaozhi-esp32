

<!-- pi-squad:98445d67d3c576a60e68bced4229f6b1cab227d289b3c5a6dedbf1ef081e2d14 -->
## tester — stroke-stt-async-close-fence
Mode: write; outcome: completed
Engine: 100f2b8b-c1da-426b-b1cc-c163634652f1

已完成 `stroke-stt-local-candidates` 的限定修订，并将完整报告写入 `/Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/100f2b8b-c1da-426b-b1cc-c163634652f1/stroke-stt-async-close-fence.md`。WebSocket 的既有 per-socket generation 现在作为预留 `open_attempt_id` 从 open 前贯穿至 close callback，并由 Application 的 opening/bound 单槽映射到确切 stroke generation；close callback 先发布 fence，再只调度 main-task abort，覆盖 open 成功到 bind 之间的窗口且不按当前 token 猜测。Network Scanning/Disconnected、Protocol OnNetworkError、ResetProtocol、RequestStartStrokeRound replacement 也均在 event/schedule 前发布 fence；最终 listening gate 与 fence publisher 串行化并在同步发送错误后、启用采集前复查。MQTT stroke voice capability 仍为 false/false。

测试已扩展到 ChannelClosed、NetworkDisconnected、NetworkError、ResetProtocol、ReplaceRound，并模拟 `STATE_CHANGED` 先于 scheduled abort；还覆盖 open-return-to-bind close identity 和 replacement 不降级 generation 0。专项 2 项通过、stroke UI 9 项通过、全量 host 100 项通过；clang-format 19.1.7 scoped check 与 `git diff --check` 通过。ESP-IDF v6.0.2 仓库外隔离 CoreS3 `CONFIG_STROKE_ORDER_LOCAL=y` 全量后增量构建通过；最终 `xiaozhi.bin` 2,903,072 bytes，SHA-256 `c9f7adf5a386e46d35aa973d4b8573f2ddcb5c6ca0f855623a9479bea3d065fa`。未 commit/stage/reset/clean，未覆盖既有未提交工作。
