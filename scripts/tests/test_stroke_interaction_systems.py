"""Narrow interaction patch: production controller/coordinator + LVGL source wiring.

No audio harness is required: feedback is now entirely LVGL pressed styling.
"""
import os
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

from test_stroke_order_ui import ROOT, _compile_with_fallback, _host_compiler, package_smoke_corpus
from stroke_order.catalog import validate_catalog
from stroke_order.convert import pack_characters
from stroke_order.unpack import StrokeOrderBlob


def synthetic_count_matrix():
    # Test-only valid vectors, NOT corpus data or a substitute for the real 顺.
    stroke = {"outline": [(0, 0), (1024, 0), (1024, 1024), (0, 0)],
              "median": [(0, 1024), (1024, 0)]}
    return pack_characters([
        {"character": chr(0x4E00 + count), "codepoint": 0x4E00 + count,
         "strokes": [stroke] * count}
        for count in range(1, 49)
    ])


class StrokeInteractionTest(unittest.TestCase):
    def test_real_catalog_shun_maps_to_nine_stroke_so06(self):
        directory = ROOT / "scripts/tests/fixtures/stroke_order/prototype_2000"
        catalog = validate_catalog((directory / "stroke_cat.bin").read_bytes())
        entry, = [entry for entry in catalog["entries"] if entry.codepoint == ord("顺")]
        shard = catalog["shards"][entry.shard]
        self.assertEqual(shard.name, "so06.bin")  # packaged asset name
        data = (directory / "so06.sob1").read_bytes()
        self.assertEqual(len(data), shard.size)
        self.assertEqual(zlib.crc32(data) & 0xFFFFFFFF, shard.crc32)
        store = StrokeOrderBlob()
        self.assertTrue(store.bind(data))
        self.assertEqual(store.codepoint_at(entry.local_index), ord("顺"))
        self.assertTrue(store.load_character(ord("顺")))
        self.assertEqual(len(store.strokes), 9)

    def test_exact_ui_session_cancel_fence_action_order_and_contention_tsan(self):
        self._run_fence_harness("thread")

    def test_sequential_clock_pause_boundaries_and_overflow_ubsan(self):
        self._run_fence_harness("undefined")

    def test_cue_step_presents_same_stroke_full_gap(self):
        self._run_fence_harness("undefined", "step-cue")

    def test_gap_step_presents_next_start_cue_without_completion(self):
        self._run_fence_harness("undefined", "step-gap")

    def test_delayed_first_callback_keeps_exact_cue_zero(self):
        for sanitizer in ("thread", "undefined"):
            for mode in ("first-160ms", "first-5s"):
                with self.subTest(sanitizer=sanitizer, mode=mode):
                    self._run_fence_harness(sanitizer, mode)

    def _run_fence_harness(self, sanitizer, mode=None):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "stroke_order_ui_fence_harness"
            sources = [
                "scripts/tests/stroke_order_ui_fence_harness.cc",
                "main/stroke_order/stroke_order_controller.cc",
                "main/stroke_order/stroke_order_catalog.cc",
                "main/stroke_order/stroke_order_pinyin.cc",
                "main/stroke_order/stroke_order_store.cc",
                "main/stroke_order/stroke_round_coordinator.cc",
            ]
            _compile_with_fallback([
                _host_compiler(), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=" + sanitizer, "-fno-sanitize-recover=all", "-g", "-O1",
                "-I", str(ROOT / "main"),
                *[str(ROOT / source) for source in sources], "-o", str(executable),
            ])
            fixture = package_smoke_corpus(Path(directory) / "assets")["bin_path"]
            shard = ROOT / "scripts/tests/fixtures/stroke_order/prototype_2000/so06.sob1"
            synthetic = Path(directory) / "synthetic.sob1"
            synthetic.write_bytes(synthetic_count_matrix())
            run = subprocess.run([str(executable), fixture, str(shard), str(synthetic)] +
                                 ([mode] if mode else []),
                                 capture_output=True, text=True,
                                 timeout=30,
                                 env={**os.environ, "TSAN_OPTIONS": "halt_on_error=1"})
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            if mode:
                expected = "first_callback" if mode.startswith("first-") else mode.replace("-", "_")
                self.assertIn("stroke_order_" + expected + ": PASS", run.stdout)
                return
            self.assertIn("stroke_order_startup_gate: PASS", run.stdout)
            self.assertIn("stroke_order_first_callback: PASS", run.stdout)
            self.assertIn("stroke_order_step_phase_matrix: PASS", run.stdout)
            self.assertIn("stroke_order_ui_fence_harness: PASS", run.stdout)
            self.assertIn("stroke_order_start_cue_snap: PASS", run.stdout)
            self.assertIn("stroke_order_cue_matrix: PASS", run.stdout)
            self.assertIn("glyph=39034 strokes=9 cue_ms=150 interval_us=0 frames=18 "
                          "completion_us=3000000", run.stdout)
            self.assertIn("glyph=39034 strokes=9 cue_ms=150 interval_us=33000 frames=86 "
                          "completion_us=2838000", run.stdout)
            self.assertIn("glyph=39034 strokes=9 cue_ms=150 interval_us=200000 frames=18 "
                          "completion_us=3600000", run.stdout)
            self.assertIn("stroke_order_pause_clock: PASS", run.stdout)
            self.assertIn("stroke_order_delayed_sequential: PASS", run.stdout)
            self.assertIn("stroke_order_sequential_matrix: PASS", run.stdout)
            for mode in range(3):
                self.assertIn(f"mode={mode} done=0->1", run.stderr)

    def test_active_render_is_start_marker_only(self):
        view = (ROOT / "main/stroke_order/stroke_order_view.cc").read_text()
        header = (ROOT / "main/stroke_order/stroke_order_view.h").read_text()
        draw = view[view.index("void StrokeOrderView::RedrawCanvas"):
                    view.index("void StrokeOrderView::UpdateControlLabels")]
        self.assertNotIn("DrawMedianReveal", view + header)
        self.assertIn("DrawStartMarker", draw)
        self.assertNotIn("DrawLine", draw)
        self.assertNotIn("current_progress_permille", draw)
        marker = view[view.index("void StrokeOrderView::DrawStartMarker"):
                      view.index("void StrokeOrderView::RedrawCanvas")]
        self.assertIn("stroke.median[0]", marker)
        self.assertNotIn("for (", marker)
        self.assertNotIn("DrawPolyline", marker)
        self.assertNotIn("DrawLine", marker)
        active = draw[draw.index("    if ((state == StrokeOrderUiState::Animating"):]
        self.assertIn("current < strokes && !controller_->in_gap()", active)
        self.assertEqual(active.count("DrawStartMarker("), 1)
        for call in ("DrawLine(", "DrawStrokeOutline(", "DrawGlyphOutlines("):
            self.assertNotIn(call, active)
        self.assertIn("reference = MixLight(theme->text_color(), theme->background_color())", draw)
        self.assertIn("done = theme->text_color()", draw)
        self.assertIn("accent = theme->user_bubble_color()", draw)

    def test_lvgl_admits_before_disarm_render_or_abort(self):
        view = (ROOT / "main/stroke_order/stroke_order_view.cc").read_text()
        candidate = view[view.index("void StrokeOrderView::CandidateClicked"):
                         view.index("void StrokeOrderView::ControlClicked")]
        handler = view[view.index("bool StrokeOrderView::HandleControlLocked"):
                       view.index("void StrokeOrderView::HandleStatePresentationLocked")]
        control = view[view.index("void StrokeOrderView::ControlClicked"):
                       view.index("void StrokeOrderView::OverlayDeleted")]
        for section in (candidate, handler):
            self.assertIn("StrokeOrderApplyUiAction", section)
            self.assertNotIn("coordinator_->Mark", section)
            self.assertNotIn("coordinator_->CurrentGeneration", section)
        self.assertLess(candidate.index("StrokeOrderApplyUiAction"), candidate.index("DisarmClick"))
        self.assertLess(candidate.index("DisarmClick"), candidate.index("HandleStatePresentationLocked"))
        self.assertNotIn("RequestAbort", candidate)
        self.assertLess(handler.index("StrokeOrderApplyUiAction"), handler.index("RequestAbortStrokeRound"))
        self.assertIn("if (self->HandleControlLocked(index) && index == 4)", control)
        self.assertLess(control.index("HandleControlLocked"), control.index("DisarmClick"))
        # No release/repeat handler or custom action queue can double-dispatch a touch.
        for name in ("EntryClicked", "CandidateClicked", "ControlClicked"):
            registrations = [line for line in view.splitlines() if "lv_obj_add_event_cb" in line
                             and name in line]
            self.assertTrue(registrations)
            self.assertTrue(all("LV_EVENT_CLICKED" in line for line in registrations))
        timer = view[view.index("void StrokeOrderView::AnimTimerCb"):]
        # Both LVGL paths must use the actual clock/admission helpers exercised
        # above under TSAN, not duplicate arithmetic tested only by source text.
        self.assertIn("StrokeOrderApplyAnimationTick", timer)
        for section in (timer, handler, candidate):
            self.assertIn("anim_clock_", section)
            self.assertIn("esp_timer_get_time()", section)
            self.assertNotIn("esp_timer_get_time() / 1000", section)
        self.assertNotIn("controller_->Tick", timer)
        self.assertNotIn("controller_->Pause", handler)
        self.assertNotIn("LV_OBJ_FLAG_CLICKABLE", timer)
        sync = view[view.index("bool StrokeOrderView::SyncAnimTimer"):
                    view.index("bool StrokeOrderView::EnsureOverlay")]
        self.assertIn("anim_clock_.Sync(state", sync)
        self.assertEqual(sync.count("StrokeOrderController::kTimerPeriodMs"), 2)
        self.assertNotIn("kMaxAnimationAdvanceMs", sync)
        self.assertNotIn("anim_clock_.Reset", sync)
        # Every admitted timer boundary synchronously redraws before returning
        # to LVGL. Candidate/replay/control paths also draw before a later tick.
        self.assertEqual(timer.count("StrokeOrderApplyAnimationTick"), 1)
        self.assertLess(timer.index("StrokeOrderApplyAnimationTick"), timer.index("RedrawCanvas()"))
        self.assertLess(handler.index("StrokeOrderApplyUiAction"), handler.index("RedrawCanvas()"))
        page = view[view.index("bool StrokeOrderView::RenderAnimationPage"):
                    view.index("bool StrokeOrderView::RenderErrorPage")]
        self.assertIn("RedrawCanvas()", page)

    def test_fresh_token_wires_candidate_replay_retry_and_timer_only(self):
        view = (ROOT / "main/stroke_order/stroke_order_view.cc").read_text()
        header = (ROOT / "main/stroke_order/stroke_order_view.h").read_text()
        action = (ROOT / "main/stroke_order/stroke_order_ui_action.h").read_text()
        page = view[view.index("bool StrokeOrderView::RenderAnimationPage"):
                    view.index("bool StrokeOrderView::RenderErrorPage")]
        self.assertIn("StopAnimTimer(false)", page)
        self.assertNotIn("anim_clock_.Reset", page)
        self.assertLess(page.index("StopAnimTimer(false)"), page.index("RedrawCanvas()"))
        self.assertLess(page.index("RedrawCanvas()"), page.index("SyncAnimTimer()"))
        self.assertIn("StopAnimTimer(bool reset_clock = true)", header)
        stop = view[view.index("void StrokeOrderView::StopAnimTimer"):
                    view.index("bool StrokeOrderView::SyncAnimTimer")]
        self.assertIn("if (reset_clock)", stop)
        self.assertIn("anim_clock_.Reset()", stop)
        # All three successful fresh actions share one admission-owned seam;
        # failed RetryLoad, rejected actions, Resume and Sync cannot mint tokens.
        applied = action[action.index("        if (applied) {"):]
        self.assertIn("action == StrokeOrderUiAction::Candidate", applied)
        self.assertIn("action == StrokeOrderUiAction::Replay", applied)
        self.assertIn("action == StrokeOrderUiAction::RetryLoad", applied)
        self.assertEqual(applied.count("clock.BeginPlayback()"), 1)
        self.assertNotIn("BeginPlayback()", view)
        tick = action[action.index("inline bool StrokeOrderApplyAnimationTick"):
                      action.index("enum class StrokeOrderUiAction")]
        self.assertLess(tick.index("coordinator.TryUiAction"), tick.index("clock.OnTimer"))
        self.assertLess(tick.index("!session.IsCurrentGeneration"), tick.index("clock.OnTimer"))
        self.assertLess(tick.index("controller.state() !="), tick.index("clock.OnTimer"))
        self.assertEqual(action.count("clock.OnTimer("), 1)
        self.assertNotIn("lv_refr_now", view)
        self.assertNotIn("controller_->Tick", view)
        sync = view[view.index("bool StrokeOrderView::SyncAnimTimer"):
                    view.index("bool StrokeOrderView::EnsureOverlay")]
        self.assertNotIn("BeginPlayback", sync)
        self.assertNotIn("first_frame_pending", sync)
        for start, end in (("OverlayDeleted", "EntryDeleted"),
                           ("EntryDeleted", "CandidateDraw")):
            deleted = view[view.index("void StrokeOrderView::" + start):
                           view.index("void StrokeOrderView::" + end)]
            self.assertIn("StopAnimTimer()", deleted)

    def test_timeout_closes_retired_overlay_and_restores_entry(self):
        view = (ROOT / "main/stroke_order/stroke_order_view.cc").read_text()
        timeout = view[view.index("void StrokeOrderView::ShowSpeechTimedOutFromMain"):
                       view.index("void StrokeOrderView::AbortFromMain")]
        self.assertIn("lifecycle_.SetDeviceIdle(true)", timeout)
        self.assertIn("CancelSessionLocked(false)", timeout)
        self.assertIn("display_->ShowNotification", timeout)
        self.assertNotIn("RenderTimedOut", view)
        self.assertNotIn("TryOpenOverlay", timeout)
        self.assertNotIn("RequestStartStrokeRound", timeout)
        close = view[view.index("void StrokeOrderView::CancelSessionLocked"):
                     view.index("void StrokeOrderView::RequestAbortLocked")]
        for token in ("session_.Cancel()", "presented_generation_ = 0", "controller_->Exit()",
                      "lifecycle_.SetListenHold(false)", "lifecycle_.CloseOverlay()",
                      "DestroyOverlay()", "ReevaluateEntryLocked()"):
            self.assertIn(token, close)
        entry = view[view.index("bool StrokeOrderView::ShowEntryLocked"):
                     view.index("void StrokeOrderView::HideEntryLocked")]
        self.assertIn("lv_obj_add_flag(entry_, LV_OBJ_FLAG_CLICKABLE)", entry)
        self.assertIn("lv_obj_remove_flag(entry_, LV_OBJ_FLAG_HIDDEN)", entry)

    def test_pressed_visuals_cover_candidates_and_animation_controls(self):
        view = (ROOT / "main/stroke_order/stroke_order_view.cc").read_text()
        style = view[view.index("void StyleControl"):view.index("}  // namespace")]
        self.assertIn("lv_obj_set_style_bg_color", style)
        self.assertIn("lv_color_mix", style)
        self.assertIn("lv_obj_set_style_border_color(obj, theme->text_color(), LV_STATE_PRESSED)", style)
        self.assertIn("lv_obj_set_style_border_width(obj, 3, LV_STATE_PRESSED)", style)
        self.assertEqual(style.count("LV_STATE_PRESSED"), 3)
        self.assertNotIn("lv_timer_create", style)
        for start, end in [("RenderCandidates", "RenderAnimationPage"),
                           ("RenderAnimationPage", "RenderErrorPage")]:
            page = view[view.index("bool StrokeOrderView::" + start):
                        view.index("bool StrokeOrderView::" + end)]
            self.assertIn("StyleControl(button, theme)", page)
            self.assertIn("LV_EVENT_CLICKED", page)

    def test_feedback_audio_and_broad_helpers_are_absent(self):
        for path in [ROOT / "main/application.cc", ROOT / "main/application.h",
                     ROOT / "main/audio/audio_service.cc", ROOT / "main/audio/audio_service.h",
                     *list((ROOT / "main/stroke_order").glob("*.h")),
                     *list((ROOT / "main/stroke_order").glob("*.cc"))]:
            source = path.read_text()
            for token in ("TryPlayUiFeedback", "RequestTouchFeedback", "TryUiFeedback",
                          "RequestStrokeOrderTouchFeedback", "MAIN_EVENT_STROKE_TOUCH_FEEDBACK",
                          "StrokeOrderFeedbackCounter"):
                self.assertNotIn(token, source, str(path))
        view = (ROOT / "main/stroke_order/stroke_order_view.cc").read_text()
        self.assertNotIn("PlaySound", view)
        for path in ("main/audio/audio_capture_admission.h", "main/audio/audio_codec_io.h",
                     "main/audio/audio_playback_slot_queue.h", "main/audio/audio_shutdown_contract.h",
                     "main/audio/ui_feedback_tone.h", "main/stroke_order/stroke_order_feedback.h"):
            self.assertFalse((ROOT / path).exists(), path)


if __name__ == "__main__":
    unittest.main()
