

<!-- pi-squad:65d5c341bb0d7602ce026a91c787ca7577e8874d55f410956cfd718e396b4480 -->
# squad-c54c8ff7-600c-4c4d-ae5d-0ac6d988ba54
completed; 1/1 successful jobs.
- stroke-ui-cores3-retry: In-place retry of the CoreS3 笔划 UI (no rollback/commit). Nested DisplayLockGuard removed; draw uses CopyLoadedGlyph caches, not cross-lock StrokeViews; overlay timer/canvas_buf lifecycle and generation-gated clicks tightened; CoreS3 I2C stays on the 20ms poll with atomic indev coords and exclusive overlay vs short-tap ToggleChatState. Smoke extra-file remains stroke_order.bin + APL/NOTICE. clang-format 19 clean. Host tests 93/93 including production controller harness. Isolated ESP-IDF v6.0.2 m5stack-core-s3 CONFIG_STROKE_ORDER_LOCAL=y build succeeded: /tmp/xiaozhi-stroke-ui-cores3-build/xiaozhi.bin 2879248 bytes, app partition 30% free (0x1310f0). Hardware not flashed. 一/人/口 is not a release 字库; no type=stroke/remote STT.
