"""SPY2 CSR relations. Independent limits/API; never relax SPY1's 8/32."""
from __future__ import annotations

import struct

from .pinyin import normalize_pinyin, parse_khanyu_pinyin, parse_kmandarin
from .v2 import HEADER_SIZE, PROFILE, codepoint, envelope, open_envelope, require

MAX_READINGS = 16
MAX_MEMBERS = 64
MAX_GROUPS = 4096
MAX_BYTES = 512 * 1024
CHAR = struct.Struct("<IHH")


def build_readings(rows: list[dict], unihan: dict) -> list[dict]:
    result = []
    for row in rows:
        cp = row["codepoint_int"]
        fields = unihan.get(cp, {})
        primary = parse_kmandarin(fields.get("kMandarin", ""))
        readings = list(dict.fromkeys(primary + parse_khanyu_pinyin(fields.get("kHanyuPinyin", ""))))
        require(1 <= len(readings) <= MAX_READINGS, "SPY2 reading union exceeds 16; no truncation")
        result.append(dict(codepoint=cp, rank=row["rank"], readings=readings))
    return result


def normalized_characters(characters: list[dict]) -> list[dict]:
    require(1 <= len(characters) <= PROFILE, "SPY2 character count")
    result = []
    cps, ranks = set(), set()
    for item in characters:
        cp, rank = item["codepoint"], item["rank"]
        codepoint(cp)
        require(cp not in cps and rank not in ranks and 1 <= rank <= len(characters), "SPY2 duplicate/rank")
        readings = [normalize_pinyin(value) for value in item["readings"]]
        require(1 <= len(readings) <= MAX_READINGS and len(set(readings)) == len(readings),
                "SPY2 readings duplicate/count")
        cps.add(cp)
        ranks.add(rank)
        result.append(dict(codepoint=cp, rank=rank, readings=readings))
    return sorted(result, key=lambda x: x["codepoint"])


def array(kind: str, values: list[int]) -> bytes:
    return struct.pack("<" + kind * len(values), *values)


def pack_spy2(characters: list[dict], corpus: bytes) -> bytes:
    chars = normalized_characters(characters)
    syllables = sorted({r for c in chars for r in c["readings"]})
    require(1 <= len(syllables) <= MAX_GROUPS, "SPY2 group count")
    ids = {r: g for g, r in enumerate(syllables)}
    groups = [[] for _ in syllables]
    char_offsets, edges = [0], []
    for index, char in enumerate(chars):
        for reading in char["readings"]:
            gid = ids[reading]
            edges.append(gid)
            groups[gid].append(index)
        char_offsets.append(len(edges))
    group_offsets, members = [0], []
    for group in groups:
        require(1 <= len(group) <= MAX_MEMBERS, "SPY2 group exceeds 64; no truncation")
        members.extend(sorted(group, key=lambda index: chars[index]["rank"]))
        group_offsets.append(len(members))
    body = b"".join(CHAR.pack(c["codepoint"], c["rank"], 0) for c in chars)
    body += array("I", char_offsets) + array("H", edges) + array("I", group_offsets) + array("H", members)
    blob = envelope(b"SPY2", corpus, len(chars), len(groups), HEADER_SIZE,
                    HEADER_SIZE + CHAR.size * len(chars), len(edges), body)
    validate_spy2(blob, corpus)
    return blob


def validate_spy2(data: bytes, corpus: bytes | None = None) -> dict:
    header = open_envelope(data, b"SPY2", MAX_BYTES, corpus)
    n, g, e = header["count"], header["aux"], header["flags"]
    require(1 <= n <= PROFILE and 1 <= g <= MAX_GROUPS
            and n <= e <= n * MAX_READINGS and g <= e <= g * MAX_MEMBERS, "SPY2 count/edge bounds")
    char_csr = HEADER_SIZE + CHAR.size * n
    edge_start = char_csr + 4 * (n + 1)
    group_csr = edge_start + 2 * e
    member_start = group_csr + 4 * (g + 1)
    require(header["offset1"] == HEADER_SIZE and header["offset2"] == char_csr
            and member_start + 2 * e == len(data), "SPY2 canonical offsets/size")
    chars = []
    previous, ranks = 0, set()
    for index in range(n):
        cp, rank, reserved = CHAR.unpack_from(data, HEADER_SIZE + index * CHAR.size)
        codepoint(cp)
        require(cp > previous and 1 <= rank <= n and rank not in ranks and reserved == 0,
                "SPY2 duplicate/rank/reserved")
        chars.append(dict(codepoint=cp, rank=rank))
        previous = cp
        ranks.add(rank)
    co = struct.unpack_from("<" + "I" * (n + 1), data, char_csr)
    go = struct.unpack_from("<" + "I" * (g + 1), data, group_csr)
    edges = struct.unpack_from("<" + "H" * e, data, edge_start)
    members = struct.unpack_from("<" + "H" * e, data, member_start)
    require(co[0] == go[0] == 0 and co[-1] == go[-1] == e, "SPY2 CSR endpoints")
    forward = set()
    for c in range(n):
        require(0 <= co[c] < co[c + 1] <= e and co[c + 1] - co[c] <= MAX_READINGS, "SPY2 char CSR bounds")
        readings = list(edges[co[c]:co[c + 1]])
        require(all(gid < g for gid in readings) and len(readings) == len(set(readings)), "SPY2 group ids")
        chars[c]["readings"] = readings
        forward.update((c, gid) for gid in readings)
    reverse, groups = set(), []
    for gid in range(g):
        require(0 <= go[gid] < go[gid + 1] <= e and go[gid + 1] - go[gid] <= MAX_MEMBERS, "SPY2 group CSR bounds")
        group = list(members[go[gid]:go[gid + 1]])
        require(all(c < n for c in group) and len(group) == len(set(group)), "SPY2 char index")
        require([chars[c]["rank"] for c in group] == sorted(chars[c]["rank"] for c in group),
                "SPY2 members not rank ordered")
        groups.append(group)
        reverse.update((c, gid) for c in group)
    require(forward == reverse and len(forward) == e, "SPY2 non-reciprocal relations")
    return dict(**header, characters=chars, groups=groups, relations=e)
