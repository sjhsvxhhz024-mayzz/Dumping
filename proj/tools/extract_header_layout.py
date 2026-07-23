#!/usr/bin/env python3
"""
extract_header_layout.py

Recovers the global-metadata.dat header layout for protected Unity 6 (il2cpp v39)
builds by disassembling the metadata loader inside libil2cpp.so and reading the exact
byte-offsets the engine uses for each header section.

This build's protection:
  - XOR key: 0xA5C3F19D
  - Encrypted region: header bytes 0x08..0x17C
  - Header format: 31 TRIPLES of (offset, size, count), 12 bytes each  (NOT classic pairs)
  - Section order is NON-STANDARD (e.g. images = triple #14 instead of #20)

Usage:
  python3 extract_header_layout.py libil2cpp.so [loader_start_vaddr] [loader_end_vaddr]

Requires: pip install capstone pyelftools
"""
import sys, struct
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

# Loader window found via the "global-metadata.dat" string xref (IDA: sub_4D765C8).
DEFAULT_START = 0x4D765C8
DEFAULT_END   = 0x4D87600

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    path = sys.argv[1]
    start = int(sys.argv[2], 0) if len(sys.argv) > 2 else DEFAULT_START
    end   = int(sys.argv[3], 0) if len(sys.argv) > 3 else DEFAULT_END

    data = open(path, "rb").read()
    elf = ELFFile(open(path, "rb"))
    text = elf.get_section_by_name(".text")
    ta, to = text["sh_addr"], text["sh_offset"]
    va2fo = lambda va: to + (va - ta)

    code = data[va2fo(start):va2fo(end)]
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)

    reads = []
    for ins in md.disasm(code, start):
        if ins.mnemonic in ("ldr", "ldur") and ins.op_str.strip().startswith("w") \
           and "#0x" in ins.op_str and "[x" in ins.op_str:
            try:
                imm = int(ins.op_str.split("#0x")[1].rstrip("]!"), 16)
            except ValueError:
                continue
            if 0x08 <= imm <= 0x17C and imm % 4 == 0:
                reads.append((ins.address, imm))

    offs = sorted(set(imm for _, imm in reads))
    print("distinct 32-bit header field offsets read by engine (%d):" % len(offs))
    print(" ".join("0x%X" % o for o in offs))
    print("\nheader triples (offset, size, count), 12 bytes each from 0x08:")
    for i in range(31):
        base = 0x08 + i * 12
        print("  triple #%02d @ hdr+0x%03X : off@0x%03X size@0x%03X count@0x%03X"
              % (i, base, base, base + 4, base + 8))

if __name__ == "__main__":
    main()
