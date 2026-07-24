# FULLWORK — рабочая версия чита

Полностью рабочий Eclips Oxide для Oxide Survival Island. Все известные баги
подтверждены починенными offline reverse-engineering'ом.

## Что работает

- **s_TypeInfoTable резолв** через RVA `0xBE3BB58` (offline-verified через ARM64
  disasm `MetadataCache::Initialize` @ `0x4D765C8` и `GetTypeInfoFromTypeDefinitionIndex`
  @ `0x4D773DC`)
- **Позиции игроков** через `PM.lastTickPosition` @ `0x1C8` (server-authoritative
  Vector3, работает для ВСЕХ игроков — local и remote)
- **Wall check (LOS)**: `BuildingPiece.saveList` = **`HashSet<T>`** (не List!),
  итерация через Mono HashSet layout (slots@0x18, count@0x20, stride 0x10,
  value@+0x8). Плюс учёт rotation стен (`m_CorrectRotation@0x98`) в AABB и
  target-точка на chest/head, не в ноги (иначе фундамент блокирует луч)
- **Hide teammates**: чтение через raw UTF-16 (не через `String::Get()` который
  дропал кириллицу ≥0x80), fallback на `pf.teamId`
- **Anti-drift**: базис камеры = ТОЛЬКО нормализованный `m_LookRoot` кватернион.
  Ранее добавлялся отдельный pitch с `MouseLook+0x60` — disasm показал что это
  аккумулированный input, движок сам его применяет, у нас было двойное применение
- **360° player counter**: `g_enemyCount = cached_players.size()` (все живые,
  не только те что в frustum'е)
- **Anti-cheat obfuscation bypass**: не сканируем анон-память игры, все резолвы
  через фиксированные offset'ы + минимальное количество vm_readv (~13 на резолв)

## Что НЕ работает (честно)

**Автоматическое определение приседания** — offline нерешаемо. `PlayerFlags@0x250`
не содержит crouch-бит, `Animator.GetBool()` требует in-process call. Компромисс:
ручной тоггл "Crouch-safe aim" в меню с настраиваемой Y-дельтой.

## Ключевые offline-найденные оффсеты

```
s_TypeInfoTable slot @ RVA 0xBE3BB58 (BSS, Init'ит Calloc'ом)
MetadataCache*       @ RVA 0xBE3BB10
metadata_base*       @ RVA 0xBE3BB20
Il2CppGlobalMetadataHeader* @ RVA 0xBE3BB28

TDI PlayerManager = 8251, byval = 60939
TDI BuildingPiece = 8521, byval = 46390
TDI PlayerVitals  = 7923, byval = 61003

PM.worldCameraRoot   @ 0x68 (Transform, только для local)
PM.mouseLook          @ 0x70
PM.animator           @ 0x190
PM.lastTickPosition   @ 0x1C8 (Vector3, server-authoritative)
PM.lastSavedPosition  @ 0x1D4 (Vector3)
PM.teamName           @ 0x280 (string, UTF-16 raw)
PM.userID             @ 0x278 (string)

BuildingPiece.saveList (static, +0x0)   = HashSet<T>
BuildingPiece.saveLookup (static, +0x8) = Dictionary<int,T>

CLASS_STATIC_FIELDS = 0xB8
CLASS_NAME          = 0x10
CLASS_NAMESPACE     = 0x18
```

## Как собрать

Нужен NDK r27+, все source из `proj/` (main.h, memory.cpp, oxlog.cpp,
LuaIntegration.cpp, ImGui/, oxorany/, Android_draw/, Android_touch/, lua/lua-5.4.7/,
Font.h).

```bash
cd FULLWORK
$NDK/ndk-build NDK_PROJECT_PATH=$PWD NDK_APPLICATION_MK=$PWD/Application.mk \
    APP_BUILD_SCRIPT=$PWD/Android.mk
```

Этот коммит содержит main.cpp + oxide_offsets.h + mk-файлы + README. Остальные
модули берутся из основной ветки `proj/`.

## Как запускать на устройстве

```bash
adb push eclipsoxide /data/local/tmp/
adb shell "su -c 'chmod +x /data/local/tmp/eclipsoxide && /data/local/tmp/eclipsoxide &'"
```

Запускать ПОСЛЕ входа в игру (класс PM резолвится через lazy init s_TypeInfoTable,
при первой сцене с игроком). При запуске из главного меню — retry-loop 30 минут
пока не поймает.
