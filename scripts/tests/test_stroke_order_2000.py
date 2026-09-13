import csv
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

from build_default_assets import build_assets_integrated  # noqa: E402
from package_stroke_order_2000 import (  # noqa: E402
    COPY_MAP,
    SOURCE_PAYLOADS,
    package_corpus,
    verify_checksum_directory,
    verify_source,
)
from stroke_order.catalog import (  # noqa: E402
    ENTRY_SIZE,
    HEADER_SIZE,
    SHARD_SIZE,
    CatalogError,
    crc32,
    pack_catalog,
    validate_catalog,
)
from stroke_order.pinyin import (  # noqa: E402
    PinyinError,
    StrokePinyinBlob,
    pack_spy1,
    union_readings,
    validate_spy1,
)
from stroke_order.pinyin_constants import (  # noqa: E402
    MAX_CHARACTERS as SPY1_MAX_CHARACTERS,
    MAX_FILE_BYTES as SPY1_MAX_FILE_BYTES,
    MAX_GROUP_MEMBERS,
    MAX_GROUPS as SPY1_MAX_GROUPS,
)
from stroke_order.select import OFFICIAL_RANGE_END_2000, load_selection_csv  # noqa: E402
from stroke_order.unpack import validate_blob  # noqa: E402

PROTOTYPE = ROOT / "scripts/tests/fixtures/stroke_order/prototype_2000"
EXPECTED_SIZES = [443020, 603800, 680236, 723504, 771996, 816964, 849284, 911688]
EXPECTED_HASHES = [
    "a9108c23656aa91613c8fb8c7fc34e9ad59858b843af09d9b48cfb9460bbea6b",
    "2fc7d1c5f948bf2ea867fa9a31398c62b3a561fe94a31911a3fe7efa594a9bb8",
    "a915d0376995a074daa9f9b17d394019936121dcd5bba1e67fcd17bd74998a98",
    "98a6bc6c21e8a736878092460d9280c242f3d064b0b34c379ae07d21006cd843",
    "c7a9088e4ade040b5c2b91941f08aeb9e2862a81f9243e0c9418a47a2cab9a90",
    "8855644a164204a83aa5ff64d5eab9a102eaef012ec660b301ccc25088b0b1dd",
    "900cb7ff2c288f64b937614969f3f0cff5e6b6cd5a0e421222264e8ee3d20778",
    "094287ea791635449092a6f366308b84f6571a664b74f155e882312a57358ba5",
]


def _refresh_catalog_header(blob: bytearray):
    struct.pack_into("<I", blob, 24, crc32(bytes(blob[:24])))


def _refresh_catalog_body(blob: bytearray):
    struct.pack_into("<I", blob, 28, crc32(bytes(blob[HEADER_SIZE:])))


def _pinyin_with_swapped_ranks(blob: bytes) -> bytes:
    damaged = bytearray(blob)
    char_count = struct.unpack_from("<I", damaged, 8)[0]
    group_count = struct.unpack_from("<I", damaged, 12)[0]
    char_offset = struct.unpack_from("<I", damaged, 16)[0]
    group_offset = struct.unpack_from("<I", damaged, 20)[0]
    if char_count < 2:
        raise AssertionError("SPY1 mismatch fixture needs two characters")
    cp_a, rank_a = struct.unpack_from("<IH", damaged, char_offset)
    cp_b, rank_b = struct.unpack_from("<IH", damaged, char_offset + 12)
    struct.pack_into("<H", damaged, char_offset + 4, rank_b)
    struct.pack_into("<H", damaged, char_offset + 12 + 4, rank_a)
    for group in range(group_count):
        count, _reserved, members_offset = struct.unpack_from(
            "<HHI", damaged, group_offset + group * 8
        )
        members = [
            list(struct.unpack_from("<IHH", damaged, members_offset + member * 8))
            for member in range(count)
        ]
        for member in members:
            if member[0] == cp_a:
                member[1] = rank_b
            elif member[0] == cp_b:
                member[1] = rank_a
        members.sort(key=lambda item: (item[1], item[0]))
        for member, values in enumerate(members):
            struct.pack_into("<IHH", damaged, members_offset + member * 8, *values)
    struct.pack_into("<I", damaged, 28, crc32(bytes(damaged[32:])))
    validate_spy1(bytes(damaged), load_all=True)
    return bytes(damaged)


def _compiler():
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise AssertionError("no host C++ compiler")
    return compiler


def _unpack_generated_assets(blob: bytes):
    if len(blob) < 12:
        raise AssertionError("generated assets header is truncated")
    file_count, checksum, payload_length = struct.unpack_from("<III", blob, 0)
    if payload_length != len(blob) - 12:
        raise AssertionError("generated assets payload length mismatch")
    if sum(blob[12:]) & 0xFFFF != checksum:
        raise AssertionError("generated assets checksum mismatch")
    table_size = file_count * 44
    if 12 + table_size > len(blob):
        raise AssertionError("generated assets table is truncated")
    payload_base = 12 + table_size
    files = {}
    occupied = []
    for index in range(file_count):
        offset = 12 + index * 44
        raw_name, size, payload_offset, width, height = struct.unpack_from(
            "<32sIIHH", blob, offset
        )
        if b"\0" not in raw_name:
            raise AssertionError("generated asset name is not NUL terminated")
        name_end = raw_name.index(0)
        if any(raw_name[name_end + 1 :]):
            raise AssertionError("generated asset name padding is not zero")
        name = raw_name[:name_end].decode("utf-8")
        start = payload_base + payload_offset
        end = start + 2 + size
        if end > len(blob) or blob[start : start + 2] != b"ZZ":
            raise AssertionError(f"generated asset {name} range/magic invalid")
        occupied.append((start, end))
        if name in files:
            raise AssertionError("duplicate generated asset name")
        files[name] = {
            "data": blob[start + 2 : end],
            "height": height,
            "size": size,
            "width": width,
        }
    if sorted(occupied) != occupied:
        raise AssertionError("generated asset payload order is not canonical")
    for previous, current in zip(occupied, occupied[1:]):
        if previous[1] != current[0]:
            raise AssertionError("generated asset payload has gaps or overlaps")
    if occupied and occupied[-1][1] != len(blob):
        raise AssertionError("generated assets has trailing bytes")
    return files


class StrokeOrder2000Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog_bytes = (PROTOTYPE / "stroke_cat.bin").read_bytes()
        cls.catalog = validate_catalog(cls.catalog_bytes)
        cls.rows = load_selection_csv(PROTOTYPE / "selection-2000.csv", 2000)

    def test_selection_is_continuous_unique_2000_and_charset_matches(self):
        self.assertEqual(OFFICIAL_RANGE_END_2000, 2000)
        self.assertEqual(len(self.rows), 2000)
        self.assertEqual([row["rank"] for row in self.rows], list(range(1, 2001)))
        self.assertEqual(self.rows[0]["official_number"], "0001")
        self.assertEqual(self.rows[-1]["official_number"], "2000")
        self.assertEqual(len({row["character"] for row in self.rows}), 2000)
        charset = [
            line
            for line in (PROTOTYPE / "charset-2000.txt").read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]
        self.assertEqual(charset, [row["character"] for row in self.rows])

    def test_source_lock_coverage_notice_and_closed_hashes(self):
        info = verify_source(PROTOTYPE)
        self.assertEqual(info["character_count"], 2000)
        coverage = json.loads((PROTOTYPE / "stroke_order.cov.json").read_text(encoding="utf-8"))
        self.assertTrue(coverage["complete"])
        self.assertEqual((coverage["ok_count"], coverage["error_count"]), (2000, 0))
        self.assertEqual(len(coverage["records"]), 2000)
        self.assertTrue(all(record["status"] == "ok" for record in coverage["records"]))
        source = json.loads((PROTOTYPE / "stroke_order.src.json").read_text(encoding="utf-8"))
        self.assertEqual(source["official_table"]["range"], "一级字表 0001-2000")
        self.assertEqual(
            source["official_table"]["sha256"],
            "af85c706a53d3b3bbad818bcce7415ac9a2284ea14f79fe7f54ce1248a7bdac9",
        )
        self.assertEqual(
            source["hanzi_writer_data"]["commit"],
            "68d10a4b21150cae5e1ebbd223eed289cf32d90c",
        )
        notice = (PROTOTYPE / "NOTICE.md").read_text(encoding="utf-8")
        for marker in ("prototype", "not official", "not commercially reviewed", "Arphic"):
            self.assertIn(marker.lower(), notice.lower())
        verify_checksum_directory(PROTOTYPE, SOURCE_PAYLOADS)

    def test_catalog_and_all_shards_full_load_exact_size_hash_coverage(self):
        self.assertEqual(self.catalog["character_count"], 2000)
        self.assertEqual(self.catalog["shard_count"], 8)
        self.assertEqual(self.catalog["total_shard_bytes"], 5_800_492)
        row_by_cp = {int(row["codepoint_int"]): row for row in self.rows}
        catalog_by_cp = {entry.codepoint: entry for entry in self.catalog["entries"]}
        self.assertEqual(set(catalog_by_cp), set(row_by_cp))
        all_cps = set()
        max_record = (0, 0, 0)
        max_strokes = (0, 0)
        for index, expected_size in enumerate(EXPECTED_SIZES):
            path = PROTOTYPE / f"so{index:02d}.sob1"
            blob = path.read_bytes()
            descriptor = self.catalog["shards"][index]
            self.assertEqual(descriptor.name, f"so{index:02d}.bin")
            self.assertEqual((descriptor.first_rank, descriptor.last_rank),
                             (index * 250 + 1, (index + 1) * 250))
            self.assertEqual(descriptor.character_count, 250)
            self.assertEqual(descriptor.size, expected_size)
            self.assertEqual(descriptor.crc32, crc32(blob))
            self.assertEqual(len(blob), expected_size)
            self.assertEqual(hashlib.sha256(blob).hexdigest(), EXPECTED_HASHES[index])
            parsed = validate_blob(blob, load_all=True)
            self.assertEqual(parsed["char_count"], 250)
            records_by_cp = {item[0]: item[2] for item in parsed["entries"]}
            for local, character in enumerate(parsed["characters"]):
                cp = int(character["codepoint"])
                self.assertNotIn(cp, all_cps)
                all_cps.add(cp)
                entry = catalog_by_cp[cp]
                row = row_by_cp[cp]
                self.assertEqual(entry.rank, int(row["rank"]))
                self.assertEqual(entry.shard, (int(row["rank"]) - 1) // 250)
                self.assertEqual((entry.shard, entry.local_index), (index, local))
                max_record = max(max_record, (records_by_cp[cp], cp, index))
                max_strokes = max(max_strokes, (len(character["strokes"]), cp))
        self.assertEqual(all_cps, set(row_by_cp))
        self.assertEqual(len(all_cps), 2000)
        self.assertGreater(max_record[0], 0)
        # The official table is stroke-count ordered; rank 0001-2000 tops out at 10.
        self.assertEqual(max_strokes[0], 10)

    def test_catalog_corruption_duplicate_offset_and_wrong_shard_rejected(self):
        corrupt = bytearray(self.catalog_bytes)
        corrupt[-1] ^= 1
        with self.assertRaises(CatalogError):
            validate_catalog(bytes(corrupt))

        duplicate = bytearray(self.catalog_bytes)
        first_cp = struct.unpack_from("<I", duplicate, HEADER_SIZE)[0]
        struct.pack_into("<I", duplicate, HEADER_SIZE + ENTRY_SIZE, first_cp)
        _refresh_catalog_body(duplicate)
        with self.assertRaises(CatalogError):
            validate_catalog(bytes(duplicate))

        wrong_offset = bytearray(self.catalog_bytes)
        struct.pack_into("<I", wrong_offset, 16, HEADER_SIZE)
        _refresh_catalog_header(wrong_offset)
        with self.assertRaises(CatalogError):
            validate_catalog(bytes(wrong_offset))

        wrong_shard = bytearray(self.catalog_bytes)
        # Rank 1 belongs to shard 0; force its codepoint-sorted entry to shard 1.
        rank_one = next(i for i, item in enumerate(self.catalog["entries"]) if item.rank == 1)
        wrong_shard[HEADER_SIZE + rank_one * ENTRY_SIZE + 6] = 1
        _refresh_catalog_body(wrong_shard)
        with self.assertRaises(CatalogError):
            validate_catalog(bytes(wrong_shard))

    def test_spy1_2000_measured_limits_no_truncation_and_cross_shard_group(self):
        spy_bytes = (PROTOTYPE / "stroke_pinyin.spy1").read_bytes()
        parsed = validate_spy1(spy_bytes, load_all=True)
        self.assertEqual(len(spy_bytes), 63_618)
        self.assertEqual(SPY1_MAX_FILE_BYTES - len(spy_bytes), 1_918)
        self.assertGreaterEqual(SPY1_MAX_CHARACTERS, 2_048)
        self.assertGreaterEqual(SPY1_MAX_GROUPS, parsed["group_count"])
        self.assertEqual(parsed["char_count"], 2000)
        self.assertEqual(parsed["group_count"], 1047)
        max_group = max(parsed["groups"], key=len)
        self.assertEqual(len(max_group), 29)
        self.assertLessEqual(len(max_group), MAX_GROUP_MEMBERS)
        by_cp = {entry.codepoint: entry for entry in self.catalog["entries"]}
        cross = next(
            group
            for group in parsed["groups"]
            if len({by_cp[int(member["codepoint"])].shard for member in group}) > 1
        )
        primary = int(cross[0]["codepoint"])
        blob = StrokePinyinBlob()
        self.assertTrue(blob.bind(spy_bytes))
        candidates = blob.append_homophones(primary, 24)
        self.assertTrue(candidates)
        self.assertGreater(len({by_cp[cp].shard for cp in [primary] + candidates}), 1)
        with self.assertRaises(PinyinError):
            union_readings("shi4", "1.1:shí,shǐ,shì,shī,shi5,shéi,shén,shěn,shāng")

        oversized_group = [
            {"codepoint": 0x4E00 + index, "rank": index + 1, "readings": ["yi1"]}
            for index in range(MAX_GROUP_MEMBERS + 1)
        ]
        with self.assertRaisesRegex(PinyinError, "member count out of range"):
            pack_spy1(oversized_group)

    def test_runtime_pack_excludes_audit_manifests_and_assets_margin(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            runtime = tmp / "runtime"
            package_corpus(str(PROTOTYPE), str(runtime))
            runtime_payloads = tuple(COPY_MAP.values())
            expected = set(runtime_payloads) | {"SHA256SUMS"}
            self.assertEqual({item.name for item in runtime.iterdir()}, expected)
            verify_checksum_directory(runtime, runtime_payloads)
            self.assertLessEqual((runtime / "runtime.json").stat().st_size, 4096)
            for source_name, runtime_name in COPY_MAP.items():
                self.assertEqual(
                    (runtime / runtime_name).read_bytes(),
                    (PROTOTYPE / source_name).read_bytes(),
                    runtime_name,
                )
            for excluded in (
                "selection-2000.csv",
                "charset-2000.txt",
                "stroke_order.cov.json",
                "stroke_order.src.json",
                "stroke_pinyin.cov.json",
                "stroke_pinyin.manifest.json",
                "so00.manifest.json",
            ):
                self.assertNotIn(excluded, expected)
            self.assertTrue(all(len(name.encode("utf-8")) <= 31 for name in expected))

            output = tmp / "generated_assets.bin"
            limit = 8 * 1024 * 1024
            minimum_free = 256 * 1024
            self.assertTrue(
                build_assets_integrated(
                    [],
                    [],
                    None,
                    None,
                    str(runtime),
                    str(output),
                    max_output_bytes=limit,
                    min_free_bytes=minimum_free,
                )
            )
            output_blob = output.read_bytes()
            self.assertLess(len(output_blob), limit)
            self.assertGreaterEqual(limit - len(output_blob), minimum_free)
            packed = _unpack_generated_assets(output_blob)
            self.assertEqual(set(packed), expected | {"index.json"})
            for name in expected:
                self.assertEqual(packed[name]["data"], (runtime / name).read_bytes(), name)
            first_hash = hashlib.sha256(output_blob).hexdigest()

            second = tmp / "generated_assets_second.bin"
            self.assertTrue(
                build_assets_integrated(
                    [],
                    [],
                    None,
                    None,
                    str(runtime),
                    str(second),
                    max_output_bytes=limit,
                    min_free_bytes=minimum_free,
                )
            )
            self.assertEqual(hashlib.sha256(second.read_bytes()).hexdigest(), first_hash)
            self.assertFalse(
                build_assets_integrated(
                    [],
                    [],
                    None,
                    None,
                    str(runtime),
                    str(output),
                    max_output_bytes=output.stat().st_size + minimum_free - 1,
                    min_free_bytes=minimum_free,
                )
            )
            self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(), first_hash)

    def test_2000_packager_failure_preserves_previous_complete_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "runtime"
            package_corpus(str(PROTOTYPE), str(output))
            before = {item.name: hashlib.sha256(item.read_bytes()).hexdigest() for item in output.iterdir()}
            with mock.patch(
                "package_stroke_order_2000.shutil.copyfile",
                side_effect=OSError("injected copy failure"),
            ):
                with self.assertRaises(OSError):
                    package_corpus(str(PROTOTYPE), str(output))
            after = {item.name: hashlib.sha256(item.read_bytes()).hexdigest() for item in output.iterdir()}
            self.assertEqual(after, before)
            self.assertEqual(list(Path(directory).glob(".runtime.staging-*")), [])
            self.assertEqual(list(Path(directory).glob(".runtime.backup-*")), [])

    def test_cpp_catalog_and_controller_full_shard_validation_suspend_rebind(self):
        compiler = _compiler()
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "sharded"
            command = [
                compiler,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=undefined",
                "-fno-sanitize-recover=all",
                "-DSTROKE_ORDER_TESTING=1",
                "-I",
                str(ROOT / "main"),
                str(ROOT / "scripts/tests/stroke_order_sharded_harness.cc"),
                str(ROOT / "main/stroke_order/stroke_order_assets.cc"),
                str(ROOT / "main/stroke_order/stroke_order_catalog.cc"),
                str(ROOT / "main/stroke_order/stroke_order_controller.cc"),
                str(ROOT / "main/stroke_order/stroke_order_pinyin.cc"),
                str(ROOT / "main/stroke_order/stroke_order_store.cc"),
                "-o",
                str(executable),
            ]
            attempts = [command]
            xcrun = shutil.which("xcrun")
            if xcrun:
                sdk = subprocess.run(
                    [xcrun, "--show-sdk-path"], check=False, capture_output=True, text=True
                )
                if sdk.returncode == 0:
                    attempts.append(
                        command[:1]
                        + ["-isystem", f"{sdk.stdout.strip()}/usr/include/c++/v1"]
                        + command[1:]
                    )
            failures = []
            for attempt in attempts:
                compiled = subprocess.run(attempt, check=False, capture_output=True, text=True)
                if compiled.returncode == 0:
                    break
                failures.append(compiled.stdout + compiled.stderr)
            else:
                self.fail("C++ compile failed:\n" + "\n---\n".join(failures))
            mismatched_pinyin = Path(directory) / "mismatched.spy1"
            mismatched_pinyin.write_bytes(
                _pinyin_with_swapped_ranks((PROTOTYPE / "stroke_pinyin.spy1").read_bytes())
            )
            args = [
                str(executable),
                str(PROTOTYPE / "stroke_cat.bin"),
                str(PROTOTYPE / "stroke_pinyin.spy1"),
                str(mismatched_pinyin),
            ]
            args.extend(str(PROTOTYPE / f"so{i:02d}.sob1") for i in range(8))
            args.extend(
                [
                    f"{int(self.rows[0]['codepoint_int']):x}",
                    f"{int(self.rows[-1]['codepoint_int']):x}",
                ]
            )
            run = subprocess.run(args, check=False, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("stroke_order_sharded_harness: PASS", run.stdout)


if __name__ == "__main__":
    unittest.main()
