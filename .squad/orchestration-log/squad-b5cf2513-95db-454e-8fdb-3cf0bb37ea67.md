

<!-- pi-squad:64b9c00d7c7ea3400465bdda2e9cc05e456bf7d7853789881d4e5f311c641829 -->
## backend — stroke-stt-local-candidates
Mode: write; outcome: completed
Engine: 37009461-db49-42a5-89b7-93f3c675000c

Implemented CoreS3 local-first 笔划 STT session: Idle entry schedules a main-task voice session (AwaitingSpeech overlay + existing StartListening), intercepts STT only when pending, parses one Hanzi by Unicode code point, SetCandidates (recognized char first, store-only, dedup, max 6; smoke 一/人/口 correctly shows 1 with no fake homophones), then local playback. No type=stroke. Host 98/98 (kept prior 96). Isolated IDF v6.0.2 m5stack-core-s3 CONFIG_STROKE_ORDER_LOCAL=y build produced /tmp/xiaozhi-stroke-stt-local-candidates-build/xiaozhi.bin 2,886,016 bytes (30% free). Report: subagent-artifacts/.../stroke-stt-local-candidates.md. Residual: real ASR + CoreS3 e2e.
