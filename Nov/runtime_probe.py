#!/usr/bin/env python3
"""
runtime_probe.py — вытащить свежие TypeInfo RVA из runtime-лога чита.

Использование:
    python3 runtime_probe.py <path-to-eclips_oxide_YYYYMMDD_HHMMSS.log>

Лог берётся с устройства:
    adb pull /sdcard/Download/EclipsOxide/eclips_oxide_*.log

Что парсит:
    [sweep] <ClassName> ns=<Namespace> -> klass=0x... TypeInfoRVA=0x...  OK
    [AUTO]  <Ns>.<ClassName>: klass=0x... TypeInfoRVA=0x...
    [scan]  <Ns>.<Name> FOUND klass=0x... после N MB

Что выводит:
    - Табличку "новые" vs. "старые" RVA (сравнение с oxide_offsets.h)
    - Готовый патч-фрагмент для вставки в include/oxide_offsets.h
"""
import argparse, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OFFSETS_PATH = os.path.normpath(os.path.join(HERE, "..", "proj", "include", "oxide_offsets.h"))

# TypeInfo константы, которые чит держит в oxide_offsets.h.
# Имя_в_логе, namespace, макро_в_offsets.h
KNOWN_TYPEINFOS = [
    ("PlayerManager",  "Oxide",          "PLAYERMANAGER_TYPEINFO"),
    ("BuildingPiece",  "Oxide.Building", "BUILDINGPIECE_TYPEINFO"),
    ("PlayerVitals",   "Oxide",          "PLAYERVITALS_TYPEINFO"),
]

# Регексы под форматы, которые emit'ит main.cpp
RE_SWEEP = re.compile(
    r"\[sweep\]\s+(?P<name>\S+)\s+ns=(?P<ns>\S+)\s+->\s+klass=0x(?P<klass>[0-9a-fA-F]+)\s+TypeInfoRVA=0x(?P<rva>[0-9a-fA-F]+)\s+(?P<ok>OK|MISS)"
)
RE_AUTO = re.compile(
    r"\[AUTO\]\s+(?P<ns>[^.]+)\.(?P<name>\S+):\s+klass=0x(?P<klass>[0-9a-fA-F]+)\s+.*?TypeInfoRVA=0x(?P<rva>[0-9a-fA-F]+)"
)
RE_SCAN_FOUND = re.compile(
    r"\[scan\]\s+(?P<ns>\S+)\.(?P<name>\S+)\s+FOUND\s+klass=0x(?P<klass>[0-9a-fA-F]+)"
)
RE_CONSTEXPR = re.compile(
    r"static\s+constexpr\s+uint64_t\s+(?P<name>[A-Z0-9_]+)\s*=\s*0x(?P<val>[0-9a-fA-F]+)"
)


def parse_log(path):
    """Возвращает {(ns, name): {'klass': int, 'rva': int|None, 'ok': bool}}"""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            for rx in (RE_SWEEP, RE_AUTO):
                m = rx.search(ln)
                if not m:
                    continue
                key = (m.group("ns"), m.group("name"))
                rva = int(m.group("rva"), 16) if m.group("rva") else None
                klass = int(m.group("klass"), 16)
                ok = True
                if rx is RE_SWEEP:
                    ok = m.group("ok") == "OK"
                # Первое найденное имеет приоритет; повторные записи не переписывают
                # (в логе одна строка [sweep] на класс + возможно [AUTO] дубли).
                if key not in out or (out[key]["rva"] is None and rva):
                    out[key] = {"klass": klass, "rva": rva, "ok": ok}
                break
            else:
                m = RE_SCAN_FOUND.search(ln)
                if m:
                    key = (m.group("ns"), m.group("name"))
                    out.setdefault(key, {"klass": int(m.group("klass"), 16),
                                         "rva": None, "ok": True})
    return out


def parse_offsets_h(path):
    """Возвращает {NAME: int} для всех static constexpr uint64_t объявлений."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            m = RE_CONSTEXPR.search(ln)
            if m:
                out.setdefault(m.group("name"), int(m.group("val"), 16))
    return out


def render_report(found, current):
    print("=" * 72)
    print("  RUNTIME TypeInfo REPORT")
    print("=" * 72)
    print(f"{'Class':<32} {'log RVA':>12} {'oxide_offsets':>14} {'status':>8}")
    print("-" * 72)
    patch_lines = []
    for name_log, ns_log, macro in KNOWN_TYPEINFOS:
        entry = found.get((ns_log, name_log))
        cur = current.get(macro)
        log_rva = entry["rva"] if entry and entry["rva"] else None
        if log_rva is None:
            status = "MISS" if entry else "no log"
            print(f"{name_log:<32} {'-':>12} {('0x%x' % cur) if cur else '-':>14} {status:>8}")
            continue
        cur_str = f"0x{cur:x}" if cur else "-"
        marker = "same" if cur == log_rva else "NEW"
        print(f"{name_log:<32} {'0x%x' % log_rva:>12} {cur_str:>14} {marker:>8}")
        if cur != log_rva:
            patch_lines.append(
                f"static constexpr uint64_t {macro:<24} = 0x{log_rva:X}; "
                f"// {ns_log}.{name_log}_TypeInfo (runtime-resolved)")

    # Дополнительно — sweep-only находки без макроса.
    extra = []
    known_pairs = {(ns, n) for n, ns, _ in KNOWN_TYPEINFOS}
    for (ns, name), entry in found.items():
        if (ns, name) not in known_pairs and entry.get("rva"):
            extra.append((ns, name, entry["rva"]))
    if extra:
        print()
        print("Additional TypeInfo RVAs found (no matching macro in oxide_offsets.h):")
        for ns, name, rva in sorted(extra):
            print(f"  {ns:<20}.{name:<24} 0x{rva:x}")

    if patch_lines:
        print()
        print("=" * 72)
        print("  PATCH for include/oxide_offsets.h (paste as needed):")
        print("=" * 72)
        for ln in patch_lines:
            print(ln)


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip())
    ap.add_argument("logfile", help="/sdcard/Download/EclipsOxide/eclips_oxide_*.log")
    ap.add_argument("--offsets", default=OFFSETS_PATH,
                    help="path to include/oxide_offsets.h (default: sibling repo)")
    args = ap.parse_args()

    if not os.path.exists(args.logfile):
        print(f"log not found: {args.logfile}", file=sys.stderr)
        sys.exit(1)

    found = parse_log(args.logfile)
    current = parse_offsets_h(args.offsets)
    if not found:
        print("Никаких [sweep] / [AUTO] / [scan] строк в логе — фоновый sweep, "
              "видимо, не завершился (ищи 'TypeInfo auto-resolve' / 'sweep завершён').",
              file=sys.stderr)
        sys.exit(2)
    render_report(found, current)


if __name__ == "__main__":
    main()
