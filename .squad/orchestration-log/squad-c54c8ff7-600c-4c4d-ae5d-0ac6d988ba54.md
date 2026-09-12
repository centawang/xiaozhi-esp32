

<!-- pi-squad:6a7d04cf4d3144dc4ef2d7079cc11946450542b0241a506eeaebd1b5739d5a1b -->
## frontend — stroke-ui-cores3-retry
Mode: write; outcome: completed
Engine: 37735d95-7453-4f57-aa32-b3df06852dd8

In-place retry of the CoreS3 笔划 UI (no rollback/commit). Nested DisplayLockGuard removed; draw uses CopyLoadedGlyph caches, not cross-lock StrokeViews; overlay timer/canvas_buf lifecycle and generation-gated clicks tightened; CoreS3 I2C stays on the 20ms poll with atomic indev coords and exclusive overlay vs short-tap ToggleChatState. Smoke extra-file remains stroke_order.bin + APL/NOTICE. clang-format 19 clean. Host tests 93/93 including production controller harness. Isolated ESP-IDF v6.0.2 m5stack-core-s3 CONFIG_STROKE_ORDER_LOCAL=y build succeeded: /tmp/xiaozhi-stroke-ui-cores3-build/xiaozhi.bin 2879248 bytes, app partition 30% free (0x1310f0). Hardware not flashed. 一/人/口 is not a release 字库; no type=stroke/remote STT.
