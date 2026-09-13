

<!-- pi-squad:2840d07b4187768c0b8ff659b10cf5e29d465305182af500b5a2dc8109ac704e -->
## rai — stroke-animation-speed-review
Mode: review; outcome: completed
Engine: c1388334-acdf-47b2-8736-c3443a056399

# Final independent strict review — 150 ms animation calibration

## Verdict

**APPROVED** for `stroke-order-interaction-improvements`.

- Blocker: **0**
- Major: **0**
- Minor: **0**
- Speed regression: **source-correct and covered by the retained production-shaped red/green test**.
- Skip/burst safety: **source-correct; one settlement cannot cross more than one adjacent boundary or complete an unseen next stroke**.

This was read-only. I inspected the actual shared source, frozen patch, author report, preserved before/final sources, test/mutation logs, build/package audits, and provenance files. I made no edits and did not stage, commit, push, flash, or publish. I had no shell/test runner, so I did not rerun tests or independently calculate hashes; all execution/hash results below are clearly identified as retained author evidence and were cross-checked against source and audit-script logic.

## Source correctness

### Speed and clock

- `main/stroke_order/stroke_order_controller.h:60-67` fixes reveal/gap at 1000/160 ms, retains `kTimerPeriodMs=33`, and separately defines `kMaxAnimationAdvanceMs=150`.
- `main/stroke_order/stroke_order_view.cc:608-628` uses 33 ms at both LVGL create/set-period sites. `main/stroke_order/stroke_order_view.cc:1441-1462` routes real timer timestamps through the tested animation helper.
- `main/stroke_order/stroke_order_ui_action.h:36-46` caps the production clock at 150 ms. Each admitted monotonic sample computes elapsed whole ms plus prior sub-ms carry, rebases `last_tick_us_`, retains only `%1000` remainder, and discards excess whole-ms time. Equal samples add no credit; backward samples preserve baseline/remainder. State is limited to running/baseline/sub-ms remainder—no hidden debt or queue.
- `main/stroke_order/stroke_order_controller.cc:386-454` independently caps public `Tick()` at the same 150 ms, preventing alternate callers from bypassing the clock cap.

### Sequential visibility and edges

- `main/stroke_order/stroke_order_controller.cc:394-399` statically requires the cap below both 160 ms gap and 1000 ms reveal and limits processing to two phase visits.
- Starting in a reveal, at most 149 ms can remain for a newly entered gap; starting in a gap, at most 149 ms can remain for a newly entered reveal. Thus one call crosses at most one boundary and cannot traverse a gap and finish an unseen stroke.
- Exact reveal/gap equality transitions are correct; final completion has no synthetic gap. Zero, same-time, backward, and repeated huge inputs expose no whole-ms debt.
- At the cap, a stroke starting at zero shows 150/300/450/600/750/900 before 1000. A stroke entered with 1–149 ms after a gap still receives at least six positive partial presentations before completion.

## TDD integrity and retained results

`/tmp/stroke-speed-evidence/red-before.log` records the exact six-test interaction suite before the production calibration: both TSAN and UBSAN cases failed with return **-6**, while the other four tests passed. Seven rational-6Hz callbacks produced `33/66/99/132/165/198/231`, frame 7 remained `completed=0`, and both failures stopped at the exact assertion retained in `scripts/tests/stroke_order_ui_fence_harness.cc:156-159`: **“first stroke should complete by about 1.17s”**. Preserved prior source under `before/` has both clock/controller capped at 33 ms, consistent with this trace. Chronology is retained evidence, not live-observed by this reviewer.

The assertion remains verbatim and is not weakened. `final-tsan.log` and `final-ubsan.log` return 0 with `150/300/450/600/750/900/1000`; first completion is frame 7 at 1,166,667 us.

The cadence matrix at `stroke_order_ui_fence_harness.cc:126-169` asserts no earlier callback completion, exact final callback/time, and completion no earlier than 1000 ms. Retained results are:

- 33 ms → 1023 ms
- 50 ms → 1000 ms
- 100 ms → 1000 ms
- 150 ms → 1050 ms
- rational 6 Hz → 1166.667 ms
- 5 Hz → 1400 ms

These match `ceil(1000/min(interval,150)) × interval`.

`stroke_order_ui_fence_harness.cc:30-39,73-76,267-307` checks completed-count/phase adjacency for each admitted settlement. Its 一/人/口 matrix covers 33/50/100/150/166.667/200 ms and huge deltas, requires at least six earlier positive partial snapshots per automatic stroke, and records a gap snapshot before every non-final index advance. It also covers single/final strokes, exact edges, zero/near-reveal/near-gap stalls, same/backward timestamps, `UINT64_MAX`, repeated huge deltas, timer miss/retry, miss→Pause, Pause/Resume fractional carry, paused-time exclusion, Replay, Step/debounce, final completion during Pause settlement, fences, stale sessions, and action-first/fence-after ordering.

Mutation evidence is substantive: either layer left at 33 ms fails at frame-7 progress 231; removing the public cap yields 28 controller failures; removing Pause settlement, losing fractional carry, or retaining baseline/backlog also fails. Unmodified TSAN/UBSAN variants return 0.

## Controls, fences, and visual-only scope

- `stroke_order_ui_action.h:62-151` puts timers and controls inside exact nonzero generation/session/cancel-fence admission. Pause settlement and state change share that admission; failed try-lock/stale/fenced actions mutate neither timing nor controller state.
- Replay/non-Pause actions reset and rebase appropriately; Resume retains fractional carry while excluding paused wall time. Step remains a single 120 ms-debounced manual action with no queue.
- `stroke_round_coordinator.h:142-166` rejects contention, wrong generation, inactive phase, and cancel-fenced generations before mutation.
- `stroke_order_view.cc:1214-1359` keeps controls usable during animation and admits before rendering/disarm/abort effects.
- `stroke_order_view.cc:76-89` uses LVGL pressed background/border styles; registrations use `LV_EVENT_CLICKED`. No sound, audio event, feedback queue, or feedback timer exists.
- Timeout closes the retired overlay and restores the ordinary fresh SO entry instead of exposing generation-0 retry controls.

No control/fence/timeout/visual regression requiring rejection was found.

## Artifact, provenance, and retained build evidence

- `EVIDENCE-SHA256SUMS` and `freeze.log` identify `/tmp/stroke-speed-evidence/artifact.patch` as **`c2600b31798f817f364688575901657a4b30df9bd795b2e2b4343ba8de1be1aa`**. Its content is consistent with inspected shared source.
- The manifest has exactly 12 paths. `final-scope-audit.log` reports a clean-HEAD isolated checkout, the exact 12-path set, matching shared/isolated source hashes, successful reverse-apply/whitespace checks, 29 unrelated dirty files unchanged, and no logical diff in Application/audio/board/CMake/default-assets. Calibration itself changed eight existing artifact paths.
- Catalog SHA remains **`91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`**. `runtime.json` records 2000 characters/eight shards. NOTICE/source locks retain Hanzi Writer Data commit `68d10a4b21150cae5e1ebbd223eed289cf32d90c`, Arphic licensing, transcription provenance, and explicit prototype/non-release/non-commercially-reviewed status. This review is not commercial-release legal approval.
- Retained focused results: interaction **6/6**, UI **12/12**, TSAN/UBSAN successful.
- Retained full results: shared **133/133** (74.227 s) and isolated clean-HEAD+patch **133/133** (73.947 s), exit 0, `OK`, no skips.
- Retained clean build evidence: ESP-IDF **v6.0.2**, ESP32-S3/CoreS3, no initial build directory and fullclean between feature-on/off; seven stroke units on, zero off; only CoreS3 board factory/codec; 72 dependency manifests/10,629 files with zero mismatches.
- Feature-on app/assets: **2,918,096 / 7,568,207 bytes**. ZIP: **`d735c48e39159baf5c1af1f88edb0bcb7dfafe26dc0be36db0dde6d2bd598f74`**; merged: **`ac14ea56c9536988814515b94c1c6bbc0f5fb46de9810218c4bea4eb2b42ea07`**. Feature-off build/merge/ZIP also passed. Slice, ZIP payload, partition, and ESP image checksum/validation audits passed.

## Remaining hardware gates

No CoreS3 was flashed for this calibration. Approval covers source and retained evidence, not physical performance. Still required:

1. Measure actual active-reveal duration and confirm every stroke remains visibly sequential.
2. Distinguish the intentional faint full reference outline from completed contours and current accent/start marker in both themes.
3. Confirm LVGL invalidation/panel flush does not coalesce host-proven presentation opportunities.
4. Verify FT6336/control responsiveness and pressed contrast during animation, rapid controls, cancel/new-dialogue races, ordinary audio behavior, sustained FPS, and 100-session heap/timer/WDT stability.

A physical speed, panel-flush, or reference-outline perception failure needs follow-up, but no remaining source defect, early completion, double-boundary transition, backlog, control/fence regression, or scope/provenance mismatch was found.
