"""Lossless DZZ1 transform for the unchanged normalized SOB1 character record.

Geometry limits are intentionally identical to v1; DZZ1 is not an SVG parser.
"""
from __future__ import annotations

import struct

from .v2 import FormatError, MAX_RAW_BYTES, MAX_TRANSFORMED_BYTES, codepoint, require


def inspect_raw(raw: bytes) -> list[tuple[list[tuple[int, int]], list[tuple[int, int]]]]:
    require(isinstance(raw, bytes) and 8 <= len(raw) <= MAX_RAW_BYTES, "raw length")
    cp, strokes, reserved = struct.unpack_from("<IHH", raw)
    codepoint(cp)
    require(1 <= strokes <= 48 and reserved == 0, "raw header")
    pos = 8
    result = []
    for _ in range(strokes):
        require(pos + 4 <= len(raw), "truncated stroke header")
        outline, median = struct.unpack_from("<HH", raw, pos)
        pos += 4
        require(4 <= outline <= 256 and 2 <= median <= 64, "point counts")
        paths = []
        for count in (outline, median):
            require(pos + count * 4 <= len(raw), "truncated points")
            points = [struct.unpack_from("<HH", raw, pos + i * 4) for i in range(count)]
            require(all(x <= 1024 and y <= 1024 for x, y in points), "coordinate range")
            paths.append(points)
            pos += count * 4
        require(paths[0][0] == paths[0][-1], "outline not closed")
        result.append((paths[0], paths[1]))
    require(pos == len(raw), "raw trailing input")
    return result


def encode(raw: bytes) -> bytes:
    strokes = inspect_raw(raw)
    out = bytearray(b"DZZ1" + raw[:8])
    for outline, median in strokes:
        out.extend(struct.pack("<HH", len(outline), len(median)))
        for points in (outline, median):
            previous = (0, 0)
            for point in points:
                for value, prev in zip(point, previous):
                    delta = value - prev
                    zigzag = 2 * delta if delta >= 0 else -2 * delta - 1
                    if zigzag < 128:
                        out.append(zigzag)
                    else:
                        out.extend(((zigzag & 127) | 128, zigzag >> 7))
                previous = point
    require(len(out) <= len(raw) + 4, "internal transform bound")
    return bytes(out)


def decode(data: bytes, raw_length: int) -> bytes:
    require(isinstance(data, bytes) and 12 <= len(data) <= MAX_TRANSFORMED_BYTES,
            "DZZ1 length")
    require(8 <= raw_length <= MAX_RAW_BYTES and len(data) <= raw_length + 4,
            "raw/transform bound")
    require(data[:4] == b"DZZ1", "DZZ1 magic")
    cp, strokes, reserved = struct.unpack_from("<IHH", data, 4)
    codepoint(cp)
    require(1 <= strokes <= 48 and reserved == 0, "DZZ1 header")
    out = bytearray(data[4:12])
    pos = 12
    for _ in range(strokes):
        require(pos + 4 <= len(data), "truncated DZZ1 counts")
        outline, median = struct.unpack_from("<HH", data, pos)
        require(4 <= outline <= 256 and 2 <= median <= 64, "DZZ1 point counts")
        require(len(out) + 4 + 4 * (outline + median) <= raw_length, "DZZ1 output overflow")
        out.extend(data[pos:pos + 4])
        pos += 4
        for count in (outline, median):
            previous = [0, 0]
            for _ in range(count):
                for axis in range(2):
                    require(pos < len(data), "truncated varint")
                    value = data[pos]
                    pos += 1
                    if value & 128:
                        require(pos < len(data), "truncated varint continuation")
                        high = data[pos]
                        pos += 1
                        require(1 <= high <= 16, "non-minimal or overflowing varint")
                        value = (value & 127) | (high << 7)
                    require(value <= 2048, "zigzag overflow")
                    delta = value // 2 if not value & 1 else -(value // 2) - 1
                    coordinate = previous[axis] + delta
                    require(0 <= coordinate <= 1024, "DZZ1 coordinate overflow")
                    out.extend(struct.pack("<H", coordinate))
                    previous[axis] = coordinate
    require(pos == len(data) and len(out) == raw_length, "DZZ1 trailing/missing input")
    raw = bytes(out)
    inspect_raw(raw)
    return raw
