#ifndef OXLOG_H
#define OXLOG_H

// ============================================================================
//  Eclips Oxide — детальный файловый логгер.
//  Пишет в /sdcard/Download/eclips_oxide_<timestamp>.log (+ дублирует в stdout).
//  Задача: максимально подробная трассировка резолва памяти, камеры и W2S,
//  чтобы диагностировать оффсеты и математику без доступа к устройству.
// ============================================================================

#include <cstdint>
#include <cstddef>

namespace oxlog {

// Инициализация: открывает файл лога. Вызывать один раз в начале main().
// Возвращает путь к созданному файлу (для вывода в stdout).
const char* init();

// Каталог, в который пишутся все артефакты (лог, dump метадаты и т.п.).
// Заполняется в init() первым каталогом из CANDIDATE_DIRS, куда удалось
// открыть файл. Возвращает пустую строку до init().
const char* activeDir();

// Закрыть лог (flush + close). Обычно не требуется — flush идёт после каждой строки.
void shutdown();

// Форматированная строка с таймстампом и уровнем.
//   level: 'I' info, 'W' warn, 'E' error, 'D' debug, 'T' trace
void logf(char level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Заголовок секции (визуальный разделитель в логе).
void section(const char* title);

// Hex-дамп произвольного буфера (сырые байты памяти игры).
//   addr — виртуальный адрес в процессе игры (для подписи), buf — локальная копия.
void hexdump(const char* label, uint64_t addr, const void* buf, size_t size);

// Логировать один шаг ptr-chain: "label: [addr] -> value".
void ptrstep(const char* label, uint64_t addr, uint64_t value);

// Управление детализацией.
extern bool verbose;       // подробный режим (hex-дампы, каждый шаг цепочки)
extern bool enabled;       // включён ли лог вообще
extern int  frameCounter;  // номер текущего кадра обновления кэша

// Сколько кадров логировать в verbose-режиме подробно, потом — реже.
// (первые кадры пишем всё, дальше — только сводку, чтобы файл не разросся).
bool shouldLogFull();

// Включить подробный лог ещё на N кадров начиная с текущего.
// Вызывается при важных событиях (найден список игроков, вход в матч).
void armFull(int frames);

} // namespace oxlog

// Удобные макросы.
#define OXLOGI(...) oxlog::logf('I', __VA_ARGS__)
#define OXLOGW(...) oxlog::logf('W', __VA_ARGS__)
#define OXLOGE(...) oxlog::logf('E', __VA_ARGS__)
#define OXLOGD(...) oxlog::logf('D', __VA_ARGS__)
#define OXLOGT(...) oxlog::logf('T', __VA_ARGS__)

#endif // OXLOG_H
