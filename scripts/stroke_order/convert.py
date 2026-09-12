"""Offline hanzi-writer-data -> SOB1 converter. Never accesses the network."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

from .constants import (
    CHARACTER_RECORD_HEADER_SIZE,
    COORD_MAX,
    DEFAULT_SOURCE_REPO,
    DICTIONARY_LICENSE_NOTE,
    FLATTEN_MAX_DEPTH,
    FLATTEN_TOLERANCE,
    FORMAT_VERSION,
    GRAPHICS_LICENSE,
    HEADER_SIZE,
    HEADER_STRUCT_FORMAT,
    INDEX_ENTRY_SIZE,
    INDEX_STRUCT_FORMAT,
    MAGIC,
    MAX_CHARACTER_BYTES,
    MAX_CHARACTERS,
    MAX_FILE_BYTES,
    MAX_MEDIAN_POINTS_PER_STROKE,
    MAX_OUTLINE_POINTS_PER_STROKE,
    MAX_STROKES_PER_CHARACTER,
    PACK_OUTLINE_POINTS_PER_STROKE,
    MAX_TARGET_CODEPOINT,
    MIN_MEDIAN_POINTS_PER_STROKE,
    MIN_OUTLINE_POINTS_PER_STROKE,
    MIN_TARGET_CODEPOINT,
    POINT_STRUCT_FORMAT,
    SOURCE_PROJECT,
    SOURCE_UPSTREAM,
    STROKE_HEADER_STRUCT_FORMAT,
)
from .path import PathError, convert_median_points, parse_outline_path
from .unpack import crc32, validate_blob

COMMIT_RE = re.compile(r"^[0-9a-fA-F]{40}$")
URL_RE = re.compile(r"^[a-zA-Z][a-zA-Z0-9+.-]*://")

Point = Tuple[int, int]
Stroke = Dict[str, List[Point]]
Character = Dict[str, object]


class ConvertError(ValueError):
    """Raised when conversion inputs or outputs are invalid."""


def report_charset_coverage(
    hanzi_writer_data: str,
    charset_path: str,
    source_commit: str,
    source_repo: str = DEFAULT_SOURCE_REPO,
) -> Dict[str, object]:
    data_dir = _require_local_dir(hanzi_writer_data, "hanzi-writer-data")
    charset = load_charset(charset_path)
    commit = _require_commit(source_commit)
    repo = _require_source_repo(source_repo)
    checkout = _verify_git_checkout(data_dir, commit, repo)
    if len(charset) > MAX_CHARACTERS:
        raise ConvertError("character count exceeds limit")

    records: List[Dict[str, object]] = []
    ok_count = 0
    for char in charset:
        rec: Dict[str, object] = {
            "character": char,
            "codepoint": f"U+{ord(char):04X}",
            "status": "ok",
        }
        try:
            character = load_character_json(data_dir, char, checkout["root"])
            _verify_source_file_at_head(
                checkout["root"],
                str(character["source_path"]),
                str(character["source_sha256"]),
            )
            packed = _pack_character(character)
            if len(packed) > MAX_CHARACTER_BYTES:
                raise ConvertError("record exceeds size limit")
            rec["source_path"] = character["source_path"]
            rec["source_sha256"] = character["source_sha256"]
            rec["stroke_count"] = len(character["strokes"])  # type: ignore[arg-type]
            rec["record_bytes"] = len(packed)
            rec["character_data"] = character
            ok_count += 1
        except (ConvertError, PathError) as exc:
            rec["status"] = "error"
            rec["error"] = str(exc)
        records.append(rec)

    error_records = [item for item in records if item["status"] != "ok"]
    return {
        "character_count": len(charset),
        "complete": ok_count == len(charset) and len(charset) > 0 and not error_records,
        "error_count": len(error_records),
        "errors": [
            {
                "character": item["character"],
                "codepoint": item["codepoint"],
                "error": item.get("error"),
            }
            for item in error_records
        ],
        "not_a_release_library": True,
        "ok_count": ok_count,
        "records": records,
        "source": {
            "checkout_clean": True,
            "commit": str(checkout["head"]),
            "repo": str(checkout["origin"]),
        },
    }


def convert_dataset(
    hanzi_writer_data: str,
    charset_path: str,
    source_commit: str,
    output_dir: str,
    source_repo: str = DEFAULT_SOURCE_REPO,
) -> Dict[str, object]:
    coverage = report_charset_coverage(
        hanzi_writer_data, charset_path, source_commit, source_repo
    )
    if not coverage["complete"]:
        raise ConvertError(
            "HWD coverage is not complete; refusing to fill from later ranks: "
            + json.dumps(coverage["errors"], ensure_ascii=False)
        )
    characters = [item["character_data"] for item in coverage["records"]]  # type: ignore[misc]
    checkout = {
        "head": coverage["source"]["commit"],
        "origin": coverage["source"]["repo"],
    }

    blob = pack_characters(characters)
    validate_blob(blob, load_all=True)
    if len(blob) > MAX_FILE_BYTES:
        raise ConvertError("packed file exceeds size limit")
    manifest = build_manifest(characters, blob, checkout)

    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)
    bin_path = out / "stroke_order.bin"
    manifest_path = out / "stroke_order.manifest.json"
    _atomic_write(bin_path, blob)
    _atomic_write(
        manifest_path,
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True).encode("utf-8")
        + b"\n",
    )
    return {
        "bin_path": str(bin_path),
        "manifest_path": str(manifest_path),
        "blob": blob,
        "manifest": manifest,
        "characters": characters,
    }


def load_charset(charset_path: str) -> List[str]:
    path = Path(charset_path)
    if not path.is_file():
        raise ConvertError("charset file not found")
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ConvertError("charset is not valid UTF-8") from exc
    if text.startswith("\ufeff"):
        text = text[1:]

    chars: List[str] = []
    seen = set()
    for line_no, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        try:
            _validate_target_character(stripped)
        except ConvertError as exc:
            raise ConvertError(f"charset line {line_no}: {exc}") from exc
        if stripped in seen:
            raise ConvertError(f"duplicate character {stripped!r}")
        seen.add(stripped)
        chars.append(stripped)
    return chars


def load_character_json(
    data_dir: Path, char: str, source_root: Path | None = None
) -> Character:
    _validate_target_character(char)
    json_path = _character_json_path(data_dir, char)
    raw = json_path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ConvertError(f"{char} JSON is not valid UTF-8") from exc
    try:
        payload = json.loads(text, parse_constant=_reject_json_constant)
    except json.JSONDecodeError as exc:
        raise ConvertError(f"{char} JSON is invalid") from exc
    if not isinstance(payload, dict):
        raise ConvertError(f"{char} JSON must be an object")
    strokes_raw = payload.get("strokes")
    medians_raw = payload.get("medians")
    if not isinstance(strokes_raw, list) or not isinstance(medians_raw, list):
        raise ConvertError(f"{char} missing strokes or medians")
    if len(strokes_raw) != len(medians_raw):
        raise ConvertError(f"{char} strokes/medians count mismatch")
    if not (1 <= len(strokes_raw) <= MAX_STROKES_PER_CHARACTER):
        raise ConvertError(f"{char} stroke count out of range")

    strokes: List[Stroke] = []
    for index, (stroke_path, median) in enumerate(zip(strokes_raw, medians_raw)):
        if not isinstance(stroke_path, str):
            raise ConvertError(f"{char} stroke {index} is not a path string")
        try:
            outline = parse_outline_path(stroke_path)
            median_points = convert_median_points(median)
        except PathError as exc:
            raise ConvertError(f"{char} stroke {index}: {exc}") from exc
        if not (
            MIN_OUTLINE_POINTS_PER_STROKE
            <= len(outline)
            <= MAX_OUTLINE_POINTS_PER_STROKE
        ):
            raise ConvertError(f"{char} stroke {index} outline point count out of range")
        if not (
            MIN_MEDIAN_POINTS_PER_STROKE
            <= len(median_points)
            <= MAX_MEDIAN_POINTS_PER_STROKE
        ):
            raise ConvertError(f"{char} stroke {index} median point count out of range")
        strokes.append({"outline": outline, "median": median_points})

    root = (source_root or data_dir).resolve(strict=True)
    try:
        source_path = json_path.relative_to(root).as_posix()
    except ValueError as exc:
        raise ConvertError(f"{char} JSON resolves outside the source checkout") from exc
    return {
        "character": char,
        "codepoint": ord(char),
        "source_path": source_path,
        "source_sha256": hashlib.sha256(raw).hexdigest(),
        "strokes": strokes,
    }


def pack_characters(characters: Sequence[Character]) -> bytes:
    if not isinstance(characters, (list, tuple)):
        raise ConvertError("characters must be a bounded sequence")
    if len(characters) > MAX_CHARACTERS:
        raise ConvertError("character count exceeds limit")

    seen = set()
    validated: List[Character] = []
    for item in characters:
        _validate_pack_character(item)
        codepoint = int(item["codepoint"])
        if codepoint in seen:
            raise ConvertError(f"duplicate codepoint U+{codepoint:04X}")
        seen.add(codepoint)
        validated.append(item)

    ordered = sorted(validated, key=lambda item: int(item["codepoint"]))
    records: List[Tuple[int, bytes]] = []
    for item in ordered:
        record = _pack_character(item)
        if len(record) > MAX_CHARACTER_BYTES:
            raise ConvertError(f"U+{item['codepoint']:04X} record exceeds size limit")
        records.append((int(item["codepoint"]), record))

    char_count = len(records)
    index_offset = HEADER_SIZE
    index_bytes = char_count * INDEX_ENTRY_SIZE
    data_offset = index_offset + index_bytes
    data_size = sum(len(record) for _, record in records)
    file_size = data_offset + data_size
    if file_size > MAX_FILE_BYTES:
        raise ConvertError("packed file exceeds size limit")

    index_parts = []
    data_parts = []
    cursor = data_offset
    for codepoint, record in records:
        index_parts.append(
            struct.pack(INDEX_STRUCT_FORMAT, codepoint, cursor, len(record), crc32(record))
        )
        data_parts.append(record)
        cursor += len(record)
    index = b"".join(index_parts)

    header_without_crc = struct.pack(
        HEADER_STRUCT_FORMAT,
        MAGIC,
        FORMAT_VERSION,
        COORD_MAX,
        char_count,
        index_offset,
        data_offset,
        data_size,
        0,
        0,
    )
    header_crc = crc32(header_without_crc[:24])
    header = struct.pack(
        HEADER_STRUCT_FORMAT,
        MAGIC,
        FORMAT_VERSION,
        COORD_MAX,
        char_count,
        index_offset,
        data_offset,
        data_size,
        header_crc,
        crc32(index),
    )

    blob = header + index + b"".join(data_parts)
    if len(blob) != file_size:
        raise ConvertError("internal packing size mismatch")
    return blob


def build_manifest(
    characters: Sequence[Character],
    blob: bytes,
    checkout: Dict[str, object],
) -> Dict[str, object]:
    ordered = sorted(characters, key=lambda item: int(item["codepoint"]))
    source_files = [
        {
            "character": str(item["character"]),
            "codepoint": f"U+{int(item['codepoint']):04X}",
            "path": str(item["source_path"]),
            "sha256": str(item["source_sha256"]),
        }
        for item in ordered
    ]
    digest_input = "".join(
        f"{item['codepoint']}\0{item['path']}\0{item['sha256']}\n"
        for item in source_files
    ).encode("utf-8")
    return {
        "character_count": len(ordered),
        "characters": [str(item["character"]) for item in ordered],
        "codepoints": [f"U+{int(item['codepoint']):04X}" for item in ordered],
        "conversion": {
            "coord_space": "integer-0-1024-y-down",
            "curve_flattening": {
                "algorithm": "de_casteljau_recursive",
                "max_depth": FLATTEN_MAX_DEPTH,
                "tolerance": FLATTEN_TOLERANCE,
            },
            "outline_fit_max_points": PACK_OUTLINE_POINTS_PER_STROKE,
            "outline_format_max_points": MAX_OUTLINE_POINTS_PER_STROKE,
            "script": "scripts/convert_stroke_order.py",
            "source_coordinate_slop": 128,
            "source_coordinate_slop_note": (
                "MMAH control points slightly outside 0..1024 are clamped into "
                "the SOB1 box; coordinates farther away are still rejected. "
                "Device format limits are unchanged."
            ),
            "y_axis": "flip_to_y_down",
        },
        "format": "stroke_order_bin",
        "format_version": FORMAT_VERSION,
        "limits": {
            "max_character_bytes": MAX_CHARACTER_BYTES,
            "max_characters": MAX_CHARACTERS,
            "max_file_bytes": MAX_FILE_BYTES,
            "max_median_points_per_stroke": MAX_MEDIAN_POINTS_PER_STROKE,
            "max_outline_points_per_stroke": MAX_OUTLINE_POINTS_PER_STROKE,
            "max_strokes_per_character": MAX_STROKES_PER_CHARACTER,
        },
        "magic": MAGIC.decode("ascii"),
        "not_a_release_library": True,
        "sha256_bin": hashlib.sha256(blob).hexdigest(),
        "source": {
            "checkout_clean": True,
            "commit": str(checkout["head"]),
            "dictionary_license_note": DICTIONARY_LICENSE_NOTE,
            "files": source_files,
            "graphics_license": GRAPHICS_LICENSE,
            "project": SOURCE_PROJECT,
            "repo": str(checkout["origin"]),
            "selected_files_sha256": hashlib.sha256(digest_input).hexdigest(),
            "selected_files_sha256_encoding": "UTF-8 lines: codepoint\\0path\\0sha256\\n",
            "upstream": SOURCE_UPSTREAM,
        },
        "usage": (
            "Prototype conversion output. Do not describe this file or its test "
            "fixtures as a published 字库. Commercial release requires license "
            "review and stroke-order accuracy review."
        ),
    }


def _validate_pack_character(item: Character) -> None:
    if not isinstance(item, dict):
        raise ConvertError("character entry must be an object")
    character = item.get("character")
    codepoint = item.get("codepoint")
    _validate_target_character(character)
    if type(codepoint) is not int or codepoint != ord(character):
        raise ConvertError("character/codepoint mismatch")
    strokes = item.get("strokes")
    if not isinstance(strokes, (list, tuple)) or not (
        1 <= len(strokes) <= MAX_STROKES_PER_CHARACTER
    ):
        raise ConvertError(f"U+{codepoint:04X} stroke count out of range")
    for stroke_index, stroke in enumerate(strokes):
        if not isinstance(stroke, dict):
            raise ConvertError(f"U+{codepoint:04X} stroke {stroke_index} is not an object")
        outline = stroke.get("outline")
        median = stroke.get("median")
        _validate_points(
            outline,
            MIN_OUTLINE_POINTS_PER_STROKE,
            MAX_OUTLINE_POINTS_PER_STROKE,
            f"U+{codepoint:04X} stroke {stroke_index} outline",
        )
        _validate_points(
            median,
            MIN_MEDIAN_POINTS_PER_STROKE,
            MAX_MEDIAN_POINTS_PER_STROKE,
            f"U+{codepoint:04X} stroke {stroke_index} median",
        )
        if outline[0] != outline[-1]:
            raise ConvertError(f"U+{codepoint:04X} stroke {stroke_index} outline is not closed")


def _validate_points(value: object, minimum: int, maximum: int, label: str) -> None:
    if not isinstance(value, (list, tuple)) or not (minimum <= len(value) <= maximum):
        raise ConvertError(f"{label} point count out of range")
    for point in value:
        if not isinstance(point, (list, tuple)) or len(point) != 2:
            raise ConvertError(f"{label} point must be an x/y pair")
        x, y = point
        if type(x) is not int or type(y) is not int:
            raise ConvertError(f"{label} coordinates must be integers")
        if not (0 <= x <= COORD_MAX and 0 <= y <= COORD_MAX):
            raise ConvertError(f"{label} coordinate out of range")


def _pack_character(item: Character) -> bytes:
    strokes: Sequence[Stroke] = item["strokes"]  # type: ignore[assignment]
    parts = [struct.pack("<IHH", int(item["codepoint"]), len(strokes), 0)]
    size = CHARACTER_RECORD_HEADER_SIZE
    for stroke in strokes:
        outline = stroke["outline"]
        median = stroke["median"]
        parts.append(struct.pack(STROKE_HEADER_STRUCT_FORMAT, len(outline), len(median)))
        size += 4 + 4 * (len(outline) + len(median))
        for x, y in outline:
            parts.append(struct.pack(POINT_STRUCT_FORMAT, x, y))
        for x, y in median:
            parts.append(struct.pack(POINT_STRUCT_FORMAT, x, y))
        if size > MAX_CHARACTER_BYTES:
            raise ConvertError("character exceeded size while packing")
    return b"".join(parts)


def _validate_target_character(char: object) -> None:
    if not isinstance(char, str) or len(char) != 1:
        raise ConvertError("entry must be exactly one Unicode character")
    if char in {"/", "\\"} or char == os.sep or (os.altsep and char == os.altsep):
        raise ConvertError("path separators are not allowed")
    codepoint = ord(char)
    if codepoint < MIN_TARGET_CODEPOINT or codepoint > MAX_TARGET_CODEPOINT:
        raise ConvertError("character is outside the supported CJK Unified Ideographs block")


def _character_json_path(data_dir: Path, char: str) -> Path:
    _validate_target_character(char)
    root = data_dir.resolve(strict=True)
    for candidate in (root / f"{char}.json", root / "data" / f"{char}.json"):
        if not candidate.is_file():
            continue
        resolved = candidate.resolve(strict=True)
        try:
            resolved.relative_to(root)
        except ValueError as exc:
            raise ConvertError(f"graphics JSON for {char!r} resolves outside data directory") from exc
        return resolved
    raise ConvertError(f"missing graphics JSON for {char!r}")


def _require_local_dir(path_str: str, label: str) -> Path:
    if not path_str or URL_RE.match(path_str) or path_str.startswith(("//", "\\\\")):
        raise ConvertError(f"{label} must be a local directory; URLs and UNC paths are rejected")
    path = Path(path_str)
    if not path.is_dir():
        raise ConvertError(f"{label} directory not found")
    return path.resolve(strict=True)


def _require_commit(commit: str) -> str:
    if not commit or not COMMIT_RE.fullmatch(commit):
        raise ConvertError("source commit must be a complete 40-hex git commit")
    return commit.lower()


def _require_source_repo(source_repo: str) -> str:
    if not source_repo or not URL_RE.match(source_repo):
        raise ConvertError("source repo must be an explicit URL used only for provenance")
    return source_repo


def _verify_git_checkout(data_dir: Path, expected_commit: str, expected_repo: str) -> Dict[str, object]:
    root_text = _run_git(data_dir, "rev-parse", "--show-toplevel")
    root = Path(root_text).resolve(strict=True)
    try:
        data_dir.relative_to(root)
    except ValueError as exc:
        raise ConvertError("hanzi-writer-data directory is outside its git checkout") from exc

    head = _run_git(root, "rev-parse", "--verify", "HEAD^{commit}").lower()
    if not COMMIT_RE.fullmatch(head):
        raise ConvertError("git returned an invalid checkout HEAD")
    if head != expected_commit:
        raise ConvertError(f"source commit does not match checkout HEAD {head}")

    dirty = _run_git(root, "status", "--porcelain=v1", "--untracked-files=all")
    if dirty:
        raise ConvertError("source checkout is dirty; commit or remove all changes first")

    origin = _run_git(root, "config", "--get", "remote.origin.url")
    if _normalize_repo_url(origin) != _normalize_repo_url(expected_repo):
        raise ConvertError("source repo does not match checkout remote.origin.url")
    return {"root": root, "head": head, "origin": origin}


def _verify_source_file_at_head(root: Path, relative_path: str, actual_sha256: str) -> None:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "show", f"HEAD:{relative_path}"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise ConvertError("git is required to verify source provenance") from exc
    if result.returncode != 0:
        raise ConvertError(f"selected source file is not tracked at HEAD: {relative_path}")
    if hashlib.sha256(result.stdout).hexdigest() != actual_sha256:
        raise ConvertError(f"selected source file differs from HEAD: {relative_path}")


def _run_git(directory: Path, *args: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(directory), *args],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as exc:
        raise ConvertError("git is required to verify source provenance") from exc
    if result.returncode != 0:
        detail = result.stderr.strip() or "git command failed"
        raise ConvertError(f"cannot verify source checkout: {detail}")
    return result.stdout.strip()


def _normalize_repo_url(url: str) -> str:
    normalized = url.strip().rstrip("/")
    if normalized.endswith(".git"):
        normalized = normalized[:-4]
    return normalized


def _reject_json_constant(value: str) -> None:
    raise ConvertError(f"non-finite JSON number {value} is not allowed")


def _atomic_write(path: Path, data: bytes) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Convert an explicit hanzi-writer-data subset into stroke_order.bin. "
            "Does not download data or accept SVG/script input."
        )
    )
    parser.add_argument(
        "--hanzi-writer-data", required=True, help="Clean local git checkout directory"
    )
    parser.add_argument("--charset", required=True, help="UTF-8 file with one character per line")
    parser.add_argument(
        "--source-commit", required=True, help="Complete 40-hex checkout HEAD"
    )
    parser.add_argument("--output-dir", required=True, help="Directory for bin and manifest")
    parser.add_argument(
        "--source-repo",
        default=DEFAULT_SOURCE_REPO,
        help="Expected checkout origin URL; never fetched",
    )
    return parser.parse_args(argv)


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(list(argv) if argv is not None else sys.argv[1:])
    try:
        result = convert_dataset(
            hanzi_writer_data=args.hanzi_writer_data,
            charset_path=args.charset,
            source_commit=args.source_commit,
            output_dir=args.output_dir,
            source_repo=args.source_repo,
        )
    except (ConvertError, PathError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(result["bin_path"])
    print(result["manifest_path"])
    return 0
