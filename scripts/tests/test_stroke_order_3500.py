"""Offline audit fixture tests; no device integration or automatic downloads.

Optional fixed-input regeneration accepts STROKE3500_REGEN_INPUTS pointing to
an external JSON object of seven absolute local paths (the generator's input
option names with underscores). An absent opt-in is an explicit skip; invalid
or missing requested inputs fail, never trigger a download or a replacement.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from stroke_order.corpus2 import parse_checksums  # noqa: E402
from stroke_order.policy3500 import (  # noqa: E402
    ASSETS_BASELINE, ASSETS_LIMIT, OLD_STROKE_BYTES, SOURCE_PINS,
    verify_generated, verify_notices,
)
from stroke_order.select import load_selection_csv  # noqa: E402

FIXTURES = ROOT / "scripts/tests/fixtures/stroke_order"
PROTOTYPE = FIXTURES / "prototype_3500"
CORPUS_ID = "bcd474c127b2138738d96b9bf64832ad"
CHECKSUMS_SHA256 = "a1db55de14d17cf7e8113709391c525b3460fe3ae13f80ac76a5143f59d4bc13"
SHARD_SIZES = [1048204, 1047698, 1048441, 1047547, 1047300, 323134]
SHARDS = [f"so{i:02d}.bin" for i in range(6)]
MEMBERS = set(SHARDS) | {
    "stroke_cat.bin", "stroke_pinyin.bin", "selection-3500.csv", "charset-3500.txt",
    "source.json", "readings.json", "coverage.json", "runtime.json",
    "ARPHICPL.TXT", "UNICODE-LICENSE.txt", "NOTICE.md", "SHA256SUMS",
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class StrokeOrder3500Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Full provenance policy, not only structural parsing or self-asserted JSON.
        # Missing codec source pins or any invalid fixture is a failure, not a skip.
        cls.checked = verify_generated(PROTOTYPE)
        cls.runtime = json.loads(cls.checked["files"]["runtime.json"])

    def test_exact_closed_members_hashes_and_size(self):
        self.assertEqual({p.name for p in PROTOTYPE.iterdir()}, MEMBERS)
        self.assertEqual(len(MEMBERS), 18)
        self.assertTrue(all(p.is_file() and not p.is_symlink() for p in PROTOTYPE.iterdir()))
        self.assertEqual(sha256(PROTOTYPE / "SHA256SUMS"), CHECKSUMS_SHA256)
        checksums = parse_checksums((PROTOTYPE / "SHA256SUMS").read_bytes())
        self.assertEqual(set(checksums), MEMBERS - {"SHA256SUMS"})
        for name, expected in checksums.items():
            with self.subTest(name=name):
                self.assertEqual(sha256(PROTOTYPE / name), expected)
        self.assertEqual(sum(p.stat().st_size for p in PROTOTYPE.iterdir()), 7_240_949)

    def test_policy_identity_and_complete_graphics_pinyin(self):
        self.assertEqual(self.checked["corpus_id"], CORPUS_ID)
        catalog = self.checked["parsed"]["catalog"]
        spy = self.checked["parsed"]["pinyin"]
        self.assertEqual((len(catalog["characters"]), len(self.checked["shard_names"]),
                          spy["relations"], spy["aux"],
                          max(len(c["readings"]) for c in spy["characters"]),
                          max(map(len, spy["groups"]))), (3500, 6, 5367, 1233, 10, 37))
        self.assertEqual(len(spy["characters"]), 3500)
        self.assertEqual(sorted(c["rank"] for c in catalog["characters"]), list(range(1, 3501)))
        dun = next(c for c in spy["characters"] if c["codepoint"] == ord("敦"))
        self.assertEqual(len(dun["readings"]), 10)
        self.assertEqual(self.runtime["legacy_golden_shards_equal"], 8)
        for key in ("graphics_errors", "pinyin_errors", "ignored_supplement_tokens"):
            self.assertEqual(self.runtime[key], 0)

    def test_full_tier1_selection_and_unchanged_legacy_prefix(self):
        rows = load_selection_csv(PROTOTYPE / "selection-3500.csv", 3500)
        legacy = load_selection_csv(FIXTURES / "prototype_2000/selection-2000.csv", 2000)
        self.assertEqual(rows[:2000], legacy)
        self.assertEqual([r["rank"] for r in rows], list(range(1, 3501)))
        self.assertEqual((rows[0]["official_number"], rows[-1]["official_number"]), ("0001", "3500"))
        self.assertEqual(len({r["codepoint_int"] for r in rows}), 3500)

    def test_source_and_license_pins_and_prototype_notice(self):
        files = self.checked["files"]
        self.assertEqual(json.loads(files["source.json"]), SOURCE_PINS)
        verify_notices(files)
        self.assertEqual(SOURCE_PINS["hwd_commit"], "68d10a4b21150cae5e1ebbd223eed289cf32d90c")
        self.assertEqual(SOURCE_PINS["table_commit"], "f9786a82be6e1672bdc60f85760a9e4a3791d1f1")
        notice = files["NOTICE.md"].decode("utf-8")
        for marker in ("technical prototype", "no explicit in-tree license", "not a release library",
                       "PDF dual-person page review", "NOT supported by the current firmware"):
            self.assertIn(marker, notice)
        self.assertIs(self.runtime["prototype"], True)
        self.assertIs(self.runtime["device_compatible"], False)

    def test_all_six_shards_within_one_mib_and_v2_magic(self):
        self.assertEqual(self.checked["shard_names"], SHARDS)
        sizes = [(PROTOTYPE / name).stat().st_size for name in SHARDS]
        self.assertEqual(sizes, SHARD_SIZES)
        self.assertTrue(all(size <= 1024 * 1024 for size in sizes))
        self.assertEqual(sum(sizes), 5_562_324)
        for name in SHARDS:
            with (PROTOTYPE / name).open("rb") as stream:
                self.assertEqual(stream.read(4), b"SOB2")
        for name, magic in (("stroke_cat.bin", b"SCB2"), ("stroke_pinyin.bin", b"SPY2")):
            self.assertEqual(self.checked["files"][name][:4], magic)

    def test_conditional_assets_projection_is_not_a_firmware_measurement(self):
        # Same other assets and packaging overhead as the pinned 2000 baseline.
        # This arithmetic is NOT a newly generated assets.bin or device claim.
        self.assertEqual((ASSETS_BASELINE, OLD_STROKE_BYTES, ASSETS_LIMIT),
                         (7_568_207, 5_888_462, 8_126_464))
        files = self.checked["files"]
        projection = (ASSETS_BASELINE - OLD_STROKE_BYTES + sum(SHARD_SIZES)
                      + len(files["stroke_cat.bin"]) + len(files["stroke_pinyin.bin"])
                      + 46 * (len(SHARDS) - 8))
        self.assertEqual(projection, 7_352_753)
        self.assertEqual(self.runtime["conditional_assets_bytes"], projection)
        self.assertIs(self.runtime["other_assets_unchanged_assumption"], True)
        self.assertEqual(ASSETS_LIMIT - projection, 773_711)
        self.assertGreaterEqual(8 * 1024 * 1024 - projection, 256 * 1024)

    def test_package_cli_create_verify_preserves_payload_and_host_only_flag(self):
        with tempfile.TemporaryDirectory(prefix="stroke3500-fixture-") as tmp:
            output = Path(tmp) / "package"
            command = [sys.executable, str(ROOT / "scripts/package_stroke_order_3500.py")]
            results = []
            for args in (["--source", str(PROTOTYPE), "--output", str(output)],
                         ["--verify", str(output)]):
                result = subprocess.run(command + args, capture_output=True, text=True, timeout=180)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                results.append(json.loads(result.stdout))
            self.assertEqual(results[0], results[1])
            manifest = results[0]
            self.assertEqual(manifest["corpus_id"], CORPUS_ID)
            self.assertIs(manifest["device_compatible"], False)
            names = set(SHARDS) | {"stroke_cat.bin", "stroke_pinyin.bin", "ARPHICPL.TXT",
                                    "UNICODE-LICENSE.txt", "NOTICE.md"}
            self.assertEqual(set(manifest["files"]), names)
            self.assertEqual({p.name for p in output.iterdir()}, names | {"package.json", "SHA256SUMS"})
            for name in names:
                self.assertEqual((output / name).read_bytes(), (PROTOTYPE / name).read_bytes(), name)


class LegacyFixtureHashTest(unittest.TestCase):
    def test_v1_500_and_2000_closed_fixture_hashes_unchanged(self):
        # Pin the manifests themselves: rewriting payload AND manifest must fail.
        pins = {
            "prototype_500": "9dfd1ef0c6d3bb7c9420e904a0ac62912baa23b98d9f25e1bc4030555c9f7d3f",
            "prototype_2000": "2c5cafc3384332f7f81900f89b26ec37bad5639fef3ab7562ef2c7f64be370ab",
        }
        for dirname, expected in pins.items():
            with self.subTest(fixture=dirname):
                directory = FIXTURES / dirname
                self.assertEqual(sha256(directory / "SHA256SUMS"), expected)
                checks = parse_checksums((directory / "SHA256SUMS").read_bytes())
                self.assertEqual({p.name for p in directory.iterdir()}, set(checks) | {"SHA256SUMS"})
                for name, checksum in checks.items():
                    self.assertEqual(sha256(directory / name), checksum, f"{dirname}/{name}")


class FixedInputRegenerationTest(unittest.TestCase):
    def test_optional_external_fixed_input_regeneration_is_byte_identical(self):
        config = os.environ.get("STROKE3500_REGEN_INPUTS")
        if config is None:
            self.skipTest("STROKE3500_REGEN_INPUTS not set; external fixed-input regeneration not run (offline)")
        inputs = json.loads(Path(config).read_text(encoding="utf-8"))
        keys = {"table_json", "official_pdf", "hanzi_writer_data", "unihan_zip",
                "unicode_license", "selection_csv", "source_manifest"}
        self.assertIsInstance(inputs, dict)
        self.assertEqual(set(inputs), keys)
        command = [sys.executable, str(ROOT / "scripts/generate_stroke_order_3500.py")]
        for key in sorted(keys):
            value = inputs[key]
            self.assertIsInstance(value, str)
            self.assertTrue(Path(value).is_absolute() and not value.startswith("//"), key)
            self.assertTrue(Path(value).is_dir() if key == "hanzi_writer_data" else Path(value).is_file(), key)
            command.extend(["--" + key.replace("_", "-"), value])
        env = dict(os.environ, GIT_NO_LAZY_FETCH="1", GIT_TERMINAL_PROMPT="0")
        with tempfile.TemporaryDirectory(prefix="stroke3500-regeneration-") as tmp:
            output = Path(tmp) / "generated"
            result = subprocess.run(command + ["--output-dir", str(output)], env=env,
                                    capture_output=True, text=True, timeout=900)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual({p.name for p in output.iterdir()}, MEMBERS)
            for name in sorted(MEMBERS):
                self.assertEqual((output / name).read_bytes(), (PROTOTYPE / name).read_bytes(), name)


if __name__ == "__main__":
    unittest.main()
