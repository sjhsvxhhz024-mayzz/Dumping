#ifndef UTILS_H
#define UTILS_H

#include "imgui.h"
#include "Vector3.h"  // используем оригинальный Vector3

struct Matrix {
    float m11, m12, m13, m14;
    float m21, m22, m23, m24;
    float m31, m32, m33, m34;
    float m41, m42, m43, m44;
};

ImVec2 world2screen(Vector3 pos, Matrix m, bool* visible = nullptr);
float lerp(float a, float b, float f);

// --- Угловой World-to-Screen для Oxide (как в эталоне libesp) ---
// Не требует матрицы Unity: проецирует по позиции камеры + углам + FOV.
struct CameraView {
    Vector3 pos;      // мировая позиция камеры (глаз локального игрока)
    float   yaw;      // горизонтальный угол камеры (градусы) — fallback
    float   pitch;    // вертикальный угол камеры (градусы) — fallback
    float   fovH;     // горизонтальный FOV (градусы)
    bool    valid;    // удалось ли собрать данные камеры
    // Полный базис камеры из кватерниона взгляда (m_LookRoot). Если hasBasis,
    // W2S делает корректную перспективную проекцию в пространстве камеры
    // вместо раздельных yaw/pitch (которые точны только у центра экрана).
    bool    hasBasis = false;
    Vector3 right{1,0,0};
    Vector3 up{0,1,0};
    Vector3 forward{0,0,1};
};
// Возвращает экранные координаты; *visible=false если цель вне экрана / за спиной.
ImVec2 w2s_angular(Vector3 target, const CameraView& cam, bool* visible = nullptr);

// Если true — w2s_angular пишет ВСЕ промежуточные значения расчёта в лог
// (dx/dy/dz, углы, tan, проекцию). Ставится из main для первых целей.
extern bool w2sLogDetail;

template<typename T> 
inline T clamp(T value, T min, T max) {
    return value < min ? min : (value > max ? max : value);
}

#endif
