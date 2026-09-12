"""Auditable 0001-0500 selection from the official 一级字表.

The official PDF/page are the membership source. The structured JSON checkout is
only a prototype transcription aid: it is not an official digital annex and not
a commercial grant. Dual-person page-by-page PDF verification has not been done.
"""

from __future__ import annotations

import csv
import hashlib
import json
import re
import subprocess
from pathlib import Path
from typing import Dict, List, Sequence

from .constants import MAX_TARGET_CODEPOINT, MIN_TARGET_CODEPOINT

OFFICIAL_TITLE = "通用规范汉字表"
OFFICIAL_DOCUMENT = "国发〔2013〕23号"
OFFICIAL_PAGE = "https://www.gov.cn/zwgk/2013-08/19/content_2469793.htm"
OFFICIAL_PDF = "https://www.gov.cn/gzdt/att/att/site1/20130819/tygfhzb.pdf"
OFFICIAL_PDF_SHA256 = "af85c706a53d3b3bbad818bcce7415ac9a2284ea14f79fe7f54ce1248a7bdac9"
OFFICIAL_PDF_BYTES = 100606660
OFFICIAL_RANGE_START = 1
OFFICIAL_RANGE_END = 500

TRANSCRIPTION_REPO = (
    "https://github.com/leonsilicon/table-of-general-standard-chinese-characters"
)
TRANSCRIPTION_COMMIT = "f9786a82be6e1672bdc60f85760a9e4a3791d1f1"
TRANSCRIPTION_JSON_PATH = "table-of-general-standard-chinese-characters.json"
TRANSCRIPTION_JSON_SHA256 = "a9f0a21fb83a84dd695eaeec7b77743a6099ca559768c8275ae0c22c827917d0"
COMMIT_RE = re.compile(r"^[0-9a-fA-F]{40}$")
CODEPOINT_RE = re.compile(r"^U\+([0-9A-Fa-f]{4,6})$")


class SelectionError(ValueError):
    """Raised when the 0001-0500 selection cannot be produced or verified."""


def _normalize_repo_url(url: str) -> str:
    normalized = url.strip().rstrip("/")
    if normalized.endswith(".git"):
        normalized = normalized[:-4]
    return normalized


def _run_git_text(directory: Path, *args: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(directory), *args],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as exc:
        raise SelectionError("git is required to verify transcription provenance") from exc
    if result.returncode != 0:
        detail = result.stderr.strip() or "git command failed"
        raise SelectionError(f"cannot verify transcription checkout: {detail}")
    return result.stdout.strip()


def _git_show(directory: Path, relative_path: str) -> bytes:
    try:
        result = subprocess.run(
            ["git", "-C", str(directory), "show", f"HEAD:{relative_path}"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise SelectionError("git is required to verify transcription provenance") from exc
    if result.returncode != 0:
        raise SelectionError("transcription JSON is not a tracked blob at HEAD")
    return result.stdout


def _verify_transcription_checkout(
    table_json: str,
    *,
    expected_commit: str,
    expected_repo: str,
    expected_json_sha256: str,
    expected_relative_path: str = TRANSCRIPTION_JSON_PATH,
) -> Dict[str, object]:
    """Verify one transcription blob against a clean, pinned git checkout.

    The production selection API below supplies only module-pinned expectations;
    the explicit arguments exist so host tests can exercise every provenance
    failure without vendoring the unlicensed third-party checkout.
    """
    if not COMMIT_RE.fullmatch(expected_commit):
        raise SelectionError("transcription commit must be a complete 40-hex git commit")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", expected_json_sha256):
        raise SelectionError("transcription JSON SHA-256 must be complete")
    path = Path(table_json)
    if not path.is_file():
        raise SelectionError("transcription JSON not found")
    path = path.resolve(strict=True)
    root = Path(_run_git_text(path.parent, "rev-parse", "--show-toplevel")).resolve(strict=True)
    try:
        relative_path = path.relative_to(root).as_posix()
    except ValueError as exc:
        raise SelectionError("transcription JSON is outside its git checkout") from exc
    if relative_path != expected_relative_path:
        raise SelectionError("transcription JSON path does not match the pinned tracked blob")

    head = _run_git_text(root, "rev-parse", "--verify", "HEAD^{commit}").lower()
    if not COMMIT_RE.fullmatch(head) or head != expected_commit.lower():
        raise SelectionError("transcription commit does not match checkout HEAD")
    origin = _run_git_text(root, "config", "--get", "remote.origin.url")
    if _normalize_repo_url(origin) != _normalize_repo_url(expected_repo):
        raise SelectionError("transcription repo does not match checkout remote.origin.url")

    workspace_bytes = path.read_bytes()
    head_bytes = _git_show(root, relative_path)
    if workspace_bytes != head_bytes:
        raise SelectionError("transcription JSON workspace bytes differ from HEAD")
    digest = hashlib.sha256(workspace_bytes).hexdigest()
    if digest != expected_json_sha256.lower():
        raise SelectionError("transcription JSON does not match the pinned SHA-256")
    dirty = _run_git_text(root, "status", "--porcelain=v1", "--untracked-files=all")
    if dirty:
        raise SelectionError("transcription checkout is dirty")
    return {
        "checkout_clean": True,
        "commit": head,
        "json_path": relative_path,
        "json_sha256": digest,
        "repo": origin,
    }


def verify_pinned_transcription(table_json: str) -> Dict[str, object]:
    return _verify_transcription_checkout(
        table_json,
        expected_commit=TRANSCRIPTION_COMMIT,
        expected_repo=TRANSCRIPTION_REPO,
        expected_json_sha256=TRANSCRIPTION_JSON_SHA256,
    )


def load_tier1_characters(table_json: str) -> List[str]:
    path = Path(table_json)
    if not path.is_file():
        raise SelectionError("transcription JSON not found")
    raw = path.read_bytes()
    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SelectionError("transcription JSON is not valid UTF-8 JSON") from exc
    tier1 = payload.get("tier1") if isinstance(payload, dict) else None
    if not isinstance(tier1, list) or not tier1:
        raise SelectionError("transcription JSON missing tier1 array")
    return [str(item) for item in tier1]


def select_official_500(table_json: str) -> List[Dict[str, object]]:
    """Return ranks 1-500 only from the module-pinned transcription checkout."""
    verify_pinned_transcription(table_json)
    tier1 = load_tier1_characters(table_json)
    if len(tier1) < OFFICIAL_RANGE_END:
        raise SelectionError("transcription aid does not contain 500 first-tier characters")
    selected = tier1[OFFICIAL_RANGE_START - 1 : OFFICIAL_RANGE_END]
    rows = []
    seen = set()
    for index, character in enumerate(selected, start=OFFICIAL_RANGE_START):
        if not isinstance(character, str) or len(character) != 1:
            raise SelectionError(f"official number {index:04d} is not a single character")
        codepoint = ord(character)
        if codepoint < MIN_TARGET_CODEPOINT or codepoint > MAX_TARGET_CODEPOINT:
            raise SelectionError(
                f"official number {index:04d} is outside Basic CJK Unified Ideographs"
            )
        if character in seen:
            raise SelectionError(f"duplicate character {character} at official number {index:04d}")
        seen.add(character)
        rows.append(
            {
                "rank": index,
                "official_number": f"{index:04d}",
                "character": character,
                "codepoint": f"U+{codepoint:04X}",
                "codepoint_int": codepoint,
            }
        )
    verify_selection_rows(rows)
    return rows


def verify_selection_rows(rows: Sequence[Dict[str, object]]) -> None:
    if len(rows) != OFFICIAL_RANGE_END:
        raise SelectionError(f"selection must contain exactly {OFFICIAL_RANGE_END} rows")
    seen_chars = set()
    seen_cps = set()
    for index, row in enumerate(rows, start=1):
        rank = int(row["rank"])
        official = str(row["official_number"])
        character = str(row["character"])
        codepoint = int(row["codepoint_int"])
        codepoint_text = str(row["codepoint"])
        match = CODEPOINT_RE.fullmatch(codepoint_text)
        if match is None or int(match.group(1), 16) != ord(character):
            raise SelectionError("text codepoint/character mismatch")
        if rank != index:
            raise SelectionError("rank must be the official numbering order 1-500")
        if official != f"{index:04d}":
            raise SelectionError("official_number must be 0001-0500 with no gaps")
        if len(character) != 1:
            raise SelectionError("selection character must be one scalar")
        if codepoint < MIN_TARGET_CODEPOINT or codepoint > MAX_TARGET_CODEPOINT:
            raise SelectionError("selection is not unique Basic CJK")
        if character in seen_chars or codepoint in seen_cps:
            raise SelectionError("selection characters must be unique")
        if codepoint != ord(character):
            raise SelectionError("character/codepoint mismatch")
        seen_chars.add(character)
        seen_cps.add(codepoint)
    if len(seen_chars) != OFFICIAL_RANGE_END or len(seen_cps) != OFFICIAL_RANGE_END:
        raise SelectionError("selection is not 500 unique Basic CJK characters")


def write_selection_csv(rows: Sequence[Dict[str, object]], path: Path) -> None:
    verify_selection_rows(rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["rank", "official_number", "character", "codepoint"])
        for row in rows:
            writer.writerow(
                [row["rank"], row["official_number"], row["character"], row["codepoint"]]
            )


def write_charset(rows: Sequence[Dict[str, object]], path: Path) -> None:
    verify_selection_rows(rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Prototype charset: 通用规范汉字表 一级字表 0001-0500.",
        "# Not official certification. Not a commercial release.",
        "# Dual-person official PDF verification has not been completed.",
    ]
    for row in rows:
        lines.append(str(row["character"]))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def load_selection_csv(path: Path) -> List[Dict[str, object]]:
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise SelectionError("selection CSV is not valid UTF-8") from exc
    reader = csv.DictReader(text.splitlines())
    if reader.fieldnames != ["rank", "official_number", "character", "codepoint"]:
        raise SelectionError("selection CSV header mismatch")
    rows: List[Dict[str, object]] = []
    for line in reader:
        character = line["character"]
        codepoint_text = line["codepoint"]
        if not character:
            raise SelectionError("selection CSV has an empty character")
        match = CODEPOINT_RE.fullmatch(codepoint_text)
        if match is None:
            raise SelectionError("selection CSV codepoint is not U+ hexadecimal text")
        codepoint = int(match.group(1), 16)
        if len(character) != 1 or codepoint != ord(character):
            raise SelectionError("selection CSV character/codepoint column mismatch")
        rows.append(
            {
                "rank": int(line["rank"]),
                "official_number": line["official_number"],
                "character": character,
                "codepoint": codepoint_text,
                "codepoint_int": codepoint,
            }
        )
    verify_selection_rows(rows)
    return rows


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def official_source_lock(
    table_json: Path,
    pdf_path: Path | None = None,
) -> Dict[str, object]:
    transcription = verify_pinned_transcription(str(table_json))
    pdf_sha = OFFICIAL_PDF_SHA256
    pdf_bytes = OFFICIAL_PDF_BYTES
    if pdf_path is not None:
        if not pdf_path.is_file():
            raise SelectionError("official PDF not found")
        pdf_sha = file_sha256(pdf_path)
        pdf_bytes = pdf_path.stat().st_size
        if pdf_sha != OFFICIAL_PDF_SHA256 or pdf_bytes != OFFICIAL_PDF_BYTES:
            raise SelectionError("official PDF hash/size does not match the pinned government file")
    return {
        "kind": "stroke_order_source_lock",
        "not_a_release_library": True,
        "not_official_certification": True,
        "not_commercially_reviewed": True,
        "prototype": True,
        "dual_person_official_pdf_verified": False,
        "official_table": {
            "bytes": pdf_bytes,
            "document": OFFICIAL_DOCUMENT,
            "page": OFFICIAL_PAGE,
            "pdf": OFFICIAL_PDF,
            "range": "一级字表 0001-0500",
            "sha256": pdf_sha,
            "title": OFFICIAL_TITLE,
        },
        "transcription_aid": {
            "checkout_clean": transcription["checkout_clean"],
            "commit": transcription["commit"],
            "json_path": transcription["json_path"],
            "json_sha256": transcription["json_sha256"],
            "license": "no explicit license in the checkout",
            "repo": TRANSCRIPTION_REPO,
            "role": (
                "prototype transcription aid only; not an official digital annex "
                "and not a commercial authorization source"
            ),
        },
    }
