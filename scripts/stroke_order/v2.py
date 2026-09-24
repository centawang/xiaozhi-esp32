"""Common *new* v2 envelope. Does not change any v1 reader or limit."""
from __future__ import annotations

import struct
import zlib

PROFILE = 3500
MAX_SHARDS = 32
MAX_SHARD_BYTES = 1 << 20
MAX_RAW_BYTES = 16 * 1024
MAX_TRANSFORMED_BYTES = MAX_RAW_BYTES + 4
MAX_STORED_BYTES = (MAX_TRANSFORMED_BYTES * 9 + 7) // 8
HEADER = struct.Struct("<4sHH16s10I")
HEADER_SIZE = HEADER.size


class FormatError(ValueError):
    """Invalid or non-canonical v2 input."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise FormatError(message)


def crc(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def codepoint(cp: int) -> None:
    require(0x4E00 <= cp <= 0x9FFF, "codepoint outside Basic CJK")


def envelope(magic: bytes, corpus: bytes, count: int, aux: int,
             offset1: int, offset2: int, flags: int, body: bytes) -> bytes:
    require(isinstance(corpus, bytes) and len(corpus) == 16 and any(corpus),
            "corpus_id must be 16 nonzero bytes")
    header = bytearray(HEADER.pack(magic, 2, PROFILE, corpus, count, aux,
                                  offset1, offset2, HEADER_SIZE + len(body),
                                  crc(body), 0, flags, 0, 0))
    struct.pack_into("<I", header, 48, crc(header))
    return bytes(header) + body


def open_envelope(data: bytes, magic: bytes, maximum: int, corpus: bytes | None = None) -> dict:
    require(isinstance(data, bytes) and HEADER_SIZE <= len(data) <= maximum,
            "file size/type out of bounds")
    m, version, profile, cid, count, aux, off1, off2, size, body_crc, header_crc, flags, r1, r2 = HEADER.unpack_from(data)
    require((m, version, profile, r1, r2) == (magic, 2, PROFILE, 0, 0),
            "magic/version/profile/reserved mismatch")
    require(any(cid) and (corpus is None or cid == corpus), "mixed or empty corpus_id")
    header = bytearray(data[:HEADER_SIZE])
    struct.pack_into("<I", header, 48, 0)
    require(crc(header) == header_crc and crc(data[HEADER_SIZE:]) == body_crc,
            "header/body CRC mismatch")
    require(size == len(data), "file size mismatch/trailing input")
    return dict(corpus_id=cid, count=count, aux=aux, offset1=off1, offset2=off2, flags=flags)
