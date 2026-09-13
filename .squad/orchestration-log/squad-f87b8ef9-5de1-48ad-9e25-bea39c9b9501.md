

<!-- pi-squad:9cf2d3c5acb908410db6228ea78b925b6556648e81542a30523b63c1b47066ef -->
## animation — stroke-first-cue-startup-gate
Mode: write; outcome: completed
Engine: 009e3702-6fb7-445a-9aee-fb5ca3ec6227

Implemented the bounded first-cue startup gate within the existing 12-path visual-only artifact. Only six existing artifact files changed: stroke_order_ui_action.h, stroke_order_view.cc/.h, stroke_order_ui_fence_harness.cc, test_stroke_interaction_systems.py, and the interaction documentation.

Fresh Candidate/Replay/successful RetryLoad now creates one pending bool. The first successfully admitted automatic callback consumes it, rebases timing, clears fractional debt, calls no Controller::Tick, and redraws exact cue0. Rejected admission preserves all state. Pre-arm Pause/Resume preserves the token; ordinary Resume/Sync and later phases do not rearm it. Accepted Step supersedes it; teardown clears it. RenderAnimationPage preserves the token through StopAnimTimer(false) and draws before starting the timer. No synchronous refresh, sound, queue, allocation, or scope expansion was introduced.

TDD evidence: before production changes, +160ms and +5s first-callback regressions each aborted -6 under both TSAN and UBSAN with stroke0/progress1000/done1/gap1. The exact cue0 assertion remains. Green focused interaction/sanitizer/render suite: 12 passed; existing UI/controller suite: 12 passed. Shared and clean-HEAD+patch full host suites: 139 passed each, without skips. Five temporary mutations—disable token, consume before admission, clear on pre-arm Pause, rearm on Resume, rearm on Sync—all failed retained assertions. Formatting, diff checks, scope checks, source hashes and evidence checksums passed.

Real 顺 now completes in 18 automatic callbacks at exact6Hz/5Hz (3.000s/3.600s), or 86 callbacks at33ms (2.838s). Every cue/full/gap assertion remains, including real and synthetic 1–48 stroke matrices.

Clean artifact-only ESP-IDF v6.0.2 CoreS3 feature-on/off builds, merged images and ZIPs passed with fullclean between variants and the byte-identical prior dependency lock. Compile audits: 7 stroke units on, 0 off; only CoreS3 board factory. Dependency audit: 72 manifests, 10,629 files, zero mismatches. App/assets bytes: on 2,917,584/7,568,207; off 2,868,448/1,664,169. Partition margins, merged slices, ZIP payloads and image validation hashes passed.

Frozen artifact: /tmp/stroke-first-cue-evidence/artifact.patch SHA256 467c07ee8bd1e8db73c44dd14944a6f25330717eaabdb823f90ac671f3f4be4c. SOURCE-SHA256SUMS SHA256 cd92642d7a95a302b5cec6a5085891dc93ad668fd2401e70740ecb6f5a3ef174. Exactly12 paths; protected logical diff zero; required catalog remains 91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4. Unrelated dirty non-Squad files preserved. No staging, commit, push, publication or Squad-state edit.

Feature-on candidate: /tmp/stroke-first-cue-evidence/cores3-on/v2.4.2_m5stack-core-s3.zip SHA256 375307e1e441ed0014b422487f0994e0900453ad04ffe8242a2f94145b1de0f8; merged image SHA256 504cbd759ee209e3e952d42e53af489dd73d115df660738c615f48fd565fcc3c. Feature-off ZIP SHA256 846f8438eb86e24e972f9902fdd5dddfefec2836487bc80ae4667e406f2b2bba.

Full report: /Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/009e3702-6fb7-445a-9aee-fb5ca3ec6227/stroke-first-cue-startup-gate.md. Report SHA256 ad3ad9eb69f9a130c2d6b4b9bdcce458c6d7a9818a7c16b0388d1408af24bb53.

Independent Rai review remains pending. No hardware flashing/testing occurred. The extra scheduling turn and mutable-buffer model do not prove physical LCD flush visibility; first-cue visibility under device load, theme contrast, touch/cancel behavior, ordinary audio behavior and 100-session stability remain hardware gates.
