# Freeze fix v2 — статус плана «1..5» (2026-07-24)

Сводка по итогам ревью репо. Основной код (`proj/src/*`, `proj/include/*`)
этот PR НЕ трогает — только дописывает утилиту и заметки.

## Что уже в `main` (сделано ранее)

### 1. Startup freeze в `main.cpp` — исправлен
- [`435cd35`](../../commit/435cd352c60def08c053e4f0d41a0e0e1b0d2446)
  «async startup + runtime TypeInfo sweep»: диагностика и
  `ox_autoResolveTypeInfos` вынесены в фоновый `std::thread`, UI поднимается
  с первого кадра, cooperative deadline 90 с в `ox_scanClassByName` /
  `ox_findClassByName` / `ox_findTypeInfoRVA` / `ox_findAscii`,
  `UpdatePlayerCache` пропускает проход пока стартап работает.
- [`514104a`](../../commit/514104a1871d1b2963c3a882867f45c711c08d2f)
  «memmem-based ascii sweep + retry-loop»: `ox_findAscii` перешёл на
  `memmem()` (~200 MB/сек вместо 8), 4 MB чанки, retry-loop 45 × 20 с,
  cooperative check `main_thread_flag` в `sleep` для чистого выхода.

### 4. Инструменты — часть уже есть
- `proj/tools/decrypt_metadata.py` — расшифровка `global-metadata.dat`
  XOR-ключом `0xA5C3F19D` в диапазоне `[0x08, 0x17C)`, metadata v39,
  ожидаемый magic `0xFAB11BAF`.
- `Nov/runtime_probe.py` — парсит `[sweep]` / `[AUTO]` / `[scan] FOUND`
  строки runtime-лога и печатает готовый патч для `oxide_offsets.h`
  (`PLAYERMANAGER_TYPEINFO`, `BUILDINGPIECE_TYPEINFO`, `PLAYERVITALS_TYPEINFO`).

## Что добавляет этот PR

- **`Nov/reassemble_split_zip.sh`** — сборка split-архива `Н.z01..Н.zip`,
  лежащего в корне репо (10 томов × 5 МиБ + последний том с центральным
  каталогом, всего ≈ 55 МБ). Обходит кириллическое имя через копирование
  во временный каталог с латинскими именами, потом
  `zip -s 0 N.zip --out full.zip` → `unzip`, проверяет ELF-magic
  `libil2cpp.so` и magic `0xFAB11BAF` у `global-metadata.dat`.
  ```bash
  ./Nov/reassemble_split_zip.sh                          # src = repo root, out = ./unpacked
  ./Nov/reassemble_split_zip.sh /path/to/src /path/to/out
  ```
- **Этот файл** — сводка статуса + ссылки на follow-up issues.

## Что осталось (открыто отдельными issue)

### 2. Обновить TypeInfo RVA в `oxide_offsets.h` — ждём runtime-лог
Значения `PLAYERMANAGER_TYPEINFO`, `BUILDINGPIECE_TYPEINFO`,
`PLAYERVITALS_TYPEINFO` в шапке помечены `[STALE — резолвится в рантайме]`
и в бою всё равно перекрываются `ox_autoResolveTypeInfo`. Обновление нужно
только для «холодного» fallback до того, как фон подхватит новые адреса.
Запустить `Nov/runtime_probe.py` на снятом in-game логе, применить
выведенный патч, закоммитить.

### 3. MouseLook pitch source — Vector2 kqP vs Quaternion LookRoot
В `oxide_offsets.h` живой наклон читается как `float ML_PITCH = 0x60`
(это `Vector2 kqP.x`). Альтернатива — разобрать `Quaternion localRotation`
у `Transform` по `ML_LOOKROOT = 0x28`. На дальних целях по вертикали ESP
у нескольких сессий вело; нужен in-game лог с обоими значениями бок о бок,
чтобы выбрать источник. Правка — в `ox_readLookBasis`
(`eclips_oxide_menu.cpp` / `main.cpp`), в этот PR не входит: без сборки
и runtime-верификации не пушу.

## Проверка

`Nov/reassemble_split_zip.sh` — bash-скрипт, ничего в C++/Android коде
не меняет. `FREEZE_FIX_V2_NOTES.md` — маркдаун-документ. `ndk-build`
на ветке `main` и на этой ветке даёт идентичный артефакт.
