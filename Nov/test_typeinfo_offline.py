#!/usr/bin/env python3
"""Self-tests for typeinfo_offline.py.

Covers both the legacy ARM64 (dis)assembly scan and the new metadata-
registration / type-index resolver (validated on a hand-built synthetic ELF).
Pure stdlib, no external binary required.

Run: python3 Nov/test_typeinfo_offline.py
"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import typeinfo_offline as tio  # noqa: E402

_passed = 0
_failed = 0


def check(name, cond, detail=""):
    global _passed, _failed
    if cond:
        _passed += 1
        print(f"  PASS  {name}")
    else:
        _failed += 1
        print(f"  FAIL  {name}  {detail}")


# --- legacy ARM64 helper tests ----------------------------------------------

def test_movz_encoding():
    # MOVZ W0,#8251 == 0x52840760 (little-endian bytes 60 07 84 52)
    check("movz W0,#8251 bytes", tio.enc_movz_w0(8251) == bytes.fromhex("60078452"),
          tio.enc_movz_w0(8251).hex())
    check("movz W0,#8521 bytes", tio.enc_movz_w0(8521) == bytes.fromhex("20298452"),
          tio.enc_movz_w0(8521).hex())
    check("movz W0,#7923 bytes", tio.enc_movz_w0(7923) == bytes.fromhex("60de8352"),
          tio.enc_movz_w0(7923).hex())


def test_bl_roundtrip():
    word = 0x94000000 | 0x40  # BL +0x100
    check("bl decode +0x100", tio.dec_bl(word) == 0x100, tio.dec_bl(word))
    check("bl decode negative", tio.dec_bl(0x97FFFFC0) == -0x100, tio.dec_bl(0x97FFFFC0))
    check("bl rejects non-bl", tio.dec_bl(0xD2800000) is None)


def test_adrp_decode():
    rd, page = tio.dec_adrp(0xB0000001)
    check("adrp rd", rd == 1, rd)
    check("adrp page off", page == 0x1000, hex(page))
    check("adrp rejects non-adrp", tio.dec_adrp(0x94000000) is None)


def test_str_uimm_decode():
    rt, rn, off = tio.dec_str_uimm64(0xF9002020)
    check("str rt", rt == 0, rt)
    check("str rn", rn == 1, rn)
    check("str byte off", off == 64, off)
    check("str rejects non-str", tio.dec_str_uimm64(0x94000000) is None)


def test_add_imm_decode():
    # ADD x0,x1,#0x10  == 0x91004020
    rd, rn, imm = tio.dec_add_imm64(0x91004020)
    check("add rd", rd == 0, rd)
    check("add rn", rn == 1, rn)
    check("add imm", imm == 0x10, hex(imm))


def test_scanner_synthetic():
    text_va = 0x1000
    words = [
        struct.unpack("<I", tio.enc_movz_w0(8251))[0],  # MOVZ W0,#8251
        0x94000000 | 0x40,                              # BL +0x100
        0xB0000001,                                     # ADRP x1, +0x1000 -> 0x2000
        0xF9002020,                                     # STR x0,[x1,#0x40]
    ]
    text = b"".join(struct.pack("<I", w) for w in words)
    cands = tio.find_slots_for_tdi(text, text_va, 8251)
    check("scanner one candidate", len(cands) == 1, len(cands))
    if cands:
        mv, bl, slot = cands[0]
        check("scanner movz va", mv == 0x1000, hex(mv))
        check("scanner slot va", slot == 0x2040, hex(slot))


# --- synthetic ELF for the new reloc / type-index resolver ------------------

R_REL = tio.R_AARCH64_RELATIVE


def build_synth_elf():
    MR = 0x100          # metadata registration struct
    TYPES_VA = 0x200    # types array
    TYPES_COUNT = 4
    TYPE0 = 0x300       # Il2CppType structs, 16 bytes each
    REL = 0x400         # .rela.dyn
    STRT = 0x500        # shstrtab
    SHOFF = 0x600       # section headers
    TOTAL = 0x700

    buf = bytearray(TOTAL)

    # ELF header
    buf[0:4] = b"\x7fELF"
    buf[4] = 2   # ELFCLASS64
    buf[5] = 1   # little-endian
    buf[6] = 1   # version
    struct.pack_into("<H", buf, 0x10, 3)       # e_type = ET_DYN
    struct.pack_into("<H", buf, 0x12, 0xB7)    # e_machine = aarch64
    struct.pack_into("<I", buf, 0x14, 1)       # e_version
    struct.pack_into("<Q", buf, 0x20, 0x40)    # e_phoff
    struct.pack_into("<Q", buf, 0x28, SHOFF)   # e_shoff
    struct.pack_into("<H", buf, 0x34, 64)      # e_ehsize
    struct.pack_into("<H", buf, 0x36, 56)      # e_phentsize
    struct.pack_into("<H", buf, 0x38, 1)       # e_phnum
    struct.pack_into("<H", buf, 0x3A, 64)      # e_shentsize
    struct.pack_into("<H", buf, 0x3C, 3)       # e_shnum
    struct.pack_into("<H", buf, 0x3E, 2)       # e_shstrndx

    # Program header: one PT_LOAD identity-mapping the whole file
    ph = 0x40
    struct.pack_into("<I", buf, ph + 0x00, 1)      # p_type = PT_LOAD
    struct.pack_into("<I", buf, ph + 0x04, 5)      # p_flags = R+X
    struct.pack_into("<Q", buf, ph + 0x08, 0)      # p_offset
    struct.pack_into("<Q", buf, ph + 0x10, 0)      # p_vaddr
    struct.pack_into("<Q", buf, ph + 0x18, 0)      # p_paddr
    struct.pack_into("<Q", buf, ph + 0x20, TOTAL)  # p_filesz
    struct.pack_into("<Q", buf, ph + 0x28, TOTAL)  # p_memsz
    struct.pack_into("<Q", buf, ph + 0x30, 0x1000) # p_align

    # Metadata registration: 8 (count, ptr) pairs. Only types is anchored.
    counts = [2, 1, 1, TYPES_COUNT, 1, 1, 1, 0]
    for idx, c in enumerate(counts):
        struct.pack_into("<I", buf, MR + idx * 16, c)
    # types ptr field at MR + 3*16 + 8 -> RELATIVE reloc addend TYPES_VA (below)

    # Il2CppType structs: (data u64 [low32=klassIndex], bitfield u32 [type<<16])
    def put_type(i, klass_index, type_enum):
        base = TYPE0 + i * 16
        struct.pack_into("<Q", buf, base, klass_index & 0xFFFFFFFF)
        struct.pack_into("<I", buf, base + 8, (type_enum & 0xFF) << 16)
    put_type(0, 1000, tio.IL2CPP_TYPE_CLASS)      # unrelated class
    put_type(1, 8251, tio.IL2CPP_TYPE_CLASS)      # PlayerManager -> match
    put_type(2, 8251, 0x1E)                        # MVAR w/ same idx -> ignored
    put_type(3, 7923, tio.IL2CPP_TYPE_VALUETYPE)  # PlayerVitals -> match

    # .rela.dyn: types ptr + 4 type-array slots
    relocs = [(MR + 3 * 16 + 8, TYPES_VA)]
    for i in range(TYPES_COUNT):
        relocs.append((TYPES_VA + i * 8, TYPE0 + i * 16))
    for k, (r_off, r_add) in enumerate(relocs):
        b = REL + k * 24
        struct.pack_into("<Q", buf, b + 0, r_off)
        struct.pack_into("<Q", buf, b + 8, R_REL)   # r_info: RELATIVE
        struct.pack_into("<q", buf, b + 16, r_add)
    rela_size = len(relocs) * 24

    # shstrtab
    shstr = b"\x00.rela.dyn\x00.shstrtab\x00"
    buf[STRT:STRT + len(shstr)] = shstr
    name_rela = shstr.index(b".rela.dyn")
    name_str = shstr.index(b".shstrtab")

    # section headers: [0]=null [1]=.rela.dyn(SHT_RELA) [2]=.shstrtab
    def put_sh(i, name_off, stype, off, size, entsize):
        b = SHOFF + i * 64
        struct.pack_into("<I", buf, b + 0x00, name_off)
        struct.pack_into("<I", buf, b + 0x04, stype)
        struct.pack_into("<Q", buf, b + 0x18, off)
        struct.pack_into("<Q", buf, b + 0x20, size)
        struct.pack_into("<Q", buf, b + 0x38, entsize)
    put_sh(0, 0, 0, 0, 0, 0)
    put_sh(1, name_rela, 4, REL, rela_size, 24)   # SHT_RELA
    put_sh(2, name_str, 3, STRT, len(shstr), 0)   # SHT_STRTAB

    return bytes(buf), TYPES_VA, TYPES_COUNT


def test_reloc_parse():
    data, types_va, _ = build_synth_elf()
    img = tio.Image(data)
    check("reloc count == 5", len(img.off2addend) == 5, len(img.off2addend))
    check("types ptr resolves", img.ptr_at(0x100 + 3 * 16 + 8) == types_va,
          hex(img.ptr_at(0x100 + 3 * 16 + 8) or 0))
    check("va_to_off identity", img.va_to_off(0x300) == 0x300)


def test_locate_metadata_registration():
    data, types_va, types_count = build_synth_elf()
    img = tio.Image(data)
    mr = tio.locate_metadata_registration(img, types_va, types_count)
    check("metareg found", mr is not None)
    if mr:
        check("metareg base", mr["__base__"][0] == 0x100, hex(mr["__base__"][0]))
        check("types count", mr["types"][0] == types_count, mr["types"][0])
        check("metadataUsages empty", mr["metadataUsages"][0] == 0, mr["metadataUsages"][0])


def test_find_type_indices():
    data, types_va, types_count = build_synth_elf()
    img = tio.Image(data)
    found = tio.find_type_indices(img, types_va, types_count, {8251: "PM", 7923: "PV"})
    check("8251 one match (MVAR excluded)", len(found[8251]) == 1, found[8251])
    check("8251 type index == 1", found[8251] and found[8251][0][0] == 1, found[8251])
    check("7923 valuetype index == 3", found[7923] and found[7923][0][0] == 3, found[7923])


def test_end_to_end_file():
    data, types_va, types_count = build_synth_elf()
    with tempfile.NamedTemporaryFile(suffix=".so", delete=False) as f:
        f.write(data)
        path = f.name
    try:
        d2, _ = tio.parse_elf(path)
        check("parse_elf reads synth ELF", d2 == data)
    finally:
        os.unlink(path)


def main():
    tests = [
        test_movz_encoding, test_bl_roundtrip, test_adrp_decode,
        test_str_uimm_decode, test_add_imm_decode, test_scanner_synthetic,
        test_reloc_parse, test_locate_metadata_registration,
        test_find_type_indices, test_end_to_end_file,
    ]
    for t in tests:
        print(f"[{t.__name__}]")
        t()
    print(f"\n{_passed} passed, {_failed} failed")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
