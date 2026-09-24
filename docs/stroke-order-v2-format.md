# Stroke-order host formats v2: 3500 profile

- Status: versioned host-format technical prototype; **not firmware support or release approval**
- Version: envelope 2, profile 3500, DZZ1 transform 1
- Scope: `scripts/stroke_order/{v2,dzz1,heatshrink,sob2,scb2,spy2,corpus2}.py`
- Host provenance policy: `scripts/stroke_order/policy3500.py`
- Related: [v1 sources and format](stroke-order-data.md), [ADR 0005](adr/0005-shard-local-stroke-order-2000.md)

This is the normative byte contract for the new host prototype. “Must” denotes
validation requirements, not a decision to enable these formats on devices.
No v1 format, limit, fixture, firmware reader, asset selection or CMake rule is
changed. Current firmware must not receive this package as a compatible update.

## 1. Conventions and limits

All multi-byte integers are unsigned little-endian (LE); byte arrays have no
endianness. Offsets are absolute from byte zero unless explicitly identified as
array indexes. Tables and payloads are tightly packed: no alignment gaps,
implicit padding, overlapping ranges, or trailing bytes are permitted. Reserved
fields and explicit padding are zero. All arithmetic must be range-checked
before indexing/allocating; validate lengths using subtraction to avoid overflow.
Codepoints are Basic CJK `U+4E00..U+9FFF`; membership in the simplified official
selection is a separate host provenance rule, not inferred from that range.

| Quantity | Limit |
|---|---:|
| Complete SCB2/SPY2 bundle characters | exactly 3500 |
| Official rank | 1..3500, unique and complete |
| SOB2 shards | 1..32 |
| Each SOB2 file | 1,048,576 bytes maximum |
| SCB2 file | 65,536 bytes maximum |
| SPY2 file | 524,288 bytes maximum |
| Decoded raw character record | 16,384 bytes maximum |
| DZZ1 transformed record | 16,388 bytes maximum and at most raw length + 4 |
| Stored compressed block | 18,437 bytes maximum (`ceil(16388*9/8)`) |
| Strokes / character | 1..48 |
| Closed outline points / stroke | 4..256, including repeated closing point |
| Median points / stroke | 2..64 |
| Coordinate | 0..1024 inclusive |
| Readings / character | 1..16, no silent truncation |
| Members / pinyin group | 1..64, no silent truncation |
| Pinyin groups | 1..4096 |
| Relations | at most 56,000; also at most 64 * groups |

A standalone SPY2 may contain 1..3500 characters (useful for synthetic tests),
with ranks exactly 1..N. Binding a complete bundle still requires exactly 3500
and identical SCB2/SPY2 `(codepoint, rank)` pairs. Standalone SOB2 shards contain
contiguous subsets of ranks. These allowances do not weaken complete-profile
validation. V1 retains its independent 8-readings / 32-members limits.

## 2. Common 64-byte envelope

The layout is `<4sHH16s10I`. CRC is CRC-32/ISO-HDLC, as returned by
`zlib.crc32(data) & 0xffffffff` (not CRC32C). CRC is corruption detection, not
authentication. Header CRC includes the body-CRC field.

| Offset | Bytes | Field | Required value / meaning |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `SOB2`, `SCB2`, or `SPY2` |
| 4 | 2 | version | 2 |
| 6 | 2 | profile | 3500 |
| 8 | 16 | corpus_id | opaque digest prefix, not all zero |
| 24 | 4 | count | characters in this file |
| 28 | 4 | aux | SOB2 first rank; SCB2 shard count; SPY2 group count |
| 32 | 4 | offset1 | 64 for all three formats |
| 36 | 4 | offset2 | format-specific second table/payload offset |
| 40 | 4 | total_bytes | exactly the supplied file length |
| 44 | 4 | body_crc32 | CRC of bytes `[64, total_bytes)` |
| 48 | 4 | header_crc32 | CRC of all 64 header bytes with bytes 48..51 zero |
| 52 | 4 | flags | SOB2 codec parameters; SCB2 zero; SPY2 relation count |
| 56 | 4 | reserved1 | zero |
| 60 | 4 | reserved2 | zero |

Every file in a bound corpus must have the same nonzero corpus_id. Unknown
magic/version/profile/codec, nonzero reserved bytes, size/CRC mismatch or mixed
identity is fatal. There is no fallback to v1 or another codec.

### Corpus identity

`canonical_json(x)` is UTF-8 of Python JSON with `ensure_ascii=False`,
`sort_keys=True`, `indent=2`, default separators, followed by exactly one LF.
For this schema only strings, integers, booleans, lists and objects are used;
there are no floats or non-finite numbers.

```
records = [{"rank": rank, "sha256_raw": lowercase_sha256(raw_record)}, ...]
           # sorted by rank, exactly ranks 1..3500
readings = [{"codepoint": cp, "rank": rank, "readings": [normalized_syllable, ...]}, ...]
           # sorted by codepoint; readings preserve primary-first order
identity = {"profile": 3500, "source": source_json_object,
            "records": records, "readings": readings}
corpus_id = SHA256(b"stroke3500-v2\0" + canonical_json(identity))[0:16]
```

`source` is the complete pinned provenance object, not just a source commit.
Raw geometry, official rank, exact reading order, or provenance changes alter
identity. Compression and sharding do not enter identity. This is not a signature
and cannot alone establish trusted origin. Only the generated host bundle has
the source/readings needed to recompute it; a runtime package validator checks
cross-file identity and structure, not omitted source provenance.

## 3. Raw glyph and DZZ1 transform

Raw glyphs are unchanged normalized SOB1 character records (not entire SOB1
files). At offset 0: `u32 codepoint`, offset 4: `u16 stroke_count`, offset 6:
`u16 reserved=0`. Starting at 8, each stroke contains `u16 outline_count`,
`u16 median_count`, then outline and median points, each `u16 x, u16 y`.
Each outline's first and last points must match. The final point must end
exactly at the declared raw length; there are no per-stroke offsets.

DZZ1 prepends ASCII `DZZ1`, retains that eight-byte glyph header at offsets
4..11, and retains each stroke's two `u16` counts. Point coordinates become
unsigned canonical base-128 varints in x,y order. Previous x,y reset to `(0,0)`
**separately for each outline and median of every stroke**. For delta `d`:

```
u = 2*d            if d >= 0
u = -2*d - 1       otherwise
```

One byte is mandatory for `u < 128`. Otherwise exactly two bytes are required:
`(u & 127) | 128`, then `u >> 7`. The second byte must be 1..16 and the
combined value at most 2048. Overlong zero, a continuation in the second byte,
a third byte, overflow or truncation is invalid. Invert with `u/2` for even u
and `-(u/2)-1` for odd u (integer division). Accumulated coordinates must remain
0..1024. Validate the reconstructed raw record, including closure and exact
length. Consume every DZZ1 byte. No geometry simplification occurs here.

## 4. Heatshrink bitstream

Each DZZ1 character is independently compressed by heatshrink **0.4.1**, window
bits 10 (1024-byte window), lookahead bits 4 (maximum match 16). There is no
cross-character state, dictionary or stream header. Bits are consumed MSB-first:

- Tag 1: the following 8 bits are a literal byte.
- Tag 0: the following 10 bits plus one are distance (1..1024), followed by
  4 bits plus one for length (1..16). Copy with overlap. History before the
  start of output is zero, matching the reference's zero-initialized window.

The SOB2 transformed length is the **exact** decoder output length. No token
may overrun it; truncation even within a token is fatal. Once exactly that many
bytes have been produced, only 0..7 zero padding bits may remain. Eight or more
trailing bits, even all zero, or any nonzero padding is invalid. Thus a
permissive `heatshrink_decoder_finish()` result alone is not sufficient proof of
complete input. The independent strict host decoder checks these conditions
before cross-checking against the C decoder.

The encoder uses the unchanged C reference's deterministic match selection;
there is no fallback compressor. Decoder validity does not require replaying
encoder match selection. Six C/header source hashes are fixed independently of
the mutable managed-component checksum manifest and verified before use. The
host bridge is compiled into a temporary directory, never generated in-tree.

## 5. SOB2 independent glyph blocks

Envelope: count N, aux first rank, offset1=64,
offset2=`64+32*N`, flags=`0x040A0101`. The flag bytes from low to high mean
codec 1 (heatshrink), transform 1 (DZZ1), window 10, lookahead 4. Other values
are invalid. Each 32-byte index entry is `<7I2H>`:

| Relative offset | Type | Field |
|---:|---|---|
| 0 | u32 | codepoint |
| 4 | u32 | absolute compressed block offset |
| 8 | u32 | stored/compressed length |
| 12 | u32 | exact transformed/DZZ1 length |
| 16 | u32 | exact raw length |
| 20 | u32 | CRC32 of stored bytes |
| 24 | u32 | CRC32 of reconstructed raw bytes |
| 28 | u16 | official rank |
| 30 | u16 | reserved=0 |

Index order is rank order, not codepoint order. Ranks must be
`first_rank + local_index`, within 1..3500; codepoints are unique within a shard.
First block starts at offset2; every subsequent block starts at the previous
block's end, and the last block ends at EOF. Both CRCs and the raw record's
codepoint must agree with the index. Check all geometry and codec bounds above.

The generator greedily adds rank-ordered blocks while
`64 + sum(32 + stored_length)` stays at most 1 MiB; otherwise starts the next
shard. The full rank sequence is 1..3500 and at most 32 shards may be emitted.
Validators require complete contiguous coverage, but do not infer provenance
from a particular shard count or require greedy packing.

## 6. SCB2 catalog

Envelope: count=3500, aux=S (1..32), offset1=64,
offset2=`64+12*3500`, flags=0. At 64 are 3500 12-byte entries (`<IHHHH>`):

| Relative offset | Type | Field |
|---:|---|---|
| 0 | u32 | codepoint, strictly increasing |
| 4 | u16 | unique official rank 1..3500 |
| 6 | u16 | shard id 0..S-1 |
| 8 | u16 | local index within that shard |
| 10 | u16 | reserved=0 |

At offset2 are S 40-byte descriptors (`<16sIIHHHH8s>`):

| Relative offset | Type | Field |
|---:|---|---|
| 0 | byte[16] | exact ASCII `so00.bin`..`so31.bin`, NUL-padded |
| 16 | u32 | exact shard file length |
| 20 | u32 | CRC32 of entire SOB2 file, including envelope |
| 24 | u16 | first rank |
| 26 | u16 | last rank |
| 28 | u16 | character count |
| 30 | u16 | reserved=0 |
| 32 | byte[8] | zero padding |

Descriptor i names `so{i:02d}.bin`; rank ranges are adjacent from 1 to 3500.
`last=first+count-1`; each entry's `rank=descriptor.first+local`. A full bundle
validates every declared SOB2, its size/CRC/corpus_id/rank range/count, and every
catalog codepoint/local mapping. No missing/extra shards or duplicate mapping
is accepted. File size is exactly `offset2+40*S`.

## 7. SPY2 reciprocal CSR pinyin relations

Envelope count=N, aux=G, flags=E (relation count), offset1=64,
offset2=`64+8*N`. Layout has no syllable text table:

| Absolute offset | Data |
|---|---|
| 64 | N records `<IHH>`: codepoint, rank, reserved=0 |
| `64+8*N` | N+1 u32 character CSR offsets |
| `64+8*N+4*(N+1)` | E u16 group ids (forward relations) |
| previous + `2*E` | G+1 u32 group CSR offsets |
| previous + `4*(G+1)` | E u16 character indexes (reverse relations) |

Characters are strictly codepoint sorted; ranks are unique 1..N. Both CSR
offset arrays start at zero, end at E, and increase strictly. Character ranges
have 1..16 unique group ids below G. Group ranges have 1..64 unique character
indexes below N, ordered by official rank. The forward and reverse edge sets
must be exactly equal and have E members; duplicate/nonreciprocal relations
are invalid. Final member array ends exactly at EOF.

Host construction normalizes numeric-tone pinyin with the existing normalizer,
retains the kMandarin primary-first list followed by unique kHanyuPinyin
supplements in source order, and never truncates over-limit lists. Group ids
are assigned by sorting the unique normalized syllable strings. The character
edge list retains primary-first order, not sorted group-id order. Strings live
only in `readings.json`; the generated verifier re-packs SPY2 from that pinned
list and compares exact bytes. In this corpus 敦 keeps all ten readings.

## 8. Host provenance and diagnostics policy

Structural parsers do not import a generator or the fixed provenance policy.
`policy3500.verify_generated()` composes structure with the fixed source policy.
Full source-manifest-complete.json SHA256 is:

`ab282a13d101a7e8de976107436c71031d69887b734884f21f99862d4045d9c5`.

Generation checks that exact manifest, clean HWD commit/blob identities, official
selection, PDF/transcription, licenses and Unihan pins. Policy contains all
source.json keys, including this manifest digest, and rejects additions or
changes. It verifies the rank-ordered `{rank, codepoint, source_sha256}` audit
projection against a digest derived from that exact manifest. This verifies
all 3500 source-hash bindings without copying the external manifest into the
repository or making validation depend on a `/tmp` path. It does not reopen
3500 original JSON files at package-verification time. The normalized full
reading projection is separately pinned from the verified Unihan source.

Coverage is compared field-for-field with decoded codepoint/rank, raw SHA256,
raw length, inspected stroke count, re-encoded DZZ1 length and actual stored
length. The complete source projection is checked first. Runtime diagnostics
are recomputed from these records and SPY2, including sums, maxima, shard
sizes, counts, success-only error/token counters, codec source hashes and
all eight reconstructed legacy SOB1 golden shards. Unknown/missing fields,
wrong JSON types or noncanonical diagnostic bytes fail closed. Rewriting
SHA256SUMS or regenerating an internally consistent corpus_id cannot authorize
a different provenance policy or a false diagnostic value.

Conditional assets estimate (not a build or hardware measurement):

```
7,568,207 - (5,800,492 + 24,352 + 63,618)
+ sum(SOB2 sizes) + SCB2 size + SPY2 size + 46*(shards - 8)
```

It must not exceed 8,126,464 bytes. `other_assets_unchanged_assumption=true`,
`prototype=true`, and `device_compatible=false` are mandatory. No claim about
actual current firmware partition usage follows from this estimate.

## 9. Closed directories and packages

`SHA256SUMS` is a nonempty ASCII **byte** stream, ending in LF. Each line is
exactly `<64 lowercase hex><two ASCII spaces><safe basename><LF>`; a safe
basename matches `[A-Za-z0-9][A-Za-z0-9_.-]{0,63}`. No newline normalization is
allowed. Reject CRLF, bare CR, Unicode line separators/non-ASCII, missing final
LF, blank lines, uppercase hex, duplicate names and manifest self-inclusion.
It lists every other file exactly once, never itself. Reject symlinks,
nonregular files (including directories/FIFOs), traversal, missing/extra files
and digest mismatches. Checksums are not signatures; closure and semantic
validation are both required.

Before reading **any** content (including SHA256SUMS) or hashing, callers must
supply `verify_directory` with the exact profile-derived member-to-limit map
and aggregate limit. Profile discovery uses only directory entry names: the
fixed required set below plus a contiguous `so00.bin`..`so{S-1:02d}.bin` set,
1 <= S <= 32. Arbitrary manifest names cannot authorize extra members. The
catalog subsequently must declare exactly that shard set, and its descriptors,
lengths and content are validated normally. This preserves dynamic legal
sharding; it does not pin the observed six-shard layout.

The verifier checks exact directory membership, then lstat of every member and
SHA256SUMS, rejecting symlinks/nonregular files, negative sizes and per-file
excess. It sums **all** sizes including SHA256SUMS and rejects aggregate excess
before any content I/O. Only then is the bounded manifest parsed and each member
SHA256 streamed in chunks of at most 65,536 bytes. The common verifier retains
only stat/hash metadata, not all member bytes. A read-through mapping lets
structural/policy consumers load individual approved files on demand; each
read checks the original stat identity/size and checksum again, with bounded
reads and no byte cache. `O_NOFOLLOW`, nonblocking open and fstat reject final
component substitution without hanging on a replacement FIFO.

| Member / complete profile | Maximum bytes | Observed generated-b bytes |
|---|---:|---:|
| Each SOB2 shard | 1,048,576 | 1,048,441 largest |
| SCB2 | 65,536 | 42,304 |
| SPY2 | 524,288 | 68,472 |
| coverage.json | 1,572,864 | 1,130,675 |
| readings.json | 524,288 | 333,530 |
| source.json | 4,096 | 958 |
| runtime.json | 8,192 | 1,530 |
| selection-3500.csv | 131,072 | 75,935 |
| charset-3500.txt | 32,768 | 14,187 |
| ARPHICPL.TXT | 8,192 | 6,900 |
| UNICODE-LICENSE.txt | 4,096 | 1,995 |
| NOTICE.md | 4,096 | 798 |
| package.json (runtime only) | 16,384 | 1,603 in derived runtime package |
| SHA256SUMS | 16,384 | 1,341 |
| Entire generated directory, including SHA256SUMS | **10,485,760 (10 MiB)** | **7,240,949** |
| Entire runtime package, including SHA256SUMS | **7,340,032 (7 MiB)** | **5,685,331** |

Binary caps are the unchanged format limits. Audit/text caps round the real
sizes up with headroom (coverage about 39%, readings about 57%); pinned license
and notice contents remain separately mandatory. package.json's 16 KiB bound
also covers 32 descriptors. The runtime aggregate allows headroom above the
measured payload and conditional assets budget; the generated aggregate adds
room for host audits. Both are host-profile envelope limits, not Flash layout
changes or device support. They are deliberately much smaller than the old
64 × 16 MiB generic allowance. A 32-shard layout is permitted only if its actual
aggregate fits; 32 full-size shards and 64 fake shards fail before reads.

A generated bundle contains exactly:

- `so00.bin` through the final contiguous shard;
- `stroke_cat.bin`, `stroke_pinyin.bin`;
- `readings.json`, `source.json`, `coverage.json`, `runtime.json`;
- `selection-3500.csv`, `charset-3500.txt`;
- `ARPHICPL.TXT`, `UNICODE-LICENSE.txt`, `NOTICE.md`, `SHA256SUMS`.

A runtime host package contains only the shards, catalog, pinyin, the two
licenses, NOTICE, `package.json` and SHA256SUMS. Canonical package.json contains
exactly `format="stroke3500-host-v2"`, corpus_id (lowercase hex),
`device_compatible=false`, and `files`. `files` maps every payload name (not
package.json or SHA256SUMS) to exactly `bytes` and lowercase `sha256`.
`verify_package()` checks this closed set, all fields and file hashes, licenses,
notice, structure and matching corpus identity. It cannot reconstruct omitted
source/readings or certify provenance of an arbitrary supplied package.
`package_corpus()` first applies the full generated policy, then stages and
re-verifies the package before atomic rename to a nonexistent output path.

```
python3 scripts/package_stroke_order_3500.py --source /tmp/generated --output /tmp/package
python3 scripts/package_stroke_order_3500.py --verify /tmp/package
STROKE3500_BUNDLE=/tmp/generated python3 -m unittest discover -s scripts/tests -p test_stroke_order_v2.py -v
```

The external-bundle tests explicitly skip when STROKE3500_BUNDLE is unset.
When it is set, even to an empty/invalid path, they must read and validate it;
missing or corrupt data is a failure, not a skip. No test downloads fixtures.

## 10. Failure and compatibility boundaries

Validate into local, unpublished state; never publish partially validated
records or fall back to a smaller corpus, substituted glyph, truncated pinyin,
old format or guessed geometry. No generated output directory is published on
verification failure. Any future device implementation needs its own bounded
allocation, asset-generation fencing, cancellation/lifetime design and hardware
acceptance; this document does not assert those exist.

APL, Unicode and transcription legal review, dual-person PDF review and manual
stroke-order accuracy review remain open. Neither these pins nor successful
host tests mean official certification or commercial release clearance.
