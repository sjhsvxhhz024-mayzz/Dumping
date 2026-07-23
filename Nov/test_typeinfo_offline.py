#!/usr/bin/env python3
"""Self-tests for Nov/typeinfo_offline.py.

Runs anywhere with Python 3.8+ and no real libil2cpp.so:
  1. Round-trip encoder/decoder checks for MOVZ / BL / ADRP / ADD / STR.
  2. Builds a minimal synthetic ELF64 (arm64) with a hand-crafted .text
     containing several TypeInfo init patterns — three real ones and one
     red-herring MOVZ #TDI whose BL points elsewhere — and runs the actual
     scanner from typeinfo_offline.py against it, verifying that:
       (a) each real slot RVA is recovered exactly;
       (b) the red-herring is reported but distinguishable via BL target;
       (c) BL-tally cross-check picks GetTypeInfoFromTypeDefinitionIndex.

Exit code 0 on success, 1 on any failure.

Usage:
    python3 Nov/test_typeinfo_offline.py
"""

import os
import struct
import sys
import tempfile
import traceback

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

import typeinfo_offline as tio  # noqa: E402


# --- Round-trip encoder/decoder tests ----------------------------------------

def test_movz_encoding():
    for tdi in [1, 0x100, 0x1EF3, 0x203B, 0x2149, 0xFFFF]:
        b = tio.enc_movz_w0(tdi)
        assert len(b) == 4
        w = struct.unpack("<I", b)[0]
        assert (w & 0xFF800000) == 0x52800000, f"MOVZ opcode wrong for tdi={tdi:#x}: {w:#x}"
        assert ((w >> 5) & 0xFFFF) == tdi, f"imm16 wrong for tdi={tdi:#x}: {w:#x}"
        assert (w & 0x1F) == 0, f"Rd should be 0 for tdi={tdi:#x}"
    # Known-good byte sequence for tdi 8251
    assert tio.enc_movz_w0(8251) == b"\x60\x07\x84\x52", \
        f"MOVZ #8251 bytes wrong: {tio.enc_movz_w0(8251).hex()}"


def test_bl_roundtrip():
    for rel in [4, -4, 0x1000, -0x1000, 0x2000, -0x1_000_000, 0x3FF_FFFC, -0x400_0000]:
        assert rel % 4 == 0
        imm26 = (rel >> 2) & 0x03FFFFFF
        w = 0x94000000 | imm26
        got = tio.dec_bl(w)
        assert got == rel, f"BL roundtrip failed for rel={rel:#x}: got {got}"
    assert tio.dec_bl(0x00000000) is None
    assert tio.dec_bl(0x52840760) is None  # MOVZ, not BL


def test_adrp_decode():
    pc = 0x1000
    target_page = 0xB8B0000
    page_off = target_page - (pc & ~0xFFF)
    imm21 = (page_off >> 12) & 0x1FFFFF
    immlo = imm21 & 3
    immhi = (imm21 >> 2) & 0x7FFFF
    w = (1 << 31) | (immlo << 29) | (0b10000 << 24) | (immhi << 5) | 1
    rd, po = tio.dec_adrp(w)
    assert rd == 1, f"Rd wrong: {rd}"
    assert po == page_off, f"page_off wrong: got {po:#x}, want {page_off:#x}"
    assert (pc & ~0xFFF) + po == target_page
    assert tio.dec_adrp(0x52840760) is None


def test_str_uimm_decode():
    imm = 0x1AE8
    imm12 = imm >> 3
    w = 0xF9000000 | (imm12 << 10) | (1 << 5) | 0
    rt, rn, off = tio.dec_str_uimm64(w)
    assert rt == 0 and rn == 1 and off == imm, f"rt={rt} rn={rn} off={off:#x}"
    # LDR-immediate, not STR:
    assert tio.dec_str_uimm64(0xF9400020) is None


def test_add_imm_decode():
    imm = 0x40
    imm12 = imm & 0xFFF
    w = 0x91000000 | (imm12 << 10) | (1 << 5) | 2
    rd, rn, off = tio.dec_add_imm64(w)
    assert rd == 2 and rn == 1 and off == imm

    imm = 0x2000
    imm12 = imm >> 12
    w = 0x91000000 | (1 << 22) | (imm12 << 10) | (1 << 5) | 2
    rd, rn, off = tio.dec_add_imm64(w)
    assert rd == 2 and rn == 1 and off == imm


# --- Encoder helpers for building synthetic .text ---------------------------

def enc_bl(rel: int) -> int:
    assert rel % 4 == 0, f"BL rel must be 4-aligned: {rel}"
    imm26 = (rel >> 2) & 0x03FFFFFF
    return 0x94000000 | imm26


def enc_adrp(rd: int, pc: int, page_va: int) -> int:
    page_off = page_va - (pc & ~0xFFF)
    imm21 = (page_off >> 12) & 0x1FFFFF
    immlo = imm21 & 3
    immhi = (imm21 >> 2) & 0x7FFFF
    return (1 << 31) | (immlo << 29) | (0b10000 << 24) | (immhi << 5) | (rd & 0x1F)


def enc_str_uimm(rt: int, rn: int, imm: int) -> int:
    assert imm % 8 == 0, f"STR uimm must be 8-aligned: {imm:#x}"
    imm12 = imm >> 3
    assert imm12 <= 0xFFF, f"STR uimm too large: {imm:#x}"
    return 0xF9000000 | ((imm12 & 0xFFF) << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)


def emit_typeinfo_init(text: bytearray, off: int, text_va: int,
                        tdi: int, get_ti_va: int, slot_va: int) -> int:
    text[off:off + 4] = tio.enc_movz_w0(tdi)
    pc_bl = text_va + off + 4
    struct.pack_into("<I", text, off + 4, enc_bl(get_ti_va - pc_bl))
    pc_adrp = text_va + off + 8
    slot_page = slot_va & ~0xFFF
    slot_lo = slot_va & 0xFFF
    struct.pack_into("<I", text, off + 8, enc_adrp(1, pc_adrp, slot_page))
    struct.pack_into("<I", text, off + 12, enc_str_uimm(0, 1, slot_lo))
    return off + 16


# --- Synthetic ELF builder --------------------------------------------------

def build_synthetic_elf(text: bytes, text_va: int) -> bytes:
    shstrtab = b"\x00.text\x00.shstrtab\x00"
    ehdr_size = 64
    shent_size = 64
    shnum = 3  # NULL, .text, .shstrtab
    shoff = ehdr_size
    text_off = shoff + shnum * shent_size
    shstrtab_off = text_off + len(text)

    def mk_shdr(name_off, sh_type, flags, addr, off, size):
        return struct.pack(
            "<IIQQQQIIQQ",
            name_off, sh_type, flags, addr, off, size,
            0, 0, 8, 0,
        )

    text_name = shstrtab.find(b".text\x00")
    shstr_name = shstrtab.find(b".shstrtab\x00")
    assert text_name == 1 and shstr_name == 7

    sh0 = mk_shdr(0, 0, 0, 0, 0, 0)
    sh1 = mk_shdr(text_name, 1, 6, text_va, text_off, len(text))
    sh2 = mk_shdr(shstr_name, 3, 0, 0, shstrtab_off, len(shstrtab))
    assert len(sh0) == 64 and len(sh1) == 64 and len(sh2) == 64

    ehdr = bytearray(ehdr_size)
    ehdr[0:4] = b"\x7fELF"
    ehdr[4] = 2
    ehdr[5] = 1
    ehdr[6] = 1
    struct.pack_into("<H", ehdr, 0x10, 3)
    struct.pack_into("<H", ehdr, 0x12, 0xB7)
    struct.pack_into("<I", ehdr, 0x14, 1)
    struct.pack_into("<Q", ehdr, 0x28, shoff)
    struct.pack_into("<H", ehdr, 0x34, ehdr_size)
    struct.pack_into("<H", ehdr, 0x3A, shent_size)
    struct.pack_into("<H", ehdr, 0x3C, shnum)
    struct.pack_into("<H", ehdr, 0x3E, 2)

    return bytes(ehdr) + sh0 + sh1 + sh2 + text + shstrtab


# --- Full end-to-end scanner test -------------------------------------------

def test_scanner_synthetic():
    text_va = 0x10000
    text = bytearray(0x2000)

    GET_TI = 0x18000
    OTHER = 0x19000

    slot_pm = 0xB8C1AE8
    slot_bp = 0xB8B97E0
    slot_pv = 0xB8C1B48
    slot_rh = 0xC000000

    emit_typeinfo_init(text, 0x000, text_va, 8251, GET_TI, slot_pm)
    emit_typeinfo_init(text, 0x100, text_va, 8521, GET_TI, slot_bp)
    emit_typeinfo_init(text, 0x200, text_va, 7923, GET_TI, slot_pv)

    off = 0x400
    text[off:off + 4] = tio.enc_movz_w0(8251)
    pc_bl = text_va + off + 4
    struct.pack_into("<I", text, off + 4, enc_bl(OTHER - pc_bl))
    pc_adrp = text_va + off + 8
    struct.pack_into("<I", text, off + 8, enc_adrp(1, pc_adrp, slot_rh & ~0xFFF))
    struct.pack_into("<I", text, off + 12, enc_str_uimm(0, 1, slot_rh & 0xFFF))

    elf_bytes = build_synthetic_elf(bytes(text), text_va)

    fd, elf_path = tempfile.mkstemp(suffix=".so")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(elf_bytes)

        data, sections = tio.parse_elf(elf_path)
        assert ".text" in sections, "parse_elf missed .text"
        tsec = sections[".text"]
        assert tsec["addr"] == text_va, f"text addr: {tsec['addr']:#x}"
        assert tsec["size"] == len(text), f"text size: {tsec['size']:#x}"
        text_read = data[tsec["off"]: tsec["off"] + tsec["size"]]
        assert text_read == bytes(text), "parsed .text bytes differ from source"

        for tdi, want in [(8251, slot_pm), (8521, slot_bp), (7923, slot_pv)]:
            cands = tio.find_slots_for_tdi(text_read, text_va, tdi)
            assert cands, f"no candidates for tdi={tdi}"
            slots = [c[2] for c in cands]
            assert want in slots, (
                f"tdi={tdi}: expected slot {want:#x} missing; got "
                + ", ".join(f"{s:#x}" for s in slots)
            )

        cands_pm = tio.find_slots_for_tdi(text_read, text_va, 8251)
        real = [c for c in cands_pm if c[1] == GET_TI]
        herring = [c for c in cands_pm if c[1] == OTHER]
        assert real, f"real PM slot missing among candidates: {cands_pm}"
        assert herring, f"red-herring PM slot missing: {cands_pm}"
        assert real[0][2] == slot_pm, f"real PM slot mismatch: {real[0][2]:#x} vs {slot_pm:#x}"
        assert herring[0][2] == slot_rh, f"herring slot mismatch: {herring[0][2]:#x} vs {slot_rh:#x}"

        tally = {}
        for tdi in (8251, 8521, 7923):
            for _, bl, _ in tio.find_slots_for_tdi(text_read, text_va, tdi):
                tally[bl] = tally.get(bl, 0) + 1
        best_bl, best_count = max(tally.items(), key=lambda kv: kv[1])
        assert best_bl == GET_TI, (
            f"BL-tally chose wrong function: got {best_bl:#x} ({best_count}x); "
            f"tally = {[(hex(k), v) for k, v in tally.items()]}"
        )
    finally:
        try:
            os.unlink(elf_path)
        except OSError:
            pass


TESTS = [
    test_movz_encoding,
    test_bl_roundtrip,
    test_adrp_decode,
    test_str_uimm_decode,
    test_add_imm_decode,
    test_scanner_synthetic,
]


def main() -> int:
    failed = 0
    for t in TESTS:
        try:
            t()
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"  FAIL  {t.__name__}: {e!r}")
            traceback.print_exc()
        else:
            print(f"  ok    {t.__name__}")
    if failed:
        print(f"\n{failed}/{len(TESTS)} tests FAILED")
        return 1
    print(f"\nAll {len(TESTS)} tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
