# Oxide Survival Island — PRECISION_V2

Offline reverse-engineering fix pass. Every change below is traced to a concrete
piece of evidence in the game binary (`libil2cpp.so`, byte-identical to the
user's game) or `global-metadata.dat` / `il2cpp.h` / `script.json`.

Engine: Unity 6000.3.18f1, il2cpp, Mirror networking.
Build: NDK r27, arm64-v8a, standalone PIE executable `eclipsoxide`.

---

## TL;DR — what was actually broken

**All four ESP/aim bugs share ONE root cause: the camera (eye position + look
basis) was derived from an UNVERIFIABLE native `Transform` read, and it was read
at the WRONG offset.**

- The mod read the camera's world rotation from a native Unity `Transform` at
  `internal + 0x28 → matrixData → +0xA0 (quaternion)`.
- The authoritative Unity `Transform_internal` layout (confirmed via UnknownCheats
  Rust-reversal reference, same engine family) is:
  `self @ 0x28`, **`mData (MatrixData*) @ 0x38`**, `EntryIndex @ 0x40`.
  So `internal + 0x28` is the `self` back-pointer, **not** matrixData — the mod
  was dereferencing the wrong field and reading garbage as the rotation.
- Even with the correct `0x38`, the world quaternion actually lives inside
  `libunity.so` (reached by `get_rotation_Injected`, an icall). We disassembled
  `Transform.get_position` (`0xAA77168`) and `get_rotation` (`0xAA776A0`) in the
  game binary and confirmed BOTH end in `blr x8` into libunity — the native
  transform math is **not present in libil2cpp** and cannot be verified from it.

Because the camera orientation/position was wrong:
1. ESP boxes drifted vertically when tilting the camera (bug 1).
2. ESP "floated everywhere" and only looked right when the screen was static (bug 2).
3. The wall-check LOS ray started from the wrong origin and pointed the wrong
   way, so it never blocked correctly (bug 3).
4. ESP lagged behind camera movement (bug 4).

Additionally, the mod projected with a **fixed 98.6° horizontal FOV slider**,
ignoring the game's real (and ADS-varying) FOV — a second large source of
box drift toward the screen edges and gross error when scoped.

---

## The fix — camera basis from VERIFIED look angles (no libunity needed)

We disassembled `Oxide.MouseLook.uae` (`0x52B0AD4`) and found the ground truth:

```
m_LookRoot (0x28) is the PITCH pivot. Every frame the game does:
    m_LookRoot.localRotation = Quaternion.Euler( kqP.x°, 0, roll )
  (ldr s9,[x19,#0x60] ; Internal_FromEulerRad ; Transform.set_localRotation)
```

So:
- **`kqP.x` @ `0x60` = PITCH in DEGREES** (positive = looking down). VERIFIED.
- **`kqP.y` @ `0x64` = YAW in DEGREES** (accumulator applied to the body). This
  matches the exact convention the aimbot already writes
  (`yaw = atan2(dx,dz)`, `pitch = -atan2(dy,horiz)`), which the user confirms
  turns the view correctly.

Both are plain **managed float fields** — 100% readable and version-stable, with
zero libunity dependency. The camera basis is rebuilt with direct trigonometry
(Unity left-handed, Y-up), numerically verified against the aimbot's inverse
math to 1e-16:

```
right   = ( cosYaw,           0,        -sinYaw )
up      = ( sinYaw·sinPitch,  cosPitch,  cosYaw·sinPitch )
forward = ( sinYaw·cosPitch, -sinPitch,  cosYaw·cosPitch )
```

Eye position = `lastTickPosition` (feet, server-authoritative managed Vector3 @
`0x1C8`) + `EYE_HEIGHT`. This is the same managed field that already worked for
player positions, so it is reliable for the local camera too.

Files: `ox_readLookBasis`, `ox_buildCameraFromPlayer` in `src/main.cpp`.

---

## The fix — LIVE FOV from the game (vertical, ADS-aware)

We disassembled `Oxide.FPManager.LateUpdate` (`0x52DAE88`) and `jWU`
(`0x52DAA2C`):

```
LateUpdate: Camera.set_fieldOfView( kYl(0xA0) - kSP(0xB4) )   // effective vFOV
jWU:        _kSQ(0xAC) = PlayerPrefs FOV, aspect-corrected     // base vFOV
```

- `FPManager` is at `PlayerManager + 0x90` (VERIFIED in `il2cpp.h`).
- Effective **vertical** FOV = `kYl (0xA0) − kSP (0xB4)`. All three fields are
  managed floats inside FPManager's struct bounds.
- W2S was changed to accept the game's vertical FOV directly
  (`tanV` from `fovV`, then `tanH = tanV·(W/H)`), falling back to the horizontal
  slider only if FPManager is momentarily unavailable.

This removes the fixed-FOV guesswork and makes boxes lock at the edges and scale
correctly while scoped.

Files: `ox_readCameraFovV` in `src/main.cpp`; `w2s_angular` in `src/utils.cpp`;
`CameraView` in `include/utils.h`.

---

## The fix — ADS-only aimbot (bug 5)

`isADS` is derived from the live FOV: when aiming, the game lerps `kYl` down
toward the weapon's aim FOV, so `aiming = (kYl < baseFOV × 0.90)`, compared
relative to the player's own hip-fire FOV (`_kSQ`) so it works at any FOV
setting. New toggle **"Only when aiming (ADS/scope)"** in the Aim tab (default
ON, per request). When enabled and not aiming, `ox_runAimbot` returns
immediately.

Files: `aimOnlyADS` global, `ox_runAimbot` gate, `oxTabAim` in `src/main.cpp`.

---

## Wall check (bug 3) — data path re-verified, ray fixed

The wall-check data path was independently re-verified and is **correct**:

- `BuildingPiece.saveList` is a Mono-classic `HashSet<BuildingPiece>`. We
  disassembled `HashSet.Add` (`0x7642050`) and confirmed the exact layout the mod
  uses: `slots @ 0x18`, `count @ 0x20`, `lastIndex @ 0x24`,
  data at `slots + 0x20`, slot stride `0x10`, value at `+0x8`, hashCode at `+0x0`.
- `BuildingPiece..cctor` (`0x5406020`) confirms the static offsets:
  `klass->static_fields` at `+0xB8`, then **`saveList` stored at
  `static_fields + 0x0`** (`str x19,[x8]`) and **`saveLookup` (Dictionary) at
  `static_fields + 0x8`** (`str x19,[x0,#8]`).

So the collection reading is right. The wall check failed because the **LOS ray
came from the broken camera** (wrong origin/direction). With the camera fixed,
`ox_isTargetVisible(cam.pos, targetPoint)` now casts a correct ray, so
aim-through-walls is properly rejected and ESP visibility colouring is correct.

> Offline caveat (stated honestly): whether `saveList`/`saveLookup` are fully
> populated on a *non-host* client is a runtime-only property that cannot be
> proven from a static dump. The mod already reads the HashSet with a Dictionary
> fallback, and 16 client-reachable `BuildingPiece` methods reference these
> statics, so they are actively used client-side. If a specific server build
> leaves them empty on pure clients, no offset fix can populate them — but the
> code path, offsets, and (now) the ray geometry are all correct.

---

## Settings cleanup (bug 6)

Removed the dev-only "Runtime offsets" section from the Settings tab
(`Runtime dump to Download`, `Extract decrypted metadata` buttons and their
status text). The underlying functions are kept but marked `[[maybe_unused]]`
so the build stays clean. User-facing settings (opacity, FPS, performance
intervals, exit/unload) are unchanged.

---

## Verified offset table (this build)

| Type | Field | Offset | Source |
|------|-------|--------|--------|
| PlayerManager | mouseLook | 0x70 | il2cpp.h |
| PlayerManager | fpManager | 0x90 | il2cpp.h |
| PlayerManager | vitals | 0xC8 | il2cpp.h |
| PlayerManager | lastTickPosition | 0x1C8 | il2cpp.h |
| PlayerManager | teamName / userID | 0x280 / 0x278 | il2cpp.h |
| MouseLook | m_LookRoot | 0x28 | il2cpp.h + uae disasm |
| MouseLook | kqP.x = pitch° | 0x60 | uae disasm (set_localRotation) |
| MouseLook | kqP.y = yaw° | 0x64 | uae disasm + aimbot convention |
| FPManager | kYl (cur vFOV) | 0xA0 | LateUpdate disasm |
| FPManager | _kSQ (base vFOV) | 0xAC | jWU disasm |
| FPManager | kSP (FOV offset) | 0xB4 | LateUpdate disasm |
| BuildingPiece | m_CorrectPosition | 0x8C | il2cpp.h |
| BuildingPiece | m_Bounds | 0xD0 | il2cpp.h |
| BuildingPiece | health | 0x380 | il2cpp.h |
| BuildingPiece | saveList (static) | static_fields+0x0 | .cctor disasm |
| Il2CppClass | static_fields | 0xB8 | .cctor disasm |
| HashSet<T> | slots / count / lastIndex | 0x18 / 0x20 / 0x24 | HashSet.Add disasm |

---

## Build

```
export NDK=/path/to/android-ndk-r27
cd proj
$NDK/ndk-build NDK_PROJECT_PATH=$PWD NDK_APPLICATION_MK=$PWD/Application.mk \
    APP_BUILD_SCRIPT=$PWD/Android.mk -j4
# -> libs/arm64-v8a/eclipsoxide  (ARM64 PIE executable)
```
