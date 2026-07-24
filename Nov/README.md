# Offline + verified TypeInfo resolution (`typeinfo_offline.py`)

Resolve `Il2CppClass*` TypeInfo for target classes **statically**, straight from
`libil2cpp.so`, with **no runtime log**, and **verify** every result against
`global-metadata.dat` so the output is trustworthy.

## TL;DR

```
python3 Nov/typeinfo_offline.py libil2cpp.so --metadata global-metadata.dat
```

Emits `oxide_offsets.h`-ready constants: a single `IL2CPP_TYPES_RVA` plus a
verified `*_TYPEIDX` per class. The mod resolves each TypeInfo at load with one
call. Exit code is non-zero if any class fails to resolve (2) or fails name
verification (3).

## The dead end (metadata v39 / il2cpp v27+)

The original scanner assumed codegen emits the TypeDefIndex as an immediate:
`MOVZ W0,#<TDI>` -> `BL init` -> `ADRP/STR slot`. On this build that is simply
not how it works. Verified against the real binary:

| Signal | Result |
| --- | --- |
| `MOVZ Wd,#TDI` in `.text` for 8251 / 8521 / 7923 (any reg, 32/64-bit) | **0 hits** |
| `Il2CppMetadataRegistration.metadataUsages` | **empty (count 0)** — removed in v27+ |
| TDI as raw `u32` in `.rodata` | present (54 / 70 / 7) — data, not code |

So there is no immediate to scan and no global usage table to walk. The old
path is kept behind `--legacy` for older builds only.

## The approach that works: `metadataRegistration->types`

`Il2CppMetadataRegistration.types` is a live `Il2CppType*[]`. For this build:

```
Il2CppMetadataRegistration @0xB590D98
  genericClasses       count=39380     ptr=0xB236B20
  genericInsts         count=32775     ptr=0xB303A30
  genericMethodTable   count=302288    ptr=0x23DEA0C
  types                count=104482    ptr=0xB63EB48   <-- anchor
  methodSpecs          count=355380    ptr=0x1FCD79C
  fieldOffsets         count=29366     ptr=0xB980808
  typeDefinitionsSizes count=29366     ptr=0xB9B9DB8
  metadataUsages       count=0         ptr=null
```

Each class/valuetype `Il2CppType` carries `data.__klassIndex == TypeDefIndex`
and `type == IL2CPP_TYPE_CLASS` (0x12) / `IL2CPP_TYPE_VALUETYPE` (0x11). For a
known TDI we find its **type index** into that array offline. Because the `.so`
is a PIE, every pointer field is an `R_AARCH64_RELATIVE` reloc (file stores 0,
`.rela.dyn` addend holds the VA); the tool resolves them.

## Reaching 100%: canonical pick + name verification

Several `Il2CppType` entries can share a klassIndex — they are `byref`/`pinned`
variants of the same class. The tool:

1. Parses the `byref` (bit 30) and `pinned` (bit 31) flags and picks the
   **canonical** entry (`byref=0, pinned=0`).
2. With `--metadata`, resolves `klassIndex -> Il2CppTypeDefinition.name`
   (including the nested `declaringType` walk) and asserts it equals the
   expected class name. A mismatch is a hard error (exit 3); a class is only
   emitted if it **verifies**. No "picked first, hope it's right."

### Verified output (this build, all `VERIFY OK`)

| Class | TypeDefIndex | TYPE_INDEX |
| --- | --- | --- |
| `Oxide.PlayerManager` | 8251 | **60939** |
| `Oxide.Building.BuildingPiece` | 8521 | **39693** |
| `Oxide.PlayerVitals` | 7923 | **61003** |
| `Oxide.RaycastManager` | 8048 | **62162** |
| `Oxide.MouseLook` | 7902 | **58695** |
| `UnityEngine.Camera` | 13698 | **39710** |

### Mod-side usage

```cpp
static constexpr uint64_t IL2CPP_TYPES_RVA = 0xB63EB48;
Il2CppType** g = (Il2CppType**)((uint8_t*)il2cpp_base + IL2CPP_TYPES_RVA);
Il2CppClass* PlayerManager_TypeInfo =
    il2cpp_class_from_il2cpp_type(g[PLAYERMANAGER_TYPEINFO_TYPEIDX]);
```

## Adding classes

```
python3 Nov/typeinfo_offline.py libil2cpp.so --metadata global-metadata.dat \
    --tdi CAMERA_TYPEINFO:13698:UnityEngine.Camera
```

The `--tdi` value is `NAME:TypeDefIndex[:Expected.Full.Name]`. When the expected
name is given, it is enforced by verification.

## Per-build constants

Overridable if the dumper reports different values for a new build:

- types array: `--types-va` / `--types-count`
- metadata tables: `--meta-str` / `--meta-td-off` / `--meta-td-count` / `--meta-td-rec`

## Tests

```
python3 Nov/test_typeinfo_offline.py    # 39/39
```

Covers the legacy ARM64 encoders/decoders, reloc parsing, metadata-registration
location, type-index resolution, canonical (byref/pinned) selection, metadata
name resolution (incl. nested declaringType), and the magic guard — on
hand-built synthetic ELF + metadata blobs.

## Dependencies

Pure Python 3 stdlib. No `capstone`, `lief`, or `pyelftools`.

---

# Full offline dumper pipeline (env-var configurable)

`resolver.py` / `codereg.py` / `dumpgen.py` now read their input/output paths
from environment variables instead of the old hardcoded `/data/*`. No root,
no `/data` prepare needed:

```
OX_LIBIL2CPP=./libil2cpp.so \
OX_METADATA=./global-metadata.dec.dat \
OX_CODEREG=./codereg.json \
OX_DUMP_CS=./dump.cs \
python3 codereg.py && python3 dumpgen.py
```

Or use the one-shot wrapper (auto-detects encrypted vs decrypted metadata,
verifies all 8 known TypeInfos at the end):

```
./Nov/generate_dump.sh <libil2cpp.so> <global-metadata.dat> [out_dir]
```

Produces (in `out_dir/`):
- `global-metadata.dec.dat` — un-XORed header
- `codereg.json` — per-module `(mpc, mpp)` for RVA resolution
- `dump.cs` — full `Il2CppDumper` format, all 29366 types with fields
  (name + type + offset) and methods (name + signature + RVA).

## `field_search.py` — offline dump.cs inspector

Grep-ish tool tuned for the dump format. Finds the right class block by short
name (won't collide with nested types), enumerates fields/methods with type
filters, lists enum values in declaration order.

```
export OX_DUMP_CS=./dump.cs

# fields of a class, filtered by declared type
python3 Nov/field_search.py --class PlayerWeapon --type-regex 'WeaponState|bool'

# fields by name pattern
python3 Nov/field_search.py --class PlayerManager --field-regex 'team|user|prime|respawn'

# enum values with computed integer values (0..N-1 in declaration order)
python3 Nov/field_search.py --list-enum WeaponState

# global regex with N lines of context
python3 Nov/field_search.py --regex 'AimAssist|IsAiming' --context 3
```
