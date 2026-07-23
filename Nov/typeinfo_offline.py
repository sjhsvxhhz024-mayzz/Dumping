#!/usr/bin/env python3
"""
typeinfo_offline.py — resolve Il2CppClass* TypeInfo slot RVA statically from
libil2cpp.so, WITHOUT any runtime log.

Background
==========

In il2cpp v29 builds (like Oxide Survival Island) there is no single global
``s_TypeInfoTable`` indexed by TypeDefIndex — arithmetic on the known slots
proves it (``0xB8C1AE8 - 8251*8`` ≠ ``0xB8B97E0 - 8521*8``). Instead every
generated method that uses class C has its own per-callsite ``Il2CppClass*``
cache slot placed in .bss. Codegen initializes it with the pattern::

    MOVZ W0, #<TypeDefIndex>        ; TDI fits in 16 bits (< 65536)
    BL   GetTypeInfoFromTypeDefinitionIndex
    ADRP Xn, <slot_page>
    STR  X0, [Xn, #<slot_lo12>]     ; write returned Il2CppClass* into slot

``ox_scanClassByName`` finds one such slot at runtime by class name; this
script finds the SAME kind of slot statically by looking for that exact
pattern with a known TDI.

Usage
=====

    python3 Nov/typeinfo_offline.py path/to/libil2cpp.so
    python3 Nov/typeinfo_offline.py libil2cpp.so \\
        --tdi PLAYERMANAGER_TYPEINFO:8251:Oxide.PlayerManager \\
        --tdi CAMERA_TYPEINFO:13698:UnityEngine.Camera

Defaults probe the three classes hard-coded in oxide_offsets.h.

Dependencies: pure Python 3 stdlib.  No capstone, no lief, no pyelftools.
"""

import argparse
import struct
import sys
from typing import Dict, List, Optional, Tuple

# --- Minimal ELF64 parsing (little-endian, aarch64) --------------------------

ELF_MAGIC = b"\x7fELF"


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


# --- ARM64 instruction (dis)assembly helpers ---------------------------------

def enc_movz_w0(imm16: int) -> bytes:
    """Encode 'MOVZ W0, #imm16, LSL #0' (32-bit MOVZ, hw=0)."""
    op = 0x52800000 | ((imm16 & 0xFFFF) << 5)  # Rd=0
    return struct.pack("<I", op)


def dec_bl(word: int) -> Optional[int]:
    if (word & 0xFC000000) != 0x94000000:
        return None
    imm26 = word & 0x03FFFFFF
    if imm26 & (1 << 25):
        imm26 -= 1 << 26  # sign-extend 26-bit
    return imm26 * 4


def dec_adrp(word: int) -> Optional[Tuple[int, int]]:
    if (word & 0x9F000000) != 0x90000000:
        return None
    rd     = word & 0x1F
    immlo  = (word >> 29) & 0x3
    immhi  = (word >> 5)  & 0x7FFFF
    imm21  = (immhi << 2) | immlo
    if imm21 & (1 << 20):
        imm21 -= 1 << 21
    return rd, imm21 << 12  # in bytes, page offset


def dec_add_imm64(word: int) -> Optional[Tuple[int, int, int]]:
    # ADD (immediate) 64-bit: 1001 0001 sh(1) imm12 Rn Rd  (opcode mask 0xFF800000 == 0x91000000)
    if (word & 0xFF800000) != 0x91000000:
        return None
    sh    = (word >> 22) & 1
    imm12 = (word >> 10) & 0xFFF
    if sh:
        imm12 <<= 12
    rn = (word >> 5) & 0x1F
    rd = word & 0x1F
    return rd, rn, imm12


def dec_str_uimm64(word: int) -> Optional[Tuple[int, int, int]]:
    # STR (immediate, 64-bit, unsigned offset): 11 111 001 00 imm12 Rn Rt   mask 0xFFC00000 == 0xF9000000
    if (word & 0xFFC00000) != 0xF9000000:
        return None
    imm12 = (word >> 10) & 0xFFF
    rn = (word >> 5) & 0x1F
    rt = word & 0x1F
    return rt, rn, imm12 * 8


# --- Scanner -----------------------------------------------------------------

Candidate = Tuple[int, int, int]  # (movz_va, bl_target_va, slot_va)


def find_slots_for_tdi(text: bytes,
                       text_va: int,
                       tdi: int,
                       max_hits: int = 32) -> List[Candidate]:
    """Return list of (movz_va, bl_target_va, slot_va) candidates."""
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

        # 1) BL within next 5 instructions
        bl_target = None
        bl_idx    = None
        for k in range(1, 6):
            off = j + k * 4
            if off + 4 > len(text):
                break
            w = struct.unpack_from("<I", text, off)[0]
            rel = dec_bl(w)
            if rel is not None:
                bl_idx    = k
                bl_target = text_va + off + rel
                break
        if bl_idx is None:
            continue

        # 2) ADRP + (ADD | STR) within 10 instructions after BL
        adrp_page: Optional[int] = None
        adrp_rd:   Optional[int] = None
        slot_va:   Optional[int] = None
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
                if rn == adrp_rd and rt == 0:  # STR X0, [Xadrp, #imm]
                    slot_va = adrp_page + imm
                    break

            ad = dec_add_imm64(w)
            if ad is not None and adrp_page is not None:
                rd, rn, imm = ad
                if rn == adrp_rd:
                    slot_va = adrp_page + imm
                    adrp_rd = rd  # follow computed address into a later STR

        if slot_va is not None:
            results.append((text_va + j, bl_target, slot_va))

    return results


# --- CLI ---------------------------------------------------------------------

KNOWN_DEFAULTS = [
    ("PLAYERMANAGER_TYPEINFO", 8251, "Oxide.PlayerManager"),
    ("BUILDINGPIECE_TYPEINFO", 8521, "Oxide.Building.BuildingPiece"),
    ("PLAYERVITALS_TYPEINFO",  7923, "Oxide.PlayerVitals"),
]


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Offline TypeInfo slot RVA resolver for il2cpp v29.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("libil2cpp", help="path to libil2cpp.so")
    ap.add_argument(
        "--tdi", action="append", default=[],
        help="extra target as NAME:TDI[:Full.Type.Name] (repeatable)",
    )
    ap.add_argument("--all-hits", action="store_true",
                    help="list every matching slot, not just the first")
    args = ap.parse_args()

    data, sections = parse_elf(args.libil2cpp)
    if ".text" not in sections:
        raise SystemExit("[!] no .text section in libil2cpp.so")
    text_sec = sections[".text"]
    text = data[text_sec["off"]: text_sec["off"] + text_sec["size"]]
    text_va = text_sec["addr"]
    print(f"[+] .text  addr=0x{text_va:X}  size=0x{text_sec['size']:X} "
          f"({text_sec['size'] / 1024 / 1024:.1f} MiB)", file=sys.stderr)

    targets = list(KNOWN_DEFAULTS)
    for extra in args.tdi:
        parts = extra.split(":")
        if len(parts) < 2:
            print(f"[!] bad --tdi spec: {extra}", file=sys.stderr)
            continue
        name = parts[0].strip()
        tdi  = int(parts[1], 0)
        cname = parts[2] if len(parts) > 2 else "(custom)"
        targets.append((name, tdi, cname))

    # Global tally of BL targets: if the same VA appears across multiple TDIs it
    # is very likely GetTypeInfoFromTypeDefinitionIndex.
    bl_tally: Dict[int, int] = {}
    all_results: List[Tuple[str, int, str, List[Candidate]]] = []
    for name, tdi, cname in targets:
        cands = find_slots_for_tdi(text, text_va, tdi)
        all_results.append((name, tdi, cname, cands))
        for _, bl, _ in cands:
            bl_tally[bl] = bl_tally.get(bl, 0) + 1

    metadata_bl = None
    if bl_tally:
        metadata_bl, metadata_bl_hits = max(bl_tally.items(), key=lambda kv: kv[1])
        print(f"[+] most common BL target: 0x{metadata_bl:X} (seen {metadata_bl_hits}x) "
              f"— likely GetTypeInfoFromTypeDefinitionIndex", file=sys.stderr)

    print()
    print("// ---------------------------------------------------------------")
    print("// oxide_offsets.h  —  TypeInfo slots (offline, static scan)")
    print("// Generated by Nov/typeinfo_offline.py")
    print("// ---------------------------------------------------------------")
    for name, tdi, cname, cands in all_results:
        print(f"\n// --- {cname} (TypeDefIndex {tdi}) ---")
        if not cands:
            print(f"//   UNRESOLVED: no 'MOVZ W0, #{tdi}' + BL + ADRP/STR match in .text")
            print(f"//   (fallback: keep runtime resolver — ox_scanClassByName)")
            continue

        # Prefer a candidate whose BL matches the dominant metadata_bl
        chosen = cands[0]
        if metadata_bl is not None:
            for c in cands:
                if c[1] == metadata_bl:
                    chosen = c
                    break

        print(f"//   {len(cands)} candidate slot(s)"
              f"{' matching metadata BL' if metadata_bl and chosen[1] == metadata_bl else ''}")
        print(f"//   picked: MOVZ site VA=0x{chosen[0]:X}, BL=0x{chosen[1]:X}")
        print(f"static constexpr uint64_t {name:<24s} = 0x{chosen[2]:X}; "
              f"// {cname}")

        if args.all_hits:
            for i, (mv, bl, sv) in enumerate(cands):
                mark = "*" if (mv, bl, sv) == chosen else " "
                print(f"//   {mark} [{i}] MOVZ=0x{mv:X}  BL=0x{bl:X}  slot=0x{sv:X}")

    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
