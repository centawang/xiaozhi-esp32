"""Build/profile admission regressions; host checks only, no flashing."""
import copy
import os
import contextlib
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import verify_stroke_order_3500 as admission
import build_default_assets as assets
from stroke_order import dzz1, sob2, v2
from stroke_order.corpus2 import digest

FIXTURE = ROOT / "scripts/tests/fixtures/stroke_order/prototype_3500"


COMPILERS = {"cc": Path(shutil.which("cc")).resolve(), "cxx": Path(shutil.which("c++")).resolve()}


def admit(source, output=None, report=None):
    return admission.admit(source, output, report, **COMPILERS)


class BuildProfileTest(unittest.TestCase):
    def test_legacy_packager_preserves_pinned_payload_bytes(self):
        from package_stroke_order_2000 import COPY_MAP, package_corpus
        legacy = FIXTURE.with_name("prototype_2000")
        self.assertEqual(digest((legacy / "SHA256SUMS").read_bytes()),
                         "2c5cafc3384332f7f81900f89b26ec37bad5639fef3ab7562ef2c7f64be370ab")
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "legacy"
            package_corpus(str(legacy), str(output))
            self.assertEqual({p.name for p in output.iterdir()}, set(COPY_MAP.values()) | {"SHA256SUMS"})
            for source, destination in COPY_MAP.items():
                self.assertEqual((legacy / source).read_bytes(), (output / destination).read_bytes())

    def test_kconfig_default_off_level1_default_and_legacy_override(self):
        import kconfiglib
        text = (ROOT / "main/Kconfig.projbuild").read_text()
        block = text[text.index('menu "Stroke Order"'):].split("endmenu", 1)[0] + "endmenu\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Kconfig"
            path.write_text('config BOARD_TYPE_M5STACK_CORE_S3\n    bool\n    default y\n'
                            'config USE_EMOTE_MESSAGE_STYLE\n    bool\n' + block)
            k = kconfiglib.Kconfig(str(path), warn=False)
            local = k.syms["STROKE_ORDER_LOCAL"]
            old = k.syms["STROKE_ORDER_DATASET_LEGACY2000"]
            new = k.syms["STROKE_ORDER_DATASET_LEVEL1_3500"]
            self.assertEqual((local.str_value, old.str_value, new.str_value), ("n", "n", "n"))
            local.set_value(2)
            self.assertEqual((old.str_value, new.str_value), ("n", "y"))
            old.set_value(2)
            self.assertEqual((old.str_value, new.str_value), ("y", "n"))
            # Existing explicit Legacy selections survive the new default.
            saved = Path(tmp) / "sdkconfig"
            k.write_config(str(saved))
            k = kconfiglib.Kconfig(str(path), warn=False)
            k.load_config(str(saved))
            local = k.syms["STROKE_ORDER_LOCAL"]
            old = k.syms["STROKE_ORDER_DATASET_LEGACY2000"]
            new = k.syms["STROKE_ORDER_DATASET_LEVEL1_3500"]
            self.assertEqual((local.str_value, old.str_value, new.str_value), ("y", "y", "n"))
            new.set_value(2)
            self.assertEqual((old.str_value, new.str_value), ("n", "y"))
            self.assertIs(old.choice, new.choice)
            local.set_value(0)
            self.assertEqual((old.str_value, new.str_value), ("n", "n"))

    def test_cmake_real_configure_profiles_outputs_byproducts_and_deps(self):
        text = (ROOT / "main/CMakeLists.txt").read_text()
        block = text[text.index("# Optional local stroke-order"):text.index('# IDF discovers dependencies')]
        for profile in ("OFF", "LEGACY2000", "LEVEL1_3500"):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                prelude = (f'cmake_minimum_required(VERSION 3.16)\nproject(profile NONE)\n'
                           f'set(PROJECT_DIR "{ROOT}")\n'
                           f'set(CONFIG_STROKE_ORDER_LOCAL {"OFF" if profile == "OFF" else "ON"})\n'
                           f'set(CONFIG_STROKE_ORDER_DATASET_{profile} ON)\n')
                (directory / "CMakeLists.txt").write_text(prelude + block + '''
file(WRITE "${CMAKE_BINARY_DIR}/outputs" "${STROKE_ORDER_ASSET_OUTPUTS}")
file(WRITE "${CMAKE_BINARY_DIR}/sources" "${SOURCES}")
add_custom_target(assets ALL DEPENDS ${STROKE_ORDER_ASSET_OUTPUTS})
''')
                run = subprocess.run(["cmake", "-G", "Ninja", "-S", str(directory), "-B", str(directory / "build")],
                                     capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                names = {Path(n).name for n in (directory / "build/outputs").read_text().split(";") if n}
                sources = (directory / "build/sources").read_text()
                ninja = (directory / "build/build.ninja").read_text()
                if profile == "OFF":
                    self.assertFalse(names)
                    self.assertNotIn("stroke_order_v2.cc", sources)
                    continue
                for name in ("stroke_order_v2.cc", "stroke_order_source_adapter.cc", "stroke_order_store.cc"):
                    self.assertIn(name, sources)
                if profile == "LEGACY2000":
                    self.assertIn("so07.bin", names)
                    self.assertIn("package_stroke_order_2000.py", ninja)
                    self.assertNotIn("verify_stroke_order_3500.py", ninja)
                else:
                    self.assertEqual(names, set(admission.DEVICE_FILES))
                    self.assertIn("stroke3500-admission.json", ninja)
                    self.assertIn("verify_stroke_order_3500.py", ninja)
                    for dependency in ("policy3500.py", "device_verify.cc", "stroke_order_v2.cc",
                                       "heatshrink_decoder.c", "heatshrink_config.h", "SHA256SUMS",
                                       "coverage.json", "so07.sob1"):
                        self.assertIn(dependency, ninja)
                    self.assertNotIn("package_stroke_order_2000.py", ninja)
        registration = text[text.index('idf_component_register(SRCS ${SOURCES}'):]
        self.assertIn('                        laride__heatshrink\n', registration)
        self.assertNotIn('list(APPEND MAIN_PRIV_REQUIRES_EXTRA laride__heatshrink)', text)
        self.assertIn('"--min_free_bytes" "262144"', text)

    def test_ninja_rebuild_missing_outputs_and_switch_profiles_same_root(self):
        text = (ROOT / "main/CMakeLists.txt").read_text()
        block = text[text.index("# Optional local stroke-order"):text.index('# IDF discovers dependencies')]
        with tempfile.TemporaryDirectory(prefix="w7a-ninja-") as tmp:
            root = Path(tmp)
            (root / "CMakeLists.txt").write_text(
                f'cmake_minimum_required(VERSION 3.16)\nproject(profile NONE)\nset(PROJECT_DIR "{ROOT}")\n'
                + block + '\nadd_custom_target(assets ALL DEPENDS ${STROKE_ORDER_ASSET_OUTPUTS})\n'
                + 'file(WRITE "${CMAKE_BINARY_DIR}/selected" "${STROKE_ORDER_ASSETS_DIR}")\n')
            build = root / "build"
            def run(*args):
                result = subprocess.run(args, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout
            def configure(profile):
                run("cmake", "-G", "Ninja", "-S", str(root), "-B", str(build),
                    "-DCONFIG_STROKE_ORDER_LOCAL=ON",
                    f"-DCONFIG_STROKE_ORDER_DATASET_LEVEL1_3500={'ON' if profile == '3500' else 'OFF'}",
                    f"-DCONFIG_STROKE_ORDER_DATASET_LEGACY2000={'OFF' if profile == '3500' else 'ON'}",
                    f"-DSTROKE_ORDER_HOST_CC={COMPILERS['cc']}",
                    f"-DSTROKE_ORDER_HOST_CXX={COMPILERS['cxx']}")
                run("cmake", "--build", str(build))
                return Path((build / "selected").read_text())
            new = configure("3500")
            baseline = admission.snapshot(new)
            report = build / "stroke3500-admission.json"
            proof = report.read_bytes()
            for deleted in (report, new / "so05.bin"):
                deleted.unlink()
                self.assertIn("Admitting 3500/6", run("cmake", "--build", str(build)))
                self.assertEqual(admission.snapshot(new), baseline)
                self.assertEqual(report.read_bytes(), proof)
            old = configure("legacy")
            legacy = admission.snapshot(old)
            self.assertIn("so07.bin", legacy)
            self.assertEqual(configure("3500"), new)
            self.assertEqual(admission.snapshot(new), baseline)
            self.assertNotIn("so07.bin", admission.snapshot(new))
            self.assertEqual(configure("legacy"), old)
            self.assertEqual(admission.snapshot(old), legacy)
            self.assertEqual(report.read_bytes(), proof)


class DeviceAdmissionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="w7a-test-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.output = cls.root / "device"
        cls.report = cls.root / "admission.json"
        cls.result = admit(FIXTURE, cls.output, cls.report)

    def test_exact_eight_bytes_proof_and_unchanged_host_identity(self):
        self.assertEqual({p.name for p in self.output.iterdir()}, set(admission.DEVICE_FILES))
        for name in admission.DEVICE_FILES:
            self.assertEqual((self.output / name).read_bytes(), (FIXTURE / name).read_bytes())
        self.assertEqual(self.result["corpus_id"], admission.CORPUS_ID)
        self.assertEqual(self.result["max_stored_bytes"], 2945)
        self.assertIn("W4a ValidateBundle PASS 3500 6", self.result["reader_verification"])
        self.assertFalse(self.result["view_application_integrated"])
        self.assertFalse(json.loads((FIXTURE / "runtime.json").read_bytes())["device_compatible"])
        self.assertEqual(digest((FIXTURE / "SHA256SUMS").read_bytes()), admission.FIXTURE_SHA256)
        self.assertEqual(json.loads(self.report.read_bytes()), self.result)
        self.assertEqual(self.result["compilers"],
                         {name: admission.compiler_identity(path) for name, path in COMPILERS.items()})

    def test_closed_hash_failure_preserves_existing_output(self):
        source = self.root / "bad-source"
        shutil.copytree(FIXTURE, source)
        baseline = {p.name: p.read_bytes() for p in self.output.iterdir()}
        try:
            (source / "unexpected.json").write_text("{}")
            with self.assertRaises(ValueError):
                admit(source, self.output)
            (source / "unexpected.json").unlink()
            data = bytearray((source / "so00.bin").read_bytes()); data[-1] ^= 1
            (source / "so00.bin").write_bytes(data)
            with self.assertRaises(ValueError):
                admit(source, self.output)
            self.assertEqual({p.name: p.read_bytes() for p in self.output.iterdir()}, baseline)
        finally:
            shutil.rmtree(source)

    def test_mixed_identity_wrong_shards_and_host_valid_large_stored_rejected(self):
        raw = bytearray(struct.pack("<IHH", 0x4e00, 13, 0))
        for _ in range(13):
            outline = [(0, 0) if i % 2 == 0 else (1024, 1024) for i in range(255)] + [(0, 0)]
            median = [(0, 0) if i % 2 == 0 else (1024, 1024) for i in range(56)]
            raw.extend(struct.pack("<HH", len(outline), len(median)))
            for x, y in outline + median:
                raw.extend(struct.pack("<HH", x, y))
        transformed = dzz1.encode(bytes(raw))
        bits = "".join("1" + f"{b:08b}" for b in transformed)
        bits += "0" * (-len(bits) % 8)
        stored = int(bits, 2).to_bytes(len(bits) // 8, "big")
        self.assertGreater(len(stored), 16384)
        block = sob2.Block(0x4e00, 1, stored, len(transformed), len(raw), v2.crc(raw))
        blob = sob2.pack_sob2([block], bytes.fromhex(admission.CORPUS_ID))
        host = sob2.validate_sob2(blob)  # A genuinely host-valid record, not just forged JSON.
        verified = dict(corpus_id=admission.CORPUS_ID, shard_names=list(admission.DEVICE_FILES[:6]),
                        parsed={"catalog": {"shards": [{"characters": host["characters"] * 3500}]}})
        with self.assertRaisesRegex(ValueError, "16 KiB"):
            admission.check_device_profile(verified)
        good = copy.deepcopy(verified)
        for c in good["parsed"]["catalog"]["shards"][0]["characters"]:
            c["stored_bytes"] = 16384
        self.assertEqual(admission.check_device_profile(good), 16384)
        for change in ({"corpus_id": "00" * 16}, {"shard_names": list(admission.DEVICE_FILES[:5])}):
            with self.assertRaises(ValueError):
                admission.check_device_profile(dict(good, **change))

    def test_collision_and_device_assets_contain_no_host_metadata(self):
        stage = self.root / "merged"
        stage.mkdir()
        self.assertEqual(set(assets.process_extra_files(str(self.output), str(stage))), set(admission.DEVICE_FILES))
        with self.assertRaisesRegex(ValueError, "collision"):
            assets.process_extra_files(str(self.output), str(stage))
        image = self.root / "generated.bin"
        self.assertTrue(assets.build_assets_integrated([], [], None, None, str(self.output), str(image),
                                                      max_output_bytes=8388608, min_free_bytes=262144))
        blob = image.read_bytes(); count, checksum, size = struct.unpack_from("<III", blob)
        names = {struct.unpack_from("<32s", blob, 12 + i * 44)[0].split(b"\0")[0].decode()
                 for i in range(count)}
        self.assertEqual(names, set(admission.DEVICE_FILES) | {"index.json"})
        self.assertEqual((checksum, size), (sum(blob[12:]) & 65535, len(blob) - 12))

    def test_publish_rollback_and_no_source_or_report_overlap(self):
        output, staging = self.root / "transaction", self.root / "staging"
        output.mkdir(); staging.mkdir()
        (output / "old").write_text("preserve")
        (staging / "new").write_text("complete")
        rename = os.replace
        def fail_publish(path, destination):
            if path == staging:
                raise OSError("injected publish failure")
            return rename(path, destination)
        with mock.patch.object(admission.os, "replace", fail_publish):
            with self.assertRaises(OSError):
                admission.publish(staging, output)
        self.assertEqual({p.name for p in output.iterdir()}, {"old"})
        admission.publish(staging, output)
        self.assertEqual({p.name for p in output.iterdir()}, {"new"})
        for out, report in ((FIXTURE, None), (FIXTURE / "child", None),
                            (self.root / "safe", self.root / "safe/report.json")):
            with self.assertRaises(ValueError):
                admit(FIXTURE, out, report)


class AdmissionTransactionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="w7a-transaction-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.output = self.root / "output"
        self.report = self.root / "report.json"
        self.stage = self.root / "stage"
        self.pending = self.root / "pending"
        self.stage.mkdir()
        (self.stage / "new").write_bytes(b"new device")
        self.pending.write_bytes(b"new report")

    def old(self):
        self.output.mkdir()
        (self.output / "old").write_bytes(b"old device")
        self.report.write_bytes(b"old report")

    def assert_old(self):
        self.assertEqual(admission.snapshot(self.output), {"old": b"old device"})
        self.assertEqual(self.report.read_bytes(), b"old report")

    def backups(self):
        return sorted(self.root.glob(".*.backup-*"))

    def publish(self):
        admission.publish(self.stage, self.output, self.pending, self.report)

    def test_existing_absent_output_report_matrix(self):
        for out in (False, True):
            for report in (False, True):
                with self.subTest(output=out, report=report), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    staging, pending, target, proof = [root / n for n in ("stage", "pending", "output", "report")]
                    staging.mkdir(); (staging / "new").write_bytes(b"new")
                    pending.write_bytes(b"proof")
                    if out:
                        target.mkdir(); (target / "old").write_bytes(b"old")
                    if report:
                        proof.write_bytes(b"old proof")
                    admission.publish(staging, target, pending, proof)
                    self.assertEqual(admission.snapshot(target), {"new": b"new"})
                    self.assertEqual(proof.read_bytes(), b"proof")
                    self.assertEqual(list(root.glob(".*.backup-*")), [])

    def test_reject_wrong_types_symlinks_and_bidirectional_overlap_before_verification(self):
        self.old()
        link = self.root / "source-link"; link.symlink_to(self.source, target_is_directory=True)
        out_link = self.root / "out-link"; out_link.symlink_to(self.output, target_is_directory=True)
        rep_link = self.root / "rep-link"; rep_link.symlink_to(self.report)
        dangling = self.root / "dangling"; dangling.symlink_to(self.root / "missing")
        unrelated_directory = self.root / "unrelated-report-dir"; unrelated_directory.mkdir()
        cases = [(self.source, self.output, unrelated_directory),
                 (self.report, self.output, None),
                 (link, self.output, self.report), (self.source, out_link, self.report),
                 (self.source, self.output, rep_link), (self.source, self.report, None),
                 (self.source, self.output, self.source), (self.source, self.output, self.root),
                 (self.source, self.output, self.output), (self.source, self.output, self.output / "proof"),
                 (self.source, self.root, self.report), (self.source, self.source / "new", None),
                 (self.source, self.output, dangling), (self.source, dangling / "new", None),
                 (self.source, self.report / "new", None)]
        with mock.patch.object(admission, "verify_generated") as verify:
            for source, output, report in cases:
                with self.subTest(paths=(source, output, report)), self.assertRaises(ValueError):
                    admit(source, output, report)
            verify.assert_not_called()
        self.assert_old()
        self.assertEqual(self.backups(), [])
        # Parent aliases are canonicalized before the symmetric overlap check.
        parent = self.root / "parent-link"; parent.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError):
            admission.validate_paths(self.source, parent / "source/new", self.report)
        paths = admission.validate_paths(self.source, parent / "new/deep/output", self.report)
        self.assertEqual(paths[1], (self.root / "new/deep/output").resolve())

    def test_publish_failures_restore_both_including_failure_after_report_replace(self):
        self.old()
        replace = os.replace
        for phase in ("output-backup", "report-backup", "output", "report", "after-report"):
            def fail(source, destination):
                match = ((phase == "output-backup" and source == self.output) or
                         (phase == "report-backup" and source == self.report) or
                         (phase == "output" and source == self.stage) or
                         (phase in ("report", "after-report") and source == self.pending))
                if match:
                    if phase == "after-report":
                        replace(source, destination)
                    raise OSError("injected " + phase)
                return replace(source, destination)
            # Previous failed publication may have consumed the disposable stage.
            self.stage.mkdir(exist_ok=True); (self.stage / "new").write_bytes(b"new device")
            self.pending.write_bytes(b"new report")
            with self.subTest(phase=phase), mock.patch.object(admission.os, "replace", fail):
                with self.assertRaises(OSError):
                    self.publish()
            self.assert_old()
            self.assertEqual(self.backups(), [])

    def test_rollback_restore_failure_preserves_exact_backup_and_restores_other_resource(self):
        self.old()
        replace = os.replace
        def fail(source, destination):
            if source == self.pending:
                replace(source, destination)
                raise OSError("after report publication")
            if source.name.startswith(".output.backup-"):
                raise OSError("cannot restore output")
            return replace(source, destination)
        with mock.patch.object(admission.os, "replace", fail):
            with self.assertRaises(OSError) as caught:
                self.publish()
        self.assertEqual(self.report.read_bytes(), b"old report")
        backups = self.backups()
        self.assertEqual(len(backups), 1)
        self.assertEqual(admission.snapshot(backups[0]), {"old": b"old device"})
        self.assertIn(str(backups[0]), str(caught.exception))
        self.assertFalse(self.output.exists())

    def test_failed_publication_preserves_absence_matrix(self):
        replace = os.replace
        for old_output in (False, True):
            for old_report in (False, True):
                with self.subTest(output=old_output, report=old_report), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    stage, pending, output, report = [root / n for n in ("stage", "pending", "output", "report")]
                    stage.mkdir(); (stage / "new").write_bytes(b"new")
                    pending.write_bytes(b"new proof")
                    if old_output:
                        output.mkdir(); (output / "old").write_bytes(b"old")
                    if old_report:
                        report.write_bytes(b"old proof")
                    def fail(source, destination):
                        replace(source, destination)
                        if source == pending:
                            raise OSError("after publish")
                    with mock.patch.object(admission.os, "replace", fail), self.assertRaises(OSError):
                        admission.publish(stage, output, pending, report)
                    self.assertEqual(output.exists(), old_output)
                    self.assertEqual(report.exists(), old_report)
                    if old_output:
                        self.assertEqual(admission.snapshot(output), {"old": b"old"})
                    if old_report:
                        self.assertEqual(report.read_bytes(), b"old proof")
                    self.assertEqual(list(root.glob(".*.backup-*")), [])

    def test_report_restore_failure_keeps_old_report_backup(self):
        self.old()
        replace = os.replace
        def fail(source, destination):
            if source == self.pending or source.name.startswith(".report.json.backup-"):
                raise OSError("report unavailable")
            return replace(source, destination)
        with mock.patch.object(admission.os, "replace", fail), self.assertRaises(OSError) as caught:
            self.publish()
        self.assertEqual(admission.snapshot(self.output), {"old": b"old device"})
        self.assertFalse(self.report.exists())
        backups = self.backups()
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), b"old report")
        self.assertIn(str(backups[0]), str(caught.exception))

    def test_readback_failure_after_both_publications_rolls_back_both(self):
        self.old()
        snapshot = admission.snapshot
        def fail(path):
            if path == self.report:
                raise OSError("report readback failed")
            return snapshot(path)
        with mock.patch.object(admission, "snapshot", fail), self.assertRaises(OSError):
            self.publish()
        self.assert_old()
        self.assertEqual(self.backups(), [])

    def test_backup_cleanup_failure_is_success_and_retains_old_copies(self):
        self.old()
        remove = admission.remove
        def fail(path):
            if ".backup-" in path.name:
                raise OSError("cleanup denied")
            return remove(path)
        err = io.StringIO()
        with mock.patch.object(admission, "remove", fail), contextlib.redirect_stderr(err):
            self.publish()  # Must return success, even under warnings-as-errors.
        self.assertEqual(admission.snapshot(self.output), {"new": b"new device"})
        self.assertEqual(self.report.read_bytes(), b"new report")
        backups = self.backups()
        self.assertEqual(len(backups), 2)
        for backup in backups:
            self.assertIn(str(backup), err.getvalue())
            self.assertEqual(admission.snapshot(backup),
                             {"old": b"old device"} if backup.is_dir() else b"old report")

    def test_staging_report_failures_never_publish_device(self):
        self.old()
        nested = self.root / "reports"
        nested.mkdir()
        self.report.rename(nested / "proof.json")
        self.report = nested / "proof.json"
        # Reuse real immutable payloads; mock expensive policy/reader work only.
        files = {p.name: p.read_bytes() for p in FIXTURE.iterdir() if p.is_file()}
        mkdir = Path.mkdir
        for phase in ("mkdir", "temp", "write"):
            def fail_mkdir(path, *args, **kwargs):
                if phase == "mkdir" and path == self.report.parent.resolve():
                    raise OSError("report mkdir failed")
                return mkdir(path, *args, **kwargs)
            real_temp = tempfile.NamedTemporaryFile
            def fail_temp(*args, **kwargs):
                if phase == "temp":
                    raise OSError("report temp failed")
                return real_temp(*args, **kwargs)
            real_json = admission.canonical_json
            def fail_json(value):
                if phase == "write" and value.get("format") == "stroke3500-product-admission-v1":
                    raise OSError("report serialization failed")
                return real_json(value)
            with self.subTest(phase=phase), \
                 mock.patch.object(admission, "verify_generated", return_value={"files": files}), \
                 mock.patch.object(admission, "check_device_profile", return_value=2945), \
                 mock.patch.object(admission, "verify_package", return_value={"device_compatible": False, "files": {n: {} for n in admission.DEVICE_FILES}}), \
                 mock.patch.object(admission, "run_device_reader", return_value="mock proof"), \
                 mock.patch.object(Path, "mkdir", fail_mkdir), \
                 mock.patch.object(admission.tempfile, "NamedTemporaryFile", fail_temp), \
                 mock.patch.object(admission, "canonical_json", fail_json):
                with self.assertRaises(OSError):
                    admit(FIXTURE, self.output, self.report)
            self.assert_old()
            self.assertEqual(self.backups(), [])
            self.assertEqual(list(self.report.parent.glob(".*.pending-*")), [])

    def test_compiler_paths_and_identity(self):
        for compiler in COMPILERS.values():
            identity = admission.compiler_identity(compiler)
            self.assertEqual(identity["path"], str(compiler))
            self.assertTrue(identity["version"])
            self.assertEqual(identity["sha256"], digest(compiler.read_bytes()))
        link = self.root / "compiler-link"; link.symlink_to(COMPILERS["cc"])
        for path in (Path("cc"), link, self.root):
            with self.assertRaises(ValueError):
                admission.compiler_identity(path)


if __name__ == "__main__":
    unittest.main()
