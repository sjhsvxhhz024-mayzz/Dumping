// ============================================================================
//  Eclips Oxide — visual-only menu (Android / touch adaptive)
//  Pure Dear ImGui. No game process access, memory reads, hooks, input
//  automation, targeting, or anti-cheat interaction. This is a UI mock only.
// ============================================================================
#pragma once

#include "imgui.h"

// ----------------------------------------------------------------------------
//  Menu state — every field is purely presentational (drives the mock preview).
// ----------------------------------------------------------------------------
struct EclipsOxideMenuState
{
    // Navigation
    int   page            = 0;      // 0 Dashboard, 1 Visuals, 2 Aim, 3 World, 4 Settings
    bool  open            = true;   // whole panel visible

    // Visuals (ESP) page — mock toggles
    bool  boxes           = true;
    bool  skeleton        = false;
    bool  health          = true;
    bool  labels          = true;
    bool  item_esp        = false;
    bool  edge_arrows     = true;
    float max_distance    = 140.0f;
    float overlay_alpha   = 0.85f;
    int   box_style       = 0;      // 0 Corner, 1 Full, 2 Rounded

    // Aim page — mock toggles
    bool  aim_enabled     = true;
    bool  visible_only    = true;
    bool  smooth          = true;
    bool  prediction      = false;
    float fov             = 96.0f;
    float smoothing       = 6.0f;
    int   target_bone     = 0;      // Head / Chest / Nearest
    bool  show_fov        = true;
    bool  hitmarker       = true;

    // World / HUD page
    bool  safe_area       = false;
    bool  compact_hud     = false;
    int   crosshair_style = 1;      // Dot / Cross / Circle / Chevron
    int   anchor          = 1;      // TL / Center / BR
    float world_time      = 0.5f;

    // Settings / theme page
    int   accent          = 0;      // index into accent palette
    float ui_scale        = 1.0f;   // user scale multiplier (on top of DPI)
    bool  glass           = true;   // frosted panels
    bool  animations      = true;
    bool  show_watermark  = true;

    // Runtime (set by the host each frame; not user editable)
    float dpi_scale       = 1.0f;   // device pixel density scale from the host
    float fps             = 0.0f;   // for the status chip

    // Optional ESP-preview model texture. When set by the host, the preview
    // draws this image (the real character model) instead of the procedural
    // fallback dummy. preview_tex is a backend texture id (GL texture, etc).
    ImTextureID preview_tex   = 0;
    float       preview_tex_w = 0.0f;
    float       preview_tex_h = 0.0f;
};

// Renders the entire menu for one frame. Call between ImGui::NewFrame() and
// ImGui::Render(). Fully self contained — only needs a live ImGui context.
void EclipsOxideMenu_Render(EclipsOxideMenuState& s);
