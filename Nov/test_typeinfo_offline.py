#!/usr/bin/env python3
"""Self-tests for typeinfo_offline.py.

Covers the legacy ARM64 (dis)assembly scan, the reloc / type-index resolver,
canonical (byref/pinned) selection, and metadata-based name verification — all
on hand-built synthetic ELF + metadata blobs. Pure stdlib, no external binary.

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
    rd, rn, imm = tio.dec_add_imm64(0x91004020)  # ADD x0,x1,#0x10
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


# --- synthetic ELF helpers ---------------------------------------------------

R_REL = tio.R_AARCH64_RELATIVE


def _elf_header(buf, shoff, shnum, shstrndx, total):
    buf[0:4] = b"\x7fELF"
    buf[4] = 2   # ELFCLASS64
    buf[5] = 1   # little-endian
    buf[6] = 1
    struct.pack_into("<H", buf, 0x10, 3)      # ET_DYN
    struct.pack_into("<H", buf, 0x12, 0xB7)   # aarch64
    struct.pack_into("<I", buf, 0x14, 1)
    struct.pack_into("<Q", buf, 0x20, 0x40)   # e_phoff
    struct.pack_into("<Q", buf, 0x28, shoff)  # e_shoff
    struct.pack_into("<H", buf, 0x34, 64)
    struct.pack_into("<H", buf, 0x36, 56)     # e_phentsize
    struct.pack_into("<H", buf, 0x38, 1)      # e_phnum
    struct.pack_into("<H", buf, 0x3A, 64)     # e_shentsize
    struct.pack_into("<H", buf, 0x3C, shnum)
    struct.pack_into("<H", buf, 0x3E, shstrndx)
    ph = 0x40
    struct.pack_into("<I", buf, ph + 0x00, 1)      # PT_LOAD
    struct.pack_into("<I", buf, ph + 0x04, 5)      # R+X
    struct.pack_into("<Q", buf, ph + 0x20, total)  # p_filesz
    struct.pack_into("<Q", buf, ph + 0x28, total)  # p_memsz
    struct.pack_into("<Q", buf, ph + 0x30, 0x1000) # p_align


def _sections(buf, shoff, rel_off, rel_size, strt_off):
    shstr = b"\x00.rela.dyn\x00.shstrtab\x00"
    buf[strt_off:strt_off + len(shstr)] = shstr
    name_rela = shstr.index(b".rela.dyn")
    name_str = shstr.index(b".shstrtab")

    def put_sh(i, name_off, stype, off, size, entsize):
        b = shoff + i * 64
        struct.pack_into("<I", buf, b + 0x00, name_off)
        struct.pack_into("<I", buf, b + 0x04, stype)
        struct.pack_into("<Q", buf, b + 0x18, off)
        struct.pack_into("<Q", buf, b + 0x20, size)
        struct.pack_into("<Q", buf, b + 0x38, entsize)
    put_sh(0, 0, 0, 0, 0, 0)
    put_sh(1, name_rela, 4, rel_off, rel_size, 24)  # SHT_RELA
    put_sh(2, name_str, 3, strt_off, len(shstr), 0)  # SHT_STRTAB


def build_synth_elf():
    """types with klassIndex 8251 (+MVAR dup) and 7923 (valuetype)."""
    MR, TYPES_VA, TYPES_COUNT = 0x100, 0x200, 4
    TYPE0, REL, STRT, SHOFF, TOTAL = 0x300, 0x400, 0x500, 0x600, 0x700
    buf = bytearray(TOTAL)
    _elf_header(buf, SHOFF, 3, 2, TOTAL)

    counts = [2, 1, 1, TYPES_COUNT, 1, 1, 1, 0]
    for idx, c in enumerate(counts):
        struct.pack_into("<I", buf, MR + idx * 16, c)

    def put_type(i, klass_index, type_enum):
        base = TYPE0 + i * 16
        struct.pack_into("<Q", buf, base, klass_index & 0xFFFFFFFF)
        struct.pack_into("<I", buf, base + 8, (type_enum & 0xFF) << 16)
    put_type(0, 1000, tio.IL2CPP_TYPE_CLASS)
    put_type(1, 8251, tio.IL2CPP_TYPE_CLASS)      # match
    put_type(2, 8251, 0x1E)                        # MVAR -> ignored
    put_type(3, 7923, tio.IL2CPP_TYPE_VALUETYPE)  # match

    relocs = [(MR + 3 * 16 + 8, TYPES_VA)]
    for i in range(TYPES_COUNT):
        relocs.append((TYPES_VA + i * 8, TYPE0 + i * 16))
    for k, (r_off, r_add) in enumerate(relocs):
        b = REL + k * 24
        struct.pack_into("<Q", buf, b + 0, r_off)
        struct.pack_into("<Q", buf, b + 8, R_REL)
        struct.pack_into("<q", buf, b + 16, r_add)
    _sections(buf, SHOFF, REL, len(relocs) * 24, STRT)
    return bytes(buf), TYPES_VA, TYPES_COUNT


def build_synth_elf2():
    """6 types exercising byref/pinned variants + nested-decl resolution.

    idx0 klass2 CLASS canonical | idx1 klass2 CLASS byref | idx2 klass5 VT canonical
    idx3 klass99 CLASS          | idx4 klass2 CLASS pinned | idx5 klass5 VT byref
    """
    MR, TYPES_VA, TYPES_COUNT = 0x100, 0x200, 6
    TYPE0, REL, STRT, SHOFF, TOTAL = 0x300, 0x400, 0x600, 0x700, 0x800
    buf = bytearray(TOTAL)
    _elf_header(buf, SHOFF, 3, 2, TOTAL)

    counts = [1, 1, 1, TYPES_COUNT, 1, 1, 1, 0]
    for idx, c in enumerate(counts):
        struct.pack_into("<I", buf, MR + idx * 16, c)

    def put_type(i, ki, type_enum, byref=0, pinned=0):
        base = TYPE0 + i * 16
        struct.pack_into("<Q", buf, base, ki & 0xFFFFFFFF)
        bf = ((type_enum & 0xFF) << 16) | ((byref & 1) << 30) | ((pinned & 1) << 31)
        struct.pack_into("<I", buf, base + 8, bf)
    put_type(0, 2, tio.IL2CPP_TYPE_CLASS)
    put_type(1, 2, tio.IL2CPP_TYPE_CLASS, byref=1)
    put_type(2, 5, tio.IL2CPP_TYPE_VALUETYPE)
    put_type(3, 99, tio.IL2CPP_TYPE_CLASS)
    put_type(4, 2, tio.IL2CPP_TYPE_CLASS, pinned=1)
    put_type(5, 5, tio.IL2CPP_TYPE_VALUETYPE, byref=1)

    relocs = [(MR + 3 * 16 + 8, TYPES_VA)]
    for i in range(TYPES_COUNT):
        relocs.append((TYPES_VA + i * 8, TYPE0 + i * 16))
    for k, (r_off, r_add) in enumerate(relocs):
        b = REL + k * 24
        struct.pack_into("<Q", buf, b + 0, r_off)
        struct.pack_into("<Q", buf, b + 8, R_REL)
        struct.pack_into("<q", buf, b + 16, r_add)
    _sections(buf, SHOFF, REL, len(relocs) * 24, STRT)
    return bytes(buf), TYPES_VA, TYPES_COUNT


def build_synth_metadata():
    """Metadata with klass2='Game.PlayerX', klass5='Vitals',
    klass3='Inner' nested under type-index 0 (=klass2) -> 'Game.PlayerX.Inner'."""
    STR_OFF, TD_OFF, TD_COUNT, TD_REC = 0x40, 0x100, 6, 0x10
    total = 0x200
    buf = bytearray(total)
    struct.pack_into("<I", buf, 0, tio.META_MAGIC)
    struct.pack_into("<i", buf, 4, 39)

    # string table: leading NUL so rel 0 == empty string
    blob = bytearray(b"\x00")
    offs = {"": 0}
    for s in ("PlayerX", "Game", "Inner", "Vitals"):
        offs[s] = len(blob)
        blob += s.encode() + b"\x00"
    buf[STR_OFF:STR_OFF + len(blob)] = blob

    # typeDefinitions: (nameIndex u32 @0, nsIndex u32 @4, declTypeIndex i32 @0xC)
    entries = {
        2: ("PlayerX", "Game", -1),
        3: ("Inner", "", 0),      # decl -> type index 0 (klass2)
        5: ("Vitals", "", -1),
    }
    for i in range(TD_COUNT):
        name, ns, decl = entries.get(i, ("", "", -1))
        b = TD_OFF + i * TD_REC
        struct.pack_into("<I", buf, b + 0x00, offs[name])
        struct.pack_into("<I", buf, b + 0x04, offs[ns])
        struct.pack_into("<i", buf, b + 0x0C, decl)
    return bytes(buf), STR_OFF, TD_OFF, TD_COUNT, TD_REC


# --- reloc / type-index tests -----------------------------------------------

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
    check("8251 one match (MVAR excluded)", len(found[8251]) == 1, len(found[8251]))
    check("8251 type index == 1", found[8251] and found[8251][0].index == 1)
    check("7923 valuetype index == 3", found[7923] and found[7923][0].index == 3)


def test_canonical_selection():
    data, types_va, types_count = build_synth_elf2()
    img = tio.Image(data)
    found = tio.find_type_indices(img, types_va, types_count, {2: "X", 5: "V"})
    check("klass2 has 3 variants", len(found[2]) == 3, len(found[2]))
    check("klass2 canonical picked first (idx0)", found[2][0].index == 0, found[2][0].index)
    check("klass2 first is canonical", found[2][0].canonical)
    check("klass5 canonical picked first (idx2)", found[5][0].index == 2, found[5][0].index)
    check("klass5 byref variant not canonical", not found[5][1].canonical)


def test_metadata_names():
    edata, types_va, types_count = build_synth_elf2()
    img = tio.Image(edata)
    mdata, so, tdo, tdc, tdr = build_synth_metadata()
    meta = tio.Metadata(mdata, so, tdo, tdc, tdr)
    n2 = tio.class_full_name(meta, img, types_va, types_count, 2)
    n5 = tio.class_full_name(meta, img, types_va, types_count, 5)
    n3 = tio.class_full_name(meta, img, types_va, types_count, 3)
    check("klass2 -> Game.PlayerX", n2 == "Game.PlayerX", n2)
    check("klass5 -> Vitals", n5 == "Vitals", n5)
    check("nested klass3 -> Game.PlayerX.Inner", n3 == "Game.PlayerX.Inner", n3)


def test_metadata_magic_guard():
    bad = bytearray(0x200)
    struct.pack_into("<I", bad, 0, 0xDEADBEEF)
    try:
        tio.Metadata(bytes(bad), 0x40, 0x100, 6, 0x10)
        check("bad magic raises", False, "no exception")
    except SystemExit:
        check("bad magic raises", True)


def test_end_to_end_file():
    data, _, _ = build_synth_elf()
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
        test_find_type_indices, test_canonical_selection,
        test_metadata_names, test_metadata_magic_guard, test_end_to_end_file,
    ]
    for t in tests:
        print(f"[{t.__name__}]")
        t()
    print(f"\n{_passed} passed, {_failed} failed")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
