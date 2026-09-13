import hashlib
import inspect
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
sys_path_inserted = False
import sys

sys.path.insert(0, str(SCRIPTS))

from stroke_order.pinyin import (  # noqa: E402
    PinyinError,
    StrokePinyinBlob,
    crc32,
    normalize_pinyin,
    pack_spy1,
    parse_khanyu_pinyin,
    parse_kmandarin,
    u32_add,
    u32_mul,
    union_readings,
    validate_spy1,
)
from stroke_order.constants import MAX_FILE_BYTES as SOB1_MAX_FILE_BYTES  # noqa: E402
from stroke_order.pinyin_constants import (  # noqa: E402
    CHAR_INDEX_ENTRY_SIZE,
    FORMAT_VERSION,
    GROUP_INDEX_ENTRY_SIZE,
    HEADER_SIZE,
    MAX_FILE_BYTES,
    MAX_GROUP_MEMBERS,
    MAX_READINGS_PER_CHARACTER,
)
from stroke_order.select import (  # noqa: E402
    OFFICIAL_RANGE_END,
    TRANSCRIPTION_COMMIT,
    TRANSCRIPTION_JSON_PATH,
    TRANSCRIPTION_JSON_SHA256,
    TRANSCRIPTION_REPO,
    SelectionError,
    load_selection_csv,
    official_source_lock,
    _verify_transcription_checkout,
    select_official_500,
    verify_selection_rows,
    write_charset,
    write_selection_csv,
)
from stroke_order.unpack import validate_blob  # noqa: E402
from package_stroke_order_prototype import (  # noqa: E402
    CHECKSUM_NAME,
    COPY_MAP,
    PackageError,
    package_prototype_corpus,
    verify_checksum_directory,
)


PROTOTYPE = ROOT / "scripts/tests/fixtures/stroke_order/prototype_500"
GOLDEN_HEX = ROOT / "scripts/tests/fixtures/stroke_order/handwritten_spy1_v1.hex"
SOB1 = PROTOTYPE / "stroke_order.sob1"
SPY1 = PROTOTYPE / "stroke_pinyin.spy1"


def _host_compiler():
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise AssertionError("no host C++ compiler found")
    return compiler


def _run(command, *, cwd=None, env=None):
    result = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {' '.join(map(str, command))}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout.strip()


def _write_transcription_checkout(directory: Path, *, origin=TRANSCRIPTION_REPO):
    repo = directory / "transcription"
    repo.mkdir()
    payload = {"tier1": ["一", "乙", "二"]}
    table = repo / TRANSCRIPTION_JSON_PATH
    table.write_text(json.dumps(payload, ensure_ascii=False) + "\n", encoding="utf-8")
    _run(["git", "init", "-q", str(repo)])
    _run(["git", "-C", str(repo), "config", "user.name", "Stroke Test"])
    _run(["git", "-C", str(repo), "config", "user.email", "stroke@example.invalid"])
    _run(["git", "-C", str(repo), "remote", "add", "origin", origin])
    _run(["git", "-C", str(repo), "add", TRANSCRIPTION_JSON_PATH])
    env = os.environ.copy()
    env.update(
        {
            "GIT_AUTHOR_DATE": "2000-01-01T00:00:00+0000",
            "GIT_COMMITTER_DATE": "2000-01-01T00:00:00+0000",
        }
    )
    _run(["git", "-C", str(repo), "commit", "-q", "-m", "transcription fixture"], env=env)
    commit = _run(["git", "-C", str(repo), "rev-parse", "HEAD"])
    digest = hashlib.sha256(table.read_bytes()).hexdigest()
    return repo, table, commit, digest


def _refresh_spy_header_crc(blob: bytearray):
    struct.pack_into("<I", blob, 24, crc32(bytes(blob[:24])))


def _refresh_spy_body_crc(blob: bytearray):
    struct.pack_into("<I", blob, 28, crc32(bytes(blob[HEADER_SIZE:])))


def _compile_with_fallback(common):
    attempts = [common]
    xcrun = shutil.which("xcrun")
    if xcrun:
        sdk = subprocess.run(
            [xcrun, "--show-sdk-path"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if sdk.returncode == 0:
            attempts.append(
                common[:1] + ["-isystem", f"{sdk.stdout.strip()}/usr/include/c++/v1"] + common[1:]
            )
    failures = []
    for command in attempts:
        run = subprocess.run(command, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if run.returncode == 0:
            return
        failures.append(run.stderr)
    raise AssertionError("compile failed:\n" + "\n".join(failures))


class StrokeOrderPinyinTest(unittest.TestCase):
    def test_selection_is_500_unique_consecutive_official_numbers(self):
        rows = load_selection_csv(PROTOTYPE / "selection-500.csv")
        verify_selection_rows(rows)
        self.assertEqual(len(rows), OFFICIAL_RANGE_END)
        self.assertEqual(rows[0]["character"], "一")
        self.assertEqual(rows[0]["official_number"], "0001")
        self.assertEqual(rows[-1]["official_number"], "0500")
        charset = [
            line.strip()
            for line in (PROTOTYPE / "charset-500.txt").read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")
        ]
        self.assertEqual(charset, [row["character"] for row in rows])
        src = json.loads((PROTOTYPE / "stroke_order.src.json").read_text(encoding="utf-8"))
        self.assertFalse(src["dual_person_official_pdf_verified"])
        self.assertTrue(src["not_official_certification"])
        self.assertIn("not an official digital annex", src["transcription_aid"]["role"])

    def test_selection_csv_rejects_mismatched_text_codepoint(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "selection.csv"
            lines = (PROTOTYPE / "selection-500.csv").read_text(encoding="utf-8").splitlines()
            fields = lines[1].split(",")
            fields[3] = "U+4E01"
            lines[1] = ",".join(fields)
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            with self.assertRaises(SelectionError):
                load_selection_csv(path)

    def test_transcription_provenance_rejects_wrong_origin_head_dirty_untracked_and_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            repo, table, commit, digest = _write_transcription_checkout(tmp)
            kwargs = {
                "expected_commit": commit,
                "expected_repo": TRANSCRIPTION_REPO,
                "expected_json_sha256": digest,
            }
            verified = _verify_transcription_checkout(str(table), **kwargs)
            self.assertEqual(verified["commit"], commit)
            self.assertTrue(verified["checkout_clean"])

            with self.assertRaises(SelectionError):
                _verify_transcription_checkout(
                    str(table),
                    expected_commit=("0" if commit[0] != "0" else "1") + commit[1:],
                    expected_repo=TRANSCRIPTION_REPO,
                    expected_json_sha256=digest,
                )
            _run(["git", "-C", str(repo), "remote", "set-url", "origin", "https://example.invalid/wrong"])
            with self.assertRaises(SelectionError):
                _verify_transcription_checkout(str(table), **kwargs)
            _run(["git", "-C", str(repo), "remote", "set-url", "origin", TRANSCRIPTION_REPO])

            (repo / "unrelated.tmp").write_text("dirty\n", encoding="utf-8")
            with self.assertRaises(SelectionError):
                _verify_transcription_checkout(str(table), **kwargs)
            (repo / "unrelated.tmp").unlink()

            original = table.read_bytes()
            table.write_bytes(original + b" ")
            with self.assertRaises(SelectionError):
                _verify_transcription_checkout(str(table), **kwargs)
            table.write_bytes(original)

        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            repo = tmp / "untracked"
            repo.mkdir()
            table = repo / TRANSCRIPTION_JSON_PATH
            table.write_text('{"tier1":["一"]}\n', encoding="utf-8")
            (repo / "README").write_text("tracked\n", encoding="utf-8")
            _run(["git", "init", "-q", str(repo)])
            _run(["git", "-C", str(repo), "config", "user.name", "Stroke Test"])
            _run(["git", "-C", str(repo), "config", "user.email", "stroke@example.invalid"])
            _run(["git", "-C", str(repo), "remote", "add", "origin", TRANSCRIPTION_REPO])
            _run(["git", "-C", str(repo), "add", "README"])
            _run(["git", "-C", str(repo), "commit", "-q", "-m", "no JSON"])
            commit = _run(["git", "-C", str(repo), "rev-parse", "HEAD"])
            with self.assertRaises(SelectionError):
                _verify_transcription_checkout(
                    str(table),
                    expected_commit=commit,
                    expected_repo=TRANSCRIPTION_REPO,
                    expected_json_sha256=hashlib.sha256(table.read_bytes()).hexdigest(),
                )

    def test_production_selection_api_is_pinned_and_rebuilds_committed_outputs(self):
        self.assertNotIn("transcription_commit", inspect.signature(select_official_500).parameters)
        table = os.environ.get("STROKE_TRANSCRIPTION_JSON")
        if not table:
            self.skipTest("set STROKE_TRANSCRIPTION_JSON to the clean pinned checkout JSON")
        verified = _verify_transcription_checkout(
            table,
            expected_commit=TRANSCRIPTION_COMMIT,
            expected_repo=TRANSCRIPTION_REPO,
            expected_json_sha256=TRANSCRIPTION_JSON_SHA256,
        )
        self.assertEqual(verified["json_path"], TRANSCRIPTION_JSON_PATH)
        rows = select_official_500(table)
        with tempfile.TemporaryDirectory() as directory:
            selection = Path(directory) / "selection-500.csv"
            charset = Path(directory) / "charset-500.txt"
            write_selection_csv(rows, selection)
            write_charset(rows, charset)
            self.assertEqual(selection.read_bytes(), (PROTOTYPE / selection.name).read_bytes())
            self.assertEqual(charset.read_bytes(), (PROTOTYPE / charset.name).read_bytes())
        lock = official_source_lock(Path(table))
        self.assertTrue(lock["transcription_aid"]["checkout_clean"])
        self.assertEqual(lock["transcription_aid"]["commit"], TRANSCRIPTION_COMMIT)
        self.assertEqual(lock["transcription_aid"]["json_sha256"], TRANSCRIPTION_JSON_SHA256)

    def test_hwd_coverage_report_is_500_of_500(self):
        coverage = json.loads((PROTOTYPE / "stroke_order.cov.json").read_text(encoding="utf-8"))
        self.assertTrue(coverage["complete"])
        self.assertEqual(coverage["ok_count"], 500)
        self.assertEqual(coverage["error_count"], 0)
        self.assertEqual(coverage["errors"], [])
        self.assertTrue(coverage["not_a_release_library"])

    def test_python_full_sob1_and_spy1_load(self):
        sob1 = SOB1.read_bytes()
        spy1 = SPY1.read_bytes()
        parsed = validate_blob(sob1, load_all=True)
        self.assertEqual(parsed["char_count"], 500)
        self.assertEqual(len(sob1), 1_046_788)
        self.assertEqual(1024 * 1024 - len(sob1), 1788)
        self.assertEqual(SOB1_MAX_FILE_BYTES, 1024 * 1024)
        self.assertLessEqual(len(sob1), SOB1_MAX_FILE_BYTES)
        store_header = (ROOT / "main/stroke_order/stroke_order_store.h").read_text(encoding="utf-8")
        self.assertIn("kMaxFileBytes = 1048576", store_header)
        spy = validate_spy1(spy1, load_all=True)
        self.assertEqual(spy["char_count"], 500)
        self.assertEqual(len(spy1), 18152)
        self.assertLessEqual(len(spy1), MAX_FILE_BYTES)
        blob = StrokePinyinBlob()
        self.assertTrue(blob.bind(spy1))
        self.assertEqual(blob.append_top_ranked(6), [ord("一"), ord("乙"), ord("二"), ord("十"), ord("丁"), ord("厂")])
        yi = blob.append_homophones(ord("一"), 8)
        self.assertTrue(all(cp != ord("一") for cp in yi))
        self.assertIn(ord("伊"), yi)

    def test_sha256sums_and_reproducible_sizes(self):
        sums = {}
        for line in (PROTOTYPE / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
            digest, name = line.split("  ", 1)
            actual = hashlib.sha256((PROTOTYPE / name).read_bytes()).hexdigest()
            self.assertEqual(actual, digest, name)
            sums[name] = digest
        self.assertEqual(sums["stroke_order.sob1"], hashlib.sha256(SOB1.read_bytes()).hexdigest())
        self.assertEqual(sums["stroke_pinyin.spy1"], hashlib.sha256(SPY1.read_bytes()).hexdigest())
        order_manifest = json.loads((PROTOTYPE / "stroke_order.manifest.json").read_text(encoding="utf-8"))
        pinyin_manifest = json.loads((PROTOTYPE / "stroke_pinyin.manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(order_manifest["sha256_bin"], sums["stroke_order.sob1"])
        self.assertEqual(pinyin_manifest["sha256_bin"], sums["stroke_pinyin.spy1"])
        self.assertTrue(order_manifest["not_a_release_library"])
        self.assertTrue(pinyin_manifest["prototype"])
        notice = (PROTOTYPE / "NOTICE.md").read_text(encoding="utf-8")
        self.assertIn("not a release", notice.lower())
        self.assertIn("not official certification", notice.lower())
        self.assertIn("af85c706a53d3b3bbad818bcce7415ac9a2284ea14f79fe7f54ce1248a7bdac9", notice)
        self.assertIn("UNICODE-LICENSE.txt", notice)
        self.assertIn("no explicit license", notice.lower())
        self.assertIn("Arphic Public License", notice)
        self.assertIn("2026-09-12", notice)
        self.assertIn("stroke-order-converter/1", notice)
        self.assertIn("SOB1 v1 + SPY1 v1", notice)
        self.assertIn("Legal review", notice)
        self.assertIn("not a legal conclusion", notice)
        docs = (ROOT / "docs/stroke-order-data.md").read_text(encoding="utf-8")
        self.assertIn("af85c706a53d3b3bbad818bcce7415ac9a2284ea14f79fe7f54ce1248a7bdac9", docs)
        self.assertIn("manually reviewed", docs)
        self.assertIn("UNICODE-LICENSE.txt", docs)
        self.assertIn("ingested", docs)
        self.assertIn("LGPL", docs)
        self.assertIn("2026-09-12", docs)
        self.assertIn("legal review", docs.lower())
        generate = (SCRIPTS / "generate_stroke_order_500.py").read_text(encoding="utf-8")
        self.assertIn("if len(blob) > MAX_FILE_BYTES", generate)

    def test_pinyin_normalizer(self):
        self.assertEqual(normalize_pinyin("yī"), "yi1")
        self.assertEqual(normalize_pinyin("rén"), "ren2")
        self.assertEqual(normalize_pinyin("nǚ"), "nv3")
        self.assertEqual(normalize_pinyin("de"), "de5")
        self.assertEqual(normalize_pinyin("yi1"), "yi1")
        self.assertEqual(parse_kmandarin("wàn mò"), ["wan4", "mo4"])
        self.assertEqual(parse_khanyu_pinyin("10001.010:yī"), ["yi1"])
        self.assertEqual(union_readings("xíng", "20811.060:háng,xìng,xíng"), ["xing2", "hang2", "xing4"])
        with self.assertRaises(PinyinError):
            normalize_pinyin("")
        with self.assertRaises(PinyinError):
            normalize_pinyin("yi0")
        with self.assertRaises(PinyinError):
            normalize_pinyin("中")

    def test_homophone_polyphone_primary_rank_dedup_max6(self):
        blob = pack_spy1(
            [
                {"codepoint": 0x884C, "rank": 4, "readings": ["xing2", "hang2"]},
                {"codepoint": 0x5211, "rank": 2, "readings": ["xing2"]},
                {"codepoint": 0x822A, "rank": 5, "readings": ["hang2"]},
                {"codepoint": 0x5F62, "rank": 3, "readings": ["xing2"]},
                {"codepoint": 0x4E00, "rank": 1, "readings": ["yi1"]},
                {"codepoint": 0x4F0A, "rank": 6, "readings": ["yi1"]},
                {"codepoint": 0x8863, "rank": 7, "readings": ["yi1"]},
                {"codepoint": 0x533B, "rank": 8, "readings": ["yi1"]},
                {"codepoint": 0x4F9D, "rank": 9, "readings": ["yi1"]},
                {"codepoint": 0x6307, "rank": 10, "readings": ["yi1"]},
                {"codepoint": 0x6C14, "rank": 11, "readings": ["yi1"]},
            ]
        )
        index = StrokePinyinBlob()
        self.assertTrue(index.bind(blob))
        hang_union = index.append_homophones(0x884C, 8)
        self.assertEqual(hang_union[0], 0x5211)
        self.assertIn(0x822A, hang_union)
        self.assertNotIn(0x884C, hang_union)
        yi = index.append_homophones(0x4E00, 6)
        self.assertEqual(len(yi), 6)
        self.assertEqual(yi[0], 0x4F0A)
        self.assertNotIn(0x4E00, yi)
        self.assertEqual(index.append_top_ranked(3), [0x4E00, 0x5211, 0x5F62])

    def test_handwritten_golden_crc_and_table_driven_structural_corruption(self):
        hex_text = GOLDEN_HEX.read_text(encoding="utf-8").strip()
        golden = bytes.fromhex(hex_text)
        self.assertEqual(crc32(golden[:24]), 0x65605A07)
        self.assertEqual(crc32(golden[HEADER_SIZE:]), 0xC92B5D14)
        parsed = validate_spy1(golden, load_all=True)
        self.assertEqual(parsed["char_count"], 3)

        def h32(offset, value):
            return lambda blob: struct.pack_into("<I", blob, offset, value)

        def h16(offset, value):
            return lambda blob: struct.pack_into("<H", blob, offset, value)

        cases = [
            ("char_count_zero", h32(8, 0), True),
            ("char_count_large", h32(8, 0xFFFFFFFF), True),
            ("group_count_zero", h32(12, 0), True),
            ("group_index_offset", h32(20, 0xFFFFFFF8), True),
            ("header_reserved", h16(6, 1), True),
            ("char_rank_zero", h16(36, 0), False),
            ("char_rank_duplicate", h16(48, 1), False),
            ("char_reading_count_zero", lambda blob: blob.__setitem__(38, 0), False),
            (
                "char_reading_count_large",
                lambda blob: blob.__setitem__(38, MAX_READINGS_PER_CHARACTER + 1),
                False,
            ),
            ("char_reserved", lambda blob: blob.__setitem__(39, 1), False),
            ("readings_offset_add_overflow", h32(40, 0xFFFFFFFF), False),
            ("readings_payload_overlap", h32(52, 84), False),
            ("group_member_count_zero", h16(68, 0), False),
            ("group_member_count_large", h16(68, MAX_GROUP_MEMBERS + 1), False),
            ("group_reserved", h16(70, 1), False),
            ("members_offset_add_overflow", h32(72, 0xFFFFFFFC), False),
            ("member_payload_overlap", h32(80, 90), False),
            ("member_reserved", h16(96, 1), False),
            ("char_group_membership", h16(84, 0), False),
            ("member_missing_char", h32(90, 0x4E01), False),
            ("member_rank_mismatch", h16(94, 3), False),
        ]
        for name, mutate, header_change in cases:
            with self.subTest(name=name):
                damaged = bytearray(golden)
                mutate(damaged)
                if header_change:
                    _refresh_spy_header_crc(damaged)
                else:
                    _refresh_spy_body_crc(damaged)
                with self.assertRaises(PinyinError):
                    validate_spy1(bytes(damaged), load_all=True)

        reverse_only = bytearray(golden)
        struct.pack_into("<H", reverse_only, 48, 4)  # 人 char rank
        struct.pack_into("<H", reverse_only, 76, 3)  # final group gains one member
        struct.pack_into("<H", reverse_only, 94, 4)  # 人 rank in its real group
        reverse_only.extend(struct.pack("<IHH", ord("人"), 4, 0))
        _refresh_spy_body_crc(reverse_only)
        with self.assertRaises(PinyinError):
            validate_spy1(bytes(reverse_only), load_all=True)

        trailing = bytearray(golden + b"\x00")
        _refresh_spy_body_crc(trailing)
        with self.assertRaises(PinyinError):
            validate_spy1(bytes(trailing), load_all=True)

        with self.assertRaises(PinyinError):
            u32_add(0xFFFFFFFF, 1)
        with self.assertRaises(PinyinError):
            u32_mul(0x40000000, 8)
        self.assertEqual(u32_add(32, 16), 48)
        self.assertEqual(u32_mul(32, 8), 256)

    def test_cpp_constants_and_500_harness(self):
        header = (ROOT / "main/stroke_order/stroke_order_pinyin.h").read_text(encoding="utf-8")
        self.assertIn("kFormatVersion = 1", header)
        self.assertIn("kMaxFileBytes = 65536", header)
        self.assertIn(str(MAX_READINGS_PER_CHARACTER), header)
        self.assertIn(str(MAX_GROUP_MEMBERS), header)
        self.assertEqual(FORMAT_VERSION, 1)
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "stroke_order_pinyin_harness"
            common = [
                _host_compiler(),
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=undefined",
                "-fno-sanitize-recover=all",
                "-DSTROKE_ORDER_TESTING=1",
                "-I",
                str(ROOT / "main"),
                str(ROOT / "scripts/tests/stroke_order_pinyin_harness.cc"),
                str(ROOT / "main/stroke_order/stroke_order_pinyin.cc"),
                str(ROOT / "main/stroke_order/stroke_order_controller.cc"),
                str(ROOT / "main/stroke_order/stroke_order_catalog.cc"),
                str(ROOT / "main/stroke_order/stroke_order_store.cc"),
                "-o",
                str(executable),
            ]
            _compile_with_fallback(common)
            run = subprocess.run(
                [str(executable), str(GOLDEN_HEX), str(SPY1), str(SOB1)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
            )
            self.assertEqual(run.returncode, 0, f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}")
            self.assertIn("stroke_order_pinyin_harness: PASS", run.stdout)

    def test_prototype_packager_is_offline_and_final_sha256sums_is_closed(self):
        source = (SCRIPTS / "package_stroke_order_prototype.py").read_text(encoding="utf-8")
        self.assertNotIn("urllib", source)
        self.assertNotIn("requests", source)
        self.assertNotRegex(source, r"[\"']all\.json[\"']")
        with tempfile.TemporaryDirectory() as directory:
            result = package_prototype_corpus(str(PROTOTYPE), str(Path(directory) / "assets"))
            out = Path(result["output_dir"])
            payload_names = tuple(
                dest for source_name, dest in COPY_MAP.items() if source_name != CHECKSUM_NAME
            )
            self.assertEqual(len(payload_names), 13)
            verify_checksum_directory(out, payload_names)
            names = {path.name for path in out.iterdir() if path.is_file()}
            self.assertEqual(names, set(payload_names) | {CHECKSUM_NAME})
            self.assertEqual(len(names), 14)
            self.assertIn("stroke_order.bin", names)
            self.assertIn("stroke_pinyin.bin", names)
            self.assertNotIn("stroke_order.sob1", names)
            self.assertNotIn("stroke_pinyin.spy1", names)
            self.assertTrue(all(len(name) <= 31 for name in names))
            sums = (out / CHECKSUM_NAME).read_text(encoding="utf-8")
            self.assertIn("  stroke_order.bin\n", sums)
            self.assertIn("  stroke_pinyin.bin\n", sums)
            self.assertNotIn("  SHA256SUMS", sums)

            cases = {}
            missing = Path(directory) / "missing"
            shutil.copytree(out, missing)
            (missing / payload_names[0]).unlink()
            cases["missing"] = missing

            extra = Path(directory) / "extra"
            shutil.copytree(out, extra)
            (extra / "unexpected").write_text("x", encoding="utf-8")
            cases["extra"] = extra

            duplicate = Path(directory) / "duplicate"
            shutil.copytree(out, duplicate)
            first_line = (duplicate / CHECKSUM_NAME).read_text(encoding="utf-8").splitlines()[0]
            with (duplicate / CHECKSUM_NAME).open("a", encoding="utf-8") as handle:
                handle.write(first_line + "\n")
            cases["duplicate"] = duplicate

            recursive = Path(directory) / "recursive"
            shutil.copytree(out, recursive)
            with (recursive / CHECKSUM_NAME).open("a", encoding="utf-8") as handle:
                handle.write("0" * 64 + "  SHA256SUMS\n")
            cases["self"] = recursive

            for name, invalid in cases.items():
                with self.subTest(name=name), self.assertRaises(PackageError):
                    verify_checksum_directory(invalid, payload_names)

    def test_cmake_uses_reviewed_prototype_pack(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("package_stroke_order_2000.py", cmake)
        self.assertIn("stroke_pinyin.bin", cmake)
        self.assertIn("stroke_cat.bin", cmake)
        self.assertIn("UNICODE-LICENSE.txt", cmake)
        self.assertIn("prototype_2000", cmake)
        self.assertIn("selection-2000.csv", cmake)
        self.assertIn("charset-2000.txt", cmake)
        self.assertIn("SHA256SUMS", cmake)
        self.assertNotIn("package_stroke_order_smoke.py", cmake)


if __name__ == "__main__":
    unittest.main()
