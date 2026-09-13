# ADR 0005: Shard the local 2000-character stroke-order prototype

- Status: Accepted for the CoreS3 technical prototype
- Date: 2026-09-12
- Scope: local data packaging and runtime loading; the future `type=stroke` protocol remains unchanged

## Context

The original local prototype copied one 500-character SOB1 file (1,046,788
bytes) into PSRAM. Extending that design to 一级字表 ranks 0001-2000 would require
5,800,492 bytes for stroke data alone, before decoded glyphs and the pinyin
index. Copying the complete corpus into PSRAM is not acceptable. Ordinary builds
must also remain offline and must not vendor or read upstream `all.json`.

The pinned inputs remain:

- official government PDF SHA-256
  `af85c706a53d3b3bbad818bcce7415ac9a2284ea14f79fe7f54ce1248a7bdac9`;
- transcription aid commit `f9786a82be6e1672bdc60f85760a9e4a3791d1f1`;
- `chanind/hanzi-writer-data` commit
  `68d10a4b21150cae5e1ebbd223eed289cf32d90c`;
- Unicode 16.0.0 Unihan for pinyin.

These inputs do not constitute official per-character certification or
commercial approval.

## Decision

### Eight deterministic SOB1 shards

Ranks 0001-2000 are split by official rank into eight contiguous groups of 250.
Each group is converted independently to unchanged SOB1 v1. Runtime names are
`so00.bin` through `so07.bin`; every name is shorter than the 31-byte asset
limit. Pinned sizes are:

```
443020, 603800, 680236, 723504,
771996, 816964, 849284, 911688 bytes
```

Every shard is at most 1 MiB. No later-ranked character is substituted for a
missing character; generation requires 2000/2000 HWD coverage.

### SCB1 catalog

`stroke_cat.bin` uses SCB1 v1. Its header has independent header/body CRC-32,
canonical entry/shard offsets, total shard bytes, and bounded counts. Each
codepoint-sorted entry stores Basic-CJK codepoint, official rank, shard id, and
local SOB1 index. Each shard descriptor stores a NUL-terminated short asset
name, exact size, whole-file CRC-32, character count, and rank range.

Validators reject unknown magic/version, noncanonical offsets, overflow,
reserved bits, duplicate names/codepoints/ranks/local indexes, gaps in ranks or
local indexes, out-of-range shard references, inconsistent rank ranges, and
wrong total bytes. At bind, firmware transiently visits every mapped shard,
checks descriptor size/CRC, binds SOB1, loads every record, and verifies each
codepoint/local-index mapping. It retains no shard view after validation.

### Runtime ownership and asset generations

Startup owns only the 24,352-byte catalog copy and the 63,618-byte pinyin index
copy in PSRAM. Candidate and selected characters are fetched through the Assets
mmap, parsed while the Controller mutex and display serialization are held, and
decoded into bounded owned glyph vectors. The shard-source contract allows only
one acquired shard view at a time; an RAII guard releases that view before the
Controller mutex is unlocked. A `StrokeView` into a shard is never returned or
retained by production code across unlock or an Assets update.

`Assets::UnApplyPartition()` synchronously calls `StrokeOrderView::SuspendAssets()`
before `esp_partition_munmap()`. Suspension closes the overlay, clears decoded
candidate/current glyphs, unbinds SCB1/SPY1/SOB1 state, and removes the source
reference. Rebind is one fail-closed transaction: SPY1 must validate and contain
exactly the same 2000 codepoint/rank pairs as SCB1, then all eight declared
shards must pass size, CRC, SOB1, and local-index validation before the entry is
shown. Missing, corrupt, swapped, or partially updated files hide the entry. The
implementation does not allocate a whole-corpus copy; the legacy single-SOB1
host path remains capped at 1 MiB.

### Pinyin compatibility

Measured ranks 0001-2000 require 2000 characters, 1047 groups, at most 29 group
members, and 63,618 bytes. Because SPY1 was never published, its byte layout and
version remain v1 while bounded character/group validator limits rise from 1024
to 2048. The 64 KiB file limit, eight readings/character, and 32 members/group
remain unchanged. Generation rejects rather than silently truncates excess
readings or groups.

Candidate behavior remains recognized character first, polyphone union, rank
order, deduplication, catalog/store filtering, and maximum six. WebSocket keeps
correlated remote STT. MQTT keeps local `TopRanked` candidates and does not claim
voice routing.

### Firmware asset contents and margin

Firmware assets contain only the eight SOB1 shards, SCB1 catalog, SPY1 index,
APL, Unicode license, NOTICE, compact runtime manifest, and a closed checksum
file. Selection CSV, charset, source locks, coverage, and per-shard source-file
manifests stay in the repository audit fixture and are not copied to flash.

Packaging is offline and atomic. Basename collisions and names over 31 bytes are
fatal. A stroke-enabled build requires `generated_assets.bin` to be strictly
smaller than the assets partition with at least 256 KiB remaining; otherwise
the build stops.

## Consequences

- Persistent stroke metadata is about 88 KiB in PSRAM, plus bounded decoded
  candidate/current glyph vectors. The 5.8 MiB corpus remains flash-mapped.
- Startup validation reads all eight shards but does not retain them or copy them
  to PSRAM.
- The 500-character fixture remains useful for legacy converter/parser tests but
  is no longer the CoreS3 runtime package.
- This remains a technical prototype. APL/transcription/Unicode legal review,
  manual stroke-order review, and hardware performance/stability validation are
  still required before external or commercial distribution.
