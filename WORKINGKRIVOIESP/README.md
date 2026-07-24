# WORKINGKRIVOIESP — ESP появился (кривой но появился)

Первая версия где **ESP реально отрисовывается** в игре после месяцев борьбы с
anti-cheat watchdog'ом CatsBit'а и стухшими RVA.

## Что работает

- Игра НЕ вылетает от чита (watchdog обходится)
- `s_TypeInfoTable` резолвится через offline-найденный RVA `0xBE3BB58`
- `Il2CppClass* PlayerManager` находится за 3 vm_readv (детерминированно)
- ESP-боксы рисуются

## Что кривое (TODO в следующей ревизии)

1. **ESP боксы привязаны к камере**, не к игрокам. Двигаешь камеру — ESP двигается вместе с ней. Похоже читаем неверный position field (наверное worldCameraRoot вместо реального Transform игрока), или worldToScreen матрица берёт неправильный источник
2. **Счётчик игроков сверху** — показывает только тех, кто в frustum'е камеры. Надо 360°, всех игроков в матче

## Механика резолва (offline-verified через disasm)

Subagent прошёл libil2cpp.so через capstone ARM64 disasm и нашёл цепочку:

```
MetadataCache::Initialize @ RVA 0x4D765C8
  → Calloc'ит s_TypeInfoTable, кладёт указатель в BSS slot @ 0xBE3BB58

GetTypeInfoFromTypeDefinitionIndex @ RVA 0x4D773DC:
  0x4D77404: ldr x22, [x8, #0xb58]        ; x22 = *(0xBE3BB58) = s_TypeInfoTable
  0x4D77408: ldr x20, [x22, w0, sxtw #3]  ; klass = s_TypeInfoTable[TDI]
```

Runtime внешний резолв (external process, только чтение):

```c
Il2CppClass** table = *(Il2CppClass***)(il2cpp_base + 0xBE3BB58);
Il2CppClass*  pm    = table[8251];   // TDI PlayerManager
```

3 vm_readv на класс. Watchdog не убивает.

## Ключевые константы (offline-baked)

```
S_TYPEINFO_TABLE_RVA         = 0xBE3BB58   (BSS slot, populated by Init)
TDI_PLAYERMANAGER            = 8251
TDI_BUILDINGPIECE            = 8521
TDI_PLAYERVITALS             = 7923
META_NAME_OFF_PLAYERMANAGER  = 0x1704f0    (для name verification)
META_NAME_OFF_BUILDINGPIECE  = 0x10c22b
META_NAME_OFF_PLAYERVITALS   = 0x169546
```

Все проверены на dump'е `libil2cpp.so` (md5 7ebaf7428502ba9bf8ebed6da533fe50).

## Как собрать

Нужен NDK r27+, все source из `proj/` (main.h, memory.cpp, oxlog.cpp, LuaIntegration.cpp,
ImGui/, oxorany/, Android_draw/, Android_touch/, lua/lua-5.4.7/, Font.h).

```bash
cd WORKINGKRIVOIESP
$NDK/ndk-build NDK_PROJECT_PATH=$PWD NDK_APPLICATION_MK=$PWD/Application.mk \
    APP_BUILD_SCRIPT=$PWD/Android.mk
```

Этот коммит содержит только `main.cpp` + `oxide_offsets.h` + mk-файлы. Остальные
модули берутся из основной ветки `proj/`.
