# GLASS UI — visual rewrite of the Eclips Oxide menu

Pure visual/UX rework of the in-app Dear ImGui menu (`proj/src/main.cpp`,
`Layout_tick_UI` and its helpers), adopting the "Trial Engine v4" dark-glass
idiom. **No behavior changed** — every toggle, slider range, default value,
threshold, and bound global is identical to before. Only rendering code was
rewritten. No game-memory / offset / aimbot / tracking / LOS / player-cache
logic was touched.

## What changed (visual only)

- **Dark glass shell**: layered background (gradient + optional muted bg-art),
  two accent aura glows, soft drop shadows, frosted panel, rounded corners,
  top sheen, thin accent border with outer glow.
- **iOS-style toggle switches** — custom widget replacing `ImGui::Checkbox`
  visually: sprung knob, track glow when on, drop-shadowed knob. Same `bool*`
  bindings.
- **Custom sliders** — gradient fill along the track, glowing knob, live value
  readout on the right. Same `float*` / ranges. `SliderInt`-style values reuse
  the same widget.
- **Segmented controls** with an animated sliding accent pill — replace the two
  `Combo` controls (Line origin, Target point). Same `int*` index semantics.
- **Navigation**: sidebar in landscape / bottom tab bar in portrait, each with
  an animated active indicator. Same 4 tabs (ESP, Aim, Camera, Settings) and
  the same `g_activeTab` binding. Vector-drawn tab icons (eye / target / lens /
  gear) via `ImDrawList` primitives — no PNG assets.
- **5 accent themes** (Aurora, Violet, Mint, Ember, Rose), swappable from a
  color-swatch strip on the Settings tab (`g_accentTheme`). Violet ≈ the
  project's original magenta.
- **Page-transition fade** when switching tabs.
- **Watermark chip** (app name + FPS) in a corner, compact.
- **Redesigned enemy counter** (top center): glassy pill, accent aura glow that
  scales with enemy count, vector crosshair icon, large number + small
  "ENEMIES" tag, pulse animation on count change, danger-red when enemies are
  near. Still taps to toggle the menu; still writes `g_window` for the
  orientation hack.

## Not changed

- Tab set, option set, all variable bindings (`&espdraw`, `&aimm`,
  `&espHideTeammates`, `&aimFovPixels`, `&camFov`, `&g_cacheInterval`, …).
- Slider ranges, toggle defaults, thresholds.
- The Chinese font (`OPPOSans_H`) — only color/size/layout use it.
- Any game logic.

## Files here

- `src/main.cpp` — the rewritten menu (the only source file changed).
- `Android.mk`, `Application.mk` — unchanged build files, included so this
  folder documents the exact build inputs.

## Build

```bash
export NDK=/path/to/android-ndk-r27
cd proj
$NDK/ndk-build -j$(nproc) NDK_PROJECT_PATH=$PWD \
  NDK_APPLICATION_MK=$PWD/Application.mk APP_BUILD_SCRIPT=$PWD/Android.mk
```

Output: `proj/libs/arm64-v8a/eclipsoxide` (standalone arm64 PIE).

## Known notes / regressions

- Draw-call count is higher than the old menu (custom widgets emit many
  `ImDrawList` primitives per row: shadows, glow loops, gradients). On very
  low-end devices this may cost a few FPS while the menu is open; the in-game
  ESP path is unaffected.
- The old menu's stat band and animated tab pills were replaced by the
  sidebar/bottom-bar navigation and page header, so those exact widgets are gone
  (superseded, not removed features).
