

<!-- pi-squad:d65d17a422d7c5b53f3a84afe6e45090f6efeb4242da29e10f81543705a7cd25 -->
# squad-6e8fd736-c1a5-4abc-bc2d-ea9c23b35bcc
completed; 1/1 successful jobs.
- stroke-interaction-improvements-animation-review: # Final independent strict review — `stroke-order-interaction-improvements`

## Verdict: APPROVED

I found **no release-blocking source defect** in the frozen 12-file visual-only artifact.

- Blocker: **0**
- Major: **0**
- Minor: **1 test-evidence limitation only**; it is not a production-source defect.

Approval is for the reviewed technical-prototype artifact and matching CoreS3 firmware candidate. It is not physical-hardware acceptance, commercial-data approval, or per-character stroke-order certification.

## Prior timing Major: RESOLVED

The prior loss of pending active elapsed time on Pause is repaired correctly.

1. `main/stroke_order/stroke_order_ui_action.h:10-57` defines one display/LVGL-owned `StrokeOrderAnimationClock`. `Settle()` at lines 36-47:
   - computes monotonic delta only for an ordered sample;
   - carries fractional microseconds in `[0,999]`;
   - advances the baseline only after admission;
   - saturates whole milliseconds at `UINT32_MAX` rather than wrapping;
   - calls the bounded controller `Tick()` once;
   - synchronizes terminal/paused state afterward.
2. `StrokeOrderApplyAnimationTick()` at `stroke_order_ui_action.h:61-73` performs the exact nonzero generation, current presentation session, active coordinator phase, and cancel-fence admission before touching the clock or controller.
3. Pause at `stroke_order_ui_action.h:115-130` settles elapsed while the controller is still Animating and while the same coordinator admission remains held, then performs Animating→Paused. If settlement reaches Completed/Error, it returns an admitted result for presentation and does not call invalid `Pause()`.
4. Resume retains the fractional remainder: `Sync(Paused)` clears only the running baseline, while `Sync(Animating)` starts a fresh baseline without clearing the remainder. Paused wall time is therefore excluded. The post-action code at lines 159-164 deliberately resets timing for replay/step/back/exit/candidate/retry, but not for Pause/Resume.
5. `main/stroke_order/stroke_order_view.cc:599-629` resets only on explicit timer teardown and otherwise calls `anim_clock_.Sync`. A running clock is not rebased by presentation sync. `HandleControlLocked` at lines 1214-1261 and `AnimTimerCb` at lines 1439-1464 pass microsecond samples to the same production seam; duplicate timer arithmetic is gone.
6. Candidate/new-playback presentation intentionally invokes `StopAnimTimer()` before constructing the new animation page, then establishes the visible playback baseline through `SyncAnimTimer()`. Replay starts a new baseline at its admitted action timestamp; back, exit, abort, overlay deletion, and new-session teardown clear the clock. Pause presentation preserves the remainder, and Resume presentation cannot rebase the baseline already established by the admitted Resume action.
7. `main/stroke_order/stroke_order_controller.cc:380-436` consumes reveal/gap phases with a bounded loop. For the valid maximum of 48 strokes, the 98-iteration cap exceeds the at-most-95 iterations needed to finish all 48 reveals and 47 gaps, so a saturated delta cannot leave valid glyph elapsed stranded. The fixed reveal and gap constants remain 1000 ms and 160 ms (`stroke_order_controller.h:60-63`).

I found no elapsed loss or double counting across timer miss→Pause→Resume, no paused-wall-time inclusion, no invalid Pause after completion, and no wraparound in the validated large-timestamp path.

## Admission, concurrency, and stale-action review

`main/stroke_order/stroke_round_coordinator.h:143-166` uses `std::try_to_lock`, then rejects zero/wrong generation, inactive phase, raised cancel fence, and illegal phase transitions before invoking the mutation. The coordinator lock remains held across clock settlement, controller/session mutation, and the optional coordinator phase transition. Thus:

- fence-first and replacement-first actions cannot mutate controller/session/clock;
- action-first settlement is linearized before a later fence;
- a benign contention miss changes neither clock baseline nor remainder;
- the LVGL control remains armed after a failed admission;
- timer callbacks and Pause use the same gate;
- rendering and Application abort/start calls occur only after `TryUiAction` releases the coordinator lock.

The effective lock order is LVGL/display ownership → nonblocking coordinator try-lock → controller mutex. I found no reverse path that holds the coordinator mutex while acquiring the display lock: `Application::AbortStrokeRound` retires the coordinator round and releases its route/coordinator locks before `StrokeOrderView::AbortFromMain`. Timer/action mutation callbacks do not call Application or re-enter coordinator methods. Controller work is bounded, and no queue or retry loop was added.

## Reconfirmed requirements

- **Visual-only feedback:** `main/stroke_order/stroke_order_view.cc:77-89` applies LVGL `LV_STATE_PRESSED` background, border color, and 3 px border styling through the shared `StyleControl`. Candidate and playback controls use that style. No sound or audio feedback path appears in the 12-file patch.
- **Controls during animation:** Pause/continue, step, replay, back, and exit remain clickable on the animation page. Dispatch is only `LV_EVENT_CLICKED`; no repeated press handler or unbounded action queue exists.
- **Exact stale/fence behavior:** candidate, control, and animation tick paths all use the exact presented generation plus current `StrokeOrderSession` and coordinator fence admission. Candidate and Exit disarm only after success.
- **Benign contention:** rejected try-lock attempts do not disarm controls or change state/clock; later touches/ticks can retry normally.
- **Timeout lifecycle:** `ShowSpeechTimedOutFromMain` closes the already-retired session/overlay and reevaluates the SO entry, then shows a notification. It does not construct an unusable generation-0 retry page.
- **Controller timing:** every reveal is fixed at 1000 ms, the 160 ms gap is separate, delayed ticks carry remainder, paused ticks are inert, and large deltas are bounded.
- **Scope:** the frozen patch contains exactly the 12 paths listed in `/tmp/stroke-animation-evidence/artifact-files.txt`. Inspection of every `diff --git` entry confirms there are no Application, audio, codec, board, CMake, or default-assets changes. The final isolated status lists those same 12 paths.
- **Catalog:** `.gitignore:18-20` adds only the exact `prototype_2000/stroke_cat.bin` exception. `runtime.json:2-12` identifies the 24,352-byte SCB1 catalog, SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4`, 2000 characters, and prototype/non-commercial flags. `SHA256SUMS` records the same digest. `NOTICE.md` retains the pinned Hanzi Writer Data commit, APL and Unicode provenance, transcription caveat, and explicit non-release/non-commercial limitations.

## Deterministic harness assessment

`scripts/tests/stroke_order_ui_fence_harness.cc:62-86` creates real coordinator mutex contention with condition-variable barriers. `TestPauseClock` at lines 89-231 covers:

- accepted tick, forced timer miss, then Pause before another accepted tick;
- 600.600 ms + 399.399 ms fractional carry and the next 1 µs crossing exactly 1000 ms;
- 159.999 ms gap and the next 1 µs crossing exactly 160 ms;
- settlement entering a gap, crossing a gap, and completing the final glyph;
- `UINT64_MAX`-scale elapsed and equal/backward samples;
- Pause admission miss followed by a later retry with unchanged baseline/remainder;
- fence-first and presentation-session-stale rejection;
- action-first Pause followed by fenced Resume;
- replay/back clock reset.

The harness uses the actual production clock/action/tick helpers, controller, session, and coordinator rather than a copied timing model. Mutation-sensitivity evidence shows that removing Pause settlement fails the 600 ms assertion and clearing paused remainder fails the 600 µs assertion.

### Minor — test/model limitation, non-blocking

`scripts/tests/stroke_order_ui_fence_harness.cc:89-231` invokes the production helpers directly and manually calls `clock.Sync` to emulate presentation; it does not compile or execute `StrokeOrderView` against a live LVGL scheduler. `scripts/tests/test_stroke_interaction_systems.py:39-77` couples the real view to the helper using source-shape assertions, and the ESP-IDF build compiles the actual view, but neither proves real LVGL timer ordering, deletion callbacks, or FT6336 scheduling dynamically. Static inspection of `SyncAnimTimer`, control callbacks, timer callback, and teardown found their wiring correct, so this is not a source blocker. A future LVGL integration/fake-timer test would strengthen regression coverage.

## Retained validation evidence reviewed

These results are **retained author-run evidence**, not commands rerun by this reviewer:

- focused interaction/TSAN suite: **5 passed**;
- focused UI suite: **12 passed**;
- shared full host suite: **132 passed, no skips**;
- clean HEAD + frozen patch isolated suite: **132 passed, no skips**;
- additional production helper/controller/coordinator harness under UBSAN: both PASS markers, no sanitizer failure;
- mutation sensitivity: both deliberately damaged clock variants failed at the expected assertions;
- clang-format 19.1.7 scoped dry-run and artifact `git diff --check`: pass;
- dependency audit: **72 manifests / 10,629 files / zero mismatches**;
- clean artifact-only ESP-IDF **v6.0.2** CoreS3 feature-on and feature-off builds, with fullclean between variants, successful image creation, partition checks, merge-bin, and ZIP creation;
- feature-on compiled seven stroke units; feature-off compiled zero;
- feature-on app/assets sizes: 2,918,080 / 7,568,207 bytes, with 1,210,688 / 820,401 bytes spare;
- feature-on ZIP SHA-256: `9a0de13a42a16676b068d0b1938d73f9866519f8a19f5f77f3aed251d6881650`;
- feature-on merged image SHA-256: `e9327d9ce781d8981a3237d3fc1dd64a145efcdb89dab5e1673e04a6c7921068`;
- feature-off ZIP SHA-256: `dd613614accc9fcb50e368207b14881031b71548fb394e7d74ba1beb1eb2d358`.

The expected frozen patch digest `2686ac777f77fc887d0ca409dbaabbdf1982c4217bac7852f08603ad8609deb5` is recorded in the retained `EVIDENCE-SHA256SUMS`; the 12 source hashes are consistently reported for shared and isolated checkouts. I inspected the complete patch, actual shared source, key isolated source, manifests, and logs. Because this review environment exposes read/search tools only, I did not independently execute tests/builds or recompute digests. The expected negative-test message `assets safety margin too small: 262143 < 262144` appears after the full-suite `OK` and belongs to the intentional boundary-rejection test, not to either firmware build.

## Remaining acceptance gates

- Flash and exercise the feature-on image on M5Stack CoreS3.
- Verify pressed-state contrast in both themes, animation-time touch responsiveness, ≤100 ms perceived feedback, approximately 30 FPS target / 20 FPS sustained floor, rapid Pause/Resume/step/replay/back/exit, and real cancel/new-session races.
- Run the 100-session heap/timer/touch/WDT/audio-underrun stability gate and confirm no button sound plus unchanged ordinary capture/TTS/wake behavior.
- Do not treat the 2000-character corpus as a release or commercial library until the documented APL/transcription/Unicode review and manual per-character stroke-order verification are complete.
