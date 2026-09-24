# Level1_3500 technical-prototype product runtime

`CONFIG_STROKE_ORDER_LOCAL` remains default off. For feature-on CoreS3 builds
without an explicit dataset selection, Level1_3500 is now the Kconfig default.
Legacy2000 remains an explicit compatibility fallback; its synchronous
SCB1/SPY1/SOB1 View path is unchanged. The default switch does not change fixtures,
formats, source policy, build admission, partitions, audio, protocol, or animation
semantics. The W4b source-interface document describes the earlier host-only
integration; this document describes the integrated product caller.

## Execution and admission

- Application attaches the View before CheckAssets. The Level1 branch defers
  binding until the existing successful `Assets::Apply -> RebindAssets` callback;
  Attach neither constructs/checksums Assets under LVGL nor validates a corpus.
- Assets supplies an immutable owner containing exactly `stroke_cat.bin`,
  `stroke_pinyin.bin`, and `so00.bin` through `so05.bin`. It retains a mapping pin.
  Closing mapping admission prevents subsequent leases. The last lease must be
  released before UnApply can unmap or Download can erase/overwrite flash.
- One FreeRTOS task (16 KiB stack, idle priority) owns a shared portable worker,
  never a View pointer. It polls a fixed mailbox every 20 ms. Only that task calls
  PrepareBundle/PrepareGlyphs; no validation or decompression occurs in LVGL,
  the main task, or audio tasks. Idle priority permits audio/idle/watchdog work
  during a long whole-corpus validation; real latency/stack margins need hardware.
- The worker has one pending command, one completion, one active operation,
  and no callback/event queue. New glyph commands coalesce into the pending slot.
  An epoch invalidates obsolete completions. The adapter owns at most one staged
  bundle and its existing six-glyph transactional raw cache.
- One 33 ms LVGL timer consumes completion metadata. Bundle publication requires
  the current mapping generation and an uncancelled View preparation, then
  Commit + Controller::BindSource before readiness/SO publication. Rebind during
  old work retains one latest lease and retries nonblocking drain for at most
  300 timer turns, failing closed thereafter. No worker waits for LVGL.
- Controller::PlanSourceCandidates uses only source metadata. Primary + ranked
  homophones, or the first six official basic ranks for local fallback, are
  filtered/deduplicated into <=6 exact codepoints. The worker prepares exactly
  those glyphs while the View displays Loading with Exit. Completion must match
  assets/source/round/session and the existing coordinator cancel fence.
  A contended TryUiAction retains one completion for a later timer turn.
- The existing candidate timeout starts while glyph preparation is pending.
  Cancel/replacement/external surface deletion clear pending presentation. Late
  or duplicate results cannot display candidates. Candidate copy and selection
  Acquire only prepared owned raw data and Controller immediately copies/releases.
  No new sound, animation timing, pressed-state, or control-action semantics.

## Suspend, abort, and lifetime

`SuspendAssets` first snapshots the worker owner atomically and closes source
admission/generation before taking the display lock. It unbinds Controller and
invalidates UI, then asks for nonblocking drain. False forbids unmap. An active
borrow keeps its owned raw data valid until Release; in-flight work drops local
leases before clearing the active fence. A successful Suspend releases cache,
staged/published readers and owners.

The existing `Assets::UnApplyPartition` already returns bool. Its callers are
Download and the Assets destructor; no signature change is needed. Download
returns false before erase/write when suspension or the pin gate rejects unload.
It does not wait globally, busy-wait, or automatically retry; a later explicit
attempt may retry. Admission remains closed and the mapping is retained on
failure. The destructor also leaves the mapping intact on failure rather than
forcing an unsafe unmap (process/device shutdown only).

Shutdown deletes the View's polling timer under LVGL, unbinds Controller, closes
and stops the worker, and drops its reference. The task retains only its own
state through completion/drain and destroys it before deleting itself. The source
destructor asserts no worker, borrow, published/staged state, cache, or accepting
admission remains. Display lifetime must continue to encompass the View, as for
its existing LVGL objects.

## Validation and limits

`python3 -m unittest scripts.tests.test_stroke_order_worker -v` exercises the
portable production mailbox with real 3500 data and ASan/UBSan: delayed bundle
and glyph jobs, replacement, cancellation, one-slot coalescing, duplicate/late
completion, generation/cancel fencing, active borrow/worker unload veto, owned
lease release, real metadata candidates/select/load, and 100 sessions without
pending work accumulation. Static integration assertions check Kconfig/worker
branching and the production unload-before-erase gates. Existing source tests
continue to cover the full v1/v2 corpora and adapter interleavings.

Host tests and CoreS3 clean builds have passed. On 2026-09-24 the user confirmed
the SO entry on hardware (bundle prepare 13.364 s, ready=1, entry_eligible=1) and
later reported the tested interactions normal. This is entry/basic-interaction
evidence, not exhaustive hardware acceptance. Remaining checks include candidate
latency, task stack watermark, idle/audio scheduling, PSRAM/internal heap over
100 sessions, task/timer teardown, asset Download failure/retry, pressed feedback,
step/cue ordering, and touch/UI behavior during active playback. The default
switch is not commercial-data, licensing, PDF or stroke-order accuracy
certification; the boundaries in `docs/stroke-order-data.md` remain in force.

## Startup admission and readiness (performance correction)

Level1 `PrepareBundle` now explicitly selects `ValidationMode::Structural`.
This still checks all 64-byte envelopes/header/body CRCs, descriptor file CRCs,
SOB2 index/offset/rank/codepoint/length/stored CRCs, corpus identities, complete
SCB2-to-SOB2 mappings and SPY2 rank/CSR reciprocity. It never parses heatshrink,
DZZ1 or raw geometry. A container with recomputed stored CRCs and invalid glyph
payloads can therefore be admitted as metadata; **this is not a glyph validity
promise**. `PrepareGlyphs` always strictly decodes, checks raw CRC and shared
geometry, and publishes its <=6-glyph replacement cache only on complete success.
A bad glyph fails closed and preserves the prior cache; it is never presented.
Immutable mapped ownership and generation/drain fences are unchanged.

Host product admission (`device_verify.cc`, called by the existing verify/build
script) explicitly uses `Deep`; the default reader mode also remains `Deep`.
That mode performs every structural check plus all 3500 glyph decodes. W4a tests
exercise both modes; no fixture, format or build acceptance criterion changed.
Structural work is O(total stored bytes + bounded metadata/CSR), not O(1) time;
owned allocation **count** is `2*shard_count+6` in the current adapter, with zero
glyph decodes. Legacy2000 validation is unchanged and remains available via an
explicit dataset selection.

Bundle readiness is asset-scoped, not UI-round-scoped. Visual invalidation,
including power save while the initial bundle is preparing, must not discard
that bundle. Completion may establish readiness while sleeping, but the lifecycle
gate keeps SO hidden until wake/idle/pointer readiness. Explicit asset suspend,
rebind, shutdown, stale mapping generation and stale commit tickets still fence
old work and keep unavailable entries hidden. This distinction matters because
the retained pre-fix boot entered power save at 69.163s while its full-corpus
allocation sequence continued to 93.543s.

At INFO, `StrokePrepare` emits one bundle start/end pair with structural stage,
generation, success/reason, elapsed milliseconds, stack high-water mark, and
free/minimum PSRAM/internal heap. `invalid_or_stale` is intentionally an aggregate
reader/admission failure, not a claim about the exact invalid byte. The mailbox
logs discarded bundle completions once; View logs submit/drain/asset/commit/bind
rejections or ready/entry eligibility once per consumed bundle. No pointer values
are printed. Per-owned-allocation and candidate timing logs are DEBUG-only.
Stack high-water units follow ESP-IDF FreeRTOS (bytes on this target).

Regression commands:

```sh
python3 -m unittest scripts.tests.test_stroke_order_prepare -v
python3 -m unittest scripts.tests.test_stroke_order_source scripts.tests.test_stroke_order_worker scripts.tests.test_stroke_order_v2_device -v
```

The first includes a minimal one-shard synthetic 3500 container, the real six-shard
bundle, exact Decode/owned-allocation counters and <2s per-prepare host guard
(compile/setup excluded), with ASan/UBSan. The View harness extracts production
preparation and visual-invalidation methods and tests the worker-to-Commit-to-
BindSource capability chain, sleep/wake during an active prepare, invalid data,
stale assets and stale Commit. Its entry object is a portable stub: the separate
hardware SO entry confirmation above is screen evidence, not a consequence of
this harness. Real LVGL scheduling, latency and 100-session heap/audio/touch
acceptance still require extended device testing.
