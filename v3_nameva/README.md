# v3_nameva — offline-baked name-VA search

Резолв Il2CppClass* через **прямой поиск ссылки на имя класса** в libil2cpp writable memory,
без сканов анон-памяти игры (которые убивают процесс через watchdog CatsBit'а).

## Идея

1. **Offline** (из decrypted `global-metadata.dat`):
   - `PlayerManager` name string @ metadata + `0x1704f0`
   - `BuildingPiece` name string @ metadata + `0x10c22b`
   - `PlayerVitals` name string @ metadata + `0x169546`

2. **Runtime**:
   - Читаем `metadata_base = *(base + 0xbe3bb20)` — 1 vm_readv
   - Вычисляем `pm_name_va = metadata_base + 0x1704f0`
   - **1 bulk vm_readv** на libil2cpp writable (~5.6 МБ) — в локальный буфер
   - Локальный поиск слота содержащего `pm_name_va`
   - Найденный слот `- 0x10` = **Il2CppClass\* PlayerManager**

## Watchdog

Игра CatsBit'а убивает процесс через ~90с если чит делает **200 МБ** anon-memory скана
(старый подход через `ox_findAscii` / `ox_scanClassByName`). Этот подход делает
**1 vm_readv** на libil2cpp writable (мод либы игры), никакого скана heap/anon памяти.
Проверено на устройстве: игра живёт.

## Что ещё в этом коммите

- Все сканы (`ox_findAscii`, `ox_scanClassByName`, `ox_findTypeInfoRVA`, `ox_sweepAllTypeInfos`)
  ОТКЛЮЧЕНЫ. Никогда не вызываются в этом билде.
- MetadataCache-цепочка через `0xbe3bb10/0xbe3bb20/0xbe3bb28/0xbe3bb34` кэшируется на старте.
- Fast-seed повторяется каждые 3с максимум 600 попыток (30 мин) — покрывает кейс
  "чит запущен до входа в матч, Class::Init прогонит имя когда войду".
- Startup retry-loop 45×20с убит.
- Font.h — реальный OPPOSans_H (2.2 MB, кириллица через `AddFontFromMemoryTTF(Verdana)`
  во втором ImFont).

## Как собрать

```bash
cd v3_nameva
$NDK/ndk-build NDK_PROJECT_PATH=$PWD NDK_APPLICATION_MK=$PWD/Application.mk \
    APP_BUILD_SCRIPT=$PWD/Android.mk
```

Требует NDK r27+ и весь остальной source из `proj/` (main.h, memory.cpp, oxlog.cpp,
LuaIntegration.cpp, ImGui/, oxorany/, Android_draw/, Android_touch/, lua/lua-5.4.7/, и т.д.).
Этот коммит содержит только правки в main.cpp + оффсеты — остальные файлы берутся из
основной ветки `proj/`.
