"""Offline SPY1 pinyin index: Unihan kMandarin (+ optional kHanyuPinyin).

Never accesses the network. Device storage is group id + rank, not strings.
"""

from __future__ import annotations

import hashlib
import json
import re
import struct
import unicodedata
import zipfile
import zlib
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from .pinyin_constants import (
    CHAR_INDEX_ENTRY_SIZE,
    CHAR_INDEX_STRUCT_FORMAT,
    FORMAT_VERSION,
    GROUP_INDEX_ENTRY_SIZE,
    GROUP_INDEX_STRUCT_FORMAT,
    HEADER_SIZE,
    HEADER_STRUCT_FORMAT,
    MAGIC,
    MAX_CHARACTERS,
    MAX_FILE_BYTES,
    MAX_GROUP_MEMBERS,
    MAX_GROUPS,
    MAX_READINGS_PER_CHARACTER,
    MAX_TARGET_CODEPOINT,
    MEMBER_SIZE,
    MEMBER_STRUCT_FORMAT,
    MIN_TARGET_CODEPOINT,
    PINYIN_FIELDS,
    PINYIN_SOURCE,
    READING_SIZE,
    UNICODE_LICENSE,
    U32_MAX,
)

NUMERIC_PINYIN_RE = re.compile(r"^[a-z]+[1-5]$")
UNIHAN_CP_RE = re.compile(r"^U\+[0-9A-F]{4,6}$")

TONE_CHAR = {
    "ā": ("a", 1),
    "á": ("a", 2),
    "ǎ": ("a", 3),
    "à": ("a", 4),
    "ē": ("e", 1),
    "é": ("e", 2),
    "ě": ("e", 3),
    "è": ("e", 4),
    "ī": ("i", 1),
    "í": ("i", 2),
    "ǐ": ("i", 3),
    "ì": ("i", 4),
    "ō": ("o", 1),
    "ó": ("o", 2),
    "ǒ": ("o", 3),
    "ò": ("o", 4),
    "ū": ("u", 1),
    "ú": ("u", 2),
    "ǔ": ("u", 3),
    "ù": ("u", 4),
    "ǖ": ("v", 1),
    "ǘ": ("v", 2),
    "ǚ": ("v", 3),
    "ǜ": ("v", 4),
    "ü": ("v", 0),
    "ê": ("e", 0),
    "ế": ("e", 2),
    "ề": ("e", 4),
    "ḿ": ("m", 2),
    "ń": ("n", 2),
    "ň": ("n", 3),
    "ǹ": ("n", 4),
}
COMBINING_TONE = {
    "\u0304": 1,
    "\u0301": 2,
    "\u030c": 3,
    "\u0300": 4,
}


class PinyinError(ValueError):
    """Raised when pinyin input or SPY1 output is invalid."""


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def u32_add(a: int, b: int) -> int:
    total = a + b
    if a < 0 or b < 0 or total > U32_MAX:
        raise PinyinError("unsigned 32-bit addition overflow")
    return total


def u32_mul(a: int, b: int) -> int:
    product = a * b
    if a < 0 or b < 0 or product > U32_MAX:
        raise PinyinError("unsigned 32-bit multiplication overflow")
    return product


def normalize_pinyin(raw: str) -> str:
    if not isinstance(raw, str):
        raise PinyinError("pinyin must be a string")
    text = raw.strip().lower().replace("u:", "v").replace("ü", "v")
    if not text:
        raise PinyinError("empty pinyin")
    if NUMERIC_PINYIN_RE.fullmatch(text):
        return text

    nfd = unicodedata.normalize("NFD", text)
    letters: List[str] = []
    tone = 0
    for char in nfd:
        if char in COMBINING_TONE:
            value = COMBINING_TONE[char]
            if tone not in (0, value):
                raise PinyinError(f"multiple tones in {raw!r}")
            tone = value
            continue
        if char == "\u0308":  # diaeresis on u -> v
            if not letters or letters[-1] != "u":
                raise PinyinError(f"unsafe pinyin {raw!r}")
            letters[-1] = "v"
            continue
        mapped = TONE_CHAR.get(char)
        if mapped is not None:
            base, mapped_tone = mapped
            letters.append(base)
            if mapped_tone:
                if tone not in (0, mapped_tone):
                    raise PinyinError(f"multiple tones in {raw!r}")
                tone = mapped_tone
            continue
        if "a" <= char <= "z":
            letters.append(char)
            continue
        raise PinyinError(f"unsafe pinyin {raw!r}")

    if not letters:
        raise PinyinError(f"unsafe pinyin {raw!r}")
    if tone == 0:
        tone = 5
    encoded = "".join(letters) + str(tone)
    if not NUMERIC_PINYIN_RE.fullmatch(encoded):
        raise PinyinError(f"unsafe pinyin {raw!r}")
    return encoded


def parse_kmandarin(value: str) -> List[str]:
    if not isinstance(value, str) or not value.strip():
        raise PinyinError("kMandarin is required")
    readings = []
    seen = set()
    for part in value.split():
        normalized = normalize_pinyin(part)
        if normalized not in seen:
            seen.add(normalized)
            readings.append(normalized)
    if not readings:
        raise PinyinError("kMandarin produced no readings")
    return readings


def parse_khanyu_pinyin(value: str) -> List[str]:
    if not isinstance(value, str) or not value.strip():
        return []
    readings: List[str] = []
    seen = set()
    for token in value.split():
        if ":" not in token:
            continue
        _, payload = token.rsplit(":", 1)
        for part in payload.split(","):
            item = part.strip()
            if not item:
                continue
            try:
                normalized = normalize_pinyin(item)
            except PinyinError:
                continue
            if normalized not in seen:
                seen.add(normalized)
                readings.append(normalized)
    return readings


def union_readings(kmandarin: str, khanyu: str = "") -> List[str]:
    primary = parse_kmandarin(kmandarin)
    extras = parse_khanyu_pinyin(khanyu)
    ordered = []
    seen = set()
    for item in primary + extras:
        if item in seen:
            continue
        seen.add(item)
        ordered.append(item)
    if len(ordered) > MAX_READINGS_PER_CHARACTER:
        raise PinyinError("pinyin reading union exceeds format limit; refusing to truncate")
    return ordered


def load_unihan_readings(zip_path: str, needed_codepoints: Sequence[int]) -> Dict[int, Dict[str, str]]:
    path = Path(zip_path)
    if not path.is_file():
        raise PinyinError("Unihan zip not found")
    needed = {f"U+{cp:04X}": cp for cp in needed_codepoints}
    found: Dict[int, Dict[str, str]] = {cp: {} for cp in needed_codepoints}
    try:
        with zipfile.ZipFile(path) as archive:
            if "Unihan_Readings.txt" not in archive.namelist():
                raise PinyinError("Unihan zip missing Unihan_Readings.txt")
            with archive.open("Unihan_Readings.txt") as handle:
                for raw in handle:
                    line = raw.decode("utf-8")
                    if line.startswith("#") or not line.strip():
                        continue
                    parts = line.rstrip("\n").split("\t")
                    if len(parts) < 3:
                        continue
                    key, field, value = parts[0], parts[1], parts[2]
                    if key not in needed or field not in PINYIN_FIELDS:
                        continue
                    found[needed[key]][field] = value
    except zipfile.BadZipFile as exc:
        raise PinyinError("Unihan zip is not a valid zip archive") from exc
    return found


def build_character_readings(
    rows: Sequence[Dict[str, object]],
    unihan: Dict[int, Dict[str, str]],
) -> List[Dict[str, object]]:
    characters: List[Dict[str, object]] = []
    missing: List[str] = []
    for row in rows:
        codepoint = int(row["codepoint_int"])
        fields = unihan.get(codepoint, {})
        mandarin = fields.get("kMandarin")
        if not mandarin:
            missing.append(str(row["character"]))
            continue
        readings = union_readings(mandarin, fields.get("kHanyuPinyin", ""))
        characters.append(
            {
                "character": str(row["character"]),
                "codepoint": codepoint,
                "rank": int(row["rank"]),
                "readings": readings,
                "kMandarin": mandarin,
                "kHanyuPinyin": fields.get("kHanyuPinyin", ""),
            }
        )
    if missing:
        raise PinyinError("kMandarin missing for: " + "".join(missing))
    if len(characters) != len(rows):
        raise PinyinError("SPY1 coverage is incomplete")
    return characters


def pack_spy1(characters: Sequence[Dict[str, object]]) -> bytes:
    if not isinstance(characters, (list, tuple)) or not characters:
        raise PinyinError("SPY1 requires a non-empty character list")
    if len(characters) > MAX_CHARACTERS:
        raise PinyinError("character count exceeds SPY1 limit")

    normalized: List[Dict[str, object]] = []
    seen_cp = set()
    seen_rank = set()
    all_syllables = set()
    for item in characters:
        codepoint = int(item["codepoint"])
        rank = int(item["rank"])
        readings = [normalize_pinyin(str(value)) for value in item["readings"]]
        if not (MIN_TARGET_CODEPOINT <= codepoint <= MAX_TARGET_CODEPOINT):
            raise PinyinError("SPY1 character outside Basic CJK")
        if codepoint in seen_cp:
            raise PinyinError("duplicate SPY1 codepoint")
        if rank < 1 or rank > 65535 or rank in seen_rank:
            raise PinyinError("SPY1 ranks must be unique and >= 1")
        if not readings or len(readings) > MAX_READINGS_PER_CHARACTER:
            raise PinyinError("SPY1 reading count out of range")
        deduped = []
        seen_reading = set()
        for reading in readings:
            if reading in seen_reading:
                continue
            seen_reading.add(reading)
            deduped.append(reading)
        seen_cp.add(codepoint)
        seen_rank.add(rank)
        all_syllables.update(deduped)
        normalized.append({"codepoint": codepoint, "rank": rank, "readings": deduped})

    syllables = sorted(all_syllables)
    if not syllables or len(syllables) > MAX_GROUPS:
        raise PinyinError("SPY1 group count out of range")
    group_id = {name: index for index, name in enumerate(syllables)}

    members: List[List[Tuple[int, int]]] = [[] for _ in syllables]
    for item in normalized:
        for reading in item["readings"]:
            gid = group_id[reading]
            members[gid].append((int(item["rank"]), int(item["codepoint"])))
    for index, group in enumerate(members):
        unique = {}
        for rank, codepoint in group:
            unique[codepoint] = rank
        ordered = sorted(((rank, cp) for cp, rank in unique.items()), key=lambda pair: (pair[0], pair[1]))
        if not (1 <= len(ordered) <= MAX_GROUP_MEMBERS):
            raise PinyinError(f"group {index} member count out of range")
        members[index] = ordered

    ordered_chars = sorted(normalized, key=lambda item: int(item["codepoint"]))
    char_count = len(ordered_chars)
    group_count = len(syllables)
    char_index_offset = HEADER_SIZE
    group_index_offset = u32_add(char_index_offset, char_count * CHAR_INDEX_ENTRY_SIZE)
    payload_offset = u32_add(group_index_offset, group_count * GROUP_INDEX_ENTRY_SIZE)

    reading_blobs = []
    char_entries = []
    cursor = payload_offset
    for item in ordered_chars:
        ids = [group_id[name] for name in item["readings"]]
        blob = b"".join(struct.pack("<H", gid) for gid in ids)
        char_entries.append((item, cursor, len(ids)))
        reading_blobs.append(blob)
        cursor = u32_add(cursor, len(blob))

    member_blobs = []
    group_entries = []
    for gid, group in enumerate(members):
        blob = b"".join(struct.pack(MEMBER_STRUCT_FORMAT, codepoint, rank, 0) for rank, codepoint in group)
        group_entries.append((len(group), cursor))
        member_blobs.append(blob)
        cursor = u32_add(cursor, len(blob))

    if cursor > MAX_FILE_BYTES:
        raise PinyinError("SPY1 file exceeds 64 KiB")

    char_index = b"".join(
        struct.pack(
            CHAR_INDEX_STRUCT_FORMAT,
            item["codepoint"],
            item["rank"],
            reading_count,
            0,
            offset,
        )
        for item, offset, reading_count in char_entries
    )
    group_index = b"".join(
        struct.pack(GROUP_INDEX_STRUCT_FORMAT, member_count, 0, offset)
        for member_count, offset in group_entries
    )
    body = char_index + group_index + b"".join(reading_blobs) + b"".join(member_blobs)
    header_without_crc = struct.pack(
        HEADER_STRUCT_FORMAT,
        MAGIC,
        FORMAT_VERSION,
        0,
        char_count,
        group_count,
        char_index_offset,
        group_index_offset,
        0,
        0,
    )
    header = struct.pack(
        HEADER_STRUCT_FORMAT,
        MAGIC,
        FORMAT_VERSION,
        0,
        char_count,
        group_count,
        char_index_offset,
        group_index_offset,
        crc32(header_without_crc[:24]),
        crc32(body),
    )
    blob = header + body
    if len(blob) != cursor:
        raise PinyinError("internal SPY1 packing size mismatch")
    validate_spy1(blob, load_all=True)
    return blob


def _read_u16(data: bytes, offset: int) -> int:
    end = u32_add(offset, 2)
    if end > len(data):
        raise PinyinError("SPY1 u16 out of range")
    return data[offset] | (data[offset + 1] << 8)


def _read_u32(data: bytes, offset: int) -> int:
    end = u32_add(offset, 4)
    if end > len(data):
        raise PinyinError("SPY1 u32 out of range")
    return (
        data[offset]
        | (data[offset + 1] << 8)
        | (data[offset + 2] << 16)
        | (data[offset + 3] << 24)
    )


def validate_spy1(data: bytes, load_all: bool = False) -> Dict[str, object]:
    if not isinstance(data, (bytes, bytearray)):
        raise PinyinError("SPY1 blob must be bytes")
    if len(data) < HEADER_SIZE or len(data) > MAX_FILE_BYTES:
        raise PinyinError("SPY1 size out of range")
    magic, version, reserved, char_count, group_count, char_index_offset, group_index_offset, header_crc, body_crc = struct.unpack_from(
        HEADER_STRUCT_FORMAT, data, 0
    )
    if magic != MAGIC:
        raise PinyinError("SPY1 magic mismatch")
    if version != FORMAT_VERSION or reserved != 0:
        raise PinyinError("SPY1 version/reserved mismatch")
    if not (1 <= char_count <= MAX_CHARACTERS) or not (1 <= group_count <= MAX_GROUPS):
        raise PinyinError("SPY1 counts out of range")
    if char_index_offset != HEADER_SIZE:
        raise PinyinError("SPY1 char index offset mismatch")
    expected_header_crc = crc32(bytes(data[:24]))
    if header_crc != expected_header_crc:
        raise PinyinError("SPY1 header CRC mismatch")
    if body_crc != crc32(bytes(data[HEADER_SIZE:])):
        raise PinyinError("SPY1 body CRC mismatch")

    char_index_bytes = u32_mul(char_count, CHAR_INDEX_ENTRY_SIZE)
    group_index_bytes = u32_mul(group_count, GROUP_INDEX_ENTRY_SIZE)
    expected_group_offset = u32_add(char_index_offset, char_index_bytes)
    if group_index_offset != expected_group_offset:
        raise PinyinError("SPY1 group index offset mismatch")
    index_end = u32_add(group_index_offset, group_index_bytes)
    if index_end > len(data):
        raise PinyinError("SPY1 index overruns file")

    chars = []
    prev_cp = 0
    seen_ranks = set()
    payload_cursor = index_end
    for i in range(char_count):
        offset = char_index_offset + i * CHAR_INDEX_ENTRY_SIZE
        codepoint = _read_u32(data, offset)
        rank = _read_u16(data, offset + 4)
        reading_count = data[offset + 6]
        reserved_u8 = data[offset + 7]
        readings_offset = _read_u32(data, offset + 8)
        if reserved_u8 != 0:
            raise PinyinError("SPY1 char reserved must be 0")
        if not (MIN_TARGET_CODEPOINT <= codepoint <= MAX_TARGET_CODEPOINT):
            raise PinyinError("SPY1 codepoint out of range")
        if prev_cp and codepoint <= prev_cp:
            raise PinyinError("SPY1 codepoints must strictly increase")
        if rank < 1 or rank in seen_ranks or not (
            1 <= reading_count <= MAX_READINGS_PER_CHARACTER
        ):
            raise PinyinError("SPY1 rank/reading_count invalid")
        seen_ranks.add(rank)
        reading_bytes = u32_mul(reading_count, READING_SIZE)
        reading_end = u32_add(readings_offset, reading_bytes)
        if readings_offset != payload_cursor or reading_end > len(data):
            raise PinyinError("SPY1 readings payload is not canonical")
        payload_cursor = reading_end
        group_ids = []
        seen = set()
        for r in range(reading_count):
            gid = _read_u16(data, readings_offset + r * READING_SIZE)
            if gid >= group_count or gid in seen:
                raise PinyinError("SPY1 group id invalid or duplicated")
            seen.add(gid)
            group_ids.append(gid)
        chars.append(
            {
                "codepoint": codepoint,
                "rank": rank,
                "readings": group_ids,
            }
        )
        prev_cp = codepoint

    groups = []
    for g in range(group_count):
        offset = group_index_offset + g * GROUP_INDEX_ENTRY_SIZE
        member_count = _read_u16(data, offset)
        reserved_u16 = _read_u16(data, offset + 2)
        members_offset = _read_u32(data, offset + 4)
        if reserved_u16 != 0:
            raise PinyinError("SPY1 group reserved must be 0")
        if not (1 <= member_count <= MAX_GROUP_MEMBERS):
            raise PinyinError("SPY1 member count out of range")
        member_bytes = u32_mul(member_count, MEMBER_SIZE)
        member_end = u32_add(members_offset, member_bytes)
        if members_offset != payload_cursor or member_end > len(data):
            raise PinyinError("SPY1 member payload is not canonical")
        payload_cursor = member_end
        group_members = []
        seen_cp = set()
        prev_key = None
        for m in range(member_count):
            m_off = members_offset + m * MEMBER_SIZE
            codepoint = _read_u32(data, m_off)
            rank = _read_u16(data, m_off + 4)
            reserved_member = _read_u16(data, m_off + 6)
            if reserved_member != 0:
                raise PinyinError("SPY1 member reserved must be 0")
            if not (MIN_TARGET_CODEPOINT <= codepoint <= MAX_TARGET_CODEPOINT) or rank < 1:
                raise PinyinError("SPY1 member codepoint/rank invalid")
            key = (rank, codepoint)
            if prev_key is not None and key <= prev_key:
                raise PinyinError("SPY1 members must be sorted by rank then codepoint")
            if codepoint in seen_cp:
                raise PinyinError("duplicate SPY1 group member")
            seen_cp.add(codepoint)
            group_members.append({"codepoint": codepoint, "rank": rank})
            prev_key = key
        groups.append(group_members)

    if payload_cursor != len(data):
        raise PinyinError("SPY1 payload has trailing or unreferenced bytes")

    by_cp = {item["codepoint"]: item for item in chars}
    if load_all:
        for item in chars:
            for gid in item["readings"]:
                members = groups[gid]
                match = next((m for m in members if m["codepoint"] == item["codepoint"]), None)
                if match is None or match["rank"] != item["rank"]:
                    raise PinyinError("SPY1 group membership is inconsistent")
        for gid, members in enumerate(groups):
            for member in members:
                owner = by_cp.get(member["codepoint"])
                if (
                    owner is None
                    or owner["rank"] != member["rank"]
                    or gid not in owner["readings"]
                ):
                    raise PinyinError("SPY1 member is not a reciprocal listed character")

    return {
        "char_count": char_count,
        "group_count": group_count,
        "characters": chars,
        "groups": groups,
        "size": len(data),
    }


class StrokePinyinBlob:
    def __init__(self) -> None:
        self._data: Optional[bytes] = None
        self._parsed: Optional[Dict[str, object]] = None
        self._by_cp: Dict[int, Dict[str, object]] = {}

    def bind(self, data: bytes) -> bool:
        try:
            parsed = validate_spy1(data, load_all=True)
        except PinyinError:
            return False
        self._data = data
        self._parsed = parsed
        self._by_cp = {item["codepoint"]: item for item in parsed["characters"]}  # type: ignore[index]
        return True

    def unbind(self) -> None:
        self._data = None
        self._parsed = None
        self._by_cp = {}

    @property
    def is_bound(self) -> bool:
        return self._data is not None

    def contains(self, codepoint: int) -> bool:
        return codepoint in self._by_cp

    def append_homophones(self, primary: int, cap: int) -> List[int]:
        if self._parsed is None or cap <= 0 or primary not in self._by_cp:
            return []
        item = self._by_cp[primary]
        groups = self._parsed["groups"]  # type: ignore[index]
        union: Dict[int, int] = {}
        for gid in item["readings"]:
            for member in groups[gid]:  # type: ignore[index]
                cp = int(member["codepoint"])
                if cp == primary:
                    continue
                union[cp] = int(member["rank"])
        ordered = sorted(union.items(), key=lambda pair: (pair[1], pair[0]))
        return [cp for cp, _rank in ordered[:cap]]

    def append_top_ranked(self, cap: int) -> List[int]:
        if self._parsed is None or cap <= 0:
            return []
        chars = sorted(
            self._parsed["characters"],  # type: ignore[index]
            key=lambda item: (int(item["rank"]), int(item["codepoint"])),
        )
        out = []
        seen = set()
        for item in chars:
            cp = int(item["codepoint"])
            if cp in seen:
                continue
            seen.add(cp)
            out.append(cp)
            if len(out) >= cap:
                break
        return out


def build_pinyin_manifest(
    characters: Sequence[Dict[str, object]],
    blob: bytes,
    source: Dict[str, object],
) -> Dict[str, object]:
    ordered = sorted(characters, key=lambda item: int(item["codepoint"]))
    return {
        "character_count": len(ordered),
        "codepoints": [f"U+{int(item['codepoint']):04X}" for item in ordered],
        "format": "stroke_pinyin_bin",
        "format_version": FORMAT_VERSION,
        "limits": {
            "max_characters": MAX_CHARACTERS,
            "max_file_bytes": MAX_FILE_BYTES,
            "max_group_members": MAX_GROUP_MEMBERS,
            "max_groups": MAX_GROUPS,
            "max_readings_per_character": MAX_READINGS_PER_CHARACTER,
        },
        "magic": MAGIC.decode("ascii"),
        "not_a_release_library": True,
        "not_commercially_reviewed": True,
        "not_official_certification": True,
        "prototype": True,
        "sha256_bin": hashlib.sha256(blob).hexdigest(),
        "source": source,
        "usage": (
            "Prototype SPY1 pinyin index from Unicode Unihan kMandarin with optional "
            "kHanyuPinyin union. Device stores group ids and ranks, not pinyin strings. "
            "Not a published 字库 and not commercially reviewed."
        ),
    }
