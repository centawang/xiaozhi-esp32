"""Fast production PrepareBundle regression; immutable synthetic and real v2 inputs.

Compile/setup are separate from the measured <2s prepare loop. No fixture writes.
The synthetic corpus has the minimum 3500 required entries, one shard, one tiny
closed stroke per glyph and bounded singleton pinyin groups.
"""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from stroke_order import dzz1, heatshrink, scb2, sob2, spy2, v2


def refresh(blob):
    blob = bytearray(blob)
    struct.pack_into("<I", blob, 44, zlib.crc32(blob[64:]))
    struct.pack_into("<I", blob, 48, 0)
    struct.pack_into("<I", blob, 48, zlib.crc32(blob[:64]))
    return bytes(blob)


def synthetic(directory, codec):
    corpus = bytes(range(16))
    blocks, chars = [], []
    for i in range(3500):
        cp = 0x4e00 + i
        raw = struct.pack("<IHHHH12H", cp, 1, 0, 4, 2, *([0] * 12))
        blocks.append(sob2.encode_record(raw, i + 1, codec))
        chars.append(dict(codepoint=cp, rank=i + 1, readings=["g" + "".join(chr(97 + (i // power) % 26) for power in (676, 26, 1)) + "1"]))
    shard = sob2.pack_sob2(blocks, corpus)
    (directory / "so00.bin").write_bytes(shard)
    (directory / "stroke_cat.bin").write_bytes(scb2.pack_scb2([shard], corpus))
    (directory / "stroke_pinyin.bin").write_bytes(spy2.pack_spy2(chars, corpus))
    return blocks


class PrepareTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="stroke-prepare-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.directory = Path(cls.tmp.name)
        cls.exe = cls.directory / "prepare"
        source = ROOT / "managed_components/laride__heatshrink"
        flags = ["-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(["cc", "-std=c11", *flags, "-I", str(source / "include"), "-c",
                        str(source / "heatshrink_decoder.c"), "-o", str(cls.directory / "hs.o")], check=True)
        cmd = [os.environ.get("CXX", "c++"), "-std=c++17", *flags, "-pthread",
               "-fno-exceptions", "-fno-rtti", "-DSTROKE_ORDER_TESTING=1", "-I", str(ROOT / "main"),
               "-I", str(source / "include")]
        if sys.platform == "darwin":
            sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
            cmd += ["-isystem", f"{sdk}/usr/include/c++/v1"]
        cmd += [str(ROOT / "scripts/tests/stroke_order_source_harness.cc")]
        cmd += [str(ROOT / f"main/stroke_order/stroke_order_{name}.cc") for name in
                ("controller", "source_adapter", "catalog", "pinyin", "store", "v2")]
        subprocess.run([*cmd, str(cls.directory / "hs.o"), "-o", str(cls.exe)], check=True)
        cls.codec = heatshrink.ReferenceCodec()
        cls.addClassCleanup(cls.codec.close)
        cls.bundle = cls.directory / "synthetic"
        cls.bundle.mkdir()
        cls.blocks = synthetic(cls.bundle, cls.codec)

    def run_harness(self, mode, bundle):
        run = subprocess.run([str(self.exe), mode, str(bundle)], capture_output=True,
                             text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertNotIn("Sanitizer", run.stderr)
        print(run.stdout.strip())

    def test_synthetic_prepare_loop(self):
        for _ in range(3):
            self.run_harness("prepare-perf", self.bundle)

    def test_real3500_prepare(self):
        self.run_harness("prepare-perf", ROOT / "scripts/tests/fixtures/stroke_order/prototype_3500")

    def test_checksum_valid_bad_payload_rejected_deep_and_on_demand(self):
        original = (self.bundle / "so00.bin").read_bytes()
        # All surrounding checksums are recomputed. Structural admission is NOT
        # a glyph validity promise: bad codec, DZZ1, raw CRC and geometry differ.
        raw = struct.pack("<IHHHH12H", 0x4e00, 1, 0, 4, 2, *([0] * 12))
        transformed = dzz1.encode(raw)
        bad_geometry = bytearray(transformed)
        bad_geometry[22] = 2  # last outline x != first x, nonclosed outline
        cases = {
            "compressed": bytes(len(self.blocks[0].stored)),
            "dzz": self.codec.encode(b"BAD!" + transformed[4:]),
            "geometry": self.codec.encode(bytes(bad_geometry)),
            "raw-crc": self.blocks[0].stored,
        }
        for name, payload in cases.items():
            with self.subTest(name=name):
                target = self.directory / name
                target.mkdir()
                # Rebuild index offsets/lengths without the host's deep packer,
                # intentionally creating a container with an invalid first glyph.
                n = 3500
                offset = 64 + n * 32
                index, data = bytearray(), bytearray()
                for i, block in enumerate(self.blocks):
                    stored = payload if i == 0 else block.stored
                    crc = block.raw_crc ^ (1 if i == 0 and name == "raw-crc" else 0)
                    index += sob2.INDEX.pack(block.codepoint, offset + len(data), len(stored),
                                            block.transformed_length, block.raw_length,
                                            zlib.crc32(stored), crc, block.rank, 0)
                    data += stored
                shard = v2.envelope(b"SOB2", bytes(range(16)), n, 1, 64, offset,
                                    sob2.FLAGS, bytes(index + data))
                cat = bytearray((self.bundle / "stroke_cat.bin").read_bytes())
                descriptor = 64 + 3500 * 12
                struct.pack_into("<II", cat, descriptor + 16, len(shard), zlib.crc32(shard))
                (target / "so00.bin").write_bytes(shard)
                (target / "stroke_cat.bin").write_bytes(refresh(cat))
                (target / "stroke_pinyin.bin").write_bytes((self.bundle / "stroke_pinyin.bin").read_bytes())
                self.run_harness("malicious", target)
        self.assertEqual((self.bundle / "so00.bin").read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
