#!/usr/bin/env python3
"""Decrypt global-metadata.dat (Unity il2cpp metadata version 39, Oxide build).

Ground truth from libil2cpp.so loader sub_4D765C8: every uint32 field of the
metadata HEADER is XORed with 0xA5C3F19D. The magic (0x00) and version (0x04)
are stored in plaintext; section data (after the header) is not encrypted.

The loader only un-XORs the six fields it needs (0xA0, 0xB8, 0xC4, 0xD0, 0xDC,
0xE8), but other loaders un-XOR the rest, so the WHOLE header is encrypted.
Header region for this build is [0x08, 0x17C): the first plaintext dword is at
0x17C. Decrypting the full header makes all 47 (offset,size) pairs land in-range
and below 2^31, which is what Il2CppDumper requires.

Usage:
    python3 decrypt_metadata.py input.dat [output.dat]
"""

from pathlib import Path
import struct
import sys

MAGIC = 0xFAB11BAF
VERSION = 39
KEY = 0xA5C3F19D
HDR_START = 0x08
HDR_END = 0x17C  # first plaintext dword; end of the encrypted v39 header


def read_u32(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def validate(data: bytearray) -> None:
    n = len(data)
    bad = huge = 0
    for offset in range(HDR_START, HDR_END, 8):
        off = read_u32(data, offset)
        size = read_u32(data, offset + 4) if offset + 4 < HDR_END else 0
        if off > n:
            bad += 1
        if off > 0x7FFFFFFF or size > 0x7FFFFFFF:
            huge += 1
    print(f"version: {read_u32(data, 4)}")
    print(f"offsets past EOF: {bad}   fields >= 2^31: {huge}")
    if bad or huge:
        print("WARNING: decryption looks off")
    else:
        print("OK: every header field is valid and in-range")


def main() -> None:
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)

    source = Path(sys.argv[1])
    destination = (
        Path(sys.argv[2])
        if len(sys.argv) == 3
        else source.with_name(f"{source.stem}.decrypted{source.suffix}")
    )
    data = bytearray(source.read_bytes())

    if len(data) <= HDR_END:
        raise SystemExit("metadata file is too small")
    if read_u32(data, 0) != MAGIC:
        raise SystemExit("unexpected metadata magic")
    if read_u32(data, 4) != VERSION:
        raise SystemExit(f"unexpected metadata version: {read_u32(data, 4)}")

    print(f"key: 0x{KEY:08X}")
    print(f"header: 0x{HDR_START:X}..0x{HDR_END:X} ({(HDR_END - HDR_START) // 4} dwords)")
    for offset in range(HDR_START, HDR_END, 4):
        struct.pack_into("<I", data, offset, read_u32(data, offset) ^ KEY)

    validate(data)
    destination.write_bytes(data)
    print(f"written: {destination} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
