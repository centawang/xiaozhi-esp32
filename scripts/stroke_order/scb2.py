"""SCB2 strict 3500-character catalog with <=32 contiguous rank shards."""
from __future__ import annotations

import struct

from .sob2 import validate_sob2
from .v2 import (HEADER_SIZE, MAX_SHARD_BYTES, MAX_SHARDS, PROFILE, codepoint,
                 crc, envelope, open_envelope, require)

ENTRY = struct.Struct("<IHHHH")
SHARD = struct.Struct("<16sIIHHHH8s")
MAX_BYTES = 64 * 1024


def shard_name(index: int) -> str:
    return f"so{index:02d}.bin"


def pack_scb2(shards: list[bytes], corpus: bytes) -> bytes:
    require(1 <= len(shards) <= MAX_SHARDS, "SCB2 shard count")
    entries, descriptors = [], bytearray()
    for index, blob in enumerate(shards):
        parsed = validate_sob2(blob, corpus)
        for char in parsed["characters"]:
            entries.append((char["codepoint"], char["rank"], index, char["local"], 0))
        descriptors.extend(SHARD.pack(shard_name(index).encode(), len(blob), crc(blob),
                                      parsed["first_rank"], parsed["last_rank"], parsed["count"],
                                      0, bytes(8)))
    body = b"".join(ENTRY.pack(*e) for e in sorted(entries)) + descriptors
    blob = envelope(b"SCB2", corpus, len(entries), len(shards), HEADER_SIZE,
                    HEADER_SIZE + len(entries) * ENTRY.size, 0, body)
    validate_scb2(blob, shards, corpus)
    return blob


def validate_scb2(data: bytes, shards: list[bytes] | None = None,
                  corpus: bytes | None = None) -> dict:
    header = open_envelope(data, b"SCB2", MAX_BYTES, corpus)
    n, s = header["count"], header["aux"]
    desc_offset = HEADER_SIZE + n * ENTRY.size
    require(n == PROFILE and 1 <= s <= MAX_SHARDS and header["flags"] == 0, "SCB2 profile/counts")
    require(header["offset1"] == HEADER_SIZE and header["offset2"] == desc_offset
            and desc_offset + SHARD.size * s == len(data), "SCB2 offsets/size")
    if shards is not None:
        require(len(shards) == s, "SCB2 missing/extra shards")
    descriptors = []
    first = 1
    for i in range(s):
        name, size, checksum, start, last, count, r, padding = SHARD.unpack_from(data, desc_offset + i * SHARD.size)
        require(name == shard_name(i).encode().ljust(16, b"\0") and r == 0 and padding == bytes(8),
                "SCB2 descriptor name/reserved")
        require(HEADER_SIZE < size <= MAX_SHARD_BYTES and start == first
                and 1 <= count <= PROFILE and last == start + count - 1 <= PROFILE,
                "SCB2 shard rank/size")
        item = dict(name=shard_name(i), size=size, crc=checksum, first=start, last=last, count=count)
        if shards is not None:
            blob = shards[i]
            require(len(blob) == size and crc(blob) == checksum, "SCB2 shard CRC/size mismatch")
            parsed = validate_sob2(blob, header["corpus_id"])
            require((parsed["first_rank"], parsed["last_rank"], parsed["count"]) == (start, last, count),
                    "SCB2/SOB2 rank mismatch")
            item["characters"] = parsed["characters"]
        descriptors.append(item)
        first = last + 1
    require(first == PROFILE + 1, "SCB2 incomplete ranks")
    characters = []
    previous = 0
    ranks = set()
    for i in range(n):
        cp, rank, shard, local, reserved = ENTRY.unpack_from(data, HEADER_SIZE + i * ENTRY.size)
        codepoint(cp)
        require(cp > previous and 1 <= rank <= PROFILE and rank not in ranks
                and shard < s and reserved == 0, "SCB2 duplicate/invalid index")
        descriptor = descriptors[shard]
        require(local < descriptor["count"] and rank == descriptor["first"] + local,
                "SCB2 local/rank mismatch")
        if shards is not None:
            require(descriptor["characters"][local]["codepoint"] == cp, "SCB2/SOB2 codepoint mismatch")
        characters.append(dict(codepoint=cp, rank=rank, shard=shard, local=local))
        previous = cp
        ranks.add(rank)
    return dict(**header, characters=characters, shards=descriptors)
