#!/usr/bin/env python3
"""Generate the offline, sharded 2000-character 笔划 prototype corpus."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path
from typing import List

sys.path.insert(0, str(Path(__file__).resolve().parent))

from generate_stroke_order_500 import (  # noqa: E402
    APL_SHA256,
    HWD_COMMIT,
    MODIFICATION_DATE,
    UNICODE_LICENSE_SHA256,
    UNIHAN_ZIP_BYTES,
    UNIHAN_ZIP_SHA256,
    _require_hash,
)
from stroke_order.catalog import CatalogEntry, ShardInfo, crc32, pack_catalog, validate_catalog  # noqa: E402
from stroke_order.constants import DEFAULT_SOURCE_REPO, MAX_FILE_BYTES  # noqa: E402
from stroke_order.convert import ConvertError, convert_dataset, report_charset_coverage  # noqa: E402
from stroke_order.pinyin import (  # noqa: E402
    PinyinError,
    build_character_readings,
    build_pinyin_manifest,
    load_unihan_readings,
    pack_spy1,
    validate_spy1,
)
from stroke_order.pinyin_constants import (  # noqa: E402
    MAX_CHARACTERS as SPY1_MAX_CHARACTERS,
    MAX_FILE_BYTES as SPY1_MAX_FILE_BYTES,
    MAX_GROUP_MEMBERS,
    MAX_GROUPS,
)
from stroke_order.select import (  # noqa: E402
    OFFICIAL_PAGE,
    OFFICIAL_PDF,
    OFFICIAL_PDF_BYTES,
    OFFICIAL_PDF_SHA256,
    TRANSCRIPTION_COMMIT,
    TRANSCRIPTION_REPO,
    SelectionError,
    official_source_lock,
    select_official_2000,
    write_charset,
    write_selection_csv,
)
from stroke_order.unpack import validate_blob  # noqa: E402

CHARACTER_COUNT = 2000
SHARD_COUNT = 8
SHARD_CHARACTERS = 250
EXPECTED_SHARD_SIZES = [443020, 603800, 680236, 723504, 771996, 816964, 849284, 911688]
RUNTIME_SHARD_NAMES = [f"so{index:02d}.bin" for index in range(SHARD_COUNT)]


class GenerateError(ValueError):
    pass


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _dump(path: Path, payload: object) -> None:
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _write_sums(directory: Path, names: List[str]) -> None:
    lines = [f"{_sha256(directory / name)}  {name}" for name in sorted(names)]
    (directory / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")


def generate_2000(
    *,
    table_json: str,
    hanzi_writer_data: str,
    unihan_zip: str,
    unicode_license: str,
    official_pdf: str,
    output_dir: str,
    source_commit: str = HWD_COMMIT,
    source_repo: str = DEFAULT_SOURCE_REPO,
) -> dict:
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
    unicode_path = Path(unicode_license)
    _require_hash(unicode_path, UNICODE_LICENSE_SHA256, "Unicode license")

    rows = select_official_2000(table_json)
    write_selection_csv(rows, out / "selection-2000.csv", CHARACTER_COUNT)
    write_charset(rows, out / "charset-2000.txt", CHARACTER_COUNT)

    entries: List[CatalogEntry] = []
    shards: List[ShardInfo] = []
    shard_audit = []
    coverage_records = []
    total_ok = 0
    for shard_index in range(SHARD_COUNT):
        first = shard_index * SHARD_CHARACTERS
        shard_rows = rows[first : first + SHARD_CHARACTERS]
        charset_path = out / f".charset-{shard_index}.txt"
        charset_path.write_text(
            "# Deterministic 250-character rank shard.\n"
            + "\n".join(str(item["character"]) for item in shard_rows)
            + "\n",
            encoding="utf-8",
        )
        coverage = report_charset_coverage(
            hanzi_writer_data=hanzi_writer_data,
            charset_path=str(charset_path),
            source_commit=source_commit,
            source_repo=source_repo,
        )
        if not coverage["complete"] or coverage["ok_count"] != SHARD_CHARACTERS:
            raise GenerateError(
                f"HWD shard {shard_index} coverage is not 250/250: "
                + json.dumps(coverage["errors"], ensure_ascii=False)
            )
        for local, record in enumerate(coverage["records"]):
            audit_record = {
                key: value for key, value in record.items() if key != "character_data"
            }
            audit_record["official_rank"] = first + local + 1
            audit_record["shard"] = shard_index
            coverage_records.append(audit_record)
        staging = out / f".shard-{shard_index}"
        converted = convert_dataset(
            hanzi_writer_data=hanzi_writer_data,
            charset_path=str(charset_path),
            source_commit=source_commit,
            output_dir=str(staging),
            source_repo=source_repo,
        )
        blob = converted["blob"]
        expected_size = EXPECTED_SHARD_SIZES[shard_index]
        if len(blob) != expected_size or len(blob) > MAX_FILE_BYTES:
            raise GenerateError(
                f"SOB1 shard {shard_index} size {len(blob)} != pinned {expected_size}"
            )
        parsed = validate_blob(blob, load_all=True)
        if parsed["char_count"] != SHARD_CHARACTERS:
            raise GenerateError("SOB1 shard character count mismatch")
        source_name = f"so{shard_index:02d}.sob1"
        (out / source_name).write_bytes(blob)
        manifest = converted["manifest"]
        manifest.update(
            {
                "first_official_rank": first + 1,
                "last_official_rank": first + SHARD_CHARACTERS,
                "not_commercially_reviewed": True,
                "not_official_certification": True,
                "prototype": True,
                "shard_index": shard_index,
                "shard_count": SHARD_COUNT,
            }
        )
        _dump(out / f"so{shard_index:02d}.manifest.json", manifest)
        shards.append(
            ShardInfo(
                RUNTIME_SHARD_NAMES[shard_index],
                len(blob),
                crc32(blob),
                SHARD_CHARACTERS,
                first + 1,
                first + SHARD_CHARACTERS,
            )
        )
        sorted_rows = sorted(shard_rows, key=lambda item: int(item["codepoint_int"]))
        local_by_cp = {
            int(item["codepoint_int"]): local for local, item in enumerate(sorted_rows)
        }
        for row in shard_rows:
            cp = int(row["codepoint_int"])
            entries.append(CatalogEntry(cp, int(row["rank"]), shard_index, local_by_cp[cp]))
        shard_audit.append(
            {
                "character_count": SHARD_CHARACTERS,
                "crc32": f"{crc32(blob):08x}",
                "first_rank": first + 1,
                "last_rank": first + SHARD_CHARACTERS,
                "runtime_name": RUNTIME_SHARD_NAMES[shard_index],
                "sha256": hashlib.sha256(blob).hexdigest(),
                "size": len(blob),
                "source_name": source_name,
            }
        )
        total_ok += int(coverage["ok_count"])
        shutil.rmtree(staging)
        charset_path.unlink()

    entries.sort(key=lambda item: item.codepoint)
    catalog = pack_catalog(entries, shards)
    catalog_info = validate_catalog(catalog)
    if catalog_info["character_count"] != CHARACTER_COUNT:
        raise GenerateError("SCB1 character count mismatch")
    (out / "stroke_cat.bin").write_bytes(catalog)

    needed = [int(row["codepoint_int"]) for row in rows]
    readings = build_character_readings(rows, load_unihan_readings(str(zip_path), needed))
    spy1 = pack_spy1(readings)
    parsed_spy1 = validate_spy1(spy1, load_all=True)
    if parsed_spy1["char_count"] != CHARACTER_COUNT:
        raise GenerateError("SPY1 character count mismatch")
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
    pinyin_manifest = build_pinyin_manifest(readings, spy1, pinyin_source)
    _dump(out / "stroke_pinyin.manifest.json", pinyin_manifest)

    source_lock = official_source_lock(Path(table_json), pdf, CHARACTER_COUNT)
    source_lock["hanzi_writer_data"] = {
        "commit": source_commit,
        "graphics_license": "Arphic Public License",
        "repo": source_repo,
        "upstream": "skishore/makemeahanzi",
    }
    source_lock["official_page"] = OFFICIAL_PAGE
    source_lock["official_pdf"] = OFFICIAL_PDF
    _dump(out / "stroke_order.src.json", source_lock)
    _dump(
        out / "stroke_order.cov.json",
        {
            "character_count": CHARACTER_COUNT,
            "complete": True,
            "error_count": 0,
            "errors": [],
            "not_a_release_library": True,
            "not_official_certification": True,
            "ok_count": total_ok,
            "prototype": True,
            "records": coverage_records,
            "shards": shard_audit,
            "source": {"commit": source_commit, "repo": source_repo},
        },
    )
    _dump(
        out / "stroke_pinyin.cov.json",
        {
            "character_count": CHARACTER_COUNT,
            "complete": True,
            "group_count": parsed_spy1["group_count"],
            "max_group_members": max(len(group) for group in parsed_spy1["groups"]),
            "not_a_release_library": True,
            "ok_count": CHARACTER_COUNT,
            "prototype": True,
        },
    )
    runtime_manifest = {
        "catalog": {
            "file": "stroke_cat.bin",
            "format": "SCB1",
            "sha256": hashlib.sha256(catalog).hexdigest(),
            "size": len(catalog),
            "version": 1,
        },
        "character_count": CHARACTER_COUNT,
        "not_a_release_library": True,
        "not_commercially_reviewed": True,
        "not_official_certification": True,
        "pinyin": {
            "file": "stroke_pinyin.bin",
            "format": "SPY1",
            "group_count": parsed_spy1["group_count"],
            "max_group_members": max(len(group) for group in parsed_spy1["groups"]),
            "sha256": hashlib.sha256(spy1).hexdigest(),
            "size": len(spy1),
            "version": 1,
        },
        "prototype": True,
        "shards": shard_audit,
        "total_shard_bytes": sum(EXPECTED_SHARD_SIZES),
    }
    _dump(out / "runtime.json", runtime_manifest)

    hwd_root = Path(hanzi_writer_data)
    apl = hwd_root / "ARPHICPL.TXT"
    if not apl.is_file():
        apl = hwd_root.parent / "ARPHICPL.TXT"
    _require_hash(apl, APL_SHA256, "ARPHICPL.TXT")
    shutil.copyfile(apl, out / "ARPHICPL.TXT")
    shutil.copyfile(unicode_path, out / "UNICODE-LICENSE.txt")
    notice = f"""# 2000-character sharded 笔划 technical prototype

This is a prototype, not a release 字库, not official per-character certification,
and not commercially reviewed. Membership is 一级字表 0001-2000 from
《通用规范汉字表》 (国发〔2013〕23号). The official PDF SHA-256 is
`{OFFICIAL_PDF_SHA256}` ({OFFICIAL_PDF_BYTES} bytes).

The pinned transcription aid is `{TRANSCRIPTION_REPO}` commit
`{TRANSCRIPTION_COMMIT}`. It has no explicit in-tree license and is not an
official digital annex or commercial authorization source. Dual-person
page-by-page PDF verification has not been completed.

Stroke graphics use only strokes/medians from chanind/hanzi-writer-data commit
`{source_commit}` (upstream skishore/makemeahanzi), converted on
{MODIFICATION_DATE}. The complete Arphic Public License is ARPHICPL.TXT.
The corpus is split by official rank into eight deterministic 250-character
SOB1 v1 shards; the device never copies the complete {sum(EXPECTED_SHARD_SIZES)}-byte
shard corpus into PSRAM. SCB1 v1 maps codepoint/rank to a shard and local index.

Pinyin uses Unicode 16.0.0 Unihan kMandarin plus bounded kHanyuPinyin union;
UNICODE-LICENSE.txt applies. Measured SPY1 v1 is {len(spy1)} bytes with
{parsed_spy1['group_count']} groups and maximum group size
{max(len(group) for group in parsed_spy1['groups'])}; no homophone group is
truncated.

Commercial release still requires APL/transcription/Unicode legal review and
manual stroke-order accuracy review. No official per-character certification is
claimed.
"""
    (out / "NOTICE.md").write_text(notice, encoding="utf-8")

    names = [
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
    ]
    names += [f"so{index:02d}.sob1" for index in range(SHARD_COUNT)]
    names += [f"so{index:02d}.manifest.json" for index in range(SHARD_COUNT)]
    _write_sums(out, names)
    return {
        "catalog_bytes": len(catalog),
        "catalog_sha256": hashlib.sha256(catalog).hexdigest(),
        "character_count": CHARACTER_COUNT,
        "output_dir": str(out),
        "shard_sizes": EXPECTED_SHARD_SIZES,
        "spy1_bytes": len(spy1),
        "spy1_sha256": hashlib.sha256(spy1).hexdigest(),
        "total_shard_bytes": sum(EXPECTED_SHARD_SIZES),
    }


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate sharded 2000-character prototype")
    parser.add_argument("--table-json", required=True)
    parser.add_argument("--hanzi-writer-data", required=True)
    parser.add_argument("--unihan-zip", required=True)
    parser.add_argument("--unicode-license", required=True)
    parser.add_argument("--official-pdf", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--source-commit", default=HWD_COMMIT)
    args = parser.parse_args(argv)
    try:
        result = generate_2000(
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
