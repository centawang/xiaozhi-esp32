"""Host-only v2 format tests. No fixture download, codec fallback, or v1 changes."""
import dataclasses
import hashlib
import functools
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from stroke_order import corpus2, dzz1, heatshrink, scb2, sob2, spy2
from stroke_order.corpus2 import corpus_id, validate_bundle, verify_directory
from stroke_order.policy3500 import (
    verify_generated, verify_diagnostics, NOTICE_TEXT, verify_profile_directory,
    GENERATED_MEMBER_LIMITS, PACKAGE_MEMBER_LIMITS,
)
from stroke_order.corpus2 import canonical_json, digest
from package_stroke_order_3500 import package_manifest, verify_package
from stroke_order.pinyin import PinyinError, union_readings
from stroke_order.v2 import FormatError, HEADER_SIZE, MAX_RAW_BYTES, MAX_TRANSFORMED_BYTES, crc

CID = bytes(range(16))
OTHER = bytes(range(1, 17))
DUN = "21468.010:dūn,duī,tuán,diāo,dùn,dào,zhǔn,tūn,duì,tún"


def raw_glyph(cp=0x4E00):
    points = [(0, 0), (1024, 0), (1024, 1024), (0, 0), (512, 512), (0, 1024)]
    return struct.pack("<IHHHH", cp, 1, 0, 4, 2) + b"".join(struct.pack("<HH", *p) for p in points)


def repaired(data):
    data = bytearray(data)
    struct.pack_into("<I", data, 44, crc(data[HEADER_SIZE:]))
    struct.pack_into("<I", data, 48, 0)
    struct.pack_into("<I", data, 48, crc(data[:HEADER_SIZE]))
    return bytes(data)


def changed(blob, offset, value, kind="I"):
    data = bytearray(blob)
    struct.pack_into("<" + kind, data, offset, value)
    return repaired(data)


def synthetic_readings(n):
    # 100 legal numeric syllable labels; exactly 35 members/group at n=3500.
    def label(i):
        return "p" + chr(97 + i // 26) + chr(97 + i % 26) + "1"
    return [dict(codepoint=0x4E00 + i, rank=i + 1, readings=[label(i % 100)]) for i in range(n)]


@functools.lru_cache(maxsize=1)
def checked_real_bundle(path):
    return verify_generated(Path(path))


def write_closed(path, files):
    for name, data in files.items():
        (path / name).write_bytes(data)
    (path / "SHA256SUMS").write_text("".join(
        f"{digest(data)}  {name}\n" for name, data in sorted(files.items())))


class DzzTests(unittest.TestCase):
    def test_roundtrip_and_canonical_encoding(self):
        raw = raw_glyph()
        encoded = dzz1.encode(raw)
        self.assertEqual(encoded, dzz1.encode(raw))
        self.assertEqual(dzz1.decode(encoded, len(raw)), raw)
        self.assertLessEqual(len(encoded), len(raw) + 4)

    def test_truncation_at_every_byte_and_trailing(self):
        raw = raw_glyph()
        encoded = dzz1.encode(raw)
        for cut in range(len(encoded)):
            with self.subTest(cut=cut), self.assertRaises(FormatError):
                dzz1.decode(encoded[:cut], len(raw))
        with self.assertRaises(FormatError):
            dzz1.decode(encoded + b"\0", len(raw))

    def test_varint_overflow_nonminimal_negative_coordinate(self):
        raw = raw_glyph()
        encoded = dzz1.encode(raw)
        # First x is zero. Replace its one-byte varint with invalid encodings.
        for value in (b"\x80\x00", b"\x80\x80\x00", b"\xff\x10", b"\x80\x11", b"\x01"):
            with self.subTest(value=value), self.assertRaises(FormatError):
                dzz1.decode(encoded[:16] + value + encoded[17:], len(raw))

    def test_header_geometry_and_bomb_bounds(self):
        raw = raw_glyph()
        encoded = dzz1.encode(raw)
        mutations = []
        for offset, value in ((4, 0), (8, 49), (10, 1), (12, 257), (14, 65)):
            b = bytearray(encoded)
            struct.pack_into("<H", b, offset, value)
            mutations.append(bytes(b))
        mutations.extend((b"BAD!" + encoded[4:], bytes(MAX_TRANSFORMED_BYTES + 1)))
        for b in mutations:
            with self.assertRaises(FormatError):
                dzz1.decode(b, len(raw))
        for size in (0, len(raw) - 1, len(raw) + 1, MAX_RAW_BYTES + 1):
            with self.assertRaises(FormatError):
                dzz1.decode(encoded, size)

    def test_raw_invalid_closure_and_coordinate(self):
        for offset, value in ((24, 1), (12, 1025)):
            b = bytearray(raw_glyph())
            struct.pack_into("<H", b, offset, value)
            with self.assertRaises(FormatError):
                dzz1.encode(bytes(b))


class CodecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.codec = heatshrink.ReferenceCodec()

    @classmethod
    def tearDownClass(cls):
        cls.codec.close()

    def test_c_reference_crosscheck_determinism(self):
        rng = random.Random(3500)
        for size in (1, 2, 15, 16, 17, 255, 1024, 1025, 4096, MAX_TRANSFORMED_BYTES):
            for raw in (bytes(size), b"abcde" * (size // 5) + b"x" * (size % 5),
                        bytes(rng.randrange(256) for _ in range(size))):
                with self.subTest(size=size):
                    stored = self.codec.encode(raw)
                    self.assertEqual(stored, self.codec.encode(raw))
                    self.assertEqual(heatshrink.decode(stored, size), raw)
                    self.assertEqual(self.codec.decode_reference(stored, size), raw)

    def test_independent_literal_and_zero_history_vectors(self):
        # Literal A: 1 01000001 + seven zero padding bits.
        self.assertEqual(heatshrink.decode(b"\xa0\x80", 1), b"A")
        self.assertEqual(self.codec.encode(b"A"), b"\xa0\x80")
        # Reference permits zero-filled history before the first emitted byte.
        self.assertEqual(self.codec.decode_reference(b"\x00\x00", 1), b"\0")
        # 0, distance=1024, length=16, one zero pad bit.
        self.assertEqual(self.codec.decode_reference(b"\x7f\xfe", 16), bytes(16))

    def test_truncation_tail_padding_and_bomb(self):
        stored = self.codec.encode(b"A" * 128)
        for cut in range(len(stored)):
            with self.assertRaises(FormatError):
                heatshrink.decode(stored[:cut], 128)
        for b in (stored + b"\0", stored + b"\xff", b"\xa0\x81"):
            with self.assertRaises(FormatError):
                heatshrink.decode(b, 1 if b == b"\xa0\x81" else 128)
        with self.assertRaises(FormatError):
            heatshrink.decode(b"\x7f\xfe", 15)
        with self.assertRaises(FormatError):
            heatshrink.decode(stored, MAX_TRANSFORMED_BYTES + 1)

    def test_codec_missing_source_is_failure_not_skip_or_fallback(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(heatshrink, "ROOT", Path(tmp)):
            with self.assertRaises(FileNotFoundError):
                heatshrink.ReferenceCodec()


class SobTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with heatshrink.ReferenceCodec() as codec:
            cls.blocks = [sob2.encode_record(raw_glyph(0x4E00 + i), i + 1, codec) for i in range(2)]
        cls.blob = sob2.pack_sob2(cls.blocks, CID)

    def test_roundtrip_and_determinism(self):
        self.assertEqual(sob2.pack_sob2(self.blocks, CID), self.blob)
        parsed = sob2.validate_sob2(self.blob, CID)
        self.assertEqual([c["raw"] for c in parsed["characters"]], [raw_glyph(0x4E00 + i) for i in range(2)])

    def test_codec_profile_version_and_corpus_rejection(self):
        for off, value, kind in ((52, 0, "I"), (52, sob2.FLAGS ^ 1, "I"),
                                  (4, 1, "H"), (6, 2000, "H"), (56, 1, "I")):
            with self.assertRaises(FormatError):
                sob2.validate_sob2(changed(self.blob, off, value, kind))
        with self.assertRaises(FormatError):
            sob2.validate_sob2(self.blob, OTHER)

    def test_bad_offsets_overflow_lengths_duplicate_and_crc(self):
        for off, value, kind in (
            (32, 0xFFFFFFFF, "I"), (36, 0xFFFFFFFF, "I"),
            (68, 0xFFFFFFFF, "I"), (72, 0xFFFFFFFF, "I"),
            (76, MAX_TRANSFORMED_BYTES + 1, "I"), (80, MAX_RAW_BYTES + 1, "I"),
            (84, 0, "I"), (88, 0, "I"), (92, 2, "H"), (94, 1, "H"),
            (96, self.blocks[0].codepoint, "I"), (100, 128, "I"),
        ):
            with self.subTest(offset=off), self.assertRaises(FormatError):
                sob2.validate_sob2(changed(self.blob, off, value, kind))

    def test_trailing_and_truncated_index(self):
        for b in (self.blob[:-1], self.blob + b"\0", self.blob[:63], self.blob[:100]):
            with self.assertRaises(FormatError):
                sob2.validate_sob2(b)

    def test_per_block_trailing_compression_even_with_repaired_crcs(self):
        block = dataclasses.replace(self.blocks[0], stored=self.blocks[0].stored + b"\0")
        with self.assertRaises(FormatError):
            sob2.pack_sob2([block], CID)

    def test_stored_valid_but_raw_codepoint_mismatch(self):
        block = dataclasses.replace(self.blocks[0], codepoint=0x4E01)
        with self.assertRaises(FormatError):
            sob2.pack_sob2([block], CID)

    def test_mutation_every_byte(self):
        for offset in range(len(self.blob)):
            damaged = bytearray(self.blob)
            damaged[offset] ^= 1
            with self.assertRaises(FormatError):
                sob2.validate_sob2(bytes(damaged))


class SpyTests(unittest.TestCase):
    def setUp(self):
        self.chars = synthetic_readings(37)
        for c in self.chars:
            c["readings"] = ["yi4"]
        self.blob = spy2.pack_spy2(self.chars, CID)

    def test_dun_ten_readings_kept_v1_still_rejects(self):
        rows = [dict(codepoint_int=ord("敦"), rank=1)]
        chars = spy2.build_readings(rows, {ord("敦"): dict(kMandarin="dūn", kHanyuPinyin=DUN)})
        self.assertEqual(chars[0]["readings"], ["dun1", "dui1", "tuan2", "diao1", "dun4", "dao4", "zhun3", "tun1", "dui4", "tun2"])
        blob = spy2.pack_spy2(chars, CID)
        self.assertEqual(len(spy2.validate_spy2(blob)["characters"][0]["readings"]), 10)
        with self.assertRaises(PinyinError):
            union_readings("dūn", DUN)

    def test_group_37_determinism(self):
        self.assertEqual(len(spy2.validate_spy2(self.blob)["groups"][0]), 37)
        self.assertEqual(self.blob, spy2.pack_spy2(list(reversed(self.chars)), CID))

    def test_limits_16_and_64_accepted_17_65_rejected(self):
        c = [dict(codepoint=0x4E00, rank=1, readings=[chr(97 + i) + "1" for i in range(16)])]
        spy2.pack_spy2(c, CID)
        c[0]["readings"].append("z1")
        with self.assertRaises(FormatError):
            spy2.pack_spy2(c, CID)
        c = synthetic_readings(65)
        for x in c:
            x["readings"] = ["a1"]
        spy2.pack_spy2(c[:64], CID)
        with self.assertRaises(FormatError):
            spy2.pack_spy2(c, CID)

    def test_duplicate_reading_codepoint_rank_missing_primary(self):
        for key, value in (("codepoint", self.chars[0]["codepoint"]), ("rank", 1),
                           ("readings", ["yi4", "yi4"]), ("readings", [])):
            chars = [dict(c) for c in self.chars]
            chars[1][key] = value
            with self.assertRaises(FormatError):
                spy2.pack_spy2(chars, CID)
        with self.assertRaises(PinyinError):
            spy2.build_readings([dict(codepoint_int=0x4E00, rank=1)], {})

    def test_repaired_crc_structural_mutations(self):
        co = 64 + 8 * 37
        edge = co + 4 * 38
        go = edge + 2 * 37
        members = go + 8
        for off, value, kind in ((24, 3501, "I"), (28, 4097, "I"), (52, 0xFFFFFFFF, "I"),
                                  (co, 1, "I"), (co + 4, 0xFFFFFFFF, "I"),
                                  (edge, 1, "H"), (go, 1, "I"), (members, 37, "H"),
                                  (members + 2, 0, "H"), (72, 0x4E00, "I")):
            with self.subTest(offset=off), self.assertRaises(FormatError):
                spy2.validate_spy2(changed(self.blob, off, value, kind))

    def test_nonreciprocal_edges_rejected(self):
        chars = synthetic_readings(2)
        blob = spy2.pack_spy2(chars, CID)
        edge = 64 + 8 * 2 + 4 * 3
        with self.assertRaises(FormatError):
            spy2.validate_spy2(changed(blob, edge, 1, "H"))

    def test_mutation_and_mixed_corpus(self):
        with self.assertRaises(FormatError):
            spy2.validate_spy2(self.blob, OTHER)
        for offset in random.Random(1).sample(range(len(self.blob)), 100):
            damaged = bytearray(self.blob)
            damaged[offset] ^= 1
            with self.assertRaises(FormatError):
                spy2.validate_spy2(bytes(damaged))


class CatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with heatshrink.ReferenceCodec() as codec:
            cls.blocks = [sob2.encode_record(raw_glyph(0x4E00 + i), i + 1, codec) for i in range(3500)]
        cls.shards = sob2.shard_blocks(cls.blocks, CID)
        cls.catalog = scb2.pack_scb2(cls.shards, CID)
        cls.readings = synthetic_readings(3500)
        cls.pinyin = spy2.pack_spy2(cls.readings, CID)

    def test_full_profile_bundle_and_identity_determinism(self):
        parsed = validate_bundle(self.catalog, self.pinyin, self.shards)
        self.assertEqual(len(parsed["catalog"]["characters"]), 3500)
        raws = [(b.rank, raw_glyph(b.codepoint)) for b in self.blocks]
        self.assertEqual(corpus_id(raws, self.readings, {}), corpus_id(list(reversed(raws)), list(reversed(self.readings)), {}))
        self.assertNotEqual(corpus_id(raws, self.readings, {}), corpus_id(raws, self.readings, {"new": True}))

    def test_dynamic_byte_budget_and_32_shard_limit(self):
        with mock.patch.object(sob2, "MAX_SHARD_BYTES", 20000):
            shards = sob2.shard_blocks(self.blocks, CID)
        self.assertGreater(len(shards), 1)
        self.assertTrue(all(len(b) <= 20000 for b in shards))
        scb2.pack_scb2(shards, CID)
        with mock.patch.object(sob2, "MAX_SHARD_BYTES", 4000):
            with self.assertRaises(FormatError):
                sob2.shard_blocks(self.blocks, CID)

    def test_small_or_duplicate_corpus_rejected(self):
        with self.assertRaises(FormatError):
            scb2.pack_scb2([sob2.pack_sob2(self.blocks[:10], CID)], CID)
        with self.assertRaises(FormatError):
            sob2.shard_blocks(self.blocks[:-1], CID)
        with self.assertRaises(FormatError):
            sob2.shard_blocks(self.blocks[:-1] + [self.blocks[0]], CID)

    def test_cross_file_corruption_and_mixed_identity(self):
        for shards in ([], self.shards * 2, [sob2.pack_sob2(self.blocks, OTHER)]):
            with self.assertRaises(FormatError):
                validate_bundle(self.catalog, self.pinyin, shards)
        with self.assertRaises(FormatError):
            validate_bundle(self.catalog, spy2.pack_spy2(self.readings, OTHER), self.shards)
        swapped = [dict(c) for c in self.readings]
        swapped[0]["rank"], swapped[1]["rank"] = swapped[1]["rank"], swapped[0]["rank"]
        with self.assertRaises(FormatError):
            validate_bundle(self.catalog, spy2.pack_spy2(swapped, CID), self.shards)

    def test_repaired_crc_offsets_duplicates_shard_rank_mutations(self):
        desc = 64 + 3500 * 12
        for off, value, kind in ((24, 3499, "I"), (28, 33, "I"), (36, 0xFFFFFFFF, "I"),
                                  (76, 0x4E00, "I"), (80, 1, "H"), (70, 32, "H"),
                                  (72, 65535, "H"), (74, 1, "H"),
                                  (desc + 16, 1048577, "I"), (desc + 24, 2, "H"),
                                  (desc + 26, 3499, "H"), (desc + 30, 1, "H")):
            with self.subTest(offset=off), self.assertRaises(FormatError):
                scb2.validate_scb2(changed(self.catalog, off, value, kind))

    def test_catalog_mutation_campaign(self):
        for offset in random.Random(2).sample(range(len(self.catalog)), 200):
            data = bytearray(self.catalog)
            data[offset] ^= 1
            with self.assertRaises(FormatError):
                scb2.validate_scb2(bytes(data))


class DirectoryBoundsTests(unittest.TestCase):
    def make_profile(self, path, runtime, shards=6):
        limits = dict(PACKAGE_MEMBER_LIMITS if runtime else GENERATED_MEMBER_LIMITS)
        limits.update({scb2.shard_name(i): 1048576 for i in range(shards)})
        write_closed(path, {name: b"x" for name in limits})
        return limits

    def assert_no_content_read(self, path, runtime, message):
        with mock.patch.object(corpus2, "_read_checked") as read:
            with self.assertRaisesRegex(FormatError, message):
                verify_profile_directory(path, runtime=runtime)
            read.assert_not_called()

    def test_unexpected_16mib_sparse_member_rejected_before_any_read(self):
        for runtime in (False, True):
            with self.subTest(runtime=runtime), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp)
                self.make_profile(path, runtime)
                with (path / "extra").open("wb") as stream:
                    stream.truncate(16 * 1024 * 1024)
                # Even a manifest-listed extra cannot authorize content reads.
                with (path / "SHA256SUMS").open("ab") as stream:
                    stream.write(b"0" * 64 + b"  extra\n")
                self.assert_no_content_read(path, runtime, "unexpected bundle members")

    def test_32_sparse_shards_aggregate_and_64_fake_shards_rejected_before_read(self):
        for runtime in (False, True):
            for count in (32, 64):
                with self.subTest(runtime=runtime, count=count), tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp)
                    self.make_profile(path, runtime, count)
                    for i in range(count):
                        with (path / scb2.shard_name(i)).open("wb") as stream:
                            stream.truncate(1048576)
                    self.assert_no_content_read(path, runtime, "aggregate" if count == 32 else "shard names")

    def test_every_profile_member_and_manifest_size_limit_before_read(self):
        for runtime in (False, True):
            limits = dict(PACKAGE_MEMBER_LIMITS if runtime else GENERATED_MEMBER_LIMITS)
            limits.update({"so00.bin": 1048576, "SHA256SUMS": corpus2.MANIFEST_LIMIT})
            for name, limit in limits.items():
                with self.subTest(runtime=runtime, name=name), tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp)
                    self.make_profile(path, runtime)
                    with (path / name).open("wb") as stream:
                        stream.truncate(limit + 1)
                    self.assert_no_content_read(path, runtime, "exceeds bound")

    def test_symlink_directory_fifo_and_negative_size_before_read(self):
        import stat
        from types import SimpleNamespace
        for kind in ("symlink", "directory", "fifo", "negative"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp)
                self.make_profile(path, True)
                member = path / "so00.bin"
                if kind == "negative":
                    original = Path.lstat
                    def negative(p, *args, **kwargs):
                        return (SimpleNamespace(st_mode=stat.S_IFREG, st_size=-1)
                                if p == member else original(p, *args, **kwargs))
                    with mock.patch.object(Path, "lstat", negative):
                        self.assert_no_content_read(path, True, "exceeds bound")
                    continue
                member.unlink()
                if kind == "symlink":
                    member.symlink_to(path / "NOTICE.md")
                elif kind == "directory":
                    member.mkdir()
                else:
                    os.mkfifo(member)
                self.assert_no_content_read(path, True, "symlink/non-file")

    def test_strict_checksum_bytes_grammar(self):
        line = b"0" * 64 + b"  one\n"
        self.assertEqual(corpus2.parse_checksums(line), {"one": "0" * 64})
        invalid = [b"", line.replace(b"\n", b"\r\n"), line[:-1],
                   line.replace(b"\n", b"\r"), line + line,
                   line.replace(b"one", b"SHA256SUMS"), line.replace(b"one", b"../one"),
                   line.replace(b"one", b"one\\two"), line.replace(b"  ", b" "),
                   b"A" * 64 + b"  one\n", line.replace(b"one", "汉".encode()),
                   line.replace(b"\n", "\u2028".encode()) + line,
                   line.replace(b"\n", "\u2029".encode()) + b"\n",
                   line.replace(b"one", b"on\re")]
        for data in invalid:
            with self.subTest(data=data), self.assertRaises(FormatError):
                corpus2.parse_checksums(data)

    def test_manifest_grammar_rejected_by_directory_verifier(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            write_closed(path, {"one": b"abc"})
            line = (path / "SHA256SUMS").read_bytes()
            for data in (line.replace(b"\n", b"\r\n"), line[:-1],
                         line.replace(b"\n", b"\r"), line.replace(b"\n", "\u2028".encode()) + b"\n"):
                (path / "SHA256SUMS").write_bytes(data)
                with self.assertRaises(FormatError):
                    verify_directory(path, member_limits={"one": 3}, aggregate_limit=128)

    def test_exact_aggregate_includes_manifest_and_lazy_view_has_no_byte_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            write_closed(path, {"one": b"abc", "two": b"def"})
            size = sum(p.stat().st_size for p in path.iterdir())
            with mock.patch.object(corpus2, "_read_checked") as read:
                with self.assertRaisesRegex(FormatError, "aggregate"):
                    verify_directory(path, member_limits={"one": 3, "two": 3}, aggregate_limit=size - 1)
                read.assert_not_called()
            with mock.patch.object(corpus2, "_read_checked", wraps=corpus2._read_checked) as read:
                files = verify_directory(path, member_limits={"one": 3, "two": 3}, aggregate_limit=size)
                self.assertIsInstance(files, corpus2.VerifiedFiles)
                self.assertEqual([c.kwargs["collect"] for c in read.call_args_list], [True, False, False])
                self.assertEqual(files["one"], b"abc")
                self.assertTrue(read.call_args.kwargs["collect"])
                with self.assertRaises(KeyError):
                    files["extra"]
            (path / "one").write_bytes(b"xyz")
            with self.assertRaisesRegex(FormatError, "changed"):
                files["one"]

    def test_streaming_hash_uses_bounded_reads(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            payload = bytes(3 * corpus2.HASH_CHUNK_BYTES + 17)
            write_closed(path, {"one": payload})
            original = os.fdopen
            requests = []
            def checked_open(*args, **kwargs):
                stream = original(*args, **kwargs)
                reader = mock.MagicMock(wraps=stream)
                reader.__enter__.return_value = reader
                reader.__exit__.side_effect = stream.__exit__
                def bounded_read(size=-1):
                    self.assertGreater(size, 0)
                    self.assertLessEqual(size, corpus2.HASH_CHUNK_BYTES)
                    requests.append(size)
                    return stream.read(size)
                reader.read.side_effect = bounded_read
                return reader
            with mock.patch.object(corpus2.os, "fdopen", checked_open):
                files = verify_directory(path, member_limits={"one": len(payload)},
                                         aggregate_limit=len(payload) + 128)
                self.assertEqual(files["one"], payload)
            self.assertGreaterEqual(requests.count(corpus2.HASH_CHUNK_BYTES), 6)


class BundleTests(unittest.TestCase):
    def test_closed_checksum_directory_rejects_mutations(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            data = b"abc"
            (p / "one").write_bytes(data)
            sums = hashlib.sha256(data).hexdigest() + "  one\n"
            (p / "SHA256SUMS").write_text(sums)
            self.assertEqual(verify_directory(p, member_limits={"one": 3}, aggregate_limit=128), {"one": data})
            for text in (sums + sums, sums.replace("one", "../one"), "bad  one\n"):
                (p / "SHA256SUMS").write_text(text)
                with self.assertRaises(FormatError):
                    verify_directory(p, member_limits={"one": 3}, aggregate_limit=128)
            (p / "SHA256SUMS").write_text(sums)
            (p / "extra").write_bytes(b"")
            with self.assertRaises(FormatError):
                verify_directory(p, member_limits={"one": 3}, aggregate_limit=128)
            (p / "extra").unlink()
            (p / "one").unlink()
            (p / "one").symlink_to(p / "SHA256SUMS")
            with self.assertRaises(FormatError):
                verify_directory(p, member_limits={"one": 3}, aggregate_limit=128)

    def test_v1_2000_closed_golden_hashes_and_limits(self):
        p = ROOT / "scripts/tests/fixtures/stroke_order/prototype_2000"
        for line in (p / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split("  ")
            self.assertEqual(hashlib.sha256((p / name).read_bytes()).hexdigest(), digest, name)
        from stroke_order.pinyin_constants import MAX_READINGS_PER_CHARACTER, MAX_GROUP_MEMBERS
        from stroke_order.constants import MAX_CHARACTER_BYTES, MAX_FILE_BYTES
        self.assertEqual((MAX_READINGS_PER_CHARACTER, MAX_GROUP_MEMBERS), (8, 32))
        self.assertEqual((MAX_CHARACTER_BYTES, MAX_FILE_BYTES), (16384, 1048576))

    def test_clis_expose_independent_help(self):
        for script in ("generate_stroke_order_3500.py", "package_stroke_order_3500.py"):
            p = subprocess.run([sys.executable, str(ROOT / "scripts" / script), "--help"], capture_output=True)
            self.assertEqual(p.returncode, 0, p.stderr.decode())

    def test_real_generated_bundle_when_requested(self):
        # Opt-in external evidence, no downloaded or generated fixture committed.
        path = os.environ.get("STROKE3500_BUNDLE")
        if path is None:
            self.skipTest("STROKE3500_BUNDLE not set; external real bundle not tested")
        checked = checked_real_bundle(path)
        spy = checked["parsed"]["pinyin"]
        self.assertEqual(spy["relations"], 5367)
        self.assertEqual(spy["aux"], 1233)
        self.assertEqual(max(map(len, spy["groups"])), 37)
        dun = next(c for c in spy["characters"] if c["codepoint"] == ord("敦"))
        self.assertEqual(len(dun["readings"]), 10)
        self.assertEqual(len(checked["shard_names"]), 6)


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Independent synthetic structural package, no external bundle dependency.
        with heatshrink.ReferenceCodec() as codec:
            blocks = [sob2.encode_record(raw_glyph(0x4E00 + i), i + 1, codec) for i in range(3500)]
        shards = sob2.shard_blocks(blocks, CID)
        cls.files = {scb2.shard_name(i): b for i, b in enumerate(shards)}
        cls.files.update({"stroke_cat.bin": scb2.pack_scb2(shards, CID),
                          "stroke_pinyin.bin": spy2.pack_spy2(synthetic_readings(3500), CID),
                          "NOTICE.md": NOTICE_TEXT.encode()})
        golden = ROOT / "scripts/tests/fixtures/stroke_order/prototype_2000"
        for name in ("ARPHICPL.TXT", "UNICODE-LICENSE.txt"):
            cls.files[name] = (golden / name).read_bytes()
        cls.manifest = package_manifest(cls.files, list(cls.files), CID.hex())
        cls.files["package.json"] = canonical_json(cls.manifest)

    def test_closed_package_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_closed(Path(tmp), self.files)
            self.assertEqual(verify_package(Path(tmp)), self.manifest)

    def test_truncate_extra_missing_mixed_corpus_rehashed(self):
        mutations = []
        files = dict(self.files)
        files["so00.bin"] = files["so00.bin"][:-1]
        mutations.append(files)
        files = dict(self.files, extra=b"extra")
        mutations.append(files)
        files = dict(self.files)
        del files["so00.bin"]
        mutations.append(files)
        files = dict(self.files)
        files["stroke_pinyin.bin"] = spy2.pack_spy2(synthetic_readings(3500), OTHER)
        mutations.append(files)
        for i, files in enumerate(mutations):
            with self.subTest(mutation=i), tempfile.TemporaryDirectory() as tmp:
                write_closed(Path(tmp), files)
                with self.assertRaises(FormatError):
                    verify_package(Path(tmp))

    def test_manifest_all_fields_closed_and_rehashed(self):
        for key, value in (("format", "other"), ("corpus_id", OTHER.hex()),
                           ("device_compatible", True), ("extra", 0), ("files", {})):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as tmp:
                files = dict(self.files)
                files["package.json"] = canonical_json(dict(self.manifest, **{key: value}))
                write_closed(Path(tmp), files)
                with self.assertRaises(FormatError):
                    verify_package(Path(tmp))
        for key, value in (("bytes", 1), ("sha256", "0" * 64), ("extra", 1)):
            with self.subTest(file_field=key), tempfile.TemporaryDirectory() as tmp:
                manifest = json.loads(canonical_json(self.manifest))
                manifest["files"]["so00.bin"][key] = value
                write_closed(Path(tmp), dict(self.files, **{"package.json": canonical_json(manifest)}))
                with self.assertRaises(FormatError):
                    verify_package(Path(tmp))


class RealPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = os.environ.get("STROKE3500_BUNDLE")
        if path is None:
            raise unittest.SkipTest("STROKE3500_BUNDLE not set; real provenance tests not run")
        cls.path = Path(path)
        cls.checked = checked_real_bundle(path)  # invalid/missing requested paths must fail

    def check_diagnostics(self, files):
        verify_diagnostics(files, self.checked["parsed"], bytes.fromhex(self.checked["corpus_id"]),
                           self.checked["shard_names"])

    def test_every_coverage_field_recomputed(self):
        original = self.checked["files"]
        for key in json.loads(original["coverage.json"])[0]:
            with self.subTest(key=key):
                rows = json.loads(original["coverage.json"])
                old = rows[2001][key]
                rows[2001][key] = old + 1 if isinstance(old, int) else "0" * 64
                files = dict(original, **{"coverage.json": canonical_json(rows)})
                with self.assertRaises(FormatError):
                    self.check_diagnostics(files)

    def test_every_runtime_field_recomputed(self):
        original = self.checked["files"]
        runtime = json.loads(original["runtime.json"])
        for key, old in runtime.items():
            value = (not old if isinstance(old, bool) else old + 1 if isinstance(old, int)
                     else [] if isinstance(old, list) else {} if isinstance(old, dict) else "mutated")
            with self.subTest(key=key):
                files = dict(original, **{"runtime.json": canonical_json(dict(runtime, **{key: value}))})
                with self.assertRaises(FormatError):
                    self.check_diagnostics(files)

    def test_self_consistent_checksum_rewrites_do_not_authorize_diagnostics(self):
        for filename, key in (("source.json", "source_manifest_sha256"),
                              ("runtime.json", "conditional_assets_bytes"),
                              ("coverage.json", "source_sha256")):
            with self.subTest(filename=filename), tempfile.TemporaryDirectory() as tmp:
                files = dict(self.checked["files"])
                obj = json.loads(files[filename])
                target = obj[2001] if isinstance(obj, list) else obj
                target[key] = 1 if isinstance(target[key], int) else "0" * 64
                files[filename] = canonical_json(obj)
                write_closed(Path(tmp), files)
                verify_profile_directory(Path(tmp))  # manifest is deliberately self-consistent
                with self.assertRaises(FormatError):
                    verify_generated(Path(tmp))

    def test_real_package_cli_and_reverify(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "package"
            command = [sys.executable, str(ROOT / "scripts/package_stroke_order_3500.py")]
            for args in (["--source", str(self.path), "--output", str(output)],
                         ["--verify", str(output)]):
                result = subprocess.run(command + args, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertEqual(json.loads(result.stdout)["corpus_id"], self.checked["corpus_id"])


if __name__ == "__main__":
    unittest.main()
