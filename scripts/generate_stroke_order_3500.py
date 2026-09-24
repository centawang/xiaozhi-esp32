#!/usr/bin/env python3
"""Offline W3a host generator. New v2 artifacts only, no device integration.

Pinned table archives are accepted by exact SHA256 (not claimed clean Git).
The HWD checkout and every selected blob must be at the pinned clean commit.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile

from stroke_order.policy3500 import (
    APL_SHA256, HWD_COMMIT, UNICODE_LICENSE_SHA256, UNIHAN_ZIP_BYTES,
    UNIHAN_ZIP_SHA256, SOURCE_MANIFEST_SHA256, SOURCE_PINS,
    ASSETS_BASELINE, OLD_STROKE_BYTES, ASSETS_LIMIT, verify_generated,
    verify_source_projection, NOTICE_TEXT,
)
from stroke_order import convert, select
from stroke_order.constants import DEFAULT_SOURCE_REPO
from stroke_order.corpus2 import canonical_json, corpus_id, digest
from stroke_order.heatshrink import ReferenceCodec
from stroke_order.pinyin import load_unihan_readings, normalize_pinyin, PinyinError
from stroke_order.scb2 import pack_scb2, shard_name
from stroke_order.sob2 import encode_record, shard_blocks
from stroke_order.spy2 import build_readings, pack_spy2, validate_spy2
from stroke_order.v2 import PROFILE, require


def checked_file(path: Path, sha: str, size: int | None = None) -> bytes:
    require(path.is_file() and not path.is_symlink(), f"missing/symlink source: {path}")
    require(size is None or path.stat().st_size == size, f"source size mismatch: {path}")
    data = path.read_bytes()
    require(digest(data) == sha, f"source SHA256 mismatch: {path}")
    return data


def verify_sources(args: argparse.Namespace) -> tuple[list[dict], dict, dict]:
    checked_file(Path(args.table_json), select.TRANSCRIPTION_JSON_SHA256)
    checked_file(Path(args.official_pdf), select.OFFICIAL_PDF_SHA256, select.OFFICIAL_PDF_BYTES)
    checked_file(Path(args.unihan_zip), UNIHAN_ZIP_SHA256, UNIHAN_ZIP_BYTES)
    checked_file(Path(args.unicode_license), UNICODE_LICENSE_SHA256)
    root = Path(args.hanzi_writer_data).resolve(strict=True)
    checked_file(root / "ARPHICPL.TXT", APL_SHA256)
    convert._verify_git_checkout(root, HWD_COMMIT, DEFAULT_SOURCE_REPO)
    rows = select.load_selection_csv(Path(args.selection_csv), PROFILE)
    require(select.load_tier1_characters(args.table_json) == [r["character"] for r in rows],
            "selection is not complete official tier1")
    prefix = select.load_selection_csv(Path(__file__).parent / "tests/fixtures/stroke_order/prototype_2000/selection-2000.csv", 2000)
    require(rows[:2000] == prefix, "selection changed legacy 2000 prefix")
    manifest_data = checked_file(Path(args.source_manifest), SOURCE_MANIFEST_SHA256)
    manifest = json.loads(manifest_data)
    require(manifest["commit"] == HWD_COMMIT and manifest["origin"] == DEFAULT_SOURCE_REPO,
            "HWD source manifest identity")
    require(convert._run_git(root, "rev-parse", "HEAD^{tree}").strip() == manifest["tree"], "HWD tree mismatch")
    expected = {f"data/{r['character']}.json" for r in rows} | {"ARPHICPL.TXT"}
    entries = manifest["files"]
    require(len(entries) == PROFILE + 1 and {f["path"] for f in entries} == expected, "HWD manifest membership")
    by_path = {f["path"]: f for f in entries}
    for row in rows:
        item = by_path[f"data/{row['character']}.json"]
        require(item["rank"] == row["rank"] and item["codepoint_int"] == row["codepoint_int"],
                "HWD manifest rank/codepoint")
    for entry in entries:
        data = checked_file(root / entry["path"], entry["sha256"], entry["bytes"])
        blob_sha = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        require(blob_sha == entry["blob_sha1"], "HWD manifest Git blob mismatch")
        # Prove the manifest blob is in HEAD, not just internally consistent.
        actual = convert._run_git(root, "rev-parse", "HEAD:" + entry["path"]).strip()
        require(actual == blob_sha, "HWD manifest blob not at HEAD")
    source = dict(
        hwd_commit=HWD_COMMIT, hwd_repo=DEFAULT_SOURCE_REPO, hwd_tree=manifest["tree"],
        source_manifest_sha256=digest(manifest_data),
        selection_sha256=digest(Path(args.selection_csv).read_bytes()),
        table_commit=select.TRANSCRIPTION_COMMIT, table_sha256=select.TRANSCRIPTION_JSON_SHA256,
        table_verification="pinned archive SHA256; not a clean Git checkout claim",
        official_pdf_sha256=select.OFFICIAL_PDF_SHA256, unihan_sha256=UNIHAN_ZIP_SHA256,
        apl_sha256=APL_SHA256, unicode_license_sha256=UNICODE_LICENSE_SHA256,
    )
    require(source == SOURCE_PINS, "source policy mismatch")
    verify_source_projection([dict(rank=r["rank"], codepoint=r["codepoint_int"],
                                   source_sha256=by_path[f"data/{r['character']}.json"]["sha256"])
                              for r in rows])
    return rows, source, by_path


def generate(args: argparse.Namespace) -> dict:
    out = Path(args.output_dir).absolute()
    require(not out.exists(), "output must not already exist")
    rows, source, _manifest = verify_sources(args)
    root = Path(args.hanzi_writer_data).resolve(strict=True)
    unihan = load_unihan_readings(args.unihan_zip, [r["codepoint_int"] for r in rows])
    # Report/reject malformed supplemental tokens rather than silently discard them.
    diagnostics = []
    for cp, fields in unihan.items():
        for token in fields.get("kHanyuPinyin", "").split():
            if ":" not in token:
                diagnostics.append(dict(codepoint=cp, token=token))
                continue
            for part in token.rsplit(":", 1)[1].split(","):
                try:
                    normalize_pinyin(part)
                except PinyinError:
                    diagnostics.append(dict(codepoint=cp, token=part))
    require(not diagnostics, "invalid Unihan supplemental tokens: " + json.dumps(diagnostics))
    readings = build_readings(rows, unihan)
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".stroke3500-", dir=out.parent) as temp:
        staging = Path(temp) / "bundle"
        staging.mkdir()
        raw_records, blocks, audit = [], [], []
        golden = Path(__file__).parent / "tests/fixtures/stroke_order/prototype_2000"
        with ReferenceCodec() as codec:
            for first in range(0, PROFILE, 250):
                batch = rows[first:first + 250]
                charset = Path(temp) / "batch.txt"
                charset.write_text("\n".join(r["character"] for r in batch) + "\n", encoding="utf-8")
                coverage = convert.report_charset_coverage(str(root), str(charset), HWD_COMMIT)
                require(coverage["complete"] and coverage["ok_count"] == len(batch),
                        "incomplete graphics: " + json.dumps(coverage["errors"], ensure_ascii=False))
                if first < 2000:
                    legacy = convert.pack_characters([r["character_data"] for r in coverage["records"]])
                    require(legacy == (golden / f"so{first // 250:02d}.sob1").read_bytes(),
                            "legacy SOB1 golden changed")
                for row, record in zip(batch, coverage["records"]):
                    glyph = record["character_data"]
                    # Existing converter is unchanged; its canonical single-record bytes
                    # are independently validated by dzz1 before being compressed.
                    raw = convert._pack_character(glyph)
                    require(struct.unpack_from("<I", raw)[0] == row["codepoint_int"], "glyph identity")
                    block = encode_record(raw, row["rank"], codec)
                    blocks.append(block)
                    raw_records.append((row["rank"], raw))
                    audit.append(dict(rank=row["rank"], codepoint=row["codepoint_int"],
                                      source_sha256=record["source_sha256"], raw_sha256=digest(raw),
                                      raw_bytes=len(raw), transformed_bytes=block.transformed_length,
                                      stored_bytes=len(block.stored), strokes=record["stroke_count"]))
            codec_sources = codec.source_hashes
        cid = corpus_id(raw_records, readings, source)
        shards = shard_blocks(blocks, cid)
        catalog = pack_scb2(shards, cid)
        pinyin = pack_spy2(readings, cid)
        spy = validate_spy2(pinyin)
        for index, blob in enumerate(shards):
            (staging / shard_name(index)).write_bytes(blob)
        (staging / "stroke_cat.bin").write_bytes(catalog)
        (staging / "stroke_pinyin.bin").write_bytes(pinyin)
        select.write_selection_csv(rows, staging / "selection-3500.csv", PROFILE)
        select.write_charset(rows, staging / "charset-3500.txt", PROFILE)
        summary = dict(
            profile=PROFILE, corpus_id=cid.hex(), graphics_count=len(raw_records),
            pinyin_count=len(readings), relation_count=spy["relations"], group_count=spy["aux"],
            max_readings=max(len(r["readings"]) for r in readings),
            max_group_members=max(len(g) for g in spy["groups"]),
            ignored_supplement_tokens=0, graphics_errors=0, pinyin_errors=0,
            raw_bytes=sum(len(raw) for _, raw in raw_records),
            transformed_bytes=sum(b.transformed_length for b in blocks),
            stored_payload_bytes=sum(len(b.stored) for b in blocks),
            max_raw_bytes=max(b.raw_length for b in blocks),
            max_transformed_bytes=max(b.transformed_length for b in blocks),
            max_stored_bytes=max(len(b.stored) for b in blocks),
            shard_sizes=[len(b) for b in shards], catalog_bytes=len(catalog), pinyin_bytes=len(pinyin),
            sob2_bytes=sum(map(len, shards)), legacy_golden_shards_equal=8,
            codec="heatshrink-0.4.1/DZZ1/10/4", codec_source_sha256=codec_sources,
            conditional_assets_bytes=ASSETS_BASELINE - OLD_STROKE_BYTES + sum(map(len, shards))
            + len(catalog) + len(pinyin) + 46 * (len(shards) - 8),
            assets_limit=ASSETS_LIMIT, other_assets_unchanged_assumption=True,
            prototype=True, device_compatible=False,
        )
        require(summary["conditional_assets_bytes"] <= ASSETS_LIMIT, "projected assets budget exceeded")
        for name, obj in (("readings.json", readings), ("source.json", source),
                          ("coverage.json", audit), ("runtime.json", summary)):
            (staging / name).write_bytes(canonical_json(obj))
        shutil.copyfile(root / "ARPHICPL.TXT", staging / "ARPHICPL.TXT")
        shutil.copyfile(args.unicode_license, staging / "UNICODE-LICENSE.txt")
        (staging / "NOTICE.md").write_text(NOTICE_TEXT, encoding="utf-8")
        names = sorted(p.name for p in staging.iterdir())
        (staging / "SHA256SUMS").write_text("".join(
            f"{digest((staging / name).read_bytes())}  {name}\n" for name in names), encoding="utf-8")
        verify_generated(staging)
        # Recheck the clean checkout after all reads; no download or mutation.
        convert._verify_git_checkout(root, HWD_COMMIT, DEFAULT_SOURCE_REPO)
        require(not out.exists(), "output appeared during generation")
        staging.rename(out)
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("table-json", "official-pdf", "hanzi-writer-data", "unihan-zip",
                 "unicode-license", "selection-csv", "source-manifest", "output-dir"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args(argv)
    os.environ["GIT_NO_LAZY_FETCH"] = "1"
    try:
        result = generate(args)
    except (ValueError, OSError, KeyError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
