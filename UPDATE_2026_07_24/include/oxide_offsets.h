#ifndef OXIDE_OFFSETS_H
#define OXIDE_OFFSETS_H
#include "oxide_rva.h"

// ============================================================================
//  Oxide Survival Island — оффсеты (Il2CppDumper, дамп OXIDEarm64 2026/07/23)
//  Источник: dump.cs / il2cpp.h / script.json (com.catsbit build).
//  Движок Unity il2cpp, сеть Mirror (NetworkBehaviour).
//
//  ВСЕ field-оффсеты ниже сверены с дампом 2026/07/23. Исправлено относительно
//  прошлой версии заголовка:
//   * RaycastManager — прошлый «-0x8 сдвиг» был ОШИБОЧНЫМ; реальные оффсеты
//     m_WorldCamera=0x30, m_RayLength=0x38, m_AimRayLength=0x3C, m_LayerMask=0x48,
//     m_AimLayerMask=0x4C, + новое поле player=0x20.
//   * BuildingPiece — health/maxHealth/id съехали на 0x380/0x384/0x38C (было
//     0x2C0/0x2C4/0x2C8), m_Bounds=0xD0, additionalBounds=0xF8, m_Grade=0x190,
//     gradeHolder=0x198.
//   * PlayerManager.teamName = 0x280 (было ошибочно 0x270).
//
//  TypeDefIndex (UPDATE 2026/07/24): PlayerManager 8357, RaycastManager 8155,
//  PlayerVitals 8030, EntityVitals 8019, GenericVitals 8021, MouseLook 8009,
//  BuildingPiece 8633, Camera 13810. (Прошлый апдейт: 8251/8048/7923/7912/7914/7902/8521.)
//  ВСЕ field-оффсеты в этом апдейте НЕ ИЗМЕНИЛИСЬ (проверено сравнением il2cpp.h old vs new,
//  раскладка byte-identical — обфускатор лишь пере-рандомизировал имена полей).
//
//  Иерархия виталов: PlayerVitals : EntityVitals : GenericVitals — поэтому
//  GenericVitals.m_MaxHealth @0x88 читается и через указатель PlayerVitals.
//
//  === TypeInfo — новая схема (offline + verified через typeinfo_offline.py) ===
//  Резолвим статически по metadataRegistration->types + верификация имён
//  в global-metadata.dat. Все VERIFY OK на дампе 2026/07/24 (libil2cpp.so
//  + global-metadata.dat из split-zip в корне репо). Больше не сканируем
//  анон-память на старте — резолв за миллисекунды.
//
//  Резолв при старте (одним вызовом на класс):
//    Il2CppType** g = (Il2CppType**)((uint8_t*)il2cpp_base + ox::IL2CPP_TYPES_RVA);
//    Il2CppClass* k = il2cpp_class_from_il2cpp_type(g[ox::PLAYERMANAGER_TYPEINFO_TYPEIDX]);
//
//  Fallback — name-scan из main.cpp (ox_autoResolveTypeInfo) остаётся, но
//  теперь только для сборок, где types-array не подтверждает канонический тип.
//
//  Старые *_TYPEINFO (RVA) сохранены ниже как резервный путь.
// ============================================================================

namespace ox {

// --- offline TypeInfo (metadataRegistration->types indexing, verified) ---
// Regenerated offline (static + name-verified) on UPDATE dump 2026/07/24 против
// NEW libil2cpp.so (md5 b3a79ff1dcd1f8d97ba237525440f90d) + global-metadata.dat
// (md5 52208a1bbb506ab72936c04bfea63f08). Type-indexes = byvalTypeIndex из каждого
// Il2CppTypeDefinition (+0x08); каждый проверен types[idx].klassIndex==TDI, канонический
// (byref=0,pinned=0), CLASS. Пересобирать при каждом апдейте libil2cpp.so.
//
// ПРИМЕЧАНИЕ (апдейт 2026/07/24): metadata header теперь XOR-зашифрован ключом
// 0xA5C3F19D; таблицы найдены после расшифровки. types-array сместился
// 0xB63EB48 -> 0xB77C750 (104482 -> 104776). Все field-оффсеты НЕ изменились
// (обфускатор лишь пере-рандомизировал имена полей, раскладка byte-identical).
static constexpr uint64_t IL2CPP_TYPES_RVA               = 0xB77C750; // 104776 entries (was 0xB63EB48/104482)
static constexpr int32_t  PLAYERMANAGER_TYPEINFO_TYPEIDX  = 60998;   // Oxide.PlayerManager (TDI 8357)   VERIFY OK (was 60939/TDI 8251)
static constexpr int32_t  BUILDINGPIECE_TYPEINFO_TYPEIDX  = 46457;   // Oxide.Building.BuildingPiece (TDI 8633)   VERIFY OK (was 46390/TDI 8521)
static constexpr int32_t  PLAYERVITALS_TYPEINFO_TYPEIDX   = 61062;   // Oxide.PlayerVitals (TDI 8030)   VERIFY OK (was 61003/TDI 7923)
static constexpr int32_t  RAYCASTMANAGER_TYPEINFO_TYPEIDX = 62232;   // Oxide.RaycastManager (TDI 8155)   VERIFY OK (was 62162/TDI 8048)
static constexpr int32_t  MOUSELOOK_TYPEINFO_TYPEIDX      = 58788;   // Oxide.MouseLook (TDI 8009)   VERIFY OK (was 58695/TDI 7902)
static constexpr int32_t  CAMERA_TYPEINFO_TYPEIDX         = 46796;   // UnityEngine.Camera (TDI 13810)   VERIFY OK (was 39710/TDI 13698)
static constexpr int32_t  ENTITYVITALS_TYPEINFO_TYPEIDX   = 50770;   // Oxide.EntityVitals (TDI 8019)   VERIFY OK (was 50639/TDI 7912)
static constexpr int32_t  GENERICVITALS_TYPEINFO_TYPEIDX  = 52632;   // Oxide.GenericVitals (TDI 8021)   VERIFY OK (was 52479/TDI 7914)

// --- Пакет и модуль ---
static constexpr const char *PACKAGE = "com.catsbit.oxidesurvivalisland";
static constexpr const char *MODULE  = "libil2cpp.so";

// --- Схема B (нативная, из декомпил. libesp.so) ---
// Альтернативный путь: корневой указатель прямо в игровой нативной либе.
static constexpr const char *MODULE_NATIVE = "liboxidesurvivalisland.so";
static constexpr uint64_t NATIVE_ROOT_PTR  = 0xef800; // base + 0xef800 -> ptr на список
static constexpr uint64_t NATIVE_ENTITY_STRIDE = 0x18; // шаг между entity в списке
static constexpr uint64_t NATIVE_TRANSFORM_BACK = 0x100; // entity - 0x100 -> матрица 0x40

// === TypeInfos из script.json (ScriptMetadata) ===
// RVA отсчитываются ОТ БАЗЫ libil2cpp.so (реальная база = r-xp кластер).
// СТАРЫЕ RVA — в рантайме резолвятся через ox_autoResolveTypeInfo (fallback).
// [STALE RVA — резолвится в рантайме]. В il2cpp v39 глобальная таблица metadataUsages
// ПУСТА (metadataRegistration.metadataUsages count=0), поэтому стабильного статического
// слота Il2CppClass* per-class НЕТ — это и есть причина рантайм-резолва. Значения ниже
// от прошлого билда (TDI 8251/8521/7923), только seed для ОТКЛЮЧЁННОГО scan-fallback;
// перезаписываются рантаймом. Не dereferences в активном пути (fast-seed).
static constexpr uint64_t PLAYERMANAGER_TYPEINFO  = 0xB8C1AE8; // Oxide.PlayerManager_TypeInfo (prev-build TDI 8251) [STALE]
static constexpr uint64_t BUILDINGPIECE_TYPEINFO  = 0xB8B97E0; // Oxide.Building.BuildingPiece_TypeInfo (prev-build TDI 8521) [STALE]
static constexpr uint64_t PLAYERVITALS_TYPEINFO   = 0xB8C1B48; // Oxide.PlayerVitals_TypeInfo (prev-build TDI 7923) [STALE]

// --- il2cpp статическая цепочка до списка игроков ---
// base + PLAYERMANAGER_TYPEINFO -> Il2CppClass*
//   klass + CLASS_STATIC_FIELDS -> static_fields*
//   statics + STATIC_ACTIVE_LIST -> List<PlayerManager> activePlayerList
// CLASS_STATIC_FIELDS = 0xB8 — ПОДТВЕРЖДЕНО offline disasm BuildingPiece.cctor:
//   ldr x8,[classptr] ; ldr x8,[x8,#0xb8] (static_fields) ; str x19,[x8] (saveList).
static constexpr uint64_t CLASS_STATIC_FIELDS    = 0xB8;      // Il2CppClass.static_fields (VERIFY OK)
static constexpr uint64_t STATIC_ACTIVE_LIST     = 0x8;       // activePlayerList (уточняется авто-сканом)
// ── BuildingPiece static collections (offline-verified через .cctor disasm) ──
//   static_fields + 0x0 -> HashSet<BuildingPiece> saveList   (str x19,[x8])
//   static_fields + 0x8 -> Dictionary<int,BuildingPiece> saveLookup (str x19,[x0,#8])
// КРИТИЧНО: saveList — это HashSet<T>, НЕ List<T>! Старый код читал его как
// List (items@0x10,size@0x18) -> всегда мусор/пусто -> LOS не работал вообще.
// dump.cs:296932  public static HashSet<Oxide.Building.BuildingPiece> saveList; // 0x0
// dump.cs:296933  public static Dictionary<int,Oxide.Building.BuildingPiece> saveLookup; // 0x8
static constexpr uint64_t STATIC_BUILDING_SAVELIST   = 0x0;   // HashSet<BuildingPiece>
static constexpr uint64_t STATIC_BUILDING_SAVELOOKUP = 0x8;   // Dictionary<int,BuildingPiece>

// --- Il2CppClass ---
static constexpr uint64_t CLASS_NAME      = 0x10; // const char* name
static constexpr uint64_t CLASS_NAMESPACE = 0x18; // const char* namespaze

// --- generic List<T> ---
static constexpr uint64_t LIST_ITEMS = 0x10; // T[] _items
static constexpr uint64_t LIST_SIZE  = 0x18; // int _size

// --- Il2CppArray ---
static constexpr uint64_t ARRAY_COUNT = 0x18; // il2cpp_array_size_t max_length
static constexpr uint64_t ARRAY_DATA  = 0x20; // начало элементов

// --- Mono System.Collections.Generic.HashSet<T> (managed ref-object layout) ---
// Резолвится в этом билде как Mono/классический HashSet (Slot[]+_lastIndex),
// НЕ .NET Core (Entry[]+_freeList head). Подтверждено il2cpp.h полями
// { _buckets,_slots,_count,_lastIndex,_freeList,_comparer,_version,_siInfo }
// и disasm (HashSet.Add читает _count @ +0x20). 64-bit il2cpp object header = 0x10.
//   +0x00 Il2CppObject.klass, +0x08 monitor
static constexpr uint64_t HASHSET_SLOTS      = 0x18; // Slot[] _slots (Il2CppArray*)
static constexpr uint64_t HASHSET_COUNT      = 0x20; // int _count  (живых элементов)  VERIFY OK (disasm)
static constexpr uint64_t HASHSET_LASTINDEX  = 0x24; // int _lastIndex (верхняя граница занятых слотов)
// Slot value-type в массиве: { int hashCode; int next; T value } -> stride 0x10 (ref T),
// value @ slot+0x8. Занятый слот: hashCode >= 0 (свободные помечены < 0).
static constexpr uint64_t HASHSET_SLOT_STRIDE   = 0x10;
static constexpr uint64_t HASHSET_SLOT_HASHCODE = 0x0;
static constexpr uint64_t HASHSET_SLOT_VALUE    = 0x8;

// --- Mono System.Collections.Generic.Dictionary<TKey,TValue> (fallback source) ---
// saveLookup = Dictionary<int,BuildingPiece>. Entry { int hashCode; int next;
// TKey key; TValue value }. Для <int, ref>: hashCode@0x0, next@0x4, key@0x8,
// value@0x10 -> stride 0x18. entries @ Dictionary+0x18, count @ +0x28.
static constexpr uint64_t DICT_ENTRIES        = 0x18; // Entry[] entries
static constexpr uint64_t DICT_COUNT          = 0x28; // int count (верхняя граница)
static constexpr uint64_t DICT_ENTRY_STRIDE   = 0x18; // {int,int,int,ptr} align8
static constexpr uint64_t DICT_ENTRY_HASHCODE = 0x0;
static constexpr uint64_t DICT_ENTRY_VALUE    = 0x10; // BuildingPiece* (ключ int @0x8)

// --- Авто-скан статиков ---
static constexpr uint64_t STATIC_SCAN_MAX = 0x400; // сколько байт static_fields сканировать
static constexpr int      LIST_COUNT_MAX  = 100;   // разумный максимум игроков в списке

// --- PlayerManager instance (сверено с dump.cs 2026/07/23, TypeDefIndex 8251) ---
static constexpr uint64_t PM_TRANSFORM         = 0x68;  // Transform worldCameraRoot (держит YAW камеры)
// Прямая позиция игрока в мире: server-authoritative анти-чит поле.
// Работает для ВСЕХ игроков (local + remote). worldCameraRoot есть только у
// локального — remote игроки его не имеют, отсюда баг "ESP крепится к камере".
// Vector3 @ 0x1C8 (12 байт: float x,y,z).
static constexpr uint64_t PM_LAST_TICK_POS     = 0x1C8;
static constexpr uint64_t PM_LAST_SAVED_POS    = 0x1D4;
static constexpr uint64_t PM_CHARACTER_MODEL   = 0x150; // GameObject* characterModel
static constexpr uint64_t PM_MOUSELOOK         = 0x70;  // Oxide.MouseLook mouseLook (VERIFY OK il2cpp.h)
static constexpr uint64_t PM_FPMANAGER         = 0x90;  // Oxide.FPManager fpManager (VERIFY OK) — живой FOV + ADS
static constexpr uint64_t PM_RAYCASTMANAGER    = 0x88;  // Oxide.RaycastManager raycastManager
static constexpr uint64_t PM_VITALS            = 0xC8;  // Oxide.PlayerVitals vitals
static constexpr uint64_t PM_LOOK_ANGLE        = 0x1A8; // float lookAngle (диагностика)
// Прямого флага isLocalPlayer НЕТ. 0x188 = bool <NpC>k__BackingField, 0x190 = Animator*.
// Локал-детект — сравнением с observedPlayer (0x310) или глобальным локальным игроком.
static constexpr uint64_t PM_IS_LOCAL_PLAYER   = 0x188; // [НЕТ прямого флага] bool k__BackingField — НЕнадёжно
static constexpr uint64_t PM_OBSERVED_PLAYER   = 0x310; // PlayerManager observedPlayer (для локал-детекта)
static constexpr uint64_t PM_ANIMATOR          = 0x190; // Animator animator
// team-объект @0x120 (WNn), строковое имя команды teamName @0x280.
// teamName (0x280) — Mirror SyncVar [string], реплицируется НА ВСЕХ клиентов для
// ВСЕХ игроков -> самый надёжный кросс-игроковый идентификатор команды.
// Подтверждено il2cpp.h/dump.cs: PlayerManager.teamName // 0x280 (+ SyncVarHook @0x370).
static constexpr uint64_t PM_TEAM             = 0x120; // WNn team (компонент команды)
static constexpr uint64_t PM_TEAM_ID           = 0x120; // (алиас, историч.)
static constexpr uint64_t PM_TEAM_NAME         = 0x280; // string teamName (SyncVar) — VERIFY OK
static constexpr uint64_t PM_USER_ID           = 0x278; // string userID (SyncVar) — уникальный id игрока
// --- WNn (team component) -> pf (team data) --- для corroborating team-check.
//   WNn.team (pf*) @ 0x90 ; pf.mYs (string teamId) @ 0x10 ; pf.mYo (string) @ 0x18
// pf — ШАРЕННЫЙ объект данных команды; у сокомандников совпадает pf.mYs (teamId).
static constexpr uint64_t WNN_TEAM_DATA        = 0x90;  // pf team (data object)
static constexpr uint64_t PF_TEAM_ID_STR       = 0x10;  // string mYs (team id)

// --- MouseLook (Oxide.MouseLook, TypeDefIndex 7902) ---
// ============================================================================
//  ГЛАВНЫЙ ФИКС ДРЕЙФА ESP (bug #1/#2/#3).
//  Offline disasm MouseLook.uae (0x52B0AD4) на erafox libil2cpp (byte-identical
//  игре), Unity 6000.3.18f1 — ГРУНТ-ТРУС того, что реально хранится в полях:
//
//   * m_LookRoot @0x28 — Transform (PITCH-пивот, ребёнок yaw-пивота). Его
//     localRotation КАЖДЫЙ кадр ставится игрой как:
//        m_LookRoot.localRotation = Quaternion.Euler( kqP.x°, 0, roll )
//     (disasm: ldr s9,[x19,#0x60]; ...Internal_FromEulerRad; set_localRotation).
//     Значит kqP.x (0x60) = ПИТЧ В ГРАДУСАХ (+вниз). Yaw на m_LookRoot = 0.
//   * kqP.y (0x64) = YAW В ГРАДУСАХ (аккумулятор, применяется к телу через
//     WPr.GTv). Совпадает с конвенцией, которую пишет аимбот
//     (yaw=atan2(dx,dz), pitch=-atan2(dy,horiz)) — проверено численно.
//
//  ПОЧЕМУ РАНЬШЕ ДРЕЙФИЛО: ESP-базис строился из МИРОВОГО кватерниона
//  m_LookRoot, читаемого через НАТИВНЫЙ Transform (matrixData+0xA0). Но:
//    (a) matrixData читался по internal+0x28, а 0x28 в Unity Transform_internal
//        = `self` (back-ptr), НЕ mData (mData@0x38) — читался мусор;
//    (b) даже с верным 0x38 нативный world-кватернион лежит в libunity
//        (icall get_rotation_Injected) — оффсет непроверяем из libil2cpp.
//  РЕШЕНИЕ: базис камеры строим ИЗ УГЛОВ kqP.x/kqP.y (managed float'ы,
//  100% проверяемо, без libunity). См. ox_readLookBasis в main.cpp.
//
//  Для aimbot-ПИСЬМА в 0x60/0x64 (движок сам строит из них поворот) — как было.
// ============================================================================
static constexpr uint64_t ML_ANGLES_BACKING = 0x90; // Vector2 _kqm_k__BackingField (accum look, вторичн.)
static constexpr uint64_t ML_ANGLES   = 0x90; // (алиас)
static constexpr uint64_t ML_LOOKROOT = 0x28; // Transform m_LookRoot (PITCH-пивот; localRot=Euler(kqP.x,0,roll))
static constexpr uint64_t ML_PITCH    = 0x60; // kqP.x — PITCH В ГРАДУСАХ (+вниз). read(ESP)+write(aim). VERIFY OK
static constexpr uint64_t ML_YAW      = 0x64; // kqP.y — YAW В ГРАДУСАХ. read(ESP)+write(aim). VERIFY OK
static constexpr uint64_t ML_SENSITIVITY = 0x34; // float m_Sensitivity
static constexpr uint64_t ML_INVERT      = 0x30; // bool m_Invert
static constexpr uint64_t ML_DEFAULT_LOOK_LIMITS = 0x38; // Vector2 (min,max pitch) standing
static constexpr uint64_t ML_CROUCH_LOOK_LIMITS  = 0x40; // Vector2 (min,max pitch) crouched

// --- Oxide.FPManager (PM+0x90) — живой FOV камеры + ADS-детект ---
// ============================================================================
//  Offline disasm FPManager.LateUpdate (0x52DAE88) + jWU (0x52DAA2C):
//    kYl  @0xA0 — интерполированный ТЕКУЩИЙ ВЕРТИКАЛЬНЫЙ FOV (движок пишет его
//                 в Camera.set_fieldOfView каждый LateUpdate). VERIFY OK.
//    kSP  @0xB4 — FOV-оффсет (отдача/дыхание). Эффективный FOV = kYl - kSP.
//    _kSQ @0xAC — БАЗОВЫЙ верт. FOV (из PlayerPrefs, с коррекцией на aspect).
//  Итог: реальный вертикальный FOV камеры = kYl(0xA0) - kSP(0xB4).
//  ADS-детект: при прицеле kYl падает ниже _kSQ (зум) -> aiming = kYl < _kSQ*τ.
//  Это заменяет ФИКСИРОВАННЫЙ camFov-слайдер (тот игнорировал реальный и
//  меняющийся при прицеле FOV -> боксы уезжали к краям / грубо врали в прицеле).
// ============================================================================
static constexpr uint64_t FP_WORLD_CAMERA   = 0x20; // UnityEngine.Camera m_WorldCamera
static constexpr uint64_t FP_CURRENT_WEAPON  = 0x50; // Oxide.FPObject _currentWeapon
static constexpr uint64_t FP_FOV_CURRENT     = 0xA0; // float kYl — интерполир. текущий верт. FOV  VERIFY OK
static constexpr uint64_t FP_FOV_BASE        = 0xAC; // float _kSQ — базовый верт. FOV             VERIFY OK
static constexpr uint64_t FP_FOV_OFFSET      = 0xB4; // float kSP — FOV-оффсет (recoil)            VERIFY OK
// FPObject (_currentWeapon) — для доп. ADS-инфо (статический конфиг оружия).
static constexpr uint64_t FPOBJ_NORMAL_FOV   = 0x80; // int normalFOV
static constexpr uint64_t FPOBJ_AIM_FOV      = 0x84; // int aimFOV
// Порог ADS: aiming, если текущий FOV меньше базового в этот раз.
static constexpr float    ADS_FOV_RATIO      = 0.90f; // kYl < _kSQ*0.90 => прицеливается
static constexpr float    EYE_FOV_MIN        = 5.0f;  // санитарный минимум верт. FOV
static constexpr float    EYE_FOV_MAX        = 160.0f;// санитарный максимум верт. FOV

// --- RaycastManager (TypeDefIndex 8048) ---
// PlayerManager+0x88. Собственный слой raycast'ов прицела.
// dump.cs 2026/07/23: player @0x20, teamManager @0x28, дальше камера и маски.
// Прошлый «-0x8 сдвиг» был ОШИБОЧНЫМ — реальные оффсеты ниже.
static constexpr uint64_t RM_PLAYER         = 0x20; // PlayerManager player (новое поле)
static constexpr uint64_t RM_WORLD_CAMERA   = 0x30; // Camera m_WorldCamera (FIX: было 0x28)
static constexpr uint64_t RM_RAY_LENGTH     = 0x38; // float m_RayLength (FIX: было 0x30)
static constexpr uint64_t RM_AIM_RAY_LENGTH = 0x3C; // float m_AimRayLength (FIX: было 0x34)
static constexpr uint64_t RM_LAYER_MASK     = 0x48; // LayerMask m_LayerMask (FIX: было 0x40)
static constexpr uint64_t RM_AIM_LAYER_MASK = 0x4C; // LayerMask m_AimLayerMask (FIX: было 0x44)

// --- PlayerVitals / GenericVitals base ---
// Иерархия: PlayerVitals : EntityVitals : GenericVitals. m_MaxHealth @0x88 (GenericVitals)
// читается и через указатель PlayerVitals. Отдельного поля «текущее HP» нет;
// для alive-чека (>0) хватает.
static constexpr uint64_t VITALS_HEALTH = 0x88; // float m_MaxHealth (GenericVitals, подтв.)

// --- UnityEngine.Transform -> нативная цепочка (стандарт il2cpp) ---
// managed Transform + 0x10 -> m_CachedPtr (нативный Transform*)
static constexpr uint64_t OBJ_CACHED_PTR      = 0x10;

// --- ПРЯМАЯ мировая позиция (нативные оффсеты движка, НЕ из dump.cs) ---
// нативный Transform хранит указатель на TRS-данные:
//   internal + 0x28 -> matrixData
//   matrixData + 0x90 position, +0xA0 rotation (quat), +0xB0 scale
// forward = quatRotate(rot, (0,0,1)). Эти оффсеты Unity-внутренние, сверяются
// рантайм-дампом (PDUMP), а не dump.cs.
static constexpr uint64_t TR_INT_MATRIXPTR    = 0x28;
static constexpr uint64_t TR_MATRIX_WORLDPOS  = 0x90;
static constexpr uint64_t TR_MATRIX_ROT       = 0xA0;
static constexpr uint64_t TR_MATRIX_SCALE     = 0xB0;

static constexpr uint64_t TR_INT_HIERARCHY    = 0x38;
static constexpr uint64_t TR_INT_INDEX        = 0x40;
static constexpr uint64_t TR_HIER_LOCALDATA   = 0x18;
static constexpr uint64_t TR_HIER_PARENTIDX   = 0x20;
static constexpr uint64_t TR_DATA_STRIDE      = 0x30; // Quaternion(0x10)+Vec3(0x0C)+Vec3(0x0C)->pad 0x30
static constexpr uint64_t TR_DATA_LOCALROT    = 0x00; // Quaternion (x,y,z,w)
static constexpr uint64_t TR_DATA_LOCALPOS    = 0x10; // Vector3
static constexpr uint64_t TR_DATA_LOCALSCALE  = 0x1C; // Vector3
static constexpr int      TR_MAX_PARENTS      = 64;   // защита от циклов

// --- Валидация HP «живой сущности» (как в эталоне libesp) ---
static constexpr float HP_MIN = 1.0f;
static constexpr float HP_MAX = 200.0f;

// --- Камера / W2S ---
// Unity 6000.3.18f1: Camera.fieldOfView = ВЕРТИКАЛЬНЫЙ FOV. Игру читаем НАПРЯМУЮ
// (FPManager.kYl - kSP -> реальный верт. FOV), поэтому W2S теперь работает в
// ВЕРТИКАЛЬНОМ FOV (tanV напрямую; tanH = tanV*(W/H)). Базис камеры строится из
// углов kqP.x(pitch)/kqP.y(yaw) — оба managed, проверяемы, без libunity icall'ов.
static constexpr float EYE_HEIGHT   = 1.6f;   // высота глаз над ногами игрока (стоя)
static constexpr float PLAYER_TOP   = 1.85f;  // макушка (для верхней точки бокса)
static constexpr float DEFAULT_VFOV = 60.0f;  // верт. FOV (град) fallback, если FPManager недоступен
static constexpr float DEFAULT_FOV  = 60.0f;  // (истор. алиас — верт. fallback)
// ox_readPos возвращает FEET position (lastTickPosition = server-authoritative).
// Бокс строится ВВЕРХ от pos: низ = pos (ноги), верх = pos + PLAYER_TOP (макушка).
static constexpr float BOX_FOOT     = 0.0f;   // низ бокса = pos (ноги)
static constexpr float BOX_HEAD     = 1.85f;  // верх бокса = pos + BOX_HEAD (макушка)

// ============================================================================
//  BuildingPiece — для LOS-чек (аимбот не наводится через стены)
// ============================================================================
//  Oxide.Building.BuildingPiece : Mirror.NetworkBehaviour (TypeDefIndex 8521)
//
//  m_Bounds (0xD0) — Bounds в ЛОКАЛЬНЫХ координатах префаба. Мировую position
//  берём через m_CorrectPosition (0x8C). World-bounds = m_CorrectPosition +
//  m_Bounds.center ± m_Bounds.extents.
//  additionalBounds (0xF8) — доп. Bounds[] (для составных построек).
//  m_MeshCollider (0x130) / m_Colliders (0x170) — настоящие коллайдеры (на потом).
//  saveList (static 0x0) — HashSet<BuildingPiece> ВСЕХ построек (НЕ List!).
//  saveLookup (static 0x8) — Dictionary<int,BuildingPiece> (fallback-источник).
//  BuildingPiece.OnStartServer вызывает saveList.Add(this) (offline disasm
//  0x53fa314: bl HashSet.Add). Клиент читает saveList/saveLookup в десятках
//  методов (decay/neighbor/grade) -> коллекции существуют и на клиенте.
// ВСЕ оффсеты ниже сверены с il2cpp.h/dump.cs (Unity 6000.3.18f1, erafox dump).
static constexpr uint64_t BP_PIECE_NAME         = 0x80;  // string m_PieceName (подтв.)
static constexpr uint64_t BP_CORRECT_POSITION  = 0x8C;  // Vector3 m_CorrectPosition (подтв.)
static constexpr uint64_t BP_CORRECT_ROTATION   = 0x98;  // Vector3 m_CorrectRotation (подтв.)
static constexpr uint64_t BP_BOUNDS             = 0xD0;  // Bounds m_Bounds { center; extents } (FIX: было 0xC0)
static constexpr uint64_t BP_ADDITIONAL_BOUNDS  = 0xF8;  // Bounds[] additionalBounds (FIX: было 0xD8)
static constexpr uint64_t BP_M_GRADE             = 0x190; // BuildingGrade.Enum m_Grade (FIX: было 0x150)
static constexpr uint64_t BP_GRADE_HOLDER        = 0x198; // Transform gradeHolder (FIX: было 0x158)
static constexpr uint64_t BP_HEALTH             = 0x380; // float health (>0 = стоит) (FIX: было 0x2C0)
static constexpr uint64_t BP_MAXHEALTH          = 0x384; // float maxHealth (FIX: было 0x2C4)
static constexpr uint64_t BP_ID                 = 0x38C; // int id (FIX: было 0x2C8)

// Bounds struct (UnityEngine): m_Center, m_Extents. min = center-extents, max = center+extents
static constexpr uint64_t BOUNDS_CENTER  = 0x0;
static constexpr uint64_t BOUNDS_EXTENTS = 0xC;

// LOS-чек настройки
static constexpr float    LOS_AABB_INFLATE   = 0.30f;  // расширим AABB на 30см — conservatism
static constexpr int      LOS_SAVELIST_MAX   = 2000;   // верхнее ограничение (защита от мусора)
static constexpr float    LOS_TARGET_EPS     = 1.0f;   // 1 м — чтобы не ложно-позитивить у цели

} // namespace ox

#endif // OXIDE_OFFSETS_H
