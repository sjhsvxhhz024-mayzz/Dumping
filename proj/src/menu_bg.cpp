// Загрузка фоновой картинки меню в GL-текстуру (ленивая, один раз).
// Декодирование встроенного JPEG через stb_image. Вызывать только когда
// активен GL-контекст (во время кадра ImGui) — так и есть в Layout_tick_UI.
#include <GLES3/gl3.h>
#include <cstdint>

#include "imgui.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"

#include "menu_bg.h"

// Возвращает ImTextureID фоновой картинки (0 если не удалось). Размер — в out.
ImTextureID oxGetMenuBg(int* outW, int* outH) {
    static bool         tried = false;
    static ImTextureID  tex   = (ImTextureID)0;
    static int          W = 0, H = 0;

    if (!tried) {
        tried = true;
        int n = 0;
        unsigned char* px = stbi_load_from_memory(
            menu_bg_jpg, (int)menu_bg_jpg_len, &W, &H, &n, 4);
        if (px) {
            GLuint id = 0;
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, px);
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(px);
            tex = (ImTextureID)(intptr_t)id;
        }
    }

    if (outW) *outW = W;
    if (outH) *outH = H;
    return tex;
}
