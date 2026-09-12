---
status: accepted
---

# Keep stroke-order interaction separate from audio device state

The first version of the 笔划 feature will use a dedicated `StrokeOrderController` and UI sub-state instead of adding candidate and animation states to `DeviceStateMachine`. The server owns ASR candidate generation, ranking, support filtering, and on-demand stroke data; the device validates bounded input, keeps the selected character data only for the active session, and does not persist a full stroke library.

Feature availability is explicit and defaults off: only builds with a color LVGL display and registered coordinate pointer input advertise support. When candidates appear, the device stops listening and returns the audio state to Idle while retaining the protocol control channel and independent stroke overlay for at most 60 seconds. Wake words, buttons, new sessions, timeouts, and higher-priority system events cancel the overlay before normal processing continues.

## Consequences

This keeps UI interaction orthogonal to voice/network lifecycle and prevents unsupported boards from exposing an unusable feature. It requires a new capability declaration, a bounded controller lifecycle, protocol support for asynchronous selection, and careful cancellation of stale network responses and LVGL callbacks.

## Amendment — local-first CoreS3 prototype

The first implementation is a CoreS3 local dataset plus `StrokeOrderStore`, not a full on-demand server library. The device still does not pack complete `hanzi-writer-data`, does not persist a Flash 字库, and does not add candidate/animation states to `DeviceStateMachine`. Server-side ranking and `type=stroke` data delivery remain the future networked path; locally converted SOB1 data is session/mmap memory for the prototype only. See `docs/adr/0004-local-stroke-order-binary.md`.
