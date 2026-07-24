# Offline TypeInfo resolution (`typeinfo_offline.py`)

Resolve `Il2CppClass*` TypeInfo for target classes **statically**, straight from
`libil2cpp.so`, with **no runtime log** and no in-game name scan.

## TL;DR

```
python3 Nov/typeinfo_offline.py libil2cpp.so
```

Emits `oxide_offsets.h`-ready constants: a single `IL2CPP_TYPES_RVA` plus a
`*_TYPEIDX` per class. The mod resolves each TypeInfo at load with one call.

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

### Verified indices (this build)

| Class | TypeDefIndex | TYPE_INDEX |
| --- | --- | --- |
| `Oxide.PlayerManager` | 8251 | **60939** |
| `Oxide.Building.BuildingPiece` | 8521 | **39693** |
| `Oxide.PlayerVitals` | 7923 | **61003** |

(Several `Il2CppType` entries can share a klassIndex; they resolve to the same
class. The tool picks the first and lists the equivalents.)

### Mod-side usage

```cpp
static constexpr uint64_t IL2CPP_TYPES_RVA = 0xB63EB48;
Il2CppType** g = (Il2CppType**)((uint8_t*)il2cpp_base + IL2CPP_TYPES_RVA);
Il2CppClass* PlayerManager_TypeInfo =
    il2cpp_class_from_il2cpp_type(g[PLAYERMANAGER_TYPEINFO_TYPEIDX]);
// optional one-time sanity:
// assert(strcmp(il2cpp_class_get_name(PlayerManager_TypeInfo), "PlayerManager") == 0);
```

## Adding classes

```
python3 Nov/typeinfo_offline.py libil2cpp.so \
    --tdi CAMERA_TYPEINFO:13698:UnityEngine.Camera
```

The `types` anchor is overridable via `--types-va` / `--types-count` if the
dumper reports different values for a new build.

## Tests

```
python3 Nov/test_typeinfo_offline.py    # 30/30
```

Covers the legacy ARM64 encoders/decoders and the new reloc parsing, metadata
registration location, and type-index resolution on a hand-built synthetic ELF.

## Dependencies

Pure Python 3 stdlib. No `capstone`, `lief`, or `pyelftools`.
