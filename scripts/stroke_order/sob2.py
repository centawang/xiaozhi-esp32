"""SOB2 independent compressed glyph blocks; rank-ordered greedy shards."""
from __future__ import annotations

from dataclasses import dataclass
import struct

from . import dzz1, heatshrink
from .v2 import (HEADER_SIZE, MAX_RAW_BYTES, MAX_SHARD_BYTES, MAX_SHARDS,
                 MAX_STORED_BYTES, MAX_TRANSFORMED_BYTES, PROFILE, codepoint,
                 crc, envelope, open_envelope, require)

INDEX = struct.Struct("<7I2H")
# Low to high bytes: heatshrink codec 1, DZZ1 transform 1, window 10, lookahead 4.
FLAGS = 0x040A0101


@dataclass(frozen=True)
class Block:
    codepoint: int
    rank: int
    stored: bytes
    transformed_length: int
    raw_length: int
    raw_crc: int


def encode_record(raw: bytes, rank: int, codec: heatshrink.ReferenceCodec) -> Block:
    transformed = dzz1.encode(raw)
    require(1 <= rank <= PROFILE, "rank outside profile")
    stored = codec.encode(transformed)
    return Block(struct.unpack_from("<I", raw)[0], rank, stored,
                 len(transformed), len(raw), crc(raw))


def pack_sob2(blocks: list[Block], corpus: bytes) -> bytes:
    require(1 <= len(blocks) <= PROFILE, "SOB2 count")
    offset = HEADER_SIZE + INDEX.size * len(blocks)
    index = bytearray()
    payload = bytearray()
    for block in blocks:
        index.extend(INDEX.pack(block.codepoint, offset + len(payload), len(block.stored),
                                block.transformed_length, block.raw_length, crc(block.stored),
                                block.raw_crc, block.rank, 0))
        payload.extend(block.stored)
    data = envelope(b"SOB2", corpus, len(blocks), blocks[0].rank, HEADER_SIZE,
                    offset, FLAGS, bytes(index + payload))
    validate_sob2(data, corpus)
    return data


def validate_sob2(data: bytes, corpus: bytes | None = None) -> dict:
    header = open_envelope(data, b"SOB2", MAX_SHARD_BYTES, corpus)
    n, first = header["count"], header["aux"]
    require(1 <= n <= PROFILE and 1 <= first <= first + n - 1 <= PROFILE, "SOB2 ranks/count")
    cursor = HEADER_SIZE + INDEX.size * n
    require(header["flags"] == FLAGS, "unknown SOB2 codec/transform/parameters")
    require(header["offset1"] == HEADER_SIZE and header["offset2"] == cursor <= len(data),
            "SOB2 index/data offset")
    characters = []
    seen = set()
    for local in range(n):
        cp, off, stored, transformed, raw_len, stored_crc, raw_crc, rank, reserved = INDEX.unpack_from(
            data, HEADER_SIZE + local * INDEX.size)
        codepoint(cp)
        require(cp not in seen and rank == first + local and reserved == 0,
                "SOB2 duplicate codepoint/non-contiguous rank/reserved")
        seen.add(cp)
        require(1 <= stored <= MAX_STORED_BYTES and 12 <= transformed <= MAX_TRANSFORMED_BYTES
                and 8 <= raw_len <= MAX_RAW_BYTES and transformed <= raw_len + 4,
                "SOB2 block lengths")
        require(off == cursor and stored <= len(data) - cursor, "SOB2 offset/length overflow")
        payload = data[off:off + stored]
        require(crc(payload) == stored_crc, "SOB2 stored CRC")
        raw = dzz1.decode(heatshrink.decode(payload, transformed), raw_len)
        require(crc(raw) == raw_crc and struct.unpack_from("<I", raw)[0] == cp,
                "SOB2 raw CRC/codepoint mismatch")
        characters.append(dict(codepoint=cp, rank=rank, raw=raw, local=local,
                               stored_bytes=stored, transformed_bytes=transformed))
        cursor += stored
    require(cursor == len(data), "SOB2 trailing input")
    return dict(**header, characters=characters, first_rank=first, last_rank=first + n - 1)


def shard_blocks(blocks: list[Block], corpus: bytes) -> list[bytes]:
    require(len(blocks) == PROFILE, "3500 profile requires exactly 3500 glyphs")
    require([x.rank for x in blocks] == list(range(1, PROFILE + 1)), "corpus ranks not contiguous")
    require(len({x.codepoint for x in blocks}) == PROFILE, "duplicate corpus codepoint")
    batches = []
    batch = []
    size = HEADER_SIZE
    for block in blocks:
        need = INDEX.size + len(block.stored)
        require(HEADER_SIZE + need <= MAX_SHARD_BYTES, "single glyph exceeds shard budget")
        if batch and size + need > MAX_SHARD_BYTES:
            batches.append(pack_sob2(batch, corpus))
            batch, size = [], HEADER_SIZE
        batch.append(block)
        size += need
    if batch:
        batches.append(pack_sob2(batch, corpus))
    require(len(batches) <= MAX_SHARDS, "too many SOB2 shards")
    return batches
