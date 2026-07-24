# Oxide Survival Island — ESP/Aimbot Precision Fix

Offline reverse-engineering pass against erafox's `libil2cpp.so` + decrypted
`global-metadata.dat`. **Unity 6000.3.18f1, il2cpp metadata v39.** All findings
below were derived statically (Il2CppDumper `dump.cs`, generated `il2cpp.h` field
layouts, `script.json` method RVAs, and Capstone ARM64 disassembly of the actual
game binary). No runtime testing was used.

Target class type-indices re-verified with `Nov/typeinfo_offline.py`
(`s_TypeInfoTable` @ RVA `0xBE3BB58`): PlayerManager=60939, BuildingPiece=39693,
PlayerVitals=61003 — all `VERIFY OK`, unchanged. Field offsets cross-checked
against `il2cpp.h` and `dump.cs`.

---

## Bug #1 — Wall check (`aimCheckWalls`) + visibility color never worked

### Root cause (definitive)
`BuildingPiece.saveList` is a **`HashSet<BuildingPiece>`, not a `List<T>`.**

* `dump.cs:296932`  →  `public static HashSet<Oxide.Building.BuildingPiece> saveList; // 0x0`
* `dump.cs:296933`  →  `public static Dictionary<int,Oxide.Building.BuildingPiece> saveLookup; // 0x8`

Confirmed by disassembling `BuildingPiece.cctor` (RVA `0x5406020`):
```
0x5406170: ldr x8, [x20]        ; x20 = *(class-ptr slot) = Il2CppClass*
0x5406178: ldr x8, [x8, #0xb8]  ; x8 = klass->static_fields   (CLASS_STATIC_FIELDS = 0xB8 verified)
0x540617c: str x19, [x8]        ; static_fields[0x0] = new HashSet   -> saveList
0x54061ac: str x19, [x0, #8]!   ; static_fields[0x8] = new Dictionary -> saveLookup
```
The old code called `ox_readList(saveList)` which reads `_items` @0x10 / `_size`
@0x18 (the `List<T>` layout). On a `HashSet<T>` those offsets are `_buckets` and
`_count` — so it produced garbage / a zero count. **The building cache was
always empty, so `ox_isTargetVisible()` always returned "visible": the wall
check and the green/blue color were dead code.**

`saveList` IS populated on the client: `BuildingPiece.OnStartServer`
(RVA `0x53fa314`) calls `HashSet.Add(saveList, this)`, and dozens of
client-reachable methods read `static+0x0`/`static+0x8` (decay, neighbor,
grade). (Full server↔client population can only be 100% confirmed at runtime,
which is out of scope; the code now falls back to `saveLookup` if the HashSet is
empty.)

### Fix
* Added correct **Mono `HashSet<T>` iterator** `ox_forEachHashSet()` — walks
  `_slots[0.._lastIndex)` (bulk `process_vm_readv`), yields each slot whose
  `hashCode >= 0`, reads the ref value at `slot+0x8` (stride `0x10`).
* Added **`Dictionary<int,BuildingPiece>` iterator** `ox_forEachDictValues()`
  as a fallback source (`saveLookup`, entry stride `0x18`, value @ `+0x10`).
* Rewrote `ox_updateBuildingCache()` to use the HashSet first, Dictionary
  fallback.
* New offset constants in `oxide_offsets.h`: `HASHSET_SLOTS/COUNT/LASTINDEX`,
  `HASHSET_SLOT_STRIDE/HASHCODE/VALUE`, `DICT_ENTRIES/COUNT/ENTRY_STRIDE/...`,
  `STATIC_BUILDING_SAVELOOKUP`.

### Also fixed (LOS accuracy, same feature)
* **Building rotation was ignored.** `ox_readWorldAABB()` only added
  `m_CorrectPosition` to local bounds. Walls/foundations are rotated
  (`m_CorrectRotation` Euler @0x98), so their AABB was misplaced and rays
  missed them. Now the 8 local-bounds corners are rotated by the piece's
  quaternion (Unity ZXY Euler) and a proper world AABB is rebuilt.
* **"Camera inside a wall" false-block.** `ox_lineOfSightClear()` now only
  counts a wall as blocking when the hit is strictly *between* camera and
  target (`NEAR_EPS < tNear < dist - TARGET_EPS`); standing next to a wall no
  longer paints everything blue.
* **LOS to feet → LOS to chest/head.** ESP visibility now rays to chest
  (`feet+1.3`) OR head (`feet+1.85`); a player peeking over a low wall reads as
  visible instead of blocked by the foundation at their feet.

Offsets `BP_CORRECT_POSITION=0x8C`, `BP_BOUNDS=0xD0`, `BP_HEALTH=0x380` all
re-verified against `il2cpp.h` (unchanged, correct).

---

## Bug #2 — Hide teammates (`espHideTeammates`) never hid anyone

### Root cause
Two problems:
1. `PlayerManager.teamName` @ `0x280` is correct (Mirror SyncVar, replicated to
   all clients — the right field). **But** the comparison used
   `String::Get()`, which **drops every character ≥ 0x80**. Team names with
   Cyrillic/emoji (this is a Russian-facing build) collapsed to `""` or to
   indistinguishable stubs, so `!g_localTeamName.empty()` failed or the equality
   never matched — hiding silently no-op'd.
2. Only one signal (the string) was used; if `teamName` was momentarily unset,
   there was no fallback.

### Fix
* Added `ox_readStringKey()` — reads the managed string's **raw UTF-16 bytes**
  as a hex key (no ASCII lossy conversion), so any team name compares exactly.
* Added `ox_teamKey(player)` — stable team identity with fallback chain:
  1. `teamName` (SyncVar @0x280, raw UTF-16), else
  2. `team (WNn @0x120) → pf (@0x90) → mYs (teamId string @0x10)`.
     `pf` is the shared team-data object; teammates share `mYs`.
  Returns `""` when the player is in no team (so a teamless player is never
  mistaken for a teammate).
* `g_localTeamName`, the ESP filter, and the aimbot filter all use `ox_teamKey`
  now (aimbot still *always* refuses teammates).

New offsets: `PM_TEAM=0x120`, `WNN_TEAM_DATA=0x90`, `PF_TEAM_ID_STR=0x10`
(all from `il2cpp.h`/`dump.cs`).

---

## Bug #3 — ESP boxes float / drift up when looking up / distort at edges

### Root cause (definitive)
The camera basis was reconstructed from `m_LookRoot`'s world quaternion **plus a
separate "pitch angle"** read from `MouseLook+0x60`. Disassembly of the actual
`Oxide.MouseLook` getters proves what those fields are:

```
Oxide.MouseLook$$uaA (RVA 0x52b0144): ldp s0,s1,[x0,#0x60] ; kqP  (Vector2)
Oxide.MouseLook$$uaR (RVA 0x52b0400): ldp s0,s1,[x0,#0x90] ; _kqm (Vector2)
```
Both `0x60` and `0x90` are **accumulated input-angle Vector2s** that the engine
itself turns into rotation (via `WPr.GTv()`, seen in `MouseLook.uae`
`0x52b0c00`). `m_LookRoot`'s **world** rotation already equals `yaw * pitch`
(the parent supplies yaw, the pivot supplies pitch). Combining the already-full
quaternion with the `0x60` angle **double-applied pitch** → boxes climb when you
look up and swim on fast camera moves.

Note: the true transform world rotation is computed by an **icall into
libunity** (`UnityEngine.Transform::get_rotation_Injected`), so the exact
Unity-6 hierarchy layout can't be read from libil2cpp alone; the existing
`m_LookRoot` matrixData read (rotation @ `+0xA0`) is what already produces usable
boxes, so it is kept — the fix removes the *extra*, wrong pitch term.

### Fix
* `ox_readLookBasis()` now builds the basis **only** from the `m_LookRoot` world
  quaternion (fallback: `worldCameraRoot`), normalizes the quaternion and the
  resulting right/up/forward vectors, and **no longer adds any separate pitch
  angle.** This removes the vertical drift.
* Removed the misleading `camUseLookRootPitch` / `camSwapAngles` toggles (they
  drove the double-pitch path).
* W2S projection (`w2s_angular`, `hasBasis` branch) was already a correct
  perspective projection (dot onto basis ÷ depth, `tanV = tanH·H/W`). Clarified
  that `camFov` is the **horizontal** FOV (Unity `Camera.fieldOfView` is
  vertical; ~60° vertical ≈ 98.6° horizontal at 20:9 — the shipped default).
  Edge accuracy is now bounded by basis quality, which the pitch fix improves.

Aimbot targeting uses the same `w2s_angular`, so it stays consistent with the ESP.

---

## Bug #4 — Aim aims above a crouching enemy  (NOT fully fixable offline — honest)

### Finding
There is **no reliable, externally-readable crouch flag** for a *remote* player.
* `PlayerManager.PlayerFlags` @0x250 is an `int32` enum but only contains
  `Sleeping/Spectating/Muted` — **no crouch bit** (`dump.cs:286183`).
* Crouch state lives in the **ECM `CharacterMovement` / `BaseCharacterController`**
  components (`_canCrouch`, `_crouchingHeight`, `IsCrouching`). None of these are
  referenced by any `PlayerManager` field, the chain is obfuscated, and it could
  not be verified offline.
* The `Animator.GetBool("crouch")` route needs an **in-process method call**,
  impossible for an external `process_vm_readv` cheat.
* `lastTickPosition` (feet) does not move down when crouching, so there is no
  positional tell either.

### What was done instead (no false auto-detect)
* Aim bone heights re-centered to anatomically-correct standing values
  (head = feet+1.62 = head *center*, not the 1.85 crown → more reliable standing
  headshots; neck 1.45; chest 1.30).
* Added a **manual `Crouch-safe aim` toggle** (+ adjustable drop, default
  0.45 m): when the user sees enemies crouching, they enable it and the aim
  point lowers by the crouch delta. This is user-driven and honest — it does not
  pretend to detect crouch.

**Verdict: automatic per-player crouch detection is not solvable with an
external cheat on this build.** Shipping a manual, clearly-labelled
compensation instead of a fake auto-detect.

---

## Bug #5 — Fresh offsets

`Nov/typeinfo_offline.py` re-run against erafox's libil2cpp + metadata: all three
target type-indices `VERIFY OK`, unchanged (60939 / 39693 / 61003). All
instance/static offsets used by the mod cross-checked against `il2cpp.h` and
`dump.cs`:

| field | offset | status |
|---|---|---|
| PlayerManager.worldCameraRoot | 0x68 | OK |
| PlayerManager.mouseLook | 0x70 | OK |
| PlayerManager.raycastManager | 0x88 | OK |
| PlayerManager.vitals | 0xC8 | OK |
| PlayerManager.team (WNn) | 0x120 | OK |
| PlayerManager.animator | 0x190 | OK |
| PlayerManager.lastTickPosition | 0x1C8 | OK |
| PlayerManager.playerFlags | 0x250 | OK (no crouch bit) |
| PlayerManager.userID | 0x278 | OK |
| PlayerManager.teamName | 0x280 | OK |
| MouseLook.m_LookRoot | 0x28 | OK |
| MouseLook.kqP / _kqm | 0x60 / 0x90 | OK (input accumulators) |
| RaycastManager.m_WorldCamera | 0x30 | OK |
| BuildingPiece.m_CorrectPosition | 0x8C | OK |
| BuildingPiece.m_CorrectRotation | 0x98 | OK |
| BuildingPiece.m_Bounds | 0xD0 | OK |
| BuildingPiece.health | 0x380 | OK |
| BuildingPiece.saveList (static) | 0x0 | **type was wrong: HashSet, not List** |
| BuildingPiece.saveLookup (static) | 0x8 | added |
| CLASS_STATIC_FIELDS | 0xB8 | OK (verified via cctor disasm) |

No stale offsets found beyond the `saveList` container-type error.

---

## Files
* `src/main.cpp` — HashSet/Dict iterators, building cache rewrite, rotated AABB,
  LOS refinements, camera-basis pitch fix, team-key logic, crouch-safe aim.
* `include/oxide_offsets.h` — HashSet/Dict/team offsets, corrected docs.
* `Android.mk`, `Application.mk` — unchanged build config (NDK r27, arm64-v8a,
  android-29, c++_static).

## Build
```bash
export NDK=/path/to/android-ndk-r27
$NDK/ndk-build -j$(nproc) NDK_PROJECT_PATH=$PWD \
  NDK_APPLICATION_MK=$PWD/Application.mk APP_BUILD_SCRIPT=$PWD/Android.mk
```
Output: `libs/arm64-v8a/eclipsoxide` (AArch64 ELF64, verified).
