#!/usr/bin/env python3
"""Offline generator for the 500-character prototype pack.

Never accesses the network. Fails closed if official 0001-0500 selection or
hanzi-writer-data coverage is not 500/500. Does not fill from 0501.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Dict, List

sys.path.insert(0, str(Path(__file__).resolve().parent))

from stroke_order.constants import (  # noqa: E402
    DEFAULT_SOURCE_REPO,
    DICTIONARY_LICENSE_NOTE,
    GRAPHICS_LICENSE,
    MAX_CHARACTER_BYTES,
    MAX_FILE_BYTES,
    SOURCE_PROJECT,
    SOURCE_UPSTREAM,
)
from stroke_order.convert import ConvertError, convert_dataset, report_charset_coverage  # noqa: E402
from stroke_order.pinyin import (  # noqa: E402
    PinyinError,
    build_character_readings,
    build_pinyin_manifest,
    load_unihan_readings,
    pack_spy1,
    validate_spy1,
)
from stroke_order.pinyin_constants import MAX_FILE_BYTES as SPY1_MAX_FILE_BYTES  # noqa: E402
from stroke_order.select import (  # noqa: E402
    OFFICIAL_PAGE,
    OFFICIAL_PDF,
    OFFICIAL_PDF_BYTES,
    OFFICIAL_PDF_SHA256,
    SelectionError,
    TRANSCRIPTION_COMMIT,
    TRANSCRIPTION_REPO,
    official_source_lock,
    select_official_500,
    write_charset,
    write_selection_csv,
)
from stroke_order.unpack import validate_blob  # noqa: E402

HWD_COMMIT = "68d10a4b21150cae5e1ebbd223eed289cf32d90c"
UNIHAN_ZIP_SHA256 = "b8f000df69de7828d21326a2ffea462b04bc7560022989f7cc704f10521ef3e0"
UNIHAN_ZIP_BYTES = 8382485
UNICODE_LICENSE_SHA256 = "e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96"
APL_SHA256 = "5590533436c70f10f2f524ee61456238c290175c6662fbe1c700b5f038a6d328"
ASSET_NAME_MAX = 31
MODIFICATION_DATE = "2026-09-12"
CONVERTER_VERSION = "stroke-order-converter/1"
SCHEMA_VERSION = "SOB1 v1 + SPY1 v1"


class GenerateError(ValueError):
    """Raised when the 500-character prototype pack cannot be produced."""


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _require_hash(path: Path, expected: str, label: str) -> None:
    digest = _sha256(path)
    if digest != expected:
        raise GenerateError(f"{label} SHA-256 mismatch: {digest}")


def _dump(path: Path, payload: dict) -> None:
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _write_sha256sums(directory: Path, names: List[str]) -> None:
    lines = []
    for name in names:
        digest = _sha256(directory / name)
        lines.append(f"{digest}  {name}")
    (directory / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")


def generate_prototype_500(
    *,
    table_json: str,
    hanzi_writer_data: str,
    unihan_zip: str,
    unicode_license: str,
    official_pdf: str,
    output_dir: str,
    source_commit: str = HWD_COMMIT,
    source_repo: str = DEFAULT_SOURCE_REPO,
) -> Dict[str, object]:
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)

    pdf = Path(official_pdf)
    _require_hash(pdf, OFFICIAL_PDF_SHA256, "official PDF")
    if pdf.stat().st_size != OFFICIAL_PDF_BYTES:
        raise GenerateError("official PDF size mismatch")

    zip_path = Path(unihan_zip)
    _require_hash(zip_path, UNIHAN_ZIP_SHA256, "Unihan zip")
    if zip_path.stat().st_size != UNIHAN_ZIP_BYTES:
        raise GenerateError("Unihan zip size mismatch")
    license_path = Path(unicode_license)
    _require_hash(license_path, UNICODE_LICENSE_SHA256, "Unicode license")

    rows = select_official_500(table_json)
    write_selection_csv(rows, out / "selection-500.csv")
    write_charset(rows, out / "charset-500.txt")

    coverage = report_charset_coverage(
        hanzi_writer_data=hanzi_writer_data,
        charset_path=str(out / "charset-500.txt"),
        source_commit=source_commit,
        source_repo=source_repo,
    )
    public_coverage = {
        "character_count": coverage["character_count"],
        "complete": coverage["complete"],
        "error_count": coverage["error_count"],
        "errors": coverage["errors"],
        "not_a_release_library": True,
        "not_official_certification": True,
        "ok_count": coverage["ok_count"],
        "prototype": True,
        "source": coverage["source"],
    }
    _dump(out / "stroke_order.cov.json", public_coverage)
    if not coverage["complete"] or coverage["ok_count"] != 500:
        raise GenerateError(
            "HWD coverage is not 500/500; refusing to substitute later ranks: "
            + json.dumps(coverage["errors"], ensure_ascii=False)
        )

    converted = convert_dataset(
        hanzi_writer_data=hanzi_writer_data,
        charset_path=str(out / "charset-500.txt"),
        source_commit=source_commit,
        output_dir=str(out / ".sob1-staging"),
        source_repo=source_repo,
    )
    blob = converted["blob"]
    if len(blob) > MAX_FILE_BYTES:
        raise GenerateError(f"SOB1 is {len(blob)} bytes; format limit is {MAX_FILE_BYTES}")
    parsed = validate_blob(blob, load_all=True)
    if parsed["char_count"] != 500:
        raise GenerateError("converted SOB1 is not 500 characters")
    for item in coverage["records"]:
        if int(item.get("record_bytes", MAX_CHARACTER_BYTES + 1)) > MAX_CHARACTER_BYTES:
            raise GenerateError("a character record exceeds SOB1 limits")

    sob1_path = out / "stroke_order.sob1"
    sob1_path.write_bytes(blob)
    manifest = converted["manifest"]
    manifest["not_official_certification"] = True
    manifest["not_commercially_reviewed"] = True
    manifest["prototype"] = True
    manifest["usage"] = (
        "Prototype 500-character SOB1 from 通用规范汉字表 一级字表 0001-0500. "
        "Not official certification, not a commercial release, and not a published 字库. "
        "Dual-person official PDF verification has not been completed."
    )
    _dump(out / "stroke_order.manifest.json", manifest)

    hwd_root = Path(hanzi_writer_data)
    apl_src = hwd_root / "ARPHICPL.TXT"
    if not apl_src.is_file():
        apl_src = hwd_root.parent / "ARPHICPL.TXT"
    # Checkout root is the git toplevel; ARPHICPL.TXT lives there.
    if not apl_src.is_file():
        raise GenerateError("ARPHICPL.TXT not found in hanzi-writer-data checkout")
    _require_hash(apl_src, APL_SHA256, "ARPHICPL.TXT")
    shutil.copyfile(apl_src, out / "ARPHICPL.TXT")
    shutil.copyfile(license_path, out / "UNICODE-LICENSE.txt")

    source_lock = official_source_lock(Path(table_json), pdf)
    source_lock["hanzi_writer_data"] = {
        "commit": source_commit,
        "dictionary_license_note": DICTIONARY_LICENSE_NOTE,
        "graphics_license": GRAPHICS_LICENSE,
        "project": SOURCE_PROJECT,
        "repo": source_repo,
        "upstream": SOURCE_UPSTREAM,
    }
    source_lock["official_page"] = OFFICIAL_PAGE
    source_lock["official_pdf"] = OFFICIAL_PDF
    _dump(out / "stroke_order.src.json", source_lock)

    needed = [int(row["codepoint_int"]) for row in rows]
    unihan = load_unihan_readings(str(zip_path), needed)
    pinyin_chars = build_character_readings(rows, unihan)
    spy1 = pack_spy1(pinyin_chars)
    if len(spy1) > SPY1_MAX_FILE_BYTES:
        raise GenerateError(f"SPY1 is {len(spy1)} bytes; format limit is {SPY1_MAX_FILE_BYTES}")
    spy1_parsed = validate_spy1(spy1, load_all=True)
    if spy1_parsed["char_count"] != 500:
        raise GenerateError("SPY1 does not cover 500 characters")
    (out / "stroke_pinyin.spy1").write_bytes(spy1)

    pinyin_source = {
        "fields": ["kMandarin", "kHanyuPinyin"],
        "file": "Unihan_Readings.txt",
        "license": "Unicode License v3",
        "license_sha256": UNICODE_LICENSE_SHA256,
        "primary": "kMandarin",
        "unicode_version": "16.0.0",
        "zip_bytes": UNIHAN_ZIP_BYTES,
        "zip_sha256": UNIHAN_ZIP_SHA256,
    }
    pinyin_manifest = build_pinyin_manifest(pinyin_chars, spy1, pinyin_source)
    _dump(out / "stroke_pinyin.manifest.json", pinyin_manifest)
    _dump(
        out / "stroke_pinyin.src.json",
        {
            "kind": "stroke_pinyin_source_lock",
            "not_a_release_library": True,
            "not_commercially_reviewed": True,
            "not_official_certification": True,
            "prototype": True,
            "source": pinyin_source,
        },
    )
    _dump(
        out / "stroke_pinyin.cov.json",
        {
            "character_count": 500,
            "complete": True,
            "kHanyuPinyin_optional": True,
            "kMandarin_required": True,
            "not_a_release_library": True,
            "ok_count": 500,
            "prototype": True,
        },
    )

    notice = f"""# 500-character 笔划 prototype notice

This directory is a **prototype**, not a release 字库, not official certification,
and not commercially reviewed.

## Character membership

Membership is the first 500 numbered entries (0001-0500) of the 一级字表 in
《通用规范汉字表》 (国发〔2013〕23号):

- page: {OFFICIAL_PAGE}
- PDF: {OFFICIAL_PDF}
- PDF SHA-256: `{OFFICIAL_PDF_SHA256}`
- PDF bytes: {OFFICIAL_PDF_BYTES}

A structured JSON checkout of `{TRANSCRIPTION_REPO}` at commit
`{TRANSCRIPTION_COMMIT}` was used only as a prototype transcription aid. That
checkout has no explicit license in-tree. It is **not** an official digital
annex and **not** a commercial authorization source. Dual-person page-by-page
official PDF verification has **not** been completed.

## Stroke graphics

Converted from `chanind/hanzi-writer-data` commit `{source_commit}`
(upstream `skishore/makemeahanzi`) using only `strokes` and `medians`. The
complete Arphic Public License is in `ARPHICPL.TXT`.

Modification notice: on `{MODIFICATION_DATE}`, converter `{CONVERTER_VERSION}`
produced schema `{SCHEMA_VERSION}` by flattening curves, rounding coordinates,
flipping y into SOB1, clamping MMAH control points that sit slightly outside
0..1024, and downsampling long outlines to stay inside the unchanged SOB1 point
and 1 MiB file limits. This reproducible date/version describes how and when the
converted files were produced; it is not a legal conclusion. Dictionary text,
`all.json`, and the full upstream checkout are not vendored.

## Pinyin

SPY1 readings come from Unicode 16.0.0 `Unihan_Readings.txt` (`kMandarin`
required, `kHanyuPinyin` union when it normalizes safely). The Unicode license
is `UNICODE-LICENSE.txt`. Device storage is group id and rank, not strings.

Do not describe this pack as officially certified or commercially releasable.
Legal review of Arphic Public License compliance is still required before any
external or commercial distribution; this notice does not provide legal advice
or an authorization conclusion.
"""
    (out / "NOTICE.md").write_text(notice, encoding="utf-8")

    hashed_names = [
        "ARPHICPL.TXT",
        "NOTICE.md",
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
    ]
    _write_sha256sums(out, hashed_names)
    staging = out / ".sob1-staging"
    if staging.exists():
        shutil.rmtree(staging)

    for name in hashed_names + ["SHA256SUMS"]:
        if len(name) > ASSET_NAME_MAX:
            raise GenerateError(f"basename {name} exceeds 31 characters")
    return {
        "output_dir": str(out),
        "sob1_bytes": len(blob),
        "spy1_bytes": len(spy1),
        "sob1_sha256": hashlib.sha256(blob).hexdigest(),
        "spy1_sha256": hashlib.sha256(spy1).hexdigest(),
        "character_count": 500,
    }


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate the 500-character prototype pack")
    parser.add_argument("--table-json", required=True)
    parser.add_argument("--hanzi-writer-data", required=True)
    parser.add_argument("--unihan-zip", required=True)
    parser.add_argument("--unicode-license", required=True)
    parser.add_argument("--official-pdf", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--source-commit", default=HWD_COMMIT)
    args = parser.parse_args(argv)
    try:
        result = generate_prototype_500(
            table_json=args.table_json,
            hanzi_writer_data=args.hanzi_writer_data,
            unihan_zip=args.unihan_zip,
            unicode_license=args.unicode_license,
            official_pdf=args.official_pdf,
            output_dir=args.output_dir,
            source_commit=args.source_commit,
        )
    except (GenerateError, ConvertError, PinyinError, SelectionError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
