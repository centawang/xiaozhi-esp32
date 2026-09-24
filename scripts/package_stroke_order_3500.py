#!/usr/bin/env python3
"""Verify a closed 3500 v2 source bundle and stage host-only runtime payloads.

Not wired into CMake. Existing firmware will reject these v2 bytes.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys
import tempfile

from stroke_order.corpus2 import canonical_json, digest, validate_bundle
from stroke_order.policy3500 import verify_generated, verify_notices, verify_profile_directory
from stroke_order.v2 import require


def package_manifest(files: dict[str, bytes], names: list[str], cid: str) -> dict:
    return dict(format="stroke3500-host-v2", corpus_id=cid, device_compatible=False,
                files={name: dict(bytes=len(files[name]), sha256=digest(files[name])) for name in names})


def verify_package(path: Path) -> dict:
    """Validate closed runtime package; not a substitute for source provenance.

    Unlike generated bundles, runtime packages omit the source/reading audits.
    This validates every payload, cross-file identity and all manifest fields;
    package_corpus additionally requires the full fixed host source policy.
    """
    files, shards = verify_profile_directory(path, runtime=True)
    parsed = validate_bundle(files["stroke_cat.bin"], files["stroke_pinyin.bin"], [files[n] for n in shards])
    verify_notices(files)
    names = shards + ["stroke_cat.bin", "stroke_pinyin.bin", "ARPHICPL.TXT", "UNICODE-LICENSE.txt", "NOTICE.md"]
    expected = package_manifest(files, names, parsed["catalog"]["corpus_id"].hex())
    # Byte equality also rejects duplicate JSON keys, unknown keys and bool/int substitution.
    require(files["package.json"] == canonical_json(expected), "package manifest mismatch")
    return expected


def package_corpus(source: Path, output: Path) -> dict:
    verified = verify_generated(source)
    require(not output.exists(), "output must not already exist")
    names = verified["shard_names"] + ["stroke_cat.bin", "stroke_pinyin.bin",
                                      "ARPHICPL.TXT", "UNICODE-LICENSE.txt", "NOTICE.md"]
    files = verified["files"]
    manifest = package_manifest(files, names, verified["corpus_id"])
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".package3500-", dir=output.parent) as tmp:
        staging = Path(tmp) / "payload"
        staging.mkdir()
        for name in names:
            (staging / name).write_bytes(files[name])
        (staging / "package.json").write_bytes(canonical_json(manifest))
        (staging / "SHA256SUMS").write_text("".join(
            f"{digest(p.read_bytes())}  {p.name}\n" for p in sorted(staging.iterdir())), encoding="utf-8")
        verify_package(staging)
        require(not output.exists(), "output appeared during packaging")
        staging.rename(output)
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--verify", type=Path, help="verify an existing closed runtime package")
    args = parser.parse_args(argv)
    if args.verify is not None:
        if args.source is not None or args.output is not None:
            parser.error("--verify cannot be combined with --source/--output")
    elif args.source is None or args.output is None:
        parser.error("--source and --output are required when packaging")
    try:
        result = verify_package(args.verify) if args.verify is not None else package_corpus(args.source, args.output)
    except (ValueError, OSError, KeyError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(canonical_json(result).decode(), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
