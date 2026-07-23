#!/usr/bin/env bash
# reassemble_split_zip.sh — reassemble the split-zip archive Н.z01..Н.zip
# committed at the repo root and verify contents.
#
# On-disk layout after checkout:
#   Н.z01 Н.z02 … Н.z10 Н.zip     (10 × 5 MiB volumes + last volume/CD)
#
# Usage:
#   ./Nov/reassemble_split_zip.sh [source_dir] [output_dir]
#
# Defaults: source_dir = repo root (parent of this script),
#           output_dir = ./unpacked (relative to CWD).
#
# Requires: zip (Info-ZIP ≥ 3.0), unzip, xxd, od. The Cyrillic filename is
# handled by copying parts to Latin-named tempfiles first, so `zip` on
# any locale can pick them up.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEFAULT_SRC="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="${1:-$DEFAULT_SRC}"
OUT_DIR="${2:-$(pwd)/unpacked}"
WORK="$(mktemp -d -t oxide_split.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

for bin in zip unzip xxd od; do
    command -v "$bin" >/dev/null 2>&1 || { echo "[!] missing dependency: $bin"; exit 1; }
done

echo "[+] source_dir : $SRC_DIR"
echo "[+] out_dir    : $OUT_DIR"
echo "[+] work_dir   : $WORK"

# Copy Cyrillic-named parts to Latin names inside the work dir.
for i in 01 02 03 04 05 06 07 08 09 10; do
    part="$SRC_DIR/Н.z$i"
    [ -f "$part" ] || { echo "[!] missing $part"; exit 1; }
    cp "$part" "$WORK/N.z$i"
done
[ -f "$SRC_DIR/Н.zip" ] || { echo "[!] missing $SRC_DIR/Н.zip"; exit 1; }
cp "$SRC_DIR/Н.zip" "$WORK/N.zip"
echo "[+] copied 10 volumes + main to work_dir"

# Reassemble split zip into a single archive.
echo "[+] running: zip -s 0 N.zip --out full.zip"
( cd "$WORK" && zip -s 0 N.zip --out full.zip >/dev/null )
echo "[+] full.zip: $(du -h "$WORK/full.zip" | cut -f1)"

mkdir -p "$OUT_DIR"
echo "[+] extracting to $OUT_DIR"
unzip -o "$WORK/full.zip" -d "$OUT_DIR" >/dev/null

echo
echo "[+] contents:"
ls -lh "$OUT_DIR"

# Verify expected binaries if present.
META="$OUT_DIR/global-metadata.dat"
if [ -f "$META" ]; then
    magic=$(xxd -l 4 -p "$META")
    case "$magic" in
        af1bb1fa)
            echo "[+] global-metadata.dat: magic OK (0xFAB11BAF, decrypted, v39)"
            ;;
        *)
            echo "[!] global-metadata.dat: unexpected magic $magic"
            echo "    encrypted? run: python3 proj/tools/decrypt_metadata.py <in.dat> <out.dat>"
            ;;
    esac
fi

SO="$OUT_DIR/libil2cpp.so"
if [ -f "$SO" ]; then
    if head -c 4 "$SO" | od -An -c | grep -q "177   E   L   F"; then
        echo "[+] libil2cpp.so: ELF magic OK"
    else
        echo "[!] libil2cpp.so: not an ELF (or truncated)"
    fi
fi

echo
echo "[+] done. next: cp $OUT_DIR/{libil2cpp.so,global-metadata.dat} Nov/ && cd Nov && python3 -B dumpgen.py && python3 -B scriptgen.py && python3 -B il2cppgen.py"
