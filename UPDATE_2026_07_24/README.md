# Oxide Survival Island — Offset Refresh (UPDATE 2026-07-24)

Full offline re-dump against the **updated** game binaries. The game shipped a new
`libil2cpp.so` + `global-metadata.dat`; every il2cpp offset was re-derived statically
(no runtime probing) and verified against the new metadata. This folder contains the
regenerated mod source with the new constants and a freshly built ARM64 binary.

## Binaries

| Artifact | Size (bytes) | md5 |
|---|---:|---|
| NEW `libil2cpp.so` | 200,561,288 | `b3a79ff1dcd1f8d97ba237525440f90d` |
| NEW `global-metadata.dat` | 26,980,208 | `52208a1bbb506ab72936c04bfea63f08` |
| OLD `libil2cpp.so` (prev version) | 199,229,776 | `7ebaf7428502ba9bf8ebed6da533fe50` |
| OLD `global-metadata.dat` (decrypted dump) | 26,890,460 | `28ca933b02b0d046ddda550a6d55469e` |
| Built mod `eclipsoxide` (ARM64) | 3,494,736 | `ad29ded9385beab8d13a6e33ac372d5a` |

Both metadata files are il2cpp **v39** (`magic 0xFAB11BAF`).

## TL;DR — what changed

1. **The types array RVA shifted** and its entry count grew (metadata grew by 120 type
   definitions). This is why the old `IL2CPP_TYPES_RVA = 0xB63EB48` failed to confirm the
   `metadataRegistration` anchor.
2. **All MetadataCache runtime slots** (`s_TypeInfoTable`, `metadata_base`, header,
   MetadataCache*, stride) moved because `.bss` layout shifted.
3. **All TypeDefIndex (TDI) and byvalTypeIndex values shifted** (types were inserted).
4. **The metadata header is now XOR-encrypted** with key `0xA5C3F19D` (the previous
   decrypted dump we had on disk was plaintext). The mod already knew this key (used in
   the `MetadataCache` chain); the same key survives unchanged in the new binary's code.
5. **NO field offset changed.** The obfuscator only re-randomized field *names*; every
   struct layout is byte-identical between old and new (verified field-by-field against a
   regenerated `il2cpp.h` / `dump.cs`). `main.cpp` and the field-offset half of
   `oxide_offsets.h` did not need any layout edits — only the metadata identifiers/RVAs.

## 1. Core RVAs & MetadataCache slots

| Constant | OLD | NEW | Where |
|---|---|---|---|
| `IL2CPP_TYPES_RVA` (metadataRegistration→types) | `0xB63EB48` (104482) | **`0xB77C750`** (104776) | `oxide_offsets.h` |
| `s_TypeInfoTable` slot (`OX_S_TYPEINFO_TABLE_RVA`) | `0xBE3BB58` | **`0xBF80FF0`** | `main.cpp` |
| MetadataCache* slot (`OX_META_MC_RVA`) | `0xBE3BB10` | **`0xBF80FF8`** | `main.cpp` |
| metadata_base slot (`OX_META_BASE_PTR_RVA`) | `0xBE3BB20` | **`0xBF81008`** | `main.cpp` |
| header ptr slot (`OX_META_HEADER_PTR_RVA`) | `0xBE3BB28` | **`0xBF81010`** | `main.cpp` |
| stride slot (`OX_META_STRIDE_RVA`) | `0xBE3BB34` | **`0xBF8101C`** | `main.cpp` |
| header XOR key (`OX_META_XOR_KEY`) | `0xA5C3F19D` | `0xA5C3F19D` (unchanged) | `main.cpp` |
| `OX_TYPEDEF_COUNT` | 29366 | **29486** | `main.cpp` |

### How the RVAs were found (offline, deterministic)

- **types array**: scanned `.data.rel.ro` for the `Il2CppMetadataRegistration` struct whose
  `typeDefinitionsSizesCount == <typeDef count>` (29486 for NEW), then took the `types`
  pointer (pair index 3). The old anchor reproduces exactly (`0xB63EB48`/104482, first 8
  type pointers all valid), which validated the method before applying to the new binary.
- **MetadataCache slots**: disassembled `GetTypeInfoFromTypeDefinitionIndex` (NEW
  `@0x4DFF360`) and `MetadataCache::Initialize` (NEW `@0x4DFF4F8`) with capstone. The
  slots are the `adrp;ldr/str [xN,#imm]` targets. Cross-checked 1:1 against the OLD
  function (OLD `GetTypeInfoFromTypeDefinitionIndex @0x4D773DC`, s_TypeInfoTable load
  `0x4D77404: ldr x22,[x8,#0xb58]` → `0xBE3BB58`). The NEW equivalent is
  `0x4DFF370: ldr x21,[x8,#0xff0]` → `0xBF80FF0`. All slots live in `.bss` (RW, zero in
  file, allocated at runtime) in both builds.

## 2. TypeDefIndex (TDI) and byvalTypeIndex

`byvalTypeIndex` = `int32` at offset `+0x08` in the `Il2CppTypeDefinition` record.
It is also the index into `metadataRegistration->types[]` used by the mod to resolve a
class: `il2cpp_class_from_il2cpp_type(types[byvalTypeIndex])`. Each was verified:
`types[byvalTypeIndex].klassIndex == TDI`, canonical (`byref=0, pinned=0`), `IL2CPP_TYPE_CLASS`.

| Class (namespace) | OLD TDI | NEW TDI | OLD byval | NEW byval |
|---|---:|---:|---:|---:|
| `Oxide.PlayerManager` | 8251 | **8357** | 60939 | **60998** |
| `Oxide.Building.BuildingPiece` | 8521 | **8633** | 46390 | **46457** |
| `Oxide.PlayerVitals` | 7923 | **8030** | 61003 | **61062** |
| `Oxide.RaycastManager` | 8048 | **8155** | 62162 | **62232** |
| `Oxide.MouseLook` | 7902 | **8009** | 58695 | **58788** |
| `UnityEngine.Camera` | 13698 | **13810** | 39710 | **46796** |
| `Oxide.EntityVitals` | 7912 | **8019** | 50639 | **50770** |
| `Oxide.GenericVitals` | 7914 | **8021** | 52479 | **52632** |
| `Oxide.FPManager` | — | **8123** | — | **51358** |

All 9 classes located by full-name walk over the new metadata and **VERIFY OK**.

### Metadata string-table name offsets (used for runtime name verification)

`OX_META_NAME_OFF_* = STR_OFF + nameIndex` (absolute offset from `metadata_base` to the
class name in the string table). The old formula reproduces the old constants exactly.

| Constant | OLD | NEW |
|---|---|---|
| `OX_META_NAME_OFF_PLAYERMANAGER` | `0x1704F0` | **`0x171EE6`** |
| `OX_META_NAME_OFF_BUILDINGPIECE` | `0x10C22B` | **`0x10C379`** |
| `OX_META_NAME_OFF_PLAYERVITALS` | `0x169546` | **`0x16AEE6`** |

## 3. Decrypted metadata header (NEW)

The NEW header is XOR-encrypted (key `0xA5C3F19D`). After decrypt, `size == count × recordSize`
validates for every table (proof of correct decryption). The header uses a
`(offset, size, count)` triplet per table for this protected build.

| Table | NEW offset | NEW count | rec |
|---|---|---:|---:|
| string | `0x000DBE2C` | 209195 (bytes: 3102099) | — |
| typeDefinitions | `0x01523780` | 29486 | 82 |
| methods | `0x00476CAC` | 226854 | 32 |
| fields | `0x0121118C` | 127816 | 12 |
| parameters | `0x00FA6B68` | 211075 | 12 |
| properties | `0x003DC3A0` | 31655 | 20 |
| images | `0x01771C3C` | 189 | 36 |

Binary-side `metadataRegistration` (NEW, base `0xB6CE220`): `types` 0xB77C750/104776,
`fieldOffsets` 0xBAC2700/29486, `typeDefinitionsSizes` 29486, `metadataUsages` **empty**
(count 0 — this is why per-class TypeInfo has no stable static slot and must be resolved
at runtime). Assembly-CSharp code-gen module slot: `0xB9CA0F8`.

## 4. Field offsets — NO CHANGES

Regenerated `il2cpp.h` and `dump.cs` for both builds and compared the target classes
field-by-field. Every offset is identical; only the obfuscated field/type *names* were
re-randomized (e.g. PlayerManager `Np*`→`Gr*`, MouseLook `kq*`→`GN*`, FPManager
`kY*/kS*`→`GO*`). Confirmation from the regenerated NEW `dump.cs` (human-readable names):

| Class | Field | Offset (OLD == NEW) |
|---|---|---|
| PlayerManager | worldCameraRoot / mouseLook / raycastManager / fpManager | 0x68 / 0x70 / 0x88 / 0x90 |
| PlayerManager | vitals / team / lastTickPosition | 0xC8 / 0x120 / 0x1C8 |
| PlayerManager | userID / teamName | 0x278 / 0x280 |
| MouseLook | m_LookRoot / kqP.x (pitch) / kqP.y (yaw) | 0x28 / 0x60 / 0x64 |
| RaycastManager | player / m_WorldCamera / m_RayLength / m_AimRayLength | 0x20 / 0x30 / 0x38 / 0x3C |
| RaycastManager | m_LayerMask / m_AimLayerMask | 0x48 / 0x4C |
| FPManager | m_WorldCamera / FOV current / FOV base / FOV offset | 0x20 / 0xA0 / 0xAC / 0xB4 |
| BuildingPiece | m_PieceName / m_CorrectPosition / m_Bounds | 0x80 / 0x8C / 0xD0 |
| BuildingPiece | m_Grade / gradeHolder | 0x190 / 0x198 |
| BuildingPiece | health / maxHealth / id | 0x380 / 0x384 / 0x38C |
| BuildingPiece (static) | saveList (HashSet) / saveLookup (Dictionary) | +0x0 / +0x8 |
| GenericVitals | m_MaxHealth | 0x88 |

Il2CppClass ABI offsets (`CLASS_NAME=0x10`, `CLASS_NAMESPACE=0x18`,
`CLASS_STATIC_FIELDS=0xB8`) are il2cpp-runtime-version constants; both builds are the same
Unity/il2cpp v39, so they are unchanged.

## 5. Files in this folder

- `src/main.cpp` — updated MetadataCache chain RVAs, TDIs, byvalTypeIndex, name offsets,
  typedef count. No structural/logic changes (field offsets were already correct).
- `include/oxide_offsets.h` — updated `IL2CPP_TYPES_RVA` and all `*_TYPEINFO_TYPEIDX`
  (= byvalTypeIndex). Field-offset constants unchanged.
- `Android.mk`, `Application.mk` — unchanged build config (arm64-v8a, android-29,
  c++_static, release, strip-all). Output module: `eclipsoxide`.
- `eclipsoxide.gz.b64` — the rebuilt ARM64 executable (links cleanly with `ndk-build`,
  NDK r27), committed as base64-of-gzip because the commit API caps request bodies at 4MB
  and accepts only text content. Recover the exact binary with:

  ```
  base64 -d eclipsoxide.gz.b64 | gunzip > eclipsoxide
  # verify: md5sum eclipsoxide  ->  ad29ded9385beab8d13a6e33ac372d5a  (3494736 bytes)
  ```

## Build

```
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=./Android.mk NDK_APPLICATION_MK=./Application.mk -j4
# => libs/arm64-v8a/eclipsoxide
```

Verified: compiles and links (only `main.cpp` recompiled). The updated log string
`offline-verified RVA 0xBF80FF0` is present in the built binary, confirming the new
constants are baked in. (Numeric constants are additionally scrambled by `oxorany` at
compile time, so they don't appear as raw immediates.)

## Notes / limitations

- Per-class static TypeInfo RVAs (`PLAYERMANAGER_TYPEINFO = 0xB8C1AE8`, etc.) are **not**
  resolvable as stable static slots in il2cpp v39 (the global `metadataUsages` table is
  empty). They are retained only as seeds for the disabled scan-based fallback and are
  overwritten at runtime; the active path resolves via `s_TypeInfoTable[TDI]`.
- The active runtime resolution path (`ox_fastSeedTypeInfoFromHeader`) uses:
  `s_TypeInfoTable` slot (`0xBF80FF0`), `metadata_base` slot (`0xBF81008`), the new TDIs,
  `OX_TYPEDEF_COUNT` (29486), and the new name offsets — all updated here.
