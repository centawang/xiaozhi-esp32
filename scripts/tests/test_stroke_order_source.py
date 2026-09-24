"""W4b production Controller + source adapter, real checked-in v1/v2 corpora.

No input download, IDF, fixture generation, or optional environment-dependent skip.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "scripts/tests/fixtures/stroke_order"
sys.path.insert(0, str(ROOT / "scripts"))
from stroke_order import heatshrink  # noqa: E402


class StrokeOrderSourceTest(unittest.TestCase):
    def test_production_profiles_lifetime_rollback_and_interleaving(self):
        before = self.fixture_hashes()
        decoder_hashes = heatshrink.verified_source_hashes()
        with tempfile.TemporaryDirectory(prefix="stroke-w4b-") as tmp:
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
            command += [str(ROOT / "scripts/tests/stroke_order_source_harness.cc")]
            command += [str(ROOT / f"main/stroke_order/stroke_order_{name}.cc") for name in
                        ("controller", "source_adapter", "catalog", "pinyin", "store", "v2")]
            exe = directory / "source-harness"
            subprocess.run([*command, str(directory / "hs.o"), "-o", str(exe)], check=True)
            run = subprocess.run([str(exe), str(FIXTURES / "prototype_2000"),
                                  str(FIXTURES / "prototype_3500")],
                                 text=True, capture_output=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("PASS profile 2000", run.stdout)
            self.assertIn("PASS profile 3500", run.stdout)
            self.assertIn("PASS rollback generation controlled-interleave", run.stdout)
            self.assertIn("PASS W4b", run.stdout)
            self.assertNotIn("Sanitizer", run.stderr)
        self.assertEqual(before, self.fixture_hashes())
        self.assertEqual(decoder_hashes, heatshrink.verified_source_hashes())

    @staticmethod
    def fixture_hashes():
        result = {}
        for profile in ("prototype_2000", "prototype_3500"):
            directory = FIXTURES / profile
            for line in (directory / "SHA256SUMS").read_text().splitlines():
                digest, name = line.split("  ", 1)
                actual = hashlib.sha256((directory / name).read_bytes()).hexdigest()
                if digest != actual:
                    raise AssertionError(f"fixture checksum mismatch: {profile}/{name}")
                result[f"{profile}/{name}"] = actual
            result[f"{profile}/SHA256SUMS"] = hashlib.sha256(
                (directory / "SHA256SUMS").read_bytes()).hexdigest()
        return result


if __name__ == "__main__":
    unittest.main()
