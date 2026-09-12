"""Restricted SVG path parser with deterministic offline curve flattening."""

from __future__ import annotations

import math
import re
from typing import List, Sequence, Tuple

from .constants import (
    COORD_MAX,
    FLATTEN_MAX_DEPTH,
    FLATTEN_TOLERANCE,
    MAX_MEDIAN_POINTS_PER_STROKE,
    MIN_MEDIAN_POINTS_PER_STROKE,
    MIN_OUTLINE_POINTS_PER_STROKE,
    PACK_OUTLINE_POINTS_PER_STROKE,
)

# Make Me a Hanzi 1024-space control points can sit slightly outside the box.
# Clamp that slop into SOB1; reject coordinates far outside the viewBox.
SOURCE_COORD_SLOP = 128

Point = Tuple[float, float]
IntPoint = Tuple[int, int]

_ALLOWED_COMMANDS = set("MLQCZmlqcz")
_NUMBER_RE = re.compile(r"[+-]?(?:\d+\.\d*|\.\d+|\d+)")


class PathError(ValueError):
    """Raised when a stroke outline path is not in the accepted subset."""


def parse_outline_path(path: str) -> List[IntPoint]:
    """Parse one restricted SVG path into a closed y-down integer polyline."""
    if not isinstance(path, str) or not path.strip():
        raise PathError("empty path")
    tokens = _tokenize(path)
    raw_points = _commands_to_points(tokens)
    if len(raw_points) < 3:
        raise PathError("path has too few points")
    return _to_device_points(raw_points, closed=True)


def convert_median_points(points: Sequence[Sequence[float]]) -> List[IntPoint]:
    """Convert a median polyline to y-down integer points."""
    if not isinstance(points, (list, tuple)) or len(points) < 2:
        raise PathError("median must contain at least two points")
    raw: List[Point] = []
    for item in points:
        if not isinstance(item, (list, tuple)) or len(item) != 2:
            raise PathError("median point must be [x, y]")
        try:
            x = float(item[0])
            y = float(item[1])
        except (TypeError, ValueError, OverflowError) as exc:
            raise PathError("median point is not numeric") from exc
        if not math.isfinite(x) or not math.isfinite(y):
            raise PathError("median point must be finite")
        raw.append((x, y))
    converted = _to_device_points(raw, closed=False)
    if len(converted) < 2:
        raise PathError("median collapsed below two points")
    return converted


def flatten_quadratic(p0: Point, p1: Point, p2: Point) -> List[Point]:
    """Flatten a quadratic Bezier; omits p0 so callers can concatenate."""
    out: List[Point] = []
    _flatten_quadratic(p0, p1, p2, 0, out)
    return out


def flatten_cubic(p0: Point, p1: Point, p2: Point, p3: Point) -> List[Point]:
    """Flatten a cubic Bezier; omits p0 so callers can concatenate."""
    out: List[Point] = []
    _flatten_cubic(p0, p1, p2, p3, 0, out)
    return out


def _tokenize(path: str) -> List[Tuple[str, object]]:
    tokens: List[Tuple[str, object]] = []
    i = 0
    n = len(path)
    while i < n:
        ch = path[i]
        if ch.isspace() or ch == ",":
            i += 1
            continue
        if ch in _ALLOWED_COMMANDS:
            tokens.append(("cmd", ch))
            i += 1
            continue
        if ch in "HhVvSsTtAa":
            raise PathError(f"unknown command {ch}")
        match = _NUMBER_RE.match(path, i)
        if match is None:
            raise PathError(f"unexpected character {ch!r}")
        token = match.group()
        if "e" in token.lower():
            raise PathError("scientific notation is not allowed")
        value = float(token)
        if not math.isfinite(value):
            raise PathError("path number must be finite")
        tokens.append(("num", value))
        i = match.end()
    if not tokens:
        raise PathError("empty path")
    return tokens


def _commands_to_points(tokens: Sequence[Tuple[str, object]]) -> List[Point]:
    i = 0
    n = len(tokens)
    points: List[Point] = []
    cx = 0.0
    cy = 0.0
    start_x = 0.0
    start_y = 0.0
    have_start = False
    closed = False
    current_cmd = None

    def need_nums(count: int) -> List[float]:
        nonlocal i
        values: List[float] = []
        while len(values) < count:
            if i >= n or tokens[i][0] != "num":
                raise PathError("missing path numbers")
            values.append(float(tokens[i][1]))
            i += 1
        return values

    if tokens[0][0] != "cmd" or tokens[0][1] not in ("M", "m"):
        raise PathError("path must start with M")

    while i < n:
        kind, value = tokens[i]
        if kind == "cmd":
            cmd = str(value)
            i += 1
            current_cmd = cmd
            if cmd in "Zz":
                if not have_start:
                    raise PathError("close without move")
                if not points or points[-1] != (start_x, start_y):
                    points.append((start_x, start_y))
                cx, cy = start_x, start_y
                closed = True
                current_cmd = None
                continue
        else:
            if current_cmd is None:
                raise PathError("number without command")
            cmd = current_cmd

        if cmd in "Mm":
            if have_start:
                raise PathError("multiple subpaths are not allowed")
            x, y = need_nums(2)
            if cmd == "m":
                x += cx
                y += cy
            cx, cy = x, y
            start_x, start_y = x, y
            have_start = True
            closed = False
            points.append((cx, cy))
            current_cmd = "L" if cmd == "M" else "l"
            continue

        if cmd in "Ll":
            x, y = need_nums(2)
            if cmd == "l":
                x += cx
                y += cy
            _append_line(points, (cx, cy), (x, y))
            cx, cy = x, y
            closed = False
            continue

        if cmd in "Qq":
            x1, y1, x, y = need_nums(4)
            if cmd == "q":
                x1 += cx
                y1 += cy
                x += cx
                y += cy
            _append_quadratic(points, (cx, cy), (x1, y1), (x, y))
            cx, cy = x, y
            closed = False
            continue

        if cmd in "Cc":
            x1, y1, x2, y2, x, y = need_nums(6)
            if cmd == "c":
                x1 += cx
                y1 += cy
                x2 += cx
                y2 += cy
                x += cx
                y += cy
            _append_cubic(points, (cx, cy), (x1, y1), (x2, y2), (x, y))
            cx, cy = x, y
            closed = False
            continue

        raise PathError(f"unknown command {cmd}")

    if not closed:
        raise PathError("path is not closed")
    return points


def _append_line(points: List[Point], start: Point, end: Point) -> None:
    if not points:
        points.append(start)
    points.append(end)


def _append_quadratic(points: List[Point], p0: Point, p1: Point, p2: Point) -> None:
    if not points:
        points.append(p0)
    points.extend(flatten_quadratic(p0, p1, p2))


def _append_cubic(points: List[Point], p0: Point, p1: Point, p2: Point, p3: Point) -> None:
    if not points:
        points.append(p0)
    points.extend(flatten_cubic(p0, p1, p2, p3))


def _flatten_quadratic(p0: Point, p1: Point, p2: Point, depth: int, out: List[Point]) -> None:
    if depth >= FLATTEN_MAX_DEPTH or _quadratic_flat(p0, p1, p2):
        out.append(p2)
        return
    q0 = _mid(p0, p1)
    q1 = _mid(p1, p2)
    mid = _mid(q0, q1)
    _flatten_quadratic(p0, q0, mid, depth + 1, out)
    _flatten_quadratic(mid, q1, p2, depth + 1, out)


def _flatten_cubic(
    p0: Point, p1: Point, p2: Point, p3: Point, depth: int, out: List[Point]
) -> None:
    if depth >= FLATTEN_MAX_DEPTH or _cubic_flat(p0, p1, p2, p3):
        out.append(p3)
        return
    p01 = _mid(p0, p1)
    p12 = _mid(p1, p2)
    p23 = _mid(p2, p3)
    p012 = _mid(p01, p12)
    p123 = _mid(p12, p23)
    mid = _mid(p012, p123)
    _flatten_cubic(p0, p01, p012, mid, depth + 1, out)
    _flatten_cubic(mid, p123, p23, p3, depth + 1, out)


def _quadratic_flat(p0: Point, p1: Point, p2: Point) -> bool:
    return _point_to_segment_distance(p1, p0, p2) <= FLATTEN_TOLERANCE


def _cubic_flat(p0: Point, p1: Point, p2: Point, p3: Point) -> bool:
    return (
        _point_to_segment_distance(p1, p0, p3) <= FLATTEN_TOLERANCE
        and _point_to_segment_distance(p2, p0, p3) <= FLATTEN_TOLERANCE
    )


def _mid(a: Point, b: Point) -> Point:
    return ((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5)


def _point_to_segment_distance(point: Point, a: Point, b: Point) -> float:
    px, py = point
    ax, ay = a
    bx, by = b
    vx = bx - ax
    vy = by - ay
    length2 = vx * vx + vy * vy
    if length2 == 0.0:
        dx = px - ax
        dy = py - ay
        return (dx * dx + dy * dy) ** 0.5
    t = ((px - ax) * vx + (py - ay) * vy) / length2
    t = 0.0 if t < 0.0 else 1.0 if t > 1.0 else t
    dx = px - (ax + t * vx)
    dy = py - (ay + t * vy)
    return (dx * dx + dy * dy) ** 0.5


def _clamp_source_coord(value: float) -> float:
    if not math.isfinite(value):
        raise PathError("coordinate must be finite")
    if -SOURCE_COORD_SLOP <= value <= COORD_MAX + SOURCE_COORD_SLOP:
        if value < 0:
            return 0.0
        if value > COORD_MAX:
            return float(COORD_MAX)
        return value
    raise PathError("coordinate out of 0..1024 after conversion")


def _dedup_consecutive(points: Sequence[IntPoint]) -> List[IntPoint]:
    converted: List[IntPoint] = []
    for point in points:
        if not converted or converted[-1] != point:
            converted.append(point)
    return converted


def _resample_polyline(points: Sequence[IntPoint], count: int) -> List[IntPoint]:
    if count <= 0 or not points:
        raise PathError("cannot resample empty polyline")
    if len(points) == 1:
        return [points[0]] * count
    if len(points) == count:
        return list(points)
    last = len(points) - 1
    sampled: List[IntPoint] = []
    for i in range(count):
        src = int(round(i * last / (count - 1))) if count > 1 else 0
        sampled.append(points[src])
    sampled[0] = points[0]
    sampled[-1] = points[-1]
    return sampled


def _fit_closed_outline(points: Sequence[IntPoint]) -> List[IntPoint]:
    converted = _dedup_consecutive(points)
    if len(converted) < 3:
        raise PathError("closed path collapsed")
    if converted[0] != converted[-1]:
        converted.append(converted[0])
    if len(converted) < MIN_OUTLINE_POINTS_PER_STROKE:
        raise PathError("closed path has too few points")
    if len(converted) <= PACK_OUTLINE_POINTS_PER_STROKE:
        return converted
    unique = converted[:-1]
    target_unique = PACK_OUTLINE_POINTS_PER_STROKE - 1
    if target_unique < MIN_OUTLINE_POINTS_PER_STROKE - 1:
        raise PathError("closed path has too few points")
    sampled = _resample_polyline(unique, target_unique)
    fitted = _dedup_consecutive(sampled + [sampled[0]])
    if fitted[0] != fitted[-1]:
        fitted.append(fitted[0])
    if not (
        MIN_OUTLINE_POINTS_PER_STROKE <= len(fitted) <= PACK_OUTLINE_POINTS_PER_STROKE
    ):
        raise PathError("closed path exceeds outline point limit")
    return fitted


def _fit_median(points: Sequence[IntPoint]) -> List[IntPoint]:
    converted = _dedup_consecutive(points)
    if len(converted) < MIN_MEDIAN_POINTS_PER_STROKE:
        raise PathError("median collapsed below two points")
    if len(converted) <= MAX_MEDIAN_POINTS_PER_STROKE:
        return converted
    fitted = _dedup_consecutive(
        _resample_polyline(converted, MAX_MEDIAN_POINTS_PER_STROKE)
    )
    if not (MIN_MEDIAN_POINTS_PER_STROKE <= len(fitted) <= MAX_MEDIAN_POINTS_PER_STROKE):
        raise PathError("median point count out of range")
    return fitted


def _to_device_points(points: Sequence[Point], closed: bool) -> List[IntPoint]:
    converted: List[IntPoint] = []
    for x, y in points:
        ix = int(round(_clamp_source_coord(x)))
        iy = int(round(COORD_MAX - _clamp_source_coord(y)))
        if ix < 0 or ix > COORD_MAX or iy < 0 or iy > COORD_MAX:
            raise PathError("coordinate out of 0..1024 after conversion")
        converted.append((ix, iy))
    if closed:
        return _fit_closed_outline(converted)
    return _fit_median(converted)
