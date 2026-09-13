"""Deterministic SCB1 catalog for bounded SOB1 shards. Never accesses the network."""

from __future__ import annotations

import re
import struct
import zlib
from dataclasses import dataclass
from typing import Dict, List, Sequence

MAGIC = b"SCB1"
FORMAT_VERSION = 1
HEADER_SIZE = 32
ENTRY_SIZE = 12
SHARD_SIZE = 40
MAX_CHARACTERS = 2048
MAX_SHARDS = 16
MAX_FILE_BYTES = 64 * 1024
MAX_SHARD_BYTES = 1024 * 1024
MAX_ASSET_NAME_BYTES = 15
HEADER_FORMAT = "<4sHHIIIIII"
ENTRY_FORMAT = "<IHBBHH"
SHARD_FORMAT = "<16sIIIIII"
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_.-]*$")


class CatalogError(ValueError):
    """Raised when an SCB1 catalog or its shard declarations are invalid."""


@dataclass(frozen=True)
class CatalogEntry:
    codepoint: int
    rank: int
    shard: int
    local_index: int


@dataclass(frozen=True)
class ShardInfo:
    name: str
    size: int
    crc32: int
    character_count: int
    first_rank: int
    last_rank: int


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _checked_name(name: str) -> bytes:
    try:
        encoded = name.encode("ascii")
    except UnicodeEncodeError as exc:
        raise CatalogError("SCB1 shard name must be ASCII") from exc
    if not NAME_RE.fullmatch(name) or not (1 <= len(encoded) <= MAX_ASSET_NAME_BYTES):
        raise CatalogError("SCB1 shard name is invalid or too long")
    return encoded + b"\0" * (16 - len(encoded))


def pack_catalog(entries: Sequence[CatalogEntry], shards: Sequence[ShardInfo]) -> bytes:
    if not (1 <= len(entries) <= MAX_CHARACTERS):
        raise CatalogError("SCB1 character count out of range")
    if not (1 <= len(shards) <= MAX_SHARDS):
        raise CatalogError("SCB1 shard count out of range")

    ordered = sorted(entries, key=lambda item: item.codepoint)
    if list(entries) != ordered:
        raise CatalogError("SCB1 entries must be sorted by codepoint")
    names = set()
    for shard in shards:
        _checked_name(shard.name)
        if shard.name in names:
            raise CatalogError("duplicate SCB1 shard name")
        names.add(shard.name)
        if not (1 <= shard.size <= MAX_SHARD_BYTES) or not (
            1 <= shard.character_count <= MAX_CHARACTERS
        ):
            raise CatalogError("SCB1 shard size/count out of range")
        if not (1 <= shard.first_rank <= shard.last_rank <= len(entries)):
            raise CatalogError("SCB1 shard rank range invalid")
        if shard.last_rank - shard.first_rank + 1 != shard.character_count:
            raise CatalogError("SCB1 shard rank range/count mismatch")

    seen_cp = set()
    seen_rank = set()
    locals_by_shard: List[set[int]] = [set() for _ in shards]
    for item in entries:
        if not (0x4E00 <= item.codepoint <= 0x9FFF):
            raise CatalogError("SCB1 codepoint outside Basic CJK")
        if item.codepoint in seen_cp or item.rank in seen_rank:
            raise CatalogError("duplicate SCB1 codepoint or rank")
        if not (1 <= item.rank <= len(entries)) or not (0 <= item.shard < len(shards)):
            raise CatalogError("SCB1 entry rank/shard invalid")
        shard = shards[item.shard]
        if not (shard.first_rank <= item.rank <= shard.last_rank):
            raise CatalogError("SCB1 entry rank outside shard range")
        if not (0 <= item.local_index < shard.character_count):
            raise CatalogError("SCB1 local index out of range")
        if item.local_index in locals_by_shard[item.shard]:
            raise CatalogError("duplicate SCB1 shard local index")
        locals_by_shard[item.shard].add(item.local_index)
        seen_cp.add(item.codepoint)
        seen_rank.add(item.rank)
    if seen_rank != set(range(1, len(entries) + 1)):
        raise CatalogError("SCB1 ranks must be continuous")
    for index, shard in enumerate(shards):
        if locals_by_shard[index] != set(range(shard.character_count)):
            raise CatalogError("SCB1 shard local indexes must be complete")

    entry_offset = HEADER_SIZE
    shard_offset = entry_offset + len(entries) * ENTRY_SIZE
    body = b"".join(
        struct.pack(ENTRY_FORMAT, item.codepoint, item.rank, item.shard, 0, item.local_index, 0)
        for item in entries
    ) + b"".join(
        struct.pack(
            SHARD_FORMAT,
            _checked_name(shard.name),
            shard.size,
            shard.crc32,
            shard.character_count,
            shard.first_rank,
            shard.last_rank,
            0,
        )
        for shard in shards
    )
    total_shard_bytes = sum(item.size for item in shards)
    header_zero = struct.pack(
        HEADER_FORMAT,
        MAGIC,
        FORMAT_VERSION,
        len(shards),
        len(entries),
        entry_offset,
        shard_offset,
        total_shard_bytes,
        0,
        0,
    )
    header = struct.pack(
        HEADER_FORMAT,
        MAGIC,
        FORMAT_VERSION,
        len(shards),
        len(entries),
        entry_offset,
        shard_offset,
        total_shard_bytes,
        crc32(header_zero[:24]),
        crc32(body),
    )
    blob = header + body
    if len(blob) > MAX_FILE_BYTES:
        raise CatalogError("SCB1 file exceeds limit")
    validate_catalog(blob)
    return blob


def validate_catalog(data: bytes) -> Dict[str, object]:
    if not isinstance(data, (bytes, bytearray)) or not (HEADER_SIZE <= len(data) <= MAX_FILE_BYTES):
        raise CatalogError("SCB1 size out of range")
    magic, version, shard_count, char_count, entry_offset, shard_offset, total_bytes, header_crc, body_crc = struct.unpack_from(
        HEADER_FORMAT, data, 0
    )
    if magic != MAGIC or version != FORMAT_VERSION:
        raise CatalogError("SCB1 magic/version mismatch")
    if not (1 <= char_count <= MAX_CHARACTERS) or not (1 <= shard_count <= MAX_SHARDS):
        raise CatalogError("SCB1 counts out of range")
    if entry_offset != HEADER_SIZE or shard_offset != entry_offset + char_count * ENTRY_SIZE:
        raise CatalogError("SCB1 offsets are not canonical")
    expected_size = shard_offset + shard_count * SHARD_SIZE
    if expected_size != len(data):
        raise CatalogError("SCB1 size/offset mismatch")
    if header_crc != crc32(bytes(data[:24])) or body_crc != crc32(bytes(data[HEADER_SIZE:])):
        raise CatalogError("SCB1 checksum mismatch")

    raw_shards = []
    for index in range(shard_count):
        offset = shard_offset + index * SHARD_SIZE
        name_raw, size, digest, count, first_rank, last_rank, reserved = struct.unpack_from(
            SHARD_FORMAT, data, offset
        )
        if reserved != 0 or b"\0" not in name_raw:
            raise CatalogError("SCB1 shard reserved/name invalid")
        end = name_raw.index(0)
        if any(name_raw[end + 1 :]):
            raise CatalogError("SCB1 shard name padding must be zero")
        try:
            name = name_raw[:end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise CatalogError("SCB1 shard name must be ASCII") from exc
        raw_shards.append(ShardInfo(name, size, digest, count, first_rank, last_rank))

    raw_entries = []
    for index in range(char_count):
        offset = entry_offset + index * ENTRY_SIZE
        cp, rank, shard, reserved8, local_index, reserved16 = struct.unpack_from(
            ENTRY_FORMAT, data, offset
        )
        if reserved8 != 0 or reserved16 != 0:
            raise CatalogError("SCB1 entry reserved must be zero")
        raw_entries.append(CatalogEntry(cp, rank, shard, local_index))

    # Reuse the pack-time semantic checks without recursively packing.
    names = set()
    sum_bytes = 0
    for shard in raw_shards:
        _checked_name(shard.name)
        if shard.name in names:
            raise CatalogError("duplicate SCB1 shard name")
        names.add(shard.name)
        if not (1 <= shard.size <= MAX_SHARD_BYTES) or not (
            1 <= shard.character_count <= MAX_CHARACTERS
        ):
            raise CatalogError("SCB1 shard size/count out of range")
        if not (1 <= shard.first_rank <= shard.last_rank <= char_count):
            raise CatalogError("SCB1 shard rank range invalid")
        if shard.last_rank - shard.first_rank + 1 != shard.character_count:
            raise CatalogError("SCB1 shard rank range/count mismatch")
        sum_bytes += shard.size
    if sum_bytes != total_bytes:
        raise CatalogError("SCB1 total shard size mismatch")

    seen_cp = set()
    seen_rank = set()
    locals_by_shard: List[set[int]] = [set() for _ in raw_shards]
    previous_cp = 0
    for item in raw_entries:
        if not (0x4E00 <= item.codepoint <= 0x9FFF) or item.codepoint <= previous_cp:
            raise CatalogError("SCB1 codepoints must strictly increase")
        if item.codepoint in seen_cp or item.rank in seen_rank:
            raise CatalogError("duplicate SCB1 codepoint or rank")
        if not (1 <= item.rank <= char_count) or not (0 <= item.shard < shard_count):
            raise CatalogError("SCB1 entry rank/shard invalid")
        shard = raw_shards[item.shard]
        if not (shard.first_rank <= item.rank <= shard.last_rank):
            raise CatalogError("SCB1 entry rank outside shard range")
        if not (0 <= item.local_index < shard.character_count):
            raise CatalogError("SCB1 local index out of range")
        if item.local_index in locals_by_shard[item.shard]:
            raise CatalogError("duplicate SCB1 shard local index")
        locals_by_shard[item.shard].add(item.local_index)
        seen_cp.add(item.codepoint)
        seen_rank.add(item.rank)
        previous_cp = item.codepoint
    if seen_rank != set(range(1, char_count + 1)):
        raise CatalogError("SCB1 ranks must be continuous")
    for index, shard in enumerate(raw_shards):
        if locals_by_shard[index] != set(range(shard.character_count)):
            raise CatalogError("SCB1 shard local indexes must be complete")

    return {
        "character_count": char_count,
        "entries": raw_entries,
        "shard_count": shard_count,
        "shards": raw_shards,
        "size": len(data),
        "total_shard_bytes": total_bytes,
    }
