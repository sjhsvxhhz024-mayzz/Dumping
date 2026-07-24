#!/usr/bin/env python3
"""
field_search.py — offline field/method search over generated dump.cs.

Usage:
    export OX_DUMP_CS=/path/to/dump.cs
    python3 field_search.py --class PlayerManager --field-regex 'prime|user'
    python3 field_search.py --class PlayerWeapon  --enum WeaponState
    python3 field_search.py --regex 'isAiming|IsADS|scoped' --context 3
    python3 field_search.py --list-enum WeaponState

Pure-python. Reads the whole dump into memory (~40 MB for this build).
"""
import argparse, os, re, sys
from pathlib import Path


def load_dump(path: str) -> list:
    p = Path(path)
    if not p.exists():
        raise SystemExit(f"[!] dump.cs not found: {path} (set OX_DUMP_CS)")
    return p.read_text(encoding="utf-8", errors="replace").splitlines()


def find_class_block(lines: list, name: str) -> tuple:
    """Return (start_line, end_line) — inclusive line indices of the class body."""
    # Match the class/struct/interface/enum whose FULL short name is `name`
    # (i.e. terminated by whitespace, ':' or newline — never by '.', which would
    # match a nested type such as PlayerManager.PlayerFlags when searching
    # for PlayerManager).
    pat = re.compile(rf"^(public|private|internal|protected)?\s*(sealed|abstract|static)?\s*"
                     rf"(class|struct|interface|enum)\s+{re.escape(name)}(?=[\s:{{])")
    for i, l in enumerate(lines):
        if pat.match(l):
            # walk to matching '}'
            for j in range(i + 1, len(lines)):
                if lines[j].rstrip() == "}":
                    return i, j
            return i, len(lines) - 1
    return -1, -1


def print_class_fields(lines: list, name: str, field_re: "re.Pattern | None",
                       type_re: "re.Pattern | None") -> None:
    s, e = find_class_block(lines, name)
    if s < 0:
        print(f"[!] class not found: {name}")
        return
    print(f"// {lines[s]}  (lines {s+1}..{e+1})")
    for l in lines[s+1:e]:
        # a "field" line matches "type name; // 0xNN"
        m = re.match(r"^\t([^;{]+?)\s+([\w<>\[\].`]+);\s*//\s*(0x[0-9A-Fa-f]+)\s*$", l)
        if not m: continue
        ftype, fname, off = m.groups()
        if type_re and not type_re.search(ftype): continue
        if field_re and not field_re.search(fname): continue
        print(f"    {ftype:<60} {fname:<40} @ {off}")


def print_enum_values(lines: list, name: str) -> None:
    s, e = find_class_block(lines, name)
    if s < 0 or "enum" not in lines[s]:
        print(f"[!] enum not found: {name}")
        return
    print(f"// {lines[s]}")
    idx = 0
    for l in lines[s+1:e]:
        m = re.match(r"^\tpublic static const [\w.<>]+\s+(\w+);", l)
        if not m: continue
        print(f"    {m.group(1):<20} = {idx}")
        idx += 1


def global_regex(lines: list, pat: re.Pattern, context: int) -> None:
    for i, l in enumerate(lines):
        if pat.search(l):
            lo = max(0, i - context); hi = min(len(lines), i + context + 1)
            print(f"\n--- line {i+1} ---")
            for j in range(lo, hi):
                mark = ">>> " if j == i else "    "
                print(f"{mark}{lines[j]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", default=os.environ.get("OX_DUMP_CS", "dump.cs"),
                    help="path to dump.cs (default $OX_DUMP_CS or ./dump.cs)")
    ap.add_argument("--class", dest="cls", help="class name (short) to inspect")
    ap.add_argument("--field-regex", help="filter fields by name regex")
    ap.add_argument("--type-regex", help="filter fields by declared type regex")
    ap.add_argument("--list-enum", help="list values of a given enum")
    ap.add_argument("--regex", help="global search across whole dump")
    ap.add_argument("--context", type=int, default=1, help="context lines around global match")
    args = ap.parse_args()

    lines = load_dump(args.dump)
    print(f"[+] dump: {args.dump}  ({len(lines)} lines)")

    if args.list_enum:
        print_enum_values(lines, args.list_enum); return

    if args.cls:
        fr = re.compile(args.field_regex, re.I) if args.field_regex else None
        tr = re.compile(args.type_regex, re.I) if args.type_regex else None
        print_class_fields(lines, args.cls, fr, tr); return

    if args.regex:
        global_regex(lines, re.compile(args.regex, re.I), args.context); return

    ap.print_help()


if __name__ == "__main__":
    main()
