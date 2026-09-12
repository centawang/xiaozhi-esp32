import hashlib
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

from stroke_order import constants as C  # noqa: E402
from stroke_order.convert import (  # noqa: E402
    ConvertError,
    convert_dataset,
    load_character_json,
    load_charset,
    pack_characters,
)
from stroke_order.path import (  # noqa: E402
    PathError,
    flatten_cubic,
    flatten_quadratic,
    parse_outline_path,
)
from stroke_order.unpack import (  # noqa: E402
    StrokeOrderBlob,
    ValidationError,
    crc32,
    u32_add,
    u32_mul,
    validate_blob,
)


SOURCE_REPO = "https://github.com/chanind/hanzi-writer-data"
UPSTREAM_COMMIT = "68d10a4b21150cae5e1ebbd223eed289cf32d90c"
RECT_PATH = "M 0 256 L 1024 256 L 1024 768 L 0 768 Z"
RECT_MEDIAN = [[512, 256], [512, 768]]
GOLDEN_HEX = (
    "534f423101000004010000002000000030000000240000002147e17c32b26c6e"
    "004e0000300000002400000074c8df3b"
    "004e00000100000004000200000000000004000000040004000000000000000400040000"
)
SMOKE_HASHES = {
    "一": "ed728673d86fb9aa559c5b150e59ffbee666b085d4a55597c524079a91ebc8ce",
    "人": "18ffb9fb727576b0c3e7dc44914be7ffd43164de9ba121710824b83f27bd3bb1",
    "口": "ac208865e3166fc35214cec326f00a4b6788a8cac9b4c5a6a37745077e32ddab",
}
SMOKE_AGGREGATE = "c0f0eb725e23c91029ed59a5955c077fc4a1643606d21f9a935505b2088d78d6"
SMOKE_ROOT = ROOT / "scripts/tests/fixtures/stroke_order/upstream_smoke"
GOLDEN_PATH = ROOT / "scripts/tests/fixtures/stroke_order/handwritten_sob1_v1.hex"


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


def _write_dataset(directory: Path, mapping: dict):
    repo = directory / "hanzi-writer-data"
    data_dir = repo / "data"
    data_dir.mkdir(parents=True)
    for char, payload in mapping.items():
        (data_dir / f"{char}.json").write_text(
            json.dumps(payload, ensure_ascii=False),
            encoding="utf-8",
        )
    charset = directory / "charset.txt"
    charset.write_text("\n".join(mapping.keys()) + "\n", encoding="utf-8")

    _run(["git", "init", "-q", str(repo)])
    _run(["git", "-C", str(repo), "config", "user.name", "Stroke Test"])
    _run(["git", "-C", str(repo), "config", "user.email", "stroke@example.invalid"])
    _run(["git", "-C", str(repo), "remote", "add", "origin", SOURCE_REPO])
    _run(["git", "-C", str(repo), "add", "data"])
    env = os.environ.copy()
    env.update(
        {
            "GIT_AUTHOR_DATE": "2000-01-01T00:00:00+0000",
            "GIT_COMMITTER_DATE": "2000-01-01T00:00:00+0000",
        }
    )
    _run(["git", "-C", str(repo), "commit", "-q", "-m", "test fixture"], env=env)
    commit = _run(["git", "-C", str(repo), "rev-parse", "HEAD"])
    return repo, charset, commit


def _rect_payload():
    return {"strokes": [RECT_PATH], "medians": [RECT_MEDIAN]}


def _packed_item(char="一", outline=None, median=None):
    return {
        "character": char,
        "codepoint": ord(char),
        "strokes": [
            {
                "outline": outline or [(0, 0), (1024, 0), (1024, 1024), (0, 0)],
                "median": median or [(0, 1024), (1024, 0)],
            }
        ],
    }


def _convert(tmp: Path, mapping: dict):
    repo, charset, commit = _write_dataset(tmp, mapping)
    out = tmp / "out"
    return convert_dataset(
        hanzi_writer_data=str(repo),
        charset_path=str(charset),
        source_commit=commit,
        output_dir=str(out),
        source_repo=SOURCE_REPO,
    )


def _refresh_header_crc(blob: bytearray):
    struct.pack_into("<I", blob, 24, crc32(bytes(blob[:24])))


def _refresh_index_crc(blob: bytearray):
    index_offset, data_offset = struct.unpack_from("<II", blob, 12)
    struct.pack_into("<I", blob, 28, crc32(bytes(blob[index_offset:data_offset])))


def _compile_cpp_harness(output: Path):
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise AssertionError("no host C++ compiler found for StrokeOrderStore harness")
    common = [
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
        str(ROOT / "scripts/tests/stroke_order_store_harness.cc"),
        str(ROOT / "main/stroke_order/stroke_order_store.cc"),
        "-o",
        str(output),
    ]
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
            attempts.append(common[:1] + ["-isystem", f"{sdk.stdout.strip()}/usr/include/c++/v1"] + common[1:])

    failures = []
    for command in attempts:
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode == 0:
            return command
        failures.append(f"{' '.join(command)}\n{result.stdout}{result.stderr}")
    raise AssertionError("C++ harness compilation failed:\n" + "\n---\n".join(failures))


class StrokeOrderFoundationTest(unittest.TestCase):
    def test_cpp_constants_match_python(self):
        header = (ROOT / "main/stroke_order/stroke_order_store.h").read_text(encoding="utf-8")

        def grab(name: str) -> int:
            match = re.search(rf"{name} = (\d+);", header)
            self.assertIsNotNone(match, name)
            return int(match.group(1))

        self.assertEqual(grab("kFormatVersion"), C.FORMAT_VERSION)
        self.assertEqual(grab("kCoordMax"), C.COORD_MAX)
        self.assertEqual(grab("kHeaderSize"), C.HEADER_SIZE)
        self.assertEqual(grab("kIndexEntrySize"), C.INDEX_ENTRY_SIZE)
        self.assertEqual(grab("kCharacterRecordHeaderSize"), C.CHARACTER_RECORD_HEADER_SIZE)
        self.assertEqual(grab("kStrokeHeaderSize"), C.STROKE_HEADER_SIZE)
        self.assertEqual(grab("kPointSize"), C.POINT_SIZE)
        self.assertEqual(grab("kMaxCharacters"), C.MAX_CHARACTERS)
        self.assertEqual(grab("kMaxStrokesPerCharacter"), C.MAX_STROKES_PER_CHARACTER)
        self.assertEqual(grab("kMaxOutlinePointsPerStroke"), C.MAX_OUTLINE_POINTS_PER_STROKE)
        self.assertEqual(grab("kMaxMedianPointsPerStroke"), C.MAX_MEDIAN_POINTS_PER_STROKE)
        self.assertEqual(grab("kMaxCharacterBytes"), C.MAX_CHARACTER_BYTES)
        self.assertEqual(grab("kMaxFileBytes"), C.MAX_FILE_BYTES)

    def test_deterministic_output_and_bound_provenance(self):
        mapping = {"一": _rect_payload(), "人": _rect_payload()}
        with tempfile.TemporaryDirectory() as first_dir, tempfile.TemporaryDirectory() as second_dir:
            first = _convert(Path(first_dir), mapping)
            second = _convert(Path(second_dir), mapping)
            self.assertEqual(first["blob"], second["blob"])
            self.assertEqual(first["manifest"], second["manifest"])
            manifest = first["manifest"]
            self.assertEqual(manifest["sha256_bin"], hashlib.sha256(first["blob"]).hexdigest())
            self.assertTrue(manifest["not_a_release_library"])
            self.assertEqual(len(manifest["source"]["commit"]), 40)
            self.assertTrue(manifest["source"]["checkout_clean"])
            self.assertEqual(len(manifest["source"]["files"]), 2)
            digest_input = "".join(
                f"{item['codepoint']}\0{item['path']}\0{item['sha256']}\n"
                for item in manifest["source"]["files"]
            ).encode("utf-8")
            self.assertEqual(
                manifest["source"]["selected_files_sha256"],
                hashlib.sha256(digest_input).hexdigest(),
            )

    def test_provenance_rejects_short_mismatch_and_dirty_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            repo, charset, commit = _write_dataset(tmp, {"一": _rect_payload()})
            kwargs = {
                "hanzi_writer_data": str(repo),
                "charset_path": str(charset),
                "output_dir": str(tmp / "out"),
                "source_repo": SOURCE_REPO,
            }
            with self.assertRaises(ConvertError):
                convert_dataset(source_commit=commit[:12], **kwargs)
            mismatch = ("0" if commit[0] != "0" else "1") + commit[1:]
            with self.assertRaises(ConvertError):
                convert_dataset(source_commit=mismatch, **kwargs)
            (repo / "untracked.txt").write_text("dirty", encoding="utf-8")
            with self.assertRaises(ConvertError):
                convert_dataset(source_commit=commit, **kwargs)

    def test_provenance_rejects_wrong_origin(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            repo, charset, commit = _write_dataset(tmp, {"一": _rect_payload()})
            with self.assertRaises(ConvertError):
                convert_dataset(
                    hanzi_writer_data=str(repo),
                    charset_path=str(charset),
                    source_commit=commit,
                    output_dir=str(tmp / "out"),
                    source_repo="https://example.invalid/not-the-checkout",
                )

    def test_provenance_rejects_ignored_file_not_present_at_head(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            repo = tmp / "hanzi-writer-data"
            data = repo / "data"
            data.mkdir(parents=True)
            (repo / ".gitignore").write_text("data/\n", encoding="utf-8")
            (data / "一.json").write_text(json.dumps(_rect_payload()), encoding="utf-8")
            charset = tmp / "charset.txt"
            charset.write_text("一\n", encoding="utf-8")
            _run(["git", "init", "-q", str(repo)])
            _run(["git", "-C", str(repo), "config", "user.name", "Stroke Test"])
            _run(["git", "-C", str(repo), "config", "user.email", "stroke@example.invalid"])
            _run(["git", "-C", str(repo), "remote", "add", "origin", SOURCE_REPO])
            _run(["git", "-C", str(repo), "add", ".gitignore"])
            _run(["git", "-C", str(repo), "commit", "-q", "-m", "ignored fixture"])
            commit = _run(["git", "-C", str(repo), "rev-parse", "HEAD"])
            with self.assertRaises(ConvertError):
                convert_dataset(
                    hanzi_writer_data=str(repo),
                    charset_path=str(charset),
                    source_commit=commit,
                    output_dir=str(tmp / "out"),
                    source_repo=SOURCE_REPO,
                )

    def test_legal_minimal_character_and_y_axis(self):
        with tempfile.TemporaryDirectory() as directory:
            result = _convert(Path(directory), {"一": _rect_payload()})
            blob = StrokeOrderBlob()
            self.assertTrue(blob.bind(result["blob"]))
            self.assertTrue(blob.load_character(ord("一")))
            stroke = blob.strokes[0]
            self.assertEqual(
                stroke["outline"],
                [(0, 768), (1024, 768), (1024, 256), (0, 256), (0, 768)],
            )
            self.assertEqual(stroke["median"], [(512, 768), (512, 256)])
            parsed = validate_blob(result["blob"], load_all=True)
            self.assertEqual(parsed["char_count"], 1)

    def test_curve_flattening_quadratic_and_cubic(self):
        quadratic = parse_outline_path("M 0 0 Q 512 1024 1024 0 Z")
        self.assertEqual(quadratic[0], (0, 1024))
        self.assertEqual(quadratic[-1], (0, 1024))
        self.assertGreater(len(quadratic), 4)
        self.assertTrue(
            any(
                point == (512, 512)
                or abs(point[1] - 512) <= 1 and abs(point[0] - 512) <= 16
                for point in quadratic
            )
        )

        flat_q = flatten_quadratic((0.0, 0.0), (512.0, 0.0), (1024.0, 0.0))
        self.assertEqual(flat_q, [(1024.0, 0.0)])

        cubic_points = flatten_cubic(
            (0.0, 0.0), (0.0, 1024.0), (1024.0, 1024.0), (1024.0, 0.0)
        )
        self.assertEqual(cubic_points[-1], (1024.0, 0.0))
        self.assertGreater(len(cubic_points), 2)
        closed = parse_outline_path("M 0 0 C 0 1024 1024 1024 1024 0 Z")
        self.assertEqual(closed[0], (0, 1024))
        self.assertGreater(len(closed), 4)

    def test_unknown_command_rejected(self):
        with self.assertRaises(PathError):
            parse_outline_path("M 0 0 H 10 Z")
        with self.assertRaises(PathError):
            parse_outline_path("M 0 0 A 10 10 0 0 1 10 0 Z")

    def test_path_not_closed_rejected(self):
        with self.assertRaises(PathError):
            parse_outline_path("M 0 0 L 1024 0 L 1024 1024")

    def test_strokes_medians_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ConvertError):
                _convert(
                    Path(directory),
                    {"一": {"strokes": [RECT_PATH, RECT_PATH], "medians": [RECT_MEDIAN]}},
                )

    def test_coordinate_and_nonfinite_rejected(self):
        with self.assertRaises(PathError):
            parse_outline_path("M 0 0 L 2000 0 L 2000 10 L 0 10 Z")
        with self.assertRaises(PathError):
            parse_outline_path("M 0 0 L " + "9" * 400 + " 0 L 0 10 Z")
        for bad in (float("nan"), float("inf"), float("-inf")):
            with tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(ConvertError):
                    _convert(
                        Path(directory),
                        {"一": {"strokes": [RECT_PATH], "medians": [[[bad, 0], [0, 0]]]}},
                    )

    def test_point_count_limit(self):
        commands = ["M 0 0"]
        for i in range(1, C.MAX_OUTLINE_POINTS_PER_STROKE + 2):
            commands.append(f"L {min(i, C.COORD_MAX)} 0")
        commands.append("L 0 10 Z")
        path = " ".join(commands)
        with tempfile.TemporaryDirectory() as directory:
            result = _convert(
                Path(directory),
                {"一": {"strokes": [path], "medians": [[[0, 0], [10, 0]]]}},
            )
            parsed = validate_blob(result["blob"], load_all=True)
            outline = parsed["characters"][0]["strokes"][0]["outline"]
            self.assertLessEqual(len(outline), C.MAX_OUTLINE_POINTS_PER_STROKE)
            self.assertGreaterEqual(len(outline), C.MIN_OUTLINE_POINTS_PER_STROKE)

    def test_truncated_record_checksum_and_index_checksum(self):
        with tempfile.TemporaryDirectory() as directory:
            result = _convert(Path(directory), {"一": _rect_payload(), "人": _rect_payload()})
            blob = bytearray(result["blob"])
            store = StrokeOrderBlob()
            self.assertTrue(store.bind(bytes(blob)))
            self.assertTrue(store.load_character(ord("一")))

            with self.assertRaises(ValidationError):
                validate_blob(bytes(blob[:-1]))
            self.assertFalse(StrokeOrderBlob().bind(bytes(blob[:-1])))

            parsed = validate_blob(bytes(blob), load_all=False)
            second = parsed["entries"][1]
            blob[second[1] + 8] ^= 0xFF
            damaged = StrokeOrderBlob()
            self.assertTrue(damaged.bind(bytes(blob)))
            self.assertTrue(damaged.load_character(ord("一")))
            self.assertFalse(damaged.load_character(ord("人")))
            self.assertEqual(damaged.loaded_codepoint, ord("一"))

            index_damaged = bytearray(result["blob"])
            index_damaged[C.HEADER_SIZE] ^= 1
            self.assertFalse(StrokeOrderBlob().bind(bytes(index_damaged)))
            with self.assertRaises(ValidationError):
                validate_blob(bytes(index_damaged))

    def test_charset_rejects_duplicates_paths_and_non_target_scalars(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            cases = {
                "duplicate": "一\n一\n",
                "slash": "/\n",
                "backslash": "\\\n",
                "ascii": "A\n",
                "emoji": "😀\n",
            }
            for name, value in cases.items():
                path = tmp / f"{name}.txt"
                path.write_text(value, encoding="utf-8")
                with self.assertRaises(ConvertError, msg=name):
                    load_charset(str(path))
            bad = tmp / "invalid-utf8.txt"
            bad.write_bytes(b"\xff\xfe\x01")
            with self.assertRaises(ConvertError):
                load_charset(str(bad))
            with self.assertRaises(ConvertError):
                pack_characters([_packed_item(chr(0xD800))])

    def test_resolved_glyph_path_must_stay_in_data_dir(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            root = tmp / "source"
            data = root / "data"
            data.mkdir(parents=True)
            outside = tmp / "outside.json"
            outside.write_text(json.dumps(_rect_payload()), encoding="utf-8")
            try:
                (data / "一.json").symlink_to(outside)
            except OSError as exc:
                self.skipTest(f"symlinks unavailable: {exc}")
            with self.assertRaises(ConvertError):
                load_character_json(root, "一", root)

    def test_pack_characters_enforces_all_public_invariants(self):
        valid = _packed_item()
        self.assertTrue(validate_blob(pack_characters([valid])))
        with self.assertRaises(ConvertError):
            pack_characters([valid, valid])
        with self.assertRaises(ConvertError):
            pack_characters([valid] * (C.MAX_CHARACTERS + 1))
        mismatch = _packed_item()
        mismatch["codepoint"] = ord("人")
        with self.assertRaises(ConvertError):
            pack_characters([mismatch])
        bad_coordinate = _packed_item(median=[(0, 0), (C.COORD_MAX + 1, 0)])
        with self.assertRaises(ConvertError):
            pack_characters([bad_coordinate])
        open_outline = _packed_item(outline=[(0, 0), (1, 0), (1, 1), (0, 1)])
        with self.assertRaises(ConvertError):
            pack_characters([open_outline])

    def test_malicious_offset_length_size_and_count(self):
        with tempfile.TemporaryDirectory() as directory:
            result = _convert(Path(directory), {"一": _rect_payload()})
            blob = bytearray(result["blob"])
            struct.pack_into("<II", blob, C.HEADER_SIZE + 4, 0xFFFFFFF0, 0x20)
            _refresh_index_crc(blob)
            with self.assertRaises(ValidationError):
                validate_blob(bytes(blob))
            self.assertFalse(StrokeOrderBlob().bind(bytes(blob)))

            overflow_header = bytearray(result["blob"])
            struct.pack_into("<I", overflow_header, 8, 0x10000000)
            _refresh_header_crc(overflow_header)
            with self.assertRaises(ValidationError):
                validate_blob(bytes(overflow_header))
            self.assertFalse(StrokeOrderBlob().bind(bytes(overflow_header)))

        with self.assertRaises(ValidationError):
            u32_add(0xFFFFFFF0, 0x20)
        with self.assertRaises(ValidationError):
            u32_mul(0x10000000, 16)
        self.assertEqual(u32_add(32, 16), 48)

    def test_failed_bind_does_not_clear_previous(self):
        with tempfile.TemporaryDirectory() as directory:
            result = _convert(Path(directory), {"一": _rect_payload()})
            store = StrokeOrderBlob()
            self.assertTrue(store.bind(result["blob"]))
            self.assertTrue(store.load_character(ord("一")))
            self.assertFalse(store.bind(b"not-a-blob"))
            self.assertTrue(store.is_bound)
            self.assertEqual(store.loaded_codepoint, ord("一"))

    def test_pack_rejects_empty_stroke_set_via_json(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ConvertError):
                _convert(Path(directory), {"一": {"strokes": [], "medians": []}})

    def test_network_url_and_unc_data_paths_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            charset = tmp / "charset.txt"
            charset.write_text("一\n", encoding="utf-8")
            for source in ("https://example.com/hanzi-writer-data", "//server/share", "\\\\server\\share"):
                with self.assertRaises(ConvertError):
                    convert_dataset(
                        hanzi_writer_data=source,
                        charset_path=str(charset),
                        source_commit=UPSTREAM_COMMIT,
                        output_dir=str(tmp / "out"),
                    )

    def test_handwritten_golden_and_crc_known_answers(self):
        compact = "".join(GOLDEN_PATH.read_text(encoding="ascii").split())
        self.assertEqual(compact, GOLDEN_HEX)
        blob = bytes.fromhex(compact)
        self.assertEqual(crc32(b"123456789"), 0xCBF43926)
        self.assertEqual(crc32(blob[:24]), 0x7CE14721)
        self.assertEqual(crc32(blob[32:48]), 0x6E6CB232)
        self.assertEqual(crc32(blob[48:]), 0x3BDFC874)
        parsed = validate_blob(blob, load_all=True)
        self.assertEqual(parsed["characters"][0]["codepoint"], ord("一"))
        self.assertEqual(parsed["characters"][0]["strokes"][0]["outline"][2], (1024, 1024))

    def test_real_upstream_smoke_corpus_hashes_license_and_conversion(self):
        license_path = SMOKE_ROOT / "ARPHICPL.TXT"
        self.assertEqual(
            hashlib.sha256(license_path.read_bytes()).hexdigest(),
            "5590533436c70f10f2f524ee61456238c290175c6662fbe1c700b5f038a6d328",
        )
        notice = (SMOKE_ROOT / "NOTICE.md").read_text(encoding="utf-8")
        self.assertIn(UPSTREAM_COMMIT, notice)
        self.assertIn(SMOKE_AGGREGATE, notice)
        characters = []
        aggregate_lines = []
        for char in sorted(SMOKE_HASHES, key=ord):
            path = SMOKE_ROOT / "data" / f"{char}.json"
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            self.assertEqual(digest, SMOKE_HASHES[char])
            self.assertIn(digest, notice)
            aggregate_lines.append(f"U+{ord(char):04X}\0data/{char}.json\0{digest}\n")
            characters.append(load_character_json(SMOKE_ROOT, char, SMOKE_ROOT))
        self.assertEqual(
            hashlib.sha256("".join(aggregate_lines).encode("utf-8")).hexdigest(),
            SMOKE_AGGREGATE,
        )
        parsed = validate_blob(pack_characters(characters), load_all=True)
        self.assertEqual([item["codepoint"] for item in parsed["characters"]], [0x4E00, 0x4EBA, 0x53E3])

    def test_cpp_host_harness_generated_golden_unaligned_and_corruption(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            result = _convert(tmp, {"一": _rect_payload(), "人": _rect_payload()})
            generated = tmp / "python-generated.bin"
            generated.write_bytes(result["blob"])
            smoke_characters = [
                load_character_json(SMOKE_ROOT, char, SMOKE_ROOT)
                for char in sorted(SMOKE_HASHES, key=ord)
            ]
            smoke = tmp / "upstream-smoke.bin"
            smoke.write_bytes(pack_characters(smoke_characters))
            executable = tmp / "stroke_order_store_harness"
            _compile_cpp_harness(executable)
            run = subprocess.run(
                [str(executable), str(generated), str(GOLDEN_PATH), str(smoke)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
            )
            self.assertEqual(run.returncode, 0, f"stdout:\n{run.stdout}\nstderr:\n{run.stderr}")
            self.assertIn("stroke_order_store_harness: PASS", run.stdout)

    def test_cli_module_exists_and_is_offline(self):
        spec = importlib.util.spec_from_file_location(
            "convert_stroke_order_cli", SCRIPTS / "convert_stroke_order.py"
        )
        self.assertIsNotNone(spec)
        source = (SCRIPTS / "convert_stroke_order.py").read_text(encoding="utf-8")
        self.assertNotIn("urllib", source)
        converter = (SCRIPTS / "stroke_order" / "convert.py").read_text(encoding="utf-8")
        self.assertIn("Never accesses the network", converter)
        self.assertNotIn("requests", converter)


if __name__ == "__main__":
    unittest.main()
