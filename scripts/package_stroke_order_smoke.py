#!/usr/bin/env python3
"""Package the reviewed 一/人/口 smoke corpus into SOB1 extra-files.

Offline only. Does not access the network, does not import all.json, and does
not claim the three-character fixture is a release 字库.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from stroke_order.constants import (  # noqa: E402
    DEFAULT_SOURCE_REPO,
    DICTIONARY_LICENSE_NOTE,
    FLATTEN_MAX_DEPTH,
    FLATTEN_TOLERANCE,
    FORMAT_VERSION,
    GRAPHICS_LICENSE,
    SOURCE_PROJECT,
    SOURCE_UPSTREAM,
)
from stroke_order.convert import ConvertError, load_character_json, pack_characters  # noqa: E402
from stroke_order.unpack import validate_blob  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SMOKE_ROOT = ROOT / "scripts/tests/fixtures/stroke_order/upstream_smoke"
UPSTREAM_COMMIT = "68d10a4b21150cae5e1ebbd223eed289cf32d90c"
SMOKE_HASHES = {
    "一": "ed728673d86fb9aa559c5b150e59ffbee666b085d4a55597c524079a91ebc8ce",
    "人": "18ffb9fb727576b0c3e7dc44914be7ffd43164de9ba121710824b83f27bd3bb1",
    "口": "ac208865e3166fc35214cec326f00a4b6788a8cac9b4c5a6a37745077e32ddab",
}
SMOKE_AGGREGATE = "c0f0eb725e23c91029ed59a5955c077fc4a1643606d21f9a935505b2088d78d6"
LICENSE_SHA256 = "5590533436c70f10f2f524ee61456238c290175c6662fbe1c700b5f038a6d328"


def _build_smoke_corpus(output_dir: Path) -> dict:
    out = output_dir
    out.mkdir(parents=True, exist_ok=False)

    license_path = SMOKE_ROOT / "ARPHICPL.TXT"
    license_digest = hashlib.sha256(license_path.read_bytes()).hexdigest()
    if license_digest != LICENSE_SHA256:
        raise ConvertError("ARPHICPL.TXT digest mismatch; refusing to package")

    characters = []
    aggregate_lines = []
    for char in sorted(SMOKE_HASHES, key=ord):
        path = SMOKE_ROOT / "data" / f"{char}.json"
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != SMOKE_HASHES[char]:
            raise ConvertError(f"smoke JSON digest mismatch for {char}")
        aggregate_lines.append(f"U+{ord(char):04X}\0data/{char}.json\0{digest}\n")
        characters.append(load_character_json(SMOKE_ROOT, char, SMOKE_ROOT))

    aggregate = hashlib.sha256("".join(aggregate_lines).encode("utf-8")).hexdigest()
    if aggregate != SMOKE_AGGREGATE:
        raise ConvertError("smoke aggregate digest mismatch")

    blob = pack_characters(characters)
    parsed = validate_blob(blob, load_all=True)
    codepoints = [item["codepoint"] for item in parsed["characters"]]
    if codepoints != [0x4E00, 0x4EBA, 0x53E3]:
        raise ConvertError("packaged smoke corpus is not 一/人/口")

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
    manifest = {
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
            "script": "scripts/package_stroke_order_smoke.py",
            "y_axis": "flip_to_y_down",
        },
        "format": "stroke_order_bin",
        "format_version": FORMAT_VERSION,
        "not_a_release_library": True,
        "sha256_bin": hashlib.sha256(blob).hexdigest(),
        "source": {
            "commit": UPSTREAM_COMMIT,
            "dictionary_license_note": DICTIONARY_LICENSE_NOTE,
            "files": source_files,
            "graphics_license": GRAPHICS_LICENSE,
            "packaging": "scripts/tests/fixtures/stroke_order/upstream_smoke",
            "project": SOURCE_PROJECT,
            "repo": DEFAULT_SOURCE_REPO,
            "selected_files_sha256": SMOKE_AGGREGATE,
            "upstream": SOURCE_UPSTREAM,
            "verbatim_fixture": True,
        },
        "usage": (
            "Three-character CoreS3 smoke corpus (一/人/口). Not a published 字库. "
            "Converted graphics are a modified form of Arphic-licensed outlines "
            "and medians: curves flattened, coordinates rounded, y flipped."
        ),
    }

    bin_path = out / "stroke_order.bin"
    manifest_path = out / "stroke_order.manifest.json"
    bin_path.write_bytes(blob)
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    shutil.copyfile(license_path, out / "ARPHICPL.TXT")
    notice = (SMOKE_ROOT / "NOTICE.md").read_text(encoding="utf-8")
    packaged_notice = (
        notice
        + "\n## Device packaging\n\n"
        + "This extra-files copy is generated offline from the reviewed fixture. "
        + "It is a three-character smoke corpus, not a 500-character or release 字库.\n"
    )
    (out / "NOTICE.md").write_text(packaged_notice, encoding="utf-8")
    return {
        "bin_path": str(bin_path),
        "manifest_path": str(manifest_path),
        "blob": blob,
        "manifest": manifest,
        "characters": characters,
    }


def package_smoke_corpus(output_dir: str) -> dict:
    """Build all four files in staging, then atomically publish the directory."""
    out = Path(output_dir)
    out.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{out.name}.staging-", dir=out.parent))
    staging.rmdir()  # _build_smoke_corpus creates and owns the complete directory.
    backup = out.parent / f".{out.name}.backup-{uuid.uuid4().hex}"
    moved_old = False
    published = False
    try:
        result = _build_smoke_corpus(staging)
        required = (
            "stroke_order.bin",
            "stroke_order.manifest.json",
            "ARPHICPL.TXT",
            "NOTICE.md",
        )
        if any(not (staging / name).is_file() for name in required):
            raise ConvertError("incomplete staged stroke-order package")
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
        result["bin_path"] = str(out / "stroke_order.bin")
        result["manifest_path"] = str(out / "stroke_order.manifest.json")
        return result
    finally:
        if staging.exists():
            shutil.rmtree(staging, ignore_errors=True)
        if moved_old and not published and backup.exists() and not out.exists():
            os.replace(backup, out)
        elif backup.exists():
            shutil.rmtree(backup, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Package the 一/人/口 smoke corpus for CoreS3 extra-files"
    )
    parser.add_argument("--output-dir", required=True, help="Directory for extra-files output")
    args = parser.parse_args(argv)
    try:
        result = package_smoke_corpus(args.output_dir)
    except (ConvertError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(result["bin_path"])
    print(result["manifest_path"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
