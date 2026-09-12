# StrokeOrder local prototype

Owning layer for the CoreS3 **local-first** 笔划 prototype: SOB1 parser,
session controller, lifecycle gate, coherent touch sample, and LVGL overlay.
This is not a published 字库 and not the future `type=stroke` protocol.

## Files

- `stroke_order_store.h` / `.cc` — bounded read-only SOB1 parser.
- `stroke_order_controller.h` / `.cc` — owned bounded blob plus Hidden/AwaitingSpeech/Candidates/Loading/Animating/Paused/Completed/Error/NoMatch/TimedOut. Candidates come only from bounded `SetCandidates`.
- `stroke_order_session.h` — main/LVGL presentation state for the current voice round.
- `stroke_round_coordinator.h` / `.cc` — mutex-serialized generation, fresh-channel binding, session-ID route snapshots, retired-session rejection, and 15s/60s monotonic deadlines.
- `stroke_order_parse.h` — strict UTF-8 target parser (单字 / 某字 / 某怎么写 / 某词的某).
- `stroke_order_candidates.h` — extra-candidate provider seam.
- `stroke_order_pinyin.h` / `.cc` — SPY1 owned index + `StrokeOrderPinyinProvider`.
- `stroke_order_alloc.h` — PSRAM-preferring fail-closed owned copies.
- `stroke_order_lifecycle.h` — platform-independent asset/power/device-state/surface gate used by production and host tests. Listen-hold keeps the overlay during Connecting/Listening.
- `stroke_order_touch_input.h` — packed pointer sample and one-action touch sequence tracker used by CoreS3 and host tests.
- `stroke_order_view.h` / `.cc` — CoreS3 overlay, awaiting-speech page, candidate grid, 田字格 animation.
- `stroke_order_layout.h` — 320x240 geometry used by the view and host tests.
- Host conversion: `scripts/convert_stroke_order.py`, `scripts/generate_stroke_order_500.py`, `scripts/stroke_order/`.
- Prototype packaging: `scripts/package_stroke_order_prototype.py` copies the reviewed 500-character pack offline.
- Smoke packaging remains available for host tests: `scripts/package_stroke_order_smoke.py` (一/人/口).
- License and format: `docs/stroke-order-data.md`.

## Enablement

`CONFIG_STROKE_ORDER_LOCAL` is default `n`, CoreS3-only, and excluded from Emote
builds. The clickable entry is shown only when all of these hold: a validated
local `stroke_order.bin`, an LVGL pointer indev, a server transport with usable
session identities, awake power state, intact LVGL surface, and
`kDeviceStateIdle`. Missing/corrupt assets hide the entry and leave ordinary
chat unchanged. If hello or a stroke service message has no bounded valid
`session_id`, that stroke round fails closed and the voice entry is disabled
until the protocol is reinitialized; the firmware never substitutes the
currently stored protocol token for a message identity.

The 500-character prototype is not official certification and not a release 字库.
Missing or corrupt SPY1 keeps the SO entry and exact-only/fallback candidates;
it must not unbind SOB1.

## Store and asset lifetime

`StrokeOrderStore` itself is a non-owning parser. `StrokeOrderController::BindStore`
first enforces `StrokeOrderStore::kMaxFileBytes`, then allocates a PSRAM-capable
owned copy of the complete blob (`MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT` on ESP,
`new[]` on host tests). PSRAM allocation failure is fail-closed: there is no
internal-DRAM fallback, the store stays unbound, and the entry is hidden. SPY1
uses the same bounded owned-copy helper; a missing or corrupt index degrades to
exact-only/fallback candidates without unbinding SOB1. The copy is validated
before bind, so the Assets mmap can be released immediately afterwards.

The runtime additionally enforces an explicit transition:

```text
Assets::UnApplyPartition
  -> StrokeOrderView::SuspendAssets (display lock)
  -> hide entry + clear hit rect + stop timer + delete overlay/cache
  -> controller Unbind
  -> partition munmap

Assets::Apply (new mapping is active, success or failure known)
  -> StrokeOrderView::RebindAssets (display lock)
  -> unconditional GetAssetData + bounded copy + validation + Bind
  -> entry re-evaluation against pointer/power/Idle gates
```

Candidate and loaded glyph drawing use decoded owned vectors. No renderer path
re-reads the Assets mmap.

## Task and lock contract

- LVGL click/timer/delete callbacks execute their StrokeOrder-only controller and
  view changes directly in the LVGL task; they do not use `Application::Schedule`.
- Device-state, asset, power-save, interruption, and shutdown calls take one
  `DisplayLockGuard`. The shared order is **display/LVGL lock -> controller mutex**.
  The touch poll only takes the controller mutex and never takes the display lock.
- `DisplayLockGuard` records acquisition, never unlocks after a failed lock, and
  fails closed instead of allowing unlocked UI access. StrokeOrder lifecycle
  callers also check the exposed acquisition state before touching controller or LVGL state.
- External `LV_EVENT_DELETE` invalidates the lifecycle surface and cancels the
  controller. Only an explicit Attach/assets rebuild can make that surface valid.
- Paused, Completed, and Error are static: their animation timer is paused or
  deleted. Resume/Replay resumes/recreates it; exit/delete are idempotent.
- Step debounce receives a real monotonic millisecond value from
  `esp_timer_get_time()`; timestamp zero is a valid first sample.
- CoreS3 performs FT6336 I2C only on the 20 ms poll path and publishes a single
  acquire/release `atomic<uint32_t>` containing pressed/x/y. One touch sequence
  can produce at most one legacy short-tap action.

## Product boundary

The lifecycle gate requests one main-task abort for unexpected device states,
power save, asset update, external LVGL deletion, shutdown, high-priority alert,
wake/button interruption, channel close, and both timeouts. Abort invalidates
the exact generation first, stops voice processing, clears queued/in-flight
uplink generations, resets and isolates the downlink decoder/playback queues,
and closes the stroke channel.

A stroke round never reuses an existing audio channel. The sequence is:

```text
close/retire old channel -> clear streaming state -> Begin(generation)
  -> Connecting(generation) -> OpenAudioChannel(exact hello session_id)
  -> bind(session_id, generation) -> Listening -> STT snapshot(session_id, epoch)
  -> main revalidation/commit -> stop+drain+retire+close channel -> Candidates
```

Network callbacks route STT/TTS/LLM from the bounded `session_id` carried by
that message; WebSocket and MQTT share this Application parser. Binary audio is
tagged by each transport with the channel identity captured when that exact
WebSocket/UDP callback was installed. A route snapshot contains session ID,
generation, and route epoch and is revalidated on main before side effects.
Old/missing/wrong stroke output is dropped or fails the current round closed;
a subsequently opened and bound normal session passes normally. Stroke STT is
bounded and UTF-8 validated on the network callback before its text is copied
and scheduled. Candidates expose an X control and expire after 60 seconds.
Ordinary chat `stt` / `glyph_push` / bubbles remain on their existing branch.
This phase does not implement `type=stroke`.
