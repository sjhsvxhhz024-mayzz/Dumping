// ============================================================================
//  Eclips Oxide — visual-only menu implementation (Android / touch adaptive)
//  Pure Dear ImGui draw code. Redesigned dark "glass" UI with animated
//  toggles, sliding navigation, soft shadows, adaptive portrait/landscape
//  layout and DPI/touch aware sizing.
// ============================================================================
#define IMGUI_DEFINE_MATH_OPERATORS
#include "eclips_oxide_menu.h"
#include "imgui_internal.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <algorithm>

// ----------------------------------------------------------------------------
//  Theme
// ----------------------------------------------------------------------------
namespace {

struct Palette {
    ImU32 bg0, bg1;         // window background gradient
    ImU32 panel, panelHi;   // card fill + hover
    ImU32 stroke;           // subtle borders
    ImU32 text, textDim, textFaint;
    ImU32 accent, accentHi; // primary accent + glow
    ImU32 track, trackOn;   // toggle / slider tracks
    ImU32 good, warn, danger;
};

struct AccentDef { const char* name; ImU32 a, b; };
const AccentDef kAccents[] = {
    { "Aurora",  IM_COL32( 88,196,255,255), IM_COL32(120,120,255,255) },
    { "Violet",  IM_COL32(167,120,255,255), IM_COL32(236,110,240,255) },
    { "Mint",    IM_COL32( 60,230,180,255), IM_COL32( 70,200,255,255) },
    { "Ember",   IM_COL32(255,150, 74,255), IM_COL32(255, 92,120,255) },
    { "Rose",    IM_COL32(255,110,160,255), IM_COL32(255,150,120,255) },
};
const int kAccentCount = (int)(sizeof(kAccents)/sizeof(kAccents[0]));

Palette MakePalette(int accentIdx)
{
    accentIdx = ImClamp(accentIdx, 0, kAccentCount - 1);
    Palette p;
    p.bg0       = IM_COL32(18, 20, 28, 255);
    p.bg1       = IM_COL32(11, 12, 18, 255);
    p.panel     = IM_COL32(255,255,255, 10);
    p.panelHi   = IM_COL32(255,255,255, 20);
    p.stroke    = IM_COL32(255,255,255, 24);
    p.text      = IM_COL32(238,240,247,255);
    p.textDim   = IM_COL32(168,173,188,255);
    p.textFaint = IM_COL32(120,125,140,255);
    p.accent    = kAccents[accentIdx].a;
    p.accentHi  = kAccents[accentIdx].b;
    p.track     = IM_COL32(255,255,255, 28);
    p.trackOn   = kAccents[accentIdx].a;
    p.good      = IM_COL32( 74,222,150,255);
    p.warn      = IM_COL32(255,196, 84,255);
    p.danger    = IM_COL32(255, 96,110,255);
    return p;
}

// ---- small helpers ---------------------------------------------------------
ImU32 Fade(ImU32 c, float a) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w *= ImClamp(a, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(v);
}
ImU32 MixU32(ImU32 x, ImU32 y, float t) {
    ImVec4 a = ImGui::ColorConvertU32ToFloat4(x);
    ImVec4 b = ImGui::ColorConvertU32ToFloat4(y);
    return ImGui::ColorConvertFloat4ToU32(ImLerp(a, b, ImClamp(t,0.0f,1.0f)));
}
// animation state stored per-id in the current window storage
float Anim(ImGuiID id, float target, float speed) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float cur = st->GetFloat(id, target);
    float dt  = ImGui::GetIO().DeltaTime;
    float t   = 1.0f - ImPow(2.0f, -speed * ImMax(dt, 0.0f) * 60.0f * 0.0166667f * 8.0f);
    // simple exponential smoothing, framerate independent
    float k   = 1.0f - ImPow(1.0f - ImClamp(speed * dt, 0.0f, 1.0f), 1.0f);
    (void)t;
    cur += (target - cur) * ImClamp(k, 0.0f, 1.0f);
    if (fabsf(target - cur) < 0.0009f) cur = target;
    st->SetFloat(id, cur);
    return cur;
}

// soft drop shadow behind a rounded rect
void SoftShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float size, ImU32 col) {
    const int steps = 8;
    for (int i = steps; i >= 1; --i) {
        float t  = (float)i / steps;
        float ex = size * t;
        float al = (1.0f - t) * (1.0f - t);
        dl->AddRectFilled(ImVec2(a.x - ex, a.y - ex + size * 0.35f),
                          ImVec2(b.x + ex, b.y + ex + size * 0.35f),
                          Fade(col, al), rounding + ex, 0);
    }
}

// vertical gradient rounded panel
void GradientPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bot, float rounding) {
    dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
    // clip corners by overdrawing rounded mask is complex; instead draw rounded
    // fill with average then a subtle top sheen so corners stay crisp.
    (void)rounding;
}

struct UI {
    Palette  pal;
    float    s;        // global scale (dpi * user)
    ImFont*  font;
    ImDrawList* dl;
};

float Txt(UI& ui, ImVec2 pos, ImU32 col, float px, const char* text, float wrap = 0.0f) {
    ui.dl->AddText(ui.font, px, pos, col, text, nullptr, wrap);
    return ImGui::CalcTextSize(text).x; // note: unscaled; callers use for rough layout
}
ImVec2 Measure(UI& ui, float px, const char* text) {
    return ui.font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
}

} // namespace

// ============================================================================
//  Widgets
// ============================================================================
namespace {

// Animated pill toggle. Returns true when value changed.
bool ToggleRow(UI& ui, const char* label, const char* hint, bool* v)
{
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float s   = ui.s;
    const float rowH = 52.0f * s;
    const float pad  = 14.0f * s;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float  w   = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##t", ImVec2(w, rowH));
    bool hov = ImGui::IsItemHovered();
    ImGuiID id = win->GetID("##t");
    ImDrawList* dl = ui.dl;

    float hovA = Anim(win->GetID("##th"), hov ? 1.0f : 0.0f, 16.0f);
    if (hovA > 0.001f)
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + rowH), Fade(ui.pal.panelHi, hovA), 12.0f * s);

    // switch geometry (needed to clip text away from the knob)
    float knobR = 9.5f * s;
    float trackW = 44.0f * s, trackH = 24.0f * s;

    // label + hint (clipped so long strings never run under the switch)
    float textRight = pos.x + w - pad - trackW - 12.0f * s;
    dl->PushClipRect(ImVec2(pos.x + pad, pos.y), ImVec2(textRight, pos.y + rowH), true);
    float ty = hint ? pos.y + rowH * 0.5f - 15.0f * s : pos.y + rowH * 0.5f - 9.0f * s;
    dl->AddText(ui.font, 15.5f * s, ImVec2(pos.x + pad, ty), ui.pal.text, label);
    if (hint)
        dl->AddText(ui.font, 12.0f * s, ImVec2(pos.x + pad, ty + 18.0f * s), ui.pal.textFaint, hint);
    dl->PopClipRect();

    ImVec2 tp(pos.x + w - pad - trackW, pos.y + rowH * 0.5f - trackH * 0.5f);
    if (clicked) *v = !*v;
    float on = Anim(id, *v ? 1.0f : 0.0f, 14.0f);
    ImU32 track = MixU32(ui.pal.track, ui.pal.trackOn, on);
    dl->AddRectFilled(tp, ImVec2(tp.x + trackW, tp.y + trackH), track, trackH * 0.5f);
    if (on > 0.01f) {
        // glow when on
        dl->AddRectFilled(ImVec2(tp.x-2*s,tp.y-2*s), ImVec2(tp.x+trackW+2*s,tp.y+trackH+2*s),
                          Fade(ui.pal.accentHi, 0.25f*on), (trackH+4*s)*0.5f);
        dl->AddRectFilled(tp, ImVec2(tp.x + trackW, tp.y + trackH), Fade(ui.pal.trackOn, on), trackH*0.5f);
    }
    float kx = ImLerp(tp.x + trackH*0.5f, tp.x + trackW - trackH*0.5f, on);
    ImVec2 kc(kx, tp.y + trackH*0.5f);
    dl->AddCircleFilled(ImVec2(kc.x, kc.y+1.0f*s), knobR, IM_COL32(0,0,0,60));
    dl->AddCircleFilled(kc, knobR, IM_COL32(255,255,255,255));

    ImGui::PopID();
    return clicked;
}

// Modern slider with glowing fill. Returns true when value changed.
bool SliderRow(UI& ui, const char* label, float* v, float lo, float hi, const char* fmt)
{
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float s = ui.s;
    const float rowH = 54.0f * s;
    const float pad  = 14.0f * s;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float  w   = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(label);
    ImGui::InvisibleButton("##s", ImVec2(w, rowH));
    bool active = ImGui::IsItemActive();
    bool hov    = ImGui::IsItemHovered();
    ImDrawList* dl = ui.dl;

    float barY = pos.y + rowH - 16.0f * s;
    float bx0 = pos.x + pad, bx1 = pos.x + w - pad;
    float barW = bx1 - bx0;

    bool changed = false;
    if (active && barW > 1.0f) {
        float mx = ImClamp((ImGui::GetIO().MousePos.x - bx0) / barW, 0.0f, 1.0f);
        float nv = lo + mx * (hi - lo);
        if (nv != *v) { *v = nv; changed = true; }
    }
    float t = (hi > lo) ? ImClamp((*v - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;

    // label + value
    dl->AddText(ui.font, 15.0f * s, ImVec2(bx0, pos.y + 6.0f * s), ui.pal.text, label);
    char buf[64]; snprintf(buf, sizeof(buf), fmt, *v);
    ImVec2 vsz = Measure(ui, 14.0f * s, buf);
    dl->AddText(ui.font, 14.0f * s, ImVec2(bx1 - vsz.x, pos.y + 7.0f * s), ui.pal.accent, buf);

    // track
    float th = 5.0f * s;
    dl->AddRectFilled(ImVec2(bx0, barY - th*0.5f), ImVec2(bx1, barY + th*0.5f), ui.pal.track, th);
    float fx = bx0 + barW * t;
    dl->AddRectFilledMultiColor(ImVec2(bx0, barY - th*0.5f), ImVec2(fx, barY + th*0.5f),
                                ui.pal.accent, ui.pal.accentHi, ui.pal.accentHi, ui.pal.accent);
    // knob
    float kr = (hov || active ? 11.0f : 9.0f) * s;
    ImVec2 kc(fx, barY);
    dl->AddCircleFilled(kc, kr + 4.0f*s, Fade(ui.pal.accentHi, 0.30f));
    dl->AddCircleFilled(ImVec2(kc.x, kc.y+1.0f*s), kr, IM_COL32(0,0,0,70));
    dl->AddCircleFilled(kc, kr, IM_COL32(255,255,255,255));
    dl->AddCircleFilled(kc, kr*0.42f, ui.pal.accent);

    ImGui::PopID();
    return changed;
}

// Segmented control. Returns true when selection changed.
bool Segmented(UI& ui, const char* label, int* v, const char* const items[], int count)
{
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float s = ui.s;
    const float pad = 14.0f * s;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ui.dl;

    if (label && label[0]) {
        dl->AddText(ui.font, 15.0f * s, ImVec2(pos.x + pad, pos.y), ui.pal.text, label);
        ImGui::Dummy(ImVec2(0, 24.0f * s));
        pos = ImGui::GetCursorScreenPos();
    }

    float segH = 38.0f * s;
    float x0 = pos.x + pad, x1 = pos.x + w - pad;
    float segW = (x1 - x0);
    float cellW = segW / (float)count;

    ImGui::PushID(label ? label : "seg");
    bool clicked = ImGui::InvisibleButton("##seg", ImVec2(w, segH));
    ImDrawList* d = ui.dl;
    d->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x1, pos.y + segH), ui.pal.panel, segH * 0.5f);
    d->AddRect(ImVec2(x0, pos.y), ImVec2(x1, pos.y + segH), ui.pal.stroke, segH * 0.5f, 0, 1.0f);

    bool changed = false;
    if (clicked) {
        float mx = ImGui::GetIO().MousePos.x - x0;
        int idx = ImClamp((int)(mx / cellW), 0, count - 1);
        if (idx != *v) { *v = idx; changed = true; }
    }
    // sliding pill
    ImGuiID id = win->GetID("##segpos");
    float target = (float)*v;
    float cur = Anim(id, target, 16.0f);
    float px = x0 + cur * cellW;
    ImVec2 pa(px + 3.0f*s, pos.y + 3.0f*s), pb(px + cellW - 3.0f*s, pos.y + segH - 3.0f*s);
    d->AddRectFilled(ImVec2(pa.x-1,pa.y-1), ImVec2(pb.x+1,pb.y+1), Fade(ui.pal.accentHi,0.25f), (segH-6*s)*0.5f);
    d->AddRectFilledMultiColor(pa, pb, ui.pal.accent, ui.pal.accentHi, ui.pal.accentHi, ui.pal.accent);

    for (int i = 0; i < count; ++i) {
        ImVec2 sz = Measure(ui, 13.5f * s, items[i]);
        float cellCx = x0 + (i + 0.5f) * cellW;
        bool sel = (i == *v);
        d->AddText(ui.font, 13.5f * s, ImVec2(cellCx - sz.x*0.5f, pos.y + segH*0.5f - sz.y*0.5f),
                   sel ? IM_COL32(12,14,20,255) : ui.pal.textDim, items[i]);
    }
    ImGui::PopID();
    return changed;
}

void SectionLabel(UI& ui, const char* text) {
    const float s = ui.s;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ui.dl->AddText(ui.font, 11.5f * s, ImVec2(pos.x + 14.0f*s, pos.y), ui.pal.textFaint, text);
    ImVec2 sz = Measure(ui, 11.5f*s, text);
    ui.dl->AddLine(ImVec2(pos.x + 14.0f*s + sz.x + 10.0f*s, pos.y + sz.y*0.5f),
                   ImVec2(pos.x + ImGui::GetContentRegionAvail().x - 14.0f*s, pos.y + sz.y*0.5f),
                   ui.pal.stroke, 1.0f);
    ImGui::Dummy(ImVec2(0, sz.y + 10.0f*s));
}

// A rounded content card that owns a child region. Call CardEnd() after.
void CardBegin(UI& ui, const char* id, float height) {
    const float s = ui.s;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = (height > 0.0f) ? height : ImGui::GetContentRegionAvail().y;
    SoftShadow(ui.dl, pos, ImVec2(pos.x + w, pos.y + h), 16.0f*s, 18.0f*s, IM_COL32(0,0,0,120));
    ui.dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui.pal.panel, 16.0f*s);
    ui.dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), ui.pal.stroke, 16.0f*s, 0, 1.0f);
    // top sheen
    ui.dl->AddRectFilledMultiColor(pos, ImVec2(pos.x + w, pos.y + 40.0f*s),
        IM_COL32(255,255,255,14), IM_COL32(255,255,255,14), IM_COL32(255,255,255,0), IM_COL32(255,255,255,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f*s, 12.0f*s));
    ImGui::BeginChild(id, ImVec2(w, h), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}
void CardEnd() { ImGui::EndChild(); ImGui::PopStyleVar(); }

} // namespace

// ============================================================================
//  Live preview (procedural, no assets / no game access)
// ============================================================================
namespace {
void DrawCorner(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 c, float L, float th) {
    d->AddLine(a, ImVec2(a.x+L,a.y), c, th); d->AddLine(a, ImVec2(a.x,a.y+L), c, th);
    d->AddLine(ImVec2(b.x,a.y), ImVec2(b.x-L,a.y), c, th); d->AddLine(ImVec2(b.x,a.y), ImVec2(b.x,a.y+L), c, th);
    d->AddLine(ImVec2(a.x,b.y), ImVec2(a.x+L,b.y), c, th); d->AddLine(ImVec2(a.x,b.y), ImVec2(a.x,b.y-L), c, th);
    d->AddLine(b, ImVec2(b.x-L,b.y), c, th); d->AddLine(b, ImVec2(b.x,b.y-L), c, th);
}

void DrawPreview(UI& ui, ImVec2 a, ImVec2 b, EclipsOxideMenuState& st)
{
    ImDrawList* d = ui.dl;
    const float s = ui.s;
    float rounding = 16.0f * s;
    d->PushClipRect(a, b, true);
    // sky gradient scene
    d->AddRectFilledMultiColor(a, b, IM_COL32(30,34,48,255), IM_COL32(30,34,48,255),
                                     IM_COL32(16,18,26,255), IM_COL32(16,18,26,255));
    // perspective floor lines
    float cx = (a.x + b.x) * 0.5f;
    float horizon = a.y + (b.y - a.y) * 0.42f;
    for (int i = -6; i <= 6; ++i) {
        float fx = cx + i * (b.x - a.x) * 0.09f;
        d->AddLine(ImVec2(cx + i*8*s, horizon), ImVec2(fx, b.y), Fade(ui.pal.stroke, 0.6f), 1.0f);
    }
    for (int i = 1; i <= 6; ++i) {
        float ty = horizon + (b.y - horizon) * (float)(i*i) / 49.0f;
        d->AddLine(ImVec2(a.x, ty), ImVec2(b.x, ty), Fade(ui.pal.stroke, 0.5f), 1.0f);
    }

    float t = (float)ImGui::GetTime();
    float bob = sinf(t * 1.6f) * 3.0f * s;
    ImU32 ec = ui.pal.accent;

    ImVec2 ba, bb;   // ESP bounding box
    ImVec2 aimc;     // FOV / hitmarker center

    if (st.preview_tex && st.preview_tex_w > 0 && st.preview_tex_h > 0) {
        // -------- real character model (image, taken from the assets) -----
        float availW = (b.x - a.x) - 52*s;
        float availH = (b.y - a.y) - 74*s;
        float fit = ImMin(availW / st.preview_tex_w, availH / st.preview_tex_h);
        float iw = st.preview_tex_w * fit, ih = st.preview_tex_h * fit;
        ImVec2 ip(cx - iw*0.5f, a.y + (b.y - a.y)*0.53f - ih*0.5f + bob);
        d->AddImage(st.preview_tex, ip, ImVec2(ip.x + iw, ip.y + ih));
        ba   = ImVec2(ip.x + iw*0.15f, ip.y + ih*0.015f);
        bb   = ImVec2(ip.x + iw*0.85f, ip.y + ih*0.99f);
        aimc = ImVec2(cx, ip.y + ih*0.30f);
    } else {
        // -------- procedural fallback dummy --------
        float H = (b.y - a.y) * 0.52f;
        float ccx = cx, ccy = a.y + (b.y - a.y) * 0.56f + bob;
        float sc = H / 190.0f;
        ImU32 body = IM_COL32(70,78,96,255), skin = IM_COL32(210,196,182,255), hair = IM_COL32(44,48,60,255);
        ImVec2 head(ccx, ccy - 116*sc), pelvis(ccx, ccy + 18*sc);
        ImVec2 lh(ccx-52*sc, ccy-6*sc), rh(ccx+52*sc, ccy-6*sc), lf(ccx-30*sc, ccy+120*sc), rf(ccx+30*sc, ccy+120*sc);
        d->AddQuadFilled(ImVec2(ccx-34*sc,ccy-80*sc),ImVec2(ccx+34*sc,ccy-80*sc),ImVec2(ccx+46*sc,ccy+24*sc),ImVec2(ccx-46*sc,ccy+24*sc),body);
        d->AddLine(ImVec2(ccx-32*sc,ccy-66*sc), lh, body, 8*sc); d->AddLine(ImVec2(ccx+32*sc,ccy-66*sc), rh, body, 8*sc);
        d->AddLine(ImVec2(ccx-16*sc,ccy+20*sc), lf, body, 11*sc); d->AddLine(ImVec2(ccx+16*sc,ccy+20*sc), rf, body, 11*sc);
        d->AddCircleFilled(lh, 7*sc, skin); d->AddCircleFilled(rh, 7*sc, skin);
        d->AddCircleFilled(ImVec2(head.x, head.y+2*sc), 27*sc, skin);
        d->AddCircleFilled(ImVec2(head.x, head.y-8*sc), 26*sc, hair);
        ba = ImVec2(ccx-64*sc, ccy-150*sc); bb = ImVec2(ccx+64*sc, ccy+140*sc);
        aimc = ImVec2(cx, ccy - H*0.20f);
        if (st.skeleton) {
            ImU32 sk = Fade(ui.pal.accentHi, 0.9f);
            d->AddLine(head, pelvis, sk, 2*s); d->AddLine(ImVec2(ccx,ccy-70*sc), lh, sk, 2*s);
            d->AddLine(ImVec2(ccx,ccy-70*sc), rh, sk, 2*s); d->AddLine(pelvis, lf, sk, 2*s); d->AddLine(pelvis, rf, sk, 2*s);
            for (ImVec2 p : {head, pelvis, lh, rh, lf, rf}) d->AddCircleFilled(p, 3.2f*s, sk);
        }
    }

    if (st.boxes) {
        if (st.box_style == 0)      DrawCorner(d, ba, bb, ec, 20.0f*s, 2.4f*s);
        else if (st.box_style == 1) d->AddRect(ba, bb, ec, 0, 0, 2.0f*s);
        else                        d->AddRect(ba, bb, ec, 8.0f*s, 0, 2.0f*s);
    }
    if (st.health) {
        d->AddRectFilled(ImVec2(ba.x-12*s, ba.y), ImVec2(ba.x-6*s, bb.y), IM_COL32(0,0,0,120), 3*s);
        float hp = 0.62f;
        d->AddRectFilledMultiColor(ImVec2(ba.x-12*s, bb.y - (bb.y-ba.y)*hp), ImVec2(ba.x-6*s, bb.y),
            ui.pal.good, ui.pal.good, ui.pal.good, ui.pal.good);
    }
    if (st.labels) {
        d->AddText(ui.font, 12.0f*s, ImVec2(ba.x, ba.y - 20*s), ui.pal.text, "ENEMY PLAYER");
        char dist[32]; snprintf(dist, sizeof(dist), "%.1f m  /  VISIBLE", 24.0f + sinf(t)*3.0f);
        d->AddText(ui.font, 11.0f*s, ImVec2(ba.x, bb.y + 6*s), ui.pal.textDim, dist);
    }
    if (st.show_fov && st.page == 2)
        d->AddCircle(aimc, st.fov * 0.9f * s, Fade(ui.pal.accentHi, 0.8f), 64, 1.8f*s);
    if (st.hitmarker) {
        ImVec2 hc = aimc; float r = 10*s;
        d->AddLine(ImVec2(hc.x-r, hc.y-r), ImVec2(hc.x-r*0.4f, hc.y-r*0.4f), ui.pal.text, 2*s);
        d->AddLine(ImVec2(hc.x+r, hc.y-r), ImVec2(hc.x+r*0.4f, hc.y-r*0.4f), ui.pal.text, 2*s);
        d->AddLine(ImVec2(hc.x-r, hc.y+r), ImVec2(hc.x-r*0.4f, hc.y+r*0.4f), ui.pal.text, 2*s);
        d->AddLine(ImVec2(hc.x+r, hc.y+r), ImVec2(hc.x+r*0.4f, hc.y+r*0.4f), ui.pal.text, 2*s);
    }
    d->PopClipRect();
    d->AddRect(a, b, ui.pal.stroke, rounding, 0, 1.0f);
}
} // namespace

// ============================================================================
//  Pages
// ============================================================================
namespace {

struct NavItem { const char* label; };
const NavItem kNav[] = { {"Dashboard"}, {"Visuals"}, {"Aim"}, {"World"}, {"Settings"} };
const int kNavCount = 5;

// simple vector glyphs for nav / header
void Glyph(ImDrawList* d, ImVec2 c, int page, ImU32 col, float s) {
    float k = s;
    if (page == 0) { // dashboard grid
        d->AddRect(ImVec2(c.x-8*k,c.y-8*k), ImVec2(c.x-1*k,c.y-1*k), col, 1.5f*k,0,1.8f*k);
        d->AddRect(ImVec2(c.x+1*k,c.y-8*k), ImVec2(c.x+8*k,c.y-1*k), col, 1.5f*k,0,1.8f*k);
        d->AddRect(ImVec2(c.x-8*k,c.y+1*k), ImVec2(c.x-1*k,c.y+8*k), col, 1.5f*k,0,1.8f*k);
        d->AddRect(ImVec2(c.x+1*k,c.y+1*k), ImVec2(c.x+8*k,c.y+8*k), col, 1.5f*k,0,1.8f*k);
    } else if (page == 1) { // eye
        d->AddEllipse(ImVec2(c.x,c.y), 9*k, 6*k, col, 0,24,1.8f*k);
        d->AddCircleFilled(ImVec2(c.x,c.y), 2.6f*k, col);
    } else if (page == 2) { // target
        d->AddCircle(c, 8*k, col, 28, 1.8f*k); d->AddCircle(c, 3*k, col, 16, 1.8f*k);
        d->AddLine(ImVec2(c.x,c.y-11*k),ImVec2(c.x,c.y-5*k),col,1.8f*k);
        d->AddLine(ImVec2(c.x,c.y+5*k),ImVec2(c.x,c.y+11*k),col,1.8f*k);
        d->AddLine(ImVec2(c.x-11*k,c.y),ImVec2(c.x-5*k,c.y),col,1.8f*k);
        d->AddLine(ImVec2(c.x+5*k,c.y),ImVec2(c.x+11*k,c.y),col,1.8f*k);
    } else if (page == 3) { // globe
        d->AddCircle(c, 8*k, col, 28, 1.8f*k);
        d->AddEllipse(ImVec2(c.x,c.y), 3.4f*k, 8*k, col, 0,20,1.6f*k);
        d->AddLine(ImVec2(c.x-8*k,c.y),ImVec2(c.x+8*k,c.y),col,1.6f*k);
    } else { // settings gear (simplified)
        d->AddCircle(c, 7*k, col, 28, 1.8f*k); d->AddCircle(c, 2.6f*k, col, 16, 1.8f*k);
        for (int i=0;i<8;i++){ float ang=i*3.14159f/4.0f; ImVec2 p1(c.x+cosf(ang)*7*k,c.y+sinf(ang)*7*k), p2(c.x+cosf(ang)*10*k,c.y+sinf(ang)*10*k); d->AddLine(p1,p2,col,1.8f*k); }
    }
}

void StatCard(UI& ui, ImVec2 a, float w, float h, const char* caption, const char* value, ImU32 tint) {
    const float s = ui.s;
    ImVec2 b(a.x + w, a.y + h);
    ui.dl->AddRectFilled(a, b, ui.pal.panel, 14.0f*s);
    ui.dl->AddRect(a, b, ui.pal.stroke, 14.0f*s, 0, 1.0f);
    ui.dl->AddRectFilled(a, ImVec2(a.x + 4.0f*s, b.y), tint, 2.0f*s);
    ui.dl->AddText(ui.font, 11.5f*s, ImVec2(a.x + 14*s, a.y + 12*s), ui.pal.textFaint, caption);
    ui.dl->AddText(ui.font, 24.0f*s, ImVec2(a.x + 14*s, a.y + 30*s), ui.pal.text, value);
}

void PageDashboard(UI& ui, EclipsOxideMenuState& st) {
    const float s = ui.s;
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 base = ImGui::GetCursorScreenPos();
    float gap = 10.0f * s;
    float cardW = (w - gap) * 0.5f;
    float cardH = 66.0f * s;
    char fps[24]; snprintf(fps, sizeof(fps), "%.0f", st.fps > 0 ? st.fps : ImGui::GetIO().Framerate);
    StatCard(ui, base, cardW, cardH, "FRAME RATE", fps, ui.pal.good);
    StatCard(ui, ImVec2(base.x + cardW + gap, base.y), cardW, cardH, "PING (MOCK)", "12 ms", ui.pal.accent);
    ImGui::Dummy(ImVec2(0, cardH + gap));
    StatCard(ui, ImGui::GetCursorScreenPos(), cardW, cardH, "PROFILE", "Default", ui.pal.accentHi);
    StatCard(ui, ImVec2(ImGui::GetCursorScreenPos().x + cardW + gap, ImGui::GetCursorScreenPos().y), cardW, cardH, "STATUS", "Sandbox", ui.pal.warn);
    ImGui::Dummy(ImVec2(0, cardH + gap + 4*s));

    SectionLabel(ui, "QUICK TOGGLES");
    ToggleRow(ui, "Enemy boxes", "Corner box overlay in preview", &st.boxes);
    ToggleRow(ui, "Aim assist", "Mock aim point indicator", &st.aim_enabled);
    ToggleRow(ui, "Watermark", "Show the floating brand tag", &st.show_watermark);
}

void PageVisuals(UI& ui, EclipsOxideMenuState& st) {
    SectionLabel(ui, "ENTITY OVERLAYS");
    ToggleRow(ui, "Boxes", "Draw a bounding box on targets", &st.boxes);
    ToggleRow(ui, "Skeleton", "Bone lines over the model", &st.skeleton);
    ToggleRow(ui, "Health bar", "Vertical HP bar beside the box", &st.health);
    ToggleRow(ui, "Name & distance", "Text label above / below", &st.labels);
    ToggleRow(ui, "Item highlights", "Mark nearby pickups", &st.item_esp);
    ToggleRow(ui, "Edge arrows", "Off-screen direction hints", &st.edge_arrows);
    ImGui::Dummy(ImVec2(0, 6*ui.s));
    const char* boxItems[] = { "Corner", "Full", "Rounded" };
    Segmented(ui, "Box style", &st.box_style, boxItems, 3);
    ImGui::Dummy(ImVec2(0, 12*ui.s));
    SectionLabel(ui, "RENDER");
    SliderRow(ui, "Max distance", &st.max_distance, 20, 300, "%.0f m");
    SliderRow(ui, "Overlay opacity", &st.overlay_alpha, 0.2f, 1.0f, "%.0f%%");
}

void PageAim(UI& ui, EclipsOxideMenuState& st) {
    SectionLabel(ui, "AIM (VISUAL MOCK)");
    ToggleRow(ui, "Enable", "Show the aim point indicator", &st.aim_enabled);
    ToggleRow(ui, "Visible only", "Ignore occluded targets", &st.visible_only);
    ToggleRow(ui, "Smoothing", "Ease the indicator movement", &st.smooth);
    ToggleRow(ui, "Prediction", "Lead moving targets (mock)", &st.prediction);
    ImGui::Dummy(ImVec2(0, 6*ui.s));
    SliderRow(ui, "FOV", &st.fov, 24, 180, "%.0f px");
    SliderRow(ui, "Smoothing amount", &st.smoothing, 1, 20, "%.1f");
    ImGui::Dummy(ImVec2(0, 6*ui.s));
    const char* bones[] = { "Head", "Chest", "Nearest" };
    Segmented(ui, "Aim point", &st.target_bone, bones, 3);
    ImGui::Dummy(ImVec2(0, 10*ui.s));
    ToggleRow(ui, "Show FOV circle", "Draw the FOV ring in preview", &st.show_fov);
}

void PageWorld(UI& ui, EclipsOxideMenuState& st) {
    SectionLabel(ui, "HUD");
    ToggleRow(ui, "Hitmarker", "Cross flash on the target", &st.hitmarker);
    ToggleRow(ui, "Safe area", "Draw screen-edge guide", &st.safe_area);
    ToggleRow(ui, "Compact HUD", "Denser status widgets", &st.compact_hud);
    ImGui::Dummy(ImVec2(0, 6*ui.s));
    const char* cross[] = { "Dot", "Cross", "Circle", "Chevron" };
    Segmented(ui, "Crosshair", &st.crosshair_style, cross, 4);
    ImGui::Dummy(ImVec2(0, 12*ui.s));
    const char* anch[] = { "Top left", "Center", "Bottom right" };
    Segmented(ui, "HUD anchor", &st.anchor, anch, 3);
    ImGui::Dummy(ImVec2(0, 12*ui.s));
    SectionLabel(ui, "ENVIRONMENT");
    SliderRow(ui, "Time of day", &st.world_time, 0.0f, 1.0f, "%.2f");
}

void PageSettings(UI& ui, EclipsOxideMenuState& st) {
    const float s = ui.s;
    SectionLabel(ui, "ACCENT");
    // swatches
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float sw = 40.0f * s, sgap = 12.0f * s;
    ImGui::PushID("accents");
    if (ImGui::InvisibleButton("##sw", ImVec2(w, sw))) {
        float mx = ImGui::GetIO().MousePos.x - (pos.x + 14*s);
        int idx = (int)(mx / (sw + sgap));
        if (idx >= 0 && idx < kAccentCount) st.accent = idx;
    }
    for (int i = 0; i < kAccentCount; ++i) {
        ImVec2 a(pos.x + 14*s + i * (sw + sgap), pos.y);
        ImVec2 b(a.x + sw, a.y + sw);
        ui.dl->AddRectFilledMultiColor(a, b, kAccents[i].a, kAccents[i].b, kAccents[i].b, kAccents[i].a);
        // round mask via border
        ui.dl->AddRect(a, b, (i==st.accent)?IM_COL32(255,255,255,255):ui.pal.stroke, 10.0f*s, 0, (i==st.accent)?2.5f*s:1.0f);
        if (i == st.accent)
            ui.dl->AddCircleFilled(ImVec2(b.x-9*s, a.y+9*s), 3.2f*s, IM_COL32(255,255,255,255));
    }
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, sw + 14*s));

    SectionLabel(ui, "INTERFACE");
    SliderRow(ui, "UI scale", &st.ui_scale, 0.8f, 1.6f, "%.2fx");
    ToggleRow(ui, "Frosted panels", "Translucent glass cards", &st.glass);
    ToggleRow(ui, "Animations", "Smooth toggles & transitions", &st.animations);
    ToggleRow(ui, "Watermark", "Floating brand tag", &st.show_watermark);

    ImGui::Dummy(ImVec2(0, 10*s));
    SectionLabel(ui, "ABOUT");
    ImVec2 tp = ImGui::GetCursorScreenPos();
    ui.dl->AddText(ui.font, 12.5f*s, ImVec2(tp.x+14*s, tp.y), ui.pal.textDim,
        "Eclips Oxide \xe2\x80\x94 visual-only sandbox.");
    ui.dl->AddText(ui.font, 12.5f*s, ImVec2(tp.x+14*s, tp.y+18*s), ui.pal.textFaint,
        "No game access, hooks, or automation. UI mock.");
    ImGui::Dummy(ImVec2(0, 44*s));
}

} // namespace

// ============================================================================
//  Main render
// ============================================================================
void EclipsOxideMenu_Render(EclipsOxideMenuState& st)
{
    ImGuiIO& io = ImGui::GetIO();
    UI ui;
    ui.pal  = MakePalette(st.accent);
    ui.font = ImGui::GetFont();
    ui.s    = ImClamp(st.dpi_scale * st.ui_scale, 0.6f, 3.0f);
    const float s = ui.s;

    ImVec2 disp = io.DisplaySize;
    if (disp.x < 1 || disp.y < 1) disp = ImVec2(1280, 720);

    bool  portrait = disp.y > disp.x * 1.05f;
    float margin   = (portrait ? 12.0f : 18.0f) * s;
    float barH     = 34.0f * s;   // top-left toggle bar height
    float barGap   = 10.0f * s;

    if (st.open) {
    // ---- fullscreen backdrop window --------------------------------------
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(disp, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##eclips_oxide_root", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    ui.dl = ImGui::GetWindowDrawList();
    ImDrawList* bg = ui.dl;
    // dim + gradient background
    bg->AddRectFilled(ImVec2(0,0), disp, IM_COL32(0,0,0,150));
    bg->AddRectFilledMultiColor(ImVec2(0,0), disp, ui.pal.bg0, ui.pal.bg0, ui.pal.bg1, ui.pal.bg1);
    // accent aura glow top-left
    for (int i = 6; i >= 1; --i) {
        float r = 220.0f * s * i / 6.0f;
        bg->AddCircleFilled(ImVec2(disp.x*0.16f, disp.y*0.12f), r, Fade(ui.pal.accent, 0.03f), 48);
    }

    // ---- adaptive layout decision ----------------------------------------
    // panel rect (leave safe margins; leave room up top for the toggle bar)
    ImVec2 pa(margin, margin + barH + barGap);
    ImVec2 pb(disp.x - margin, disp.y - margin);

    // header height + nav sizing
    float headerH = 60.0f * s;
    float navW    = 168.0f * s;   // landscape sidebar width
    float navH    = 64.0f * s;    // portrait bottom bar height

    // outer panel shadow + fill
    SoftShadow(bg, pa, pb, 22.0f*s, 26.0f*s, IM_COL32(0,0,0,150));
    bg->AddRectFilled(pa, pb, IM_COL32(20, 22, 31, 245), 22.0f*s);
    bg->AddRect(pa, pb, ui.pal.stroke, 22.0f*s, 0, 1.2f);
    bg->AddRectFilledMultiColor(pa, ImVec2(pb.x, pa.y + 60*s),
        IM_COL32(255,255,255,12), IM_COL32(255,255,255,12), IM_COL32(255,255,255,0), IM_COL32(255,255,255,0));

    // ---- header ----------------------------------------------------------
    {
        ImVec2 h0(pa.x, pa.y);
        ImVec2 lc(h0.x + 34*s, h0.y + headerH*0.5f);
        bg->AddCircleFilled(lc, 15*s, Fade(ui.pal.accent, 0.18f));
        bg->AddCircle(lc, 15*s, ui.pal.accent, 32, 1.6f*s);
        // stylized T
        bg->AddLine(ImVec2(lc.x-7*s, lc.y-6*s), ImVec2(lc.x+7*s, lc.y-6*s), ui.pal.accentHi, 2.4f*s);
        bg->AddLine(ImVec2(lc.x, lc.y-6*s), ImVec2(lc.x, lc.y+7*s), ui.pal.accentHi, 2.4f*s);
        bg->AddText(ui.font, 19.0f*s, ImVec2(h0.x + 58*s, h0.y + headerH*0.5f - 18*s), ui.pal.text, "Eclips Oxide");
        bg->AddText(ui.font, 11.5f*s, ImVec2(h0.x + 58*s, h0.y + headerH*0.5f + 3*s), ui.pal.textFaint, "visual-only sandbox");

        // close button (top-right) — hides the panel; the top-left bar re-opens it
        float cbR = 14.0f*s;
        ImVec2 cbc(pb.x - 26.0f*s, pa.y + headerH*0.5f);
        ImGui::SetCursorScreenPos(ImVec2(cbc.x - cbR, cbc.y - cbR));
        ImGui::PushID("closebtn");
        if (ImGui::InvisibleButton("##x", ImVec2(cbR*2, cbR*2))) st.open = false;
        bool xh = ImGui::IsItemHovered();
        ImGui::PopID();
        bg->AddCircleFilled(cbc, cbR, xh ? Fade(ui.pal.danger, 0.22f) : ui.pal.panel);
        bg->AddCircle(cbc, cbR, xh ? ui.pal.danger : ui.pal.stroke, 24, 1.2f*s);
        ImU32 xcol = xh ? ui.pal.danger : ui.pal.textDim;
        float xr = 4.2f*s;
        bg->AddLine(ImVec2(cbc.x-xr, cbc.y-xr), ImVec2(cbc.x+xr, cbc.y+xr), xcol, 1.9f*s);
        bg->AddLine(ImVec2(cbc.x-xr, cbc.y+xr), ImVec2(cbc.x+xr, cbc.y-xr), xcol, 1.9f*s);
        bg->AddLine(ImVec2(pa.x, pa.y+headerH), ImVec2(pb.x, pa.y+headerH), ui.pal.stroke, 1.0f);
    }

    // ---- body regions ----------------------------------------------------
    ImVec2 bodyA, bodyB;   // scrollable content region
    ImVec2 previewA, previewB; bool showPreview = !portrait;

    if (portrait) {
        bodyA = ImVec2(pa.x, pa.y + headerH);
        bodyB = ImVec2(pb.x, pb.y - navH);
    } else {
        // sidebar | content | preview
        bodyA = ImVec2(pa.x + navW, pa.y + headerH);
        float previewW = ImClamp((pb.x - bodyA.x) * 0.42f, 240.0f*s, 460.0f*s);
        previewB = ImVec2(pb.x - 14*s, pb.y - 14*s);
        previewA = ImVec2(previewB.x - previewW, bodyA.y + 14*s);
        bodyB = ImVec2(previewA.x - 14*s, pb.y);
    }

    // ---- navigation ------------------------------------------------------
    {
        if (portrait) {
            // bottom tab bar
            float y0 = pb.y - navH, y1 = pb.y;
            bg->AddLine(ImVec2(pa.x, y0), ImVec2(pb.x, y0), ui.pal.stroke, 1.0f);
            float cellW = (pb.x - pa.x) / (float)kNavCount;
            ImGui::SetCursorScreenPos(ImVec2(pa.x, y0));
            ImGui::PushID("navbar");
            if (ImGui::InvisibleButton("##nav", ImVec2(pb.x - pa.x, navH))) {
                float mx = io.MousePos.x - pa.x;
                st.page = ImClamp((int)(mx / cellW), 0, kNavCount - 1);
            }
            ImGui::PopID();
            float curp = Anim(ImGui::GetCurrentWindow()->GetID("navind_p"), (float)st.page, 16.0f);
            float ix = pa.x + (curp + 0.5f) * cellW;
            bg->AddRectFilledMultiColor(ImVec2(ix-16*s, y0), ImVec2(ix+16*s, y0+3*s),
                ui.pal.accent, ui.pal.accentHi, ui.pal.accentHi, ui.pal.accent);
            for (int i = 0; i < kNavCount; ++i) {
                float cxp = pa.x + (i+0.5f)*cellW;
                bool sel = (i == st.page);
                ImU32 col = sel ? ui.pal.accent : ui.pal.textFaint;
                Glyph(bg, ImVec2(cxp, y0 + navH*0.42f), i, col, 0.85f*s);
                ImVec2 tsz = Measure(ui, 10.5f*s, kNav[i].label);
                bg->AddText(ui.font, 10.5f*s, ImVec2(cxp - tsz.x*0.5f, y0 + navH - 16*s), col, kNav[i].label);
            }
        } else {
            // left sidebar
            float x0 = pa.x, x1 = pa.x + navW;
            bg->AddLine(ImVec2(x1, pa.y + headerH), ImVec2(x1, pb.y), ui.pal.stroke, 1.0f);
            float itemH = 50.0f * s;
            float startY = pa.y + headerH + 14*s;
            ImGui::SetCursorScreenPos(ImVec2(x0 + 12*s, startY));
            ImGui::PushID("navside");
            if (ImGui::InvisibleButton("##nav", ImVec2(navW - 24*s, itemH * kNavCount))) {
                float my = io.MousePos.y - startY;
                st.page = ImClamp((int)(my / itemH), 0, kNavCount - 1);
            }
            ImGui::PopID();
            float curp = Anim(ImGui::GetCurrentWindow()->GetID("navind_l"), (float)st.page, 16.0f);
            ImVec2 sa(x0 + 12*s, startY + curp*itemH + 4*s);
            ImVec2 sbb(x1 - 12*s, sa.y + itemH - 8*s);
            bg->AddRectFilled(sa, sbb, Fade(ui.pal.accent, 0.16f), 12*s);
            bg->AddRectFilled(sa, ImVec2(sa.x + 3.5f*s, sbb.y), ui.pal.accent, 2*s);
            for (int i = 0; i < kNavCount; ++i) {
                float iy = startY + i*itemH;
                bool sel = (i == st.page);
                ImU32 col = sel ? ui.pal.text : ui.pal.textDim;
                Glyph(bg, ImVec2(x0 + 30*s, iy + itemH*0.5f), i, sel?ui.pal.accent:ui.pal.textFaint, 0.9f*s);
                bg->AddText(ui.font, 14.5f*s, ImVec2(x0 + 48*s, iy + itemH*0.5f - 9*s), col, kNav[i].label);
            }
            // footer brand in sidebar
            bg->AddText(ui.font, 10.5f*s, ImVec2(x0 + 16*s, pb.y - 26*s), ui.pal.textFaint, "v4 \xe2\x80\xa2 mock ui");
        }
    }

    // ---- live preview (landscape only) -----------------------------------
    if (showPreview) {
        DrawPreview(ui, previewA, previewB, st);
    }

    // ---- page title above content ----------------------------------------
    {
        const char* subs[] = {
            "Overview & quick toggles", "Entity visual overlays",
            "Aim point controls (mock)", "HUD & environment", "Theme & interface" };
        bg->AddRectFilledMultiColor(ImVec2(bodyA.x + 6*s, bodyA.y + 8*s), ImVec2(bodyA.x + 10*s, bodyA.y + 40*s),
            ui.pal.accent, ui.pal.accent, ui.pal.accentHi, ui.pal.accentHi);
        bg->AddText(ui.font, 20.0f*s, ImVec2(bodyA.x + 20*s, bodyA.y + 8*s), ui.pal.text, kNav[st.page].label);
        bg->AddText(ui.font, 12.0f*s, ImVec2(bodyA.x + 20*s, bodyA.y + 32*s), ui.pal.textFaint, subs[st.page]);
    }
    float contentTop = bodyA.y + 52*s;

    // ---- scrollable content child ----------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(bodyA.x + 6*s, contentTop));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f*s);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(ui.pal.stroke));
    ImGui::BeginChild("##content", ImVec2(bodyB.x - bodyA.x - 12*s, bodyB.y - contentTop - 8*s), false,
        ImGuiWindowFlags_NoBackground);
    // rebind drawlist to the child so widgets draw in the right layer
    ui.dl = ImGui::GetWindowDrawList();

    // page transition fade
    ImGuiID pid = ImGui::GetCurrentWindow()->GetID("pagefade");
    ImGuiStorage* pst = ImGui::GetStateStorage();
    int lastPage = pst->GetInt(pid, -1);
    if (lastPage != st.page) { pst->SetInt(pid, st.page); pst->SetFloat(pid+1, 0.0f); }
    float appear = Anim(pid+1, 1.0f, 12.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.0f - appear) * 10.0f * s);

    switch (st.page) {
        case 0: PageDashboard(ui, st); break;
        case 1: PageVisuals(ui, st);   break;
        case 2: PageAim(ui, st);       break;
        case 3: PageWorld(ui, st);     break;
        default: PageSettings(ui, st); break;
    }
    ImGui::Dummy(ImVec2(0, 12*s));
    ImGui::EndChild();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ui.dl = bg;

    // ---- floating watermark (landscape only; header carries brand otherwise)
    if (st.show_watermark && !portrait) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const char* wm = "Eclips Oxide";
        ImVec2 wsz = Measure(ui, 12.0f*s, wm);
        ImVec2 wp(disp.x - wsz.x - 26*s, 14*s);
        fg->AddRectFilled(ImVec2(wp.x-10*s, wp.y-6*s), ImVec2(wp.x+wsz.x+10*s, wp.y+wsz.y+6*s), IM_COL32(12,14,20,180), 10*s);
        fg->AddRect(ImVec2(wp.x-10*s, wp.y-6*s), ImVec2(wp.x+wsz.x+10*s, wp.y+wsz.y+6*s), Fade(ui.pal.accent,0.6f), 10*s, 0, 1.0f);
        fg->AddText(ui.font, 12.0f*s, wp, ui.pal.text, wm);
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    } // end if (st.open)

    // ---- always-on toggle bar (top-left): FPS + clock, tap to open/close ---
    {
        time_t tt = time(nullptr);
        struct tm* lt = localtime(&tt);
        char tbuf[16];
        if (lt) snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
        else    snprintf(tbuf, sizeof(tbuf), "--:--:--");
        char fbuf[16]; snprintf(fbuf, sizeof(fbuf), "%.0f FPS", st.fps > 0 ? st.fps : io.Framerate);

        float barW = 190.0f * s;
        ImVec2 bp(margin, margin);
        ImGui::SetNextWindowPos(bp, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(barW, barH), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::Begin("##togglebar", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
        ImDrawList* d = ImGui::GetWindowDrawList();
        if (ImGui::InvisibleButton("##barbtn", ImVec2(barW, barH))) st.open = !st.open;
        bool hov = ImGui::IsItemHovered();

        ImVec2 a = bp, b(bp.x + barW, bp.y + barH);
        d->AddRectFilled(a, b, IM_COL32(18, 20, 28, 240), barH * 0.5f);
        d->AddRect(a, b, hov ? Fade(ui.pal.accent, 0.85f) : ui.pal.stroke, barH * 0.5f, 0, 1.3f);

        float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 3.0f);
        ImVec2 dotc(a.x + barH * 0.5f, a.y + barH * 0.5f);
        d->AddCircleFilled(dotc, 4.5f * s + pulse * 1.6f * s, Fade(ui.pal.good, 0.30f));
        d->AddCircleFilled(dotc, 3.4f * s, ui.pal.good);

        float tx = a.x + barH * 0.5f + 12 * s;
        d->AddText(ui.font, 13.0f * s, ImVec2(tx, a.y + barH * 0.5f - 8 * s), ui.pal.text, fbuf);
        ImVec2 fsz = Measure(ui, 13.0f * s, fbuf);
        float sepx = tx + fsz.x + 11 * s;
        d->AddLine(ImVec2(sepx, a.y + 9 * s), ImVec2(sepx, b.y - 9 * s), ui.pal.stroke, 1.0f);
        d->AddText(ui.font, 13.0f * s, ImVec2(sepx + 11 * s, a.y + barH * 0.5f - 8 * s), ui.pal.textDim, tbuf);

        ImVec2 chc(b.x - 16 * s, a.y + barH * 0.5f);
        ImU32 ccol = hov ? ui.pal.accent : ui.pal.textFaint;
        if (st.open) {
            d->AddLine(ImVec2(chc.x - 4 * s, chc.y + 2 * s), ImVec2(chc.x, chc.y - 2 * s), ccol, 1.9f * s);
            d->AddLine(ImVec2(chc.x, chc.y - 2 * s), ImVec2(chc.x + 4 * s, chc.y + 2 * s), ccol, 1.9f * s);
        } else {
            d->AddLine(ImVec2(chc.x - 4 * s, chc.y - 2 * s), ImVec2(chc.x, chc.y + 2 * s), ccol, 1.9f * s);
            d->AddLine(ImVec2(chc.x, chc.y + 2 * s), ImVec2(chc.x + 4 * s, chc.y - 2 * s), ccol, 1.9f * s);
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}
