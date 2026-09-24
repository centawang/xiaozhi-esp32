"""W7b1 portable production worker + real3500 controller, ASan/UBSan."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from scripts.tests import test_stroke_order_source as source_tests

ROOT = Path(__file__).resolve().parents[2]


class StrokeOrderWorkerTest(unittest.TestCase):
    def test_bounded_worker_real_corpus_and_lifetime(self):
        before = source_tests.StrokeOrderSourceTest.fixture_hashes()
        with tempfile.TemporaryDirectory(prefix="stroke-w7b1-worker-") as tmp:
            directory = Path(tmp)
            source = ROOT / "managed_components/laride__heatshrink"
            flags = ["-Wall", "-Wextra", "-Werror", "-O1", "-g",
                     "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                     "-fno-omit-frame-pointer"]
            subprocess.run(["cc", "-std=c11", *flags, "-I", str(source / "include"), "-c",
                            str(source / "heatshrink_decoder.c"), "-o", str(directory / "hs.o")],
                           check=True)
            command = [os.environ.get("CXX", "c++"), "-std=c++17", *flags, "-pthread",
                       "-fno-exceptions", "-fno-rtti", "-DSTROKE_ORDER_TESTING=1",
                       "-I", str(ROOT / "main"), "-I", str(source / "include")]
            if sys.platform == "darwin":
                sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
                command += ["-isystem", f"{sdk}/usr/include/c++/v1"]
            command += [str(ROOT / "scripts/tests/stroke_order_worker_harness.cc")]
            command += [str(ROOT / f"main/stroke_order/{name}.cc") for name in
                        ("stroke_order_worker", "stroke_order_controller", "stroke_order_source_adapter",
                         "stroke_order_catalog", "stroke_order_pinyin", "stroke_order_store",
                         "stroke_order_v2", "stroke_round_coordinator")]
            exe = directory / "worker-harness"
            subprocess.run([*command, str(directory / "hs.o"), "-o", str(exe)], check=True)
            run = subprocess.run([str(exe), str(ROOT / "scripts/tests/fixtures/stroke_order/prototype_3500")],
                                 text=True, capture_output=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("PASS W7b1 worker", run.stdout)
            self.assertNotIn("Sanitizer", run.stderr)
        self.assertEqual(before, source_tests.StrokeOrderSourceTest.fixture_hashes())

    def test_product_preparation_methods_with_portable_ui_stubs(self):
        view = (ROOT / "main/stroke_order/stroke_order_view.cc").read_text()
        begin = view.index("void StrokeOrderView::CancelPrepareLocked(")
        end = view.index("#endif\n\nbool StrokeOrderView::ShowEntryLocked()", begin)
        prefix = (ROOT / "scripts/tests/stroke_order_product_harness.inc").read_text()
        visual_begin = view.index("void StrokeOrderView::CancelSessionLocked(bool hide_entry)")
        visual_end = view.index("void StrokeOrderView::RequestAbortLocked", visual_begin)
        harness = prefix.replace("// PRODUCTION_METHODS_INSERTED_HERE",
                                 view[begin:end] + view[visual_begin:visual_end])
        with tempfile.TemporaryDirectory(prefix="stroke-w7b1-product-") as tmp:
            directory = Path(tmp)
            (directory / "product.cc").write_text(harness)
            source = ROOT / "managed_components/laride__heatshrink"
            flags = ["-Wall", "-Wextra", "-Werror", "-O1", "-g",
                     "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
            subprocess.run(["cc", "-std=c11", *flags, "-I", str(source / "include"), "-c",
                            str(source / "heatshrink_decoder.c"), "-o", str(directory / "hs.o")], check=True)
            command = [os.environ.get("CXX", "c++"), "-std=c++17", *flags, "-pthread",
                       "-fno-exceptions", "-fno-rtti", "-DCONFIG_STROKE_ORDER_DATASET_LEVEL1_3500=1", "-DSTROKE_ORDER_TESTING=1",
                       "-I", str(ROOT / "main"),
                       "-I", str(source / "include")]
            if sys.platform == "darwin":
                sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
                command += ["-isystem", f"{sdk}/usr/include/c++/v1"]
            command += [str(directory / "product.cc")]
            command += [str(ROOT / f"main/stroke_order/{name}.cc") for name in
                        ("stroke_order_worker", "stroke_order_controller", "stroke_order_source_adapter",
                         "stroke_order_catalog", "stroke_order_pinyin", "stroke_order_store",
                         "stroke_order_v2", "stroke_round_coordinator")]
            exe = directory / "product"
            subprocess.run([*command, str(directory / "hs.o"), "-o", str(exe)], check=True)
            subprocess.run([str(exe), str(ROOT / "scripts/tests/fixtures/stroke_order/prototype_3500")],
                           check=True, timeout=60)

    def test_product_guards_and_unmap_abort(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text()
        self.assertIn('if(CONFIG_STROKE_ORDER_DATASET_LEVEL1_3500)\n'
                      '        list(APPEND SOURCES "stroke_order/stroke_order_worker.cc")', cmake)
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        self.assertIn("default STROKE_ORDER_DATASET_LEVEL1_3500", kconfig)
        assets = (ROOT / "main/assets.cc").read_text()
        unload = assets[assets.index("bool Assets::UnApplyPartition()"):assets.index("void Assets::UseBuiltInTextFontCapability()")]
        self.assertLess(unload.index("stroke_mapping_accepting_ = false"), unload.index("SuspendAssets()"))
        self.assertLess(unload.index("SuspendAssets()"), unload.index("strategy_->UnApplyPartition(this)"))
        self.assertLess(unload.index("stroke_mapping_pin_.use_count() != 1"), unload.index("strategy_->UnApplyPartition(this)"))
        download = assets[assets.index("bool Assets::Download("):]
        self.assertLess(download.index("if (!UnApplyPartition())"), download.index("esp_partition_erase_range"))
        view = (ROOT / "main/stroke_order/stroke_order_view.cc").read_text()
        self.assertNotIn("PrepareBundle(", view)
        self.assertNotIn("PrepareGlyphs(", view)
        self.assertIn("self->coordinator_->TryUiAction(", view)


if __name__ == "__main__":
    unittest.main()
