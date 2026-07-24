#!/usr/bin/env bash
# generate_dump.sh — full offline dumper pipeline.
#
# Takes an encrypted (or already-decrypted) global-metadata.dat and a libil2cpp.so,
# produces a full dump.cs (~40 MB, 1M lines, all 29366 types) plus the
# intermediate codereg.json. No runtime log, no root, no /data hardcode.
#
# Usage:
#   ./Nov/generate_dump.sh <libil2cpp.so> <global-metadata.dat> [out_dir]
#
# Defaults: out_dir = ./dump_out
# Requires: python3 stdlib only.

set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <libil2cpp.so> <global-metadata.dat> [out_dir]" >&2
    exit 1
fi

SO="$(realpath "$1")"
META="$(realpath "$2")"
OUT="${3:-$(pwd)/dump_out}"
mkdir -p "$OUT"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="$(cd "$SCRIPT_DIR/../proj/tools" && pwd)"

echo "[+] libil2cpp: $SO"
echo "[+] metadata:  $META"
echo "[+] out_dir:   $OUT"

# --- 1. Decrypt metadata if the header looks encrypted. ------------------
DEC_META="$OUT/global-metadata.dec.dat"
python3 - "$META" "$DEC_META" <<'PY'
import struct, sys, shutil
KEY, HDR_START, HDR_END = 0xA5C3F19D, 0x08, 0x17C
src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, 'rb').read())
magic, ver = struct.unpack_from('<II', data, 0)
if magic != 0xFAB11BAF or ver != 39:
    raise SystemExit(f"unexpected magic/version: 0x{magic:08X}/{ver}")
# Detect encrypted vs already-decrypted by checking a header dword we know
# is small in cleartext (images count = 188 == 0xBC). Header offset 0xB8 holds
# that count in plaintext; encrypted value is 0xBC ^ 0xA5C3F19D.
img_cnt = struct.unpack_from('<I', data, 0xB8)[0]
if img_cnt == 188:
    print(f"[+] metadata already decrypted (images count = {img_cnt})")
elif (img_cnt ^ KEY) == 188:
    print(f"[+] header encrypted — un-XORing [0x{HDR_START:X}..0x{HDR_END:X})")
    for off in range(HDR_START, HDR_END, 4):
        v = struct.unpack_from('<I', data, off)[0]
        struct.pack_into('<I', data, off, v ^ KEY)
else:
    raise SystemExit(f"header sanity check failed: images_count={img_cnt}")
open(dst, 'wb').write(data)
print(f"[+] wrote {dst} ({len(data)} bytes)")
PY

# --- 2. codereg + dumpgen -------------------------------------------------
export OX_LIBIL2CPP="$SO"
export OX_METADATA="$DEC_META"
export OX_CODEREG="$OUT/codereg.json"
export OX_DUMP_CS="$OUT/dump.cs"

pushd "$SCRIPT_DIR" >/dev/null
python3 codereg.py | grep -E '^(array|first|last|  |  UnityEngine|  mscorlib)' || true
python3 dumpgen.py
popd >/dev/null

echo "[+] outputs:"
ls -lh "$OUT"

# --- 3. Sanity: verify the eight TypeInfos we rely on ---------------------
python3 "$SCRIPT_DIR/typeinfo_offline.py" "$SO" --metadata "$DEC_META" \
    --tdi RAYCASTMANAGER_TYPEINFO:8048:Oxide.RaycastManager \
    --tdi MOUSELOOK_TYPEINFO:7902:Oxide.MouseLook \
    --tdi CAMERA_TYPEINFO:13698:UnityEngine.Camera \
    --tdi ENTITYVITALS_TYPEINFO:7912:Oxide.EntityVitals \
    --tdi GENERICVITALS_TYPEINFO:7914:Oxide.GenericVitals \
    2>&1 | grep -E '^// ---|VERIFY|_TYPEIDX'
