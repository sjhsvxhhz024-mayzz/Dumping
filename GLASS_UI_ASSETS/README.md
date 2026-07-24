# GLASS_UI_ASSETS — embedded PNG icon integration

Replaces the vector-drawn placeholder glyphs in the Eclips Oxide glass UI
(Dear ImGui + OpenGL ES 3, Android NativeActivity) with real embedded PNG
textures from the Trial Engine **"paper"** icon set. Pure asset work — no game
logic, memory, aim/ESP/wall/team code touched, and no new toggles/menu options.

## Icons embedded (`include/glass_ui_icons.h`)

Raw PNG file bytes baked into `static const unsigned char <name>_png[]` +
`<name>_png_size`. Decoded at runtime by stb_image (`STBI_ONLY_PNG`) and
uploaded to GL textures. All assets are RGBA and blit with alpha blending.

| C name            | Source file                                 | Bytes | Size (px) |
|-------------------|---------------------------------------------|-------|-----------|
| `icon_esp`        | `trial_engine_icons_esp_paper_4x.png`       | 659   | 96×96     |
| `icon_aim`        | `trial_engine_icons_aim_paper_4x.png`       | 755   | 96×96     |
| `icon_crosshair`  | `trial_engine_icons_crosshair_paper_4x.png` | 798   | 96×96     |
| `icon_hud`        | `trial_engine_icons_hud_paper_4x.png`       | 670   | 96×96     |
| `icon_palette`    | `trial_engine_icons_palette_paper_4x.png`   | 676   | 96×96     |
| `icon_health`     | `trial_engine_icons_health_paper_4x.png`    | 670   | 96×96     |
| `icon_distance`   | `trial_engine_icons_distance_paper_4x.png`  | 761   | 96×96     |
| `icon_shield`     | `trial_engine_icons_lock_paper_4x.png` *(substitute)* | 626 | 96×96 |
| `icon_target`     | `trial_engine_icons_target_paper_4x.png`    | 661   | 96×96     |
| `brand_badge`     | `trial_engine_brand_badge_paper.png`        | 1163  | 240×240   |

**Total embedded PNG data: 7,439 bytes.**

### Substitution

- **`icon_shield`**: the asset set has no `shield.png`, so per the fallback rule
  `icon_shield` uses `trial_engine_icons_lock_paper_4x.png`.
- `icon_target` exists in the set and is embedded as-is (no reuse of `aim`).

## How it works

1. **Embed** — `include/glass_ui_icons.h` holds the raw PNG bytes (tiny files,
   so raw arrays; `binary_to_compressed_c` was not available and would not save
   meaningful space here).
2. **Decode + upload** — `src/menu_bg.cpp` (the single `STB_IMAGE_IMPLEMENTATION`
   translation unit; `STBI_ONLY_PNG` added alongside the existing
   `STBI_ONLY_JPEG`) gains `oxLoadIcons()`. It `stbi_load_from_memory(...,4)` →
   `glGenTextures`/`glBindTexture` → LINEAR filter + CLAMP_TO_EDGE →
   `glTexImage2D(GL_RGBA)` → `stbi_image_free`, storing each `ImTextureID` in a
   global (`g_iconTexESP`, `g_iconTexAim`, …). Guarded by a `static bool` so it
   runs once. Called from `Layout_tick_UI()` in `src/main.cpp` (GL context is
   already current inside the ImGui frame — same pattern as the menu background).
3. **Blit** — vector primitive draws are replaced with
   `ImDrawList::AddImage(tex, min, max, uv0, uv1, tint)`, keeping each icon
   slot's original size and the original accent tint (so active/inactive tab
   states still recolor correctly). Each swap keeps the old vector code as an
   `if (!tex) { … }` fallback so behavior is unchanged if a texture fails to load.

### Vector → texture swap sites (`src/main.cpp`)

- **Tab icons** — `TabIcon()` helper (new) maps tab → texture: ESP→`icon_esp`,
  Aim→`icon_aim`, Camera→`icon_hud`, Settings→`icon_palette`. Called from both
  the portrait bottom-bar and landscape sidebar nav (replacing `TabGlyph`).
- **Enemy-counter crosshair** — `oxDrawCounter()` now blits `icon_crosshair`
  tinted with the counter accent (danger when enemies > 0), same center + pulse.
- **Header brand mark** — the ring+core logo now blits `brand_badge` tinted with
  the accent, in the same circular slot (soft aura kept).

## Build

```
export NDK=/path/to/android-ndk-r27
cd proj   # (this repo lays the project files at GLASS_UI_ASSETS/)
$NDK/ndk-build -j$(nproc) NDK_PROJECT_PATH=$PWD \
  NDK_APPLICATION_MK=$PWD/Application.mk APP_BUILD_SCRIPT=$PWD/Android.mk
```

Output: `libs/arm64-v8a/eclipsoxide` (arm64-v8a, standalone PIE, stripped).

## Added binary size

| Build              | Size (bytes) |
|--------------------|--------------|
| Baseline (no icons)| 3,458,792    |
| With icons         | 3,494,608    |
| **Added**          | **35,816 (~35 KB)** |

The 35,816-byte delta = 7,439 bytes of embedded PNG data + the stb PNG decoder
path (`STBI_ONLY_PNG`) and the loader/accessor code.

## Files in this folder

- `src/main.cpp` — extern decls, `oxLoadIcons()` call, `TabIcon`/`TabIconTex`
  helpers, and the three `AddImage` swap sites.
- `include/glass_ui_icons.h` — embedded PNG byte arrays (the assets).
- `Android.mk`, `Application.mk` — unchanged build scripts.
- `README.md` — this file.
