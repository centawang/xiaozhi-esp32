

<!-- pi-squad:17d456009e6ff230d48c2659f9f8404ab25929d0f01c8c6b49da9fe3c9268534 -->
# squad-b5cf2513-95db-454e-8fdb-3cf0bb37ea67
completed; 1/1 successful jobs.
- stroke-stt-local-candidates: Implemented CoreS3 local-first 笔划 STT session: Idle entry schedules a main-task voice session (AwaitingSpeech overlay + existing StartListening), intercepts STT only when pending, parses one Hanzi by Unicode code point, SetCandidates (recognized char first, store-only, dedup, max 6; smoke 一/人/口 correctly shows 1 with no fake homophones), then local playback. No type=stroke. Host 98/98 (kept prior 96). Isolated IDF v6.0.2 m5stack-core-s3 CONFIG_STROKE_ORDER_LOCAL=y build produced /tmp/xiaozhi-stroke-stt-local-candidates-build/xiaozhi.bin 2,886,016 bytes (30% free). Report: subagent-artifacts/.../stroke-stt-local-candidates.md. Residual: real ASR + CoreS3 e2e.
