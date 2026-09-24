"""Deterministic C 0.4.1 encoder and independent strict host decoder (10/4).

Builds only into a caller-owned temporary directory. No Python reimplementation
of the encoder's match selection; no alternate codec or silent fallback.
"""
from __future__ import annotations

import ctypes
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from .v2 import MAX_STORED_BYTES, MAX_TRANSFORMED_BYTES, require

ROOT = Path(__file__).resolve().parents[2]

# Reviewed heatshrink 0.4.1 sources, independent of mutable CHECKSUMS.json.
CODEC_SOURCE_SHA256 = {
    "heatshrink_decoder.c": "5ced6bb057741a5772edaf46f7c8466b5da1751d7e704ffdb79eedd5aadf7821",
    "heatshrink_encoder.c": "67ec42bb911790cab8e2c2ec5f8d5f81d6feb3a121609d8e8fd1381706e5484c",
    "include/heatshrink_common.h": "b64f5af3614f6f892d0e01cbe030bf13f1f781a018840a27b64ede82d6971ef3",
    "include/heatshrink_config.h": "f27e1236ea413ac3393af066fb62489b946edec47e3a3042e63eed64208f092f",
    "include/heatshrink_decoder.h": "2678f67add847b08b5debd234e2435135f3b83c8e5cee565789cf9f8c4887096",
    "include/heatshrink_encoder.h": "941a4f89e29a774faf2d2ce3ef86a23e3fb7c42fb04dd9e68d0abb207ba9e950"
}


def verified_source_hashes() -> dict[str, str]:
    source = ROOT / "managed_components/laride__heatshrink"
    hashes = {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
              for name in CODEC_SOURCE_SHA256}
    require(hashes == CODEC_SOURCE_SHA256, "heatshrink source checksum mismatch")
    return hashes



def decode(data: bytes, expected: int) -> bytes:
    require(isinstance(data, bytes) and 1 <= len(data) <= MAX_STORED_BYTES,
            "heatshrink input bound")
    require(1 <= expected <= MAX_TRANSFORMED_BYTES, "heatshrink output bound")
    position = 0
    total_bits = len(data) * 8

    def bits(count: int) -> int:
        nonlocal position
        require(position + count <= total_bits, "truncated heatshrink token")
        value = 0
        for _ in range(count):
            value = (value << 1) | ((data[position // 8] >> (7 - position % 8)) & 1)
            position += 1
        return value

    output = bytearray()
    while len(output) < expected:
        if bits(1):
            output.append(bits(8))
        else:
            distance, length = bits(10) + 1, bits(4) + 1
            require(len(output) + length <= expected, "heatshrink decompression bomb")
            for _ in range(length):
                # The reference starts with a zero-filled 1024-byte window.
                index = len(output) - distance
                output.append(output[index] if index >= 0 else 0)
    remaining = total_bits - position
    require(remaining < 8, "heatshrink trailing input")
    require(bits(remaining) == 0, "heatshrink nonzero padding")
    return bytes(output)


class ReferenceCodec:
    """Context-managed fresh compilation of the repository C reference."""

    def __init__(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="stroke-hs041-")
        try:
            source = ROOT / "managed_components/laride__heatshrink"
            self.source_hashes = verified_source_hashes()
            library = Path(self._temporary.name) / "codec.so"
            command = ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-dynamiclib" if sys.platform == "darwin" else "-shared", "-fPIC",
                       "-I", str(source / "include"),
                       str(Path(__file__).with_name("heatshrink_host.c")),
                       str(source / "heatshrink_encoder.c"), str(source / "heatshrink_decoder.c"),
                       "-o", str(library)]
            subprocess.run(command, check=True, capture_output=True)
            self._library = ctypes.CDLL(str(library))
            for name in ("stroke_hs_encode", "stroke_hs_decode"):
                function = getattr(self._library, name)
                function.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                     ctypes.c_void_p, ctypes.c_size_t]
                function.restype = ctypes.c_int
        except BaseException:
            self.close()
            raise

    def close(self) -> None:
        self._temporary.cleanup()

    def __enter__(self) -> ReferenceCodec:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def encode(self, data: bytes) -> bytes:
        require(isinstance(data, bytes) and 1 <= len(data) <= MAX_TRANSFORMED_BYTES,
                "heatshrink encode input bound")
        buffer = ctypes.create_string_buffer(MAX_STORED_BYTES + 1)
        size = self._library.stroke_hs_encode(data, len(data), buffer, len(buffer))
        require(0 < size <= MAX_STORED_BYTES, "C heatshrink encoder failed")
        stored = buffer.raw[:size]
        require(self.decode_reference(stored, len(data)) == data,
                "C encoder/decoder mismatch")
        return stored

    def decode_reference(self, stored: bytes, expected: int) -> bytes:
        strict = decode(stored, expected)
        buffer = ctypes.create_string_buffer(expected + 1)
        size = self._library.stroke_hs_decode(stored, len(stored), buffer, len(buffer))
        require(size == expected and buffer.raw[:size] == strict,
                "strict/C heatshrink decoder mismatch")
        return strict
