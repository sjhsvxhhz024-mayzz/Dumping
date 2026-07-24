#!/usr/bin/env python3
"""
typeinfo_offline.py — resolve Il2CppClass* TypeInfo for target classes STATICALLY
from libil2cpp.so, WITHOUT any runtime log, and VERIFY each result against
global-metadata.dat so the output is trustworthy (not "picked first, hope").

Why the old approach died
=========================

v1 scanned .text for ``MOVZ W0, #<TypeDefIndex>`` + BL + ADRP/STR, assuming
codegen embeds the TDI as an immediate. Empirically FALSE on this build
(il2cpp metadata v39, Unity 2021+/il2cpp v27+):

  * ``MOVZ Wd, #8251`` etc. -> 0 hits in .text for any dest register.
  * The global ``Il2CppMetadataRegistration.metadataUsages`` array is EMPTY
    (count 0) — v27+ removed it, so there is no global usage table to walk.
  * The TDIs only appear as raw u32 in .rodata (metadata type tables).

The approach that actually works
================================

``Il2CppMetadataRegistration.types`` is a live array of ``Il2CppType*`` (this
build: RVA 0xB63EB48, 104482 entries). Every class/valuetype ``Il2CppType`` has
``data.__klassIndex == TypeDefinitionIndex`` and ``type == IL2CPP_TYPE_CLASS``
(0x12) or ``IL2CPP_TYPE_VALUETYPE`` (0x11). For a known TDI we find its *type
index* into that array offline; the mod then resolves the class at load with a
single il2cpp call — no name scan, no in-game log:

    Il2CppType** g_types = (Il2CppType**)((uint8_t*)base + TYPES_RVA);
    Il2CppClass* k = il2cpp_class_from_il2cpp_type(g_types[TYPE_INDEX]);

Pointers live as R_AARCH64_RELATIVE relocations (this is a PIE .so), so the file
stores 0 and .rela.dyn carries the target VA in the addend; we resolve them.

The 100% part: verification
===========================

Several Il2CppType entries can share a klassIndex (byref/pinned variants). We
pick the CANONICAL one (byref=0, pinned=0) and, when global-metadata.dat is
provided, resolve klassIndex -> Il2CppTypeDefinition.name and assert it equals
the expected class name. A class is only emitted if it VERIFIES; a mismatch is
a hard error (exit 3). No guessing.

Usage
=====

    python3 Nov/typeinfo_offline.py libil2cpp.so --metadata global-metadata.dat
    python3 Nov/typeinfo_offline.py libil2cpp.so \\
        --metadata global-metadata.dat \\
        --tdi CAMERA_TYPEINFO:13698:UnityEngine.Camera
    python3 Nov/typeinfo_offline.py libil2cpp.so --legacy   # old .text scan

Metadata table constants (string table, typeDefinitions) are overridable for
other builds via --meta-str / --meta-td-off / --meta-td-count / --meta-td-rec.

Dependencies: pure Python 3 stdlib. No capstone, lief, or pyelftools.
"""

import argparse
import struct
import sys
from typing import Dict, List, Optional, Tuple

ELF_MAGIC = b"\x7fELF"
R_AARCH64_RELATIVE = 1027
IL2CPP_TYPE_VALUETYPE = 0x11
IL2CPP_TYPE_CLASS = 0x12

# Default anchor for the metadataRegistration->types array (from the dumper).
DEFAULT_TYPES_VA = 0xB63EB48
DEFAULT_TYPES_COUNT = 104482

# global-metadata.dat table constants for this build (validated by the dumper
# that produced the full, name-correct dump.cs). Overridable via CLI.
META_MAGIC = 0xFAB11BAF
DEFAULT_META_STR = 0x000DC0EC       # "string" table (identifiers)
DEFAULT_META_TD_OFF = 0x01512204    # typeDefinitions table
DEFAULT_META_TD_COUNT = 29366
DEFAULT_META_TD_REC = 82            # Il2CppTypeDefinition record size (this build)
# field offsets inside an Il2CppTypeDefinition record
MD_NAME = 0x00       # u32 nameIndex   -> string table
MD_NAMESPACE = 0x04  # u32 namespaceIndex -> string table
MD_DECLTYPE = 0x0C   # i32 declaringTypeIndex -> metadataReg.types index (or <0)


# --- Minimal ELF64 parsing (little-endian, aarch64) --------------------------

def parse_elf(path: str) -> Tuple[bytes, Dict[str, dict]]:
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != ELF_MAGIC:
        raise SystemExit(f"[!] not an ELF file: {path}")
    if data[4] != 2:
        raise SystemExit("[!] not ELF64")
    if data[5] != 1:
        raise SystemExit("[!] not little-endian")
    e_shoff     = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum     = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx  = struct.unpack_from("<H", data, 0x3E)[0]

    def sect(idx: int):
        b = e_shoff + idx * e_shentsize
        return {
            "name_off": struct.unpack_from("<I", data, b + 0x00)[0],
            "type":     struct.unpack_from("<I", data, b + 0x04)[0],
            "flags":    struct.unpack_from("<Q", data, b + 0x08)[0],
            "addr":     struct.unpack_from("<Q", data, b + 0x10)[0],
            "off":      struct.unpack_from("<Q", data, b + 0x18)[0],
            "size":     struct.unpack_from("<Q", data, b + 0x20)[0],
            "entsize":  struct.unpack_from("<Q", data, b + 0x38)[0],
        }

    shstr_hdr = sect(e_shstrndx)
    shstr = data[shstr_hdr["off"]: shstr_hdr["off"] + shstr_hdr["size"]]

    def sname(off: int) -> str:
        end = shstr.find(b"\x00", off)
        return shstr[off:end].decode("ascii", "replace")

    sections: Dict[str, dict] = {}
    for i in range(e_shnum):
        s = sect(i)
        sections[sname(s["name_off"])] = s
    return data, sections


# --- Program headers + relocations (for PIE pointer resolution) --------------

class Image:
    """Wraps the mapped ELF: VA<->offset and RELATIVE reloc resolution."""

    def __init__(self, data: bytes):
        self.data = data
        self.segs: List[Tuple[int, int, int, int]] = []  # (vaddr, off, filesz, memsz)
        self._load_segments()
        self.off2addend: Dict[int, int] = {}
        self.addend2offs: Dict[int, List[int]] = {}
        self._load_relocations()

    def _load_segments(self):
        d = self.data
        e_phoff = struct.unpack_from("<Q", d, 0x20)[0]
        e_phentsize = struct.unpack_from("<H", d, 0x36)[0]
        e_phnum = struct.unpack_from("<H", d, 0x38)[0]
        for i in range(e_phnum):
            b = e_phoff + i * e_phentsize
            if struct.unpack_from("<I", d, b)[0] != 1:  # PT_LOAD
                continue
            p_off = struct.unpack_from("<Q", d, b + 0x08)[0]
            p_vaddr = struct.unpack_from("<Q", d, b + 0x10)[0]
            p_filesz = struct.unpack_from("<Q", d, b + 0x20)[0]
            p_memsz = struct.unpack_from("<Q", d, b + 0x28)[0]
            self.segs.append((p_vaddr, p_off, p_filesz, p_memsz))
        self.segs.sort()

    def _load_relocations(self):
        d = self.data
        e_shoff = struct.unpack_from("<Q", d, 0x28)[0]
        e_shentsize = struct.unpack_from("<H", d, 0x3A)[0]
        e_shnum = struct.unpack_from("<H", d, 0x3C)[0]
        for i in range(e_shnum):
            b = e_shoff + i * e_shentsize
            if struct.unpack_from("<I", d, b + 4)[0] != 4:  # SHT_RELA
                continue
            so = struct.unpack_from("<Q", d, b + 0x18)[0]
            ssz = struct.unpack_from("<Q", d, b + 0x20)[0]
            for k in range(ssz // 24):
                r_off, r_info, r_add = struct.unpack_from("<QQq", d, so + k * 24)
                if (r_info & 0xFFFFFFFF) == R_AARCH64_RELATIVE:
                    self.off2addend[r_off] = r_add
                    self.addend2offs.setdefault(r_add & 0xFFFFFFFFFFFFFFFF, []).append(r_off)

    def va_to_off(self, va: int) -> Optional[int]:
        for v, o, fs, ms in self.segs:
            if v <= va < v + fs:
                return o + (va - v)
        return None

    def va_mapped(self, va: int) -> bool:
        return any(v <= va < v + ms for v, o, fs, ms in self.segs)

    def ptr_at(self, va: int) -> Optional[int]:
        """Value of a pointer field at VA, resolving a RELATIVE reloc if present."""
        if va in self.off2addend:
            return self.off2addend[va]
        off = self.va_to_off(va)
        if off is None:
            return None
        raw = struct.unpack_from("<Q", self.data, off)[0]
        return raw if raw else None

    def u32_at(self, va: int) -> Optional[int]:
        off = self.va_to_off(va)
        if off is None:
            return None
        return struct.unpack_from("<I", self.data, off)[0]


# --- ARM64 (dis)assembly helpers (kept for --legacy scan + tests) ------------

def enc_movz_w0(imm16: int) -> bytes:
    op = 0x52800000 | ((imm16 & 0xFFFF) << 5)
    return struct.pack("<I", op)


def dec_bl(word: int) -> Optional[int]:
    if (word & 0xFC000000) != 0x94000000:
        return None
    imm26 = word & 0x03FFFFFF
    if imm26 & (1 << 25):
        imm26 -= 1 << 26
    return imm26 * 4


def dec_adrp(word: int) -> Optional[Tuple[int, int]]:
    if (word & 0x9F000000) != 0x90000000:
        return None
    rd = word & 0x1F
    immlo = (word >> 29) & 0x3
    immhi = (word >> 5) & 0x7FFFF
    imm21 = (immhi << 2) | immlo
    if imm21 & (1 << 20):
        imm21 -= 1 << 21
    return rd, imm21 << 12


def dec_add_imm64(word: int) -> Optional[Tuple[int, int, int]]:
    if (word & 0xFF800000) != 0x91000000:
        return None
    sh = (word >> 22) & 1
    imm12 = (word >> 10) & 0xFFF
    if sh:
        imm12 <<= 12
    rn = (word >> 5) & 0x1F
    rd = word & 0x1F
    return rd, rn, imm12


def dec_str_uimm64(word: int) -> Optional[Tuple[int, int, int]]:
    if (word & 0xFFC00000) != 0xF9000000:
        return None
    imm12 = (word >> 10) & 0xFFF
    rn = (word >> 5) & 0x1F
    rt = word & 0x1F
    return rt, rn, imm12 * 8


Candidate = Tuple[int, int, int]  # (movz_va, bl_target_va, slot_va)


def find_slots_for_tdi(text: bytes, text_va: int, tdi: int,
                       max_hits: int = 32) -> List[Candidate]:
    """Legacy .text scan (kept for --legacy and regression tests)."""
    pattern = enc_movz_w0(tdi & 0xFFFF)
    results: List[Candidate] = []
    pos = 0
    while len(results) < max_hits:
        j = text.find(pattern, pos)
        if j < 0:
            break
        pos = j + 1
        if j % 4 != 0:
            continue
        bl_target = None
        bl_idx = None
        for k in range(1, 6):
            off = j + k * 4
            if off + 4 > len(text):
                break
            w = struct.unpack_from("<I", text, off)[0]
            rel = dec_bl(w)
            if rel is not None:
                bl_idx = k
                bl_target = text_va + off + rel
                break
        if bl_idx is None:
            continue
        adrp_page: Optional[int] = None
        adrp_rd: Optional[int] = None
        slot_va: Optional[int] = None
        for m in range(bl_idx + 1, bl_idx + 12):
            off = j + m * 4
            if off + 4 > len(text):
                break
            w = struct.unpack_from("<I", text, off)[0]
            adrp = dec_adrp(w)
            if adrp is not None:
                adrp_rd, page_off = adrp
                pc = text_va + off
                adrp_page = (pc & ~0xFFF) + page_off
                continue
            st = dec_str_uimm64(w)
            if st is not None and adrp_page is not None:
                rt, rn, imm = st
                if rn == adrp_rd and rt == 0:
                    slot_va = adrp_page + imm
                    break
            ad = dec_add_imm64(w)
            if ad is not None and adrp_page is not None:
                rd, rn, imm = ad
                if rn == adrp_rd:
                    slot_va = adrp_page + imm
                    adrp_rd = rd
        if slot_va is not None:
            results.append((text_va + j, bl_target, slot_va))
    return results


# --- Metadata registration + type index resolution --------------------------

METAREG_FIELDS = [
    "genericClasses", "genericInsts", "genericMethodTable", "types",
    "methodSpecs", "fieldOffsets", "typeDefinitionsSizes", "metadataUsages",
]


def locate_metadata_registration(img: Image, types_va: int,
                                  types_count: int) -> Optional[dict]:
    """Locate Il2CppMetadataRegistration by the types-array anchor.

    Returns dict {field: (count, ptr_va)} or None if the anchor is not found.
    """
    offs = img.addend2offs.get(types_va, [])
    for types_field_va in offs:
        base = types_field_va - (3 * 16 + 8)  # types is pair index 3
        fields: Dict[str, Tuple[int, Optional[int]]] = {}
        for idx, nm in enumerate(METAREG_FIELDS):
            cnt = img.u32_at(base + idx * 16)
            ptr = img.ptr_at(base + idx * 16 + 8)
            fields[nm] = (cnt if cnt is not None else -1, ptr)
        if fields["types"][0] == types_count and fields["types"][1] == types_va:
            fields["__base__"] = (base, None)
            return fields
    return None


class TypeMatch:
    __slots__ = ("index", "il2cpptype_va", "type_enum", "byref", "pinned")

    def __init__(self, index, va, type_enum, byref, pinned):
        self.index = index
        self.il2cpptype_va = va
        self.type_enum = type_enum
        self.byref = byref
        self.pinned = pinned

    @property
    def canonical(self) -> bool:
        return self.byref == 0 and self.pinned == 0


def find_type_indices(img: Image, types_va: int, types_count: int,
                      tdi_targets: Dict[int, str]) -> Dict[int, List[TypeMatch]]:
    """For each target TDI, return TypeMatch list (canonical entries first).

    Only IL2CPP_TYPE_CLASS / IL2CPP_TYPE_VALUETYPE entries are considered.
    """
    found: Dict[int, List[TypeMatch]] = {t: [] for t in tdi_targets}
    for i in range(types_count):
        slot = types_va + i * 8
        tptr = img.ptr_at(slot)
        if tptr is None:
            continue
        to = img.va_to_off(tptr)
        if to is None or to + 12 > len(img.data):
            continue
        dat = struct.unpack_from("<Q", img.data, to)[0]
        bf = struct.unpack_from("<I", img.data, to + 8)[0]
        typ = (bf >> 16) & 0xFF
        if typ in (IL2CPP_TYPE_CLASS, IL2CPP_TYPE_VALUETYPE):
            ki = dat & 0xFFFFFFFF
            if ki in found:
                byref = (bf >> 30) & 1
                pinned = (bf >> 31) & 1
                found[ki].append(TypeMatch(i, tptr, typ, byref, pinned))
    # canonical (byref=0, pinned=0) first, then by index
    for ki in found:
        found[ki].sort(key=lambda m: (0 if m.canonical else 1, m.index))
    return found


# --- global-metadata.dat name verification -----------------------------------

class Metadata:
    """Reads Il2CppTypeDefinition names from (already-decrypted) metadata."""

    def __init__(self, data: bytes, str_off: int, td_off: int,
                 td_count: int, td_rec: int):
        self.data = data
        self.str_off = str_off
        self.td_off = td_off
        self.td_count = td_count
        self.td_rec = td_rec
        magic = struct.unpack_from("<I", data, 0)[0]
        if magic != META_MAGIC:
            raise SystemExit(f"[!] bad metadata magic 0x{magic:X} (expected 0x{META_MAGIC:X})")

    def cstr(self, rel: int) -> str:
        if rel < 0:
            return ""
        o = self.str_off + rel
        e = self.data.find(b"\x00", o)
        return self.data[o:e].decode("utf-8", "replace")

    def _td(self, i: int) -> int:
        return self.td_off + i * self.td_rec

    def name_index(self, i: int) -> int:
        return struct.unpack_from("<I", self.data, self._td(i) + MD_NAME)[0]

    def ns_index(self, i: int) -> int:
        return struct.unpack_from("<I", self.data, self._td(i) + MD_NAMESPACE)[0]

    def decl_type_index(self, i: int) -> int:
        return struct.unpack_from("<i", self.data, self._td(i) + MD_DECLTYPE)[0]


def _typedef_of_typeindex(img: Image, types_va: int, types_count: int,
                          tidx: int) -> int:
    if tidx < 0 or tidx >= types_count:
        return -1
    va = img.ptr_at(types_va + tidx * 8)
    if va is None:
        return -1
    fo = img.va_to_off(va)
    if fo is None:
        return -1
    return struct.unpack_from("<Q", img.data, fo)[0] & 0xFFFFFFFF


def class_full_name(meta: Metadata, img: Image, types_va: int, types_count: int,
                    klass_index: int) -> str:
    """Reconstruct Namespace.Outer.Name for a TypeDefinitionIndex.

    Walks declaringType (a metadataReg.types index) to prefix nested names,
    matching the dumper's proven logic; strips generic-arity backticks.
    """
    if klass_index < 0 or klass_index >= meta.td_count:
        return "?"
    parts = [meta.cstr(meta.name_index(klass_index))]
    d = meta.decl_type_index(klass_index)
    top = klass_index
    guard = 0
    while d >= 0 and guard < 12:
        di = _typedef_of_typeindex(img, types_va, types_count, d)
        if di < 0 or di >= meta.td_count:
            break
        parts.append(meta.cstr(meta.name_index(di)))
        top = di
        d = meta.decl_type_index(di)
        guard += 1
    ns = meta.cstr(meta.ns_index(top))
    name = ".".join(reversed(parts))
    name = ".".join(seg.split("`")[0] for seg in name.split("."))
    return (ns + "." + name) if ns else name


# --- Targets -----------------------------------------------------------------

KNOWN_DEFAULTS = [
    ("PLAYERMANAGER_TYPEINFO", 8251, "Oxide.PlayerManager"),
    ("BUILDINGPIECE_TYPEINFO", 8521, "Oxide.Building.BuildingPiece"),
    ("PLAYERVITALS_TYPEINFO",  7923, "Oxide.PlayerVitals"),
]


def parse_targets(extra: List[str]) -> List[Tuple[str, int, str]]:
    targets = list(KNOWN_DEFAULTS)
    for spec in extra:
        parts = spec.split(":")
        if len(parts) < 2:
            print(f"[!] bad --tdi spec: {spec}", file=sys.stderr)
            continue
        name = parts[0].strip()
        tdi = int(parts[1], 0)
        cname = parts[2] if len(parts) > 2 else ""
        targets.append((name, tdi, cname))
    return targets


# --- CLI ---------------------------------------------------------------------

def run_legacy(path: str, targets) -> int:
    data, sections = parse_elf(path)
    if ".text" not in sections:
        raise SystemExit("[!] no .text section")
    ts = sections[".text"]
    text = data[ts["off"]: ts["off"] + ts["size"]]
    text_va = ts["addr"]
    print(f"[legacy] .text addr=0x{text_va:X} size=0x{ts['size']:X}", file=sys.stderr)
    any_hit = False
    for name, tdi, cname in targets:
        cands = find_slots_for_tdi(text, text_va, tdi)
        if cands:
            any_hit = True
        print(f"// {cname or name} (TDI {tdi}): {len(cands)} candidate(s)")
        for mv, bl, sv in cands:
            print(f"//   MOVZ=0x{mv:X} BL=0x{bl:X} slot=0x{sv:X}")
    if not any_hit:
        print("// legacy scan found nothing (expected on il2cpp v27+ builds)")
    return 0


def run_typeindex(path: str, targets, types_va: int, types_count: int,
                  meta: Optional[Metadata]) -> int:
    data, _ = parse_elf(path)
    img = Image(data)

    metareg = locate_metadata_registration(img, types_va, types_count)
    if metareg is None:
        print(f"[!] could not confirm metadataRegistration at types anchor "
              f"0x{types_va:X}/{types_count}. Pass correct --types-va/--types-count.",
              file=sys.stderr)
    else:
        base = metareg["__base__"][0]
        print(f"[+] Il2CppMetadataRegistration @0x{base:X}", file=sys.stderr)
        for nm in METAREG_FIELDS:
            c, p = metareg[nm]
            print(f"      {nm:22s} count={c:<10d} ptr={('0x%X' % p) if p else 'null'}",
                  file=sys.stderr)

    tdi_targets = {tdi: (cname or name) for name, tdi, cname in targets}
    found = find_type_indices(img, types_va, types_count, tdi_targets)

    print()
    print("// ---------------------------------------------------------------")
    print("// oxide_offsets.h  \u2014  TypeInfo resolution (offline, static, verified)")
    print("// Generated by Nov/typeinfo_offline.py (type-index method)")
    print("// ---------------------------------------------------------------")
    print(f"static constexpr uint64_t IL2CPP_TYPES_RVA = 0x{types_va:X}; "
          f"// metadataRegistration->types ({types_count} entries)")
    print("//")
    print("// Resolve at load (once), no runtime log / no name scan:")
    print("//   Il2CppType** g = (Il2CppType**)((uint8_t*)il2cpp_base + IL2CPP_TYPES_RVA);")
    print("//   Il2CppClass* k = il2cpp_class_from_il2cpp_type(g[<TYPE_INDEX>]);")
    if meta is None:
        print("// NOTE: run with --metadata global-metadata.dat to VERIFY each name.")

    rc = 0
    for name, tdi, expected in targets:
        cands = found.get(tdi, [])
        label = expected or name
        print(f"\n// --- {label} (TypeDefIndex {tdi}) ---")
        if not cands:
            print(f"//   UNRESOLVED: no IL2CPP_TYPE_CLASS/VALUETYPE with klassIndex={tdi}")
            print(f"//   (fallback: runtime ox_scanClassByName)")
            rc = 2
            continue
        pick = cands[0]
        kind = "CLASS" if pick.type_enum == IL2CPP_TYPE_CLASS else "VALUETYPE"
        canon = "canonical" if pick.canonical else f"byref={pick.byref} pinned={pick.pinned}"
        print(f"//   {len(cands)} matching Il2CppType(s); picked idx {pick.index} ({kind}, {canon})")
        print(f"//   Il2CppType @0x{pick.il2cpptype_va:X}")

        if meta is not None:
            got = class_full_name(meta, img, types_va, types_count, tdi)
            if expected and got != expected:
                print(f"//   VERIFY FAIL: metadata name '{got}' != expected '{expected}'")
                rc = 3
                continue
            tag = f"verified '{got}'" if expected else f"metadata name '{got}'"
            print(f"//   VERIFY OK: {tag}")

        print(f"static constexpr int32_t  {name}_TYPEIDX = {pick.index}; // {label}")
        if len(cands) > 1:
            alt = ", ".join(str(c.index) for c in cands[1:])
            print(f"//   equivalent indices (same class): {alt}")
    print()
    if rc == 3:
        print("[!] one or more classes FAILED verification", file=sys.stderr)
    return rc


def load_metadata(args) -> Optional[Metadata]:
    if not args.metadata:
        return None
    with open(args.metadata, "rb") as f:
        data = f.read()
    return Metadata(data, args.meta_str, args.meta_td_off,
                    args.meta_td_count, args.meta_td_rec)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Offline + verified TypeInfo resolver for il2cpp (v27+/v39).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("libil2cpp", help="path to libil2cpp.so")
    ap.add_argument("--metadata", help="path to (decrypted) global-metadata.dat for name verification")
    ap.add_argument("--tdi", action="append", default=[],
                    help="extra target NAME:TDI[:Expected.Full.Name] (repeatable)")
    ap.add_argument("--types-va", type=lambda x: int(x, 0), default=DEFAULT_TYPES_VA,
                    help="VA of metadataRegistration->types (default 0x%X)" % DEFAULT_TYPES_VA)
    ap.add_argument("--types-count", type=lambda x: int(x, 0), default=DEFAULT_TYPES_COUNT,
                    help="entry count of types array (default %d)" % DEFAULT_TYPES_COUNT)
    ap.add_argument("--meta-str", type=lambda x: int(x, 0), default=DEFAULT_META_STR,
                    help="metadata string table offset (default 0x%X)" % DEFAULT_META_STR)
    ap.add_argument("--meta-td-off", type=lambda x: int(x, 0), default=DEFAULT_META_TD_OFF,
                    help="metadata typeDefinitions offset (default 0x%X)" % DEFAULT_META_TD_OFF)
    ap.add_argument("--meta-td-count", type=lambda x: int(x, 0), default=DEFAULT_META_TD_COUNT,
                    help="typeDefinitions count (default %d)" % DEFAULT_META_TD_COUNT)
    ap.add_argument("--meta-td-rec", type=lambda x: int(x, 0), default=DEFAULT_META_TD_REC,
                    help="Il2CppTypeDefinition record size (default %d)" % DEFAULT_META_TD_REC)
    ap.add_argument("--legacy", action="store_true",
                    help="run the old .text MOVZ/BL/ADRP/STR scan instead")
    args = ap.parse_args()

    targets = parse_targets(args.tdi)
    if args.legacy:
        return run_legacy(args.libil2cpp, targets)
    meta = load_metadata(args)
    return run_typeindex(args.libil2cpp, targets, args.types_va, args.types_count, meta)


if __name__ == "__main__":
    sys.exit(main())
