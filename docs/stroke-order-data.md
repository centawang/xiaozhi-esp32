# Stroke-order data sources and SOB1 format

This document describes the **local-first prototype** data path for 笔划. It is
not a published 字库. Test fixtures and any converted `stroke_order.bin` must
not be described as a release character library.

The future networked product still uses transport-neutral `type=stroke` (see
`docs/adr/0002-use-shared-protocol-for-stroke-order.md`). This local binary is
an offline substitute for on-demand server data on M5Stack CoreS3 only.

## Sources and licenses

Prototype graphics are converted from a **pinned commit** of
[chanind/hanzi-writer-data](https://github.com/chanind/hanzi-writer-data), which
repackages Make Me a Hanzi (`skishore/makemeahanzi`) stroke outlines and
medians.

| Material | License / status | How this repo uses it |
|---|---|---|
| Stroke outlines and medians (graphics) | Arphic Public License | Host converter reads `strokes` + `medians` JSON only |
| Associated dictionary / extra metadata | May be LGPL or other copyleft terms | Not ingested by the converter |
| Synthetic test data | Project test data | Hand-built rectangles/curves, not upstream glyphs |
| Three-character smoke corpus | Arphic Public License | Verbatim 一、人、口 JSON from pinned upstream commit |

Requirements:

- Do not vendor the full `all.json` or an entire third-party checkout without an
  explicit character list, the upstream commit, license files, and modification
  notes.
- The converter never accesses the network. It rejects URL/UNC input strings,
  but it cannot determine whether an otherwise ordinary local path is backed by
  a network-mounted filesystem.
- Conversion requires a local git checkout, a complete 40-hex commit equal to
  `HEAD`, a matching `remote.origin.url`, and a completely clean worktree
  (tracked and untracked changes are rejected). Every selected JSON must be a
  tracked blob whose bytes match that `HEAD` exactly.
- Commercial / production release still needs authorization and stroke-order
  accuracy review. Do not claim official per-character certification from the
  upstream README.

The tiny real corpus under
`scripts/tests/fixtures/stroke_order/upstream_smoke/` is pinned to commit
`68d10a4b21150cae5e1ebbd223eed289cf32d90c`. Its `NOTICE.md` records the exact
source paths and hashes, and its `ARPHICPL.TXT` is a byte-for-byte copy of the
complete upstream license. The three JSON files are unmodified; generated SOB1
output is not committed there.

The prototype modification notice records the reproducible conversion date
`2026-09-12`, converter version `stroke-order-converter/1`, schema version
`SOB1 v1 + SPY1 v1`, and the flatten/round/y-flip/clamp/downsample operations.
Legal review of Arphic Public License compliance is still required before any
external or commercial distribution; this documentation is not legal advice or
an authorization conclusion.

## Host conversion

Use a clean checkout and write output outside that checkout:

```sh
python3 scripts/convert_stroke_order.py \
  --hanzi-writer-data /path/to/hanzi-writer-data \
  --charset charset.txt \
  --source-commit <complete-40-hex-checkout-HEAD> \
  --output-dir /outside/the/checkout/out/
```

`charset.txt` is UTF-8, one character per line, with `#` comments allowed. Each
entry must be one scalar in the basic CJK Unified Ideographs block
`U+4E00..U+9FFF`. This structural restriction rejects ASCII, emoji, surrogates,
compatibility ideographs, extension blocks, and path separators. The explicit
charset remains the audited allow-list for the prototype's simplified
characters; a codepoint being in this block alone is not a simplified-character
or stroke-accuracy certification.

Only listed files are read. Every resolved JSON path must remain under the
provided data directory, including through symlinks. Missing JSON, invalid
UTF-8, duplicate characters, non-finite JSON numbers, unclosed paths, unknown
SVG commands, `strokes`/`medians` mismatch, and limit violations fail with a
controlled conversion error.

Accepted outline commands are `M L Q C Z` and relative `m l q c z`. Curves are
flattened on the host with deterministic de Casteljau subdivision (tolerance
0.25, max depth 8). Coordinates are rounded to integers in `0..1024` and **y is
flipped** from Make Me a Hanzi y-up to device y-down. Arbitrary SVG, scripts,
external resources, images, and font-outline inference are rejected.

## SOB1 binary (`stroke_order.bin`, version 1)

All integer fields, including point coordinates, are little-endian. Magic is
`SOB1`.

```
Header 32 bytes
  u8  magic[4]       "SOB1"
  u16 format_version 1
  u16 coord_max      1024
  u32 char_count
  u32 index_offset   32
  u32 data_offset    32 + char_count * 16
  u32 data_size
  u32 header_crc32   CRC-32/ISO-HDLC of bytes 0..23
  u32 index_crc32    CRC-32/ISO-HDLC of the complete index byte range

Index[char_count] 16 bytes, strictly increasing codepoint
  u32 codepoint
  u32 offset         absolute file offset
  u32 length
  u32 crc32          CRC-32/ISO-HDLC of the character record

Character record
  u32 codepoint
  u16 stroke_count
  u16 reserved       0
  Stroke[]
    u16 outline_count
    u16 median_count
    {u16 x, u16 y} outline[outline_count]  closed, first==last
    {u16 x, u16 y} median[median_count]    reveal path
```

CRC parameters are the standard CRC-32/ISO-HDLC reflected form used by zlib:
polynomial `0xEDB88320`, initial value `0xFFFFFFFF`, and final XOR
`0xFFFFFFFF`. The known answer for ASCII `123456789` is `0xCBF43926`.

Limits: ≤1024 characters, ≤48 strokes, ≤256 outline points/stroke, ≤64 median
points/stroke, ≤16 KiB/character, and ≤1 MiB/file. All offset/length/count math
is unsigned-32 overflow checked. `Bind()` verifies header fields, header CRC,
index size/ranges/order, and index CRC before exposing candidate codepoints.
`LoadCharacter()` verifies and parses one record using its individual CRC.
Checksum or parse failure does not clobber a previous valid bind/character.

The device accepts an arbitrarily aligned byte buffer. It never casts encoded
point bytes to a C++ object. `StrokeView::GetOutlinePoint()` and
`GetMedianPoint()` explicitly decode little-endian coordinates into caller-owned
`Point` values.

## Independent fixtures and harness

`handwritten_sob1_v1.hex` is an independently authored 84-byte golden, not
emitted by the converter. It fixes header, index, and record CRC values and
includes coordinate boundaries 0 and 1024. The compiled C++ harness reads this
golden, a Python-generated binary, and a binary made from the real three-glyph
smoke corpus. It covers successful bind/load, CRC known answers, damaged
header/index/record data, overflowed sizes/offsets/counts, failure-state
preservation, unaligned input, point boundaries, and point access.

Run it through the focused test suite:

```sh
python3 -m unittest scripts.tests.test_stroke_order -v
```

## Provenance manifest

`stroke_order.manifest.json` is deterministic (sorted keys, no timestamps) and
includes the verified checkout HEAD and origin, `checkout_clean: true`,
conversion parameters, codepoints, binary SHA-256, and one relative path plus
SHA-256 for every selected source JSON.

`selected_files_sha256` binds the ordered source list. It is SHA-256 over UTF-8
lines sorted by codepoint, each encoded as
`codepoint + NUL + relative_path + NUL + file_sha256 + LF`, where `codepoint` is
the manifest's `U+XXXX` spelling. `not_a_release_library` is always true for
this prototype.

## Legacy 500-character prototype pack

`scripts/tests/fixtures/stroke_order/prototype_500/` is retained as an offline
compatibility fixture; CoreS3 firmware builds now consume the sharded 2000-character pack. Membership is 通用规范汉字表 一级字表 numbers 0001-0500.
Official page: https://www.gov.cn/zwgk/2013-08/19/content_2469793.htm
Official PDF: https://www.gov.cn/gzdt/att/att/site1/20130819/tygfhzb.pdf
Official PDF SHA-256: `af85c706a53d3b3bbad818bcce7415ac9a2284ea14f79fe7f54ce1248a7bdac9`
(100606660 bytes). The same values are locked in `stroke_order.src.json`.

A third-party JSON checkout of `leonsilicon/table-of-general-standard-chinese-characters`
is a prototype transcription aid only. The generator accepts only the pinned
origin, full HEAD `f9786a82be6e1672bdc60f85760a9e4a3791d1f1`, tracked path
`table-of-general-standard-chinese-characters.json`, and pinned blob SHA-256
`a9f0a21fb83a84dd695eaeec7b77743a6099ca559768c8275ae0c22c827917d0`.
The checkout must be completely clean and workspace bytes must exactly equal
`git show HEAD:<path>`. The generation API does not accept a caller-supplied
transcription commit. That checkout has **no explicit license** in-tree; it is
not an official digital annex and not a commercial grant. Dual-person PDF
verification has not been completed, and the 500 characters have **not** been
manually reviewed one by one. This pack is not official certification and not
a commercial release 字库.

Graphics come from pinned `chanind/hanzi-writer-data` under the Arphic Public
License (`ARPHICPL.TXT`). The converter reads only `strokes`/`medians` JSON.
Associated dictionary text may be LGPL or other copyleft terms and is **not**
ingested. Pinyin readings come from Unicode 16.0.0 Unihan (`UNICODE-LICENSE.txt`).

The retained legacy packager can copy this pack with
`scripts/package_stroke_order_prototype.py`, but it is no longer selected by the
CoreS3 CMake path. It remains offline and does not convert from a live
hanzi-writer-data checkout. Its legacy extra-files include the mapped
`stroke_order.bin`/`stroke_pinyin.bin`, selection, coverage, source locks,
manifests, SHA256SUMS, APL, Unicode license, and NOTICE. The packager regenerates
`SHA256SUMS` after runtime filename mapping; its 13 entries hash every other file
in the final 14-file directory exactly once and never hash the checksum file
itself. Missing, extra, duplicate, malformed, or self-referential entries are
rejected. Asset basenames stay at or under 31 characters.

Host conversion may clamp Make Me a Hanzi control points that sit slightly
outside 0..1024 and downsample long flattened outlines so the **unchanged** SOB1
point and 1 MiB file limits still hold. The current prototype SOB1 is 1,046,788
bytes (1,788 bytes under 1 MiB). Generation must fail if that limit would be
exceeded; do not raise `MAX_FILE_BYTES` / `kMaxFileBytes`.

## 2000-character sharded prototype and SCB1

CoreS3 builds explicitly selecting the Legacy2000 compatibility fallback consume
`scripts/tests/fixtures/stroke_order/prototype_2000/`. The source/audit fixture
contains `selection-2000.csv`, `charset-2000.txt`, the pinned source lock,
2000/2000 coverage, eight per-shard manifests, SCB1, SPY1, licenses, NOTICE,
and a closed `SHA256SUMS`. These builds run only
`scripts/package_stroke_order_2000.py`; they neither access HWD/Unihan/the PDF
nor include `all.json`.

Ranks are partitioned into fixed 250-character ranges and converted to unchanged
SOB1 v1 files. The eight sizes are 443020, 603800, 680236, 723504, 771996,
816964, 849284, and 911688 bytes (5,800,492 total), all below 1 MiB. Runtime
assets use `so00.bin` through `so07.bin`.

SCB1 v1 (`stroke_cat.bin`) is little-endian:

```
Header 32 bytes
  u8  magic[4]          "SCB1"
  u16 format_version    1
  u16 shard_count       <= 16
  u32 character_count   <= 2048
  u32 entry_offset      32
  u32 shard_offset      32 + character_count * 12
  u32 total_shard_bytes
  u32 header_crc32      CRC of bytes 0..23
  u32 body_crc32        CRC of bytes 32..end

Entry 12 bytes, strictly increasing codepoint
  u32 codepoint
  u16 official_rank
  u8  shard_index
  u8  reserved          0
  u16 local_sob1_index
  u16 reserved          0

Shard descriptor 40 bytes
  u8  asset_name[16]    ASCII, NUL-terminated and zero-padded
  u32 exact_file_size   <= 1 MiB
  u32 file_crc32
  u32 character_count
  u32 first_rank
  u32 last_rank
  u32 reserved          0
```

Catalog validation rejects noncanonical offsets, duplicate or missing ranks,
duplicate codepoints/names/local indexes, inconsistent shard ranges, overflow,
and trailing bytes. Runtime bind additionally CRC-checks and fully loads every
SOB1 record and checks every catalog local index. The controller retains only an
owned catalog, owned SPY1, and bounded decoded glyph caches. The shard-source
API permits one acquired view at a time, every view is at most 1 MiB, and an
RAII guard releases it before the Controller mutex is unlocked. Asset suspension
clears the source reference and both metadata indexes synchronously before
partition munmap. Rebind verifies every shard and requires SPY1 to contain the
same 2000 codepoint/rank pairs as SCB1 before the SO entry can appear. See
`docs/adr/0005-shard-local-stroke-order-2000.md`.

Firmware packaging deliberately excludes the large per-character audit
manifests, selection, charset, source lock, and coverage files. It includes only
the eight shards, SCB1, SPY1, APL/Unicode licenses, NOTICE, compact
`runtime.json`, and closed checksums. Asset generation rejects basename
collisions or names over 31 bytes and requires at least 256 KiB free in the
8 MiB partition.

## 3500-character prototype (default local dataset)

`scripts/tests/fixtures/stroke_order/prototype_3500/` preserves the deterministic
host bundle for the **complete tier1, official numbers 0001–3500**, without
renaming or regenerating its members. Its corpus ID is
`bcd474c127b2138738d96b9bf64832ad`. The closed directory has 18 files
(7,240,949 bytes): six SOB2 `so00.bin`–`so05.bin` shards, SCB2
`stroke_cat.bin`, SPY2 `stroke_pinyin.bin`, selection/charset,
`source.json`, `readings.json`, `coverage.json`, `runtime.json`, APL,
Unicode license, NOTICE, and `SHA256SUMS` (17 entries, excluding itself).
No raw 3500-character HWD JSON set, official PDF, or Unihan ZIP is included.

The pinned graphics source is `chanind/hanzi-writer-data` commit
`68d10a4b21150cae5e1ebbd223eed289cf32d90c`, upstream
`skishore/makemeahanzi`; graphics and derivatives remain subject to the
included `ARPHICPL.TXT`. Pinyin uses Unicode 16.0.0 Unihan `kMandarin` plus
`kHanyuPinyin` under `UNICODE-LICENSE.txt`, retaining all 5,367 relations,
1,233 groups, up to 10 readings per character and 37 members per group.
`source.json` and `scripts/stroke_order/policy3500.py` pin the source manifest,
selection, official PDF, Unicode input, and licenses. The transcription aid
is the same `f9786a82be6e1672bdc60f85760a9e4a3791d1f1` version above, accepted
by its pinned JSON SHA-256, **not a clean Git checkout claim**. It has no
explicit in-tree license. PDF dual-person page verification, per-character
stroke-order accuracy review, and legal/commercial review are **not complete**.
This is a technical/audit prototype, not official certification or a
release-ready character library.

The fixture uses host v2 formats and reversible DZZ1/heatshrink compression;
it does not change the v1 fixtures. All six shards are at most 1 MiB.
`runtime.json` projects 7,352,753 bytes of assets **only if other assets and
the baseline packaging overhead remain unchanged**, below the 8,126,464-byte
limit that reserves 256 KiB in 8 MiB. That fixture projection remains arithmetic,
not a device measurement. The immutable host package still keeps
`device_compatible=false`; product builds separately run
`scripts/verify_stroke_order_3500.py` for closed source-policy and production
reader admission before staging the six shards, catalog and pinyin.

`CONFIG_STROKE_ORDER_LOCAL` remains **default off**, CoreS3-only. When enabled
without an explicit dataset selection, Kconfig now defaults to **Level1_3500**;
`CONFIG_STROKE_ORDER_DATASET_LEGACY2000=y` remains an explicit compatibility
fallback. Existing sdkconfigs that explicitly select Legacy2000 retain it.
Level1_3500 has passed host tests, CoreS3 builds and on-device SO entry validation;
on 2026-09-24 the user also reported the tested interactions normal. This is
still a technical prototype: the licensing/PDF/accuracy reviews above and
100-session hardware heap/audio/touch acceptance are not complete. See
`docs/stroke-order-product-runtime.md` for the integrated worker and remaining
validation boundaries.

Offline fixture validation (requires the pinned local heatshrink sources used
by the existing host verifier; no automatic downloads):

```sh
python3 -m unittest scripts.tests.test_stroke_order_3500 -v
STROKE3500_BUNDLE="$PWD/scripts/tests/fixtures/stroke_order/prototype_3500" \
  python3 -m unittest scripts.tests.test_stroke_order_v2 -v
python3 scripts/package_stroke_order_3500.py \
  --source scripts/tests/fixtures/stroke_order/prototype_3500 \
  --output /outside/the/checkout/new-host-package
python3 scripts/package_stroke_order_3500.py \
  --verify /outside/the/checkout/new-host-package
```

The new fixture test also offers opt-in external fixed-input regeneration:
set `STROKE3500_REGEN_INPUTS` to a local JSON file containing exactly
`table_json`, `official_pdf`, `hanzi_writer_data`, `unihan_zip`,
`unicode_license`, `selection_csv`, and `source_manifest`, each an absolute
local path for the existing generator's corresponding argument. Source pins
and the clean HWD checkout are verified by that generator; output is written
only to a temporary directory and all 18 files must match this fixture byte
for byte. With the variable unset the test explicitly **skips regeneration**;
invalid configured inputs fail rather than downloading or silently replacing
anything. Ordinary fixture validation needs none of these external inputs.

## SPY1 pinyin index (`stroke_pinyin.bin`, version 1)

Independent of SOB1. Magic `SPY1`. Little-endian. Header and body CRC-32/ISO-HDLC.
The device stores group ids and ranks, never pinyin strings.

```
Header 32 bytes
  u8  magic[4]           "SPY1"
  u16 format_version     1
  u16 reserved           0
  u32 char_count         <= 2048
  u32 group_count        <= 2048
  u32 char_index_offset  32
  u32 group_index_offset
  u32 header_crc32       CRC of bytes 0..23
  u32 body_crc32         CRC of bytes 32..end

Char index 12 bytes, strictly increasing codepoint
  u32 codepoint
  u16 rank
  u8  reading_count      1..8, first is primary
  u8  reserved           0
  u32 readings_offset    u16 group_id[reading_count]

Group index 8 bytes
  u16 member_count       1..32, sorted by rank then codepoint
  u16 reserved           0
  u32 members_offset     {u32 codepoint, u16 rank, u16 reserved}[]
```

The payload has one canonical layout: immediately after both indexes, reading
arrays are tightly packed in character-index order, followed by member arrays
tightly packed in group-id order through exact end-of-file. Payload aliases,
overlap, gaps, and trailing bytes are invalid. Every character/group reference
is reciprocal: every referenced group contains that character with the same
rank, and every group member exists in the character index with the same rank
and references that group.

Limits: file <= 64 KiB, characters <= 2048, groups <= 2048, readings <= 8,
group members <= 32. SPY1 was never a published format; the 2000-character
upgrade raised the character/group validator bounds without changing the v1 byte
layout. The measured corpus is 63,618 bytes, 1,047 groups, and at most 29 members
in a group. Generation rejects rather than truncates a reading union or group.
Offset/length math is unsigned-32 overflow checked. Input may be unaligned.
Bind makes an owned copy. For the sharded 2000-character runtime pack, corrupt,
missing, or catalog-mismatched SPY1 fails the complete asset binding transaction
and hides SO; it does not leave an exact-only partial generation visible. MQTT
local pages use `TopRanked(6)` intersected with the catalog without opening
voice. WebSocket STT uses the pinyin provider after local parse, with the
recognized character first, polyphone union, rank order, dedup, and max 6,
still catalog-only/loadable.

Readings are generated from Unicode 16.0.0 Unihan `kMandarin` (required) plus a
safe `kHanyuPinyin` union. Tone marks become `a-z` plus a tone digit 1-5.
