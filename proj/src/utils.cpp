#include "utils.h"
#include "oxlog.h"
#include <algorithm>
#include <math.h>

extern int abs_ScreenX, abs_ScreenY;

bool w2sLogDetail = false;

float lerp(float a, float b, float f) {
    return a + f * (b - a);
}

// Угловой W2S (порт из эталона libesp FUN_0010fec4).
// Координатная система Unity: x=вправо, y=вверх, z=вперёд.
ImVec2 w2s_angular(Vector3 target, const CameraView& cam, bool* visible) {
    if (visible) *visible = false;
    bool L = w2sLogDetail;

    if (!cam.valid) {
        if (L) OXLOGT("  W2S: камера невалидна");
        return ImVec2(-1.f, -1.f);
    }

    const float DEG2RAD = 0.01745329251994f;
    const float RAD2DEG = 57.2957795131f;

    float W = (float)abs_ScreenX;
    float H = (float)abs_ScreenY;
    if (W < 1.f || H < 1.f) return ImVec2(-1.f, -1.f);

    float dx = target.x - cam.pos.x;
    float dy = target.y - cam.pos.y;
    float dz = target.z - cam.pos.z;

    // --- КОРРЕКТНАЯ перспективная проекция через базис камеры ---
    // Раздельные yaw/pitch (ниже) точны только у центра экрана; полный базис
    // (right/up/forward) даёт правильные экранные координаты по всему кадру.
    if (cam.hasBasis) {
        Vector3 d(dx, dy, dz);
        float vz = d.x*cam.forward.x + d.y*cam.forward.y + d.z*cam.forward.z; // глубина
        if (vz <= 0.01f) {
            if (L) OXLOGT("  W2S(basis) -> за камерой vz=%.3f", vz);
            return ImVec2(-1.f, -1.f);
        }
        float vx = d.x*cam.right.x + d.y*cam.right.y + d.z*cam.right.z;       // вправо
        float vy = d.x*cam.up.x    + d.y*cam.up.y    + d.z*cam.up.z;          // вверх

        // cam.fovH — ГОРИЗОНТАЛЬНЫЙ FOV (как на слайдере). Вертикальный уже и
        // выводится через соотношение сторон: tanV = tanH * (H/W). На широком
        // экране это ключ к тому, чтобы боксы у КРАЁВ не уезжали.
        float fovHb = cam.fovH > 1.f ? cam.fovH : 60.f;   // горизонтальный
        float tanHb = tanf(fovHb * 0.5f * DEG2RAD);
        float tanVb = tanHb * (H / W);
        if (tanHb < 0.0001f) tanHb = 0.0001f;
        if (tanVb < 0.0001f) tanVb = 0.0001f;

        float x = W * 0.5f + (W * 0.5f) * (vx / vz) / tanHb;
        float y = H * 0.5f - (H * 0.5f) * (vy / vz) / tanVb;

        // Видимой считаем ТОЛЬКО цель в пределах экрана (+поле). Иначе боксы
        // врагов сбоку/сзади рисуются далеко за краями и «размазываются» по
        // экрану при повороте камеры (симптом «еспы едут за камерой»).
        const float M = 64.f;   // допуск, чтобы бокс у края не мигал
        bool onScreen = (x >= -M && x <= W + M && y >= -M && y <= H + M);
        if (L) OXLOGT("  W2S(basis) v=(%.2f,%.2f,%.2f) -> экран=(%.1f,%.1f) on=%d [fovH=%.1f]",
                      vx, vy, vz, x, y, (int)onScreen, fovHb);
        if (visible) *visible = onScreen;
        return ImVec2(x, y);
    }

    float distXZ = sqrtf(dx * dx + dz * dz);
    if (distXZ < 0.0001f) distXZ = 0.0001f;

    // Углы «от камеры на цель».
    float pitchTo = atan2f(dy, distXZ) * RAD2DEG;
    float yawTo   = atan2f(dx, dz) * RAD2DEG;

    // Разница по рысканью, нормализуем в [-180,180].
    float yawDelta = yawTo - cam.yaw;
    while (yawDelta > 180.f)  yawDelta -= 360.f;
    while (yawDelta < -180.f) yawDelta += 360.f;

    float pitchDelta = pitchTo - cam.pitch;

    if (L) {
        OXLOGT("  W2S target=(%.2f,%.2f,%.2f) cam=(%.2f,%.2f,%.2f)",
               target.x, target.y, target.z, cam.pos.x, cam.pos.y, cam.pos.z);
        OXLOGT("  W2S d=(%.2f,%.2f,%.2f) distXZ=%.2f", dx, dy, dz, distXZ);
        OXLOGT("  W2S yawTo=%.2f pitchTo=%.2f | camYaw=%.2f camPitch=%.2f",
               yawTo, pitchTo, cam.yaw, cam.pitch);
        OXLOGT("  W2S yawDelta=%.2f pitchDelta=%.2f", yawDelta, pitchDelta);
    }

    // За пределами полукруга обзора — цель за спиной.
    if (fabsf(yawDelta) >= 90.f) {
        if (L) OXLOGT("  W2S -> за спиной (|yawDelta|>=90)");
        return ImVec2(-1.f, -1.f);
    }

    float fovH = cam.fovH > 1.f ? cam.fovH : 60.f;
    float fovV = fovH * (H / W); // вертикальный FOV по соотношению сторон

    float tanH = tanf(fovH * 0.5f * DEG2RAD);
    float tanV = tanf(fovV * 0.5f * DEG2RAD);
    if (tanH < 0.0001f) tanH = 0.0001f;
    if (tanV < 0.0001f) tanV = 0.0001f;

    float x = W * 0.5f + (W * 0.5f) * (tanf(yawDelta * DEG2RAD) / tanH);
    float y = H * 0.5f - (H * 0.5f) * (tanf(pitchDelta * DEG2RAD) / tanV);

    if (L) OXLOGT("  W2S -> экран=(%.1f, %.1f) [W=%.0f H=%.0f fovH=%.1f fovV=%.1f]",
                  x, y, W, H, fovH, fovV);

    if (visible) *visible = true;
    return ImVec2(x, y);
}

// Явные инстанциации
template float clamp<float>(float, float, float);
template int clamp<int>(int, int, int);

ImVec2 world2screen(Vector3 pos, Matrix m, bool* visible) {
    float screenX = (m.m11 * pos.x) + (m.m21 * pos.y) + (m.m31 * pos.z) + m.m41;
    float screenY = (m.m12 * pos.x) + (m.m22 * pos.y) + (m.m32 * pos.z) + m.m42;
    float screenW = (m.m14 * pos.x) + (m.m24 * pos.y) + (m.m34 * pos.z) + m.m44;
 
    float camX = abs_ScreenX/2;
    float camY = abs_ScreenY/2;

    if (screenW <= 0.0001f) {
        if (visible) {
            *visible = false;
        }
        return ImVec2(0.0f, 0.0f);
    }

    float x = camX + (camX * screenX / screenW);
    float y = camY - (camY * screenY / screenW);

    if (visible) {
        *visible = true;
    }
    
    return ImVec2(x, y);
}
