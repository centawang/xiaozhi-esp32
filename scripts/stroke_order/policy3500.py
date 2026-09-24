"""Fixed 3500 host provenance policy, separate from structural v2 parsers.

Pins are not authentication. Changing the approved source corpus requires an
explicit policy revision; self-consistent diagnostics are not sufficient.
No dependency on an older generator and no external-cache path is implicit.
"""
from __future__ import annotations

import json
from pathlib import Path
import struct

from . import dzz1
from .corpus2 import canonical_json, corpus_id, digest, validate_bundle, verify_directory, VerifiedFiles
from .heatshrink import verified_source_hashes
from .scb2 import shard_name, MAX_BYTES as MAX_CATALOG_BYTES
from .spy2 import MAX_BYTES as MAX_PINYIN_BYTES
from .spy2 import normalized_characters, pack_spy2
from .v2 import MAX_SHARDS, MAX_SHARD_BYTES, PROFILE, require

ROOT = Path(__file__).resolve().parents[2]
SOURCE_PINS = {
    "apl_sha256": "5590533436c70f10f2f524ee61456238c290175c6662fbe1c700b5f038a6d328",
    "hwd_commit": "68d10a4b21150cae5e1ebbd223eed289cf32d90c",
    "hwd_repo": "https://github.com/chanind/hanzi-writer-data",
    "hwd_tree": "e16c125aeaeffa1734f254e9d99c46fe77571588",
    "official_pdf_sha256": "af85c706a53d3b3bbad818bcce7415ac9a2284ea14f79fe7f54ce1248a7bdac9",
    "selection_sha256": "f32fa0c468ad9a1752da81ac973d289e59c378aab0b84eaad0475bde3fa4466f",
    "source_manifest_sha256": "ab282a13d101a7e8de976107436c71031d69887b734884f21f99862d4045d9c5",
    "table_commit": "f9786a82be6e1672bdc60f85760a9e4a3791d1f1",
    "table_sha256": "a9f0a21fb83a84dd695eaeec7b77743a6099ca559768c8275ae0c22c827917d0",
    "table_verification": "pinned archive SHA256; not a clean Git checkout claim",
    "unicode_license_sha256": "e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96",
    "unihan_sha256": "b8f000df69de7828d21326a2ffea462b04bc7560022989f7cc704f10521ef3e0"
}
HWD_COMMIT = SOURCE_PINS["hwd_commit"]
APL_SHA256 = SOURCE_PINS["apl_sha256"]
UNICODE_LICENSE_SHA256 = SOURCE_PINS["unicode_license_sha256"]
UNIHAN_ZIP_SHA256 = SOURCE_PINS["unihan_sha256"]
UNIHAN_ZIP_BYTES = 8382485
SOURCE_MANIFEST_SHA256 = SOURCE_PINS["source_manifest_sha256"]
# SHA256(canonical_json(rank-ordered [{rank, codepoint, source_sha256}])).
# Derived from the exact source-manifest-complete.json pinned above, not coverage.
SOURCE_PROJECTION_SHA256 = "012e7cc13031570b9902a0e54aa9cdd123e3e29514968b8e33831edc0d6269b4"
# SHA256(canonical_json(normalized_characters(readings))) independently rebuilt
# from pinned Unicode 16.0.0 Unihan, retaining all valid supplemental readings.
READINGS_SHA256 = "e68fbaf66fa40f86dc74e931dc63df13cf51b080b8528bc7bd0405bb3bcf79b5"
ASSETS_BASELINE = 7568207
OLD_STROKE_BYTES = 5800492 + 24352 + 63618
ASSETS_LIMIT = 8126464
NOTICE_SHA256 = 'd8e8b8f6cef9b484c0dea6e14eb4fc8f7bce1dc9246f7b61c7c431a39e64cdbb'

NOTICE_TEXT = (
    "# 3500-character host-format technical prototype\n"
    "\n"
    "Graphics: chanind/hanzi-writer-data 68d10a4b21150cae5e1ebbd223eed289cf32d90c, upstream skishore/makemeahanzi.\n"
    "Modified: normalized closed outlines and medians, reversible DZZ1/heatshrink blocks.\n"
    "ARPHICPL.TXT applies to graphics and derivatives.\n"
    "Pinyin: Unicode 16.0.0 Unihan kMandarin + kHanyuPinyin; UNICODE-LICENSE.txt applies.\n"
    "Membership: full tier1 of 通用规范汉字表; transcription f9786a82be6e1672bdc60f85760a9e4a3791d1f1.\n"
    "The transcription has no explicit in-tree license. Pinned hashes are in source.json.\n"
    "Not an official certification, not commercially reviewed, not a release library.\n"
    "PDF dual-person page review and stroke-order accuracy/legal review remain required.\n"
    "New v2 host formats are NOT supported by the current firmware.\n"
)


# Envelope budgets, including SHA256SUMS. Based on generated-b (7,240,949
# bytes) and its runtime payload (~5.7 MB), rounded up with audit headroom.
# They are host-profile bounds, not a change to the binary formats/Flash layout.
GENERATED_AGGREGATE_LIMIT = 10 * 1024 * 1024
PACKAGE_AGGREGATE_LIMIT = 7 * 1024 * 1024
COMMON_MEMBER_LIMITS = {
    "stroke_cat.bin": MAX_CATALOG_BYTES,
    "stroke_pinyin.bin": MAX_PINYIN_BYTES,
    "ARPHICPL.TXT": 8 * 1024,
    "UNICODE-LICENSE.txt": 4 * 1024,
    "NOTICE.md": 4 * 1024,
}
GENERATED_MEMBER_LIMITS = dict(COMMON_MEMBER_LIMITS, **{
    "readings.json": 512 * 1024,
    "source.json": 4 * 1024,
    "coverage.json": 1536 * 1024,
    "runtime.json": 8 * 1024,
    "selection-3500.csv": 128 * 1024,
    "charset-3500.txt": 32 * 1024,
})
PACKAGE_MEMBER_LIMITS = dict(COMMON_MEMBER_LIMITS, **{"package.json": 16 * 1024})


def verify_profile_directory(path: Path, *, runtime: bool = False) -> tuple[VerifiedFiles, list[str]]:
    # Derive the dynamic set from the profile's contiguous 1..32 shard rule,
    # never from untrusted manifest content. The SCB2 descriptors are checked
    # against this exact set by validate_bundle after preflight and hashing.
    names = {p.name for p in path.iterdir()}
    shards = sorted(n for n in names if n.startswith("so") and n.endswith(".bin"))
    require(1 <= len(shards) <= MAX_SHARDS
            and shards == [shard_name(i) for i in range(len(shards))], "bundle shard names")
    limits = dict(PACKAGE_MEMBER_LIMITS if runtime else GENERATED_MEMBER_LIMITS)
    limits.update({name: MAX_SHARD_BYTES for name in shards})
    files = verify_directory(path, member_limits=limits,
                             aggregate_limit=PACKAGE_AGGREGATE_LIMIT if runtime else GENERATED_AGGREGATE_LIMIT)
    return files, shards


def verify_notices(files: dict[str, bytes]) -> None:
    for name, sha in (("ARPHICPL.TXT", APL_SHA256),
                      ("UNICODE-LICENSE.txt", UNICODE_LICENSE_SHA256),
                      ("NOTICE.md", NOTICE_SHA256)):
        require(digest(files[name]) == sha, "bundle license/notice pins mismatch")


def source_projection(rows: list[dict]) -> list[dict]:
    return [dict(rank=r["rank"], codepoint=r["codepoint"], source_sha256=r["source_sha256"])
            for r in rows]


def verify_source_projection(rows: list[dict]) -> None:
    require(digest(canonical_json(source_projection(rows))) == SOURCE_PROJECTION_SHA256,
            "coverage source provenance mismatch")


def verify_legacy_golden(raws: list[tuple[int, bytes]]) -> int:
    # Rebuild all eight complete legacy shards, not just their record lengths.
    from .convert import pack_characters
    golden = ROOT / "scripts/tests/fixtures/stroke_order/prototype_2000"
    ordered = sorted(raws)
    for first in range(0, 2000, 250):
        chars = [dict(character=chr(struct.unpack_from("<I", raw)[0]),
                      codepoint=struct.unpack_from("<I", raw)[0],
                      strokes=[dict(outline=o, median=m) for o, m in dzz1.inspect_raw(raw)])
                 for _, raw in ordered[first:first + 250]]
        require(pack_characters(chars) == (golden / f"so{first // 250:02d}.sob1").read_bytes(),
                "legacy SOB1 golden changed")
    return 8


def verify_diagnostics(files: dict[str, bytes], parsed: dict, cid: bytes,
                       shard_names: list[str]) -> None:
    chars = sorted((c for s in parsed["catalog"]["shards"] for c in s["characters"]),
                   key=lambda c: c["rank"])
    audit = json.loads(files["coverage.json"])
    require(isinstance(audit, list) and len(audit) == PROFILE, "bundle coverage count")
    verify_source_projection(audit)
    expected_audit = []
    for c, row in zip(chars, audit):
        raw = c["raw"]
        expected_audit.append(dict(rank=c["rank"], codepoint=c["codepoint"],
                                   source_sha256=row["source_sha256"], raw_sha256=digest(raw),
                                   raw_bytes=len(raw), strokes=len(dzz1.inspect_raw(raw)),
                                   transformed_bytes=len(dzz1.encode(raw)),
                                   stored_bytes=c["stored_bytes"]))
    require(files["coverage.json"] == canonical_json(expected_audit), "bundle coverage mismatch")
    spy = parsed["pinyin"]
    shard_sizes = [len(files[n]) for n in shard_names]
    catalog_bytes, pinyin_bytes = len(files["stroke_cat.bin"]), len(files["stroke_pinyin.bin"])
    expected = dict(
        profile=PROFILE, corpus_id=cid.hex(), graphics_count=len(chars),
        pinyin_count=len(spy["characters"]), relation_count=spy["relations"], group_count=spy["aux"],
        max_readings=max(len(c["readings"]) for c in spy["characters"]),
        max_group_members=max(map(len, spy["groups"])),
        # Complete validated glyph set and the pinned complete Unihan projection
        # establish these success-only counters, never a claim from runtime.json.
        ignored_supplement_tokens=0, graphics_errors=0, pinyin_errors=0,
        raw_bytes=sum(c["raw_bytes"] for c in expected_audit),
        transformed_bytes=sum(c["transformed_bytes"] for c in expected_audit),
        stored_payload_bytes=sum(c["stored_bytes"] for c in expected_audit),
        max_raw_bytes=max(c["raw_bytes"] for c in expected_audit),
        max_transformed_bytes=max(c["transformed_bytes"] for c in expected_audit),
        max_stored_bytes=max(c["stored_bytes"] for c in expected_audit),
        shard_sizes=shard_sizes, catalog_bytes=catalog_bytes, pinyin_bytes=pinyin_bytes,
        sob2_bytes=sum(shard_sizes),
        legacy_golden_shards_equal=verify_legacy_golden([(c["rank"], c["raw"]) for c in chars]),
        codec="heatshrink-0.4.1/DZZ1/10/4", codec_source_sha256=verified_source_hashes(),
        conditional_assets_bytes=ASSETS_BASELINE - OLD_STROKE_BYTES + sum(shard_sizes)
        + catalog_bytes + pinyin_bytes + 46 * (len(shard_names) - 8),
        assets_limit=ASSETS_LIMIT, other_assets_unchanged_assumption=True,
        prototype=True, device_compatible=False)
    require(expected["conditional_assets_bytes"] <= ASSETS_LIMIT, "projected assets budget exceeded")
    require(files["runtime.json"] == canonical_json(expected), "bundle runtime diagnostics mismatch")


def verify_generated(path: Path) -> dict:
    files, shard_files = verify_profile_directory(path)
    source = json.loads(files["source.json"])
    require(files["source.json"] == canonical_json(SOURCE_PINS), "bundle source pins mismatch")
    require(digest(files["selection-3500.csv"]) == SOURCE_PINS["selection_sha256"],
            "bundle selection pin mismatch")
    verify_notices(files)
    readings = json.loads(files["readings.json"])
    require(digest(canonical_json(normalized_characters(readings))) == READINGS_SHA256,
            "bundle Unihan reading provenance mismatch")
    require(files["readings.json"] == canonical_json(sorted(normalized_characters(readings),
                                                          key=lambda c: c["rank"])),
            "noncanonical reading diagnostics")
    parsed = validate_bundle(files["stroke_cat.bin"], files["stroke_pinyin.bin"], [files[n] for n in shard_files])
    raws = [(c["rank"], c["raw"]) for shard in parsed["catalog"]["shards"] for c in shard["characters"]]
    cid = corpus_id(raws, readings, source)
    require(cid == parsed["catalog"]["corpus_id"], "bundle content identity mismatch")
    require(pack_spy2(readings, cid) == files["stroke_pinyin.bin"], "bundle reading provenance mismatch")
    from .select import load_selection_csv
    rows = load_selection_csv(path / "selection-3500.csv", PROFILE)
    expected = sorted((r["codepoint_int"], r["rank"]) for r in rows)
    require(expected == [(c["codepoint"], c["rank"]) for c in parsed["catalog"]["characters"]],
            "bundle selection mismatch")
    from .convert import load_charset
    require(load_charset(str(path / "charset-3500.txt")) == [r["character"] for r in rows],
            "bundle charset mismatch")
    verify_diagnostics(files, parsed, cid, shard_files)
    return dict(files=files, parsed=parsed, corpus_id=cid.hex(), shard_names=shard_files)
