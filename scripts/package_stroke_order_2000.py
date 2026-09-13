#!/usr/bin/env python3
"""Offline packager for the reviewed sharded 2000-character prototype."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from stroke_order.catalog import crc32, validate_catalog  # noqa: E402
from stroke_order.pinyin import validate_spy1  # noqa: E402
from stroke_order.select import load_selection_csv  # noqa: E402
from stroke_order.unpack import validate_blob  # noqa: E402

CHECKSUM_NAME = "SHA256SUMS"
SUM_LINE_RE = re.compile(r"^([0-9a-f]{64})  ([^/\\]+)$")
ASSET_NAME_MAX = 31
SHARD_COUNT = 8
SOURCE_PAYLOADS = (
    "ARPHICPL.TXT",
    "NOTICE.md",
    "UNICODE-LICENSE.txt",
    "charset-2000.txt",
    "runtime.json",
    "selection-2000.csv",
    "stroke_cat.bin",
    "stroke_order.cov.json",
    "stroke_order.src.json",
    "stroke_pinyin.cov.json",
    "stroke_pinyin.manifest.json",
    "stroke_pinyin.spy1",
) + tuple(f"so{i:02d}.sob1" for i in range(SHARD_COUNT)) + tuple(
    f"so{i:02d}.manifest.json" for i in range(SHARD_COUNT)
)
COPY_MAP = {
    **{f"so{i:02d}.sob1": f"so{i:02d}.bin" for i in range(SHARD_COUNT)},
    "stroke_cat.bin": "stroke_cat.bin",
    "stroke_pinyin.spy1": "stroke_pinyin.bin",
    "runtime.json": "runtime.json",
    "ARPHICPL.TXT": "ARPHICPL.TXT",
    "UNICODE-LICENSE.txt": "UNICODE-LICENSE.txt",
    "NOTICE.md": "NOTICE.md",
}


class PackageError(ValueError):
    pass


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_checksum_directory(directory: Path, payload_names: tuple[str, ...]) -> None:
    expected = set(payload_names) | {CHECKSUM_NAME}
    actual = {path.name for path in directory.iterdir()}
    if actual != expected:
        raise PackageError(
            f"checksum directory mismatch: missing={sorted(expected-actual)}, extra={sorted(actual-expected)}"
        )
    if any(not (directory / name).is_file() or (directory / name).is_symlink() for name in expected):
        raise PackageError("checksum directory contains non-regular payload")
    listed = {}
    for line in (directory / CHECKSUM_NAME).read_text(encoding="utf-8").splitlines():
        match = SUM_LINE_RE.fullmatch(line)
        if match is None:
            raise PackageError("malformed SHA256SUMS line")
        digest, name = match.groups()
        if name == CHECKSUM_NAME or name in listed:
            raise PackageError("duplicate/self SHA256SUMS entry")
        listed[name] = digest
    if set(listed) != set(payload_names):
        raise PackageError("SHA256SUMS entries are not closed")
    for name, digest in listed.items():
        if _sha256(directory / name) != digest:
            raise PackageError(f"SHA-256 mismatch for {name}")


def _write_sums(directory: Path, names: tuple[str, ...]) -> None:
    (directory / CHECKSUM_NAME).write_text(
        "\n".join(f"{_sha256(directory/name)}  {name}" for name in sorted(names)) + "\n",
        encoding="utf-8",
    )


def verify_source(source: Path) -> dict:
    if not source.is_dir():
        raise PackageError("2000-character source pack not found")
    verify_checksum_directory(source, SOURCE_PAYLOADS)
    rows = load_selection_csv(source / "selection-2000.csv", 2000)
    row_by_cp = {int(row["codepoint_int"]): row for row in rows}
    charset = [
        line
        for line in (source / "charset-2000.txt").read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    if charset != [str(row["character"]) for row in rows]:
        raise PackageError("2000-character selection/charset mismatch")

    catalog_path = source / "stroke_cat.bin"
    catalog_blob = catalog_path.read_bytes()
    catalog = validate_catalog(catalog_blob)
    if catalog["character_count"] != 2000 or catalog["shard_count"] != SHARD_COUNT:
        raise PackageError("SCB1 is not the fixed 2000/8 catalog")
    catalog_by_cp = {entry.codepoint: entry for entry in catalog["entries"]}
    if set(catalog_by_cp) != set(row_by_cp):
        raise PackageError("selection and SCB1 character sets differ")

    seen = set()
    total = 0
    shard_runtime = []
    for index, shard in enumerate(catalog["shards"]):
        source_name = f"so{index:02d}.sob1"
        blob = (source / source_name).read_bytes()
        first_rank = index * 250 + 1
        last_rank = first_rank + 249
        if (
            shard.name != f"so{index:02d}.bin"
            or len(blob) != shard.size
            or crc32(blob) != shard.crc32
            or shard.character_count != 250
            or shard.first_rank != first_rank
            or shard.last_rank != last_rank
        ):
            raise PackageError(f"SCB1 shard descriptor mismatch for {source_name}")
        parsed = validate_blob(blob, load_all=True)
        if parsed["char_count"] != shard.character_count:
            raise PackageError("SOB1 shard count mismatch")
        for local, item in enumerate(parsed["characters"]):
            cp = int(item["codepoint"])
            if cp in seen:
                raise PackageError("character appears in multiple SOB1 shards")
            seen.add(cp)
            entry = catalog_by_cp.get(cp)
            row = row_by_cp.get(cp)
            if (
                entry is None
                or row is None
                or entry.rank != int(row["rank"])
                or entry.shard != index
                or entry.local_index != local
                or index != (int(row["rank"]) - 1) // 250
            ):
                raise PackageError("selection/SCB1/SOB1 mapping mismatch")
        shard_runtime.append(
            {
                "character_count": 250,
                "crc32": f"{crc32(blob):08x}",
                "first_rank": first_rank,
                "last_rank": last_rank,
                "runtime_name": shard.name,
                "sha256": _sha256(source / source_name),
                "size": len(blob),
                "source_name": source_name,
            }
        )
        total += len(blob)
    if seen != set(row_by_cp) or total != catalog["total_shard_bytes"]:
        raise PackageError("SOB1 shard coverage/size mismatch")

    pinyin_path = source / "stroke_pinyin.spy1"
    pinyin_blob = pinyin_path.read_bytes()
    spy = validate_spy1(pinyin_blob, load_all=True)
    if spy["char_count"] != 2000:
        raise PackageError("SPY1 is not 2000 characters")
    spy_by_cp = {int(item["codepoint"]): int(item["rank"]) for item in spy["characters"]}
    if spy_by_cp != {cp: int(row["rank"]) for cp, row in row_by_cp.items()}:
        raise PackageError("SPY1 character/rank mapping differs from selection")

    coverage = json.loads((source / "stroke_order.cov.json").read_text(encoding="utf-8"))
    coverage_records = coverage.get("records", [])
    coverage_pairs = {
        (ord(str(item["character"])), int(item["official_rank"]))
        for item in coverage_records
        if item.get("status") == "ok"
    }
    if (
        not coverage.get("complete")
        or coverage.get("ok_count") != 2000
        or coverage.get("error_count") != 0
        or len(coverage_records) != 2000
        or coverage_pairs != {(cp, int(row["rank"])) for cp, row in row_by_cp.items()}
        or any(
            int(item.get("shard", -1)) != (int(item.get("official_rank", 0)) - 1) // 250
            for item in coverage_records
        )
    ):
        raise PackageError("coverage does not exactly match the 2000-character selection")

    max_group = max(len(group) for group in spy["groups"])
    pinyin_coverage = json.loads(
        (source / "stroke_pinyin.cov.json").read_text(encoding="utf-8")
    )
    if pinyin_coverage != {
        "character_count": 2000,
        "complete": True,
        "group_count": spy["group_count"],
        "max_group_members": max_group,
        "not_a_release_library": True,
        "ok_count": 2000,
        "prototype": True,
    }:
        raise PackageError("SPY1 coverage summary does not match the binary")

    manifest = json.loads((source / "runtime.json").read_text(encoding="utf-8"))
    expected_catalog = {
        "file": "stroke_cat.bin",
        "format": "SCB1",
        "sha256": hashlib.sha256(catalog_blob).hexdigest(),
        "size": len(catalog_blob),
        "version": 1,
    }
    expected_pinyin = {
        "file": "stroke_pinyin.bin",
        "format": "SPY1",
        "group_count": spy["group_count"],
        "max_group_members": max_group,
        "sha256": hashlib.sha256(pinyin_blob).hexdigest(),
        "size": len(pinyin_blob),
        "version": 1,
    }
    if (
        manifest.get("catalog") != expected_catalog
        or manifest.get("character_count") != 2000
        or not manifest.get("prototype")
        or not manifest.get("not_a_release_library")
        or not manifest.get("not_commercially_reviewed")
        or not manifest.get("not_official_certification")
        or manifest.get("pinyin") != expected_pinyin
        or manifest.get("shards") != shard_runtime
        or manifest.get("total_shard_bytes") != total
    ):
        raise PackageError("runtime manifest mismatch or missing prototype boundary")
    return {"catalog_bytes": catalog["size"], "character_count": 2000, "shard_bytes": total}


def package_corpus(source_dir: str, output_dir: str) -> dict:
    source = Path(source_dir)
    info = verify_source(source)
    out = Path(output_dir)
    out.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{out.name}.staging-", dir=out.parent))
    backup = out.parent / f".{out.name}.backup-{uuid.uuid4().hex}"
    moved = False
    published = False
    try:
        destinations = tuple(COPY_MAP.values())
        if len(destinations) != len(set(destinations)):
            raise PackageError("runtime asset basename collision")
        for src_name, dst_name in COPY_MAP.items():
            if len(dst_name.encode("utf-8")) > ASSET_NAME_MAX:
                raise PackageError(f"runtime basename too long: {dst_name}")
            shutil.copyfile(source / src_name, staging / dst_name)
        _write_sums(staging, destinations)
        verify_checksum_directory(staging, destinations)
        if out.exists():
            os.replace(out, backup)
            moved = True
        try:
            os.replace(staging, out)
            published = True
        except Exception:
            if moved and not out.exists():
                os.replace(backup, out)
                moved = False
            raise
        if moved:
            shutil.rmtree(backup)
            moved = False
        info["output_dir"] = str(out)
        info["runtime_file_count"] = len(destinations) + 1
        return info
    finally:
        if staging.exists():
            shutil.rmtree(staging, ignore_errors=True)
        if moved and not published and backup.exists() and not out.exists():
            os.replace(backup, out)
        elif backup.exists():
            shutil.rmtree(backup, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Package sharded 2000-character assets")
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args(argv)
    try:
        result = package_corpus(args.source_dir, args.output_dir)
    except (OSError, PackageError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
