#!/usr/bin/env python3
"""Independent product admission, not a change to the host fixture's identity.

Verify the pinned closed source policy, host package and actual W4a C++ reader
with the resolved heatshrink 0.4.1 C decoder. Only then transactionally publish
six shards + catalog + pinyin. Reports and license/audit files stay on the host.
This is build plumbing only: View/Application integration remains disabled.
Requires native cc/c++ (not the ESP cross compiler) on the build host.
"""
from __future__ import annotations

import argparse
import os
import json
import stat
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid

from package_stroke_order_3500 import package_manifest, verify_package
from stroke_order.corpus2 import canonical_json, digest
from stroke_order.heatshrink import verified_source_hashes
from stroke_order.policy3500 import verify_generated
from stroke_order.v2 import require

ROOT = Path(__file__).resolve().parents[1]
CORPUS_ID = "bcd474c127b2138738d96b9bf64832ad"
FIXTURE_SHA256 = "a1db55de14d17cf7e8113709391c525b3460fe3ae13f80ac76a5143f59d4bc13"
DEVICE_FILES = tuple(f"so{i:02d}.bin" for i in range(6)) + ("stroke_cat.bin", "stroke_pinyin.bin")
READER_SOURCES = (
    "scripts/stroke_order/device_verify.cc", "main/stroke_order/stroke_order_v2.cc",
    "main/stroke_order/stroke_order_v2.h", "main/stroke_order/stroke_order_store.cc",
    "main/stroke_order/stroke_order_store.h", "main/stroke_order/stroke_order_alloc.h",
)


def check_device_profile(verified: dict) -> int:
    """Host structural validity alone permits stored blocks larger than the MCU cap."""
    require(verified["corpus_id"] == CORPUS_ID, "product corpus identity mismatch")
    require(tuple(verified["shard_names"]) == DEVICE_FILES[:6], "product requires exactly six shards")
    chars = [c for shard in verified["parsed"]["catalog"]["shards"] for c in shard["characters"]]
    require(len(chars) == 3500, "product requires 3500 characters")
    maximum = max(c["stored_bytes"] for c in chars)
    require(0 < maximum <= 16384, "device stored block exceeds 16 KiB")
    return maximum


def compiler_identity(path: Path) -> dict:
    require(path.is_absolute(), "compiler path must be absolute")
    require(stat.S_ISREG(path.lstat().st_mode), "compiler must be regular/non-symlink")
    path = path.resolve(strict=True)
    require(os.access(path, os.X_OK), "compiler must be executable")
    return dict(path=str(path), version=subprocess.check_output(
        [str(path), "--version"], text=True).strip(), sha256=digest(path.read_bytes()))


def run_device_reader(directory: Path, work: Path, compilers: dict) -> str:
    source = ROOT / "managed_components/laride__heatshrink"
    verified_source_hashes()
    flags = ["-O2", "-Wall", "-Wextra", "-Werror"]
    subprocess.run([compilers["cc"]["path"], "-std=c11", *flags, "-I", str(source / "include"), "-c",
                    str(source / "heatshrink_decoder.c"), "-o", str(work / "hs.o")], check=True)
    command = [compilers["cxx"]["path"], "-std=c++17", *flags, "-fno-exceptions", "-fno-rtti",
               "-I", str(ROOT / "main"), "-I", str(source / "include")]
    if sys.platform == "darwin":
        sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
        command += ["-isystem", f"{sdk}/usr/include/c++/v1"]
    command += [str(ROOT / name) for name in READER_SOURCES if name.endswith(".cc")]
    command += [str(work / "hs.o"), "-o", str(work / "reader")]
    subprocess.run(command, check=True)
    return subprocess.check_output([str(work / "reader"), str(directory)], text=True).strip()


def prospective(path: Path, kind: str) -> Path:
    """Reject final symlinks BEFORE resolve (including dangling links).

    Existing parent symlinks are allowed and canonicalized (e.g. macOS /tmp).
    Missing parents are allowed; dangling links/non-directory parents are not.
    Callers must own the paths: this is not a hostile concurrent-writer sandbox.
    """
    path = path.absolute()
    try:
        mode = path.lstat().st_mode
    except NotADirectoryError as exc:
        raise ValueError(f"non-directory parent: {path}") from exc
    except FileNotFoundError:
        require(kind != "source", "source directory must exist")
    else:
        expected = stat.S_ISREG if kind == "report" else stat.S_ISDIR
        require(expected(mode), f"{kind} must be a non-symlink " +
                ("regular file" if kind == "report" else "directory"))
    for parent in path.parents:
        if parent.is_symlink():
            require(parent.is_dir(), f"invalid parent: {parent}")
        elif parent.exists():
            require(parent.is_dir(), f"non-directory parent: {parent}")
    return path.resolve()


def validate_paths(source: Path, output: Path | None, report: Path | None):
    paths = [prospective(p, kind) if p is not None else None
             for p, kind in ((source, "source"), (output, "output"), (report, "report"))]
    present = [p for p in paths if p is not None]
    for i, left in enumerate(present):
        for right in present[i + 1:]:
            require(left != right and left not in right.parents and right not in left.parents,
                    f"source/output/report overlap: {left} and {right}")
    return paths


def snapshot(path: Path):
    if path.is_dir():
        return {p.name: p.read_bytes() for p in path.iterdir()}
    return path.read_bytes()


def remove(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    else:
        path.unlink(missing_ok=True)


def publish(staging: Path | None, output: Path | None,
            pending_report: Path | None = None, report: Path | None = None) -> None:
    """Best-effort two-resource transaction, NOT crash/power-loss atomicity.

    Both stages are complete before touching either old destination. Backups
    live alongside destinations, never in an auto-cleaned staging directory.
    On rollback failure keep every unrestored backup and print its exact path.
    After commit, backup cleanup is warning-only; publication remains success.
    """
    entries = []
    for stage, target in ((staging, output), (pending_report, report)):
        if target is not None:
            require(stage is not None, "missing publication stage")
            entries.append(dict(stage=stage, target=target, expected=snapshot(stage),
                                backup=target.with_name(f".{target.name}.backup-{uuid.uuid4().hex}"),
                                had_old=target.exists(), attempted=False))
    try:
        for entry in entries:
            if entry["had_old"]:
                os.replace(entry["target"], entry["backup"])
        for entry in entries:
            entry["attempted"] = True
            os.replace(entry["stage"], entry["target"])
        # Readback is part of the transaction, including the report publication.
        for entry in entries:
            require(snapshot(entry["target"]) == entry["expected"], "publication readback mismatch")
    except BaseException as original:
        failures = []
        for entry in reversed(entries):
            target, backup = entry["target"], entry["backup"]
            try:
                if backup.exists():
                    if target.is_dir():
                        remove(target)
                    os.replace(backup, target)
                elif not entry["had_old"] and entry["attempted"]:
                    remove(target)
            except BaseException as exc:
                failures.append(f"restore {target}: {exc}; retained backup: {backup}")
        if failures:
            raise OSError(f"publication failed: {original}; rollback failed: " + "; ".join(failures)) from original
        raise
    # No backup cleanup in a finally: it could destroy the only old copy.
    for entry in entries:
        try:
            remove(entry["backup"])
        except Exception as exc:
            print(f"warning: committed; backup cleanup failed: {entry['backup']}: {exc}", file=sys.stderr)


def admit(source: Path, output: Path | None = None, report: Path | None = None,
          *, cc: Path, cxx: Path) -> dict:
    source, output, report = validate_paths(source, output, report)
    compilers = dict(cc=compiler_identity(cc), cxx=compiler_identity(cxx))
    verified = verify_generated(source)
    maximum = check_device_profile(verified)
    require(digest((source / "SHA256SUMS").read_bytes()) == FIXTURE_SHA256,
            "unapproved fixture checksum manifest")
    files = verified["files"]  # Immutable verified byte snapshot; do not re-read payload paths.
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".stroke3500-admit-", ignore_cleanup_errors=True,
                                     dir=output.parent if output is not None else None) as tmp:
        work = Path(tmp)
        package = work / "package"
        package.mkdir()
        names = list(DEVICE_FILES) + ["ARPHICPL.TXT", "UNICODE-LICENSE.txt", "NOTICE.md"]
        for name in names:
            (package / name).write_bytes(files[name])
        (package / "package.json").write_bytes(canonical_json(package_manifest(files, names, CORPUS_ID)))
        (package / "SHA256SUMS").write_text("".join(
            f"{digest(p.read_bytes())}  {p.name}\n" for p in sorted(package.iterdir())), encoding="utf-8")
        host_package = verify_package(package)
        require(host_package["device_compatible"] is False, "host identity must not change")
        proof = run_device_reader(package, work, compilers)
        result = dict(format="stroke3500-product-admission-v1", profile="Level1_3500",
                      corpus_id=CORPUS_ID, characters=3500, shards=6,
                      max_stored_bytes=maximum, device_stored_limit=16384,
                      fixture_sha256=FIXTURE_SHA256, reader_verification=proof,
                      compilers=compilers,
                      reader_source_sha256={name: digest((ROOT / name).read_bytes()) for name in READER_SOURCES},
                      codec_source_sha256=verified_source_hashes(),
                      view_application_integrated=False, host_device_compatible=False,
                      files={name: host_package["files"][name] for name in DEVICE_FILES})
        staging = None
        pending = None
        try:
            if output is not None:
                staging = work / "device"
                staging.mkdir()
                for name in DEVICE_FILES:
                    (staging / name).write_bytes(files[name])
                require(snapshot(staging) == {name: files[name] for name in DEVICE_FILES},
                        "staged device readback mismatch")
            if report is not None:
                report.parent.mkdir(parents=True, exist_ok=True)
                with tempfile.NamedTemporaryFile(prefix=f".{report.name}.pending-",
                                                 dir=report.parent, delete=False) as stream:
                    pending = Path(stream.name)
                    stream.write(canonical_json(result))
                require(json.loads(pending.read_bytes()) == result, "staged report readback mismatch")
            publish(staging, output, pending, report)
        finally:
            # Only the new temporary report; never a backup of a prior result.
            if pending is not None:
                try:
                    pending.unlink(missing_ok=True)
                except OSError as exc:
                    print(f"warning: temporary report retained: {pending}: {exc}", file=sys.stderr)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, help="optional device-only atomic staging")
    parser.add_argument("--report", type=Path, help="host admission evidence, outside device assets")
    parser.add_argument("--cc", type=Path, required=True, help="absolute native C compiler (non-symlink)")
    parser.add_argument("--cxx", type=Path, required=True, help="absolute native C++ compiler (non-symlink)")
    args = parser.parse_args(argv)
    try:
        result = admit(args.source, args.output_dir, args.report, cc=args.cc, cxx=args.cxx)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(canonical_json(result).decode(), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
