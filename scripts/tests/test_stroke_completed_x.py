"""Completed X: real LVGL events through verbatim production View methods.

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

from test_stroke_order_render import LVGL, prepare
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
        cmake = directory / "CMakeLists.txt"
        source = cmake.read_text().replace(
            "scripts/tests/stroke_order_render_harness.cc",
            "scripts/tests/stroke_completed_x_harness.cc",
        ).replace("-fsanitize=undefined", "-fsanitize=address,undefined")
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

    def test_completed_x_returns_candidates_and_reselects(self):
        self.run_mode("completed")

    def test_completed_x_disarms_before_target_deletion(self):
        self.run_mode("delete-order")

    def test_completed_x_try_lock_rejection_is_retryable(self):
        self.run_mode("contention")

    def test_completed_x_cancel_and_stale_generations_are_inert(self):
        self.run_mode("fences")

    def test_other_x_states_still_exit_and_abort(self):
        self.run_mode("other-states")

    def test_completed_x_unavailable_candidates_fail_closed(self):
        self.run_mode("unavailable")

    def test_old_control_is_not_dispatched_on_new_page(self):
        self.run_mode("old-control")

    def test_pointer_press_release_dispatches_once_and_can_reselect(self):
        self.run_mode("pointer")

    def test_real_connect_page_x_aborts_once(self):
        self.run_mode("connect-x")

    def test_real_speak_page_x_aborts_once(self):
        self.run_mode("speak-x")

    def test_real_loading_page_x_aborts_once(self):
        self.run_mode("loading-x")

    def test_real_no_match_page_x_aborts_once(self):
        self.run_mode("nomatch-x")

    def test_real_retry_page_x_aborts_once(self):
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


if __name__ == "__main__":
    unittest.main()
