"""Focused W4a: production readers + resolved C decoder, no IDF/product integration."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from stroke_order import dzz1, heatshrink, spy2, v2  # noqa: E402


def refresh(blob):
    struct.pack_into("<I", blob, 40, len(blob))
    struct.pack_into("<I", blob, 44, zlib.crc32(blob[64:]))
    struct.pack_into("<I", blob, 48, 0)
    struct.pack_into("<I", blob, 48, zlib.crc32(blob[:64]))
    return bytes(blob)


class DeviceV2Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="stroke-v2-device-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.directory = Path(cls.tmp.name)
        cls.exe = cls.directory / "reader"
        cls.source_hashes = heatshrink.verified_source_hashes()
        source = ROOT / "managed_components/laride__heatshrink"
        flags = ["-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(["cc", "-std=c11", *flags, "-I", str(source / "include"), "-c",
                        str(source / "heatshrink_decoder.c"), "-o", str(cls.directory / "hs.o")], check=True)
        command = [os.environ.get("CXX", "c++"), "-std=c++17", *flags,
                   "-fno-exceptions", "-fno-rtti", "-DSTROKE_ORDER_TESTING=1", "-I", str(ROOT / "main"),
                   "-I", str(source / "include")]
        if sys.platform == "darwin":
            sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
            command += ["-isystem", f"{sdk}/usr/include/c++/v1"]
        command += [str(ROOT / "scripts/tests/stroke_order_v2_harness.cc"),
                    str(ROOT / "main/stroke_order/stroke_order_v2.cc"),
                    str(ROOT / "main/stroke_order/stroke_order_store.cc"),
                    str(cls.directory / "hs.o"), "-o", str(cls.exe)]
        subprocess.run(command, check=True)
        cls.codec = heatshrink.ReferenceCodec()
        cls.addClassCleanup(cls.codec.close)

    def run_reader(self, mode, blob, size=None, good=False):
        path = self.directory / "input.bin"
        path.write_bytes(blob)
        args = [str(self.exe), mode, str(path)]
        if size is not None:
            args.append(str(size))
        run = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0 if good else 1, run.stdout + run.stderr)
        self.assertNotIn("Sanitizer", run.stderr)

    def external(self):
        path = os.environ.get("STROKE3500_BUNDLE")
        return Path(path) if path else ROOT / "scripts/tests/fixtures/stroke_order/prototype_3500"

    def test_full_fresh_corpus_and_legacy_raw_identity(self):
        bundle = self.external()
        dump = self.directory / "raw-records.bin"
        run = subprocess.run([str(self.exe), "bundle", str(bundle), str(dump)],
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("PASS 3500 6 37", run.stdout)
        coverage = {r["rank"]: r for r in json.loads((bundle / "coverage.json").read_text())}
        records = {}; data = dump.read_bytes(); pos = 0
        while pos < len(data):
            rank, stored, transformed, size = struct.unpack_from("<4I", data, pos); pos += 16
            raw = data[pos:pos + size]; pos += size
            cp, strokes, _ = struct.unpack_from("<IHH", raw)
            row = coverage[rank]
            self.assertNotIn(rank, records)
            self.assertEqual((cp, strokes, size, stored, transformed, hashlib.sha256(raw).hexdigest()),
                             (row["codepoint"], row["strokes"], row["raw_bytes"], row["stored_bytes"],
                              row["transformed_bytes"], row["raw_sha256"]))
            records[rank] = raw
        self.assertEqual(set(records), set(range(1, 3501)))
        for rank in (1, 2000, 2001, 3500):
            self.assertEqual(struct.unpack_from("<I", records[rank])[0], coverage[rank]["codepoint"])
        # Byte-exact legacy raw records, not only geometry or aggregate checksums.
        old = ROOT / "scripts/tests/fixtures/stroke_order/prototype_2000"
        by_cp = {struct.unpack_from("<I", raw)[0]: raw for raw in records.values()}
        legacy_count = 0
        for path in sorted(old.glob("so*.sob1")):
            blob = path.read_bytes(); count = struct.unpack_from("<I", blob, 8)[0]
            for i in range(count):
                cp, offset, size, _ = struct.unpack_from("<4I", blob, 32 + 16 * i)
                self.assertEqual(by_cp[cp], blob[offset:offset + size])
                legacy_count += 1
        self.assertEqual(legacy_count, 2000)
        parsed = spy2.validate_spy2((bundle / "stroke_pinyin.bin").read_bytes())
        chars = parsed["characters"]
        expected = {}
        for c, char in enumerate(chars):
            members = {i for g in char["readings"] for i in parsed["groups"][g]} - {c}
            expected[char["codepoint"]] = [len(char["readings"])] + [
                chars[i]["codepoint"] for i in sorted(members, key=lambda i: chars[i]["rank"])[:6]]
        actual = {}
        for line in run.stdout.splitlines():
            if line.startswith("P "):
                cp, *values = map(int, line.split()[1:]); actual[cp] = values
        self.assertEqual(actual, expected)
        self.assertEqual(actual[ord("敦")][0], 10)
        self.assertEqual(len(actual), 3500)
        self.assertEqual(heatshrink.verified_source_hashes(), self.source_hashes)

    def test_dzz_and_heatshrink_strict_edges(self):
        raw = struct.pack("<IHHHH", 0x4e00, 1, 0, 4, 2) + struct.pack("<12H", *([0] * 12))
        transformed = dzz1.encode(raw)
        self.run_reader("dzz", transformed, len(raw), good=True)
        encoded = self.codec.encode(transformed)
        self.run_reader("hs", encoded, len(transformed), good=True)
        for blob in (transformed[:-1], transformed + b"\0", b"BAD!" + transformed[4:],
                     transformed[:16] + b"\x80\0" + transformed[17:],
                     transformed[:16] + b"\xff\x11" + transformed[17:],
                     transformed[:16] + b"\x81\x10" + transformed[17:],
                     transformed[:16] + b"\x01" + transformed[17:]):
            with self.subTest(dzz=blob.hex()):
                self.run_reader("dzz", blob, len(raw))
        for end in range(len(transformed)):
            self.run_reader("dzz", transformed[:end], len(raw))
        overflow = bytearray(transformed)
        overflow[18] = 2  # second point x delta +1 after the first point's x=1024
        overflow[16:17] = b"\x80\x10"
        self.run_reader("dzz", overflow, len(raw))
        closure = bytearray(transformed); closure[22] = 2
        self.run_reader("dzz", closure, len(raw))
        self.run_reader("dzz", transformed, len(raw) - 1)
        self.run_reader("dzz", transformed, len(raw) + 1)
        # A literal 'A' token is 9 bits plus 7 zero padding bits.
        self.run_reader("hs", b"\xa0\x80", 1, good=True)
        self.run_reader("hs", b"\xa0\x81", 1)  # nonzero padding
        self.run_reader("hs", b"\xa0\x80\0", 1)  # whole trailing byte
        self.run_reader("hs", b"\xa0", 1)  # truncated literal
        self.run_reader("hs", b"\x00\x1e", 1)  # zero history, length16 bomb
        self.run_reader("hs", b"\x00\x1e", 16, good=True)
        self.run_reader("hs", b"\0" * 16385, 1)  # device stored admission
        self.run_reader("hs", encoded, len(transformed) - 1)
        self.run_reader("hs", encoded, len(transformed) + 1)

    def test_envelope_shard_catalog_corruption(self):
        bundle = self.external()
        for mode, name in (("shard", "so00.bin"), ("catalog", "stroke_cat.bin"),
                           ("pinyin", "stroke_pinyin.bin")):
            original = (bundle / name).read_bytes()
            self.run_reader(mode, original, good=True)
            for offset in (0, 4, 6, 8, 40, 44, 48, 56, 64, len(original) - 1):
                bad = bytearray(original); bad[offset] ^= 1
                with self.subTest(mode=mode, checksum_offset=offset):
                    self.run_reader(mode, bad)
            for bad in (original[:63], original[:-1], original + b"\0"):
                self.run_reader(mode, bad)
            for offset, value in ((24, 0xffffffff), (28, 0xffffffff), (32, 65),
                                  (36, 0xffffffff), (52, 0xffffffff), (56, 1)):
                bad = bytearray(original); struct.pack_into("<I", bad, offset, value)
                self.run_reader(mode, refresh(bad))
            bad = bytearray(original); bad[8:24] = bytes(16)
            self.run_reader(mode, refresh(bad))
        original = (bundle / "so00.bin").read_bytes()
        for delta in (-1, 1):
            bad = bytearray(original)
            second_offset = struct.unpack_from("<I", bad, 100)[0]
            struct.pack_into("<I", bad, 100, second_offset + delta)
            self.run_reader("shard", refresh(bad))
        for bad in (bytearray(original[:-1]), bytearray(original + b"\0")):
            self.run_reader("shard", refresh(bad))
        for offset, value in ((68, 0xffffffff), (68, struct.unpack_from("<I", original, 68)[0] - 1),
                              (68, struct.unpack_from("<I", original, 68)[0] + 1),
                              (72, 16385), (76, 16389), (80, 16385), (84, 0), (88, 0),
                              (96, struct.unpack_from("<I", original, 64)[0])):
            bad = bytearray(original); struct.pack_into("<I", bad, offset, value)
            self.run_reader("shard", refresh(bad))
        original = (bundle / "stroke_cat.bin").read_bytes()
        for offset, fmt, value in ((76, "I", struct.unpack_from("<I", original, 64)[0]),
                                   (80, "H", struct.unpack_from("<H", original, 68)[0]),
                                   (70, "H", 32), (72, "H", 3500), (74, "H", 1)):
            bad = bytearray(original); struct.pack_into("<" + fmt, bad, offset, value)
            self.run_reader("catalog", refresh(bad))
        desc = struct.unpack_from("<I", original, 36)[0]
        for offset in (desc, desc + 8, desc + 24, desc + 26, desc + 28, desc + 30, desc + 32):
            bad = bytearray(original); bad[offset] ^= 1
            self.run_reader("catalog", refresh(bad))

    def test_csr_bounds_duplicates_and_reciprocity(self):
        # Nontrivial CSR with forward ordering independent of numeric group IDs.
        corpus = bytes(range(16))
        blob = spy2.pack_spy2([
            {"codepoint": 0x4e00, "rank": 2, "readings": ["yi1", "er4"]},
            {"codepoint": 0x4e01, "rank": 1, "readings": ["yi1"]}], corpus)
        self.run_reader("pinyin", blob, good=True)
        co, edges, go, members = 80, 92, 98, 110
        for offset, fmt, value in ((72, "I", 0x4e00), (76, "H", 2),
                                   (co, "I", 1), (co + 4, "I", 0), (co + 8, "I", 4),
                                   (edges, "H", 2), (edges + 2, "H", 1),
                                   (go, "I", 1), (go + 4, "I", 0), (go + 8, "I", 4),
                                   (members, "H", 1), (members + 2, "H", 2),
                                   (members + 4, "H", 1)):
            bad = bytearray(blob); struct.pack_into("<" + fmt, bad, offset, value)
            with self.subTest(offset=offset, value=value):
                self.run_reader("pinyin", refresh(bad))

    def test_csr_limit_boundaries(self):
        corpus = bytes(range(16))
        # Construct CSR bytes directly to test limits the compliant host packer rejects.
        def csr(n, groups, forward):
            reverse = [[] for _ in range(groups)]
            co, edges = [0], []
            for c, values in enumerate(forward):
                edges.extend(values); co.append(len(edges))
                for g in values:
                    reverse[g].append(c)
            go, members = [0], []
            for values in reverse:
                members.extend(values); go.append(len(members))
            body = b"".join(struct.pack("<IHH", 0x4e00 + c, c + 1, 0) for c in range(n))
            body += struct.pack("<" + "I" * len(co), *co)
            body += struct.pack("<" + "H" * len(edges), *edges)
            body += struct.pack("<" + "I" * len(go), *go)
            body += struct.pack("<" + "H" * len(members), *members)
            return v2.envelope(b"SPY2", corpus, n, groups, 64, 64 + 8 * n, len(edges), body)
        self.run_reader("pinyin", csr(1, 16, [list(range(16))]), good=True)
        self.run_reader("pinyin", csr(1, 17, [list(range(17))]))
        self.run_reader("pinyin", csr(64, 1, [[0]] * 64), good=True)
        self.run_reader("pinyin", csr(65, 1, [[0]] * 65))
        self.run_reader("pinyin", csr(1, 1, [[0, 0]]))
        # 4096 legal groups with 256 characters and exactly 16 readings each.
        self.run_reader("pinyin", csr(256, 4096, [list(range(c * 16, (c + 1) * 16))
                                                     for c in range(256)]), good=True)
        self.run_reader("pinyin", csr(3500, 1000, [[(c * 16 + j) % 1000 for j in range(16)]
                                                 for c in range(3500)]), good=True)
        over = bytearray(csr(1, 1, [[0]]))
        struct.pack_into("<I", over, 52, 56001)
        self.run_reader("pinyin", refresh(over))

    def test_bundle_cross_file_identity_and_mapping(self):
        bundle = self.external()
        target = self.directory / "bundle"
        target.mkdir(exist_ok=True)
        # Copies confined to the temporary harness directory; never alter input.
        for name in ["stroke_cat.bin", "stroke_pinyin.bin"] + [f"so{i:02d}.bin" for i in range(6)]:
            shutil.copyfile(bundle / name, target / name)
        def reject():
            for mode in ("structural", "bundle"):
                run = subprocess.run([str(self.exe), mode, str(target), str(self.directory / "bad.raw")],
                                     capture_output=True, text=True)
                self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
                self.assertNotIn("Sanitizer", run.stderr)
        py = bytearray((target / "stroke_pinyin.bin").read_bytes()); py[8] ^= 1
        (target / "stroke_pinyin.bin").write_bytes(refresh(py)); reject()
        shutil.copyfile(bundle / "stroke_pinyin.bin", target / "stroke_pinyin.bin")
        cat = bytearray((target / "stroke_cat.bin").read_bytes())
        desc = struct.unpack_from("<I", cat, 36)[0]
        cat[desc + 20] ^= 1
        (target / "stroke_cat.bin").write_bytes(refresh(cat)); reject()
        shutil.copyfile(bundle / "stroke_cat.bin", target / "stroke_cat.bin")
        shard = bytearray((target / "so00.bin").read_bytes()); shard[8] ^= 1
        shard = refresh(shard); (target / "so00.bin").write_bytes(shard)
        cat = bytearray((target / "stroke_cat.bin").read_bytes())
        struct.pack_into("<I", cat, desc + 20, zlib.crc32(shard))
        (target / "stroke_cat.bin").write_bytes(refresh(cat)); reject()
        shutil.copyfile(bundle / "so00.bin", target / "so00.bin")
        shutil.copyfile(bundle / "stroke_cat.bin", target / "stroke_cat.bin")
        # Valid SCB2 rank/local relations, but wrong codepoint-to-shard mapping.
        cat = bytearray((target / "stroke_cat.bin").read_bytes())
        a, b = bytes(cat[68:74]), bytes(cat[80:86])
        cat[68:74], cat[80:86] = b, a
        (target / "stroke_cat.bin").write_bytes(refresh(cat)); reject()
        shutil.copyfile(bundle / "stroke_cat.bin", target / "stroke_cat.bin")
        # Recomputed CRC cannot hide an invalid forward CSR offset.
        py = bytearray((target / "stroke_pinyin.bin").read_bytes())
        co = struct.unpack_from("<I", py, 36)[0]
        struct.pack_into("<I", py, co, 1)
        (target / "stroke_pinyin.bin").write_bytes(refresh(py)); reject()


if __name__ == "__main__":
    unittest.main()
