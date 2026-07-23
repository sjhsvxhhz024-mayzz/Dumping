#include "oxlog.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <android/log.h>

// Тег для logcat: `adb logcat -s EclipsOxide`
#define OXLOG_TAG "EclipsOxide"

// Маппинг наших уровней ('I','W','E','D','T') в приоритеты Android logcat.
static int ox_androidPrio(char level) {
    switch (level) {
        case 'E': return ANDROID_LOG_ERROR;
        case 'W': return ANDROID_LOG_WARN;
        case 'D': return ANDROID_LOG_DEBUG;
        case 'T': return ANDROID_LOG_VERBOSE;
        case 'I':
        default:  return ANDROID_LOG_INFO;
    }
}

namespace oxlog {

// По умолчанию ВЫКЛЮЧЕНО: запись в файл каждый кадр давала лаги.
// Включается вручную в меню (Settings → Logging), только для диагностики.
bool verbose = false;
// Startup failures happen before the menu can expose logging controls.
// Keep diagnostics enabled so users receive a reason instead of a blank log.
bool enabled = true;
int  frameCounter = 0;

static FILE*      g_file = nullptr;
static char       g_path[256] = {0};
static const int  FULL_LOG_FRAMES = 30; // первые N кадров пишем максимально подробно
static int        g_armUntil = 0;       // до какого кадра форсить подробный лог

// Возможные каталоги для лога (первый доступный на запись).
static const char* CANDIDATE_DIRS[] = {
    "/sdcard/Download",
    "/storage/emulated/0/Download",
    "/sdcard",
    "/data/local/tmp",
};

// Текущее время в "YYYY-MM-DD HH:MM:SS.mmm".
static void timestamp(char* out, size_t n) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    snprintf(out, n, "%02d:%02d:%02d.%03d",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(tv.tv_usec / 1000));
}

const char* init() {
    if (g_file) return g_path;

    // Имя файла с датой-временем старта.
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char fname[64];
    snprintf(fname, sizeof(fname), "eclips_oxide_%04d%02d%02d_%02d%02d%02d.log",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    for (const char* dir : CANDIDATE_DIRS) {
        // пытаемся создать каталог (может уже быть)
        mkdir(dir, 0777);
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", dir, fname);
        FILE* f = fopen(full, "w");
        if (f) {
            g_file = f;
            snprintf(g_path, sizeof(g_path), "%s", full);
            break;
        }
    }

    if (g_file) {
        fprintf(g_file,
            "==============================================================\n"
            " Eclips Oxide detailed log\n"
            " build: %s %s\n"
            "==============================================================\n\n",
            __DATE__, __TIME__);
        fflush(g_file);
        printf("[oxlog] лог пишется в: %s\n", g_path);
        __android_log_print(ANDROID_LOG_INFO, OXLOG_TAG, "log file: %s", g_path);
    } else {
        enabled = false;
        printf("[oxlog] НЕ удалось открыть файл лога ни в одном каталоге!\n");
    }
    return g_path;
}

void shutdown() {
    if (g_file) {
        fflush(g_file);
        fclose(g_file);
        g_file = nullptr;
    }
}

void logf(char level, const char* fmt, ...) {
    if (!enabled) return;

    char ts[24];
    timestamp(ts, sizeof(ts));

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    // stdout (для su-стрима)
    printf("%c %s\n", level, msg);
    fflush(stdout);

    // logcat (adb logcat -s EclipsOxide) — «консоль» на устройстве
    __android_log_write(ox_androidPrio(level), OXLOG_TAG, msg);

    if (g_file) {
        fprintf(g_file, "[%s][%c][f%d] %s\n", ts, level, frameCounter, msg);
        fflush(g_file);
    }
}

void section(const char* title) {
    if (!enabled) return;
    if (g_file) {
        fprintf(g_file,
            "\n----------------------------------------------------------------\n"
            "  %s\n"
            "----------------------------------------------------------------\n",
            title);
        fflush(g_file);
    }
    printf("=== %s ===\n", title);
    fflush(stdout);
    __android_log_print(ANDROID_LOG_INFO, OXLOG_TAG, "==== %s ====", title);
}

void hexdump(const char* label, uint64_t addr, const void* buf, size_t size) {
    if (!enabled || !g_file) return;
    const unsigned char* p = (const unsigned char*)buf;

    fprintf(g_file, "  HEX %s @ 0x%llx (%zu bytes):\n", label,
            (unsigned long long)addr, size);
    for (size_t i = 0; i < size; i += 16) {
        fprintf(g_file, "    +0x%03zx: ", i);
        // hex-байты
        for (size_t j = 0; j < 16; j++) {
            if (i + j < size) fprintf(g_file, "%02x ", p[i + j]);
            else              fprintf(g_file, "   ");
        }
        // ascii
        fprintf(g_file, " | ");
        for (size_t j = 0; j < 16 && i + j < size; j++) {
            unsigned char c = p[i + j];
            fprintf(g_file, "%c", (c >= 32 && c < 127) ? c : '.');
        }
        fprintf(g_file, "\n");
    }
    // интерпретации: как uint64/float для первых 32 байт
    fprintf(g_file, "    as u64: ");
    for (size_t i = 0; i + 8 <= size && i < 32; i += 8)
        fprintf(g_file, "0x%llx ", (unsigned long long)*(const uint64_t*)(p + i));
    fprintf(g_file, "\n    as f32: ");
    for (size_t i = 0; i + 4 <= size && i < 32; i += 4)
        fprintf(g_file, "%.3f ", *(const float*)(p + i));
    fprintf(g_file, "\n");
    fflush(g_file);
}

void ptrstep(const char* label, uint64_t addr, uint64_t value) {
    if (!enabled) return;
    if (g_file) {
        fprintf(g_file, "  CHAIN %-24s [0x%llx] -> 0x%llx\n",
                label, (unsigned long long)addr, (unsigned long long)value);
        fflush(g_file);
    }
}

bool shouldLogFull() {
    return verbose && (frameCounter < FULL_LOG_FRAMES || frameCounter < g_armUntil);
}

void armFull(int frames) {
    int until = frameCounter + frames;
    if (until > g_armUntil) g_armUntil = until;
}

} // namespace oxlog
