# Stroke-order interaction improvements (visual feedback, no audio)

## Final scope

The user superseded the best-effort sound proposal with **visual-only feedback**.
Candidate and playback-control buttons use LVGL's built-in `LV_STATE_PRESSED`:
a tinted background, contrasting border, and 3 px pressed border. Release restores
the ordinary style. This is acknowledgement of pointer contact, **not** a promise
that the action was admitted; a stale/fenced click may show the press style but
cannot mutate the stroke round. There is no acknowledgement sound, audio request,
feedback queue, feedback timer, or deferred feedback event.

This artifact intentionally leaves Application, AudioService, AudioEngine, AFE,
wake-word engines, AudioCodec and every board implementation unchanged from
`7b455c74149d401e88fa729306391c10d9e73f0b`. Earlier broad shutdown, codec I/O,
playback-slot and capture-admission changes are not part of this feature.
Ordinary TTS/capture/output priority is unaffected because the overlay does not
submit audio work at all. There is no audio busy/admission policy to maintain.

## Timing and actions

- The latest UX supersedes adaptive progressive reveal. Every valid stroke has
  a **150 ms start-cue hold**, then its full contour snaps visible. There is no
  partial median/path drawing. The marker stays at the first median point in
  the theme accent color throughout the cue. All completed contours remain in
  theme text color; the faint full-glyph reference outlines remain unchanged.
- The separate gap remains **160 ms**, with no gap after the final stroke.
  During the gap the just-completed contour is visible, with no active marker.
  The next boundary enters the next stroke at cue elapsed/progress **zero**.
  LVGL's requested timer period remains **33 ms**. Delays may lengthen holds;
  order and visible start cues take precedence over wall-clock targets.
- A fresh Candidate, Replay or successful RetryLoad arms one display-owned
  `first_frame_pending` token. The first successfully admitted automatic callback
  only consumes it, rebases the clock and discards all old elapsed/fraction debt:
  **no Controller::Tick**, exact stroke0/progress0/done0/not-gap, then redraw.
  Even a first callback delayed 160ms or 5s cannot replace cue0 with a full stroke.
  Later callbacks (and post-arm Pause) credit at most **160 ms**, rebase
  the monotonic clock to that sample, and **discards old whole-ms backlog**.
  Only sub-ms remainder survives. Equal/backwards timestamps invent no time.
- Public `Controller::Tick()` applies the same cap and handles **only the phase
  it started in**, with no loop or recursion. Cue completion marks exactly its
  stroke complete and enters gap elapsed zero (or Completed), then returns.
  Gap completion advances exactly one stroke to cue elapsed zero, then returns.
  Both boundaries discard surplus. No new phase consumes time on its entry turn.
  This remains true for `UINT32_MAX` deltas, and for timer/Pause settlement of
  a `UINT64_MAX` timestamp. There is no count-adaptive helper or timing policy.
- Each automatic stroke therefore has a prior start-cue presentation, followed
  by its full-contour/gap presentation (Completed on the last stroke). Each
  update advances phase/index/completed count by at most one. Explicit Step is
  also **phase-local**: from cue it completes only the current stroke and pauses
  on that same full-contour/gap at gap elapsed zero; from gap it advances exactly
  once and pauses on the next cue at elapsed/progress zero. A separate Step is
  required to complete that new cue. The final cue completes the glyph with no
  trailing gap. Each admitted Step resets the animation clock and presents the
  resulting phase before returning; it never settles pending timer debt first.
  Invalid indices or an impossible final gap reject without mutation or debounce
  consumption. After startup arming, a 5s timer delay from cue zero completes only that stroke, not its
  gap; the next +33ms credits only 33ms of the gap, with no debt.
- Pause/continue, step, replay, back and exit stay clickable while animating.
  Pause/resume/replay preserve controller idempotence; step retains its 120 ms
  debounce. LVGL dispatch is only `LV_EVENT_CLICKED`, not repeated press events.
  No action queue is introduced.
- The display lock owns the controller/presentation session. A nonblocking
  coordinator try-lock admits an action only for the exact current nonzero
  generation, active phase, matching presentation session and absent cancel
  fence. Controller/session mutation and coordinator phase transition linearize
  together against asynchronous fence publication.
- A fence-first stale action returns before controller/session/coordinator
  mutation, rendering or an Application abort/start request. If the action wins,
  cancellation orders after its coordinator-owned mutation. Rendering runs after
  the coordinator lock is released to avoid re-entrant lock acquisition.
- Animation ticks and Pause use the same `StrokeOrderAnimationClock` and
  coordinator fence. A benign try-lock miss (timer or Pause) changes neither
  baseline nor sub-millisecond remainder and leaves controls usable. Pause
  settles zero while the fresh token is pending, preserving it across Pause/Resume.
  After arming, Pause settles at most one visual quantum **inside the admission that changes
  Animating to Paused**, including after a missed timer callback. It then discards
  old whole-ms backlog. Settlement may enter only the adjacent phase at zero elapsed; completion is
  valid only from the final cue and presents Completed without forcing
  an invalid Pause or returning a misleading rejection after mutation.
- The clock retains the fractional active millisecond while Paused. Resume starts
  a fresh monotonic baseline at the admitted action timestamp, carrying that
  fraction but excluding all paused wall time. Presentation sync never rebases a
  running clock. Replay, step, back, exit, new playback/surface teardown and
  completed/error states reset timing as appropriate. Equal/backwards timer
  samples never invent a nominal tick; even a `UINT64_MAX` sample credits at most
  one quantum without wrapping, and calls the bounded controller only once.
- Candidate buttons disarm only after successful admission and before rendering
  can delete them. Exit disarms only after successful handling (actual deletion
  remains with the main-task abort). Benign try-lock rejection leaves buttons
  usable; no re-arm timer or queued retry is needed.
- Speech timeout has already retired its generation. Close the overlay and show
  an ordinary timeout notification, restoring the SO entry for an explicit fresh
  retry. Do not leave an inert generation-0 R/X page, or bypass the generation
  fence to make it clickable. Active-round NoMatch/Error controls keep strict
  admission.

## Start-cue/snap regression and timing

The exact red regression reads real `prototype_2000/so06.sob1` U+987A 顺 and
asserts 9 strokes. With Candidate at monotonic 1,000,000us and a Tick at +160ms,
the old progressive implementation produced stroke0/progress420/done0. Both
TSAN and UBSAN runs aborted (-6) on the retained assertion: `after one start-cue
interval the current stroke should snap complete`. The render regression also
failed on the old `DrawMedianReveal` implementation before production changes.

That earlier start-cue repair used 17 admitted callbacks, but did not protect
the first mutable canvas from being overwritten before LVGL refreshed it. The
startup-gate repair below supersedes that initial-callback behavior: callback 1
leaves cue0 unchanged, callback 2 at +160ms completes stroke0 into gap0, callback 3
enters stroke1/progress0. Nine strokes now require **18** saturated callbacks. Assertions
require a cue snapshot before every completion, gap before every index advance,
new cue progress exactly zero, and at most one phase/index/completed increment.

Measured deterministic host whole-glyph timing (seconds from selection):

| Delivered interval | 一 (1) | 人 (2) | 口 (3) | 顺 (9) |
| --- | ---: | ---: | ---: | ---: |
| 33 ms | 0.198 | 0.528 | 0.858 | 2.838 (86 callbacks) |
| 50 ms | 0.200 | 0.550 | 0.900 | 3.000 |
| 100 ms | 0.300 | 0.700 | 1.100 | 3.500 |
| 150 ms | 0.300 | 0.750 | 1.200 | 3.900 |
| 160 ms | 0.320 | 0.640 | 0.960 | 2.880 (18 callbacks) |
| exact 6 Hz | 0.333334 | 0.666667 | 1.000000 | 3.000000 (18 callbacks) |
| 200 ms / 5 Hz | 0.400 | 0.800 | 1.200 | 3.600 (18 callbacks) |

For integral-ms fixed cadence `I`, let `Q=min(I,160)`; callback count is
`1+N*ceil(150/Q)+(N-1)*ceil(160/Q)` (one startup arm turn). Exact 6Hz uses samples
`ceil(frame*1000000/6)` microseconds. Nominal credited time is
`150*N+160*(N-1)` (2.630s for 顺), but callback quantization, cap and boundary
surplus discard intentionally lengthen it. At 150ms cadence a 160ms gap takes
two callbacks, so wall time is longer than at 160ms cadence. These are state
presentation opportunities, not physical LCD flush receipts or FPS measurements.

## Phase-local Step blocker repair

Two independent production-helper red tests used real two-stroke 人 before the
Step repair. Cue-Step produced `stroke=1 progress=0 done=1 gap=0`, skipping its
full/gap presentation. Gap-Step produced `stroke=1 progress=1000 done=2` and
Completed, skipping the next cue entirely. Both exact UBSAN tests failed with
assertion aborts; the focused TSAN/UBSAN/controller/render/UI run had five expected
Step assertion failures and passing marker-only/source checks before production
was edited.

The repair is confined to `StrokeOrderController::StepForward()`: validate first,
then return immediately after either gap→next-cue or cue→full/gap (Completed for
the final cue). The existing 120ms debounce, state lock, production action helper,
clock reset/sync, and synchronous view redraw already provide the required
admission and presentation; they need no behavioral change for this repair.
Automatic Tick/cue150/gap160/cap160 is unchanged. The subsequent startup-gate
repair adds one arm-only callback to the real 顺 regression (17 becomes 18).

Retained tests cover both exact cases in Animating and Paused, plus real 人/顺
and synthetic counts 1–48 through every manual phase. Each completion requires
a prior cue snapshot, each index advance a full/gap snapshot, and every accepted
Step changes phase by exactly one and index/completed count by at most one.
They check final completion, zero/backwards/119ms rejection and exact 120ms
acceptance, Replay debounce reset, coordinator contention and same-time retry,
fence-first and action-first/fence-next ordering, stale coordinator/session
rejection, and unchanged controller/session/clock state on rejected actions.
Near-boundary cue/gap Steps followed by Resume prove fresh phase elapsed zero,
no carried fraction, no pending-timer debt, and no paused-wall-time credit.

## First-cue startup gate (delayed-first-callback blocker repair)

Before production edits, the retained production-helper regression mirrored
Candidate -> RenderAnimationPage's StopAnimTimer Reset + Sync at t0, followed
by the first timer at t0+160ms or t0+5s. All four exact runs (both delays under
TSAN and UBSAN) aborted with return -6: stroke0/progress1000/done1/gap1 instead
of exact cue0. Existing marker-only rendering tests passed: state catch-up before
the first physical refresh, not progressive drawing, was the missed boundary.

`StrokeOrderAnimationClock::BeginPlayback()` now resets timing and sets one bool.
It is called only after successful Candidate/Replay/RetryLoad admission. The
first admitted `OnTimer()` clears that bool, sets baseline to its own timestamp,
clears fraction, and returns without Tick. Further callbacks use the unchanged
phase-local settlement. Rejected try-lock/generation/session/fence/state checks
cannot touch token, baseline, fraction or controller. Equal/backwards samples
after arming credit zero. No queue, epoch allocation or elapsed backlog exists.

The new-page path calls `StopAnimTimer(false)` to preserve the admission-owned
token; it draws cue0 **before** SyncAnimTimer creates/resumes the timer. Replay
uses the existing page; ordinary Sync preserves the token and the admitted
handler redraws cue0 before returning to LVGL. The display/LVGL lock serializes
these paths. Rendering is outside the coordinator lock. Default StopAnimTimer
still resets the clock on Back/Exit/delete/new-session teardown. No synchronous
`lv_refr_now` or lock-held rendering was added.

Pre-arm Pause credits zero and retains pending; Resume excludes paused time but
does not consume/rearm it. Its first successful timer still only arms. Post-arm
Pause preserves sub-ms fractions and Resume never rearms. Accepted Step clears
the pending token and performs its existing single-boundary action; a rejected
Step does neither. Replay always creates a new token. Failed RetryLoad and
NoMatch/RetryVoice do not start animation or mint a token. Ordinary Sync/redraw,
later cue/gap boundaries and later timer callbacks never create tokens.

Retained regressions cover first delays 33/149/150/160ms/5s, then cue intervals
150/160ms; fresh Candidate, Back->different Candidate, Replay in Animating/Paused/
Completed, RetryLoad in Candidates and LocalPlayback; real coordinator miss at
+5s with same-time retry; pre-arm Pause/Resume; stale generation/session, fence,
invalid state, Step supersession, teardown and final one-stroke completion.
A minimal mutable-canvas/delayed-refresh model also verifies that the first
callback leaves the buffer at cue0. It is not an LVGL or physical flush proof.
The independent oracle still checks every later cue/full/gap with no extra arm
turns and all prior phase-local Step/debounce/fence assertions remain.

## Required prototype catalog

`../scripts/tests/fixtures/stroke_order/prototype_2000/stroke_cat.bin` is the SCB1
catalog locating the 2000-character prototype across the eight existing SOB1
shards. HEAD's feature-on CMake/package path requires it. Its SHA-256 is:

```
91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4
```

The root `*.bin` ignore rule previously hid this required input from ordinary
untracked-file inventories, so it was missing in clean artifact exports. An exact
path exception now exposes this one catalog without unignoring firmware binaries.
The artifact contains the existing catalog bytes, not a regenerated or expanded
character set. Prototype provenance/licensing remains in that fixture's NOTICE,
ARPHICPL.TXT and existing manifests; this is not a certified release character set.

## Validation and hardware boundaries

The controller harness covers exact cue/gap boundaries, repeated oversized
public Tick calls, zero debt, final completion, pause/resume, explicit Step with
120ms debounce, Replay and controls. The production clock/action/tick helpers
run with the real controller, session and coordinator under TSAN and UBSAN.
Real coordinator contention is deterministic via thread barriers, without sleeps.

The real 一/人/口/顺 and all synthetic counts **1–48** pass an independent
phase-local oracle at 33/50/100/150/160ms/exact6Hz/200ms, repeated5s and huge-delta
cadences. The oracle requires phase overflow to be discarded, not accumulated
as total glyph credit. Every automatic stroke has a preceding zero-progress cue;
every non-final stroke has a full-contour gap. Same/backwards timestamps cannot
spend debt. Synthetic vectors run through the validating store and are never
packaged as corpus data. Python validates catalog U+987A -> `so06.bin`, matching
size/CRC with actual `so06.sob1` and verifying its nine-stroke record.

Other cases cover Pause during cue/gap and near/exact boundaries, timer miss ->
Pause and Pause miss/retry, fractional carry across Resume, paused-wall-time
exclusion, Replay reset, explicit Step/debounce, final-stroke Pause completion,
UINT64_MAX samples, fence-first/session-stale rejection, and action-first followed
by fenced Resume. Timeout/fresh-entry and all existing action/fence checks remain.
Source/render tests require marker-only active drawing, no `DrawMedianReveal`,
reference vs completed color roles, synchronous post-admission canvas redraw,
pressed styles, single-click dispatch and no audio integration.

Run:

```sh
python3 -m unittest discover -s scripts/tests -p 'test_stroke_order_ui.py' -v
python3 -m unittest discover -s scripts/tests -p 'test_stroke_interaction_systems.py' -v
python3 -m unittest discover -s scripts/tests -v
```

Independent review and physical CoreS3 validation remain required. In particular,
verify pressed-style contrast in both themes, rapid press/release during 30 FPS
animation, touch feedback latency, candidate/Exit retry after benign contention,
cancel/new-dialogue races, and 100-session heap/timer stability. Firmware compile
success and host tests do not demonstrate touch latency, sustained FPS, audio
quality, or real-time scheduling on hardware. No sound is expected from these
buttons in the final scope.

The host checks prove sequential **state/presentation opportunities**, not actual
LCD flush completion. The LVGL canvas/flush pipeline may coalesce redraws under
load; verify on CoreS3 that each start marker and subsequent full-contour snap is physically visible. The complete
light reference glyph is intentional and is not completed-stroke playback. Check
both themes and distinguish those reference contours from completed contours and
the current accent start marker; outline perception can otherwise look
like the whole glyph appeared at once. There is no panel acknowledgement or
flush-gated pacing in this narrow artifact.
