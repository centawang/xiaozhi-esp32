"""Host-only closed bundle validation and deterministic corpus identity."""
from __future__ import annotations

from collections.abc import Mapping, Iterator
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from .scb2 import validate_scb2
from .spy2 import normalized_characters, validate_spy2
from .v2 import PROFILE, require

MANIFEST_LIMIT = 16 * 1024
HASH_CHUNK_BYTES = 64 * 1024
SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\Z", re.ASCII)
CHECKSUM_LINE = re.compile(rb"([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9_.-]{0,63})\n")


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def corpus_id(raws: list[tuple[int, bytes]], readings: list[dict], source: dict) -> bytes:
    chars = normalized_characters(readings)
    require(len(raws) == len(chars) == PROFILE, "corpus identity count")
    records = [{"rank": rank, "sha256_raw": digest(raw)} for rank, raw in sorted(raws)]
    require([r["rank"] for r in records] == list(range(1, PROFILE + 1)), "identity ranks")
    return hashlib.sha256(b"stroke3500-v2\0" + canonical_json(
        dict(profile=PROFILE, source=source, records=records, readings=chars))).digest()[:16]


def validate_bundle(catalog: bytes, pinyin: bytes, shards: list[bytes]) -> dict:
    parsed = validate_scb2(catalog, shards)
    spy = validate_spy2(pinyin, parsed["corpus_id"])
    a = [(c["codepoint"], c["rank"]) for c in parsed["characters"]]
    b = [(c["codepoint"], c["rank"]) for c in spy["characters"]]
    require(a == b and len(a) == PROFILE, "SCB2/SPY2 character/rank set mismatch")
    return dict(catalog=parsed, pinyin=spy)


def parse_checksums(data: bytes) -> dict[str, str]:
    """Exact ASCII wire grammar: no universal-newline/Unicode normalization."""
    require(0 < len(data) <= MANIFEST_LIMIT and data.endswith(b"\n"), "checksum manifest framing")
    names = {}
    for line in data.split(b"\n")[:-1]:
        match = CHECKSUM_LINE.fullmatch(line + b"\n")
        require(match is not None, "checksum line syntax")
        checksum, name = (part.decode("ascii") for part in match.groups())
        require(name != "SHA256SUMS" and name not in names, "unsafe/duplicate checksum name")
        names[name] = checksum
        require(len(names) <= 64, "too many bundle members")
    return names


def _identity(info: os.stat_result) -> tuple:
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def _bounded_stat(path: Path, maximum: int) -> os.stat_result:
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode), "bundle symlink/non-file")
    require(0 <= info.st_size <= maximum, f"bundle file exceeds bound: {path.name}")
    return info


def _read_checked(path: Path, before: os.stat_result, *, collect: bool,
                  checksum: str | None = None) -> bytes:
    """Fixed-chunk I/O, stable size/identity; never cache the whole directory."""
    # NOFOLLOW prevents final-component symlink swaps; NONBLOCK avoids a FIFO
    # replacement hanging before the fstat check. No unbounded read is issued.
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        require(_identity(os.fstat(stream.fileno())) == _identity(before), "bundle member changed")
        sha = hashlib.sha256()
        data = bytearray()
        remaining = before.st_size
        while remaining:
            block = stream.read(min(HASH_CHUNK_BYTES, remaining))
            require(bool(block), "bundle member truncated")
            sha.update(block)
            if collect:
                data.extend(block)
            remaining -= len(block)
        require(not stream.read(1), "bundle member grew")
        require(_identity(os.fstat(stream.fileno())) == _identity(before), "bundle member changed")
        require(checksum is None or sha.hexdigest() == checksum, "bundle SHA256 mismatch")
        return bytes(data)


class VerifiedFiles(Mapping[str, bytes]):
    """Read-through view of an exact, preflighted and hashed set; no bytes cache.

    Semantic callers read only the members they need. Each access rechecks the
    stat snapshot and digest, so a subsequent mutation cannot bypass the bounds.
    """
    def __init__(self, path: Path, snapshots: dict[str, os.stat_result], hashes: dict[str, str]):
        self._path = path
        self._snapshots = snapshots
        self._hashes = hashes

    def __iter__(self) -> Iterator[str]:
        return iter(self._snapshots)

    def __len__(self) -> int:
        return len(self._snapshots)

    def __getitem__(self, name: str) -> bytes:
        return _read_checked(self._path / name, self._snapshots[name],
                             collect=True, checksum=self._hashes[name])


def verify_directory(path: Path, *, member_limits: Mapping[str, int],
                     aggregate_limit: int) -> VerifiedFiles:
    """Caller supplies the exact profile-derived set BEFORE any content I/O.

    All lstat/type/size checks and the aggregate (including SHA256SUMS) pass
    before even the manifest is read. Hashing retains only a 64 KiB chunk.
    """
    require(path.is_dir() and not path.is_symlink(), "bundle directory")
    require(type(aggregate_limit) is int and aggregate_limit > 0, "aggregate limit")
    require(0 < len(member_limits) <= 64 and all(
        SAFE_NAME.fullmatch(n) and n != "SHA256SUMS" and type(limit) is int and limit > 0
        for n, limit in member_limits.items()), "member limits")
    require({p.name for p in path.iterdir()} == set(member_limits) | {"SHA256SUMS"},
            "unexpected bundle members")
    snapshots = {name: _bounded_stat(path / name, limit) for name, limit in member_limits.items()}
    manifest_stat = _bounded_stat(path / "SHA256SUMS", MANIFEST_LIMIT)
    require(sum(s.st_size for s in snapshots.values()) + manifest_stat.st_size <= aggregate_limit,
            "bundle aggregate exceeds bound")
    names = parse_checksums(_read_checked(path / "SHA256SUMS", manifest_stat, collect=True))
    require(set(names) == set(member_limits), "bundle not closed")
    for name, info in snapshots.items():
        _read_checked(path / name, info, collect=False, checksum=names[name])
    return VerifiedFiles(path, snapshots, names)
