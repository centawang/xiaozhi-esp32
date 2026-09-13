# Animation: project knowledge

单片机设备开发

<!-- pi-squad:036c21c961b7259f84db01eedb01af7959279c75ee1308c45d7a8d122d5e9b36 -->
## 2026-09-13T05:17:55.188Z — stroke-interaction-improvements-pause-timing

Implemented the pending-elapsed Pause repair within the existing 12-file visual-only artifact; only six existing artifact files changed in this repair. Pause now settles monotonic elapsed through Controller::Tick inside the same exact-generation/session/cancel-fence coordinator admission that performs Animating→Paused. A shared production clock preserves fractional microseconds across Pause→Resume, excludes paused wall time, retains baseline/remainder on rejected admission, and accepts/presents completion reached during settlement without forcing an invalid Pause. No sound, Application/audio/codec/board/CMake/default-assets changes were introduced.

Changed repair paths: main/stroke_order/stroke_order_ui_action.h; main/stroke_order/stroke_order_view.h; main/stroke_order/stroke_order_view.cc; scripts/tests/stroke_order_ui_fence_harness.cc; scripts/tests/test_stroke_interaction_systems.py; docs/stroke-order-interaction-improvements.md.

Validation: focused interaction/TSAN suite 5 passed; focused UI suite 12 passed; shared full host suite 132 passed without skips; isolated clean-HEAD+patch full host suite 132 passed without skips. Additional production harness UBSAN run passed. Deterministic real-coordinator contention covers timer miss→Pause before another tick, exact 1000 ms reveal/160 ms gap boundaries, sub-ms carry, gap crossing, final-glyph completion, Pause miss/retry, fence-first/session-stale rejection, and action-first Pause followed by fenced Resume. Temporary mutation sensitivity checks correctly failed when settlement or remainder retention was removed. Touched-range clang-format checks and artifact-scoped git diff --check passed.

Frozen evidence: /tmp/stroke-animation-evidence/artifact.patch, SHA256 2686ac777f77fc887d0ca409dbaabbdf1982c4217bac7852f08603ad8609deb5. SOURCE-SHA256SUMS SHA256 665cefc731b2be803b6106199aa77437e6954d770e3d740c52d75bc4af9ad5b7. Exactly 12 artifact paths, based on HEAD 7b455c74149d401e88fa729306391c10d9e73f0b; required catalog SHA256 remains 91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4. Final shared/isolated source hashes match; logical protected-file diff is zero; unrelated dirty non-Squad file hashes are unchanged. No Squad state edits, staging, commits, pushes, or publishing.

Clean artifact-only ESP-IDF v6.0.2 CoreS3 feature-on and feature-off builds, merge images, and ZIPs passed in /tmp/stroke-animation-isolated, with fullclean between variants. Compile audit found 7 stroke units on and 0 off, with only the CoreS3 board factory. On app/assets sizes: 2,918,080 / 7,568,207 bytes; off: 2,868,448 / 1,664,169 bytes. Both fit partitions and asset safety margins. Dependency audit verified 72 manifests and 10,629 files with zero mismatches. Merged slices, ZIP payloads, image checksums, and final evidence hashes verified.

Feature-on candidate: /tmp/stroke-animation-evidence/cores3-on/v2.4.2_m5stack-core-s3.zip SHA256 9a0de13a42a16676b068d0b1938d73f9866519f8a19f5f77f3aed251d6881650; merged image SHA256 e9327d9ce781d8981a3237d3fc1dd64a145efcdb89dab5e1673e04a6c7921068. Feature-off ZIP SHA256 dd613614accc9fcb50e368207b14881031b71548fb394e7d74ba1beb1eb2d358.

Full findings with exact hunks, algorithm, commands, hashes, and hardware residuals written to /Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/77e93c3b-e04b-4ff6-a869-9d86277cdc02/stroke-interaction-improvements-pause-timing.md. Report SHA256: 4d18a2b412e2675ee0f8906c3245475e4c7af37071e2994129cb524ec0958a5f.

Independent Rai review remains pending; no approval claimed. No hardware flashing/testing occurred. Physical LVGL/FT6336 responsiveness, visual contrast/latency/FPS, cancel races, ordinary audio behavior, and 100-session heap/timer stability remain hardware gates.

- The production controller already bounds Tick to at most 98 phase iterations for its 48-stroke maximum; a single saturated elapsed-time settlement remains bounded even for extremely delayed samples.
- Pause must settle pending active elapsed before changing controller state inside the same coordinator admission; preserving timer elapsed alone does not prevent loss when presentation sync suspends playback.
- The shared StrokeOrderAnimationClock now used by timer and UI actions preserves fractional active milliseconds while Paused and does not rebase an already running clock during presentation sync.
- The final 12-file patch passes both 132-test host suites and clean ESP-IDF v6.0.2 CoreS3 feature-on/off builds; its required catalog remains byte-identical to SHA256 91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4.

<!-- pi-squad:be9f837bd081db5a914fad94c92ceb215348b5c25e450fc557e2c6d2bb01a027 -->
## 2026-09-13T05:57:54.876Z — stroke-sequential-animation-diagnosis

# StrokeOrder sequential-animation diagnosis

**Finding:** delayed timer settlement and Pause settlement can finish multiple strokes before one canvas redraw. This contradicts the new sequentiality priority.

**Validation:** read-only source inspection. No files changed and no commands, tests, builds or hardware validation executed. The command below is for coordinator execution; its failure is predicted, not observed.

## Minimal scenario

Select two-stroke 人 at monotonic 1,000,000 us. Present its first stroke at 250 ms. Next successful update at 3,160,000 us supplies 1910 ms: remaining first reveal 750 + gap 160 + entire second reveal 1000. The controller becomes Completed; both completed contours appear on the next redraw. Stroke 2 never received an active-reveal presentation. Pause after a timer admission miss does the same.

## Evidence

All paths are current shared files, not proof of which binary is flashed.

- `main/stroke_order/stroke_order_ui_action.h:36–46`: clock Settle passes the complete admitted monotonic delta plus fractional carry to Tick, capped only at UINT32_MAX ms.
- Same file `:61–74`: StrokeOrderApplyAnimationTick settles inside exact coordinator/session admission; rejection leaves the clock unchanged.
- Same file `:115–129`: Pause settles first; resulting Completed is accepted instead of Paused.
- `main/stroke_order/stroke_round_coordinator.h:143–158`: actual try-lock and generation/phase/cancel-fence validation surround mutation.
- `main/stroke_order/stroke_order_controller.cc:380–438`: Tick loops over remaining elapsed, consuming reveal/gap phases and incrementing strokes repeatedly. Its 98-phase bound covers an entire maximum-size glyph, not one visible update.
- `main/stroke_order/stroke_order_controller.h:60–63`: 1000 ms reveal, 160 ms gap, nominal 33 ms timer.
- `main/stroke_order/stroke_order_view.cc:1439–1463`: timer samples esp_timer_get_time, settles, then redraws once. `:1236–1248` gives Pause equivalent action-then-redraw ordering. `:608–629` configures the 33 ms LVGL timer; this does not prove actual FPS.
- `scripts/tests/stroke_order_controller_harness.cc:230–249` explicitly expects Tick(2320) on 口 to cross two reveals/two gaps. `scripts/tests/stroke_order_ui_fence_harness.cc:150–176` expects Pause to cross a gap or complete the glyph after contention. Those tests encode the superseded policy.

## Deterministic red-capable command

Run from the repository root. Only temporary files are created. Reuses existing Fixture and real coordinator-contention helper; compiles actual production sources. Cases: delayed timer, real miss then timer, real miss then Pause. No sleeps or fake Tick. Warm-up uses small samples so a future frame-budget fix need not break setup.

```sh
python3 -B - <<'PY'
import subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path.cwd() / 'scripts/tests'))
from test_stroke_order_ui import ROOT, _host_compiler, _compile_with_fallback, package_smoke_corpus
h = (ROOT / 'scripts/tests/stroke_order_ui_fence_harness.cc').read_text()
assert 'void TestPauseClock(' in h
prefix = h.split('void TestPauseClock(', 1)[0]
probe = r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(file)), {});
    assert(!blob.empty());
    constexpr uint64_t start = 1000000;
    bool sequential = true;
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f(blob); // Existing two-stroke 人 fixture.
        assert(f.ApplyUs(Action::Candidate, start));
        assert(f.controller.stroke_count() == 2);
        for (uint64_t us = 10000; us <= 250000; us += 10000)
            assert(f.TickUs(start + us));
        assert(f.controller.current_progress_permille() == 250);
        const auto before = f.controller.completed_stroke_count();
        assert(before == 0);
        if (mode != 0) WithCoordinatorHeld(f, [&]() {
            assert(!f.TickUs(start + 1000000));
            assert(f.clock.last_tick_us() == start + 250000);
        });
        assert(mode == 2 ? f.ApplyUs(Action::PauseContinue, start + 2160000)
                         : f.TickUs(start + 2160000));
        const auto after = f.controller.completed_stroke_count();
        sequential = sequential && after <= before + 1u;
        std::cerr << "mode=" << mode << " done=" << before << "->" << after << '\n';
    }
    assert(sequential && "one update completed multiple strokes");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    d = Path(directory)
    source, exe = d / 'repro.cc', d / 'repro'
    source.write_text(prefix + probe)
    units = ['stroke_order_controller', 'stroke_order_catalog', 'stroke_order_pinyin',
             'stroke_order_store', 'stroke_round_coordinator']
    _compile_with_fallback([
        _host_compiler(), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-fsanitize=undefined', '-fno-sanitize-recover=all', '-I', str(ROOT / 'main'),
        str(source), *[str(ROOT / 'main/stroke_order' / (u + '.cc')) for u in units],
        '-o', str(exe)])
    fixture = package_smoke_corpus(d / 'assets')['bin_path']
    run = subprocess.run([str(exe), fixture], capture_output=True, text=True, timeout=30)
    print(run.stdout, end='')
    print(run.stderr, end='', file=sys.stderr)
    assert run.returncode == 0, f'sequential invariant failed: {run.returncode}'
PY
```

**Expected current failure:** modes 0, 1 and 2 each print `done=0->2`, then `assert(sequential && "one update completed multiple strokes")` fails. All end Completed, including Pause. Abort codes/messages vary by host. Essential invariant: completed count after one update must be at most previous count + 1.

The host harness does not compile LVGL: it proves a per-admission violation; source call order connects it to one redraw. It does not prove physical panel visibility. A further 口 test should reject index 0→2 on one update.

## Reference is not playback

`main/stroke_order/stroke_order_view.cc:1150–1196` draws all reference outlines immediately, width 2, using text color mixed 30% into background (`:54–56`). It separately draws completed outlines in theme text color at width 3 and ONLY the current stroke's progressive width-4 median/start marker outside gaps. Completed color is dark on light theme, light on dark theme.

`DrawMedianReveal` (`:1075–1132`) reveals one median, not a filled silhouette. At completion it switches to the full contour. That contour can appear abruptly, but it represents one stroke unless timing crossed several. Seeing the entire light reference glyph initially is intentional, not evidence that all strokes completed.

## Four ranked falsifiable hypotheses

1. **Elapsed catch-up: confirmed source capability, strongest explanation.** Instrument admitted elapsed and before/after phase. No multi-phase advancement in a recorded device episode would falsify this explanation for that episode, not the source defect.
2. **Rendering/scheduling stalls create large elapsed samples: plausible, unmeasured.** Every redraw rebuilds reference/completed contours. `managed_components/lvgl__lvgl/src/widgets/canvas/lv_canvas.c:402–425` waits for canvas draw work; `main/display/lcd_display.cc:139–152` uses LVGL task priority 1 and a single width×20 display buffer. Measure callback lateness, misses, canvas and flush time; consistently short intervals weaken this hypothesis.
3. **Canvas states coalesce before panel refresh: possible.** Canvas finish invalidates, not acknowledges physical presentation. The view has no presentation gate. Correlate canvas epochs with completed refresh/flush epochs; one-to-one relevant presentations reject this explanation.
4. **Contrast/median-to-outline switching obscures sequence: plausible.** Capture initial and paused-mid-stroke close-ups in both themes. Reference-colored simultaneous contours indicate a perceptual contributor; multiple newly completed-color contours support hypotheses 1 or 3. Light-theme defaults are white background/green accent (`main/display/lcd_display.cc:37–42`).

## Recommended narrow semantics — proposals

The smallest shared behavioral seam is **Controller::Tick**, reached by clock settlement for BOTH timer and Pause. A timer-only fix misses Pause.

- Stop at the first phase boundary: reveal completion exposes that stroke's gap; gap completion exposes only the next stroke at progress zero. Discard overflow rather than saving debt.
- Also cap per-update credited progress to a small frame budget, e.g. 33 ms, using min(real elapsed, budget), not invented time. Ordinary playback stays near one second; stalls lengthen it. Phase-bounding alone can still fill one whole stroke in a delayed update.
- Retain mutation-free rejected admission, successful baseline rebasing, normal fractional carry, paused-time exclusion, resets, debounce and cancel fencing. Pause settles only the same bounded visual quantum. Legitimate final-stroke completion may still be accepted.
- Per-Tick safety cannot prove physical visibility. If refresh instrumentation shows coalescing, use one generation-stamped pending presentation token in the view, never an unbounded queue. Do not render inside coordinator admission or broaden Application/audio/board scope.
- Replace old catch-up assertions. Cover exact boundaries, repeated huge deltas, late Pause/Resume, fractional/zero/backward samples, fence rejection and final completion; test withheld acknowledgment if a presentation gate is added.

## Hardware evidence still needed

Verify flashed-image identity/hash. Record 人, 口 and a complex glyph in both themes while correlating admission elapsed, phase changes, canvas time and refresh/flush completion. Induce delay/contention including Pause before the next accepted timer. Demonstrate every stroke's active reveal/completion separately from reference outlines; recheck controls and stale-touch cancellation.

No FPS, latency, contrast, heap or audio-regression result is claimed. Coordinator execution is needed for this repro, focused/full host tests and clean feature-on/off CoreS3 builds after implementation. Physical sequentiality remains a separate gate.

- Clock settlement forwards the complete admitted monotonic elapsed delta to Controller::Tick; timer and Pause share that production seam under exact-generation/session/cancel-fence admission.
- Controller::Tick can consume multiple reveal/gap phases before the view redraws. Existing tests explicitly expect Tick(2320) on 口 to cross two reveals and two gaps.
- The renderer intentionally shows all light reference outlines, separately draws completed contours, and reveals only one current median. Reference visibility is not completion.
- The inspected LVGL canvas finish operation draws and invalidates; it does not acknowledge physical panel presentation.

<!-- pi-squad:f2ce5c091edbc4974d684a7eb3a2ee12410d20b1d9c9e50d69f442cd3dfecfa9 -->
## 2026-09-13T06:05:57.743Z — stroke-order-sequential-animation-fix

Implemented sequential-visibility repair within the same 12-file visual-only artifact. Only seven existing artifact paths changed: controller .cc/.h, shared UI-action/clock header, both harnesses, interaction Python tests, and interaction documentation.

Production clock and public Controller::Tick now cap each admitted automatic settlement to 33 ms. Successful settlement rebases to the admitted sample, discards whole-ms backlog, and preserves sub-ms remainder. A static duration check and two-phase visit bound prevent crossing more than one adjacent boundary. Pause uses the same bounded settlement inside its existing exact-generation/session/cancel-fence admission. Replay reset, Step/debounce, visual feedback and fence safety remain intact.

TDD observed: first added the exact production-shaped regression and ran it red under TSAN: modes 0/1/2 each printed done=0->2, then original assertion failed with return -6. Final TSAN and UBSAN executions return 0, all modes done=0->0; original assertion retained and strengthened with exact progress 283, adjacent snapshots and prior-partial-frame checks. Mutation probes fail when removing Pause settlement, fraction retention, baseline rebase or public Tick cap.

Validation: focused interaction suite 6 passed; focused UI suite 12 passed; shared and fresh clean-HEAD+patch full suites each passed 133 tests without skips. Earlier optional-input skip and wrong transcription-path invocations are retained transparently in evidence; final full runs use the verified pinned transcription checkout. Touched-range format checks, artifact diff checks, source hashes and protected logical scope audits pass.

Clean ESP-IDF v6.0.2 CoreS3 feature-on/off builds, merged images and ZIPs passed, with fullclean between variants. Compile audit: 7 stroke units on, 0 off, only CoreS3 board factory. App/assets sizes on: 2,918,096/7,568,207 bytes; off: 2,868,448/1,664,169 bytes. Both fit partitions and asset margins. Dependency audits verified 72 manifests/10,629 files without mismatches; all merged flash slices, ZIP payloads and app/bootloader image checksums passed.

Frozen patch: /tmp/stroke-sequential-evidence/artifact.patch; SHA256 81252835e03c4ea0116b0cb91405831f762cd2910edc96a098635fb7167da083. SOURCE-SHA256SUMS SHA256 faf9dedaa09452cd6a8059d637138221df4e0abfe76837f0255759d23db0c62f. Catalog remains 91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4. Protected Application/audio/board/CMake/default-assets logical diff is zero; unrelated dirty non-Squad file hashes are preserved. No project staging, commit, push, publishing or Squad-state edits.

Feature-on package: /tmp/stroke-sequential-evidence/cores3-on/v2.4.2_m5stack-core-s3.zip, SHA256 732fd3a1c4ef72114f0c3964470acf22fd3f4d8fa7303f0d5312a965753969ae. On merged SHA256 1c627e61ac6f4420d5ff61476976fbbcb93d275e1267e9ef779e461b6a50b2c7. Feature-off ZIP SHA256 56d1670c7ae20ca7aa47a678a0885c68f06c322eb1a7d6e57feb4db90cfe2af5.

Full findings written to the required output path. Report SHA256 1fcc05d7648dce09067d47515aa900a621b3ea2ef14b864f3dfe660f78de3d17. Evidence manifest SHA256 073559419468ac04506d48711d8ddd7a9f8fe23b94975717a54e0080f2b479bc; all 100 evidence-file checksums verified.

Independent Rai review remains pending. No hardware flashing/testing occurred. Host tests establish sequential controller/presentation opportunities, not physical panel visibility; LVGL canvas/flush coalescing and reference-outline perception remain explicit hardware gates, along with touch responsiveness, cancellation races, FPS, ordinary audio and 100-session stability.

- The exact delayed timer, timer-miss retry and timer-miss→Pause reproduction failed before repair with two strokes completed per update; the same integrated scenario now credits 33 ms and remains on stroke 0 at progress 283 in all three modes.
- At the current 33 ms quantum, 160 ms gap and 1000 ms reveal, a capped Tick can cross at most one adjacent phase boundary. The production static assertion prevents future constants from silently violating that relationship.
- Rebasing the admitted monotonic sample while retaining only its sub-ms remainder removes whole-ms catch-up debt without losing fractional active time across Pause/Resume.
- The final 12-path artifact passes both 133-test host suites without skips and clean ESP-IDF v6.0.2 CoreS3 feature-on/off builds; its required catalog is byte-identical to the specified SHA256.
- The existing LVGL timer redraws after an admitted settlement, but host state/snapshot tests do not measure panel flush completion or prove physical visibility of every accent frame.

<!-- pi-squad:697b1e8f18347d57f825dc4493f2e8148d78aef6f3c3613804777473270a1fa2 -->
## 2026-09-13T09:38:37.227Z — stroke-animation-speed-diagnosis

# StrokeOrder animation-speed diagnosis

## Finding and scope

Both clock and controller cap each admitted settlement to33 ms. The user's new observation, **approximately five seconds per active stroke**, is consistent with **~6–6.2 admitted callbacks/sec**, conditional on flashed-source identity.

**Recommend a separate159 ms maximum animation advance in both layers, retaining the33 ms requested timer period.** At6 Hz this yields ~1.05 s continuous-equivalent reveal, ~1.17 s first completion, ≥6 positive partial-update opportunities and ≤1 phase boundary per settlement.

Validation: source inspection and mathematical derivation only. **No commands, tests, builds, edits, flashing or hardware tracing performed.** User timing was supplied, not independently measured here. All test results below are predictions.

## Source evidence

`S/` means `main/stroke_order/`.

- `S/stroke_order_controller.h:60–64`: reveal1000 ms, gap160 ms, requested tick33 ms. `S/stroke_order_controller.cc:779–780`: duration independent of stroke path length.
- `S/stroke_order_ui_action.h:36–47`: monotonic delta plus fractional carry; rebase to admitted sample; retain sub-ms remainder; call `Tick(min(elapsed,kTickMs))`; discard excess whole-ms backlog.
- `S/stroke_order_controller.cc:390–399`: independent public Tick cap33 ms, static assertion against both phase lengths, two phase-processing iterations. **Changing only clock cap cannot fix speed.**
- `S/stroke_order_ui_action.h:61–75,116–130`: timer/Pause share settlement under admission; Pause settles before changing state. `:18–29`: exclude paused time, never rebase an already running clock during Sync.
- `S/stroke_round_coordinator.h:143–166`: real try-lock plus generation/phase/cancel-fence validation. Rejection leaves clock untouched; attempted cadence is not admitted cadence.
- `S/stroke_order_view.cc:608–629,1439–1463`: requested33 ms LVGL timer, real timestamp, one admission then one redraw. `:1236–1248`: controls admit before redraw. Timer configuration is not measured FPS.
- `S/stroke_order_view.cc:1058–1132,1150–1195`: redraw clears canvas, draws grid/reference/completed contours/current median/marker; recalculates median length. Plausible cost, not measured bottleneck.
- `main/boards/m5stack/core-s3/m5stack_core_s3.cc:341–369`:40MHz SPI and SpiLcdDisplay. `main/display/lcd_display.cc:138–173`: LVGL priority1, multicore affinity1, single width×20 RGB565 DMA buffer. Separate RGB constructor's50 ms timer at`:201–204` is **not CoreS3's path**.
- `scripts/tests/stroke_order_ui_fence_harness.cc:30–38,129–159`:33 ms assertions and delayed progress250→283. `scripts/tests/stroke_order_controller_harness.cc:305–306`:101 updates for口. These encode sequentiality, not reduced-cadence speed acceptance.

## Wall-clock derivation

Assume uniform **successful** callbacks F/sec, first callback one period after clock start, no Pause/Step. For F≤30 each credits33 ms. Reveal equivalent=`1000/(33F)` seconds; first completion=`ceil(1000/33)/F=31/F`. First next-stroke entry requires36 callbacks, so first observed gap lasts5/F.

| Admitted FPS | Interval ms | Reveal equivalent s | First completion s | First gap s |
|---|---:|---:|---:|---:|
|30|33.33|1.010|**1.033**|0.167|
|20|50|1.515|**1.550**|0.250|
|15|66.67|2.020|**2.067**|0.333|
|10|100|3.030|**3.100**|0.500|
|6|166.67|5.051|**5.167**|0.833|
|5|200|6.061|**6.200**|1.000|

N-stroke completion=`ceil((1000N+160(N−1))/33)/F`:人66 callbacks,口101, no terminal gap. Amortized reveal+gap=`1160/(33F)`:1.172/1.758/2.343/3.515/7.030 s at30/20/15/10/5 FPS. Exact33 ms callbacks give first completion1023 ms.

Five seconds implies F≈6.06 continuously or6.2 by first-completion quantization. For irregular cadence, sum individual admitted `min(real_delta,Q)` with fractional carry; mean FPS alone is insufficient. Alternating10/90 ms credits43 ms/100 ms withQ33, versus66 for50/50 ms. A miss creates no separate quantum.

## Policy calibration

Keep reveal1000 ms and gap160 ms. Uniform admitted-cadence predictions:

| Cap Q | First at10/15/20/30 FPS | Equivalent at6 Hz | First at6 Hz | First at5 Hz | Minimum partial opportunities |
|---|---:|---:|---:|---:|---:|
|100 ms|1.000 s|1.667 s|1.667 s|2.000 s|9|
|120 ms|1.000 s|1.389 s|1.500 s|1.800 s|8|
|150 ms|1.000 s|1.111 s|1.167 s|1.400 s|6|
|**159 ms**|**1.000 s**|**1.048 s**|**1.167 s**|**1.400 s**|**6**|

At6.2 Hz, first completion is1.452 s for120 and1.129 s for150/159. The ~1.4/~1.1/~1.05 estimates at6 Hz are **continuous equivalents**, not exact first-completion times.150/159 share first-stroke quantization here;159 loses less elapsed across longer glyphs.

**Narrow proposal:** add `kMaxAnimationAdvanceMs=159`, use it in clock and defensive public Tick, assert `<kGapMs && <kStrokeDurationMs`.159 is strictly below160; incompatible future phase changes must fail compilation. Keep `kTickMs=33`, backlog discard, fractional carry, Pause/Resume, exact fences and `kMaxPhases=2`. Update numeric expectations/docs while retaining independent behavioral assertions. No Application/audio/board/CMake expansion. Do not lengthen timer period or shorten reveal duration to compensate for presumed FPS.

150 remains safe with six partial opportunities and10 ms gap margin.159 is preferred for the reported slow cadence.120 preserves two more partial opportunities but leaves substantial slowdown. Arbitrary stalls intentionally lengthen playback.

Alternatives:
- **AdaptiveQ≤159:** same proof, but `min(real_delta,159)` already adapts advancement; EWMA/history adds reset/Pause/warm-up state without stronger safety.
- **Elapsed credit plus phase bounds:** phase limits alone do not guarantee partial frames—a reveal entered at zero could finish on its next delayed update. Debt can visibly catch up after recovery or advance at unchanged timestamps unless guarded. Requires bounded credit, independent progress/frame constraints and explicit debt retirement; larger change than needed.
- **If1 second at5 FPS becomes mandatory:** reveal advance≤200 ms plus explicit stop at first phase boundary, discarding unused elapsed, can yield five reveal samples and≥4 partial opportunities with a separate gap. This needs an algorithm change; merely raisingQ200 under the current loop can cross reveal→gap→next reveal.

## Exact sequentiality proof

For automatic timer/Pause define `P=2*stroke_index+(in_gap || Completed ? 1 : 0)`; exclude explicit Step/Replay/selection.

1. Budget `0≤b≤Q<160<1000` may finish its starting phase and enter the neighbor, but cannot consume the whole neighbor. Therefore **0≤ΔP≤1**, at most one completed stroke and one index increment.
2. Nonfinal reveal completion ends in its gap: at least one gap presentation opportunity. Gap exit ends in the next reveal with progress belowQ, never a completed next stroke.
3. Each reveal needs≥`ceil(1000/Q)` progress-contributing updates. Only the last completes it, so≥`ceil(1000/Q)−1` earlier updates have positive partial progress: **six at159/150**, eight at120, nine at100, thirty at33. Gap-entry residual also obeysQ. Includes first/only strokes and final completion during Pause, absent Step.
4. Two iterations suffice under the cap. **Iteration count alone is not the proof:** without the cap, the first may finish a gap and the second finish an unseen reveal.

These are controller/canvas opportunities, **not panel-flush receipts**. LVGL may coalesce redraws; physical visibility remains a hardware gate.

## Deterministic red-capable test specification

Proposed acceptance, not accepted policy: first reveal1.00–1.20 s at uniform admitted30/20/15/10/**6** FPS;5 FPS≤1.40 s.6 Hz rejectsQ120. Arbitrary stalls have safety, not one-second deadlines.

Implement the following host case in the existing production fence harness, using its Fixture, Snapshot and real `WithCoordinatorHeld` helper. No sleeps or fake Tick:

1. For each FPS start a fresh一 fixture at1,000,000 us. Select via production action admission. Supply timestamps `start+ceil(n*1,000,000/FPS)` until Completed; fail after400 updates.
2. Invoke `StrokeOrderApplyAnimationTick` directly, not the old Fixture wrapper whose33 ms assertion must be updated. Capture every snapshot, assert phase monotonicity/ΔP≤1, count positive partial progress, require≥6 before completion.
3. Record duration and assert the above wall-time tolerance. Aggregate failures to print all cadences before final assertion.
4. Repeat on人/口 with per-stroke partial counts and a gap opportunity between strokes. Add alternating80/120 ms and jitter around161–167 ms.
5. Warm with small real samples to0/250/990/1000/1150/1160 credited ms and990 ms into final stroke. Apply5-second/extreme delays via timer, real coordinator miss→timer and miss→Pause. Rejected admission preserves state/baseline/remainder; each admitted timer/Pause satisfiesΔP≤1. Repeated huge deltas must never complete an unpresented stroke.
6. Independently test public `Tick(UINT32_MAX)`, repeated/zero/backward timestamps, fractional Pause carry, replay reset, Step debounce, session-stale and action-first/fence-first cases.

Command after adding that case to the existing harness (not added or executed in this read-only task):

```sh
python3 -B -m unittest discover -s scripts/tests -p 'test_stroke_interaction_systems.py' -v
```

The existing Python suite compiles actual controller/catalog/pinyin/store/coordinator sources and runs the fence harness under UBSAN and TSAN (`scripts/tests/test_stroke_interaction_systems.py:14–47`). The spec must be added to the harness main; **the command alone against today's unchanged tests does not create the red regression.**

**Predicted current new-case failure:**30 FPS meets tolerance at1.033334 s;20/15/10/6/5 fail at1.550000/2.066667/3.100000/5.166667/6.200000 s. Current sequential checks should pass. With159 in both layers expected first times are1/1/1/1/1.166667/1.4 s. These are predictions, not test execution.

Mutation sensitivity: leaving either33 ms cap fails speed; removing controller cap fails direct oversized-Tick safety; removing both caps fails delayed adjacency/partial tests. Do not replace independent assertions with new-constant-derived expectations.

## Ranked hypotheses and hardware measurements

1. **33 ms clipping at~6 admitted Hz**, strongest and consistent with five seconds. Record raw/credited/dropped elapsed; predict161–167 ms admitted intervals and33-permille reveal increments. Full real-time credit during the slow episode falsifies this explanation for that episode.
2. **Render/flush/scheduling lowers invocation cadence.** Compare callback intervals, redraw time, LVGL scheduling gaps and flush completion across simple/complex glyphs. Short stable draw/flush times without delay correlation weaken this hypothesis; source settings alone prove no bottleneck.
3. **Coordinator contention lowers admission rate.** Count attempts/admissions/misses separately. Near-zero misses falsify contention as main cause; permanent stale/fenced rejection predicts stopping rather than smooth slowdown.
4. **Artifact/perception mismatch.** Verify firmware identity and progress/gap/completion times. User specified active stroke, so gap inclusion is less likely. Median spatial nonuniformity affects perception, not configured duration.

Use bounded histograms/fixed traces with `esp_timer_get_time`, not synchronous per-frame logs that perturb cadence. Video/flush evidence must confirm partial opportunities reach the panel. Verify animated Pause/Continue/Step/Replay/Back/Exit responsiveness. No audio changes.

**The seconds-per-stroke question is answered: approximately five seconds active drawing.** Optional follow-up: “请告知观察的是哪个字、当前固件版本；如方便提供约10秒视频，以便核对动态绘制和笔间停顿。”

- Current source caps elapsed twice at33 ms, in clock Settle and controller Tick; raising only one cap cannot restore speed.
- At admitted30/20/15/10/5 FPS, current first reveal takes approximately1.033/1.55/2.067/3.10/6.20 seconds. User-reported five seconds is consistent with~6–6.2 admitted Hz conditional on flashed-source identity.
- A cap strictly below160 ms gap and1000 ms reveal limits the current loop to at most one phase boundary per automatic settlement. Q159 guarantees at least six positive partial controller-update opportunities, not six proven panel flushes.
- CoreS3 uses SpiLcdDisplay with inspected LVGL priority1 and one width×20 RGB565 DMA buffer; separate RGB constructor's50 ms timer is not its path.
- Existing tests encode33 ms progress and101 updates for口. No commands or tests ran during this diagnosis.

<!-- pi-squad:de1e9ea44710812e1b69faaf6999a4cc60e785dddf4981650319f663cb48a553 -->
## 2026-09-13T09:48:37.602Z — stroke-animation-speed-calibration

Implemented speed calibration within the same 12-file visual-only stroke-order-interaction-improvements artifact. Eight existing artifact files changed: controller .h/.cc, view .cc, ui_action .h, both controller/UI harnesses, interaction Python tests, and documentation. LVGL scheduling remains 33ms; both animation clock and defensive public Tick now cap credit at 150ms. Backlog is discarded, fractional carry retained, two-phase bound/static assertions preserved, and no sound or additional runtime queue/state introduced.

TDD evidence: integrated production clock/controller/coordinator/session 6Hz regression ran red BEFORE production changes under TSAN and UBSAN, returning -6 at progress 231 after seven callbacks. The exact “first stroke should complete by about 1.17s” assertion is retained. Green runs produce 150/300/450/600/750/900/1000 and completion on frame 7 at 1,166,667us. Cadence matrix matches first completions 1023/1000/1000/1050/1167/1400ms for 33/50/100/150/~166.667/200ms intervals. 一/人/口 require >=6 prior positive partial snapshots per automatic stroke, adjacent-phase advancement <=1, and non-final gap snapshots. Stalls, no debt, timer miss→timer/Pause, fractional Pause/Resume, replay, Step/debounce, fences/stale sessions and public Tick(UINT32_MAX) remain covered.

Validation: final focused TSAN/UBSAN interaction suite 6/6; UI suite 12/12; shared full host suite 133/133; clean-HEAD+patch full host suite 133/133, both without skips. Mutations leaving either cap at33 fail the exact speed assertion; removing the public cap fails 28 controller assertions. Missing Pause settlement, lost fractional carry and retained backlog mutations also fail. Touched-range clang-format and isolated diff/reverse-apply checks pass.

Clean artifact-only ESP-IDF v6.0.2 CoreS3 feature-on/off builds, merged images and ZIPs passed, with fullclean between variants. Compile audit: 7 stroke units on, 0 off, only CoreS3 board factory. App/assets sizes on: 2,918,096/7,568,207 bytes; off: 2,868,448/1,664,169. Both fit partition and asset safety margins. Dependency verification: 72 manifests, 10,629 files, zero mismatches. All merged slices, ZIP payloads and image checksums/hashes verified.

Frozen patch: /tmp/stroke-speed-evidence/artifact.patch
SHA256: c2600b31798f817f364688575901657a4b30df9bd795b2e2b4343ba8de1be1aa
SOURCE-SHA256SUMS SHA256: 72a8f1a333fb250daf509c2552d7ee5c7ece1ac45a6e5c8429cbd5243d3296ee
Exactly 12 paths based on HEAD 7b455c74149d401e88fa729306391c10d9e73f0b. Required catalog remains 91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4. Shared/isolated source hashes match; protected Application/audio/board/CMake/default-assets logical diff is zero; all 29 captured unrelated dirty non-Squad files are unchanged. No Squad state edits, staging, commits, pushes, merges or publishing.

Feature-on candidate: /tmp/stroke-speed-evidence/cores3-on/v2.4.2_m5stack-core-s3.zip
ZIP SHA256: d735c48e39159baf5c1af1f88edb0bcb7dfafe26dc0be36db0dde6d2bd598f74
Merged SHA256: ac14ea56c9536988814515b94c1c6bbc0f5fb46de9810218c4bea4eb2b42ea07
Feature-off ZIP SHA256: 5f157a8fcd69c5b28964a5779a6768f53b43969ccd7be6fd61ec84d9c1fc1fd4.

Full findings written to /Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/fd5a342d-c5c4-4f92-95d7-0e2297a96262/stroke-animation-speed-calibration.md
Report SHA256: bf5c6b1fd295404230f0b0264bb66fe8c17d2fe9a8cadeecb9fe86990dffe818.

Independent Rai review remains pending. No hardware flashing/testing occurred. Host checks prove presentation opportunities, not LCD flush completion. Physical reveal duration/visibility, reference-outline perception, FT6336 responsiveness, pressed contrast/latency/FPS, cancellation races, ordinary audio behavior and 100-session stability remain hardware gates.

- At deterministic 6Hz delivery, the prior 33ms cap advances only 231ms after seven callbacks and needs 31 callbacks (about 5.17s) for the first reveal. The new 150ms cap completes that reveal on callback seven at 1,166,667us. This matches a plausible explanation of the reported slowdown, not an instrumented device callback measurement.
- The scheduling request and animation-credit ceiling must be independent: leaving either the clock or public controller at33ms makes the exact production-shaped speed regression fail, even if the other layer uses150ms.
- With a150ms cap below the160ms gap and1000ms reveal, the production harness maintains adjacent-phase advancement and >=6 prior positive partial snapshots per automatic stroke across 一/人/口 at normal, calibrated and huge-delta cadences.
- The final 12-path calibration passes both133-test host suites without skips and clean ESP-IDF v6.0.2 CoreS3 feature-on/off builds; the required2000-character catalog remains byte-identical.

<!-- pi-squad:7cc5e813cfc89096433546b8af846946740c6a081d86c3d66df650b1c77b36d5 -->
## 2026-09-13T10:57:07.887Z — stroke-complexity-speed-diagnosis

# Adaptive speed diagnosis — confirmed option B

The user selected **approximately 6 seconds for whole-character 顺 playback**, with count-only adaptive duration. No further product choice is pending.

**Validation:** read/search/list inspection only. No commands, tests, builds, binary decoding, hashing, edits or hardware operations ran. Predicted test results below are not observed results. Inspection does not attest the frozen 12-file hash or identify the flashed binary.

## Evidence

Source paths here are under `main/stroke_order/`:

- `stroke_order_controller.h:60–69`: reveal1000 credited ms, gap160 ms, timer request33 ms, credit cap150 ms, Step debounce120 ms.
- `stroke_order_controller.cc:779–781`: `StrokeDurationLocked()` returns1000 for every valid stroke, independent of count/path length.
- `stroke_order_controller.cc:380–450`: Tick caps credit, processes at most two adjacent phases, and omits the final gap.
- `stroke_order_ui_action.h:33–48,61–74,115–129`: timer/Pause share capped monotonic settlement inside exact generation/session/cancel-fence admission. Rejection preserves the clock; admitted excess whole-ms time is discarded, never queued; paused wall time is excluded.
- `stroke_order_view.cc:608–629,1439–1463`: timer requests33 ms; admitted settlement precedes one redraw.

`docs/stroke-order-interaction-improvements.md` identifies **6Hz as a deterministic hypothesis, not a measured device trace**. It explains the earlier approximately5-second stroke with a33ms cap. The production-clock calibration is in `scripts/tests/stroke_order_ui_fence_harness.cc:123–176`.

**Prototype 顺 exists:** `scripts/tests/fixtures/stroke_order/prototype_2000/stroke_order.cov.json:17150–17158` records U+987A, **9 strokes**, shard6,3608-byte record, source `data/顺.json`, SHA-256 `441d86ce4872794a65152354c86a6d5e1ef422a91711de7f2710cffa4ba6fe68`. Packaged `so06.sob1` exists; runtime name `so06.bin`. Standalone JSON was not found. This is manifest evidence, not fresh binary verification.

Coverage records 一=1, 人=2, 口=3. Search found no stroke counts11–48; final records have10. High-count examples below are synthetic/future cases. Store limits are48 strokes,256 outline points and64 median points per stroke (`stroke_order_store.h:49–66`). Its record layout implies 顺 has `(3608−8−9×4)/4=891` combined outline/median points, assuming the manifest size; per-stroke geometry was not decoded.

## Timing and selected formula

For N strokes of duration D: `C=N×D+(N−1)×160` credited ms. Regular successful callbacks every P ms give `T=ceil(C/min(P,150))×P`; exact6Hz gives **`T6=ceil(C/150)/6` seconds**. These exclude pauses, ASR/loading/startup and panel latency. Adjacent-phase carry means reveal/gap rounding must not be summed independently.

Current 顺 needs **10280 credited ms**, **11.5s at6Hz** (69 callbacks), **10.296s at33ms**, **13.8s at5Hz**, or **17.25s at4Hz**. The150ms-credit repair did not introduce a glyph-level budget.

Selected B constants: **minimum480, maximum600, glyph-credit target5600, gap160, cap150, scheduling33 ms; minimum3 earlier positive partial opportunities**.

`gap_budget=160×(N−1)`
`available=max(0,5600−gap_budget)` with guarded unsigned subtraction
`D(N)=clamp(floor(available/N),480,600)`

Return0 for empty glyph/invalid index before division. Integer-only O(1), stable through Pause/Resume/Replay; no estimator, queue, debt or allocation. N1–7=600ms; N8=560ms; N≥9=480ms.

| Glyph/N | Current6Hz total | B duration | B credited total | B6Hz total |
|---|---:|---:|---:|---:|
| 一/1 | 1.167s | 600ms | 0.600s | **0.667s /4 callbacks** |
| 人/2 | 2.500s | 600ms | 1.360s | **1.667s /10** |
| 口/3 | 3.833s | 600ms | 2.120s | **2.500s /15** |
| 8 | 10.167s | 560ms | 5.600s | 6.333s /38 |
| 顺/9 | **11.500s** | 480ms | 5.600s | **6.333s /38** |
| 10 | 12.833s | 480ms | 6.240s | 7.000s /42 |
| 12, hypothetical | 15.333s | 480ms | 7.520s | 8.500s /51 |
| 24, hypothetical | 30.833s | 480ms | 15.200s | 17.000s /102 |
| 48, maximum | 61.833s | 480ms | 30.560s | 34.000s /204 |

B 顺 at33ms becomes **5.610s**; at5Hz, **7.6s**. Simple characters become faster, not padded to a glyph target.

Bound: `C(N)≤max(5600,480N+160(N−1))`. Visibility overrides the target: ≤6240 credited ms for the manifest's ≤10-stroke corpus, ≤30560 for the48-stroke format bound. **No hard wall deadline exists under arbitrary stalls/Pause.** Never enforce a target by skipping strokes, suppressing partials/gaps or completing on timeout.

### Comparing4/6/8-second 顺 targets

At6Hz, credited throughput is900ms/wall-second; eight gaps cost1280ms. Unclamped `D9=floor((900×target_seconds−1280)/9)` yields:

| Target | D9 | Credited total | Minimum prior positive partials |
|---|---:|---:|---:|
| 4s | 257ms | 3593ms | 1: reject |
| 6s | 457ms | 5393ms | 3 |
| 8s | 657ms | 7193ms | 4 |

Minimum partial opportunities are `ceil(D/150)−1`, excluding zero and completion. Three require **D>450**, not450. Nine strokes need at least36 opportunities, already6s at6Hz:4s is incompatible.

Exact6s could use floor451/ceiling600/budget5400, producingD9=457. Selected B retains margin with480 and approximately6.33s. An8s profile could use budget7200/ceiling700; simple 一/人/口 would take approximately0.833/1.833/2.833s at6Hz. Uniform600 gives 顺7.5s but is not count-adaptive. These are comparisons, not pending choices.

480>3×150 guarantees three prior partial opportunities;150<160 preserves a non-final gap snapshot. This **explicitly changes the old six-partial guarantee to three**, not the strict adjacent-phase order. Physical visibility remains a hardware gate.

## Narrow implementation seam

Change `StrokeDurationLocked()` (`stroke_order_controller.cc:779`) and constants in its header. Tick, progress (`:532–549`) and Step (`:300–342`) already use that seam. At Tick's guard (`:394`) require `Q<G` and `minimum_duration>3×Q`; retain cap/two-phase bound. No clock/view/coordinator/session/Application/audio/assets/board/build behavior change is required.

**Test units trap:** `scripts/tests/stroke_order_ui_fence_harness.cc:30–40` compares permille with a millisecond cap, valid only atD1000. Normalize to `ceil(1000×150/D)` plus independent phase/index/completion checks: D480 permits313permille, not150. Update duration/progress/six-partial goldens in controller/fence harnesses and `test_stroke_interaction_systems.py:48–53`; do not delete speed/visibility assertions.

Keep path weighting out initially. The view uses approximate median arc length for spatial reveal (`:1074–1132`), not timing. Point count is sampling density. Future weighting needs measured geometry and budget-preserving redistribution above the same minimum, not additive multipliers.

## Falsifiable hypotheses

1. Fixed1000ms per stroke sufficiently explains slowness if 顺 takes approximately11.5s at steady6Hz. Longer playback with equal cadence/no misses/no pauses disproves sufficiency.
2. Complex geometry may lower cadence: `RedrawCanvas` (`view.cc:1150–1196`) redraws all reference outlines and all completed outlines each update. Compare 顺/一 callback intervals/redraw times early/late. Equal costs/cadence weaken this hypothesis; none were measured here.
3. Panel coalescing/contrast may hide partial opportunities. Compare panel video with controller progress; full light reference contours are intentional. Host snapshots cannot establish LCD visibility.
4. Verify flashed identity and existing `glyph U+987A ... strokes=9` log in `LoadSelectedLocked`; separate ASR/loading from playback. Different count/cap/binary invalidates predictions.

## Deterministic red-capable command — NOT RUN

Coordinator execution from repository root. Only temporary files are created. Reuses the existing real-coordinator Fixture, replacing its1000ms-specific adjacency assertion in the temporary harness with unit-independent phase/completion checks. Actual prototype 顺 and production clock/controller/session/coordinator are used.

Predicted current failure: after38 rational6Hz samples, only5 of9 strokes are completed; B must complete all9 at sample38 (6,333,334us), not sample37. Failure is predicted, not observed.

```sh
python3 -B - <<'PY'
import subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path.cwd()/'scripts/tests'))
from test_stroke_order_ui import ROOT, _host_compiler, _compile_with_fallback
h=(ROOT/'scripts/tests/stroke_order_ui_fence_harness.cc').read_text()
h=h[:h.index('void TestCadenceCalibration(')]
a=h.index('void AssertAdjacent('); b=h.index('struct Fixture',a)
h=h[:a]+r'''
void AssertAdjacent(const Snapshot& a, const Snapshot& b) {
    assert(b.completed>=a.completed && b.completed<=a.completed+1u);
    assert(b.phase()>=a.phase() && b.phase()<=a.phase()+1u);
}
'''+h[b:]
probe=r'''
int main(int argc, char** argv) {
    assert(argc==2);
    std::ifstream in(argv[1],std::ios::binary);
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)),{});
    Fixture f(blob,0x987A);
    assert(f.ApplyUs(Action::Candidate,1000000));
    assert(f.controller.stroke_count()==9);
    std::vector<unsigned> partial(9,0);
    std::vector<bool> gap(9,false);
    for(unsigned frame=1; frame<=38; ++frame) {
        const auto before=Snapshot::Capture(f.controller);
        assert(f.TickUs(1000000+(uint64_t{frame}*1000000+5)/6));
        const auto after=Snapshot::Capture(f.controller);
        if(after.completed>before.completed) assert(partial[before.completed]>=3);
        if(after.stroke>before.stroke) assert(gap[before.stroke]);
        if(after.gap) gap[after.stroke]=true;
        else if(!after.finished && after.progress>0 && after.progress<1000)
            ++partial[after.stroke];
        if(frame<38) assert(!after.finished);
    }
    std::cerr<<"completed="<<f.controller.completed_stroke_count()<<" expected=9\n";
    assert(f.controller.state()==StrokeOrderUiState::Completed &&
           "shun should complete at about 6.33s");
}
'''
with tempfile.TemporaryDirectory() as directory:
    d=Path(directory); src=d/'probe.cc'; exe=d/'probe'
    src.write_text(h+probe)
    units=['stroke_order_controller','stroke_order_catalog','stroke_order_pinyin',
           'stroke_order_store','stroke_round_coordinator']
    _compile_with_fallback([_host_compiler(),'-std=c++17','-Wall','-Wextra','-Werror',
        '-pthread','-fsanitize=undefined','-fno-sanitize-recover=all','-I',str(ROOT/'main'),
        str(src),*[str(ROOT/'main/stroke_order'/(u+'.cc')) for u in units],'-o',str(exe)])
    fixture=ROOT/'scripts/tests/fixtures/stroke_order/prototype_2000/so06.sob1'
    subprocess.run([str(exe),str(fixture)],check=True,timeout=30)
PY
```

Full acceptance: N1/3/7/8/9/10/12/24/35/36/48, empty/invalid index, synthetic high-count fixtures; cadence33/50/100/150ms, rational6Hz,200ms and huge stalls. Assert exact totals from the table,≥3 earlier partials,≤1 adjacent boundary/update and gap snapshots. Include near-phase boundaries, equal/backwards/UINT64_MAX samples, no debt, real coordinator miss→timer and miss→Pause, fractional carry, paused-time exclusion, final-stroke Pause completion, Replay reset, Step/debounce and fence/session rejection. Preserve controls/visual-only wiring. After revised goldens: focused UI/interaction suites, full host suite and clean CoreS3 builds.

Hardware questions: Is B's approximately6.3s 顺 readable with every accent stroke visible? What are actual admission intervals, misses, redraw times and panel updates for 一/人/口/顺, early versus late? Check both themes, all animation-time controls, pressed feedback, cancellation races and100-session stability. No sound or unrelated scope expansion.

- Current source uses1000ms credited reveals,160ms non-final gaps,33ms timer requests and150ms maximum credit. StrokeDurationLocked is the duration seam used by playback, progress and Step.
- Prototype coverage records 顺 U+987A as nine strokes in shard6 with a3608-byte record and source SHA-256 441d86ce4872794a65152354c86a6d5e1ef422a91711de7f2710cffa4ba6fe68. Packaged so06.sob1 exists; standalone source JSON was not found.
- The documentation's6Hz calibration is a deterministic hypothesis, not a measured device callback trace.
- The fence harness compares progress permille against the millisecond credit cap. That numerical equivalence depends on1000ms duration and must be normalized when duration changes.
- RedrawCanvas redraws complete reference outlines and all completed outlines on every admitted animation update, then reveals only the current median. No render timing was measured here.

<!-- pi-squad:b364c8e4f21989cae1ab5c41f930d7ee3b1f978517c2436cc4ebc2aec49cce4b -->
## 2026-09-13T11:06:01.710Z — stroke-adaptive-duration-fix

Implemented adaptive-duration option B in the existing 12-file visual-only artifact, with no sound or scope expansion.

Production policy: for validated N=1–48, gap_budget=uint64_t(160)*(N−1), available=max(0,5600−gap_budget), D=clamp(floor(available/N),480,600). The pure constexpr StrokeDurationForCount helper feeds StrokeDurationLocked; invalid counts return0 before arithmetic. Every loaded stroke shares the count-derived duration. Progress multiplication uses uint64_t. Kept timer33ms, maximum credit150ms, gap160ms and no backlog. Static assertions enforce150<160 and3*150<480; automatic playback retains >=3 earlier positive partial snapshots and a represented non-final gap, with at most one adjacent phase/index/completed advance per update.

Changed six existing artifact paths: controller.h/.cc, controller harness, UI fence harness, test_stroke_interaction_systems.py and interaction documentation. The complete frozen artifact remains exactly12 paths; protected Application/audio/board/CMake/default-assets logical diff is zero. Unrelated dirty non-Squad file hashes are unchanged. No project staging, commits, pushes, publishing, merging, Squad-state edits or hardware flashing.

TDD evidence: integrated the real checked-in so06.sob1 U+987A 顺 regression before production changes. Both TSAN and UBSAN red runs printed `shun 6Hz frame38 completed=5 expected=9` and aborted -6 at `shun should complete at about 6.33s`. The same regression now passes: frame37 not Completed, frame38 Completed at6,333,334us, retaining partial>=3 and gap-before-index assertions. Python independently verifies catalog mapping, shard CRC/size, local index and9 strokes. At33ms cadence 顺 completes at5.610s. Exact6Hz whole-glyph timings: 一0.666667s, 人1.666667s, 口2.500000s, 顺6.333334s. Full33/50/100/150ms/exact6Hz/5Hz/5s-stall/huge-delta matrix covers real glyphs and valid synthetic counts through48 with an independent exact phase/progress oracle.

Validation completed: focused interaction suite7 passed including TSAN/UBSAN; focused UI suite12 passed; shared and clean-HEAD+patch full host suites each134 passed with zero skips using STROKE_TRANSCRIPTION_JSON=/tmp/stroke-transcription-pinned.new/table-of-general-standard-chinese-characters.json. Initial one-skip logs were retained and superseded by those no-skip runs. Nine temporary mutations all failed as intended: fixed1000, fixed600, wrong8-stroke duration, clock33, public33, unbounded public Tick, retained backlog, missing Pause settlement and lost fraction. Touched-range clang-format checks, artifact diff checks and frozen-patch reverse-apply checks passed.

Clean artifact-only ESP-IDF6.0.2 CoreS3 feature-on/off builds, merged images and ZIPs passed, with fullclean between variants. Compile audit:7 stroke units on,0 off, only CoreS3 board-specific factory/codec sources. On app/assets:2,918,176/7,568,207bytes; off:2,868,448/1,664,169bytes. Both fit partitions and asset safety margins. Dependency audit:72 manifests,10,629 files, zero mismatches. ZIP payloads, all merged flash segments and image checksums/validation hashes verified. On erased OTA-data audit copy was canonically regenerated after missing its pre-fullclean copy and verified against the original merged segment and off-build generated file; documented explicitly.

Frozen evidence: /tmp/stroke-adaptive-evidence/artifact.patch SHA256 3f9a45e62be854859e429cea9eafea10aa5740aa7c273204745a9c5e70be72cd. SOURCE-SHA256SUMS SHA256 1a929fa5b58ccf7b4c96a3e6299f457f070f1b5ce02a7736862e0b3d76bdb133. Required catalog SHA256 remains91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4. Final evidence inventory SHA2569075233eec2a5e27c7a5be920f489f03620544a2d1d21d83f5471ebfb6331794;119 evidence files verified.

Feature-on candidate: /tmp/stroke-adaptive-evidence/cores3-on/v2.4.2_m5stack-core-s3.zip SHA25616f56b09d9c546054750bca0b1f167911cfe6a0fa470cf0fd397a7a5c2e462e9. Its merged-binary.bin SHA25640d5e0b050bdbfcf7f24bd85feb15fa1752a5ce8716580f7b92ba0d450d45c7a. Feature-off ZIP SHA2563080107ac083d85aecf23f14adba924a50750465b943ab84fa1f808d977d7f93.

Full findings written to /Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/f9a2ae5c-708d-43c3-83f3-1619a74b6e0a/stroke-adaptive-duration-fix.md; report SHA2565907bf8d9c6c8297c29ce9cb4af9341dc994c665f519d7d765745731aa110fe6.

Independent Rai review remains pending; no approval claimed. Physical LCD visibility/coalescing, actual cadence/FPS, both-theme contrast, FT6336 responsiveness, animation-time controls/cancel races and100-session heap/timer stability remain hardware gates. Exact6Hz is a host workload, not a device trace; at5Hz 顺 intentionally takes7.6s rather than skipping. Supervisor-authorized removal of only two obsolete generated build-cache directories resolved disk pressure; prior sources/evidence/packages were preserved and cleanup logged.

- The actual prototype_2000 catalog maps U+987A 顺 to runtime asset so06.bin; checked-in so06.sob1 matches its CRC/size and contains a validated nine-stroke record.
- The implemented bounded count formula yields600ms for1–7 strokes,560ms for8,480ms for9–48, and0 for invalid counts. Count36 exactly exhausts the5600ms gap budget; counts above36 safely saturate available time at0 before clamping.
- With150ms maximum credit,160ms gaps and minimum480ms reveal duration, production settlements cross at most one phase boundary and require at least three earlier positive partial presentation opportunities per automatic stroke.
- The production clock/controller/coordinator harness completes real 顺 at exact6Hz frame38 (6,333,334us), not frame37; at fixed33ms it completes at5,610,000us. At5Hz it takes7.6s with sequentiality preserved.
- Both final134-test host suites pass without skips, and clean ESP-IDF6.0.2 CoreS3 on/off firmware packages pass source, dependency, partition, merged-segment and image-hash audits. These tests do not establish physical LCD flush visibility or touch latency.

<!-- pi-squad:d148c027c3c3af0083e9f10404b5172f9b9c899bcb71e22136673264830c9452 -->
## 2026-09-13T11:32:07.048Z — stroke-adaptive-duration-fast-fix

Implemented the faster adaptive policy within the same exact 12-path visual-only artifact. Only six existing artifact files changed in this repair: controller .h/.cc, controller harness, UI fence harness, interaction tests, and interaction documentation. Production changes are limited to duration constants 301/500/4500 and the static two-earlier-partial safety proof; clock, phase, fence, controls, and audio scope remain unchanged.

Observed TDD red BEFORE production changes: both TSAN and UBSAN real so06.sob1 U+987A 顺 regressions failed at frame30 with completed=7 expected=9. Green results: real 顺 frame29 incomplete (8 completed), frame30 Completed at exactly 5,000,000 us; earlier positive partial counts 2,2,3,2,3,2,3,2,2, with all eight non-final gap snapshots. 顺 completes at 4.521s with 33ms callbacks and 6.0s at 5Hz. Exact6Hz 一/人/口 timings are 0.666667/1.333334/2.166667s.

Validation: focused interaction/TSAN/UBSAN 7 passed; UI 12 passed; shared and clean-HEAD+patch full suites each 134 passed without skips. Matrix covers every formula count1–48 plus invalid counts, real glyphs and synthetic N1/2/3/5/7/8/9/10/11/15/24/36/48 across six requested cadences and two stall cadences: 136 runs and 1552 automatic completions per sanitizer. Adjacency, order, gap-before-index, >=2 earlier positive partials, normalized progress, no backlog, Pause/fraction/miss/retry, Step/debounce, Replay, fence/session, timeout and visual/no-audio checks remain. Ten temporary mutations failed as intended. Touched-range clang-format and artifact diff checks passed.

Clean artifact-only ESP-IDF v6.0.2 CoreS3 feature-on/off builds, merge images and ZIPs completed. Audits verify 7 stroke units on/0 off, only CoreS3 board factory, partition margins, all merged slices, ZIP payloads, image checksums, and 72 dependency manifests/10,629 files with zero mismatches. On app/assets: 2,918,176/7,568,207 bytes; off: 2,868,448/1,664,169 bytes. Fully disclosed tooling exception: editing the running evidence driver's copy list caused a post-build EOF after both variants completed, before its final marker. Actual outputs were independently audited successfully before marker recovery; original logs and recovery details are retained. A concurrent checksum audit also timed out once at20s and passed on retry. No firmware/source changes concealed either issue.

Frozen evidence: /tmp/stroke-fast-evidence/artifact.patch SHA256 d959537e611e207ca772e40769bf0103148480dcfe91cdb5a67630b2bfb79741. SOURCE-SHA256SUMS SHA256 a77e785fc0152685c7031807a6fdb65e115747ec8ef73fccb21e281b8847f20c. Required catalog remains 91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4. Exact12 isolated paths, shared/isolated hashes match; protected logical diff zero; unrelated dirty hashes preserved. No Squad state edits, staging, commits, pushes or publishing.

Feature-on candidate: /tmp/stroke-fast-evidence/cores3-on/v2.4.2_m5stack-core-s3.zip SHA256 7aa83484a42662d0abb036a7348ff28b50b5085dbb79c2c56b4665074e35e8ce; merged image SHA256 0300d70ea10f1de5d250c3bb8ff8e8be90e4506046ecd7224d8204a25d720573. Feature-off ZIP SHA256 fd8a6a870c4423dd97f21c15dc0403ea9d827ce67dfd6171a3e190852493344a.

Full report written to /Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/d3f0514a-9c5f-47df-9912-7846fa30aa8c/stroke-adaptive-duration-fast-fix.md; SHA256 063d6fccfb7f43084b9bf828297d2ed85b76fcca47cb346dde01d8b17c09c3ed. Evidence manifest SHA256 88f7511527da966aeede638dc41f47f82733676bc20eb03753d839a73be689df.

Independent Rai review remains pending. No flashing/hardware testing occurred. Physical accent visibility despite LVGL redraw coalescing, touch latency/contrast, cancel races, ordinary audio, and 100-session heap/timer stability remain hardware gates.

- The implemented uint64 count formula clamp(floor(max(0,4500−160×(N−1))/N),301,500) yields exactly N1–7=500ms, N8=422ms, N9=357ms, N10=306ms, N11–48=301ms; invalid counts return0 before arithmetic.
- With maximum credit150ms, gap160ms and minimum reveal301ms, one settlement cannot cross two phase boundaries, and every automatic reveal requires at least two earlier positive partial snapshots. Existing phase/clock algorithms require no further state changes for this policy.
- The actual checked-in so06.sob1 顺 record has9 strokes; production clock/controller/coordinator/session tests now complete it at exact6Hz frame30=5.0s, with frame29 incomplete, all eight gap snapshots, and per-stroke earlier partial counts 2,2,3,2,3,2,3,2,2.
- The frozen 12-path artifact passes both 134-test full host suites and clean ESP-IDF6.0.2 CoreS3 on/off builds; protected Application/audio/board/CMake/default-assets logical diff is zero and catalog bytes remain unchanged.

<!-- pi-squad:363619f59017900edbdef0e5e51cab96fac2411206e7e7243daac57f45dd3ef8 -->
## 2026-09-13T12:15:52.870Z — stroke-start-cue-snap-animation

Implemented start-cue/snap playback within the same exact 12-path visual-only artifact; this repair changed nine existing artifact files. Every stroke now holds an accent start marker for 150ms, snaps its completed contour visible, then holds a 160ms gap. Progressive median drawing and adaptive durations were removed. Each Tick settles only its starting phase, caps credit at160ms, discards boundary surplus and whole-ms backlog, and enters each next cue at progress0. Timer and Pause share this settlement; fractional carry, paused-wall-time exclusion, controls, pressed visuals and cancel fencing remain intact.

TDD evidence: the integrated real so06.sob1 顺 regression failed before production changes under both TSAN and UBSAN, returning -6 with stroke0/progress420/done0 after +160ms. The exact snap and next-cue-zero assertions remain and now pass. Marker-only render regression also ran red/green. Final deterministic 顺 timings: exact6Hz17callbacks/2.833334s;5Hz17/3.400s;33ms85/2.805s. Tests cover real 一/人/口/顺, all synthetic counts1–48, nine cadences, huge deltas, no debt, cue/gap adjacency, Pause/misses/fractions, controls and fencing. Six runtime mutations plus restored median rendering fail as expected.

Validation: focused TSAN/UBSAN/render suite8 passed; focused UI12 passed; shared and clean-HEAD+patch full suites each135 passed with no skips. Touched-range formatting, patch checks, exact scope and final source/hash audits passed. Clean artifact-only ESP-IDF6.0.2 CoreS3 feature-on/off builds, merges and ZIPs passed with fullclean between variants. Compile audit:7 stroke units on,0 off, only CoreS3 board factory. Dependency audit:72 manifests/10,629files, zero mismatches. On app/assets sizes2,917,536/7,568,207bytes; off2,868,448/1,664,169bytes; both fit partitions and asset margins. ZIP payloads, flash slices and image validation hashes verified.

Frozen patch: /tmp/stroke-snap-evidence/artifact.patch
SHA256 20911785950539351709fe9153cc1d5dfe25fa20f74df095e56dda170a9ca13b
SOURCE-SHA256SUMS SHA256 9bec50c56c9eeeca9e2dac77b08cc9133fced4628e005c547ff1098fb1f068d5
Catalog unchanged:91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4.

Feature-on flash candidate: /tmp/stroke-snap-evidence/cores3-on/v2.4.2_m5stack-core-s3.zip
SHA256 7e4bf9f1b3a3a82d52500b77d6fcd4157edbbe50e5d6f33f0e43ae9b12add5be
On merged-image SHA256 42f99712f917ea89a82c61c0c9faac8a98e8b7688c5a45967e3aeda739b07293
Feature-off ZIP SHA256 ad037cc386679e3f697f8c70255bb0e53cdbc248e2e1464b3654727125867969.

Detailed findings written to /Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/d7588b07-c6f7-4fc3-aafd-66fb0b6c0989/stroke-start-cue-snap-animation.md
Report SHA256 524c3bc177b25a818e521167717d7ea814e5dc1f1916194316cbf82bbfe82f32.

Protected Application/audio/board/CMake/default-assets logical diff is zero; unrelated dirty non-Squad files remain byte-identical. No staging, commits, pushes, publishing, Squad-state edits or hardware flashing/testing. Independent Rai review remains pending. Physical cue/contour visibility through LVGL flush coalescing, theme contrast, FT6336 latency, cancellation/control races and100-session stability remain hardware gates. Explicit Step retains its intentional manual one-stroke completion behavior.

- The implemented controller can guarantee one automatic phase boundary per Tick without relying on the credit cap being shorter than a phase: settling only the starting phase and discarding surplus makes gap entry and next-cue progress0 explicit.
- With150ms cue,160ms gap and160ms credit cap, real nine-stroke 顺 completes in17 admitted updates at exact6Hz/5Hz, and85 updates at33ms. A150ms cadence is slower than160ms because each160ms gap needs two callbacks.
- Marker-only active rendering now uses the first median point; faint full reference contours and completed theme-text-color contours remain separate. Host snapshots plus synchronous canvas-redraw wiring prove presentation ordering, not physical panel flush visibility.
- The frozen12-path artifact passes135-test no-skip shared/isolated suites and clean ESP-IDF6.0.2 CoreS3 on/off builds; its2000-character catalog remains byte-identical to the required SHA256.

<!-- pi-squad:2cf6f313600f0699de6b4f1cae77a0cbe375bac6bd06f99d37abac9b9b21f051 -->
## 2026-09-13T13:16:11.736Z — stroke-first-cue-visibility-diagnosis

# First start-cue visibility diagnosis — CoreS3

## Finding and validation

**Initial cue coalescing is source-reachable and the strongest explanation for a first-stroke-only symptom, but is not yet a proven device trace.** Initial Redraw writes a mutable canvas buffer, not the panel. A delayed first automatic callback can replace cue0 with completed stroke0 before LVGL refresh reads it.

**Read-only inspection: no commands, tests, builds, edits, flashing or hardware measurements performed.** References describe the inspected working tree; equivalence to the flashed approved binary was not verified.

Important correction: the operative baseline is not simply Candidate's pre-load timestamp. `RenderAnimationPage()` resets that clock through `StopAnimTimer()`, then starts a fresh baseline through `SyncAnimTimer()` **before** `RedrawCanvas()`. Initial canvas drawing and subsequent scheduling/admission delay are still credited without a refresh receipt.

## Source evidence

Abbreviations: `S = main/stroke_order/`; `L = managed_components/lvgl__lvgl/src/`; `P = managed_components/espressif__esp_lvgl_port/src/lvgl9/`.

1. **Initial state is correct:** `S/stroke_order_controller.cc:244–260,751–763` resets index/elapsed/gap and enters Animating. `S/stroke_order_view.cc:1281–1288` admits Candidate then presents the page.
2. **Startup clock ordering:** `S/stroke_order_view.cc:598–627` resets the clock on Stop; Sync samples `esp_timer_get_time()` at 612, creates a 33ms timer at 620, sets period and resumes. `:811–873` caches glyph, calls Stop at 823, creates canvas/controls, calls Sync at 867, then Redraw at 870. Work before this Sync is excluded from the operative delta; drawing after it is not.
3. **First timer can finish cue0:** `S/stroke_order_ui_action.h:10–46` immediately starts a non-running Animating clock and settles up to 160ms; `:61–74` supplies coordinator/session admission. `S/stroke_order_controller.h:60–67` defines cue150/gap160/timer33/cap160. `S/stroke_order_controller.cc:379–427` consumes only the starting phase, but 160ms consumes the entire initial cue. Result: current0, done1, in gap; a one-stroke glyph becomes Completed.
4. **Mutation precedes redraw:** `S/stroke_order_view.cc:1387–1410` validates timer identity/state, admits settlement, then Redraw at 1406. `:1097–1143` fills the same buffer, draws reference/completed outlines, and draws the active marker only when Animating/Paused and not in gap. Completion removes marker0 on that redraw.
5. **Canvas finish is not LCD flush:** `L/widgets/canvas/lv_canvas.c:382–425` targets the canvas buffer, dispatches drawing tasks and invalidates the canvas. Background fill also invalidates (`:303–379`). `L/core/lv_refr.c:269–341` stores invalid areas, dropping an area already covered (`:324–327`), not a queue of image versions. Initial and completion invalidations can therefore yield one refresh of the latest pixels.
6. **Animation can precede refresh:** `L/display/lv_display.c:123–128,180–182` creates/readies display refresh at initialization; `:1550–1557` later requests only resume it. `L/misc/lv_timer.c:162–187` inserts new timers at the head, unpaused, with `last_run=lv_tick_get()`. Resume/period changes do not reset last_run (`:216–237`). **StrokeOrder does not call `lv_timer_ready()` here**: normally its new timer waits 33ms, but delayed work can make it due before initial refresh. Timer traversal is newest-first, restarting after callback timer creation/deletion (`:102–130`); execution timestamps before the callback (`:338–359,387–395`). If both are due, the newer animation timer can run before older display refresh.
7. **Invalidation wake is not refresh-now:** `P/esp_lvgl_port_disp.c:816–826` wakes the task; `P/esp_lvgl_port.c:205–253` processes input then runs `lv_timer_handler()` under the LVGL lock. Actual layout/rendering occurs in the refresh timer (`L/core/lv_refr.c:362–445`). Local `sdkconfig:4801` requests 33ms refresh, not guaranteed cadence or flashed configuration.
8. **Physical transfer is separate:** `main/boards/m5stack/core-s3/m5stack_core_s3.cc:334–367` creates 40MHz SPI/SpiLcdDisplay. `main/display/lcd_display.cc:147–175` selects a width×20 single partial-refresh buffer. `P/esp_lvgl_port_disp.c:744–759` submits bitmap regions; `:122–127,504–510` connects transfer completion to flush-ready. Canvas-finished, refresh-requested, rendered and physically visible differ.

### Reachable schedule

Let t0 be initial Sync: controller cue0/elapsed0 → initial Redraw writes marker0 and invalidates → no refresh before first admitted timer at t0+160ms or t0+5s → credit=min(delay,160ms)=160ms → stroke0 completes and redraw overwrites marker0 → older display-refresh timer reads full0. Initial drawing, other callbacks, task starvation or admission misses can cause the delay; none was measured.

## First stroke versus all strokes

Cue0 is initially written by the page/action path, outside an automatic turn. Later cues enter during a timer turn: `S/stroke_order_controller.cc:396–406` finishes only the gap, advances one index, sets cue elapsed0 and returns. That turn normally reaches display refresh before the next animation callback. This explains first-only behavior without claiming multiple strokes are skipped by one Tick.

One-phase-per-Tick is necessary but insufficient for physical visibility: several timer turns without refresh can coalesce later cues too. A startup fix is not a guarantee against arbitrary refresh starvation.

Existing tests assume the missing presentation: `scripts/tests/stroke_order_ui_fence_harness.cc:150–156` seeds `cue[0]=true` for Candidate's synchronous redraw, without a flush; `:464–485` expects the first +160ms callback to finish it. `docs/stroke-order-interaction-improvements.md:94–105` documents 17 callbacks for nine-stroke 顺. These test state opportunities, not panel receipts.

## Ranked falsifiable hypotheses

1. **Initial cue overwritten before first refresh — strongest, source-confirmed capability.** Trace initial canvas-finish, first admitted delta, before/after phase and refresh/transfer events. A marker-containing transfer with visible hold before completion falsifies this explanation for that episode.
2. **Cue transfers but dwell is too short.** Clock credit includes rendering/refresh latency, not marker-visible time. Measure marker-region transfer to completed-region transfer. Adequate dwell with the symptom still present points elsewhere.
3. **Reference/marker ambiguity.** `S/stroke_order_view.cc:24–26,53–55,1073–1094,1116–1141` draws all references immediately at 30% mix, width2; completed outlines width3; cue is only a radius5 dot at median[0]. Default accent is green (`main/display/lcd_display.cc:41,57`). A faint reference + green-dot still supports ambiguity; an initially high-contrast contour without dot does not.
4. **Build mismatch or unexpected Step.** Flashed identity is unverified; explicit Step legitimately finishes the current cue (`S/stroke_order_controller.cc:300–337`). Untouched playback on a verified build with action traces falsifies this alternative. A glyph-specific bad/missing marker is lower priority and unproven.

## Deterministic red-capable test specification

The complete proposed command is included in this report's `acceptanceReport.proposed_test_command`; **it was not run**. It uses actual production clock/controller/coordinator helpers, the existing fixture and real nine-stroke 顺. The modeled page boundary resets Candidate's earlier clock, matching Stop/Sync before initial Redraw. No initial snapshot is counted as a displayed frame.

Required assertion: after the first admitted automatic callback at page-start+160ms or +5s, remain Animating, current0, completed0, progress0 and not in gap. **Predicted current failure:** both cases produce `done=1 progress=1000 gap=1`, then the final assertion aborts. This state test does not compile LVGL or prove panel output.

Retained seam: use the same production begin-presentation helper from page creation and fixture, replacing the Reset/Sync imitation. Source checks should couple page startup and timer-then-redraw ordering to that helper. Add a deferred single-buffer fake: initial Draw marks dirty without Flush; first timer Draw overwrites it; only then Flush samples it. Require cue0. A real LVGL simulator extension should create older display/newer animation timers, advance synthetic ticks and capture flush pixels without sleeps.

## Fix comparison and recommendation

- **First admitted tick arms/no credit:** passes the delayed160ms/5s invariant. Rebase at that tick, discard all pre-arm elapsed/fraction, redraw cue0, then use ordinary timing. This provides a refresh opportunity, not a physical receipt.
- **Explicit first-cue pending token:** recommended implementation of that rule. A bounded startup flag/epoch separates startup from running/paused accounting. Set for new playback/page, Replay and successful RetryLoad; consume only inside successful generation/session/cancel-fence admission. Repeated Sync/redraw must not re-arm it; page Stop/reset must not erase the new requirement. Rejected admissions leave it untouched.
- **`lv_refr_now(display)`:** `L/core/lv_refr.c:87–102` directly invokes refresh, also animation refresh. It can prevent this coalescing but adds synchronous layout/render/transfer waits in UI work, does not guarantee a 150ms physical hold and still needs baseline care. Not the default; avoid drawing/refresh callbacks and never hold coordinator mutex across it.
- **Timer deferral/reset or moving Sync after Redraw:** reduces the window but a delayed first execution still credits ≥150ms. Fixed delay is not acknowledgment; resume does not reset LVGL last_run. Insufficient alone.

**Recommend the bounded startup token with first-admitted-automatic-turn/no-credit, confined to StrokeOrder clock/action/view.** Keep cue150/gap160 and phase-local controller transitions. Pre-arm Pause must not settle undisplayed startup debt; preserve startup pending across Pause/Resume. Post-arm behavior retains current Pause settlement, fractions and paused-time exclusion. Step remains phase-local and supersedes obsolete startup state; Back/Exit/deletion clear it. Replay re-arms even with an existing timer. Do not re-arm every Resume or later cue. No Application/audio/board/vendor changes.

Regression matrix: first callback at 0/33/149/150/160ms/5s; real miss/retry using existing WithCoordinatorHeld; session/generation/fence rejection with unchanged clock/token; Replay/re-entry/RetryLoad; pre/post-arm Pause; cue/gap Step; Back/Exit; one-stroke completion; same-time no debt; subsequent +160ms full0/gap then +160ms cue1 at zero. Nine-stroke startup becomes 18 instead of 17 admitted turns at ≥160ms cadence with one arming turn.

**Residual:** if refresh is starved across multiple turns, first-tick gating cannot ensure physical visibility. Such traces justify generation-scoped presentation acknowledgment and a hold beginning after the relevant rendered/transferred cue. Generic REFR_READY and canvas-finish do not prove physical visibility on asynchronous SPI.

## Hardware question

**Is the green dot missing only on the first stroke after selection, while later dots are visible—and does R on the already-open page show that same first dot?** Capture selection versus Replay for the same glyph (ideally 一/人/顺), identify the flashed package, and distinguish faint references from high-contrast completed contours. This is coordinator hardware follow-up, not a blocking clarification.

- The inspected controller uses cue150/gap160/cap160; a delayed first admitted timer can complete cue0 in one call.
- RenderAnimationPage resets the Candidate clock, then starts a fresh baseline through SyncAnimTimer before RedrawCanvas.
- LVGL canvas invalidation stores mutable-buffer areas rather than frame versions; new timers precede the older display-refresh timer.
- The UI-fence harness pre-counts cue0 after Candidate without a flush, so its 17-callback nine-stroke oracle does not establish first physical cue visibility.
- CoreS3 uses a single partial-refresh SPI display buffer with separate asynchronous transfer completion; canvas finish is not panel visibility.

<!-- pi-squad:9cf2d3c5acb908410db6228ea78b925b6556648e81542a30523b63c1b47066ef -->
## 2026-09-13T13:54:01.502Z — stroke-first-cue-startup-gate

Implemented the bounded first-cue startup gate within the existing 12-path visual-only artifact. Only six existing artifact files changed: stroke_order_ui_action.h, stroke_order_view.cc/.h, stroke_order_ui_fence_harness.cc, test_stroke_interaction_systems.py, and the interaction documentation.

Fresh Candidate/Replay/successful RetryLoad now creates one pending bool. The first successfully admitted automatic callback consumes it, rebases timing, clears fractional debt, calls no Controller::Tick, and redraws exact cue0. Rejected admission preserves all state. Pre-arm Pause/Resume preserves the token; ordinary Resume/Sync and later phases do not rearm it. Accepted Step supersedes it; teardown clears it. RenderAnimationPage preserves the token through StopAnimTimer(false) and draws before starting the timer. No synchronous refresh, sound, queue, allocation, or scope expansion was introduced.

TDD evidence: before production changes, +160ms and +5s first-callback regressions each aborted -6 under both TSAN and UBSAN with stroke0/progress1000/done1/gap1. The exact cue0 assertion remains. Green focused interaction/sanitizer/render suite: 12 passed; existing UI/controller suite: 12 passed. Shared and clean-HEAD+patch full host suites: 139 passed each, without skips. Five temporary mutations—disable token, consume before admission, clear on pre-arm Pause, rearm on Resume, rearm on Sync—all failed retained assertions. Formatting, diff checks, scope checks, source hashes and evidence checksums passed.

Real 顺 now completes in 18 automatic callbacks at exact6Hz/5Hz (3.000s/3.600s), or 86 callbacks at33ms (2.838s). Every cue/full/gap assertion remains, including real and synthetic 1–48 stroke matrices.

Clean artifact-only ESP-IDF v6.0.2 CoreS3 feature-on/off builds, merged images and ZIPs passed with fullclean between variants and the byte-identical prior dependency lock. Compile audits: 7 stroke units on, 0 off; only CoreS3 board factory. Dependency audit: 72 manifests, 10,629 files, zero mismatches. App/assets bytes: on 2,917,584/7,568,207; off 2,868,448/1,664,169. Partition margins, merged slices, ZIP payloads and image validation hashes passed.

Frozen artifact: /tmp/stroke-first-cue-evidence/artifact.patch SHA256 467c07ee8bd1e8db73c44dd14944a6f25330717eaabdb823f90ac671f3f4be4c. SOURCE-SHA256SUMS SHA256 cd92642d7a95a302b5cec6a5085891dc93ad668fd2401e70740ecb6f5a3ef174. Exactly12 paths; protected logical diff zero; required catalog remains 91779189b45526ab47cff2927b8da35e345e967a84e72096221e658e2cc919e4. Unrelated dirty non-Squad files preserved. No staging, commit, push, publication or Squad-state edit.

Feature-on candidate: /tmp/stroke-first-cue-evidence/cores3-on/v2.4.2_m5stack-core-s3.zip SHA256 375307e1e441ed0014b422487f0994e0900453ad04ffe8242a2f94145b1de0f8; merged image SHA256 504cbd759ee209e3e952d42e53af489dd73d115df660738c615f48fd565fcc3c. Feature-off ZIP SHA256 846f8438eb86e24e972f9902fdd5dddfefec2836487bc80ae4667e406f2b2bba.

Full report: /Users/mandyw/.pi/agent/sessions/--Users-mandyw-git-xiaozhi-esp32--/subagent-artifacts/outputs/009e3702-6fb7-445a-9aee-fb5ca3ec6227/stroke-first-cue-startup-gate.md. Report SHA256 ad3ad9eb69f9a130c2d6b4b9bdcce458c6d7a9818a7c16b0388d1408af24bb53.

Independent Rai review remains pending. No hardware flashing/testing occurred. The extra scheduling turn and mutable-buffer model do not prove physical LCD flush visibility; first-cue visibility under device load, theme contrast, touch/cancel behavior, ordinary audio behavior and 100-session stability remain hardware gates.

- A fresh playback token must survive RenderAnimationPage timer recreation; the former unconditional StopAnimTimer clock reset erased startup timing state before the first callback.
- The first automatic admission can be made cue-only with one display-owned pending bool; token consumption must occur after coordinator/session/state checks, separately from Pause settlement.
- Pre-arm Pause must credit zero without consuming the token. Post-arm Pause/Resume still preserves sub-millisecond active time and excludes paused wall time without rearming.
- Adding one fresh-playback arm-only turn changes the verified nine-stroke 顺 automatic count from17 to18 at saturated cadence and from85 to86 at33ms; subsequent phase-local cue/gap behavior does not change.
- The frozen 12-path artifact passes both139-test no-skip host suites and clean ESP-IDF6.0.2 CoreS3 on/off builds using the unchanged prior dependency lock; protected files and the required catalog remain unchanged.
