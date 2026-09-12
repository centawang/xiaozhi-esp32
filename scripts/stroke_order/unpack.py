"""Independent SOB1 validator and single-character loader.

This is a host-side mirror of the device StrokeOrderStore rules: it never trusts
offset/length fields, uses saturating unsigned 32-bit checks, and a failed load
leaves any previously successful character untouched.
"""

from __future__ import annotations

import struct
import zlib
from typing import Dict, List, Optional, Tuple

from .constants import (
    CHARACTER_RECORD_HEADER_SIZE,
    COORD_MAX,
    FORMAT_VERSION,
    HEADER_SIZE,
    HEADER_STRUCT_FORMAT,
    INDEX_ENTRY_SIZE,
    INDEX_STRUCT_FORMAT,
    MAGIC,
    MAX_CHARACTER_BYTES,
    MAX_CHARACTERS,
    MAX_FILE_BYTES,
    MAX_TARGET_CODEPOINT,
    MAX_MEDIAN_POINTS_PER_STROKE,
    MAX_OUTLINE_POINTS_PER_STROKE,
    MAX_STROKES_PER_CHARACTER,
    MIN_MEDIAN_POINTS_PER_STROKE,
    MIN_OUTLINE_POINTS_PER_STROKE,
    MIN_STROKES_PER_CHARACTER,
    MIN_TARGET_CODEPOINT,
    POINT_SIZE,
    STROKE_HEADER_SIZE,
    STROKE_HEADER_STRUCT_FORMAT,
    U32_MAX,
)

Point = Tuple[int, int]


class ValidationError(ValueError):
    """Raised when a blob or character record is invalid."""


class StrokeOrderBlob:
    """Bounded read-only view over a stroke_order.bin blob."""

    def __init__(self) -> None:
        self._data: Optional[bytes] = None
        self._char_count = 0
        self._index_offset = 0
        self._entries: List[Tuple[int, int, int, int]] = []
        self._loaded_codepoint: Optional[int] = None
        self._strokes: List[Dict[str, List[Point]]] = []

    @property
    def is_bound(self) -> bool:
        return self._data is not None

    @property
    def character_count(self) -> int:
        return self._char_count

    @property
    def has_character(self) -> bool:
        return self._loaded_codepoint is not None

    @property
    def loaded_codepoint(self) -> Optional[int]:
        return self._loaded_codepoint

    @property
    def strokes(self) -> List[Dict[str, List[Point]]]:
        return self._strokes

    def bind(self, data: bytes) -> bool:
        try:
            parsed = _parse_header_and_index(data)
        except ValidationError:
            return False
        self._data = data
        self._char_count = parsed["char_count"]
        self._index_offset = parsed["index_offset"]
        self._entries = parsed["entries"]
        self._loaded_codepoint = None
        self._strokes = []
        return True

    def unbind(self) -> None:
        self._data = None
        self._char_count = 0
        self._index_offset = 0
        self._entries = []
        self._loaded_codepoint = None
        self._strokes = []

    def contains(self, codepoint: int) -> bool:
        return self._find_entry(codepoint) is not None

    def codepoint_at(self, index: int) -> Optional[int]:
        if self._data is None or index < 0 or index >= len(self._entries):
            return None
        return self._entries[index][0]

    def load_character(self, codepoint: int) -> bool:
        if self._data is None:
            return False
        entry = self._find_entry(codepoint)
        if entry is None:
            return False
        try:
            strokes = _parse_character_record(self._data, entry)
        except ValidationError:
            return False
        self._loaded_codepoint = codepoint
        self._strokes = strokes
        return True

    def _find_entry(self, codepoint: int) -> Optional[Tuple[int, int, int, int]]:
        lo = 0
        hi = len(self._entries) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            found = self._entries[mid][0]
            if found == codepoint:
                return self._entries[mid]
            if found < codepoint:
                lo = mid + 1
            else:
                hi = mid - 1
        return None


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def u32_add(a: int, b: int) -> int:
    if a < 0 or b < 0 or a > U32_MAX or b > U32_MAX:
        raise ValidationError("u32 add overflow")
    total = a + b
    if total > U32_MAX:
        raise ValidationError("u32 add overflow")
    return total


def u32_mul(a: int, b: int) -> int:
    if a < 0 or b < 0 or a > U32_MAX or b > U32_MAX:
        raise ValidationError("u32 mul overflow")
    total = a * b
    if total > U32_MAX:
        raise ValidationError("u32 mul overflow")
    return total


def validate_blob(data: bytes, load_all: bool = True) -> Dict[str, object]:
    """Validate a complete blob. Used by the converter as a post-check."""
    parsed = _parse_header_and_index(data)
    characters = []
    if load_all:
        for entry in parsed["entries"]:
            strokes = _parse_character_record(data, entry)
            characters.append({"codepoint": entry[0], "strokes": strokes})
    parsed["characters"] = characters
    return parsed


def load_character(data: bytes, codepoint: int) -> List[Dict[str, List[Point]]]:
    blob = StrokeOrderBlob()
    if not blob.bind(data):
        raise ValidationError("invalid blob")
    if not blob.load_character(codepoint):
        raise ValidationError("character load failed")
    return blob.strokes


def _parse_header_and_index(data: bytes) -> Dict[str, object]:
    if not isinstance(data, (bytes, bytearray)):
        raise ValidationError("blob is not bytes")
    size = len(data)
    if size < HEADER_SIZE or size > MAX_FILE_BYTES:
        raise ValidationError("truncated or oversized blob")

    (
        magic,
        version,
        coord_max,
        char_count,
        index_offset,
        data_offset,
        data_size,
        header_crc,
        index_crc,
    ) = struct.unpack_from(HEADER_STRUCT_FORMAT, data, 0)
    if magic != MAGIC:
        raise ValidationError("bad magic")
    if version != FORMAT_VERSION:
        raise ValidationError("unsupported version")
    if coord_max != COORD_MAX:
        raise ValidationError("invalid header fields")
    expected_crc = crc32(data[0:24])
    if expected_crc != header_crc:
        raise ValidationError("header checksum mismatch")
    if char_count > MAX_CHARACTERS:
        raise ValidationError("character count exceeds limit")
    if index_offset != HEADER_SIZE:
        raise ValidationError("unexpected index offset")

    index_bytes = u32_mul(char_count, INDEX_ENTRY_SIZE)
    expected_data_offset = u32_add(index_offset, index_bytes)
    if data_offset != expected_data_offset:
        raise ValidationError("unexpected data offset")
    if data_offset > size:
        raise ValidationError("index range out of file")
    if crc32(data[index_offset:data_offset]) != index_crc:
        raise ValidationError("index checksum mismatch")
    data_end = u32_add(data_offset, data_size)
    if data_end != size:
        raise ValidationError("data_size does not fill the file")
    if data_offset > size or data_end > size:
        raise ValidationError("index/data range out of file")

    entries: List[Tuple[int, int, int, int]] = []
    prev_cp = -1
    cursor = data_offset
    offset = index_offset
    for _ in range(char_count):
        end = u32_add(offset, INDEX_ENTRY_SIZE)
        if end > size:
            raise ValidationError("truncated index")
        codepoint, rec_offset, rec_length, rec_crc = struct.unpack_from(
            INDEX_STRUCT_FORMAT, data, offset
        )
        _check_codepoint(codepoint)
        if codepoint <= prev_cp:
            raise ValidationError("index is not strictly sorted")
        if rec_length < CHARACTER_RECORD_HEADER_SIZE or rec_length > MAX_CHARACTER_BYTES:
            raise ValidationError("character length out of range")
        rec_end = u32_add(rec_offset, rec_length)
        if rec_offset != cursor or rec_end > data_end or rec_offset < data_offset:
            raise ValidationError("malicious or non-contiguous character offset")
        entries.append((codepoint, rec_offset, rec_length, rec_crc))
        prev_cp = codepoint
        cursor = rec_end
        offset = end
    if cursor != data_end:
        raise ValidationError("character records do not fill data region")
    return {
        "char_count": char_count,
        "index_offset": index_offset,
        "data_offset": data_offset,
        "data_size": data_size,
        "entries": entries,
    }


def _check_codepoint(codepoint: int) -> None:
    if codepoint < MIN_TARGET_CODEPOINT or codepoint > MAX_TARGET_CODEPOINT:
        raise ValidationError("codepoint is outside the supported CJK Unified Ideographs block")


def _parse_character_record(
    data: bytes, entry: Tuple[int, int, int, int]
) -> List[Dict[str, List[Point]]]:
    codepoint, offset, length, expected_crc = entry
    end = u32_add(offset, length)
    if end > len(data):
        raise ValidationError("character range out of file")
    record = data[offset:end]
    if len(record) != length:
        raise ValidationError("truncated character")
    if crc32(record) != expected_crc:
        raise ValidationError("character checksum mismatch")
    if length < CHARACTER_RECORD_HEADER_SIZE:
        raise ValidationError("character header truncated")

    rec_cp, stroke_count, reserved = struct.unpack_from("<IHH", record, 0)
    if rec_cp != codepoint or reserved != 0:
        raise ValidationError("character header mismatch")
    if (
        stroke_count < MIN_STROKES_PER_CHARACTER
        or stroke_count > MAX_STROKES_PER_CHARACTER
    ):
        raise ValidationError("stroke count out of range")

    pos = CHARACTER_RECORD_HEADER_SIZE
    strokes: List[Dict[str, List[Point]]] = []
    for _ in range(stroke_count):
        pos, stroke = _parse_stroke(record, pos)
        strokes.append(stroke)
    if pos != length:
        raise ValidationError("character record has leftover or missing bytes")
    return strokes


def _parse_stroke(record: bytes, pos: int) -> Tuple[int, Dict[str, List[Point]]]:
    need = u32_add(pos, STROKE_HEADER_SIZE)
    if need > len(record):
        raise ValidationError("truncated stroke header")
    outline_count, median_count = struct.unpack_from(STROKE_HEADER_STRUCT_FORMAT, record, pos)
    pos = need
    if (
        outline_count < MIN_OUTLINE_POINTS_PER_STROKE
        or outline_count > MAX_OUTLINE_POINTS_PER_STROKE
    ):
        raise ValidationError("outline point count out of range")
    if (
        median_count < MIN_MEDIAN_POINTS_PER_STROKE
        or median_count > MAX_MEDIAN_POINTS_PER_STROKE
    ):
        raise ValidationError("median point count out of range")

    outline_bytes = u32_mul(outline_count, POINT_SIZE)
    median_bytes = u32_mul(median_count, POINT_SIZE)
    outline_end = u32_add(pos, outline_bytes)
    median_end = u32_add(outline_end, median_bytes)
    if median_end > len(record):
        raise ValidationError("truncated stroke points")

    outline = _read_points(record, pos, outline_count)
    median = _read_points(record, outline_end, median_count)
    if outline[0] != outline[-1]:
        raise ValidationError("outline is not closed")
    return median_end, {"outline": outline, "median": median}


def _read_points(record: bytes, offset: int, count: int) -> List[Point]:
    points: List[Point] = []
    pos = offset
    for _ in range(count):
        x, y = struct.unpack_from("<HH", record, pos)
        if x > COORD_MAX or y > COORD_MAX:
            raise ValidationError("point out of design space")
        points.append((x, y))
        pos = u32_add(pos, POINT_SIZE)
    return points
