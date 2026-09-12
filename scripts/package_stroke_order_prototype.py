#!/usr/bin/env python3
"""Copy a reviewed 500-character prototype pack into firmware extra-files.

Offline only. Does not convert, download, or read all.json. Firmware builds
must consume the pre-generated fixture pack.
"""

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

ASSET_NAME_MAX = 31
CHECKSUM_NAME = "SHA256SUMS"
SUM_LINE_RE = re.compile(r"^([0-9a-f]{64})  ([^/\\]+)$")
REQUIRED_SOURCES = (
    "ARPHICPL.TXT",
    "NOTICE.md",
    "SHA256SUMS",
    "UNICODE-LICENSE.txt",
    "charset-500.txt",
    "selection-500.csv",
    "stroke_order.cov.json",
    "stroke_order.manifest.json",
    "stroke_order.sob1",
    "stroke_order.src.json",
    "stroke_pinyin.cov.json",
    "stroke_pinyin.manifest.json",
    "stroke_pinyin.spy1",
    "stroke_pinyin.src.json",
)
COPY_MAP = {
    "stroke_order.sob1": "stroke_order.bin",
    "stroke_pinyin.spy1": "stroke_pinyin.bin",
    "stroke_order.manifest.json": "stroke_order.manifest.json",
    "stroke_pinyin.manifest.json": "stroke_pinyin.manifest.json",
    "stroke_order.src.json": "stroke_order.src.json",
    "stroke_pinyin.src.json": "stroke_pinyin.src.json",
    "stroke_order.cov.json": "stroke_order.cov.json",
    "stroke_pinyin.cov.json": "stroke_pinyin.cov.json",
    "selection-500.csv": "selection-500.csv",
    "charset-500.txt": "charset-500.txt",
    "SHA256SUMS": "SHA256SUMS",
    "ARPHICPL.TXT": "ARPHICPL.TXT",
    "UNICODE-LICENSE.txt": "UNICODE-LICENSE.txt",
    "NOTICE.md": "NOTICE.md",
}


class PackageError(ValueError):
    """Raised when the reviewed prototype pack is incomplete or altered."""


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_checksum_directory(directory: Path, payload_names: tuple[str, ...]) -> None:
    """Verify one non-recursive directory with a non-self-referential sums file."""
    if len(payload_names) != len(set(payload_names)) or CHECKSUM_NAME in payload_names:
        raise PackageError("invalid checksum payload name set")
    expected_files = set(payload_names) | {CHECKSUM_NAME}
    actual_files = {path.name for path in directory.iterdir()}
    if actual_files != expected_files:
        missing = sorted(expected_files - actual_files)
        extra = sorted(actual_files - expected_files)
        raise PackageError(f"checksum directory mismatch: missing={missing}, extra={extra}")
    if any(
        not (directory / name).is_file() or (directory / name).is_symlink()
        for name in expected_files
    ):
        raise PackageError("checksum directory contains a non-regular payload")

    sums_path = directory / CHECKSUM_NAME
    listed: dict[str, str] = {}
    for line in sums_path.read_text(encoding="utf-8").splitlines():
        if not line:
            continue
        match = SUM_LINE_RE.fullmatch(line)
        if match is None:
            raise PackageError("malformed SHA256SUMS line")
        digest, name = match.groups()
        if name == CHECKSUM_NAME:
            raise PackageError("SHA256SUMS must not hash itself")
        if name in listed:
            raise PackageError(f"duplicate SHA256SUMS entry: {name}")
        listed[name] = digest
    if set(listed) != set(payload_names):
        missing = sorted(set(payload_names) - set(listed))
        extra = sorted(set(listed) - set(payload_names))
        raise PackageError(f"SHA256SUMS entries mismatch: missing={missing}, extra={extra}")
    for name, digest in listed.items():
        if _sha256(directory / name) != digest:
            raise PackageError(f"SHA-256 mismatch for {name}")


def _write_checksum_file(directory: Path, payload_names: tuple[str, ...]) -> None:
    lines = [f"{_sha256(directory / name)}  {name}" for name in sorted(payload_names)]
    (directory / CHECKSUM_NAME).write_text("\n".join(lines) + "\n", encoding="utf-8")


def verify_prototype_pack(source_dir: Path) -> dict:
    if not source_dir.is_dir():
        raise PackageError("prototype pack directory not found")
    source_payloads = tuple(name for name in REQUIRED_SOURCES if name != CHECKSUM_NAME)
    verify_checksum_directory(source_dir, source_payloads)

    sob1 = (source_dir / "stroke_order.sob1").read_bytes()
    spy1 = (source_dir / "stroke_pinyin.spy1").read_bytes()
    if sob1[:4] != b"SOB1" or spy1[:4] != b"SPY1":
        raise PackageError("prototype magic mismatch")
    if len(sob1) > 1024 * 1024 or len(spy1) > 64 * 1024:
        raise PackageError("prototype exceeds format limits")

    for key in ("stroke_order.manifest.json", "stroke_pinyin.manifest.json", "NOTICE.md"):
        text = (source_dir / key).read_text(encoding="utf-8")
        if "not_a_release_library" not in text and "not a release" not in text.lower():
            raise PackageError(f"{key} is missing prototype/not-release marking")
        if "prototype" not in text.lower() and "Prototype" not in text:
            raise PackageError(f"{key} is missing prototype marking")

    order_manifest = json.loads((source_dir / "stroke_order.manifest.json").read_text(encoding="utf-8"))
    pinyin_manifest = json.loads((source_dir / "stroke_pinyin.manifest.json").read_text(encoding="utf-8"))
    if order_manifest.get("character_count") != 500 or pinyin_manifest.get("character_count") != 500:
        raise PackageError("prototype is not a 500-character pack")
    if not order_manifest.get("not_a_release_library") or not pinyin_manifest.get("not_a_release_library"):
        raise PackageError("prototype must remain not_a_release_library")
    if order_manifest.get("sha256_bin") != hashlib.sha256(sob1).hexdigest():
        raise PackageError("SOB1 manifest hash mismatch")
    if pinyin_manifest.get("sha256_bin") != hashlib.sha256(spy1).hexdigest():
        raise PackageError("SPY1 manifest hash mismatch")

    dest_names = list(COPY_MAP.values())
    if len(dest_names) != len(set(dest_names)):
        raise PackageError("asset basename collision")
    for name in dest_names:
        if len(name) > ASSET_NAME_MAX:
            raise PackageError(f"{name} exceeds 31-character asset name limit")
    return {
        "sob1_bytes": len(sob1),
        "spy1_bytes": len(spy1),
        "character_count": 500,
    }


def package_prototype_corpus(source_dir: str, output_dir: str) -> dict:
    source = Path(source_dir)
    info = verify_prototype_pack(source)
    out = Path(output_dir)
    out.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{out.name}.staging-", dir=out.parent))
    backup = out.parent / f".{out.name}.backup-{uuid.uuid4().hex}"
    moved_old = False
    published = False
    try:
        for src_name, dest_name in COPY_MAP.items():
            if src_name != CHECKSUM_NAME:
                shutil.copyfile(source / src_name, staging / dest_name)
        target_payloads = tuple(
            dest_name for src_name, dest_name in COPY_MAP.items() if src_name != CHECKSUM_NAME
        )
        _write_checksum_file(staging, target_payloads)
        required = tuple(COPY_MAP.values())
        if any(not (staging / name).is_file() for name in required):
            raise PackageError("incomplete staged prototype extra-files")
        verify_checksum_directory(staging, target_payloads)
        if out.exists():
            os.replace(out, backup)
            moved_old = True
        try:
            os.replace(staging, out)
            published = True
        except Exception:
            if moved_old and not out.exists():
                os.replace(backup, out)
                moved_old = False
            raise
        if moved_old:
            shutil.rmtree(backup)
            moved_old = False
        info["output_dir"] = str(out)
        return info
    finally:
        if staging.exists():
            shutil.rmtree(staging, ignore_errors=True)
        if moved_old and not published and backup.exists() and not out.exists():
            os.replace(backup, out)
        elif backup.exists():
            shutil.rmtree(backup, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Package reviewed 500-character extra-files")
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args(argv)
    try:
        result = package_prototype_corpus(args.source_dir, args.output_dir)
    except (PackageError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(result["output_dir"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
