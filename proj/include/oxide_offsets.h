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
//  TypeDefIndex (новые): PlayerManager 8251, RaycastManager 8048,
//  PlayerVitals 7923, EntityVitals 7912, GenericVitals 7914, MouseLook 7902,
//  BuildingPiece 8521.
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
// Автосгенерённо typeinfo_offline.py на дампе 2026/07/24. Пересобирать при
// каждом апдейте libil2cpp.so — типовые индексы съезжают.
static constexpr uint64_t IL2CPP_TYPES_RVA               = 0xB63EB48; // 104482 entries
static constexpr int32_t  PLAYERMANAGER_TYPEINFO_TYPEIDX  = 60939;   // Oxide.PlayerManager (TDI 8251)   VERIFY OK
static constexpr int32_t  BUILDINGPIECE_TYPEINFO_TYPEIDX  = 39693;   // Oxide.Building.BuildingPiece (TDI 8521)   VERIFY OK
static constexpr int32_t  PLAYERVITALS_TYPEINFO_TYPEIDX   = 61003;   // Oxide.PlayerVitals (TDI 7923)   VERIFY OK
static constexpr int32_t  RAYCASTMANAGER_TYPEINFO_TYPEIDX = 62162;   // Oxide.RaycastManager (TDI 8048)   VERIFY OK
static constexpr int32_t  MOUSELOOK_TYPEINFO_TYPEIDX      = 58695;   // Oxide.MouseLook (TDI 7902)   VERIFY OK
static constexpr int32_t  CAMERA_TYPEINFO_TYPEIDX         = 39710;   // UnityEngine.Camera (TDI 13698)   VERIFY OK
static constexpr int32_t  ENTITYVITALS_TYPEINFO_TYPEIDX   = 50639;   // Oxide.EntityVitals (TDI 7912)   VERIFY OK
static constexpr int32_t  GENERICVITALS_TYPEINFO_TYPEIDX  = 52479;   // Oxide.GenericVitals (TDI 7914)   VERIFY OK

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
static constexpr uint64_t PLAYERMANAGER_TYPEINFO  = 0xB8C1AE8; // Oxide.PlayerManager_TypeInfo (TypeDefIndex 8251) [STALE RVA — резолвится в рантайме]
static constexpr uint64_t BUILDINGPIECE_TYPEINFO  = 0xB8B97E0; // Oxide.Building.BuildingPiece_TypeInfo (8521) [STALE RVA — резолвится в рантайме]
static constexpr uint64_t PLAYERVITALS_TYPEINFO   = 0xB8C1B48; // Oxide.PlayerVitals_TypeInfo (7923) [STALE RVA — резолвится в рантайме]

// --- il2cpp статическая цепочка до списка игроков ---
// base + PLAYERMANAGER_TYPEINFO -> Il2CppClass*
//   klass + CLASS_STATIC_FIELDS -> static_fields*
//   statics + STATIC_ACTIVE_LIST -> List<PlayerManager> activePlayerList
static constexpr uint64_t CLASS_STATIC_FIELDS    = 0xB8;      // Il2CppClass.static_fields
static constexpr uint64_t STATIC_ACTIVE_LIST     = 0x8;       // activePlayerList (уточняется авто-сканом)
// buildingpiece.saveList хранится в static_fields по офсету 0x0
static constexpr uint64_t STATIC_BUILDING_SAVELIST = 0x0;

// --- Il2CppClass ---
static constexpr uint64_t CLASS_NAME      = 0x10; // const char* name
static constexpr uint64_t CLASS_NAMESPACE = 0x18; // const char* namespaze

// --- generic List<T> ---
static constexpr uint64_t LIST_ITEMS = 0x10; // T[] _items
static constexpr uint64_t LIST_SIZE  = 0x18; // int _size

// --- Il2CppArray ---
static constexpr uint64_t ARRAY_COUNT = 0x18; // il2cpp_array_size_t max_length
static constexpr uint64_t ARRAY_DATA  = 0x20; // начало элементов

// --- Авто-скан статиков ---
static constexpr uint64_t STATIC_SCAN_MAX = 0x400; // сколько байт static_fields сканировать
static constexpr int      LIST_COUNT_MAX  = 100;   // разумный максимум игроков в списке

// --- PlayerManager instance (сверено с dump.cs 2026/07/23, TypeDefIndex 8251) ---
static constexpr uint64_t PM_TRANSFORM         = 0x68;  // Transform worldCameraRoot (держит YAW камеры)
static constexpr uint64_t PM_MOUSELOOK         = 0x70;  // Oxide.MouseLook mouseLook
static constexpr uint64_t PM_RAYCASTMANAGER    = 0x88;  // Oxide.RaycastManager raycastManager
static constexpr uint64_t PM_VITALS            = 0xC8;  // Oxide.PlayerVitals vitals
// Прямого флага isLocalPlayer НЕТ. 0x188 = bool <NpC>k__BackingField, 0x190 = Animator*.
// Локал-детект — сравнением с observedPlayer (0x310) или глобальным локальным игроком.
static constexpr uint64_t PM_IS_LOCAL_PLAYER   = 0x188; // [НЕТ прямого флага] bool k__BackingField — НЕнадёжно
static constexpr uint64_t PM_OBSERVED_PLAYER   = 0x310; // PlayerManager observedPlayer (для локал-детекта)
static constexpr uint64_t PM_ANIMATOR          = 0x190; // Animator animator
// team-объект @0x120 (WNn), строковое имя команды teamName @0x280.
static constexpr uint64_t PM_TEAM_ID           = 0x120; // WNn team (объект, не int) — для сравнения команд
static constexpr uint64_t PM_TEAM_NAME         = 0x280; // string teamName (FIX: было ошибочно 0x270)
static constexpr uint64_t PM_USER_ID           = 0x278; // string userID

// --- MouseLook (Oxide.MouseLook, TypeDefIndex 7902) ---
// В этом билде m_LookRoot (0x28) — yaw-пивот; живые углы в Vector2 @0x60 (kqP).
static constexpr uint64_t ML_ANGLES   = 0x90; // Vector2 <kqm>k__BackingField (не исп. напрямую)
static constexpr uint64_t ML_LOOKROOT = 0x28; // Transform m_LookRoot (yaw-пивот)
static constexpr uint64_t ML_PITCH    = 0x60; // float pitch (Vector2.x @0x60), клампы ±80
static constexpr uint64_t ML_YAW      = 0x64; // float yaw   (Vector2.y @0x64)

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
static constexpr float EYE_HEIGHT   = 1.6f;   // высота глаз над позицией игрока
static constexpr float PLAYER_TOP   = 1.8f;   // макушка (для верхней точки бокса)
static constexpr float DEFAULT_FOV  = 60.0f;  // горизонтальный FOV (град), тюнится в меню
static constexpr float BOX_FOOT     = 1.6f;   // низ бокса = pos - BOX_FOOT (ноги)
static constexpr float BOX_HEAD     = 0.25f;  // верх бокса = pos + BOX_HEAD (над макушкой)

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
//  saveList (static 0x0) — List<BuildingPiece> всех построек на сервере.
// ВСЕ оффсеты ниже сверены с dump.cs 2026/07/23.
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
