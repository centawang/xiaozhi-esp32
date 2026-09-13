

<!-- pi-squad:ca4b0a20283506a8b0bd21bf2ba5c65295d0a8337a704af6340864a3ab9491ee -->
## rai — stroke-interaction-improvements-systems-review
Mode: review; outcome: completed
Engine: 1c1c9d72-865b-4c38-8b53-8656bc8c0569

# Independent strict review — `stroke-order-interaction-improvements`

## Verdict: **REJECTED**

No Blocker; **3 Major** and **2 Minor** findings. I inspected the actual isolated source under `/tmp/stroke-systems-isolated`, corresponding shared-tree files, the frozen patch, manifests, and retained logs. I made no edits and, because only read tools were available, ran no tests/builds/hashes.

## Findings

### Major 1 — pre-stop AFE PCM can be relabeled as new capture after Stop→Start

**Files:** `main/audio/audio_service.cc:153-176,715-733`; `main/audio/engines/afe_audio_engine.cc:423-450,487-520`.

`AudioService::Start()` joins only the service input/output/Opus tasks. It does not join or obtain a quiescence acknowledgement from the separately running AFE processing task before clearing `service_stopped_`. The AFE worker validates `control_generation_` before entering `HandleVoiceResult()`, but the generation is not carried to `output_callback_`.

A valid interleaving remains: old AFE work passes the checks at lines 423-450 and is descheduled inside/before `HandleVoiceResult`; Stop fences/drains the service; Start joins service workers, disables engine controls, resets codec cancellation, and reopens service admission; a new listening start sets the service processor bit; then the old AFE handler resumes at lines 516/520. `PushTaskToEncodeQueue()` sees a running/current processor and stamps the **current** capture generation at `audio_service.cc:733`. Old PCM can therefore be accepted and sent as new-generation audio.

The retained CoreS3-on sdkconfig has `CONFIG_USE_AUDIO_PROCESSOR=y`, so this is the primary firmware path. `audio_capture_stop_harness.cc` models an in-flight Opus encode, not the production AFE producer/callback, and cannot prove this boundary.

### Major 2 — production AFE VAD state has a cross-task data race

**Files:** `main/audio/engines/afe_audio_engine.h:72`; `main/audio/engines/afe_audio_engine.cc:282-292,487-503`.

`is_speaking_` is a plain `bool`. The application/control task writes it at line 290 while the AFE processing task reads/writes it at lines 495-503. No mutex, atomic, or task-owned deferred reset synchronizes these accesses. This is C++ undefined behavior on the dual-core target and can also produce a stale VAD callback after voice processing is disabled. The retained TSAN harnesses do not compile or execute production `AfeAudioEngine`.

### Major 3 — CoreS3/K10 read errors are reported as complete microphone frames

**Files:** `main/audio/audio_codec.cc:17-21`; `main/boards/m5stack/core-s3/cores3_audio_codec.cc:234-238`; `main/boards/dfrobot/df-k10/k10_audio_codec.cc:178-183`; retained dependency `managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c:713-717`.

The new base `InputData()` exact-size check works only when the virtual `Read()` returns the true count. CoreS3 and K10 call `esp_codec_dev_read()` through `ESP_ERROR_CHECK_WITHOUT_ABORT` and then unconditionally return the requested `samples`. The locked ESP-IDF 6 dependency returns `ESP_CODEC_DEV_DRV_ERR` when its finite 1000 ms I2S read fails, but these codecs discard that result. `InputData()` consequently accepts a partial/zero-filled frame and forwards it to AFE. This violates microphone-frame validity on the actual CoreS3 path and is source-demonstrable, not hardware-only.

### Minor 1 — changed direct-output return semantics remain inconsistent

**Files:** the three changed LilyGO codecs at `tcamerapluss3_audio_codec.cc:152-162`, `tcircles3_audio_codec.cc:132-142`, and `tdisplays3promvsrlora_audio_codec.cc:141-151`, plus `df-k10/k10_audio_codec.cc:192-210`.

The LilyGO methods ignore `WriteI2s()` status/moved bytes and always return the requested count. K10 returns the number of duplicated `int32_t` entries, so a full write reports twice the input sample count. `AudioCodec::OutputData()` currently discards this return, limiting impact, but partial/error semantics are not coherent and timeout truncation is silent.

### Minor 2 — transient coordinator contention can permanently disarm Candidate/Exit

**File:** `main/stroke_order/stroke_order_view.cc:1322-1367`.

`CandidateClicked()` and Exit disarm their LVGL target before calling the intentionally nonblocking `TryUiAction`. A benign try-lock failure leaves the control non-clickable with no re-arm, even when no cancel fence exists. Disarm after successful admission or re-arm non-cancel rejection.

## Prior Major disposition

1. **Stop→Start capture fencing/drain — NOT RESOLVED.** The queue-side work is correct: Stop publishes stopped and bumps both generations under `audio_queue_mutex_`, clears encode/send/decode/testing/timestamps/playback, wakes waiters, and post-Opus send/testing admission validates stopped+capture generation under that lock. Codec cancellation is reset only after service-worker join. But the live AFE producer is not fenced/acknowledged, so old PCM/VAD callbacks can cross restart and be relabeled.
2. **Cancel-fenced UI — RESOLVED for the required controller/coordinator/abort/beep invariant.** `TryUiAction()` holds the coordinator lock, validates exact generation, active phase, transition, and cancel fence before invoking the mutation; `StrokeOrderApplyUiAction()` validates the exact presentation session inside that critical section. Candidate, pause/continue, step, replay, back, exit, and retry routes use it. A second feedback admission suppresses sound when cancellation wins after mutation but before feedback. Fence-first rejection produces no controller/coordinator mutation, abort request, or beep. Exit uses the bounded two-entry handoff. Minor 2 remains a usability issue.
3. **Finite codec I/O — NOT FULLY RESOLVED.** `AudioCodecIo` correctly uses 240-byte chunks, 200 ms per driver wait, cancellation between chunks, and termination on error, impossible moved count, or zero progress. Common NoAudio/PDM and all four changed direct-I2S codec files use the wrappers, and no direct `portMAX_DELAY` remains there. Direct microphone reads reject incomplete frames. However, CoreS3/K10 vendor-read failures are falsely accepted as full frames, and changed output methods do not consistently report partial/error results.

## Other behavior rechecked

- Fixed 1000 ms reveal and separate 160 ms gap are present; `Tick()` carries elapsed time across phases with a bounded loop.
- Controller pause/resume/replay are idempotent; step has 120 ms debounce and no queue.
- CoreS3 touch uses one atomic packed sample and one-action-per-press/release sequencing; consumed/exclusive touches do not fall through to legacy toggle.
- The 40 ms/1600 Hz tone is generated once. UI admission is try-lock, allocation-free, bounded to two fixed slots, rejects while ordinary decode is pending/in flight, and preserves ordinary `unique_ptr` ownership on capacity failure.
- Failed feedback try-lock returns before protected generation/queue reads.
- Final shutdown ordering is substantially sound: Application shuts AudioService before deleting its event group; service workers join before engine shutdown; AFE/custom wake encoder workers signal, park, and join before callback detach and owned-state destruction. This final-shutdown result does not repair restart quiescence.
- Lite-path shutdown is covered structurally after the service input producer joins. CoreS3 compiles the affected AFE path.

## Isolation, binary, and build provenance

Retained audit evidence reports base HEAD `7b455c74149d401e88fa729306391c10d9e73f0b`, an exact **43/43** patch path set, shared/isolated/clean-HEAD-apply source matches against `SOURCE-SHA256SUMS`, and artifact-scoped `git diff --check` success. The patch SHA-256 is reported as `a304b1c2b6c6a8a4e55f5af5d5a12173d436f3856bcf41f15908208c17f4e8bd`.

The path list excludes dirty Stick-S3/dictation work, emoji assets, the 8m partition, `main/CMakeLists.txt`, `build_default_assets.py`, and Squad state. `isolation-audit.log` reports those tracked exclusions unchanged/others absent.

`stroke_cat.bin` is necessary: unchanged `main/CMakeLists.txt:1053-1082` names it as a prototype source dependency and packaged runtime output. The binary patch contains a complete 24,352-byte literal. Retained audits report SHA-256 `91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4` in shared, isolated, and clean-HEAD apply trees, matching committed `prototype_2000/SHA256SUMS` and `runtime.json`. Metadata consistently identifies SCB1 v1, 2,000 characters, eight shards, and prototype/non-commercially-reviewed status.

Retained evidence reports all **63/63** saved payload hashes valid (CoreS3-on 15; four other variants 12 each), dependency audits of 9,300 files and the complete PDM set of 10,629 files with zero mismatches, and builds rooted at `/private/tmp/stroke-systems-isolated` (the `/tmp` worktree path) with ccache disabled. These are retained author results; this reviewer could not independently recompute hashes.

## Retained validation (not reviewer-run)

- Full host log: **134 tests**, **OK**, no skip marker; 32.866 s.
- Focused TSAN log: **3 tests**, **OK**.
- ESP-IDF v6.0.2 clean build/merge logs: CoreS3 feature-on/off, bread-compact ESP32 NoAudio+Lite, doit-s3-aibox PDM+AFE, and DF-K10 direct-I2S+AFE all complete.
- Feature-on image: `/tmp/stroke-systems-evidence/cores3-on/merged-binary.bin`, 15,956,815 bytes, SHA-256 `87cb40ea427534730c0e4f51530adeaad558b2bdd3a68df870c80e7d05058dbc`; app SHA-256 `8227d03f06861314a6c5063d46d340c2fb89977ec9eb7a5532b245a310cf8ece`; assets SHA-256 `54453b5232408dfb8cf8dadbde9dd5d93a1bdb90f213df69d7ff075210783dd0`.
- The canonical CoreS3 ZIP was later overwritten by feature-off validation; only the separately retained `cores3-on/merged-binary.bin` is the feature-on image.

The green tests do not execute the production AFE restart interleaving or CoreS3 `esp_codec_dev_read()` failure path.

## Missing validation and physical residuals

Before re-review: add a deterministic AFE callback/VAD Stop→Start interleaving test; test actual synchronized `is_speaking_` ownership; inject CoreS3/K10 read timeout/error results; compile the three changed LilyGO targets; rerun host/TSAN/build/hash evidence.

Hardware-only work remains: CoreS3 repeated Stop→Start and missing-clock stress; animation controls and 20/30 FPS under load; feedback audibility/latency and ordinary-audio priority; capture/playback/wake/VAD/AEC; 100+ sessions for heap/timer/task leaks, WDT, underrun, and dead touch. Commercial licensing and per-character accuracy review remain outstanding for this explicitly technical prototype.
