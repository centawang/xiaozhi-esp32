"""Demonstration X -> fresh Speak: real LVGL + production main-task start path.

Uses the existing render harness build seam, with separate tests so the approved
render-optimization harness remains unchanged. ASan/UBSan cover both LVGL and
the View/controller/session/coordinator. No firmware/generated tree is edited.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_stroke_order_render import LVGL, function, prepare
from test_stroke_order_ui import ROOT


class StrokeCompletedXTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not LVGL.is_dir():
            raise unittest.SkipTest("resolved LVGL dependency unavailable")
        if not shutil.which("cmake"):
            raise AssertionError("cmake required for production LVGL event regression")
        cls.tmp = tempfile.TemporaryDirectory(prefix="stroke-completed-x-")
        cls.addClassCleanup(cls.tmp.cleanup)
        directory = Path(cls.tmp.name)
        cls.fixture = prepare(directory)
        view = (ROOT / "main/stroke_order/stroke_order_view.cc").read_text()
        with (directory / "production.inc").open("a") as out:
            for name in ("bool StrokeOrderView::StartVoiceSessionFromMain",
                         "bool StrokeOrderView::ShowListeningFromMain",
                         "bool StrokeOrderView::RenderConnecting",
                         "bool StrokeOrderView::ApplyVoiceUtteranceLocked",
                         "bool StrokeOrderView::RenderNoMatch",
                         "bool StrokeOrderView::RenderAwaitingSpeech",
                         "void StrokeOrderView::AbortFromMain",
                         "bool StrokeOrderView::IsOverlayActive",
                         "void StrokeOrderView::CancelSessionLocked",
                         "void StrokeOrderView::InvalidateVisualsLocked"):
                out.write(function(view, name))
        with (directory / "display.h").open("a") as out:
            out.write("\nstruct DisplayLockGuard { explicit DisplayLockGuard(Display*) {} "
                      "explicit operator bool() const { return true; } };\n")
        display = directory / "display.h"
        display.write_text(display.read_text().replace("    LvglTheme theme;",
                          "    void ShowNotification(const char*) {}\n"
                          "    void SetChatMessage(const char*, const char*) {}\n    LvglTheme theme;"))
        app = (ROOT / "main/application.cc").read_text()
        (directory / "application.inc").write_text("\n".join(
            function(app, name) for name in (
                "void Application::RequestStartStrokeRound",
                "void Application::InvalidateStrokeStartLocked",
                "bool Application::IsStrokeAbortPending",
                "void Application::RequestAbortStrokeRound",
                "void Application::PublishStrokeCancelFence",
                "void Application::HandleStrokeAbortEvent",
                "void Application::AbortStrokeRound",
                "void Application::HandleStrokeStartEvent",
                "void Application::BeginStrokeRoundFromMain")))
        # The actual registered transport close callback, including its routing
        # and publication, wrapped only to supply the captured Board reference.
        callback = function(app, "protocol_->OnAudioChannelClosed")
        callback = callback[callback.index("{"):]
        with (directory / "application.inc").open("a") as out:
            out.write("\nvoid Application::OnClosed(const AudioChannelCloseInfo& info) " +
                      callback.replace("{", "{\n auto& board = Board::GetInstance();", 1))
        # Scheduling hooks only: retain every production statement and branch.
        # Deterministically stop after dequeue and on both sides of BeginRound.
        inc = directory / "application.inc"
        bodies = inc.read_text()
        bodies = bodies.replace(
            "    BeginStrokeRoundFromMain(expected_generation, sequence);",
            '    TestBoundary("dequeued");\n    BeginStrokeRoundFromMain(expected_generation, sequence);')
        bodies = bodies.replace(
            "    uint64_t generation = 0;\n    {\n        std::lock_guard<std::mutex> lock(stroke_command_mutex_);",
            '    TestBoundary("before-commit");\n    uint64_t generation = 0;\n    {\n        std::lock_guard<std::mutex> lock(stroke_command_mutex_);')
        bodies = bodies.replace(
            "    if (stroke_round_.HasCancelFence(generation) || IsStrokeAbortPending(generation)) {",
            '    TestBoundary("after-commit");\n    if (stroke_round_.HasCancelFence(generation) || IsStrokeAbortPending(generation)) {')
        inc.write_text(bodies)
        # Extract the real main-loop ordering, not a start-first test model.
        start = app.index("        if (bits & MAIN_EVENT_STROKE_ABORT)")
        end = app.index("#endif", start)
        (directory / "dispatch.inc").write_text(app[start:end])
        cmake = directory / "CMakeLists.txt"
        source = cmake.read_text().replace(
            "scripts/tests/stroke_order_render_harness.cc",
            "scripts/tests/stroke_completed_x_harness.cc",
        ).replace("-fsanitize=undefined", "-fsanitize=address,undefined")
        source += "\ntarget_compile_definitions(render PRIVATE CONFIG_STROKE_ORDER_LOCAL=1)\n"
        source += "\ntarget_compile_options(lvgl PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)\n"
        cmake.write_text(source)
        for command in (["cmake", "-S", str(directory), "-B", str(directory / "build"),
                         "-DCMAKE_BUILD_TYPE=Debug"],
                        ["cmake", "--build", str(directory / "build"), "-j", "8", "--target", "render"]):
            run = subprocess.run(command, capture_output=True, text=True, timeout=240)
            if run.returncode:
                raise AssertionError(" ".join(command) + "\n" + run.stdout + run.stderr)
        cls.executable = directory / "build/render"

    def run_mode(self, mode):
        run = subprocess.run([str(self.executable), self.fixture, mode],
                             capture_output=True, text=True, timeout=30,
                             env={**os.environ, "ASAN_OPTIONS": "halt_on_error=1",
                                  "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn(mode + ": PASS", run.stdout)
        print(run.stdout, end="")

    def test_completed_x_restarts_through_main_task_to_speak(self):
        self.run_mode("completed")

    def test_completed_x_disarms_before_target_deletion(self):
        self.run_mode("delete-order")

    def test_completed_x_try_lock_rejection_is_retryable(self):
        self.run_mode("contention")

    def test_completed_x_cancel_and_stale_generations_are_inert(self):
        self.run_mode("fences")

    def test_demonstration_x_states_restart_but_candidate_x_exits(self):
        self.run_mode("other-states")

    def test_completed_x_does_not_require_retained_candidates(self):
        self.run_mode("unavailable")

    def test_old_control_is_not_dispatched_on_new_page(self):
        self.run_mode("old-control")

    def test_pointer_x_dispatches_once_without_closing_new_speak(self):
        self.run_mode("pointer")

    def test_real_connect_page_x_aborts_once(self):
        self.run_mode("connect-x")

    def test_real_speak_page_x_aborts_once(self):
        self.run_mode("speak-x")

    def test_real_loading_page_x_restarts_to_speak(self):
        self.run_mode("loading-x")

    def test_real_no_match_page_x_aborts_once(self):
        self.run_mode("nomatch-x")

    def test_real_retry_page_x_exits(self):
        self.run_mode("retry-x")

    def test_real_no_match_and_retry_r_request_restart(self):
        self.run_mode("voice-retry")

    def test_real_error_r_deletes_target_and_loads_animation(self):
        self.run_mode("error-retry")

    def test_real_error_back_deletes_target_and_allows_reselection(self):
        self.run_mode("error-back")

    def test_real_page_controls_reject_fences_and_retry_after_contention(self):
        self.run_mode("page-rejections")

    def test_retired_status_error_controls_cannot_operate_rebuilt_pages(self):
        self.run_mode("retired-pages")

    def test_every_page_retires_control_slots_before_child_deletion(self):
        self.run_mode("slot-lifetime")

    def test_back_retains_candidates_without_starting_voice(self):
        self.run_mode("back")

    def test_old_touch_tick_stt_and_channel_close_cannot_overwrite_speak(self):
        self.run_mode("stale-speak")

    def test_delayed_restart_cannot_replace_a_newer_generation(self):
        self.run_mode("stale-start")

    def test_unavailable_voice_uses_same_fallback_as_initial_so(self):
        self.run_mode("offline")

    def test_abort_after_x_dominates_real_abort_before_start_dispatch(self):
        self.run_mode("abort-order")

    def test_real_channel_close_callback_invalidates_pending_x(self):
        self.run_mode("channel-close")

    def test_cancel_at_dequeue_teardown_and_commit_windows(self):
        self.run_mode("cancel-windows")

    def test_start_abort_slot_overwrite_and_two_consecutive_x(self):
        self.run_mode("slot-order")

    def test_concurrent_start_abort_publications(self):
        self.run_mode("publish-race")

    def test_candidates_load_failure_x_exits(self):
        self.run_mode("candidate-failure")


if __name__ == "__main__":
    unittest.main()
