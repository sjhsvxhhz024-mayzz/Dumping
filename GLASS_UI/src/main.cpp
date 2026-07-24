// NDK r27+ hardcodes -Werror=format-security at the end of the clang command
// line, after all user flags. oxorany() returns const char* (not a literal), so
// ImGui::TextColored(..., oxorany("...")) trips -Wformat-security. The original
// build was done with an older NDK that didn't force this. Pragma has priority
// over command-line -Werror, so we silence it here.
#pragma clang diagnostic ignored "-Wformat-security"
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#pragma clang diagnostic ignored "-Wformat"

#include "main.h"
#include "LuaIntegration.h"
#include "memory.h"
#include "utils.h"
#include "oxide_offsets.h"
#include "oxlog.h"
#include "oxorany/oxorany.hpp"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <malloc.h>
#include <math.h>
#include <thread>
#include <iostream>
#include <algorithm>
#include <sys/stat.h>
#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <iostream>
#include <locale>
#include <string>
#include <time.h>
#include <codecvt>
#include <dlfcn.h>
#include "Vector2.h"
#include "../include/Vector3.h"
#include "Quaternion.h"
#include <sstream>
#include <mutex>
#include <vector>
#include <atomic>
#include <chrono>

bool main_thread_flag = true;

// ============================================================================
//  Startup pipeline (async).
//  Startup diagnostics + TypeInfo auto-resolve сканируют десятки/сотни МБ памяти
//  процесса игры и раньше вызывались синхронно в main() ДО входа в цикл рендера.
//  На защищённом билде Oxide это могло длиться минуты — meню за это время
//  вообще не открывалось (erafox видел зависший лог сразу после
//  "TypeInfo fallback из хедера: PM=... BP=... PV=...").
//  Новая схема: initGUI_draw() → touch → whileLoop СРАЗУ, а диагностика/резолв
//  идут в фоновом std::thread. UpdatePlayerCache/ox_getStatics пропускают
//  проход, пока фон не закончил, — меню отрисовывается с первого кадра, а
//  в углу видно строку статуса.
// ============================================================================
static std::atomic<bool> g_startupThreadStarted{false};
static std::atomic<bool> g_startupDone{false};
static std::atomic<bool> g_startupInProgress{false};
static std::mutex        g_startupStatusMutex;
static std::string       g_startupStatus = "\xd0\xbe\xd0\xb6\xd0\xb8\xd0\xb4\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb5"; // "ожидание"

static void ox_setStartupStatus(const std::string &s) {
    std::lock_guard<std::mutex> l(g_startupStatusMutex);
    g_startupStatus = s;
}
std::string ox_getStartupStatus() {
    std::lock_guard<std::mutex> l(g_startupStatusMutex);
    return g_startupStatus;
}

// Deadline для тяжёлых сканов памяти. Возвращает true если пора закруглиться.
// Проверяется вручную внутри цикла-сканера, чтобы не подвешивать UI.
static std::atomic<uint64_t> g_scanDeadlineMs{0};
static bool ox_scanTimedOut() {
    uint64_t d = g_scanDeadlineMs.load(std::memory_order_relaxed);
    if (!d) return false;
    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return now >= d;
}
static void ox_setScanDeadline(uint64_t seconds) {
    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    g_scanDeadlineMs.store(now + seconds * 1000, std::memory_order_relaxed);
}
static void ox_clearScanDeadline() { g_scanDeadlineMs.store(0, std::memory_order_relaxed); }

int abs_ScreenX = 0;
int abs_ScreenY = 0;
int game_pid = -1;
uint64_t il2cpp_base = 0;

// ============================================================================
//  Startup-scan governance.
//
//  OX_TRUST_HEADER_TYPEINFO (default 1):
//    Верим значениям PLAYERMANAGER_TYPEINFO / BUILDINGPIECE_TYPEINFO /
//    PLAYERVITALS_TYPEINFO из oxide_offsets.h. При старте пробуем seed из
//    хедера (одна дереф + проверка имени класса) — если сходится, klass'ы
//    заполнены за миллисекунды, весь скан-путь пропускается: нет sweep-а
//    менеджеров, нет 45×20с retry, нет 200 MB/с memmem-прогрева. Если seed
//    не сходится (RVA протух, версия сменилась) — падаем на прежний
//    scan-based резолв через ox_autoResolveTypeInfos.
//
//  OX_DEEP_DIAG (default 0):
//    Полный sweep всех известных TypeInfo в конце startup-диагностики.
//    Нужен только когда обновляется libil2cpp.so и надо вытащить свежие
//    RVA для oxide_offsets.h — обычному пользователю это лаг без пользы.
// ============================================================================
#ifndef OX_TRUST_HEADER_TYPEINFO
#define OX_TRUST_HEADER_TYPEINFO 1
#endif
#ifndef OX_DEEP_DIAG
#define OX_DEEP_DIAG 0
#endif

// Типовые RVA из последнего dump.cs — fallback. Runtime resolver может заменить
// их на найденные в запущенной версии игры без пересборки после обновления.
static uint64_t g_playerManagerTypeInfoRVA = ox::PLAYERMANAGER_TYPEINFO;
static uint64_t g_buildingPieceTypeInfoRVA = ox::BUILDINGPIECE_TYPEINFO;
static bool g_typeInfoAutoResolved = false;
// Внешний чит НЕ может звать il2cpp_class_from_name (она в процессе игры).
// Поэтому кэшируем САМ указатель Il2CppClass, найденный сканом памяти по имени.
// Этого достаточно: klass + CLASS_STATIC_FIELDS -> static_fields напрямую,
// TypeInfo RVA (который на metadata v39 нестабилен) больше не нужен.
static uint64_t g_playerManagerKlass = 0;
static uint64_t g_buildingPieceKlass = 0;

// структура для кэширования данных игрока
struct PlayerCacheData {
    uint64_t address;
    Vector3 position;
    ImVec2 w2sTop;
    ImVec2 w2sBottom;
    int health;
    int armor;
    int teamId;
    float distance;
    std::string name;
    std::string weapon;
    std::string teamName;
    bool isVisibleTop;
    bool isVisibleBottom;
    // LOS по BuildingPiece.saveList: true если путь от камеры до точки игрока
    // не перекрыт стеной. Пересчитывается каждый кадр в ESP-секции (дёшево на
    // кэше построек). aimbot при aimCheckWalls пропускает цели с losBlocked=true.
    bool losBlocked;
    uint64_t viewPtr;
    uint64_t bipedMap;
    bool hasSkeletonData;
    Vector3 velocity;
    Vector3 sampledPosition;
    double sampleTime;
    
    // данные для скелета
    Vector3 headPos;
    Vector3 neckPos;
    Vector3 spine1Pos;
    Vector3 spine2Pos;
    Vector3 rshoulderPos;
    Vector3 lshoulderPos;
    Vector3 ruparmPos;
    Vector3 luparmPos;
    Vector3 rlowarmPos;
    Vector3 llowarmPos;
};

// глобальные переменные для кэширования
std::mutex player_data_mutex;
std::vector<PlayerCacheData> cached_players;
bool cache_needs_update = true;
int last_cache_frame = 0;
// Тяжёлый скан памяти (поиск игроков, HP, список) — раз в N кадров.
// Позиция КАЖДОГО игрока + камера пересчитываются КАЖДЫЙ кадр в отрисовке
// (ox_readPos по address), поэтому боксы держатся идеально на бегу и при
// повороте камеры без отставания — тяжёлый скан на это больше не влияет.
const int CACHE_UPDATE_INTERVAL = 3;
// Адрес локального игрока (для покадрового пересчёта камеры вне тяжёлого скана).
uint64_t g_localPlayer = 0;
std::string g_localTeamName;

int getProcessID(const char *packageName)
{
	DIR *dir = opendir("/proc");
	if (!dir) return -1;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL)
	{
		int id = atoi(entry->d_name);
		if (id > 0)
		{
			char filename[64];
			snprintf(filename, sizeof(filename), "/proc/%d/cmdline", id);
			FILE *fp = fopen(filename, "r");
			if (fp)
			{
				char cmdline[256] = {0};
				size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, fp);
				fclose(fp);
				if (n > 0 && strcmp(packageName, cmdline) == 0)
				{
					closedir(dir);
					return id;
				}
			}
		}
	}
	closedir(dir);
	return -1;
}

// Проверяет, есть ли в /proc/<pid>/maps строка с подстроко�� module.
static bool pidHasModule(int pid, const char *module) {
	char path[64]; sprintf(path, "/proc/%d/maps", pid);
	FILE* f = fopen(path, "rt");
	if (!f) return false;
	char line[1024];
	bool found = false;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, module)) { found = true; break; }
	}
	fclose(f);
	return found;
}

// cmdline процесса начинается с packageName? (обрезает \0-разделённые аргументы).
static bool pidCmdlineMatches(int pid, const char *packageName) {
	char path[64]; sprintf(path, "/proc/%d/cmdline", pid);
	FILE* f = fopen(path, "r");
	if (!f) return false;
	char cmd[128] = {0};
	size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
	fclose(f);
	if (n == 0) return false;
	cmd[n] = 0;
	// главный процесс: cmdline РОВНО == packageName (сервисы имеют суффикс ":name")
	return strcmp(cmd, packageName) == 0;
}

// Находит ГЛАВНЫЙ процесс игры: среди всех с совпадающим cmdline выбирает тот,
// у которого реально замаплен module (рендер-процесс), иначе — любой совпавший.
// preferModule=nullptr → вернуть первый совпавший (как раньше).
int getGameProcessID(const char *packageName, const char *preferModule) {
	DIR *dir = opendir("/proc");
	if (!dir) return -1;
	struct dirent *entry;
	int firstMatch = -1;
	int best = -1;
	while ((entry = readdir(dir)) != NULL) {
		int id = atoi(entry->d_name);
		if (id <= 0) continue;
		if (!pidCmdlineMatches(id, packageName)) continue;
		if (firstMatch < 0) firstMatch = id;
		if (preferModule && pidHasModule(id, preferModule)) { best = id; break; }
	}
	closedir(dir);
	if (best > 0) return best;
	return firstMatch;
}

uint64_t get_module_base(const char *module_name, int mod) {
	uint64_t returned = 0;
	char path[32];
	sprintf(path, "/proc/%d/maps\0", game_pid);
	char line[1024];
	FILE* maps = fopen(path, "rt");
	if (!maps) return 0;
	while(fgets(line, 1024, maps)) {
		if(strstr(line, module_name) && strstr(line, (mod == 1 ? "r--p" : "r-xp"))) {
			uint64_t start,end;
			sscanf(line, "%lx-%lx",&start,&end);
			returned = start;
			break;
		}
  }
  fclose(maps);
  return returned;
}

// Возвращает АДРЕС САМОГО НИЗКОГО маппинга модуля (простой fallback).
uint64_t get_module_lowest(const char *module_name) {
	uint64_t lowest = 0;
	char path[64];
	sprintf(path, "/proc/%d/maps", game_pid);
	FILE* maps = fopen(path, "rt");
	if (!maps) return 0;
	char line[1024];
	while (fgets(line, sizeof(line), maps)) {
		if (!strstr(line, module_name)) continue;
		uint64_t start = 0, end = 0;
		if (sscanf(line, "%lx-%lx", &start, &end) == 2 && start) {
			if (!lowest || start < lowest) lowest = start;
		}
	}
	fclose(maps);
	return lowest;
}

// ИСТИННАЯ база ELF для RVA. В maps модуль может иметь несколько несвязанных
// кластеров (стороннее mmap файла + реальная загрузка линкером). Реальный —
// тот, где есть исполняемый r-xp сегмент. Берём его файловый offset, вычисляем
// candidate = xp_start - xp_offset, и выбираем offset-0 маппинг того же кластера
// (ближайший к candidate). Так отсекаем «левый» read-only mmap того же файла.
uint64_t get_module_base_real(const char *module_name) {
	char path[64];
	sprintf(path, "/proc/%d/maps", game_pid);
	FILE* maps = fopen(path, "rt");
	if (!maps) return 0;
	char line[1024];

	uint64_t xpStart = 0, xpOff = 0;
	// первый проход: найти исполняемый сегмент модуля
	while (fgets(line, sizeof(line), maps)) {
		if (!strstr(line, module_name)) continue;
		if (!strstr(line, "r-xp")) continue;
		uint64_t s = 0, e = 0, off = 0;
		if (sscanf(line, "%lx-%lx %*s %lx", &s, &e, &off) >= 2) { xpStart = s; xpOff = off; break; }
	}

	if (!xpStart) { // нет исполняемого — падаем на низший
		fclose(maps);
		return get_module_lowest(module_name);
	}

	uint64_t candidate = xpStart - xpOff; // ожидаемая база кластера
	rewind(maps);

	// второй проход: offset-0 маппинг, ближайший к candidate
	uint64_t best = 0, bestDiff = ~0ULL;
	while (fgets(line, sizeof(line), maps)) {
		if (!strstr(line, module_name)) continue;
		uint64_t s = 0, e = 0, off = 0;
		if (sscanf(line, "%lx-%lx %*s %lx", &s, &e, &off) < 2) continue;
		if (off != 0) continue;
		uint64_t diff = (s > candidate) ? (s - candidate) : (candidate - s);
		if (diff < bestDiff) { bestDiff = diff; best = s; }
	}
	fclose(maps);

	// если offset-0 не нашёлся или далеко (>16MB) — используем candidate.
	if (!best || bestDiff > 0x1000000ULL) return candidate;
	return best;
}

// Диагностика: выводит все строки maps, содержащие подстроку.
static void dump_maps_matching(const char *needle, int maxLines) {
	char path[64];
	sprintf(path, "/proc/%d/maps", game_pid);
	FILE* maps = fopen(path, "rt");
	if (!maps) { OXLOGW("maps не открылся для '%s'", needle); return; }
	char line[1024];
	int n = 0;
	while (fgets(line, sizeof(line), maps) && n < maxLines) {
		if (strstr(line, needle)) {
			size_t l = strlen(line);
			if (l && line[l-1] == '\n') line[l-1] = 0;
			OXLOGT("  maps: %s", line);
			n++;
		}
	}
	if (n == 0) OXLOGW("  в maps нет строк с '%s'", needle);
	fclose(maps);
}

// Диапазон маппингов модуля [low, high) и число сегментов.
static void get_module_span(const char *module_name, uint64_t &low, uint64_t &high, int &segs) {
	low = 0; high = 0; segs = 0;
	char path[64]; sprintf(path, "/proc/%d/maps", game_pid);
	FILE* maps = fopen(path, "rt");
	if (!maps) return;
	char line[1024];
	while (fgets(line, sizeof(line), maps)) {
		if (!strstr(line, module_name)) continue;
		uint64_t s = 0, e = 0;
		if (sscanf(line, "%lx-%lx", &s, &e) == 2 && s) {
			if (!low || s < low) low = s;
			if (e > high) high = e;
			segs++;
		}
	}
	fclose(maps);
}

// Форвар��-объявления (определены ниже, в секции чтения списка игроков).
static inline bool ox_isPtr(uint64_t p);
static void ox_className(uint64_t klass, char *out, int outlen);

// Инфо об одном модуле: диапазон, размер, ELF-хедер. Возвращает базу (low).
static uint64_t ox_dumpModule(const char *name) {
	uint64_t low, high; int segs;
	get_module_span(name, low, high, segs);
	uint64_t size = (high > low) ? (high - low) : 0;
	if (!low) { OXLOGW("Модуль '%s' НЕ найден в maps", name); return 0; }
	OXLOGI("%s: base=0x%llx end=0x%llx size=%llu MB (0x%llx) segs=%d",
	       name, (unsigned long long)low, (unsigned long long)high,
	       (unsigned long long)(size >> 20), (unsigned long long)size, segs);
	uint8_t elf[16] = {0};
	if (vm_readv(low, elf, sizeof(elf)))
		OXLOGI("  ELF magic: %02x %02x %02x %02x %s", elf[0], elf[1], elf[2], elf[3],
		       (elf[0]==0x7f && elf[1]=='E' && elf[2]=='L' && elf[3]=='F') ? "OK ELF" : "НЕ ELF!");
	else
		OXLOGW("  ELF-хедер не читается на 0x%llx", (unsigned long long)low);
	dump_maps_matching(name, 12);
	return low;
}

// Разовая диагностика ОБЕИ�� схем доступа за один запуск.
static void ox_moduleInfo() {
	oxlog::section("MODULE INFO — все .so игры");
	dump_maps_matching(".so", 60); // общий обзор: увидеть реальные имена либ

	// ---------- Схема A: il2cpp (реальная база по r-xp кластеру) ----------
	oxlog::section("SCHEME A: il2cpp (libil2cpp.so)");
	ox_dumpModule(ox::MODULE); // общий обзор всех маппингов
	uint64_t baseA = get_module_base_real(ox::MODULE);
	OXLOGI("Истинная база (r-xp кластер) = 0x%llx", (unsigned long long)baseA);
	if (baseA) {
        uint64_t rva = g_playerManagerTypeInfoRVA;
		uint64_t at = baseA + rva;
		uint64_t klass = rpm<uint64_t>(at);
		OXLOGI("[base+RVA(0x%llx)]=0x%llx -> klass=0x%llx",
		       (unsigned long long)rva, (unsigned long long)at, (unsigned long long)klass);
		uint8_t buf[0x40];
		if (vm_readv(at, buf, sizeof(buf)))
			oxlog::hexdump("вокруг base+RVA", at, buf, sizeof(buf));
		if (ox_isPtr(klass)) {
			char cn[96]; ox_className(klass, cn, sizeof(cn));
			OXLOGI("  klass name='%s' %s", cn,
			       (cn[0]) ? "" : "(пусто — RVA неверный для этой версии)");
			// дамп самого klass — увидеть name/namespace/static_fields
			if (vm_readv(klass, buf, sizeof(buf)))
				oxlog::hexdump("Il2CppClass head", klass, buf, sizeof(buf));
		} else {
			OXLOGW("  klass не похож на указатель — RVA либо база неверны");
		}
	}
}

// Ленивый резолв базы libil2cpp.so. Возвращает true если база готова.
static bool ox_ensureBase(bool full) {
	if (il2cpp_base) {
		// Периодически проверяем, что процесс жив и модуль на месте (раз в ~120 кадров).
		static int liveCheck = 0;
		if (++liveCheck >= 120) {
			liveCheck = 0;
			if (!pidHasModule(game_pid, ox::MODULE)) {
				OXLOGW("[base] pid=%d потерял %s — сброс, пере-резолв", game_pid, ox::MODULE);
				il2cpp_base = 0;
				// падаем ниже на пере-резолв PID/базы
			}
		}
		if (il2cpp_base) {
			static bool infoOnce = false;
			if (!infoOnce && full) { infoOnce = true; ox_moduleInfo(); }
			return true;
		}
	}

	// Проверяем, жив ли текущий PID и есть ли у него нужный модуль.
	// Если maps не читается ИЛИ модуля нет — пере-резолвим PID, выбирая ГЛАВНЫЙ
	// проце������с игры (тот, где реально замаплен libil2cpp.so). Игра могла
	// перезапуститься (новый PID) или мы поймали сервисный процесс.
	bool needRePid = (game_pid <= 0) || !pidHasModule(game_pid, ox::MODULE);
	if (needRePid) {
		int np = getGameProcessID(ox::PACKAGE, ox::MODULE);
		if (np > 0 && np != game_pid) {
			OXLOGI("[pid] переключа��сь на процесс с %s: %d -> %d",
			       ox::MODULE, game_pid, np);
			game_pid = np;
		} else if (np <= 0) {
			if (full) OXLOGW("[pid] процесс игры с %s пока не найден (game_pid=%d)",
			                 ox::MODULE, game_pid);
		}
	}

	uint64_t low = get_module_base_real(ox::MODULE);
	if (low) {
		il2cpp_base = low;
		OXLOGI("[base] %s найден: 0x%llx (pid=%d, r-xp кластер)", ox::MODULE,
		       (unsigned long long)il2cpp_base, game_pid);
		return true;
	}

	if (full) {
		OXLOGW("[base] %s ещё не в maps (pid=%d). Диагностика:", ox::MODULE, game_pid);
		dump_maps_matching("il2cpp", 12);
		dump_maps_matching(".so", 4);
	}
	return false;
}

bool aimm;
bool aimvisible = true;
int aimcurbone = 0;
float aimFovPixels = 220.0f;
float aimSmooth = 6.0f;
float aimMaxDistance = 350.0f;
bool aimDrawFov = true;
bool aimPrediction = true;
float aimProjectileSpeed = 250.0f;
float aimLatencyMs = 50.0f;
float aimMaxLeadTime = 0.35f;
// bug #4 (crouch): у внешнего чита НЕТ надёжного per-player флага «присел»
// (crouch-state живёт в обфусцированных ECM-компонентах, не reachable от
// PlayerManager, а animator-bool требует вызова метода В процессе). Поэтому
// авто-детект невозможен. Вместо ложного авто-детекта — ручной тумблер:
// когда враги приседают, включаешь его и aim-точка опускается на crouch-дельту.
bool  aimCrouchSafe = false;   // опускать aim-точку (враги присели)
float aimCrouchDrop = 0.45f;   // на сколько метров опустить (стоя ~1.8, сидя ~1.35)

// LOS-чек (аим через стены): если ON — аимбот не наводится на цель, перекрытую
// building piece'ом по ходу луча. ESP при этом рисует цель СИНИМ (невидим) и
// ЗЕЛЁНЫМ (видим) — отдельно от команды/инга.
bool aimCheckWalls = true;
bool aimIgnoreTeammates = true;   // Аим пропускает сокомандников (team-key == local's team)
bool espColorByVisibility = true; // true — видимый=зелёный, за стеной=синий

// bug #5: аимбот работает ТОЛЬКО когда игрок целится (ADS/scope). ADS-состояние
// определяем по падению живого FOV ниже базового (FPManager.kYl < _kSQ*ratio) —
// проверяемо offline, не требует icall'ов. По умолчанию ВКЛ (запрос пользователя).
bool aimOnlyADS = true;

const char* aimbones[] = { "head","neck","body" };

bool check(uint64_t addr) {
	return addr != 0 && addr < 0xffffffffff;
}

std::string utf16le_to_utf8(const std::u16string &u16str) {    
    if (u16str.empty()) {
        return std::string();
    }

    const char16_t *p = u16str.data();   
    std::u16string::size_type len = u16str.length();  
    if (p[0] == 0xFEFF) {      
        p += 1;      
        len -= 1;   
    } 

    std::string u8str;   
    u8str.reserve(len * 3);    
    char16_t u16char;  
    for (std::u16string::size_type i = 0; i < len; ++i) {       
        u16char = p[i];        
        if (u16char < 0x0080) {          
            u8str.push_back((char) (u16char & 0x00FF));          
            continue;       
        }      
        if (u16char >= 0x0080 && u16char <= 0x07FF) {          
            u8str.push_back((char) (((u16char >> 6) & 0x1F) | 0xC0));        
            u8str.push_back((char) ((u16char & 0x3F) | 0x80));          
            continue;       
        }      
        if (u16char >= 0xD800 && u16char <= 0xDBFF) {           
            uint32_t highSur = u16char;          
            uint32_t lowSur = p[++i];          
            uint32_t codePoint = highSur - 0xD800;    
            codePoint <<= 10;        
            codePoint |= lowSur - 0xDC00;      
            codePoint += 0x10000;           
            u8str.push_back((char) ((codePoint >> 18) | 0xF0));     
            u8str.push_back((char) (((codePoint >> 12) & 0x3F) | 0x80));         
            u8str.push_back((char) (((codePoint >> 06) & 0x3F) | 0x80));            
            u8str.push_back((char) ((codePoint & 0x3F) | 0x80));        
            continue;       
        } { 
        u8str.push_back((char) (((u16char >> 12) & 0x0F) | 0xE0));          
        u8str.push_back((char) (((u16char >> 6) & 0x3F) | 0x80));      
        u8str.push_back((char) ((u16char & 0x3F) | 0x80));         
        continue;      
        }    
    }   
    return u8str;
}

[[maybe_unused]] int example() { return 0; }

bool espbox,esphpbar;
float espfillp = 0.f;
float espstroke = 2.f;
bool espCornerBox = false;
float espCornerLength = 0.28f;

template<typename T>
T get_prop(uint64_t player, const char* tag) {
	T property = NULL;
	uint64_t props = rpm<uint64_t>(get_photon(player) + 0x38);
	if(props) {
		int size = rpm<int>(props + 0x20);
		for(int i = 0; i < size; i++) {
			uint64_t propkey = rpm<uint64_t>(rpm<uint64_t>(props+0x18) + 0x28 + 0x18*i);
			uint64_t propval = rpm<uint64_t>(rpm<uint64_t>(props+0x18) + 0x30 + 0x18*i);
			if(propkey != 0) {
				std::string keyVal = rpm<String>(propkey).Get();
				if(strstr(keyVal.c_str(), tag)) {
					property = rpm<T>(propval + 0x10);
				}
			}
		}
	}
	return property;
}

bool espen;
bool esphpgradient;
bool espweapon;
bool esptracer;
bool espfill;
bool espgradient;
bool esphpoutline = true;
bool espdist = true;
bool esparm;

// Максимальная дальность отрисовки ESP (метры). По умолчанию огромная —
// показываем игроков хоть через всю карту. 0 = без ограничения вообще.
float espMaxDistance = 5000.0f;

// --- ESP Lines / Tracers ---
bool esplines_enabled = false;
float esplines_thickness = 2.0f;
float esplines_color[3] = {1.0f, 1.0f, 1.0f};
int   esplines_position = 0; // 0=bottom center, 1=crosshair

// --- Настройки камеры для W2S (тюнятся в меню на устройстве) ---
float camFov = 98.607f;     // ГОРИЗОНТАЛЬНЫЙ FOV (град) — откалибровано под игру
bool  camInvertPitch = false;// инвертировать вертикальную ось проекции
bool  camInvertYaw = false;  // инвертировать горизонтальную ось проекции
// FIX bug #3: базис камеры теперь ВСЕГДА из мирового кватерниона m_LookRoot
// (полный yaw+pitch). Legacy-тумблеры pitch-fallback/swap удалены — они
// приводили к двойному применению pitch («боксы уезжают вверх»).

// --- Состояние кастомного меню ---
bool  g_menuOpen  = true;    // открыто ли большое меню (счётчик врагов тумблит)
int   g_activeTab = 0;       // 0=ESP 1=Aim 2=Camera 3=Settings
int   g_accentTheme = 1;     // индекс акцент-темы GLASS UI (0..4); 1 = Violet (магента-родной)

// --- Фильтры и производительность ---
int   g_enemyCount  = 0;     // сколько игроков сейчас в кэше (для счётчика)
int   g_cacheInterval = 10;  // heavy list/HP scan; positions are refreshed separately
int   g_positionInterval = 1;  // рефреш позиций каждый кадр — минимальный drift ESP
bool  espHideTeammates = true;
bool espname;
bool esphealth;
bool vischeck;
bool esphat;
bool skeleton;
bool outoffov;
bool oofvis;
bool ESP3D;
bool esp3dcorners;
bool visible;
bool visibleskeleton;

float cornered;
float espround;
float esp3dcornerstroke;
float esphpsize = 3;
float width3d = 1.0f;
float oulinewidth3d = 0.5f;
float hatalpha = 0.200f;
float hatradius = 0.50f;
float hatsegments = 10.f;
float esphptextsize = 0;
bool espdraw;
bool espline;
float espboxcolor[3]={1,0,0};

ImDrawList* getDrawList() {
	return ImGui::GetBackgroundDrawList();
}

struct Vector4 {
	float x,y,z,w;
};

struct TMatrix {
    Vector4 position;
    Quaternion rotation;
    Vector4 scale;
};

static Vector3 getPosition(uint64_t transObj2) {
 uint64_t transObj = rpm<uint64_t>(transObj2 + 0x10);
 
    uint64_t matrix = rpm<uint64_t>(transObj + 0x38);
    uint64_t index = rpm<uint64_t>(transObj + 0x40);
 
    uint64_t matrix_list = rpm<uint64_t>(matrix + 0x18);
    uint64_t matrix_indices = rpm<uint64_t>(matrix + 0x20);
 
    Vector3 result = rpm<Vector3>(matrix_list + sizeof(TMatrix) * index);
    int transformIndex = rpm<int>(matrix_indices + sizeof(int) * index);
 
    while(transformIndex >= 0) {
        TMatrix tMatrix = rpm<TMatrix>(matrix_list + sizeof(TMatrix) * transformIndex);
 
        float rotX = tMatrix.rotation.x;
        float rotY = tMatrix.rotation.y;
        float rotZ = tMatrix.rotation.z;
        float rotW = tMatrix.rotation.w;
 
        float scaleX = result.x * tMatrix.scale.x;
        float scaleY = result.y * tMatrix.scale.y;
        float scaleZ = result.z * tMatrix.scale.z;
 
        result.x = tMatrix.position.x + scaleX +
                    (scaleX * ((rotY * rotY * -2.0) - (rotZ * rotZ * 2.0))) +
                    (scaleY * ((rotW * rotZ * -2.0) - (rotY * rotX * -2.0))) +
                    (scaleZ * ((rotZ * rotX * 2.0) - (rotW * rotY * -2.0)));
        result.y = tMatrix.position.y + scaleY +
                    (scaleX * ((rotX * rotY * 2.0) - (rotW * rotZ * -2.0))) +
                    (scaleY * ((rotZ * rotZ * -2.0) - (rotX * rotX * 2.0))) +
                    (scaleZ * ((rotW * rotX * -2.0) - (rotZ * rotY * -2.0)));
        result.z = tMatrix.position.z + scaleZ +
                    (scaleX * ((rotW * rotY * -2.0) - (rotX * rotZ * -2.0))) +
                    (scaleY * ((rotY * rotZ * 2.0) - (rotW * rotX * -2.0))) +
                    (scaleZ * ((rotX * rotX * -2.0) - (rotY * rotY * 2.0)));
 
        transformIndex = rpm<int>(matrix_indices + sizeof(int) * transformIndex);
    }
 
    return result;
}


// ============================================================================
//  OXIDE: чтение списка игроков через il2cpp static chain (с авто-детектом)
// ============================================================================

// Похоже ли значение на валидный heap/mmap указатель (выровнен, в диапазоне).
static inline bool ox_isPtr(uint64_t p) {
    return p >= 0x1000000000ULL && p < 0x8000000000ULL && (p & 0x3) == 0;
}

// Мягкая проверка указателя (для строк/полей БЕЗ требования выравнивания по 4).
static inline bool ox_isPtrLoose(uint64_t p) {
    return p >= 0x1000000000ULL && p < 0x8000000000ULL;
}

// Читает C-строку имени из Il2CppClass* (klass + CLASS_NAME -> const char*).
static void ox_className(uint64_t klass, char *out, int outlen) {
    out[0] = 0;
    if (!ox_isPtr(klass)) return;
    uint64_t namePtr = rpm<uint64_t>(klass + ox::CLASS_NAME);
    if (!ox_isPtrLoose(namePtr)) return; // строковый указатель может быть невыровнен
    char buf[96] = {0};
    if (!vm_readv(namePtr, buf, sizeof(buf) - 1)) return;
    int i = 0;
    for (; i < outlen - 1 && i < (int)sizeof(buf) - 1; i++) {
        char c = buf[i];
        if (c < 0x20 || (unsigned char)c > 0x7e) break; // только печатные ASCII
        out[i] = c;
    }
    out[i] = 0;
}

struct OxMapRange {
    uint64_t start;
    uint64_t end;
    char perms[5];
    char path[512];
};

// Snapshot /proc/<pid>/maps. Called only by the manual runtime resolver.
static void ox_collectMaps(std::vector<OxMapRange> &out, const char *pathFilter,
                           bool writableOnly, size_t maxBytes, bool anonOnly = false) {
    out.clear();
    char procPath[64]; snprintf(procPath, sizeof(procPath), "/proc/%d/maps", game_pid);
    FILE *maps = fopen(procPath, "rt");
    if (!maps) return;
    size_t total = 0;
    char line[1024];
    while (fgets(line, sizeof(line), maps)) {
        OxMapRange map{};
        unsigned long long start = 0, end = 0;
        char path[512] = {};
        int parsed = sscanf(line, "%llx-%llx %4s %*llx %*s %*s %511[^\n]",
                            &start, &end, map.perms, path);
        if (parsed < 3 || end <= start || map.perms[0] != 'r') continue;
        if (writableOnly && map.perms[1] != 'w') continue;
        if (pathFilter && *pathFilter && !strstr(path, pathFilter)) continue;
        if (anonOnly) {
            // Только анонимные регионы: расшифрованные метаданные/строки живут тут.
            if (path[0] != 0 && path[0] != '[') continue;
            // Пропускаем гигантские регионы (GC-куча) — имён классов там нет.
            if ((unsigned long long)(end - start) > 128ULL * 1024 * 1024) continue;
        }
        size_t size = (size_t)(end - start);
        if (size > maxBytes || total > maxBytes - size) continue;
        map.start = start;
        map.end = end;
        snprintf(map.path, sizeof(map.path), "%s", path);
        out.push_back(map);
        total += size;
    }
    fclose(maps);
}

static bool ox_asciiEquals(uint64_t address, const char *expected) {
    if (!ox_isPtrLoose(address) || !expected) return false;
    size_t length = strlen(expected);
    if (!length || length > 120) return false;
    char value[128] = {};
    return vm_readv(address, value, length + 1) &&
           memcmp(value, expected, length) == 0 && value[length] == 0;
}

// Searches mapped memory for an ASCII literal (with NUL) and returns addresses.
static void ox_findAscii(const std::vector<OxMapRange> &maps, const char *needle,
                         std::vector<uint64_t> &matches, size_t maxMatches) {
    matches.clear();
    const size_t needleLen = strlen(needle) + 1;
    if (needleLen < 2) return;
    // Больший чанк (4 MB) + memmem() glibc — SIMD-оптимизирован под ARM64,
    // на порядок быстрее ручного memcmp-по-байту в цикле (лог 2026-07-24
    // показал 8 MB/сек на прошлом slot-переборе; memmem даёт 200+ MB/сек).
    const size_t chunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(chunkSize + needleLen);
    for (const OxMapRange &map : maps) {
        size_t carry = 0;
        for (uint64_t at = map.start; at < map.end && matches.size() < maxMatches;) {
            if (ox_scanTimedOut()) return;
            size_t want = (size_t)std::min<uint64_t>(chunkSize, map.end - at);
            if (!vm_readv(at, buffer.data() + carry, want)) { carry = 0; at += want; continue; }
            size_t total = carry + want;
            // memmem — сплошной sub-string поиск glibc. Возвращает первое
            // вхождение или NULL; сдвигаем указатель на +1 и ищем следующее.
            const uint8_t *base = buffer.data();
            const uint8_t *cursor = base;
            size_t remaining = total;
            while (matches.size() < maxMatches) {
                void *hit = memmem(cursor, remaining, needle, needleLen);
                if (!hit) break;
                size_t off = (const uint8_t*)hit - base;
                matches.push_back(at - carry + off);
                cursor = (const uint8_t*)hit + 1;
                if (cursor >= base + total) break;
                remaining = total - (cursor - base);
            }
            carry = std::min(needleLen - 1, total);
            memmove(buffer.data(), buffer.data() + total - carry, carry);
            at += want;
        }
    }
}

// Finds Il2CppClass by its name/namespace: class+0x10 points to name, class+0x18
// points to namespace. Class objects are in writable managed runtime mappings.
static uint64_t ox_findClassByName(uint64_t nameAddress, const char *className,
                                   const char *nameSpace, const std::vector<OxMapRange> &rwMaps) {
    const size_t chunkSize = 1024 * 1024;
    std::vector<uint8_t> buffer(chunkSize);
    for (const OxMapRange &map : rwMaps) {
        for (uint64_t at = map.start; at < map.end;) {
            if (ox_scanTimedOut()) return 0;
            size_t want = (size_t)std::min<uint64_t>(chunkSize, map.end - at);
            if (!vm_readv(at, buffer.data(), want)) { at += want; continue; }
            for (size_t i = 0; i + sizeof(uint64_t) <= want; i += sizeof(uint64_t)) {
                uint64_t value = 0;
                memcpy(&value, buffer.data() + i, sizeof(value));
                if (value != nameAddress || at + i < ox::CLASS_NAME) continue;
                uint64_t klass = at + i - ox::CLASS_NAME;
                uint64_t namespaceAddress = rpm<uint64_t>(klass + ox::CLASS_NAMESPACE);
                bool nsOk = (nameSpace == nullptr) || ox_asciiEquals(namespaceAddress, nameSpace);
                if (nsOk && ox_asciiEquals(rpm<uint64_t>(klass + ox::CLASS_NAME), className))
                    return klass;
            }
            at += want;
        }
    }
    return 0;
}

// НАДЁЖНЫЙ поиск Il2CppClass: перебираем 8-байтовые слоты, трактуя значение как
// name-указатель. Если оно ведёт в пул строк и strcmp совпал — это klass+0x10.
// Не зависит от того, какую именно копию имени держит класс (каноничную из
// блоба метадаты), поэтому чинит прошлый "класс не найден" при найденных литералах.
//
// Кооперативный deadline: если ox_setScanDeadline(N) выставлен, скан свернётся
// после его истечения и вернёт 0 — исключает подвисание фона на десятки минут
// при сканировании многогигабайтной кучи Unity.
static uint64_t ox_scanClassByName(const char *className, const char *nameSpace,
                                   const std::vector<OxMapRange> &scanMaps,
                                   uint64_t strLo, uint64_t strHi) {
    // 4 MB чанки — меньше системных вызовов vm_readv (в 4 раза меньше сисколов);
    // прошлые 1 MB давали 8 MB/сек, 4 MB чанки в тестах ускоряют до ~30 MB/сек.
    const size_t chunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer(chunkSize);
    uint64_t regionsSeen = 0, regionsScanned = 0, bytesScanned = 0;
    for (const OxMapRange &map : scanMaps) {
        regionsSeen++;
        uint64_t sz = map.end - map.start;
        if (sz > 128ULL * 1024 * 1024) {
            OXLOGT("[scan] %s: skip region 0x%llx..0x%llx (%llu MB) — GC-heap",
                   className, (unsigned long long)map.start, (unsigned long long)map.end,
                   (unsigned long long)(sz >> 20));
            continue;
        }
        regionsScanned++;
        OXLOGT("[scan] %s: region %llu/%llu 0x%llx..0x%llx (%llu KB)",
               className, (unsigned long long)regionsScanned, (unsigned long long)scanMaps.size(),
               (unsigned long long)map.start, (unsigned long long)map.end,
               (unsigned long long)(sz >> 10));
        for (uint64_t at = map.start; at < map.end;) {
            if (ox_scanTimedOut()) {
                OXLOGW("[scan] %s: TIMEOUT после %llu регионов / %llu MB",
                       className, (unsigned long long)regionsScanned,
                       (unsigned long long)(bytesScanned >> 20));
                return 0;
            }
            size_t want = (size_t)std::min<uint64_t>(chunkSize, map.end - at);
            if (!vm_readv(at, buffer.data(), want)) { at += want; continue; }
            bytesScanned += want;
            for (size_t i = 0; i + sizeof(uint64_t) <= want; i += sizeof(uint64_t)) {
                uint64_t namePtr = 0;
                memcpy(&namePtr, buffer.data() + i, sizeof(namePtr));
                if (namePtr < strLo || namePtr >= strHi) continue; // должен вести в пул строк
                if (at + i < ox::CLASS_NAME) continue;
                if (!ox_asciiEquals(namePtr, className)) continue;
                uint64_t klass = at + i - ox::CLASS_NAME;
                uint64_t nsPtr = rpm<uint64_t>(klass + ox::CLASS_NAMESPACE);
                if (nameSpace && !ox_asciiEquals(nsPtr, nameSpace)) continue;
                OXLOGI("[scan] %s.%s FOUND klass=0x%llx после %llu MB",
                       nameSpace ? nameSpace : "*", className,
                       (unsigned long long)klass, (unsigned long long)(bytesScanned >> 20));
                return klass;
            }
            at += want;
        }
    }
    OXLOGT("[scan] %s: not found после %llu MB (%llu регионов)",
           className, (unsigned long long)(bytesScanned >> 20),
           (unsigned long long)regionsScanned);
    return 0;
}

// Finds the writable libil2cpp TypeInfo slot containing klass and converts its
// runtime address to an RVA from il2cpp_base. Уважает deadline из
// ox_setScanDeadline().
static uint64_t ox_findTypeInfoRVA(uint64_t klass, const std::vector<OxMapRange> &libRwMaps) {
    const size_t chunkSize = 1024 * 1024;
    std::vector<uint8_t> buffer(chunkSize);
    for (const OxMapRange &map : libRwMaps) {
        for (uint64_t at = map.start; at < map.end;) {
            if (ox_scanTimedOut()) return 0;
            size_t want = (size_t)std::min<uint64_t>(chunkSize, map.end - at);
            if (!vm_readv(at, buffer.data(), want)) { at += want; continue; }
            for (size_t i = 0; i + sizeof(uint64_t) <= want; i += sizeof(uint64_t)) {
                uint64_t value = 0;
                memcpy(&value, buffer.data() + i, sizeof(value));
                if (value == klass && at + i >= il2cpp_base)
                    return at + i - il2cpp_base;
            }
            at += want;
        }
    }
    return 0;
}

// Forward-decl: sweep вызывает auto-resolve, а тот определён ниже.
static bool ox_autoResolveTypeInfo(const char *className, const char *nameSpace,
                                   uint64_t &outRVA, uint64_t &outKlass, bool full);

// Runtime-скан ВСЕХ известных TypeInfo (по именам + namespace), с записью
// найденного RVA в лог. Служит источником свежих RVA для oxide_offsets.h после
// обновления игры — erafox прочитает Download-лог и обновит хардкод.
static void ox_sweepAllTypeInfos(bool full) {
    struct Known { const char *ns; const char *name; };
    static const Known kKnown[] = {
        {"Oxide",           "PlayerManager"},
        {"Oxide.Building",  "BuildingPiece"},
        {"Oxide",           "PlayerVitals"},
        {"Oxide",           "GenericVitals"},
        {"Oxide",           "EntityVitals"},
        {"Oxide",           "MouseLook"},
        {"Oxide",           "RaycastManager"},
        {"Oxide.Weapons",   "WeaponRecoil"},
        {"Oxide.Weapons",   "FPHitscan"},
        {"UnityEngine",     "Camera"},
        {"UnityEngine",     "Transform"},
    };
    oxlog::section("TYPEINFO SWEEP (runtime)");
    for (const Known &k : kKnown) {
        if (ox_scanTimedOut()) { OXLOGW("[sweep] deadline, дальше не сканируем"); break; }
        uint64_t rva = 0, klass = 0;
        bool ok = ox_autoResolveTypeInfo(k.name, k.ns, rva, klass, full);
        OXLOGI("[sweep] %-24s ns=%-16s -> klass=0x%llx  TypeInfoRVA=0x%llx  %s",
               k.name, k.ns, (unsigned long long)klass, (unsigned long long)rva,
               ok ? "OK" : "MISS");
    }
    OXLOGI("[sweep] завершён. Скопируй RVA в include/oxide_offsets.h если правишь fallback.");
}

// On-demand recovery of TypeInfo RVAs after a game update. First finds the
// literal in libil2cpp mappings, then Il2CppClass in writable runtime memory,
// and finally the TypeInfo pointer slot in libil2cpp's writable data segment.
static bool ox_autoResolveTypeInfo(const char *className, const char *nameSpace,
                                   uint64_t &outRVA, uint64_t &outKlass, bool full) {
    std::vector<OxMapRange> libReadMaps, rwMaps, libRwMaps, anonMaps;
    // Имена классов в защищённом билде расшифрованы в АНОНИМНУЮ память,
    // а НЕ в file-backed libil2cpp.so — поэтому прежний поиск давал 0 литералов.
    ox_collectMaps(anonMaps, nullptr, false, 1024ULL * 1024 * 1024, /*anonOnly=*/true);
    ox_collectMaps(libReadMaps, ox::MODULE, false, 768ULL * 1024 * 1024);
    ox_collectMaps(rwMaps, nullptr, true, 1024ULL * 1024 * 1024);
    ox_collectMaps(libRwMaps, ox::MODULE, true, 256ULL * 1024 * 1024);
    if (rwMaps.empty() && anonMaps.empty()) return false;

    // Границы пула строк (анонимная расшифрованная метадата + сама libil2cpp).
    uint64_t strLo = ~0ULL, strHi = 0;
    for (const OxMapRange &m : anonMaps)    { if (m.start < strLo) strLo = m.start; if (m.end > strHi) strHi = m.end; }
    for (const OxMapRange &m : libReadMaps) { if (m.start < strLo) strLo = m.start; if (m.end > strHi) strHi = m.end; }
    // ПЕРВЫЙ, надёжный путь: скан самих Il2CppClass со сравнением содержимого имени.
    if (strHi > strLo) {
        uint64_t klass = ox_scanClassByName(className, nameSpace, rwMaps, strLo, strHi);
        if (!klass) klass = ox_scanClassByName(className, nameSpace, anonMaps, strLo, strHi);
        if (!klass) klass = ox_scanClassByName(className, nullptr, rwMaps, strLo, strHi);
        if (klass) {
            outKlass = klass;
            uint64_t rva = ox_findTypeInfoRVA(klass, libRwMaps);
            if (rva) outRVA = rva;
            if (full) OXLOGI("[AUTO] %s.%s: klass=0x%llx (scan) TypeInfoRVA=0x%llx%s",
                             nameSpace, className, (unsigned long long)klass,
                             (unsigned long long)rva, rva ? "" : " (RVA n/a - klass \u043d\u0430\u043f\u0440\u044f\u043c\u0443\u044e)");
            return true;
        }
    }

    // Литерал имени ищем сперва в анонимной памяти (расшифрованный пул строк),
    // затем в самой libil2cpp (на случай незашифрованного билда).
    std::vector<uint64_t> nameAddresses;
    ox_findAscii(anonMaps, className, nameAddresses, 16);
    if (nameAddresses.empty()) ox_findAscii(libReadMaps, className, nameAddresses, 16);
    if (full) OXLOGI("[AUTO] %s: литералов=%zu (anon-рег.=%zu, lib-рег.=%zu)",
                     className, nameAddresses.size(), anonMaps.size(), libReadMaps.size());
    for (uint64_t nameAddress : nameAddresses) {
        uint64_t klass = ox_findClassByName(nameAddress, className, nameSpace, rwMaps);
        if (!klass) klass = ox_findClassByName(nameAddress, className, nameSpace, anonMaps);
        if (!klass) klass = ox_findClassByName(nameAddress, className, nullptr, rwMaps); // без namespace
        if (!klass) continue;
        // Указателя klass уже достаточно для чтения static_fields — успех здесь.
        outKlass = klass;
        uint64_t rva = ox_findTypeInfoRVA(klass, libRwMaps); // опционально (для диагностики)
        if (rva) outRVA = rva;
        if (full) OXLOGI("[AUTO] %s.%s: klass=0x%llx TypeInfoRVA=0x%llx%s",
                         nameSpace, className, (unsigned long long)klass,
                         (unsigned long long)rva,
                         rva ? "" : " (RVA n/a — читаем klass напрямую)");
        return true;
    }
    if (full) OXLOGW("[AUTO] %s.%s: класс не найден в памяти (не в матче?)", nameSpace, className);
    return false;
}

// Offline-baked class name offsets в global-metadata.dat (из decrypted split-zip
// metadata, декодировано Nov/typeinfo_offline.py + string table @0xdc0ec).
// В рантайме: expected_name_va = metadata_base + оффсет ниже.
static constexpr uint64_t OX_META_NAME_OFF_PLAYERMANAGER = 0x1704f0;  // "PlayerManager"
static constexpr uint64_t OX_META_NAME_OFF_BUILDINGPIECE = 0x10c22b;  // "BuildingPiece"
static constexpr uint64_t OX_META_NAME_OFF_PLAYERVITALS  = 0x169546;  // "PlayerVitals"

// Резолвит Il2CppClass* через ПОИСК ССЫЛКИ на имя класса в libil2cpp writable.
// Не сканирует анон-память игры (что убивает игру через watchdog CatsBit'а).
// Bulk-read libRw одним vm_readv, локальный поиск слота содержащего known name-VA.
// Найденный слот - 0x10 = Il2CppClass* (CLASS_NAME field lives at klass+0x10).
static uint64_t ox_findKlassByNameVa(uint64_t nameVa,
                                     const std::vector<OxMapRange> &libRwMaps) {
    for (const OxMapRange &m : libRwMaps) {
        if (m.end <= m.start) continue;
        size_t sz = (size_t)(m.end - m.start);
        if (sz > 16ULL * 1024 * 1024) continue;   // sanity cap 16 MB per region
        std::vector<uint8_t> buf(sz);
        if (!vm_readv(m.start, buf.data(), sz)) continue;
        // 8-byte aligned scan
        for (size_t off = 0; off + 8 <= sz; off += 8) {
            uint64_t v;
            memcpy(&v, buf.data() + off, 8);
            if (v == nameVa) {
                uint64_t slot_va = m.start + off;
                uint64_t klass = slot_va - ox::CLASS_NAME;   // slot IS klass->name field
                if (ox_isPtr(klass)) return klass;
            }
        }
    }
    return 0;
}

// Fast-seed через offline MetadataCache chain (10 read'ов total, БЕЗ сканов).
//
// Оффлайн-разбор libil2cpp.so через capstone нашёл цепочку резолва TDI → klass,
// которую собственный код игры использует (функция @0x4d777f0 в libil2cpp,
// сравнимо с MetadataCache::GetTypeInfoFromTypeDefinitionIndex):
//
//   1) metadata_base = *(base + 0xbe3bb20)         ; загруженный global-metadata
//   2) header_ptr    = *(base + 0xbe3bb28)         ; Il2CppGlobalMetadataHeader
//   3) stride        = *(base + 0xbe3bb34)         ; 1/2/4 байта на индекс
//   4) types_off_raw = *(header_ptr + 0xec)        ; XOR-encrypted оффсет
//   5) types_off     = types_off_raw ^ 0xA5C3F19D  ; decrypt (per README)
//   6) mc            = *(base + 0xbe3bb10)         ; MetadataCache*
//   7) s_TypeInfoTable = *(mc + 0x38)              ; Il2CppClass**
//   Один раз на всё, кэшируется. Дальше на класс:
//   8) ext_idx = *(metadata_base + types_off + TDI*stride)   ; u8/u16/u32
//   9) klass   = *(s_TypeInfoTable + ext_idx * 8)
//   10) validate: ox_className(klass) == expected_name
//
// Watchdog CatsBit'а срабатывает на ОБЪЁМ vm_readv (сравнение старой vs новой
// версии cheat'а: старая ~400 read'ов на резолв, новая — ~200 МБ scan). Здесь
// суммарно ~13 read'ов на 2 класса — не должен видеть.
static constexpr uint64_t OX_META_MC_RVA         = 0xbe3bb10;
static constexpr uint64_t OX_META_BASE_PTR_RVA   = 0xbe3bb20;
static constexpr uint64_t OX_META_HEADER_PTR_RVA = 0xbe3bb28;
static constexpr uint64_t OX_META_STRIDE_RVA     = 0xbe3bb34;
static constexpr uint32_t OX_META_HEADER_TYPES_FIELD = 0xec;
static constexpr uint32_t OX_META_XOR_KEY        = 0xA5C3F19D;
static constexpr uint64_t OX_MC_TYPEINFOTABLE_OFF = 0x38;

// Кэш стартап-констант (заполняется один раз, читается на каждый резолв).
static uint64_t g_metadataBase   = 0;
static uint64_t g_typesTableAddr = 0;
static uint32_t g_typeStride     = 0;
static uint64_t g_sTypeInfoTable = 0;

static bool ox_cacheMetadataChain() {
    if (!il2cpp_base) return false;
    if (g_metadataBase && g_typesTableAddr && g_typeStride && g_sTypeInfoTable) return true;

    uint64_t meta   = rpm<uint64_t>(il2cpp_base + OX_META_BASE_PTR_RVA);
    uint64_t hdr    = rpm<uint64_t>(il2cpp_base + OX_META_HEADER_PTR_RVA);
    uint32_t stride = rpm<uint32_t>(il2cpp_base + OX_META_STRIDE_RVA);
    if (!ox_isPtr(meta) || !ox_isPtr(hdr) || stride == 0 || stride > 4) {
        OXLOGW("[chain] MetadataCache pointers ещё не установлены (meta=0x%llx hdr=0x%llx stride=%u)",
               (unsigned long long)meta, (unsigned long long)hdr, stride);
        return false;
    }

    uint32_t off_raw = rpm<uint32_t>(hdr + OX_META_HEADER_TYPES_FIELD);
    uint32_t off_dec = off_raw ^ OX_META_XOR_KEY;

    uint64_t mc = rpm<uint64_t>(il2cpp_base + OX_META_MC_RVA);
    if (!ox_isPtr(mc)) {
        OXLOGW("[chain] MetadataCache pointer ещё null (mc=0x%llx)", (unsigned long long)mc);
        return false;
    }

    // === DIAGNOSTIC: пробуем ВСЕ MC offsets в диапазоне, где может лежать
    // s_TypeInfoTable. Для каждого читаем ext_idx для PM (TDI 8251), берём
    // klass, дампим 64 байта. По логу видно какой offset реально даёт
    // Il2CppClass с валидным именем.
    static bool s_diagLogged = false;
    if (!s_diagLogged) {
        s_diagLogged = true;
        uint64_t enc_addr_pm = meta + off_dec + 8251ULL * stride;
        uint32_t ext_idx_pm = (stride == 4 ? rpm<uint32_t>(enc_addr_pm) :
                              stride == 2 ? (uint32_t)rpm<uint16_t>(enc_addr_pm) :
                                            (uint32_t)rpm<uint8_t>(enc_addr_pm));
        OXLOGI("[chain-diag] meta=0x%llx hdr=0x%llx mc=0x%llx types_off=0x%x stride=%u ext_idx_pm(TDI=8251)=%u",
               (unsigned long long)meta, (unsigned long long)hdr,
               (unsigned long long)mc, off_dec, stride, ext_idx_pm);
        for (uint64_t mc_off : {0x18ULL, 0x20ULL, 0x28ULL, 0x30ULL, 0x38ULL, 0x40ULL, 0x48ULL, 0x50ULL, 0x58ULL, 0x60ULL}) {
            uint64_t table = rpm<uint64_t>(mc + mc_off);
            if (!ox_isPtr(table)) {
                OXLOGD("[chain-diag] MC+0x%llx = 0x%llx (не указатель)", mc_off, (unsigned long long)table);
                continue;
            }
            uint64_t klass_pm = rpm<uint64_t>(table + (uint64_t)ext_idx_pm * 8);
            char nm[64] = {0};
            if (ox_isPtr(klass_pm)) ox_className(klass_pm, nm, sizeof(nm));
            OXLOGI("[chain-diag] MC+0x%02llx=0x%llx  table[%u]=0x%llx  name='%s'",
                   mc_off, (unsigned long long)table, ext_idx_pm,
                   (unsigned long long)klass_pm, nm);
            if (ox_isPtr(klass_pm)) {
                uint8_t buf[64] = {0};
                if (vm_readv(klass_pm, buf, sizeof(buf))) {
                    char label[64];
                    snprintf(label, sizeof(label), "klass @ MC+0x%llx table[%u]", mc_off, ext_idx_pm);
                    oxlog::hexdump(label, klass_pm, buf, sizeof(buf));
                }
            }
        }
    }

    uint64_t tit = rpm<uint64_t>(mc + OX_MC_TYPEINFOTABLE_OFF);
    if (!ox_isPtr(tit)) {
        OXLOGW("[chain] s_TypeInfoTable ещё не выделен (mc=0x%llx tit=0x%llx)",
               (unsigned long long)mc, (unsigned long long)tit);
        return false;
    }

    g_metadataBase   = meta;
    g_typesTableAddr = meta + off_dec;
    g_typeStride     = stride;
    g_sTypeInfoTable = tit;
    OXLOGI("[chain] cached: meta=0x%llx types_off=0x%x stride=%u tit=0x%llx",
           (unsigned long long)meta, off_dec, stride,
           (unsigned long long)tit);
    return true;
}

static uint64_t ox_klassFromTDI(uint32_t tdi, const char *expectedName) {
    if (!ox_cacheMetadataChain()) return 0;
    uint64_t enc_addr = g_typesTableAddr + (uint64_t)tdi * g_typeStride;
    uint32_t ext_idx = 0;
    switch (g_typeStride) {
        case 1: ext_idx = rpm<uint8_t>(enc_addr);  if (ext_idx == 0xff)    return 0; break;
        case 2: ext_idx = rpm<uint16_t>(enc_addr); if (ext_idx == 0xffff)  return 0; break;
        case 4: ext_idx = rpm<uint32_t>(enc_addr); if (ext_idx == 0xffffffff) return 0; break;
        default: return 0;
    }
    uint64_t klass = rpm<uint64_t>(g_sTypeInfoTable + (uint64_t)ext_idx * 8);
    if (!ox_isPtr(klass)) return 0;
    char cn[96] = {0};
    ox_className(klass, cn, sizeof(cn));
    if (cn[0] == 0 || strcmp(cn, expectedName) != 0) {
        // Диагностика один раз на класс — дамп первых 32 байт klass чтобы
        // понять валиден ли указатель (Class::Init не прогнан VS указатель мусор).
        static std::atomic<uint64_t> s_dumpedFor{0};
        uint64_t marker = ((uint64_t)tdi << 40) | (klass & 0xffffffffffULL);
        if (s_dumpedFor.exchange(marker) != marker) {
            uint8_t buf[32] = {0};
            if (vm_readv(klass, buf, sizeof(buf))) {
                oxlog::hexdump("klass head (Class::Init pending?)", klass, buf, sizeof(buf));
            }
        }
        OXLOGD("[chain] TDI=%u ext=%u klass=0x%llx имя='%s' ожидали '%s'",
               tdi, ext_idx, (unsigned long long)klass, cn, expectedName);
        return 0;
    }
    return klass;
}

// Offline-baked byvalTypeIndex (external index в types[]) для каждого класса.
// Достаётся из decrypted global-metadata.dat: TypeDef record @ (0x1512204 + TDI*82) + 8.
// Проверено offline: 60939/46390/61003.
static constexpr int32_t OX_BYVAL_IDX_PLAYERMANAGER = 60939;
static constexpr int32_t OX_BYVAL_IDX_BUILDINGPIECE = 46390;
static constexpr int32_t OX_BYVAL_IDX_PLAYERVITALS  = 61003;

// Метадата диапазон для проверки: находится ли pointer в метадате (Il2CppTypeDefinition*)
// или в куче (Il2CppClass* пост-Class::Init). Метадата ~26 МБ, начинается с magic.
static bool ox_isInMetadata(uint64_t addr, uint64_t metaBase, size_t metaSize = 32ULL << 20) {
    return addr >= metaBase && addr < metaBase + metaSize;
}

// Bulk-search heap region for slot equal to nameVa (klass's name field).
// Klass в куче рядом с MC struct — Il2CppClass structs аллоцируются тем же
// аллокатором. Одна vm_readv-считка большого куска памяти игры, локальный поиск.
// Если найдено → klass = slot - CLASS_NAME (0x10).
static uint64_t ox_findKlassInHeap(uint64_t nameVa, uint64_t regionStart, size_t regionSize) {
    if (regionSize > 32ULL * 1024 * 1024) return 0;  // cap 32 MB per bulk read
    std::vector<uint8_t> buf(regionSize);
    if (!vm_readv(regionStart, buf.data(), regionSize)) return 0;
    for (size_t off = 0; off + 8 <= regionSize; off += 8) {
        uint64_t v;
        memcpy(&v, buf.data() + off, 8);
        if (v == nameVa) {
            uint64_t slot = regionStart + off;
            uint64_t klass = slot - ox::CLASS_NAME;
            // Sanity: klass должен быть в той же куче
            if (klass >= regionStart && klass < regionStart + regionSize) return klass;
        }
    }
    return 0;
}

// РЕЗОЛВ ЧЕРЕЗ s_TypeInfoTable — offline-найденный через ARM64 disasm.
//
// Subagent проанализировал libil2cpp.so offline и нашёл `MetadataCache::Initialize`
// @ RVA 0x4D765C8 + `GetTypeInfoFromTypeDefinitionIndex` @ RVA 0x4D773DC.
// Оттуда — cached lazy accessor:
//   0x4D77404: ldr x22, [x8, #0xb58]        ; x22 = *(0xBE3BB58) = s_TypeInfoTable
//   0x4D77408: ldr x20, [x22, w0, sxtw #3]  ; klass = s_TypeInfoTable[TDI]
//
// s_TypeInfoTable — Il2CppClass** массив 29366 записей, индексируется по TDI.
// Pointer-slot @ RVA 0xBE3BB58 — BSS в Seg 3 (writable), NULL в файле,
// malloc'ится MetadataCache::Initialize через Calloc.
//
// Lazy: s_TypeInfoTable[TDI] == NULL пока класс не заресолвен через
// il2cpp_class_from_type_definition_index() или через generated code metadata usage.
// PlayerManager.Awake (@ RVA 0x536449C) форсит ресолв через metadata_usage init.
// После первого захода в матч — s_TypeInfoTable[8251] = klass.
static constexpr uint64_t OX_S_TYPEINFO_TABLE_RVA = 0xBE3BB58ULL;
static constexpr uint32_t OX_TDI_PLAYERMANAGER    = 8251;
static constexpr uint32_t OX_TDI_BUILDINGPIECE    = 8521;
static constexpr uint32_t OX_TDI_PLAYERVITALS     = 7923;
static constexpr uint32_t OX_TYPEDEF_COUNT        = 29366;

// Fast-seed через s_TypeInfoTable[TDI]. Детерминированный, 3 vm_readv на класс.
static bool ox_fastSeedTypeInfoFromHeader() {
    if (!il2cpp_base) return false;

    // Читаем s_TypeInfoTable pointer из libil2cpp writable BSS slot @ 0xBE3BB58.
    // NULL если MetadataCache::Initialize ещё не отработал (крайне маловероятно
    // — Initialize вызывается при загрузке libil2cpp.so, ещё до появления
    // главного меню игры).
    uint64_t table = rpm<uint64_t>(il2cpp_base + OX_S_TYPEINFO_TABLE_RVA);
    if (!ox_isPtr(table)) {
        OXLOGD("[fast-seed] s_TypeInfoTable slot @ RVA 0x%llx = 0x%llx (init ещё не прогнан?)",
               (unsigned long long)OX_S_TYPEINFO_TABLE_RVA, (unsigned long long)table);
        return false;
    }

    // Для name-верификации нужен metadata_base
    uint64_t meta = rpm<uint64_t>(il2cpp_base + OX_META_BASE_PTR_RVA);
    if (!ox_isPtr(meta)) {
        OXLOGD("[fast-seed] metadata_base ещё null");
        return false;
    }

    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        OXLOGI("[fast-seed] s_TypeInfoTable=0x%llx meta_base=0x%llx (offline-verified RVA 0xBE3BB58)",
               (unsigned long long)table, (unsigned long long)meta);
    }

    struct Seed {
        uint32_t tdi;
        uint64_t nameOff;
        const char *expected;
        uint64_t *klass_out;
    };
    Seed seeds[] = {
        { OX_TDI_PLAYERMANAGER, OX_META_NAME_OFF_PLAYERMANAGER, "PlayerManager", &g_playerManagerKlass },
        { OX_TDI_BUILDINGPIECE, OX_META_NAME_OFF_BUILDINGPIECE, "BuildingPiece", &g_buildingPieceKlass },
    };
    int seeded = 0;
    for (const Seed &s : seeds) {
        if (s.tdi >= OX_TYPEDEF_COUNT) continue;
        // klass = s_TypeInfoTable[TDI]
        uint64_t klass = rpm<uint64_t>(table + (uint64_t)s.tdi * 8);
        if (!ox_isPtr(klass)) {
            OXLOGD("[fast-seed] %s: table[%u] = 0x%llx (класс ещё не резолвен игрой)",
                   s.expected, s.tdi, (unsigned long long)klass);
            continue;
        }
        // Верификация: name field должен совпадать с ожидаемым metadata pointer
        uint64_t name_ptr = rpm<uint64_t>(klass + ox::CLASS_NAME);
        uint64_t expected_name = meta + s.nameOff;
        if (name_ptr != expected_name) {
            OXLOGD("[fast-seed] %s: klass=0x%llx name_ptr=0x%llx (ожидали 0x%llx)",
                   s.expected, (unsigned long long)klass,
                   (unsigned long long)name_ptr, (unsigned long long)expected_name);
            continue;
        }
        *s.klass_out = klass;
        ++seeded;
        OXLOGI("[fast-seed] %s: klass=0x%llx (s_TypeInfoTable[%u], name verified)",
               s.expected, (unsigned long long)klass, s.tdi);
    }
    bool ok = (seeded == (int)(sizeof(seeds) / sizeof(seeds[0])));
    g_typeInfoAutoResolved = ok;
    return ok;
}

static bool ox_autoResolveTypeInfos(bool full) {
    uint64_t playerRVA = 0, buildingRVA = 0, playerKlass = 0, buildingKlass = 0;
    bool playerOk = ox_autoResolveTypeInfo("PlayerManager", "Oxide", playerRVA, playerKlass, full);
    bool buildingOk = ox_autoResolveTypeInfo("BuildingPiece", "Oxide.Building", buildingRVA, buildingKlass, full);
    if (playerOk)   { g_playerManagerKlass = playerKlass;   if (playerRVA)   g_playerManagerTypeInfoRVA = playerRVA; }
    if (buildingOk) { g_buildingPieceKlass = buildingKlass; if (buildingRVA) g_buildingPieceTypeInfoRVA = buildingRVA; }
    g_typeInfoAutoResolved = playerOk && buildingOk;
    if (full) OXLOGI("[AUTO] итог: PlayerManager=%s (klass=0x%llx), BuildingPiece=%s (klass=0x%llx)",
                     playerOk ? "OK" : "FAIL", (unsigned long long)g_playerManagerKlass,
                     buildingOk ? "OK" : "FAIL", (unsigned long long)g_buildingPieceKlass);
    return playerOk;
}

// Runtime metadata extractor -------------------------------------------------
// Il2CppGlobalMetadataHeader всегда начинается с 0xFAB11BAF уже ПОСЛЕ
// расшифровки. Ищем этот заголовок в native writable mappings, валидируем
// version и таблицы offset/size, затем копируем весь blob в Download.
static constexpr uint32_t IL2CPP_METADATA_MAGIC = 0xFAB11BAFU;
[[maybe_unused]] static std::string g_metadataDumpStatus = "No metadata dump yet";

static void ox_collectMetadataMaps(std::vector<OxMapRange> &out, size_t maxBytes) {
    out.clear();
    char procPath[64]; snprintf(procPath, sizeof(procPath), "/proc/%d/maps", game_pid);
    FILE *maps = fopen(procPath, "rt");
    if (!maps) return;
    size_t total = 0;
    char line[1024];
    while (fgets(line, sizeof(line), maps)) {
        // Decrypted IL2CPP metadata normally lives in malloc/anonymous native
        // memory. Dalvik heaps, per-thread stacks and device mappings are huge
        // and cannot contain it, so skip them before reading process memory.
        if (strstr(line, "dalvik") || strstr(line, "stack_and_tls") ||
            strstr(line, "/dev/") || strstr(line, "jit-"))
            continue;
        OxMapRange map{};
        unsigned long long start = 0, end = 0;
        char path[512] = {};
        int parsed = sscanf(line, "%llx-%llx %4s %*llx %*s %*s %511[^\n]",
                            &start, &end, map.perms, path);
        if (parsed < 3 || end <= start || map.perms[0] != 'r') continue;
        // Some loaders decrypt metadata and immediately mprotect it read-only.
        // Include writable native allocations as before plus anonymous read-only
        // memory and libil2cpp's own read-only mappings; skip unrelated system
        // libraries with file-backed r-- pages.
        bool isWritable = map.perms[1] == 'w';
        bool isIl2Cpp = strstr(path, "libil2cpp.so") != nullptr;
        bool isAnonymous = path[0] == 0 || strstr(path, "[anon:") != nullptr ||
                           strstr(path, "[heap]") != nullptr;
        if (!isWritable && !isIl2Cpp && !isAnonymous) continue;
        size_t size = (size_t)(end - start);
        if (size > maxBytes || total > maxBytes - size) continue;
        map.start = start;
        map.end = end;
        snprintf(map.path, sizeof(map.path), "%s", path);
        out.push_back(map);
        total += size;
    }
    fclose(maps);
}

static bool ox_validateMetadataHeader(uint64_t address, uint64_t mappingEnd,
                                      size_t &dumpSize, uint32_t &version) {
    // The header is a sequence of offset/size fields. Read enough for every
    // current Unity metadata version, but accept only table ends inside the map.
    uint32_t words[128] = {};
    if (!vm_readv(address, words, sizeof(words))) return false;
    if (words[0] != IL2CPP_METADATA_MAGIC) return false;
    version = words[1];
    if (version < 20 || version > 40) return false;

    uint64_t available = mappingEnd - address;
    uint64_t maxEnd = 0;
    int validTables = 0;
    for (size_t i = 2; i + 1 < IM_ARRAYSIZE(words); i += 2) {
        uint64_t offset = words[i];
        uint64_t size = words[i + 1];
        if (offset < 8 || size == 0) continue;
        if (offset >= available || size > available - offset) continue;
        uint64_t tableEnd = offset + size;
        if (tableEnd > maxEnd) maxEnd = tableEnd;
        ++validTables;
    }
    // A real header has many tables and is larger than a minimal header.
    if (validTables < 8 || maxEnd < 0x1000 || maxEnd > 512ULL * 1024 * 1024) return false;
    dumpSize = (size_t)maxEnd;
    return true;
}

static bool ox_copyRemoteFile(uint64_t remote, size_t size, const char *path) {
    FILE *out = fopen(path, "wb");
    if (!out) return false;
    const size_t chunkSize = 1024 * 1024;
    std::vector<uint8_t> buffer(chunkSize);
    bool ok = true;
    for (size_t offset = 0; offset < size;) {
        size_t count = std::min(chunkSize, size - offset);
        if (!vm_readv(remote + offset, buffer.data(), count) ||
            fwrite(buffer.data(), 1, count, out) != count) {
            ok = false;
            break;
        }
        offset += count;
    }
    fclose(out);
    if (!ok) remove(path);
    return ok;
}

[[maybe_unused]] static void ox_dumpDecryptedMetadata() {
    oxlog::section("DECRYPTED METADATA DUMP");
    if (game_pid <= 0) {
        g_metadataDumpStatus = "Dump failed: game process unavailable";
        OXLOGE("[META] %s", g_metadataDumpStatus.c_str());
        return;
    }

    std::vector<OxMapRange> maps;
    ox_collectMetadataMaps(maps, 1024ULL * 1024 * 1024);
    OXLOGI("[META] scanning %zu native readable/anonymous mappings", maps.size());

    const size_t chunkSize = 1024 * 1024;
    std::vector<uint8_t> buffer(chunkSize + 4);
    uint64_t metadataAddress = 0;
    uint64_t metadataEnd = 0;
    size_t metadataSize = 0;
    uint32_t metadataVersion = 0;

    for (const OxMapRange &map : maps) {
        size_t carry = 0;
        for (uint64_t at = map.start; at < map.end && !metadataAddress;) {
            size_t want = (size_t)std::min<uint64_t>(chunkSize, map.end - at);
            if (!vm_readv(at, buffer.data() + carry, want)) { carry = 0; at += want; continue; }
            size_t total = carry + want;
            for (size_t i = 0; i + sizeof(uint32_t) <= total; i += sizeof(uint32_t)) {
                uint32_t magic = 0;
                memcpy(&magic, buffer.data() + i, sizeof(magic));
                if (magic != IL2CPP_METADATA_MAGIC) continue;
                uint64_t candidate = at - carry + i;
                if (ox_validateMetadataHeader(candidate, map.end, metadataSize, metadataVersion)) {
                    metadataAddress = candidate;
                    metadataEnd = map.end;
                    break;
                }
            }
            carry = std::min(sizeof(uint32_t) - 1, total);
            memmove(buffer.data(), buffer.data() + total - carry, carry);
            at += want;
        }
        if (metadataAddress) break;
    }

    if (!metadataAddress) {
        g_metadataDumpStatus = "Metadata header not found in native writable memory";
        OXLOGE("[META] %s", g_metadataDumpStatus.c_str());
        return;
    }

    char outputPath[256];
    time_t now = time(nullptr);
    struct tm tmv{};
    localtime_r(&now, &tmv);
    const char *dir = oxlog::activeDir();
    if (!dir || !dir[0]) dir = "/sdcard/Download/EclipsOxide";
    snprintf(outputPath, sizeof(outputPath),
             "%s/global-metadata_%04d%02d%02d_%02d%02d%02d.dat",
             dir,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    OXLOGI("[META] header=0x%llx mappingEnd=0x%llx version=%u size=%zu",
           (unsigned long long)metadataAddress, (unsigned long long)metadataEnd,
           metadataVersion, metadataSize);
    if (!ox_copyRemoteFile(metadataAddress, metadataSize, outputPath)) {
        g_metadataDumpStatus = "Metadata copy failed: check root/storage permission";
        OXLOGE("[META] %s", g_metadataDumpStatus.c_str());
        return;
    }

    g_metadataDumpStatus = "Metadata saved: " + std::string(outputPath);
    OXLOGI("[META] COMPLETE: %s", g_metadataDumpStatus.c_str());
}

// Имя класса объекта: obj+0x0 -> klass -> name.
static void ox_objClassName(uint64_t obj, char *out, int outlen) {
    out[0] = 0;
    if (!ox_isPtr(obj)) return;
    ox_className(rpm<uint64_t>(obj), out, outlen);
}

// Найденный список игроков.
struct PlayerListView {
    uint64_t elems;   // адрес первого элемента (items + ARRAY_DATA)
    int      count;   // число элементов
    uint64_t container; // сам объект-контейнер (для лога)
    uint64_t statOff; // оффсет в static_fields
    bool     ok;
};

// Резолвит static_fields класса PlayerManager. Возвращает 0 при ошибке.
static uint64_t ox_getStatics(bool full) {
    if (!il2cpp_base) { if (full) OXLOGE("il2cpp_base == 0"); return 0; }

    // Приоритет: кэшированный указатель Il2CppClass (external-safe, без TypeInfo RVA).
    // Если его нет либо он протух (имя != PlayerManager) — сканируем память по имени.
    if (ox_isPtr(g_playerManagerKlass)) {
        char vn[96]; ox_className(g_playerManagerKlass, vn, sizeof(vn));
        if (strcmp(vn, "PlayerManager") != 0) {
            if (full) OXLOGW("Кэш klass протух ('%s') — пере-резолв", vn);
            g_playerManagerKlass = 0;
        }
    }
    if (!ox_isPtr(g_playerManagerKlass)) {
        // Fast-seed через TYPEIDX — троттлим 2.5с, иначе спамит on-demand
        // путь на каждом кадре пока Class::Init игры не прогнан.
#if OX_TRUST_HEADER_TYPEINFO
        static double s_lastSeed = -1000.0;
        double nowS = ImGui::GetTime();
        if (nowS - s_lastSeed >= 2.5) {
            s_lastSeed = nowS;
            if (ox_fastSeedTypeInfoFromHeader()) {
                if (full) OXLOGI("[AUTO] re-seed через TYPEIDX — klass восстановлен без скана");
            }
        }
#endif
    }
    // Scan-based auto-resolve ОТКЛЮЧЁН: убивает игру через watchdog CatsBit'а.
    // Startup thread единолично отвечает за fast-seed через MetadataCache-цепочку.
    // Если klass не заполнен здесь — статистики просто не будут читаться этот кадр.
    if (ox_isPtr(g_playerManagerKlass)) {
        uint64_t st = rpm<uint64_t>(g_playerManagerKlass + ox::CLASS_STATIC_FIELDS);
        if (full) oxlog::ptrstep("klass(cached) + STATIC_FIELDS(0xB8)",
                                 g_playerManagerKlass + ox::CLASS_STATIC_FIELDS, st);
        if (ox_isPtr(st)) return st;
        if (full) OXLOGW("static_fields по кэш-klass невалиден — откат на RVA");
    }

    // Откат: TypeInfo RVA из хедера (на metadata v39 может не совпасть).
    uint64_t typeInfoAddr = il2cpp_base + g_playerManagerTypeInfoRVA;
    uint64_t klass = rpm<uint64_t>(typeInfoAddr);
    if (full) {
        oxlog::section("RESOLVE static_fields");
        OXLOGI("base + PlayerManager_TypeInfo(0x%llx): [0x%llx] -> 0x%llx%s",
               (unsigned long long)g_playerManagerTypeInfoRVA,
               (unsigned long long)typeInfoAddr,
               (unsigned long long)klass,
               g_typeInfoAutoResolved ? " (auto)" : " (fallback)");
        char cn[96]; ox_className(klass, cn, sizeof(cn));
        OXLOGI("Им�� класса TypeInfo = '%s'  (ожидаем PlayerManager)", cn);
    }
    if (!ox_isPtr(klass)) { if (full) OXLOGE("klass невалиден — неверный RVA/база"); return 0; }

    uint64_t statics = rpm<uint64_t>(klass + ox::CLASS_STATIC_FIELDS);
    if (full) oxlog::ptrstep("klass + STATIC_FIELDS(0xB8)", klass + ox::CLASS_STATIC_FIELDS, statics);
    if (!ox_isPtr(statics)) { if (full) OXLOGE("static_fields невалиден"); return 0; }
    return statics;
}

// Оценивает объект-контейнер как список: возвращает count и elems, 0 если не список.
static bool ox_evalContainer(uint64_t c, uint64_t &elems, int &count,
                             char *clsOut, int clsLen, int &homo, int &checked) {
    elems = 0; count = 0; homo = 0; checked = 0; if (clsOut) clsOut[0] = 0;
    if (!ox_isPtr(c)) return false;
    uint64_t items = rpm<uint64_t>(c + ox::LIST_ITEMS);
    if (!ox_isPtr(items)) return false;

    int listSize   = rpm<int>(c + ox::LIST_SIZE);            // List._size (int)
    uint64_t arrLen = rpm<uint64_t>(items + ox::ARRAY_COUNT); // Il2CppArray.max_length

    if (listSize > 0 && listSize <= ox::LIST_COUNT_MAX) count = listSize;
    else if (arrLen > 0 && arrLen <= (uint64_t)ox::LIST_COUNT_MAX) count = (int)arrLen;
    else return false;

    elems = items + ox::ARRAY_DATA;
    uint64_t e0 = rpm<uint64_t>(elems);
    if (!ox_isPtr(e0)) return false;

    uint64_t k0 = rpm<uint64_t>(e0);
    for (int i = 0; i < count && i < 8; i++) {
        uint64_t e = rpm<uint64_t>(elems + i * 8);
        if (!ox_isPtr(e)) continue;
        checked++;
        if (rpm<uint64_t>(e) == k0) homo++;
    }
    if (clsOut) ox_objClassName(e0, clsOut, clsLen);
    return true;
}

// Сканирует static_fields и ищет список игроков (однородный массив объектов,
// имя класса содержит "Player"). full=true → логирует каждый кандидат.
static PlayerListView ox_scanForPlayerList(uint64_t statics, bool full) {
    PlayerListView best; best.ok = false; best.elems = 0; best.count = 0;
    best.container = 0; best.statOff = 0;
    PlayerListView fallback = best; // любой однородный список, если "Player" не найден

    if (full) oxlog::section("SCAN static_fields -> player list");

    for (uint64_t off = 0; off <= ox::STATIC_SCAN_MAX; off += 8) {
        uint64_t c = rpm<uint64_t>(statics + off);
        if (!ox_isPtr(c)) continue;

        uint64_t elems; int count, homo, checked; char cls[96];
        if (!ox_evalContainer(c, elems, count, cls, sizeof(cls), homo, checked)) continue;
        if (homo < 2) continue; // не однородный массив объектов

        bool nameMatch = (cls[0] && strstr(cls, "Player") != nullptr);
        if (full)
            OXLOGT("stat+0x%03llx: c=0x%llx count=%d homo=%d/%d cls='%s'%s",
                   (unsigned long long)off, (unsigned long long)c, count, homo, checked,
                   cls, nameMatch ? "  <-- PLAYER?" : "");

        if (nameMatch && !best.ok) {
            best.ok = true; best.elems = elems; best.count = count;
            best.container = c; best.statOff = off;
        }
        if (!fallback.ok) {
            fallback.ok = true; fallback.elems = elems; fallback.count = count;
            fallback.container = c; fallback.statOff = off;
        }
    }

    PlayerListView res = best.ok ? best : fallback;
    if (full) {
        if (res.ok)
            OXLOGI(">>> Список игроков: static+0x%llx container=0x%llx count=%d elems=0x%llx (%s)",
                   (unsigned long long)res.statOff, (unsigned long long)res.container,
                   res.count, (unsigned long long)res.elems,
                   best.ok ? "по имени класса" : "fallback: однородный список");
        else
            OXLOGW("Список игроков не найден. Ты точно В МАТЧЕ (не в меню)? Список может быть пуст.");
    }
    return res;
}

// Дампит объект-игрока целиком и ищет плавающие HP-кандидаты и позицию.
static void ox_dumpPlayer(int idx, uint64_t player) {
    char cls[96]; ox_objClassName(player, cls, sizeof(cls));
    OXLOGI("=== ИГРОК[%d] = 0x%llx  class='%s' ===", idx, (unsigned long long)player, cls);

    uint8_t buf[0x220];
    if (!vm_readv(player, buf, sizeof(buf))) { OXLOGW("  чтение объекта не удалось"); return; }
    oxlog::hexdump("player +0x00", player, buf, sizeof(buf));

    // Скан float-полей самого игрока: HP (1..200) и триплеты позиции.
    for (uint64_t o = 0x10; o + 4 <= sizeof(buf); o += 4) {
        float f; memcpy(&f, buf + o, 4);
        if (f >= 1.0f && f <= 200.0f && f == (float)(int)f)
            OXLOGT("  [HP?] player+0x%llx = %.1f", (unsigned long long)o, f);
    }
    for (uint64_t o = 0x10; o + 12 <= sizeof(buf); o += 4) {
        float x, y, z;
        memcpy(&x, buf + o, 4); memcpy(&y, buf + o + 4, 4); memcpy(&z, buf + o + 8, 4);
        bool ok = fabsf(x) > 0.01f && fabsf(x) < 100000.f &&
                  fabsf(y) > 0.01f && fabsf(y) < 100000.f &&
                  fabsf(z) > 0.01f && fabsf(z) < 100000.f;
        if (ok)
            OXLOGT("  [POS?] player+0x%llx = (%.2f, %.2f, %.2f)", (unsigned long long)o, x, y, z);
    }

    // Под-объекты игрока (NetworkIdentity, MouseLook, PlayerMap, vitals, transform...)
    // — HP и п���зиция обычно ЗДЕСЬ, а не в теле игрока. Рекурсивн�� сканируем каждый.
    for (uint64_t o = 0x8; o + 8 <= 0x120; o += 8) {
        uint64_t p; memcpy(&p, buf + o, 8);
        if (!ox_isPtr(p)) continue;
        char sub[96]; ox_objClassName(p, sub, sizeof(sub));
        OXLOGT("  [PTR] player+0x%llx -> 0x%llx class='%s'",
               (unsigned long long)o, (unsigned long long)p, sub);

        uint8_t sb[0x140];
        if (!vm_readv(p, sb, sizeof(sb))) continue;
        // HP-кандидаты в под-объекте.
        for (uint64_t so = 0x8; so + 4 <= sizeof(sb); so += 4) {
            float f; memcpy(&f, sb + so, 4);
            if (f >= 1.0f && f <= 200.0f && f == (float)(int)f)
                OXLOGT("      [HP?] %s+0x%llx = %.1f", sub[0]?sub:"?",
                       (unsigned long long)so, f);
        }
        // Триплеты позиции в под-объекте.
        for (uint64_t so = 0x8; so + 12 <= sizeof(sb); so += 4) {
            float x, y, z;
            memcpy(&x, sb + so, 4); memcpy(&y, sb + so + 4, 4); memcpy(&z, sb + so + 8, 4);
            bool ok = fabsf(x) > 0.5f && fabsf(x) < 100000.f &&
                      fabsf(y) > 0.5f && fabsf(y) < 100000.f &&
                      fabsf(z) > 0.5f && fabsf(z) < 100000.f;
            if (ok)
                OXLOGT("      [POS?] %s+0x%llx = (%.2f, %.2f, %.2f)", sub[0]?sub:"?",
                       (unsigned long long)so, x, y, z);
        }
    }
}

// HP ����грока: player+0xC8 -> PlayerVitals; vitals+0x88 -> float health.
static float ox_readHP(uint64_t player, bool full) {
    uint64_t vitals = rpm<uint64_t>(player + ox::PM_VITALS);
    if (full) oxlog::ptrstep("player + VITALS(0xC8)", player + ox::PM_VITALS, vitals);
    if (!ox_isPtr(vitals)) return -1.f;
    float hp = rpm<float>(vitals + ox::VITALS_HEALTH);
    if (full) OXLOGT("  vitals + 0x88 -> HP = %.3f", hp);
    return hp;
}

static std::string ox_readManagedString(uint64_t stringObject) {
    if (!ox_isPtr(stringObject)) return "";
    String value = rpm<String>(stringObject);
    if (value.length <= 0 || value.length > 128) return "";
    return value.Get();
}

// FIX bug #2: читает managed string как СЫРЫЕ UTF-16 байты (hex-строкой).
// String::Get() выкидывает всё >= 0x80 -> кириллические/эмодзи имена команд
// схлопывались в "" ИЛИ в неразличимые огрызки, из-за чего сравнение
// сокомандников молча ломалось. Здесь сравниваем по полному содержимому.
// Возвращает "" если строка пустая/невалидная.
static std::string ox_readStringKey(uint64_t stringObject) {
    if (!ox_isPtr(stringObject)) return "";
    int len = rpm<int>(stringObject + 0x10); // _stringLength
    if (len <= 0 || len > 128) return "";
    // читаем len*2 байт UTF-16 начиная с _firstChar (0x14)
    std::vector<uint8_t> raw((size_t)len * 2);
    if (!vm_readv(stringObject + 0x14, raw.data(), raw.size())) return "";
    static const char* HEX = "0123456789abcdef";
    std::string out; out.reserve(raw.size() * 2);
    for (uint8_t b : raw) { out.push_back(HEX[b >> 4]); out.push_back(HEX[b & 0xf]); }
    return out;
}

// FIX bug #2: строит СТАБИЛЬНЫЙ ключ команды игрока для сравнения сокомандников.
// Приоритет источников:
//   1) teamName (SyncVar @0x280) — реплицируется на всех клиентов для всех игроков.
//   2) team(WNn @0x120) -> pf(@0x90) -> mYs(teamId string @0x10) — corroborating.
// Возвращает "" если игрок ВНЕ команды (тогда его нельзя считать сокомандником).
static std::string ox_teamKey(uint64_t player) {
    if (!ox_isPtr(player)) return "";
    // 1) teamName (raw UTF-16). Ключ с префиксом источника, чтобы не путать.
    std::string tn = ox_readStringKey(rpm<uint64_t>(player + ox::PM_TEAM_NAME));
    if (!tn.empty()) return "N:" + tn;
    // 2) team -> pf -> mYs (teamId).
    // ВАЖНО: pf-объект РАЗДЕЛЯЕТСЯ между всеми членами команды (WNn per-player,
    // pf shared). Каждая PM имеет свою WNn, но их WNn.team_data указывают на
    // ОДИН И ТОТ ЖЕ pf. Поэтому pf-pointer — стабильный team-identity.
    uint64_t wnn = rpm<uint64_t>(player + ox::PM_TEAM);
    if (ox_isPtr(wnn)) {
        uint64_t pf = rpm<uint64_t>(wnn + ox::WNN_TEAM_DATA);
        if (ox_isPtr(pf)) {
            std::string id = ox_readStringKey(rpm<uint64_t>(pf + ox::PF_TEAM_ID_STR));
            if (!id.empty()) return "I:" + id;
            // 3) Fallback: сам pf-указатель как team-id. Все тимейты имеют
            // одинаковый pf pointer, разные — разные pf (или null для соло).
            char buf[32];
            snprintf(buf, sizeof(buf), "P:%llx", (unsigned long long)pf);
            return std::string(buf);
        }
    }
    return "";
}

// --- Кватернион-хелперы для обхода иерархии Transform ---
struct Quat { float x, y, z, w; };

static constexpr float DEG2RAD = 0.01745329251994f;

static inline Vector3 quatRotate(const Quat &q, const Vector3 &v) {
    float x2 = q.x * 2.f, y2 = q.y * 2.f, z2 = q.z * 2.f;
    float xx = q.x * x2, yy = q.y * y2, zz = q.z * z2;
    float xy = q.x * y2, xz = q.x * z2, yz = q.y * z2;
    float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    Vector3 r;
    r.x = (1.f - (yy + zz)) * v.x + (xy - wz) * v.y + (xz + wy) * v.z;
    r.y = (xy + wz) * v.x + (1.f - (xx + zz)) * v.y + (yz - wx) * v.z;
    r.z = (xz - wy) * v.x + (yz + wx) * v.y + (1.f - (xx + yy)) * v.z;
    return r;
}

static inline Quat quatMul(const Quat &a, const Quat &b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

// Низкоу��овневое чтение мировой TRS из НАТИВНОГО указателя Transform (managed).
// managed Transform + 0x10 -> internal; internal + 0x28 -> matrixData;
//   matrixData + 0x90 -> Vector3 pos; +0xA0 -> Quaternion rot.
// Возвращает true при валидном matrixData. rot/pos могут быть nullptr.
static bool ox_transformWorld(uint64_t transform, Vector3 *pos, Quat *rot, bool full, const char *tag) {
    if (!ox_isPtr(transform)) return false;
    uint64_t internal = rpm<uint64_t>(transform + ox::OBJ_CACHED_PTR);
    if (full && tag) oxlog::ptrstep(tag, transform + ox::OBJ_CACHED_PTR, internal);
    if (!ox_isPtr(internal)) return false;
    uint64_t matrixData = rpm<uint64_t>(internal + ox::TR_INT_MATRIXPTR);
    if (!ox_isPtr(matrixData)) return false;

    if (pos) *pos = rpm<Vector3>(matrixData + ox::TR_MATRIX_WORLDPOS);
    if (rot) *rot = rpm<Quat>(matrixData + ox::TR_MATRIX_ROT);
    return true;
}

// Мировая позиция игрока: player+0x68 (worldCameraRoot) -> matrixData+0x90.
static Vector3 ox_readPos(uint64_t player, bool full) {
    // ПЕРВИЧНО: прямое чтение PM.lastTickPosition (Vector3 @ 0x1C8). Это
    // server-authoritative поле — работает для ВСЕХ игроков (не только local).
    // Раньше читали worldCameraRoot (@0x68) — у remote игроков он NULL, отсюда
    // ESP крепился к нашей камере вместо тел противников.
    Vector3 tickPos = rpm<Vector3>(player + ox::PM_LAST_TICK_POS);
    if (full) OXLOGT("  lastTickPos = (%.2f, %.2f, %.2f)", tickPos.x, tickPos.y, tickPos.z);
    if (tickPos.x != 0.f || tickPos.y != 0.f || tickPos.z != 0.f) return tickPos;

    // Fallback #1: lastSavedPosition (иногда актуальнее чем lastTickPos на первых кадрах)
    Vector3 savedPos = rpm<Vector3>(player + ox::PM_LAST_SAVED_POS);
    if (savedPos.x != 0.f || savedPos.y != 0.f || savedPos.z != 0.f) {
        if (full) OXLOGT("  lastSavedPos fallback = (%.2f, %.2f, %.2f)", savedPos.x, savedPos.y, savedPos.z);
        return savedPos;
    }

    // Fallback #2: worldCameraRoot (работает только для локального игрока)
    Vector3 wPos(0, 0, 0);
    uint64_t transform = rpm<uint64_t>(player + ox::PM_TRANSFORM);
    if (full) oxlog::ptrstep("player + TRANSFORM(0x68)", player + ox::PM_TRANSFORM, transform);
    ox_transformWorld(transform, &wPos, nullptr, full, "  transform + m_CachedPtr(0x10)");
    if (full) OXLOGT("  worldCamRoot fallback = (%.2f, %.2f, %.2f)", wPos.x, wPos.y, wPos.z);
    return wPos;
}

static inline void ox_normalize3(Vector3 &v) {
    float n = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    if (n > 1e-6f) { v.x/=n; v.y/=n; v.z/=n; }
}

// ============================================================================
//  FIX bug #1/#2/#3 — базис камеры ИЗ УГЛОВ (kqP.x pitch, kqP.y yaw).
//  ---------------------------------------------------------------------------
//  Раньше базис брался из МИРОВОГО кватерниона m_LookRoot через нативный
//  Transform (matrixData+0xA0). Это ЛОМАЛОСЬ дважды:
//    (a) matrixData читался по internal+0x28; в Unity Transform_internal 0x28 =
//        `self` (back-ptr), а mData@0x38 -> читался мусор;
//    (b) даже с верным 0x38 world-кватернион лежит в libunity (icall
//        get_rotation_Injected) — оффсет непроверяем и версионно-хрупок.
//  Симптомы: ESP уезжал по вертикали при наклоне камеры, «плавал» по экрану,
//  а LOS-луч (для аима-через-стены) стартовал из неверной точки/направления.
//
//  Теперь используем ПРОВЕРЕННЫЕ managed-поля MouseLook.kqP:
//    pitch° = kqP.x @0x60   (disasm uae: m_LookRoot.localRotation=Euler(kqP.x,0,roll))
//    yaw°   = kqP.y @0x64   (аккумулятор yaw; конвенция == aimbot write)
//  Прямая тригонометрия (Unity left-handed, y-up), сверена численно с обратной
//  математикой аимбота до 1e-16:
//    right   = ( cosYaw,            0,        -sinYaw )
//    up      = ( sinYaw*sinPitch,   cosPitch,  cosYaw*sinPitch )
//    forward = ( sinYaw*cosPitch,  -sinPitch,  cosYaw*cosPitch )
//  Никаких нативных Transform-чтений — только два float'а. Инверты — на вызове.
//  eyePos НЕ выставляем здесь (позиция глаза считается отдельно из feet+eye).
// ============================================================================
static bool ox_readLookBasis(uint64_t player, Vector3 &right, Vector3 &up,
                             Vector3 &fwd, Vector3 *eyePos, bool full) {
    (void)eyePos;
    uint64_t mouseLook = rpm<uint64_t>(player + ox::PM_MOUSELOOK);
    if (!ox_isPtr(mouseLook)) return false;
    float pitchDeg = rpm<float>(mouseLook + ox::ML_PITCH); // kqP.x
    float yawDeg   = rpm<float>(mouseLook + ox::ML_YAW);   // kqP.y
    if (!isfinite(pitchDeg) || !isfinite(yawDeg)) return false;
    // Санитарная проверка pitch (игра клампит его к look-лимитам, |pitch|<=~89).
    if (fabsf(pitchDeg) > 179.f) return false;

    float p = pitchDeg * DEG2RAD;
    float y = yawDeg   * DEG2RAD;
    float sp = sinf(p), cp = cosf(p);
    float sy = sinf(y), cy = cosf(y);

    right   = Vector3(cy,        0.f, -sy);
    up      = Vector3(sy*sp,     cp,   cy*sp);
    fwd     = Vector3(sy*cp,    -sp,   cy*cp);
    ox_normalize3(right); ox_normalize3(up); ox_normalize3(fwd);
    if (full) OXLOGT("  lookBasis(angles) pitch=%.2f yaw=%.2f fwd=(%.3f,%.3f,%.3f)",
                     pitchDeg, yawDeg, fwd.x, fwd.y, fwd.z);
    return true;
}

// Живой ВЕРТИКАЛЬНЫЙ FOV камеры + ADS-детект из FPManager (PM+0x90).
//   effFOV = kYl(0xA0) - kSP(0xB4) ; base = _kSQ(0xAC).
//   aiming = effFOV < base*ADS_FOV_RATIO (при прицеле FOV зумится вниз).
// Возвращает верт. FOV (>0) или 0, если FPManager недоступен/значения мусор.
// aimingOut (может быть nullptr) — true если игрок целится.
static float ox_readCameraFovV(uint64_t player, bool *aimingOut, bool full) {
    if (aimingOut) *aimingOut = false;
    uint64_t fp = rpm<uint64_t>(player + ox::PM_FPMANAGER);
    if (!ox_isPtr(fp)) return 0.f;
    float cur  = rpm<float>(fp + ox::FP_FOV_CURRENT); // kYl
    float off  = rpm<float>(fp + ox::FP_FOV_OFFSET);  // kSP
    float base = rpm<float>(fp + ox::FP_FOV_BASE);    // _kSQ
    if (!isfinite(cur) || !isfinite(off) || !isfinite(base)) return 0.f;
    float eff = cur - off;
    if (eff < ox::EYE_FOV_MIN || eff > ox::EYE_FOV_MAX) return 0.f;
    if (aimingOut) {
        // ADS = текущий FOV ощутимо ниже БАЗОВОГО (зум прицела). База — это
        // хипфайр-FOV игрока (_kSQ), сравниваем ОТНОСИТЕЛЬНО его, чтобы работало
        // и при низком пользовательском FOV. Если _kSQ протух (мусор) — дефолт 60.
        float ref = (isfinite(base) && base > ox::EYE_FOV_MIN && base < ox::EYE_FOV_MAX)
                        ? base : ox::DEFAULT_VFOV;
        *aimingOut = (cur < ref * ox::ADS_FOV_RATIO);
    }
    if (full) OXLOGT("  FOV: kYl=%.2f kSP=%.2f base=%.2f -> effV=%.2f aiming=%d",
                     cur, off, base, eff, aimingOut ? (int)*aimingOut : -1);
    return eff;
}

// Строит камеру из КОНКРЕТНОГО (локального) игрока.
//   pos    = feet(lastTickPosition) + EYE_HEIGHT  (managed-поле, проверяемо)
//   basis  = из углов kqP.x(pitch)/kqP.y(yaw)     (проверяемо, без libunity)
//   fovV   = живой верт. FOV из FPManager          (kYl - kSP), меняется при ADS
// Дёшево (несколько managed-чтений) — можно звать каждый кадр.
static CameraView ox_buildCameraFromPlayer(uint64_t player, bool full) {
    CameraView cam;
    cam.valid = false;
    cam.fovH  = camFov;   // legacy горизонтальный fallback
    if (!ox_isPtr(player)) return cam;

    Vector3 body = ox_readPos(player, full);

    Vector3 right{1,0,0}, up{0,1,0}, fwd{0,0,1};
    bool haveDir = ox_readLookBasis(player, right, up, fwd, nullptr, full);

    // Позиция глаза камеры = ноги (server-authoritative lastTickPosition) + рост
    // глаз. Нативный eye-transform непроверяем (libunity), а feet+eye стабильно
    // и точно для ЛОКАЛЬНОГО игрока (его lastTickPosition всегда свежий).
    cam.pos = body + Vector3(0.f, ox::EYE_HEIGHT, 0.f);

    // Живой FOV + ADS-состояние (для W2S-масштаба и aimbot ADS-only).
    bool aiming = false;
    float fovV = ox_readCameraFovV(player, &aiming, full);
    cam.fovV   = fovV;      // 0 => W2S упадёт на горизонтальный слайдер
    cam.aiming = aiming;

    if (haveDir) {
        // Инверты — чистые флипы осей проекции (управляются галками в меню).
        if (camInvertYaw)   right = Vector3(-right.x, -right.y, -right.z);
        if (camInvertPitch) up    = Vector3(-up.x,    -up.y,    -up.z);
        cam.hasBasis = true;
        cam.right = right; cam.up = up; cam.forward = fwd;
    }

    // yaw/pitch как fallback (если базис не собрался).
    float distXZ = sqrtf(fwd.x*fwd.x + fwd.z*fwd.z);
    float yaw    = atan2f(fwd.x, fwd.z) * 57.2957795f;
    float pitch  = atan2f(fwd.y, distXZ) * 57.2957795f;
    if (camInvertPitch) pitch = -pitch;
    if (camInvertYaw)   yaw   = -yaw;
    cam.pitch = pitch;
    cam.yaw   = yaw;
    cam.valid = true;

    if (full) {
        OXLOGI("ЛОКАЛЬНЫЙ игрок=0x%llx", (unsigned long long)player);
        OXLOGI("  feet=(%.2f,%.2f,%.2f) eye=+%.2f -> cam.pos=(%.3f,%.3f,%.3f)",
               body.x, body.y, body.z, ox::EYE_HEIGHT, cam.pos.x, cam.pos.y, cam.pos.z);
        OXLOGI("  forward=(%.3f,%.3f,%.3f) haveBasis=%d fovV=%.2f aiming=%d",
               fwd.x, fwd.y, fwd.z, (int)haveDir, cam.fovV, (int)cam.aiming);
    }
    return cam;
}

// Проверка: остаётся ли кэшированный g_localPlayer валидным (не респавн).
// После смерти + респавна игра часто выделяет НОВЫЙ PlayerManager для локального
// игрока, а старый освобождается. Кэшированный g_localPlayer тогда указывает
// в garbage → камера строится с мусором → ESP плывёт.
// Валидация: у живого локального PM должны быть mouseLook + raycastManager
// non-null (только у него их выставляют).
static bool ox_localPlayerStillValid() {
    if (!ox_isPtr(g_localPlayer)) return false;
    uint64_t mouseLook = rpm<uint64_t>(g_localPlayer + ox::PM_MOUSELOOK);
    uint64_t raycast   = rpm<uint64_t>(g_localPlayer + ox::PM_RAYCASTMANAGER);
    return ox_isPtr(mouseLook) && ox_isPtr(raycast);
}

// Находит локального игрока в списке, запоминает g_localPlayer и строит камеру.
static CameraView ox_getCamera(uint64_t elems, int count, bool full) {
    if (full) oxlog::section("CAMERA (local player)");
    // Bug D FIX: если кэшированный g_localPlayer протух (респавн + новый PM) —
    // сбрасываем чтобы форсировать пере-выбор из активного списка.
    if (g_localPlayer && !ox_localPlayerStillValid()) {
        OXLOGI("[respawn] g_localPlayer=0x%llx стух (mouseLook/raycast null) — re-scan",
               (unsigned long long)g_localPlayer);
        g_localPlayer = 0;
        g_localTeamName.clear();
    }
    for (int i = 0; i < count; i++) {
        uint64_t player = rpm<uint64_t>(elems + i * 8);
        if (!ox_isPtr(player)) continue;
        // В этом билде НЕТ прямого флага isLocalPlayer (0x188 = чужой bool k__BackingField).
        // Локального детектим по наличию активной связки камеры: mouseLook + raycastManager
        // заполнены только у своего игрока.
        uint64_t mouseLook = rpm<uint64_t>(player + ox::PM_MOUSELOOK);
        uint64_t raycast   = rpm<uint64_t>(player + ox::PM_RAYCASTMANAGER);
        bool isLocal = ox_isPtr(mouseLook) && ox_isPtr(raycast);
        if (full)
            OXLOGT("player[%d]=0x%llx isLocal=%d mouseLook=0x%llx raycast=0x%llx",
                   i, (unsigned long long)player, (int)isLocal,
                   (unsigned long long)mouseLook, (unsigned long long)raycast);
        if (!isLocal) continue;
        if (g_localPlayer != player) {
            OXLOGI("[local] выбран локальный игрок PM=0x%llx (был 0x%llx)",
                   (unsigned long long)player, (unsigned long long)g_localPlayer);
            g_localPlayer = player;
            g_localTeamName = ox_teamKey(player);
            OXLOGI("[local] teamKey='%s' (empty => not in a team)", g_localTeamName.c_str());
        }
        return ox_buildCameraFromPlayer(player, full);
    }
    if (full) OXLOGW("Локальный игрок не найден (нет isLocalPlayer+mouseLook в списке)");
    CameraView cam; cam.valid = false; cam.fovH = camFov;
    return cam;
}

static float ox_normalizeAngle(float angle) {
    while (angle > 180.0f) angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
    return angle;
}

// ============================================================================
//  LOS-чек через BuildingPiece.saveList
// ============================================================================
//  Резолвим BuildingPiece_TypeInfo (обновлённый RVA 0xB8B97E0) -> static_fields ->
//  saveList (List<BuildingPiece>) -> перебираемm_Bounds (локальные) + m_CorrectPosition
//  (мировые) -> world AABB -> классический slab-method ray-AABB.
//
//  Кэш: раз в N кадров (BUILDING_CACHE_INTERVAL) перечитываем список построек,
//  между кадрами — переиспользуем. Это убирает ~2000*8 байт/кадр лишних чтений.
//  Альтернативно — для получения ВСЕХ твёрдых объектов стоило бы звать
//  Physics.Raycast, но это требует внедрения метода в процесс — вне рамок
//  внешнего чита. Building-LOS покрывает ~90% случаев (стены/фундаменты/потолки).
//
//  Структура Bounds (UnityEngine, TypeDefIndex 13482):
//    0x00  Vector3 m_Center
//    0x0C  Vector3 m_Extents
//  min = center - extents, max = center + extents
// ============================================================================

struct WorldAABB {
    Vector3 min;
    Vector3 max;
};

struct BuildingCacheEntry {
    uint64_t address;       // адрес BuildingPiece (для лога)
    WorldAABB bounds;       // World-space AABB
    bool      valid;
};

static std::vector<BuildingCacheEntry> g_buildingCache;
static std::mutex g_buildingMutex;
static int g_buildingCacheFrame = -1000;
static constexpr int BUILDING_CACHE_INTERVAL = 30; // перечитываем раз в 30 кадров (~0.5с)

// Резолв static_fields для класса по его TypeInfo RVA. Возвращает 0 при ошибке.
// Читает static_fields класса. Приоритет — кэширован��ый указатель Il2CppClass
// (надёжно на metadata v39). Если его нет, откат на TypeInfo RVA (нестабилен).
static uint64_t ox_getClassStatics(uint64_t typeInfoRVA, uint64_t cachedKlass, bool tryLog) {
    if (!il2cpp_base) { if (tryLog) OXLOGE("il2cpp_base == 0 (getClassStatics)"); return 0; }
    uint64_t klass = ox_isPtr(cachedKlass) ? cachedKlass : 0;
    if (!ox_isPtr(klass)) {
        uint64_t typeInfoAddr = il2cpp_base + typeInfoRVA;
        klass = rpm<uint64_t>(typeInfoAddr);
        if (tryLog) OXLOGD("getClassStatics: klass из RVA 0x%llx -> 0x%llx",
                           (unsigned long long)typeInfoRVA, (unsigned long long)klass);
    }
    if (!ox_isPtr(klass)) { if (tryLog) OXLOGE("klass невалиден (RVA=0x%llx, cached=0x%llx)", (unsigned long long)typeInfoRVA, (unsigned long long)cachedKlass); return 0; }
    uint64_t statics = rpm<uint64_t>(klass + ox::CLASS_STATIC_FIELDS);
    if (!ox_isPtr(statics)) { if (tryLog) OXLOGE("static_fields невалиден"); return 0; }
    return statics;
}

// Развёртка List<T> -> {elements_ptr, count}. Аналог ox_evalContainer, без проверки
// однородности (для BuildingPiece.saveList не нужно — оно типизировано).
static bool ox_readList(uint64_t listObj, uint64_t &elems, int &count) {
    elems = 0; count = 0;
    if (!ox_isPtr(listObj)) return false;
    uint64_t items = rpm<uint64_t>(listObj + ox::LIST_ITEMS);
    if (!ox_isPtr(items)) return false;
    int listSize = rpm<int>(listObj + ox::LIST_SIZE);
    uint64_t arrLen = rpm<uint64_t>(items + ox::ARRAY_COUNT);
    if (listSize > 0 && listSize <= ox::LOS_SAVELIST_MAX) count = listSize;
    else if (arrLen > 0 && arrLen <= (uint64_t)ox::LOS_SAVELIST_MAX) count = (int)arrLen;
    else return false;
    elems = items + ox::ARRAY_DATA;
    return count > 0;
}

// ============================================================================
//  FIX bug #1: BuildingPiece.saveList — это HashSet<T>, а НЕ List<T>.
//  Старый ox_readList на HashSet возвращал мусор -> кэш построек всегда пуст ->
//  aimCheckWalls/espColorByVisibility НИКОГДА не срабатывали. Ниже — корректный
//  обход Mono HashSet<T> и Dictionary<K,V> (fallback).
// ============================================================================

// Обходит Mono HashSet<T> (T — ссылочный тип), вызывая cb(value) для каждого
// живого элемента. Читает Slot[] одним bulk-vm_readv (быстро, без пер-элемент чтений).
//   set+0x18 -> Slot[] _slots ; set+0x24 -> _lastIndex
//   Slot { int hashCode; int next; T value } stride 0x10, занятый: hashCode>=0.
// Возвращает число обойдённых живых элементов, -1 при невалидном наборе.
template<typename CB>
static int ox_forEachHashSet(uint64_t setObj, CB&& cb) {
    if (!ox_isPtr(setObj)) return -1;
    uint64_t slots = rpm<uint64_t>(setObj + ox::HASHSET_SLOTS);
    if (!ox_isPtr(slots)) return -1;
    int lastIndex = rpm<int>(setObj + ox::HASHSET_LASTINDEX);
    int count     = rpm<int>(setObj + ox::HASHSET_COUNT);
    if (lastIndex <= 0 || lastIndex > ox::LOS_SAVELIST_MAX) {
        // _lastIndex мусор — попробуем _count как границу.
        if (count <= 0 || count > ox::LOS_SAVELIST_MAX) return -1;
        lastIndex = count;
    }
    uint64_t arrLen = rpm<uint64_t>(slots + ox::ARRAY_COUNT);
    if (arrLen == 0 || arrLen > (uint64_t)ox::LOS_SAVELIST_MAX) return -1;
    if ((uint64_t)lastIndex > arrLen) lastIndex = (int)arrLen;

    // Bulk-read всего slots-массива (Slot stride 0x10) одним чтением.
    uint64_t dataBase = slots + ox::ARRAY_DATA;
    size_t bytes = (size_t)lastIndex * (size_t)ox::HASHSET_SLOT_STRIDE;
    std::vector<uint8_t> buf(bytes);
    if (!vm_readv(dataBase, buf.data(), bytes)) return -1;

    int seen = 0;
    for (int i = 0; i < lastIndex; ++i) {
        const uint8_t* s = buf.data() + (size_t)i * ox::HASHSET_SLOT_STRIDE;
        int32_t hashCode; memcpy(&hashCode, s + ox::HASHSET_SLOT_HASHCODE, 4);
        if (hashCode < 0) continue; // свободный слот
        uint64_t value; memcpy(&value, s + ox::HASHSET_SLOT_VALUE, 8);
        if (!ox_isPtr(value)) continue;
        cb(value);
        ++seen;
    }
    return seen;
}

// Обходит Mono Dictionary<int,T> (fallback для saveLookup) -> cb(value).
//   dict+0x18 -> Entry[] entries ; dict+0x28 -> count
//   Entry { int hashCode; int next; int key; T value } stride 0x18, занят: hashCode>=0.
template<typename CB>
static int ox_forEachDictValues(uint64_t dictObj, CB&& cb) {
    if (!ox_isPtr(dictObj)) return -1;
    uint64_t entries = rpm<uint64_t>(dictObj + ox::DICT_ENTRIES);
    if (!ox_isPtr(entries)) return -1;
    int count = rpm<int>(dictObj + ox::DICT_COUNT);
    if (count <= 0 || count > ox::LOS_SAVELIST_MAX) return -1;
    uint64_t arrLen = rpm<uint64_t>(entries + ox::ARRAY_COUNT);
    if (arrLen == 0 || arrLen > (uint64_t)ox::LOS_SAVELIST_MAX) return -1;
    if ((uint64_t)count > arrLen) count = (int)arrLen;

    uint64_t dataBase = entries + ox::ARRAY_DATA;
    size_t bytes = (size_t)count * (size_t)ox::DICT_ENTRY_STRIDE;
    std::vector<uint8_t> buf(bytes);
    if (!vm_readv(dataBase, buf.data(), bytes)) return -1;

    int seen = 0;
    for (int i = 0; i < count; ++i) {
        const uint8_t* e = buf.data() + (size_t)i * ox::DICT_ENTRY_STRIDE;
        int32_t hashCode; memcpy(&hashCode, e + ox::DICT_ENTRY_HASHCODE, 4);
        if (hashCode < 0) continue;
        uint64_t value; memcpy(&value, e + ox::DICT_ENTRY_VALUE, 8);
        if (!ox_isPtr(value)) continue;
        cb(value);
        ++seen;
    }
    return seen;
}

// Читает Bounds в мировую AABB. Инфлейтит на LOS_AABB_INFLATE со всех сторон.
// Euler (deg, Unity ZXY) -> Quat. Для поворота локальных bounds в мир.
static Quat ox_eulerToQuat(const Vector3 &degEuler) {
    float rx = degEuler.x * DEG2RAD * 0.5f;
    float ry = degEuler.y * DEG2RAD * 0.5f;
    float rz = degEuler.z * DEG2RAD * 0.5f;
    float cx = cosf(rx), sx = sinf(rx);
    float cy = cosf(ry), sy = sinf(ry);
    float cz = cosf(rz), sz = sinf(rz);
    // Unity order ZXY: q = Y * X * Z (applied right-to-left)
    Quat qx{ sx, 0, 0, cx };
    Quat qy{ 0, sy, 0, cy };
    Quat qz{ 0, 0, sz, cz };
    return quatMul(quatMul(qy, qx), qz);
}

static bool ox_readWorldAABB(uint64_t building, WorldAABB &out) {
    out = WorldAABB{Vector3(0,0,0), Vector3(0,0,0)};
    if (!ox_isPtr(building)) return false;

    // World-позиция постройки (server-authoritative m_CorrectPosition @0x8C).
    Vector3 worldPos = rpm<Vector3>(building + ox::BP_CORRECT_POSITION);
    // m_CorrectRotation (Euler deg @0x98) — поворот постройки в мире.
    Vector3 worldRotEuler = rpm<Vector3>(building + ox::BP_CORRECT_ROTATION);
    // Bounds локальны к pivot'у (m_Center 0x0, m_Extents 0xC на building+BP_BOUNDS).
    uint64_t bAddr = building + ox::BP_BOUNDS;
    Vector3 center = rpm<Vector3>(bAddr + ox::BOUNDS_CENTER);
    Vector3 extents = rpm<Vector3>(bAddr + ox::BOUNDS_EXTENTS);

    // Валидность.
    if (extents.x < 0.f || extents.y < 0.f || extents.z < 0.f) return false;
    if (extents.x > 100.f || extents.y > 100.f || extents.z > 100.f) return false;
    if (!isfinite(center.x) || !isfinite(center.y) || !isfinite(center.z)) return false;
    if (!isfinite(extents.x) || !isfinite(extents.y) || !isfinite(extents.z)) return false;
    if (!isfinite(worldRotEuler.x) || !isfinite(worldRotEuler.y) || !isfinite(worldRotEuler.z))
        worldRotEuler = Vector3(0,0,0);

    float infl = ox::LOS_AABB_INFLATE;

    // FIX bug #1: учитываем ПОВОРОТ постройки. Стены/фундаменты в Oxide повёрнуты
    // (кратно 90° и не только) -> без учёта rotation AABB съезжал и LOS промахивался.
    // Строим мировой axis-aligned AABB из 8 повёрнутых углов локального Bounds.
    bool rotated = (fabsf(worldRotEuler.x) > 0.01f || fabsf(worldRotEuler.y) > 0.01f ||
                    fabsf(worldRotEuler.z) > 0.01f);
    if (!rotated) {
        Vector3 wc(center.x + worldPos.x, center.y + worldPos.y, center.z + worldPos.z);
        out.min = Vector3(wc.x - extents.x - infl, wc.y - extents.y - infl, wc.z - extents.z - infl);
        out.max = Vector3(wc.x + extents.x + infl, wc.y + extents.y + infl, wc.z + extents.z + infl);
        return true;
    }

    Quat q = ox_eulerToQuat(worldRotEuler);
    Vector3 mn( 1e30f, 1e30f, 1e30f), mx(-1e30f,-1e30f,-1e30f);
    for (int sx = -1; sx <= 1; sx += 2)
    for (int sy = -1; sy <= 1; sy += 2)
    for (int sz = -1; sz <= 1; sz += 2) {
        Vector3 localCorner(center.x + sx*extents.x,
                            center.y + sy*extents.y,
                            center.z + sz*extents.z);
        Vector3 r = quatRotate(q, localCorner);      // повернули вокруг pivot
        Vector3 w(r.x + worldPos.x, r.y + worldPos.y, r.z + worldPos.z);
        if (w.x < mn.x) mn.x = w.x; if (w.x > mx.x) mx.x = w.x;
        if (w.y < mn.y) mn.y = w.y; if (w.y > mx.y) mx.y = w.y;
        if (w.z < mn.z) mn.z = w.z; if (w.z > mx.z) mx.z = w.z;
    }
    out.min = Vector3(mn.x - infl, mn.y - infl, mn.z - infl);
    out.max = Vector3(mx.x + infl, mx.y + infl, mx.z + infl);
    return true;
}

// Slab-method ray-AABB. Возвращает true если луч попадает в AABB, и t-интервал
// [tNear, tFar]. Если начало луча внутри AABB — tNear=0, tFar=dist до выхода.
static bool ox_rayAABB(const Vector3 &origin, const Vector3 &dirNorm,
                       const WorldAABB &b, float &tNear, float &tFar) {
    tNear = -1e30f;
    tFar  = +1e30f;
    const float *o = &origin.x;
    const float *d = &dirNorm.x;
    const float *mn = &b.min.x;
    const float *mx = &b.max.x;
    for (int i = 0; i < 3; ++i) {
        float oi = o[i], di = d[i], mni = mn[i], mxi = mx[i];
        if (fabsf(di) < 1e-6f) {
            // Параллельная оси — луч вне slab
            if (oi < mni || oi > mxi) return false;
        } else {
            float inv = 1.0f / di;
            float t1 = (mni - oi) * inv;
            float t2 = (mxi - oi) * inv;
            if (t1 > t2) { float t = t1; t1 = t2; t2 = t; }
            if (t1 > tNear) tNear = t1;
            if (t2 < tFar)  tFar  = t2;
            if (tNear > tFar) return false;
        }
    }
    return tFar >= 0.f; // пересечение в положительном направлении
}

// Обновляет кэш построе��. Тяжёлая часть — single-thread-safe (locks mutex).
static void ox_updateBuildingCache(bool full) {
    int frame = ImGui::GetFrameCount();
    if (frame - g_buildingCacheFrame < BUILDING_CACHE_INTERVAL && !g_buildingCache.empty())
        return;
    g_buildingCacheFrame = frame;

    if (!il2cpp_base) return;
    uint64_t statics = ox_getClassStatics(g_buildingPieceTypeInfoRVA, g_buildingPieceKlass, full);
    if (!statics) { if (full) OXLOGW("[LOS] BuildingPiece.statics не резолвится"); return; }

    std::vector<BuildingCacheEntry> newCache;
    int valid = 0, total = 0;

    // Общий приёмник одного BuildingPiece -> world-AABB (с фильтром health>0).
    auto addPiece = [&](uint64_t b) {
        ++total;
        if (!ox_isPtr(b)) return;
        BuildingCacheEntry e;
        e.address = b;
        e.valid = ox_readWorldAABB(b, e.bounds);
        if (!e.valid) return;
        float hp = rpm<float>(b + ox::BP_HEALTH);
        if (hp <= 0.f || !isfinite(hp)) return; // разрушенная/невалидная — пропуск
        newCache.push_back(e);
        ++valid;
    };

    // ── ПЕРВИЧНО: saveList — HashSet<BuildingPiece> (static+0x0) ──
    // FIX bug #1: раньше читалось как List<T> -> всегда пусто -> LOS не работал.
    uint64_t saveList = rpm<uint64_t>(statics + ox::STATIC_BUILDING_SAVELIST);
    int hsSeen = ox_forEachHashSet(saveList, addPiece);
    if (full) OXLOGI("[LOS] saveList(HashSet)=0x%llx seen=%d",
                     (unsigned long long)saveList, hsSeen);

    // ── FALLBACK: saveLookup — Dictionary<int,BuildingPiece> (static+0x8) ──
    // Если HashSet не дал элементов (иная реализация/пусто) — берём словарь.
    if (hsSeen <= 0) {
        uint64_t saveLookup = rpm<uint64_t>(statics + ox::STATIC_BUILDING_SAVELOOKUP);
        int dSeen = ox_forEachDictValues(saveLookup, addPiece);
        if (full) OXLOGI("[LOS] fallback saveLookup(Dict)=0x%llx seen=%d",
                         (unsigned long long)saveLookup, dSeen);
    }

    // Всегда логируем результат раз в ~5 сек чтобы видеть если LOS не работает
    // (кэш пуст = wall check не может ничего блокировать).
    static int s_lastLogFrame = -10000;
    if (full || (frame - s_lastLogFrame > 300)) {
        s_lastLogFrame = frame;
        OXLOGI("[LOS] building cache: valid=%d/%d, active=%d", valid, total, (int)newCache.size());
        if (valid == 0 && total == 0) {
            OXLOGW("[LOS] saveList И saveLookup пусты — wall check физически не может блокировать. "
                   "Возможно клиент не получает данные о постройках от сервера (Mirror NetworkBehaviour).");
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_buildingMutex);
        g_buildingCache = std::move(newCache);
    }
}

// Главная функция LOS-чека. Возвращает true, если путь origin→target свободен
// (не перекрыт никаким BuildingPiece). assumeBuilding=true → считается, что
// невидима. distance = |target - origin|.
static bool ox_lineOfSightClear(const Vector3 &origin, const Vector3 &target, float distance) {
    if (g_buildingCache.empty()) return true; // безblocks не резолвится => считаем видимым
    Vector3 dir = target - origin;
    float len = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    if (len < 0.001f) return true;
    Vector3 dirN = Vector3(dir.x/len, dir.y/len, dir.z/len);

    // Небольшой отступ от камеры: игнорируем стены, в чьём инфлейт-AABB мы
    // фактически стоим (иначе стоя вплотную к стене ВСЁ считалось бы за стеной).
    const float NEAR_EPS = 0.35f;

    std::lock_guard<std::mutex> lock(g_buildingMutex);
    for (const BuildingCacheEntry &e : g_buildingCache) {
        if (!e.valid) continue;
        float tNear, tFar;
        if (!ox_rayAABB(origin, dirN, e.bounds, tNear, tFar)) continue;
        // Блокирует ТОЛЬКО стена, стоящая МЕЖДУ камерой и целью:
        //   tNear в (NEAR_EPS .. distance - TARGET_EPS).
        // tNear <= NEAR_EPS => мы внутри/у самой стены (не считаем перекрытием).
        // tNear >= distance - eps => стена за целью (не мешает).
        if (tNear > NEAR_EPS && tNear < distance - ox::LOS_TARGET_EPS)
            return false;
    }
    return true;
}

// Булева видимость цели: true если path от камеры до точкиBone свободен.
// Используется и в ESP (для окрашивания бокса), и в aimbot для фильтрации.
static bool ox_isTargetVisible(const Vector3 &camPos, const Vector3 &targetPos) {
    float dist = Vector3::Distance(camPos, targetPos);
    if (dist < 0.001f) return true;
    // Без искусственного лимита дальности — ray-AABB дёшево (200 построек × 12
    // сравнений), а aimMaxDistance может быть до 1000м.
    return ox_lineOfSightClear(camPos, targetPos, dist);
}

// Одноразовый runtime-профиль для обновления игры. Запускается кнопкой из меню
// и пишет в обычный eclips_oxide_*.log: реальные адреса, цепочки статиков и полей,
// а также хексы первых игроков. Это не заменяет Il2CppDumper для RVA методов,
// но даёт все runtime offsets, которые нужны ESP/LOS/aim для калибровки версии.
[[maybe_unused]] static std::string g_runtimeDumpStatus = "No runtime dump yet";

[[maybe_unused]] static void ox_dumpRuntimeProfile() {
    oxlog::section("RUNTIME OFFSET DUMP");
    const char *logPath = oxlog::init();

    if (!ox_ensureBase(true)) {
        g_runtimeDumpStatus = "Dump failed: libil2cpp.so is not loaded";
        OXLOGE("[DUMP] %s", g_runtimeDumpStatus.c_str());
        return;
    }

    OXLOGI("[DUMP] PID=%d base=0x%llx log=%s", game_pid,
           (unsigned long long)il2cpp_base, logPath ? logPath : "<none>");
    OXLOGI("[DUMP] RVA PlayerManager_TypeInfo=0x%llx BuildingPiece_TypeInfo=0x%llx",
           (unsigned long long)g_playerManagerTypeInfoRVA,
           (unsigned long long)g_buildingPieceTypeInfoRVA);
    OXLOGI("[DUMP] PlayerManager fields: transform=0x%llx mouseLook=0x%llx raycastManager=0x%llx vitals=0x%llx local=0x%llx team=0x%llx",
           (unsigned long long)ox::PM_TRANSFORM,
           (unsigned long long)ox::PM_MOUSELOOK,
           (unsigned long long)ox::PM_RAYCASTMANAGER,
           (unsigned long long)ox::PM_VITALS,
           (unsigned long long)ox::PM_IS_LOCAL_PLAYER,
           (unsigned long long)ox::PM_TEAM_NAME);
    OXLOGI("[DUMP] MouseLook fields: lookRoot=0x%llx pitch=0x%llx yaw=0x%llx",
           (unsigned long long)ox::ML_LOOKROOT,
           (unsigned long long)ox::ML_PITCH,
           (unsigned long long)ox::ML_YAW);
    OXLOGI("[DUMP] BuildingPiece fields: worldPos=0x%llx bounds=0x%llx health=0x%llx saveList(static)=0x%llx",
           (unsigned long long)ox::BP_CORRECT_POSITION,
           (unsigned long long)ox::BP_BOUNDS,
           (unsigned long long)ox::BP_HEALTH,
           (unsigned long long)ox::STATIC_BUILDING_SAVELIST);

    // Полная карта модулей и проверка ELF magic помогают сразу увидеть неверную базу.
    ox_moduleInfo();

    uint64_t playerStatics = ox_getStatics(true);
    if (!playerStatics) {
        g_runtimeDumpStatus = "Dump partial: PlayerManager statics unresolved";
        OXLOGE("[DUMP] %s", g_runtimeDumpStatus.c_str());
        return;
    }

    PlayerListView players = ox_scanForPlayerList(playerStatics, true);
    if (!players.ok) {
        g_runtimeDumpStatus = "Dump partial: activePlayerList unresolved";
        OXLOGE("[DUMP] %s", g_runtimeDumpStatus.c_str());
        return;
    }

    OXLOGI("[DUMP] players: static+0x%llx list=0x%llx elements=0x%llx count=%d",
           (unsigned long long)players.statOff,
           (unsigned long long)players.container,
           (unsigned long long)players.elems,
           players.count);

    CameraView cam = ox_getCamera(players.elems, players.count, true);
    OXLOGI("[DUMP] camera: valid=%d local=0x%llx pos=(%.3f,%.3f,%.3f) fov=%.3f",
           (int)cam.valid, (unsigned long long)g_localPlayer,
           cam.pos.x, cam.pos.y, cam.pos.z, cam.fovH);

    if (ox_isPtr(g_localPlayer)) {
        uint64_t mouseLook = rpm<uint64_t>(g_localPlayer + ox::PM_MOUSELOOK);
        uint64_t raycast = rpm<uint64_t>(g_localPlayer + ox::PM_RAYCASTMANAGER);
        OXLOGI("[DUMP] local MouseLook=0x%llx pitch=%.3f yaw=%.3f",
               (unsigned long long)mouseLook,
               rpm<float>(mouseLook + ox::ML_PITCH),
               rpm<float>(mouseLook + ox::ML_YAW));
        OXLOGI("[DUMP] RaycastManager=0x%llx camera=0x%llx rayLength=%.3f aimRayLength=%.3f mask=0x%x aimMask=0x%x",
               (unsigned long long)raycast,
               (unsigned long long)rpm<uint64_t>(raycast + ox::RM_WORLD_CAMERA),
               rpm<float>(raycast + ox::RM_RAY_LENGTH),
               rpm<float>(raycast + ox::RM_AIM_RAY_LENGTH),
               rpm<int>(raycast + ox::RM_LAYER_MASK),
               rpm<int>(raycast + ox::RM_AIM_LAYER_MASK));
    }

    oxlog::section("RUNTIME OFFSET DUMP - PLAYER SAMPLES");
    for (int i = 0, dumped = 0; i < players.count && dumped < 3; ++i) {
        uint64_t player = rpm<uint64_t>(players.elems + i * 8);
        if (!ox_isPtr(player)) continue;
        ox_dumpPlayer(i, player);
        ++dumped;
    }

    uint64_t buildingStatics = ox_getClassStatics(g_buildingPieceTypeInfoRVA, g_buildingPieceKlass, true);
    uint64_t saveList   = buildingStatics ? rpm<uint64_t>(buildingStatics + ox::STATIC_BUILDING_SAVELIST) : 0;
    uint64_t saveLookup = buildingStatics ? rpm<uint64_t>(buildingStatics + ox::STATIC_BUILDING_SAVELOOKUP) : 0;
    // saveList — HashSet<BuildingPiece>, НЕ List. Считаем корректным обходом.
    int hsCount = 0, dCount = 0;
    ox_forEachHashSet(saveList, [&](uint64_t){ ++hsCount; });
    ox_forEachDictValues(saveLookup, [&](uint64_t){ ++dCount; });
    OXLOGI("[DUMP] BuildingPiece: statics=0x%llx saveList(HashSet)=0x%llx count=%d | saveLookup(Dict)=0x%llx count=%d",
           (unsigned long long)buildingStatics,
           (unsigned long long)saveList, hsCount,
           (unsigned long long)saveLookup, dCount);

    ox_updateBuildingCache(true);
    size_t cachedBuildings = 0;
    {
        std::lock_guard<std::mutex> lock(g_buildingMutex);
        cachedBuildings = g_buildingCache.size();
    }
    OXLOGI("[DUMP] Building LOS cache=%zu", cachedBuildings);

    g_runtimeDumpStatus = "Dump saved: " + std::string(logPath ? logPath : "/sdcard/Download");
    OXLOGI("[DUMP] COMPLETE: %s", g_runtimeDumpStatus.c_str());
}

// playerPosition = FEET (lastTickPosition, server-authoritative). Bones — offsets
// ВВЕРХ от ног. Стоящая модель: макушка ~1.85, центр головы ~1.62, шея ~1.45,
// грудь ~1.30. bug #4: при aimCrouchSafe опускаем на crouch-дельту (ручной режим,
// т.к. реальное состояние присед недоступно внешнему читу — см. коммент выше).
static Vector3 ox_aimPoint(Vector3 playerPosition) {
    float h;
    switch (aimcurbone) {
        case 0: h = 1.62f; break; // head (центр головы, а не макушка — надёжнее)
        case 1: h = 1.45f; break; // neck
        default: h = 1.30f; break; // body/chest
    }
    if (aimCrouchSafe) h -= aimCrouchDrop;
    return playerPosition + Vector3(0.f, h, 0.f);
}

// Selects the target nearest the crosshair and adjusts the local MouseLook angles.
static void ox_runAimbot(const CameraView& cam, const std::vector<PlayerCacheData>& players) {
    if (!aimm || !cam.valid || !ox_isPtr(g_localPlayer)) return;

    // bug #5: ADS-only. cam.aiming вычислен из живого FOV (FPManager.kYl<_kSQ*ratio).
    // Если включено «Only when ADS» и игрок НЕ целится — аимбот молчит.
    if (aimOnlyADS && !cam.aiming) return;

    const ImVec2 screenCenter((float)abs_ScreenX * 0.5f, (float)abs_ScreenY * 0.5f);
    const float maxFovSquared = aimFovPixels * aimFovPixels;
    Vector3 bestPoint;
    float bestFovSquared = maxFovSquared;
    bool foundTarget = false;

    for (const PlayerCacheData& player : players) {
        if (!ox_isPtr(player.address)) continue;
        // Toggle-controlled skip сокомандников. player.teamName хранит стабильный
        // team-key (см. UpdatePlayerCache / ox_teamKey). Ключ имеет префикс:
        // "N:<name>", "I:<pfTeamId>", или "P:<wnn_ptr>" (fallback pointer-eq).
        if (aimIgnoreTeammates && !g_localTeamName.empty() && !player.teamName.empty() &&
            player.teamName == g_localTeamName) continue;
        Vector3 livePosition = player.position;
        Vector3 point = ox_aimPoint(livePosition);
        float distance = Vector3::Distance(cam.pos, point);
        if (aimMaxDistance > 0.f && distance > aimMaxDistance) continue;

        if (aimPrediction && aimProjectileSpeed > 1.0f) {
            float leadTime = distance / aimProjectileSpeed + aimLatencyMs * 0.001f;
            leadTime = ::clamp<float>(leadTime, 0.0f, aimMaxLeadTime);
            point += player.velocity * leadTime;
        }

        // --- LOS-чек (аим не через стены) ---
        // Внимание: player.losBlocked считается в ESP-секции для ТЕЛА игрока
        // (livePos) и используется только для окраски бокса. В aimbot проверяем
        // реальный point (head/neck/body) — т.к. голова может торчать из-за
        // стены при перекрытом теле.
        bool blocked = false;
        if (aimCheckWalls) {
            blocked = !ox_isTargetVisible(cam.pos, point);
        }
        if (blocked) continue;

        bool onScreen = false;
        ImVec2 screen = w2s_angular(point, cam, &onScreen);
        if (aimvisible && !onScreen) continue;
        float dx = screen.x - screenCenter.x;
        float dy = screen.y - screenCenter.y;
        float fovSquared = dx * dx + dy * dy;
        if (!onScreen || fovSquared > bestFovSquared) continue;

        bestFovSquared = fovSquared;
        bestPoint = point;
        foundTarget = true;
    }

    if (!foundTarget) return;

    uint64_t mouseLook = rpm<uint64_t>(g_localPlayer + ox::PM_MOUSELOOK);
    if (!ox_isPtr(mouseLook)) return;

    Vector3 delta = bestPoint - cam.pos;
    float horizontalDistance = sqrtf(delta.x * delta.x + delta.z * delta.z);
    if (horizontalDistance < 0.001f) return;

    const float radToDeg = 57.2957795131f;
    // MouseLook stores positive pitch when looking down, opposite Unity world Y.
    float targetPitch = -atan2f(delta.y, horizontalDistance) * radToDeg;
    float targetYaw = atan2f(delta.x, delta.z) * radToDeg;
    float currentPitch = rpm<float>(mouseLook + ox::ML_PITCH);
    float currentYaw = rpm<float>(mouseLook + ox::ML_YAW);
    if (!isfinite(currentPitch) || !isfinite(currentYaw)) return;

    float smooth = aimSmooth < 1.0f ? 1.0f : aimSmooth;
    float nextPitch = currentPitch + (targetPitch - currentPitch) / smooth;
    float nextYaw = currentYaw + ox_normalizeAngle(targetYaw - currentYaw) / smooth;
    nextPitch = ::clamp<float>(nextPitch, -80.0f, 80.0f);
    nextYaw = ox_normalizeAngle(nextYaw);

    wpm<float>(mouseLook + ox::ML_PITCH, nextPitch);
    wpm<float>(mouseLook + ox::ML_YAW, nextYaw);
}

// функция обновления кэша игроков (Oxide)
void UpdatePlayerCache() {
    if (!(espdraw || aimm)) {
        cache_needs_update = true;
        return;
    }

    // Пока фоновый startup thread ещё не закончил (типично 10-60 сек на
    // защищённом Oxide), не запускаем СВОЙ синхронный auto-resolve — он
    // блокировал бы главный тред и повторял бы работу. Меню продолжает
    // рисоваться. Как только g_startupDone=true — обычный ход.
    if (g_startupInProgress.load(std::memory_order_relaxed)) {
        cache_needs_update = true;
        return;
    }

    oxlog::frameCounter++;
    bool full = oxlog::shouldLogFull();   // подробно — первые N кадров
    static int diag = 0;
    bool logNow = full || ((diag++ % 120) == 0); // ��альше — сводка раз в ~2c

    if (full) {
        oxlog::section("FRAME UpdatePlayerCache");
        OXLOGT("il2cpp_base=0x%llx screen=%dx%d",
               (unsigned long long)il2cpp_base, ::abs_ScreenX, ::abs_ScreenY);
    }

    // Ленивый резолв базы: игра могла ещё не загрузить libil2cpp.so на старте.
    if (!ox_ensureBase(full || logNow)) {
        if (logNow) OXLOGW("Ждём загрузку %s игрой... (base=0)", ox::MODULE);
        cache_needs_update = true;
        return;
    }

    // static_fields резолвим один раз �� кэшируем. При смене базы (перезапуск
    // игры / другой PID) кэ�� инвалидируется.
    static uint64_t statics_cached = 0;
    static uint64_t base_at_cache  = 0;
    static uint64_t listOff_cached = ~0ULL;
    if (base_at_cache != il2cpp_base) {
        base_at_cache = il2cpp_base;
        statics_cached = 0;
        listOff_cached = ~0ULL;
        g_playerManagerKlass = 0; // база сменилась (рестарт игры) — пере-резолв klass
        g_buildingPieceKlass = 0;
    }
    if (!statics_cached) statics_cached = ox_getStatics(full);
    if (!statics_cached) {
        if (logNow) OXLOGW("static_fields не резолвится (base=0x%llx)", (unsigned long long)il2cpp_base);
        cache_needs_update = true;
        return;
    }

    // Список игроков ищем авто-сканом; кэшируем найденный оффсет (см. выше).
    PlayerListView plist;
    if (listOff_cached != ~0ULL) {
        // Быстрый путь: пересчитываем по изв��стному оффсету.
        uint64_t c = rpm<uint64_t>(statics_cached + listOff_cached);
        int cnt, homo, checked; uint64_t elems; char cls[96];
        if (ox_evalContainer(c, elems, cnt, cls, sizeof(cls), homo, checked) && homo >= 2) {
            plist.ok = true; plist.elems = elems; plist.count = cnt;
            plist.container = c; plist.statOff = listOff_cached;
        } else {
            plist.ok = false; listOff_cached = ~0ULL; // сбросить — перескан
        }
    }
    if (!plist.ok) {
        plist = ox_scanForPlayerList(statics_cached, full || logNow);
        if (plist.ok) {
            listOff_cached = plist.statOff;
            oxlog::armFull(20); // список найден (в��шли в матч) — писать подробно 20 кадров
            full = oxlog::shouldLogFull();
        }
    }
    if (!plist.ok) { cache_needs_update = true; return; }

    uint64_t elems = plist.elems;
    int size = plist.count;

    // Дамп первых игроков — только в под��обных кадрах, для поиска оффсетов HP/позиции.
    if (full) {
        oxlog::section("PLAYER DUMP (offset discovery)");
        for (int i = 0; i < size && i < 2; i++) {
            uint64_t p = rpm<uint64_t>(elems + i * 8);
            if (ox_isPtr(p)) ox_dumpPlayer(i, p);
        }
    }

    CameraView cam = ox_getCamera(elems, size, full);

    std::vector<PlayerCacheData> new_cache;
    double sampleTime = ImGui::GetTime();
    int valid = 0;
    int checkedHP = 0, skippedHP = 0, skippedPos = 0, skippedLocal = 0, skippedTeam = 0, skippedVis = 0;

    if (full) oxlog::section("PLAYER SCAN");

    for (int i = 0; i < size; i++) {
        uint64_t player = rpm<uint64_t>(elems + i * 8);
        if (!ox_isPtr(player)) continue;

        float hp = ox_readHP(player, full && i < 2);
        checkedHP++;
        if (hp < ox::HP_MIN || hp > ox::HP_MAX) {
            skippedHP++;
            if (full && i < 8) OXLOGT("player[%d]=0x%llx откинут: hp=%.2f вне [%.0f..%.0f]",
                                      i, (unsigned long long)player, hp, ox::HP_MIN, ox::HP_MAX);
            continue;
        }

        Vector3 pos = ox_readPos(player, full && i < 2);
        if (pos == Vector3::Zero()) { skippedPos++; continue; }

        // Себя не рисуем. Сверяем по указателю на локального игрока (найден в ox_getCamera),
        // т.к. прямого флага isLocalPlayer в этом билде нет.
        if (player == g_localPlayer) { skippedLocal++; continue; } // все остальные всегда видны

        // FIX bug #2: стабильный team-key (raw UTF-16 teamName / pf.teamId / pf-ptr).
        // pf-указатель как fallback — все тимейты имеют один shared pf, разные — разные pf.
        std::string teamKey = ox_teamKey(player);
        // Диагностика: первые 4 игрока за проход логируем team-ключ (одна строка).
        if (full && checkedHP < 4) {
            OXLOGI("[team] player[%d]=0x%llx teamKey='%s' localKey='%s' match=%d",
                   checkedHP, (unsigned long long)player,
                   teamKey.c_str(), g_localTeamName.c_str(),
                   (int)(!g_localTeamName.empty() && !teamKey.empty() && teamKey == g_localTeamName));
        }
        if (espHideTeammates && !g_localTeamName.empty() && !teamKey.empty() &&
            teamKey == g_localTeamName) {
            skippedTeam++;
            continue;
        }

        PlayerCacheData data{};
        data.address = player;
        data.health  = (int)hp;
        data.armor   = 0;
        data.teamId  = 0;
        data.position = pos;
        data.sampledPosition = pos;
        data.sampleTime = sampleTime;
        data.teamName = teamKey;   // хранит team-key для aimbot-фильтра
        data.velocity = Vector3::Zero();

        for (const PlayerCacheData& previous : cached_players) {
            if (previous.address != player) continue;
            double dt = sampleTime - previous.sampleTime;
            if (dt > 0.01 && dt < 2.0) {
                Vector3 measured = (pos - previous.sampledPosition) / (float)dt;
                float speed = measured.magnitude();
                if (isfinite(speed) && speed <= 30.0f)
                    data.velocity = previous.velocity * 0.65f + measured * 0.35f;
            }
            break;
        }
        data.distance = cam.valid ? Vector3::Distance(cam.pos, pos) : 0.f;

        // Огромный радиус по умолчанию (5000 м). 0 = вообще без лимита.
        if (espMaxDistance > 0.f && data.distance > espMaxDistance) { skippedVis++; continue; }

        w2sLogDetail = (full && valid < 3); // детальная трассировка для первых целей
        // pos = уровень глаз/головы → низ бокса опускаем к ногам, верх чуть выше макушки.
        data.w2sBottom = w2s_angular(pos - Vector3(0, ox::BOX_FOOT, 0), cam, &data.isVisibleBottom);
        data.w2sTop    = w2s_angular(pos + Vector3(0, ox::BOX_HEAD, 0), cam, &data.isVisibleTop);
        w2sLogDetail = false;

        data.name = "";
        data.weapon = "";
        data.viewPtr = 0;
        data.hasSkeletonData = false;

        if (logNow && valid < 5)
            OXLOGI("ЦЕЛЬ[%d]=0x%llx hp=%.0f pos=(%.2f,%.2f,%.2f) dist=%.1f visB=%d visT=%d w2sB=(%.0f,%.0f) w2sT=(%.0f,%.0f)",
                   i, (unsigned long long)player, hp, pos.x, pos.y, pos.z, data.distance,
                   (int)data.isVisibleBottom, (int)data.isVisibleTop,
                   data.w2sBottom.x, data.w2sBottom.y, data.w2sTop.x, data.w2sTop.y);

        // 360° counter fix: НЕ отсеиваем игроков вне frustum'а. Просто помечаем
        // visibility, downstream ESP-render сам скипнет отрисовку если оба
        // isVisibleBottom/Top=false. Счётчик g_enemyCount теперь считает ВСЕХ
        // живых игроков в матче, не только тех кого мы видим.
        new_cache.push_back(data);
        valid++;
    }

    if (logNow)
        OXLOGI("ИТОГ: static+0x%llx elems=0x%llx count=%d camValid=%d | проверено HP=%d, откинуто(hp=%d,pos=%d,local=%d,team=%d,vis=%d) -> целей=%d",
               (unsigned long long)plist.statOff, (unsigned long long)elems, size,
               (int)cam.valid, checkedHP, skippedHP, skippedPos, skippedLocal, skippedTeam, skippedVis, valid);

    {
        std::lock_guard<std::mutex> lock(player_data_mutex);
        cached_players = std::move(new_cache);
        cache_needs_update = false;
        g_enemyCount = (int)cached_players.size();
    }
}

// ============================================================================
//  Стартовый диагностический дамп. Пишется в файл лога
//  (/sdcard/Download/eclips_oxide_<время>.log) ОДИН раз при запуске,
//  НЕЗАВИСИМО от verbose-режима меню (oxlog::enabled по умолчанию true).
//  Содержит всё для отладки оффсетов после апдейта игры: версию
//  il2cpp-метаданных, базы модулей, результат авто-резолва TypeInfo
//  и полную таблицу оффсетов из oxide_offsets.h.
// ============================================================================

// Лёгкий пробник версии метаданных: находит первый валидный заголовок
// 0xFAB11BAF и возвращает version/size БЕЗ копирования blob в файл.
static bool ox_probeMetadataVersion(uint32_t &outVersion, size_t &outSize) {
    outVersion = 0; outSize = 0;
    if (game_pid <= 0) return false;
    std::vector<OxMapRange> maps;
    ox_collectMetadataMaps(maps, 1024ULL * 1024 * 1024);
    const size_t chunkSize = 1024 * 1024;
    std::vector<uint8_t> buffer(chunkSize + 4);
    for (const OxMapRange &map : maps) {
        size_t carry = 0;
        for (uint64_t at = map.start; at < map.end;) {
            size_t want = (size_t)std::min<uint64_t>(chunkSize, map.end - at);
            if (!vm_readv(at, buffer.data() + carry, want)) { carry = 0; at += want; continue; }
            size_t total = carry + want;
            for (size_t i = 0; i + sizeof(uint32_t) <= total; i += sizeof(uint32_t)) {
                uint32_t magic = 0;
                memcpy(&magic, buffer.data() + i, sizeof(magic));
                if (magic != IL2CPP_METADATA_MAGIC) continue;
                uint64_t candidate = at - carry + i;
                size_t sz = 0; uint32_t ver = 0;
                if (ox_validateMetadataHeader(candidate, map.end, sz, ver)) {
                    outVersion = ver; outSize = sz;
                    return true;
                }
            }
            carry = std::min(sizeof(uint32_t) - 1, total);
            memmove(buffer.data(), buffer.data() + total - carry, carry);
            at += want;
        }
    }
    return false;
}

static void ox_logStartupDiagnostics() {
    oxlog::section("STARTUP DIAGNOSTICS");
    OXLOGI("build: %s %s", __DATE__, __TIME__);
    OXLOGI("package=%s module=%s native=%s", ox::PACKAGE, ox::MODULE, ox::MODULE_NATIVE);
    OXLOGI("game_pid=%d libil2cpp_mapped=%s", game_pid,
           (game_pid > 0 && pidHasModule(game_pid, ox::MODULE)) ? "yes" : "no");

    // --- Базы модулей ---
    if (!ox_ensureBase(true))
        OXLOGW("il2cpp base ещё не готова (игра догрузит либу) — будет до-резолв в кадре");
    OXLOGI("il2cpp_base = 0x%llx", (unsigned long long)il2cpp_base);
    uint64_t nativeBase = get_module_base_real(ox::MODULE_NATIVE);
    OXLOGI("%s base = 0x%llx", ox::MODULE_NATIVE, (unsigned long long)nativeBase);

    // --- Версия il2cpp метаданных (лёгкий пробник, без дампа файла) ---
    uint32_t metaVer = 0; size_t metaSize = 0;
    if (ox_probeMetadataVersion(metaVer, metaSize))
        OXLOGI("il2cpp metadata: version=%u size=%zu (0x%zx)", metaVer, metaSize, metaSize);
    else
        OXLOGW("il2cpp metadata header не найден (зашифрован/ещё не расшифрован)");

    // --- Авто-резолв TypeInfo (перекрывает fallback из хедера) ---
    OXLOGI("TypeInfo fallback из хедера: PM=0x%llx BP=0x%llx PV=0x%llx",
           (unsigned long long)ox::PLAYERMANAGER_TYPEINFO,
           (unsigned long long)ox::BUILDINGPIECE_TYPEINFO,
           (unsigned long long)ox::PLAYERVITALS_TYPEINFO);
    if (il2cpp_base) {
        bool ok = ox_autoResolveTypeInfos(true);
        OXLOGI("TypeInfo auto-resolve: %s -> PM=0x%llx BP=0x%llx",
               ok ? "OK" : "PARTIAL/FAIL",
               (unsigned long long)g_playerManagerTypeInfoRVA,
               (unsigned long long)g_buildingPieceTypeInfoRVA);
    } else {
        OXLOGW("TypeInfo auto-resolve пропущен: нет il2cpp base");
    }

    // --- Полная таблица оффсетов из oxide_offsets.h ---
    oxlog::section("OFFSETS TABLE (oxide_offsets.h)");
    #define OXDUMP(name) OXLOGI("  %-24s = 0x%llx", #name, (unsigned long long)ox::name)
    OXDUMP(CLASS_STATIC_FIELDS); OXDUMP(STATIC_ACTIVE_LIST); OXDUMP(STATIC_BUILDING_SAVELIST);
    OXDUMP(CLASS_NAME); OXDUMP(CLASS_NAMESPACE);
    OXDUMP(LIST_ITEMS); OXDUMP(LIST_SIZE); OXDUMP(ARRAY_COUNT); OXDUMP(ARRAY_DATA);
    OXDUMP(PM_TRANSFORM); OXDUMP(PM_MOUSELOOK); OXDUMP(PM_RAYCASTMANAGER);
    OXDUMP(PM_IS_LOCAL_PLAYER); OXDUMP(PM_VITALS); OXDUMP(PM_TEAM_ID); OXDUMP(PM_TEAM_NAME);
    OXDUMP(ML_ANGLES); OXDUMP(ML_LOOKROOT); OXDUMP(ML_PITCH); OXDUMP(ML_YAW);
    OXDUMP(RM_WORLD_CAMERA); OXDUMP(RM_RAY_LENGTH); OXDUMP(RM_AIM_RAY_LENGTH);
    OXDUMP(RM_LAYER_MASK); OXDUMP(RM_AIM_LAYER_MASK);
    OXDUMP(VITALS_HEALTH);
    OXDUMP(OBJ_CACHED_PTR); OXDUMP(TR_INT_MATRIXPTR); OXDUMP(TR_MATRIX_WORLDPOS);
    OXDUMP(TR_MATRIX_ROT); OXDUMP(TR_MATRIX_SCALE);
    OXDUMP(BP_CORRECT_POSITION); OXDUMP(BP_BOUNDS); OXDUMP(BP_CORRECT_ROTATION);
    OXDUMP(BP_ADDITIONAL_BOUNDS); OXDUMP(BP_M_GRADE); OXDUMP(BP_GRADE_HOLDER);
    OXDUMP(BP_PIECE_NAME); OXDUMP(BP_ID); OXDUMP(BP_HEALTH); OXDUMP(BP_MAXHEALTH);
    #undef OXDUMP

    // Полный runtime-sweep всех известных TypeInfo — 11 классов ×
    // ox_findAscii+ox_scanClassByName по writable/anon памяти. При стабильной
    // шапке это чистый лаг; включается только под OX_DEEP_DIAG (при апдейте
    // libil2cpp.so, когда действительно нужны свежие RVA).
#if OX_DEEP_DIAG
    if (il2cpp_base) ox_sweepAllTypeInfos(true);
    else OXLOGW("[sweep] пропущен: il2cpp_base ещё не готова");
#else
    OXLOGI("[sweep] пропущен (OX_DEEP_DIAG=0). Включить: -DOX_DEEP_DIAG=1");
#endif

    OXLOGI("STARTUP DIAGNOSTICS завершён");
}

int main(int argc, char *argv[]) {
    oxlog::init();
    oxlog::section("STARTUP");
    OXLOGI("Eclips Oxide за��ущен, uid=%d, pid=%d", (int)getuid(), (int)getpid());

	// Предпочитаем ГЛАВНЫЙ процесс (где замаплен libil2cpp.so), а не сервисный.
	game_pid = getGameProcessID(ox::PACKAGE, ox::MODULE);
	if (game_pid <= 0) game_pid = getProcessID(ox::PACKAGE); // fallback: любой совпавший
	if (game_pid <= 0) {
		OXLOGW("Игра пока не запущена: %s", ox::PACKAGE);
		printf("Waiting for Oxide to start: %s\n", ox::PACKAGE);
		fflush(stdout);
		while (main_thread_flag && game_pid <= 0) {
			sleep(1);
			game_pid = getGameProcessID(ox::PACKAGE, ox::MODULE);
			if (game_pid <= 0) game_pid = getProcessID(ox::PACKAGE);
		}
		if (!main_thread_flag) return -1;
		OXLOGI("Игра найдена после ожидания, pid=%d", game_pid);
	}
	OXLOGI("PID игры '%s' = %d (%s libil2cpp)", ox::PACKAGE, game_pid,
	       pidHasModule(game_pid, ox::MODULE) ? "есть" : "пока нет");

    screen_config();
    ::abs_ScreenX = (displayInfo.height > displayInfo.width ? displayInfo.height : displayInfo.width);
    ::abs_ScreenY = (displayInfo.height < displayInfo.width ? displayInfo.height : displayInfo.width);
    OXLOGI("Экран: raw %dx%d, ориентация=%d -> abs %dx%d",
           displayInfo.width, displayInfo.height, displayInfo.orientation,
           ::abs_ScreenX, ::abs_ScreenY);
    
    ::native_window_screen_x = (displayInfo.height > displayInfo.width ? displayInfo.height : displayInfo.width);
    ::native_window_screen_y = (displayInfo.height < displayInfo.width ? displayInfo.height : displayInfo.width);
    
    if (!initGUI_draw(native_window_screen_x, native_window_screen_y, true)) {
		OXLOGE("Не удалось создать окно или инициализировать ImGui/EGL");
		fprintf(stderr, "ERROR: overlay initialization failed (window, EGL, or ImGui)\n");
        return -1;
    }
    
    if (!InitLua()) {
        printf("Failed to init Lua\n");
    }

	// RVA из script.json отсчитываются от базы модуля = самый нижний маппинг (ELF header).
	// Пробуем сразу; если игра ещё не подгрузила либу — база до-резолвится в цикле (ox_ensureBase).
	oxlog::section("MODULE BASE");
	il2cpp_base = get_module_base_real(ox::MODULE);
	OXLOGI("%s стартовая база = 0x%llx%s", ox::MODULE,
	       (unsigned long long)il2cpp_base,
	       il2cpp_base ? "" : " (пока не загруж��н — дождёмся в цикле)");
	if (!il2cpp_base) dump_maps_matching("il2cpp", 12);

    // Логи, dump метадаты и всё что чит пишет наружу — в одну папку.
    OXLOGI("Артефакты в: %s", oxlog::activeDir());

    Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);

    // Полный стартовый диагностический дамп + TypeInfo auto-resolve вынесены в
    // фоновый поток, чтобы главный цикл рендера запустился с первого кадра, а
    // меню появилось сразу — до этой правки клиент подвисал минуты на сканах
    // памяти (см. лог erafox: обрывался на "TypeInfo fallback из хедера: ...").
    //
    // Второе издание: после первой полной диагностики поток НЕ выходит, а спит
    // 20 сек и повторяет auto-resolve пока хоть один класс не нашёлся. Это
    // покрывает кейс «чит запущен раньше входа в матч»: в лобби IL2Cpp lazy-load
    // ещё не тронул PlayerManager, строк в памяти нет — но как только игрок
    // зайдёт в матч и Unity создаст класс, следующий проход через 20 сек его
    // подхватит, чит не надо перезапускать. Максимум 45 попыток ≈ 15 мин.
    if (!g_startupThreadStarted.exchange(true)) {
        std::thread([](){
            g_startupInProgress = true;
            ox_setStartupStatus("startup diagnostics...");
            OXLOGI("[startup] фоновая диагностика стартовала (thread)");

            // ONLY path: fast-seed через MetadataCache-цепочку. Никаких сканов
            // памяти игры — watchdog CatsBit'а убивает игру за объём vm_readv.
            // Пробуем каждые 3 секунды пока Class::Init не прогонится (напр.
            // при загрузке лобби/матча).
            ox_setStartupStatus("fast-seed from MetadataCache...");
            const int MAX_TRIES = 600;  // 30 минут попыток (Class::Init только при входе в матч)
            for (int i = 0; i < MAX_TRIES; ++i) {
                if (!main_thread_flag) { OXLOGI("[startup] прерван"); break; }
                if (ox_fastSeedTypeInfoFromHeader()) {
                    OXLOGI("[startup] fast-seed OK на попытке #%d (PM=0x%llx BP=0x%llx)",
                           i, (unsigned long long)g_playerManagerKlass,
                           (unsigned long long)g_buildingPieceKlass);
                    ox_setStartupStatus("ready (fast-seed)");
                    g_startupInProgress = false;
                    g_startupDone = true;
                    return;
                }
                char buf[80];
                snprintf(buf, sizeof(buf), "waiting for Class::Init (%d/%d)...", i+1, MAX_TRIES);
                ox_setStartupStatus(buf);
                // Логируем в info только раз в минуту (каждая 20-я попытка) —
                // debug-логи неудач всё равно идут в DEBUG уровень.
                if (i > 0 && i % 20 == 0) {
                    OXLOGI("[startup] жду Class::Init: %d/%d попыток (зайди в матч)", i, MAX_TRIES);
                }
                for (int s = 0; s < 3 && main_thread_flag; ++s) sleep(1);
            }
            OXLOGW("[startup] fast-seed не сработал за %d попыток. Классы не найдены.", MAX_TRIES);
            ox_setStartupStatus("failed (Class::Init не прогнан)");
            g_startupInProgress = false;
            g_startupDone = true;
            return;
            // === СТАРАЯ scan-based ветка отключена: убивает игру ===
            /*
            ox_setScanDeadline(90);
            ox_logStartupDiagnostics();
            ox_clearScanDeadline();
            g_startupInProgress = false;

            // Если TypeInfo сразу нашлись — работа окончена.
            if (g_playerManagerKlass || g_buildingPieceKlass) {
                ox_setStartupStatus("ready");
                g_startupDone = true;
                OXLOGI("[startup] TypeInfo найдены с первого прохода");
                return;
            }

            // Иначе входим в цикл повторных попыток. UpdatePlayerCache
            // продолжает работать (мы вышли из g_startupInProgress),
            // просто до успеха он видит g_playerManagerKlass=0 и не пытается
            // читать статики. Меню при этом полностью функционально.
            const int MAX_RETRIES = 45;
            const int SLEEP_SEC   = 20;
            for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
                if (!main_thread_flag) { OXLOGI("[startup] main остановлен, retry-loop выходит"); break; }
                for (int i = 0; i < SLEEP_SEC && main_thread_flag; ++i) sleep(1);
                if (!main_thread_flag) break;

                char buf[96];
                snprintf(buf, sizeof(buf), "retry TypeInfo (%d/%d)...", attempt, MAX_RETRIES);
                ox_setStartupStatus(buf);
                OXLOGI("[startup] retry #%d/%d (жду входа в матч)", attempt, MAX_RETRIES);

                g_startupInProgress = true;
                ox_setScanDeadline(60);
                ox_autoResolveTypeInfos(false);   // full=false — тихий лог, без hex-дампов
                ox_clearScanDeadline();
                g_startupInProgress = false;

                if (g_playerManagerKlass || g_buildingPieceKlass) {
                    ox_setStartupStatus("ready (found on retry)");
                    g_startupDone = true;
                    OXLOGI("[startup] TypeInfo найдены на retry #%d: PM=0x%llx BP=0x%llx",
                           attempt,
                           (unsigned long long)g_playerManagerKlass,
                           (unsigned long long)g_buildingPieceKlass);
                    return;
                }
            }
            // Все попытки исчерпаны — сдаёмся, но клиент продолжает работать
            // (меню, dump кнопка). Пользователь может пере-запустить чит уже
            // из матча.
            ox_setStartupStatus("no match yet (retry exhausted)");
            g_startupDone = true;
            OXLOGW("[startup] %d попыток исчерпано — TypeInfo так и не нашлись. "
                   "Зайди в матч и перезапусти чит.", MAX_RETRIES);
            */
            // === конец отключённой scan-based ветки ===
        }).detach();
    }

    while (main_thread_flag) {
        drawBegin();
        Layout_tick_UI();
        
        ExecuteLuaScripts();
        
        // обновляем кэш только раз в N кадров (интервал регулируется в меню)
        bool cacheUpdatedThisFrame = false;
        // Bug D FIX (respawn drift): если локальный PM протух между обновлениями
        // кэша (респавн + новый PM), форсируем refresh чтобы не строить камеру
        // из мусорной памяти старого умершего PM.
        bool localStale = g_localPlayer && !ox_localPlayerStillValid();
        if (localStale) {
            OXLOGI("[respawn] локальный PM протух между кадрами — форсирую cache refresh");
            g_localPlayer = 0;
            g_localTeamName.clear();
            cache_needs_update = true;
        }
        if (cache_needs_update || (ImGui::GetFrameCount() - last_cache_frame > g_cacheInterval)) {
            UpdatePlayerCache();
            last_cache_frame = ImGui::GetFrameCount();
            cacheUpdatedThisFrame = true;
        }

        // LOS-кэш построек — раз в BUILDING_CACHE_INTERVAL кадров (тяжёлый скан
        // статических полей BuildingPiece.saveList). Нужен и для ESP (цвет), и
        // для aimbot (фильтрация через стену).
        if (aimCheckWalls || espColorByVisibility) {
            ox_updateBuildingCache(oxlog::shouldLogFull());
        }
        
        // рисуем ESP из кэша
        if (espdraw || aimm) {
            std::lock_guard<std::mutex> lock(player_data_mutex);
            ImGui::PushFont(verdana);

            // Камеру пересчитываем КАЖДЫЙ кадр из живого трансформа локального
            // игрока — боксы мгновенно следуют за поворотом камеры (0 задержки).
            CameraView liveCam; liveCam.valid = false;
            if (g_localPlayer) liveCam = ox_buildCameraFromPlayer(g_localPlayer, false);
            int positionInterval = g_positionInterval < 1 ? 1 : g_positionInterval;
            bool refreshPositions = !cacheUpdatedThisFrame &&
                                    (ImGui::GetFrameCount() % positionInterval == 0);

            if (aimm && aimDrawFov) {
                getDrawList()->AddCircle(ImVec2(abs_ScreenX * 0.5f, abs_ScreenY * 0.5f),
                                         aimFovPixels, IM_COL32(255, 255, 255, 140), 96, 1.5f);
            }

            for (auto& player : cached_players) {
                // W2S считаем из СВЕЖЕЙ камеры и СВЕЖЕЙ мировой позиции.
                bool visB = false, visT = false;
                ImVec2 w2sBottom, w2sTop;
                Vector3 livePos = player.position;
                if (liveCam.valid) {
                    // Перечитываем позицию игрока КАЖДЫЙ кадр (дёшево: пара чтений
                    // трансформа), чтобы бокс идеально держался на бегущем игроке
                    // без отставания от тяжёлого скана UpdatePlayerCache.
                    if (refreshPositions && ox_isPtr(player.address)) {
                        Vector3 refreshed = ox_readPos(player.address, false);
                        if (refreshed != Vector3::Zero()) player.position = refreshed;
                    }
                    livePos = player.position;
                    player.distance = Vector3::Distance(liveCam.pos, livePos);
                    w2sBottom = w2s_angular(livePos - Vector3(0, ox::BOX_FOOT, 0), liveCam, &visB);
                    w2sTop    = w2s_angular(livePos + Vector3(0, ox::BOX_HEAD, 0), liveCam, &visT);
                } else {
                    w2sBottom = player.w2sBottom; w2sTop = player.w2sTop;
                    visB = player.isVisibleBottom; visT = player.isVisibleTop;
                }
                if (!visB && !visT) continue;

                // --- LOS-чек для этого игрока (на свежей камере и позиции) ---
                // Дешёвая проверка по кэшу построек: один ray на цель в кадр.
                // FIX: луч к ГРУДИ (feet+1.3), а не к ногам — иначе низкая стена/
                // фундамент у ног ложно красит видимого игрока синим. Считаем
                // видимым, если открыта хотя бы грудь ИЛИ голова.
                if (espColorByVisibility || aimCheckWalls) {
                    if (liveCam.valid) {
                        Vector3 chest = livePos + Vector3(0.f, 1.30f, 0.f);
                        Vector3 head  = livePos + Vector3(0.f, ox::BOX_HEAD, 0.f);
                        bool vis = ox_isTargetVisible(liveCam.pos, chest) ||
                                   ox_isTargetVisible(liveCam.pos, head);
                        player.losBlocked = !vis;
                    } else {
                        player.losBlocked = false;
                    }
                } else {
                    player.losBlocked = false;
                }

                // ================= АВТОРСКИЙ ESP (@xohyw) =================
                // Единый фирменный стиль: угловые скобки-боксы, вертикальный
                // хилбар со скруглением/градиентом, аккуратные подписи с тенью.
                ImDrawList* dl = getDrawList();

                // Геометрия бокса из двух W2S-точек (голова/ноги).
                float projectedHeight = fabsf(w2sTop.y - w2sBottom.y);
                float halfW = projectedHeight * 0.25f;   // half width: 2D box ratio is 0.5 width to height
                float bx0 = (w2sTop.x < w2sBottom.x ? w2sTop.x : w2sBottom.x) - halfW;
                float bx1 = (w2sTop.x > w2sBottom.x ? w2sTop.x : w2sBottom.x) + halfW;
                float by0 = w2sTop.y;      // верх (голова)
                float by1 = w2sBottom.y;   // низ (ноги)
                if (by1 < by0) { float t = by0; by0 = by1; by1 = t; }
                float bw = bx1 - bx0, bh = by1 - by0;
                float cx = (bx0 + bx1) * 0.5f;

                // HP 0..100 и его цвет (зелёный->жёлтый->красный, синий если >100).
                float hp = ::clamp<float>((float)player.health, 0, 100);
                float ht = hp / 100.0f;
                // --- Цвет бокса по видимости (зелёный=видим, синий=за стеной) ---
                // Если espColorByVisibility включён — цвет переопределяется
                // поверх espboxcolor. Зелёный = цель в прямой видимости,
                // синий = путь от камеры перекрыт BuildingPiece. Аккуратно: HP-бар
                // и т.п. остаются со своими цветами — меняется только рамка/фон.
                ImU32 accent;
                if (espColorByVisibility && player.losBlocked) {
                    // Синий — за стеной
                    accent = IM_COL32(80, 150, 255, 255);
                } else if (espColorByVisibility) {
                    // Зелёный — видим напрямую
                    accent = IM_COL32(60, 230, 110, 255);
                } else {
                    accent = ImGui::ColorConvertFloat4ToU32(
                        ImVec4(espboxcolor[0], espboxcolor[1], espboxcolor[2], 1.0f));
                }
                ImU32 shadowC = IM_COL32(0, 0, 0, 170);
                ImColor hpFull(int(90 + 90 * (1 - ht)), int(200 * ht + 40), 70, 255);
                ImColor hpEmpty(210, 60, 55, 255);
                if (player.health > 100) { hpFull = ImColor(90, 160, 255, 255); hpEmpty = hpFull; }

                // Компоненты акцентного цвета (для свечения).
                int aR = (int)((accent >> 0) & 0xFF);
                int aG = (int)((accent >> 8) & 0xFF);
                int aB = (int)((accent >> 16) & 0xFF);
                ImU32 accentGlow = IM_COL32(aR, aG, aB, 60);

                // Хелпер: текст с мягкой тенью (читаемо��ть на любом фоне).
                auto text2 = [&](float tx, float ty, ImU32 col, const char* s) {
                    dl->AddText(ImVec2(tx + 1, ty + 1), shadowC, s);
                    dl->AddText(ImVec2(tx, ty), col, s);
                };
                // Хелпер: текст по центру на полупрозрачной «таблетке»-чипе.
                auto chip = [&](float cxp, float topY, ImU32 txtCol, ImU32 barCol, const char* s) {
                    ImVec2 ts = ImGui::CalcTextSize(s);
                    float padx = 6.f, pady = 2.f;
                    ImVec2 a(cxp - ts.x * 0.5f - padx, topY);
                    ImVec2 b(cxp + ts.x * 0.5f + padx, topY + ts.y + pady * 2);
                    dl->AddRectFilled(a, b, IM_COL32(10, 8, 18, 180), 4.f);
                    if (barCol) dl->AddRectFilled(a, ImVec2(a.x + 3, b.y), barCol, 4.f,
                                                  ImDrawFlags_RoundCornersLeft);
                    dl->AddText(ImVec2(cxp - ts.x * 0.5f, topY + pady), txtCol, s);
                    return b.y;
                };

                if (espbox) {
                    // Заливка с лёгкой вертикальной виньеткой (сверху прозрачнее).
                    if (espfill) {
                        int a = (int)(90.0f * espfillp / 100.0f);
                        dl->AddRectFilledMultiColor(ImVec2(bx0, by0), ImVec2(bx1, by1),
                            IM_COL32(aR, aG, aB, a / 4),  IM_COL32(aR, aG, aB, a / 4),
                            IM_COL32(aR, aG, aB, a),      IM_COL32(aR, aG, aB, a));
                    }
                    // Rounded frame or compact corner brackets with the same accent treatment.
                    float th = espstroke < 1.f ? 2.f : espstroke;
                    float rnd = (bw < bh ? bw : bh) * 0.22f;   // радиус скругления от меньшей стороны
                    ImVec2 a(bx0, by0), b(bx1, by1);
                    if (!espCornerBox) {
                        dl->AddRect(ImVec2(a.x-1, a.y-1), ImVec2(b.x+1, b.y+1), accentGlow, rnd, 0, th + 4);
                        dl->AddRect(a, b, shadowC, rnd, 0, th + 2);
                        dl->AddRect(a, b, accent, rnd, 0, th);
                    } else {
                        float corner = ::clamp<float>((bw < bh ? bw : bh) * espCornerLength, 12.f, 52.f);
                        const ImVec2 darkLines[] = {
                            ImVec2(a.x, a.y), ImVec2(a.x + corner, a.y), ImVec2(a.x, a.y), ImVec2(a.x, a.y + corner),
                            ImVec2(b.x, a.y), ImVec2(b.x - corner, a.y), ImVec2(b.x, a.y), ImVec2(b.x, a.y + corner),
                            ImVec2(a.x, b.y), ImVec2(a.x + corner, b.y), ImVec2(a.x, b.y), ImVec2(a.x, b.y - corner),
                            ImVec2(b.x, b.y), ImVec2(b.x - corner, b.y), ImVec2(b.x, b.y), ImVec2(b.x, b.y - corner),
                        };
                        for (int i = 0; i < IM_ARRAYSIZE(darkLines); i += 2) {
                            dl->AddLine(darkLines[i], darkLines[i + 1], shadowC, th + 3.f);
                            dl->AddLine(darkLines[i], darkLines[i + 1], accent, th);
                        }
                    }
                }

                if (ESP3D && liveCam.valid) {
                    const float halfModelWidth = 0.30f;
                    const Vector3 offsets[8] = {
                        Vector3(-halfModelWidth, -ox::BOX_FOOT, -halfModelWidth),
                        Vector3( halfModelWidth, -ox::BOX_FOOT, -halfModelWidth),
                        Vector3( halfModelWidth, -ox::BOX_FOOT,  halfModelWidth),
                        Vector3(-halfModelWidth, -ox::BOX_FOOT,  halfModelWidth),
                        Vector3(-halfModelWidth,  ox::BOX_HEAD, -halfModelWidth),
                        Vector3( halfModelWidth,  ox::BOX_HEAD, -halfModelWidth),
                        Vector3( halfModelWidth,  ox::BOX_HEAD,  halfModelWidth),
                        Vector3(-halfModelWidth,  ox::BOX_HEAD,  halfModelWidth),
                    };
                    ImVec2 corners[8];
                    bool cornerVisible[8];
                    for (int i = 0; i < 8; ++i)
                        corners[i] = w2s_angular(livePos + offsets[i], liveCam, &cornerVisible[i]);

                    const int edges[][2] = {
                        {0, 1}, {1, 2}, {2, 3}, {3, 0},
                        {4, 5}, {5, 6}, {6, 7}, {7, 4},
                        {0, 4}, {1, 5}, {2, 6}, {3, 7},
                    };
                    const float lineWidth = espstroke < 1.f ? 1.f : espstroke;
                    for (const auto& edge : edges) {
                        if (cornerVisible[edge[0]] && cornerVisible[edge[1]])
                            dl->AddLine(corners[edge[0]], corners[edge[1]], accent, lineWidth);
                    }
                }

                if (esphealth && bh > 4.f) {
                    // Вертикальный хилбар слева от бокса со свечением заливки.
                    float bar = esphpsize < 3.f ? 5.f : esphpsize;
                    float hx1 = bx0 - 7.f;
                    float hx0 = hx1 - bar;
                    float r   = bar * 0.5f;
                    // фон-трек (тёмный, скруглённый)
                    dl->AddRectFilled(ImVec2(hx0 - 1.5f, by0 - 1.5f), ImVec2(hx1 + 1.5f, by1 + 1.5f),
                                      IM_COL32(6, 5, 12, 220), r + 1.f);
                    float fillTop = by1 - bh * ht;
                    // мягкое свечение вокруг заполнения (цветом текущего HP)
                    dl->AddRectFilled(ImVec2(hx0 - 2.5f, fillTop - 1.f), ImVec2(hx1 + 2.5f, by1 + 1.f),
                        IM_COL32((int)(hpFull.Value.x*255), (int)(hpFull.Value.y*255),
                                 (int)(hpFull.Value.z*255), 55), r + 1.f);
                    // само заполнение (градиент опционально)
                    if (esphpgradient) {
                        dl->AddRectFilledMultiColor(ImVec2(hx0, fillTop), ImVec2(hx1, by1),
                            hpFull, hpFull, hpEmpty, hpEmpty);
                    } else {
                        dl->AddRectFilled(ImVec2(hx0, fillTop), ImVec2(hx1, by1), hpFull, r);
                    }
                    // блик сверху заполнения
                    if (bh * ht > 4.f)
                        dl->AddRectFilled(ImVec2(hx0, fillTop), ImVec2(hx1, fillTop + 2.f),
                                          IM_COL32(255, 255, 255, 90), r, ImDrawFlags_RoundCornersTop);
                    // число HP над баром (только если не 100)
                    if (player.health != 100) {
                        char hs[16]; snprintf(hs, sizeof(hs), "%d", (int)hp);
                        ImVec2 s = ImGui::CalcTextSize(hs);
                        text2(hx0 - s.x - 4.f, fillTop - s.y * 0.5f, IM_COL32(255,255,255,255), hs);
                    }
                }

                if (espname && !player.name.empty()) {
                    ImVec2 s = ImGui::CalcTextSize(player.name.c_str());
                    // имя на чипе над боксом + акцентная полоска слева
                    chip(cx, by0 - s.y - 7.f, IM_COL32(255, 255, 255, 255), accent,
                         player.name.c_str());
                    // тонкая акцентная линия по ширине бокса — фирменная деталь
                    dl->AddLine(ImVec2(bx0, by0 - 2.f), ImVec2(bx1, by0 - 2.f), accentGlow, 2.f);
                }

                // Оружие и дистанция — чипами под боксом (стек вниз).
                float infoY = by1 + 4.f;
                if (espweapon && !player.weapon.empty()) {
                    infoY = chip(cx, infoY, IM_COL32(235, 235, 245, 255), 0, player.weapon.c_str()) + 2.f;
                }
                
                // --- Дистанция (чипом под оружием / боксом) ---
                if (espdist && player.distance > 0.f) {
                    char distStr[32];
                    snprintf(distStr, sizeof(distStr), "%.0f m", player.distance);
                    chip(cx, infoY, IM_COL32(170, 205, 235, 255), accent, distStr);
                }

                // --- ESP Lines / Tracers ---
                if (esplines_enabled) {
                    ImU32 lineCol = IM_COL32(
                        (int)(esplines_color[0] * 255),
                        (int)(esplines_color[1] * 255),
                        (int)(esplines_color[2] * 255),
                        220);
                    float sx = (float)abs_ScreenX;
                    float sy = (float)abs_ScreenY;
                    float cx_screen = sx * 0.5f;
                    float cy_screen = sy * 0.5f;
                    float originX, originY;
                    if (esplines_position == 0) {
                        // от нижнего центра экрана
                        originX = cx_screen;
                        originY = sy;
                    } else {
                        // от перекрестия (центр экрана)
                        originX = cx_screen;
                        originY = cy_screen;
                    }
                    // Берём одну W2S-точку целиком: ноги, либо голову у края кадра.
                    ImVec2 target = visB ? w2sBottom : w2sTop;
                    dl->AddLine(ImVec2(originX, originY), target, lineCol, esplines_thickness);
                    dl->AddCircleFilled(target, esplines_thickness + 1.5f, lineCol, 12);
                }
            }

            ox_runAimbot(liveCam, cached_players);
            
            ImGui::PopFont();
        }
        
        drawEnd();
        usleep(1000);
    }   
    
    CloseLua();
    shutdown();
    Touch_Close();
    return 0;
} 

// ============================================================================
//  GLASS UI — визуальный слой меню (dark-glass идиома "Trial Engine v4").
//  Только Dear ImGui-рисование. Ни одна привязка (&espdraw, &aimm, ...) и ни
//  один диапазон/дефолт не тронуты — это чистый рескин поверх тех же данных.
//  Портировано: iOS-тумблеры с пружиной, градиентные слайдеры со свечением
//  и live-значением, сегмент-контролы с едущей акцент-пилюлей, sidebar/bottom
//  навигация с анимированным индикатором, 5 акцент-тем, fade между вкладками,
//  мягкие тени, фрост-панели, watermark-чип, редизайн счётчика врагов.
// ============================================================================
namespace oxui {
    // Акцент-темы (a = ядро, b = свечение). Violet ≈ родная магента билда.
    struct AccentDef { const char* name; ImU32 a, b; };
    static const AccentDef kAccents[] = {
        { "Aurora", IM_COL32( 88,196,255,255), IM_COL32(120,120,255,255) },
        { "Violet", IM_COL32(224,100,220,255), IM_COL32(150, 96,240,255) },
        { "Mint",   IM_COL32( 60,230,180,255), IM_COL32( 70,200,255,255) },
        { "Ember",  IM_COL32(255,150, 74,255), IM_COL32(255, 92,120,255) },
        { "Rose",   IM_COL32(255,110,160,255), IM_COL32(255,150,120,255) },
    };
    static const int kAccentCount = (int)(sizeof(kAccents) / sizeof(kAccents[0]));

    struct Palette {
        ImU32 bg0, bg1;
        ImU32 panel, panelHi;
        ImU32 stroke;
        ImU32 text, textDim, textFaint;
        ImU32 accent, accentHi;
        ImU32 track;
        ImU32 good, warn, danger;
    };

    static Palette MakePalette(int accentIdx) {
        accentIdx = accentIdx < 0 ? 0 : (accentIdx >= kAccentCount ? kAccentCount - 1 : accentIdx);
        Palette p;
        p.bg0       = IM_COL32(18, 16, 28, 255);
        p.bg1       = IM_COL32(10,  9, 18, 255);
        p.panel     = IM_COL32(255, 255, 255, 12);
        p.panelHi   = IM_COL32(255, 255, 255, 22);
        p.stroke    = IM_COL32(255, 255, 255, 26);
        p.text      = IM_COL32(238, 236, 247, 255);
        p.textDim   = IM_COL32(172, 166, 190, 255);
        p.textFaint = IM_COL32(124, 120, 142, 255);
        p.accent    = kAccents[accentIdx].a;
        p.accentHi  = kAccents[accentIdx].b;
        p.track     = IM_COL32(255, 255, 255, 30);
        p.good      = IM_COL32( 74, 222, 150, 255);
        p.warn      = IM_COL32(255, 196,  84, 255);
        p.danger    = IM_COL32(255,  96, 110, 255);
        return p;
    }

    // ---- Легаси-имена: часть кода вне меню могла бы на них ссылаться. Держим
    //      совместимую палитру (не используется новым рендером, но безопасно). --
    static const ImVec4 BG       = ImVec4(0.055f, 0.045f, 0.085f, 0.97f);
    static const ImVec4 PANEL    = ImVec4(0.090f, 0.075f, 0.130f, 0.86f);
    static const ImVec4 PANEL2   = ImVec4(0.140f, 0.115f, 0.200f, 0.92f);
    static const ImVec4 PANEL3   = ImVec4(0.200f, 0.165f, 0.280f, 0.96f);
    static const ImVec4 ACCENT   = ImVec4(0.880f, 0.380f, 0.870f, 1.00f);
    static const ImVec4 ACCENT_D = ImVec4(0.600f, 0.250f, 0.600f, 1.00f);
    static const ImVec4 DANGER   = ImVec4(0.920f, 0.300f, 0.360f, 1.00f);
    static const ImVec4 DANGER_D = ImVec4(0.700f, 0.210f, 0.260f, 1.00f);
    static const ImVec4 TEXT     = ImVec4(0.950f, 0.930f, 0.970f, 1.00f);
    static const ImVec4 TEXT_DIM = ImVec4(0.620f, 0.570f, 0.700f, 1.00f);

    // ---- мелкие хелперы ----
    static inline ImU32 Fade(ImU32 c, float a) {
        ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
        v.w *= (a < 0.f ? 0.f : (a > 1.f ? 1.f : a));
        return ImGui::ColorConvertFloat4ToU32(v);
    }
    static inline ImU32 MixU32(ImU32 x, ImU32 y, float t) {
        t = (t < 0.f ? 0.f : (t > 1.f ? 1.f : t));
        ImVec4 a = ImGui::ColorConvertU32ToFloat4(x);
        ImVec4 b = ImGui::ColorConvertU32ToFloat4(y);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(
            a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
    }
    // Кадро-независимое экспоненц. сглаживание, состояние — в storage окна.
    static float Anim(ImGuiID id, float target, float speed) {
        ImGuiStorage* st = ImGui::GetStateStorage();
        float cur = st->GetFloat(id, target);
        float dt  = ImGui::GetIO().DeltaTime;
        if (dt < 0.f) dt = 0.f; if (dt > 0.1f) dt = 0.1f;
        float k = speed * dt; if (k > 1.f) k = 1.f; if (k < 0.f) k = 0.f;
        cur += (target - cur) * k;
        if (fabsf(target - cur) < 0.0009f) cur = target;
        st->SetFloat(id, cur);
        return cur;
    }
    // Мягкая тень под скруглённым прямоугольником.
    static void SoftShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float size, ImU32 col) {
        const int steps = 7;
        for (int i = steps; i >= 1; --i) {
            float t  = (float)i / steps;
            float ex = size * t;
            float al = (1.0f - t) * (1.0f - t);
            dl->AddRectFilled(ImVec2(a.x - ex, a.y - ex + size * 0.35f),
                              ImVec2(b.x + ex, b.y + ex + size * 0.35f),
                              Fade(col, al), rounding + ex, 0);
        }
    }
}

// Активная палитра текущего кадра (ставится в начале Layout_tick_UI). Виджеты
// читают её вместо глобальных ImVec4-констант.
static oxui::Palette g_pal;
static float         g_uiScale = 1.0f;   // масштаб под большой экран (стиль уже ×3)

// Фоновая текстура меню (см. src/menu_bg.cpp).
extern ImTextureID oxGetMenuBg(int* outW, int* outH);

// Применяет премиум тёмный стиль ОДИН раз (размеры подобраны под большой экран).
static void oxApplyStyle() {
    static bool applied = false;
    if (applied) return;
    applied = true;
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 42.f;  s.ChildRounding    = 28.f;
    s.FrameRounding     = 16.f;  s.GrabRounding     = 16.f;
    s.PopupRounding     = 16.f;  s.ScrollbarRounding= 16.f;
    s.WindowBorderSize  = 0.f;   s.FrameBorderSize  = 0.f;   s.ChildBorderSize = 0.f;
    s.WindowPadding     = ImVec2(30, 30);
    s.FramePadding      = ImVec2(22, 16);
    s.ItemSpacing       = ImVec2(18, 16);
    s.ItemInnerSpacing  = ImVec2(12, 10);
    s.ScrollbarSize     = 22.f;  s.GrabMinSize      = 44.f;
    using namespace oxui;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]=BG; c[ImGuiCol_ChildBg]=PANEL; c[ImGuiCol_PopupBg]=PANEL;
    c[ImGuiCol_Text]=TEXT; c[ImGuiCol_TextDisabled]=TEXT_DIM;
    c[ImGuiCol_FrameBg]=PANEL2; c[ImGuiCol_FrameBgHovered]=PANEL3; c[ImGuiCol_FrameBgActive]=PANEL3;
    c[ImGuiCol_Button]=PANEL2; c[ImGuiCol_ButtonHovered]=ACCENT_D; c[ImGuiCol_ButtonActive]=ACCENT;
    c[ImGuiCol_CheckMark]=ACCENT; c[ImGuiCol_SliderGrab]=ACCENT; c[ImGuiCol_SliderGrabActive]=ACCENT_D;
    c[ImGuiCol_Header]=PANEL2; c[ImGuiCol_HeaderHovered]=ACCENT_D; c[ImGuiCol_HeaderActive]=ACCENT;
    c[ImGuiCol_Separator]=PANEL3; c[ImGuiCol_SeparatorHovered]=ACCENT_D; c[ImGuiCol_SeparatorActive]=ACCENT;
    c[ImGuiCol_ScrollbarBg]=ImVec4(0,0,0,0); c[ImGuiCol_ScrollbarGrab]=PANEL3;
    c[ImGuiCol_ScrollbarGrabHovered]=ACCENT_D; c[ImGuiCol_ScrollbarGrabActive]=ACCENT;
    c[ImGuiCol_Border]=ImVec4(0,0,0,0);
    c[ImGuiCol_TitleBg]=PANEL; c[ImGuiCol_TitleBgActive]=PANEL;
}

// ============================================================================
//  GLASS UI — виджеты (все рисуют вручную, без ImGui::Checkbox/SliderFloat вида)
//  Каждый принимает те же указатели/диапазоны, что и старый код, — привязки
//  и границы 1:1. Меняется только внешний вид.
// ============================================================================
namespace oxui {

// iOS-тумблер с пружиной на кнопке и свечением дорожки. true = значение сменилось.
static bool Toggle(const char* label, bool* v, const char* hint = nullptr) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float s = g_uiScale;
    const float rowH = 58.0f * s;
    const float pad  = 16.0f * s;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float  w   = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton(oxorany("##t"), ImVec2(w, rowH));
    bool hov = ImGui::IsItemHovered();
    ImGuiID id = win->GetID(oxorany("##t"));
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float hovA = Anim(win->GetID(oxorany("##th")), hov ? 1.0f : 0.0f, 18.0f);
    if (hovA > 0.001f)
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + rowH), Fade(g_pal.panelHi, hovA), 14.0f * s);

    float trackW = 56.0f * s, trackH = 30.0f * s;
    float knobR  = trackH * 0.5f - 4.0f * s;

    // текст (клипуется, чтобы длинная строка не наезжала на переключатель)
    float textRight = pos.x + w - pad - trackW - 14.0f * s;
    dl->PushClipRect(ImVec2(pos.x + pad, pos.y), ImVec2(textRight, pos.y + rowH), true);
    ImFont* f = ImGui::GetFont();
    float lblSz = 17.5f * s;
    float ty = hint ? pos.y + rowH * 0.5f - 17.0f * s : pos.y + rowH * 0.5f - lblSz * 0.5f;
    dl->AddText(f, lblSz, ImVec2(pos.x + pad, ty), g_pal.text, label);
    if (hint)
        dl->AddText(f, 12.5f * s, ImVec2(pos.x + pad, ty + 20.0f * s), g_pal.textFaint, hint);
    dl->PopClipRect();

    ImVec2 tp(pos.x + w - pad - trackW, pos.y + rowH * 0.5f - trackH * 0.5f);
    if (clicked) *v = !*v;
    float on = Anim(id, *v ? 1.0f : 0.0f, 16.0f);
    ImU32 track = MixU32(g_pal.track, g_pal.accent, on);
    if (on > 0.01f)
        dl->AddRectFilled(ImVec2(tp.x - 3 * s, tp.y - 3 * s),
                          ImVec2(tp.x + trackW + 3 * s, tp.y + trackH + 3 * s),
                          Fade(g_pal.accentHi, 0.28f * on), (trackH + 6 * s) * 0.5f);
    dl->AddRectFilled(tp, ImVec2(tp.x + trackW, tp.y + trackH), track, trackH * 0.5f);
    // лёгкая пружина: небольшой overshoot у кнопки
    float spring = sinf((on) * 3.14159f) * 2.0f * s;
    float kx = tp.x + trackH * 0.5f + on * (trackW - trackH) + (on > 0.001f && on < 0.999f ? spring : 0.f);
    ImVec2 kc(kx, tp.y + trackH * 0.5f);
    dl->AddCircleFilled(ImVec2(kc.x, kc.y + 1.5f * s), knobR, IM_COL32(0, 0, 0, 70));
    dl->AddCircleFilled(kc, knobR, IM_COL32(255, 255, 255, 255));

    ImGui::PopID();
    return clicked;
}

// Градиентный слайдер float со свечёной кнопкой и live-значением справа.
static bool SliderF(const char* label, float* v, float lo, float hi, const char* fmt) {
    const float s = g_uiScale;
    const float rowH = 62.0f * s;
    const float pad  = 16.0f * s;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float  w   = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(label);
    ImGui::InvisibleButton(oxorany("##s"), ImVec2(w, rowH));
    bool active = ImGui::IsItemActive();
    bool hov    = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = ImGui::GetFont();

    float barY = pos.y + rowH - 20.0f * s;
    float bx0 = pos.x + pad, bx1 = pos.x + w - pad;
    float barW = bx1 - bx0;

    bool changed = false;
    if (active && barW > 1.0f) {
        float mx = (ImGui::GetIO().MousePos.x - bx0) / barW;
        mx = mx < 0.f ? 0.f : (mx > 1.f ? 1.f : mx);
        float nv = lo + mx * (hi - lo);
        if (nv != *v) { *v = nv; changed = true; }
    }
    float t = (hi > lo) ? (*v - lo) / (hi - lo) : 0.0f;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);

    dl->AddText(f, 16.0f * s, ImVec2(bx0, pos.y + 6.0f * s), g_pal.text, label);
    char buf[64]; snprintf(buf, sizeof(buf), fmt, *v);
    ImVec2 vsz = f->CalcTextSizeA(15.0f * s, FLT_MAX, 0, buf);
    dl->AddText(f, 15.0f * s, ImVec2(bx1 - vsz.x, pos.y + 7.0f * s), g_pal.accent, buf);

    float th = 6.0f * s;
    dl->AddRectFilled(ImVec2(bx0, barY - th * 0.5f), ImVec2(bx1, barY + th * 0.5f), g_pal.track, th);
    float fx = bx0 + barW * t;
    dl->AddRectFilledMultiColor(ImVec2(bx0, barY - th * 0.5f), ImVec2(fx, barY + th * 0.5f),
                                g_pal.accent, g_pal.accentHi, g_pal.accentHi, g_pal.accent);
    float kr = (hov || active ? 13.0f : 11.0f) * s;
    ImVec2 kc(fx, barY);
    dl->AddCircleFilled(kc, kr + 5.0f * s, Fade(g_pal.accentHi, 0.32f));
    dl->AddCircleFilled(ImVec2(kc.x, kc.y + 1.5f * s), kr, IM_COL32(0, 0, 0, 80));
    dl->AddCircleFilled(kc, kr, IM_COL32(255, 255, 255, 255));
    dl->AddCircleFilled(kc, kr * 0.42f, g_pal.accent);

    ImGui::PopID();
    return changed;
}

// Слайдер int (переиспользует float-версию, чтобы сохранить внешний вид и границы).
static bool SliderI(const char* label, int* v, int lo, int hi) {
    float fv = (float)*v;
    // формат без дробей — целочисленный вид
    bool ch = SliderF(label, &fv, (float)lo, (float)hi, "%.0f");
    int nv = (int)(fv + (fv >= 0 ? 0.5f : -0.5f));
    if (nv < lo) nv = lo; if (nv > hi) nv = hi;
    if (nv != *v) { *v = nv; return true; }
    return ch;
}

// Сегмент-контрол с едущей акцент-пилюлей. true = выбор сменился.
static bool Segmented(const char* label, int* v, const char* const items[], int count) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float s = g_uiScale;
    const float pad = 16.0f * s;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = ImGui::GetFont();

    if (label && label[0]) {
        dl->AddText(f, 16.0f * s, ImVec2(pos.x + pad, pos.y), g_pal.text, label);
        ImGui::Dummy(ImVec2(0, 28.0f * s));
        pos = ImGui::GetCursorScreenPos();
    }

    float segH = 44.0f * s;
    float x0 = pos.x + pad, x1 = pos.x + w - pad;
    float cellW = (x1 - x0) / (float)count;

    ImGui::PushID(label ? label : oxorany("seg"));
    bool clicked = ImGui::InvisibleButton(oxorany("##seg"), ImVec2(w, segH));
    dl->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x1, pos.y + segH), g_pal.panel, segH * 0.5f);
    dl->AddRect(ImVec2(x0, pos.y), ImVec2(x1, pos.y + segH), g_pal.stroke, segH * 0.5f, 0, 1.2f * s);

    bool changed = false;
    if (clicked) {
        float mx = ImGui::GetIO().MousePos.x - x0;
        int idx = (int)(mx / cellW); if (idx < 0) idx = 0; if (idx > count - 1) idx = count - 1;
        if (idx != *v) { *v = idx; changed = true; }
    }
    ImGuiID id = win->GetID(oxorany("##segpos"));
    float cur = Anim(id, (float)*v, 18.0f);
    float px = x0 + cur * cellW;
    ImVec2 pa(px + 4.0f * s, pos.y + 4.0f * s), pb(px + cellW - 4.0f * s, pos.y + segH - 4.0f * s);
    dl->AddRectFilled(ImVec2(pa.x - 1, pa.y - 1), ImVec2(pb.x + 1, pb.y + 1),
                      Fade(g_pal.accentHi, 0.28f), (segH - 8 * s) * 0.5f);
    dl->AddRectFilledMultiColor(pa, pb, g_pal.accent, g_pal.accentHi, g_pal.accentHi, g_pal.accent);

    for (int i = 0; i < count; ++i) {
        ImVec2 sz = f->CalcTextSizeA(14.5f * s, FLT_MAX, 0, items[i]);
        float cellCx = x0 + (i + 0.5f) * cellW;
        bool sel = (i == *v);
        dl->AddText(f, 14.5f * s, ImVec2(cellCx - sz.x * 0.5f, pos.y + segH * 0.5f - sz.y * 0.5f),
                    sel ? IM_COL32(14, 12, 20, 255) : g_pal.textDim, items[i]);
    }
    ImGui::PopID();
    return changed;
}

// Заголовок-секция: мелкий caps-лейбл + тонкая линия справа.
static void Section(const char* text) {
    const float s = g_uiScale;
    ImGui::Dummy(ImVec2(0, 6.0f * s));
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = ImGui::GetFont();
    float sz = 12.5f * s;
    dl->AddText(f, sz, ImVec2(pos.x + 16.0f * s, pos.y), g_pal.textFaint, text);
    ImVec2 tsz = f->CalcTextSizeA(sz, FLT_MAX, 0, text);
    dl->AddLine(ImVec2(pos.x + 16.0f * s + tsz.x + 12.0f * s, pos.y + tsz.y * 0.5f),
                ImVec2(pos.x + ImGui::GetContentRegionAvail().x - 16.0f * s, pos.y + tsz.y * 0.5f),
                g_pal.stroke, 1.0f * s);
    ImGui::Dummy(ImVec2(0, tsz.y + 12.0f * s));
}

// Строка-подсказка (заменяет прежние TextColored(TEXT_DIM, ...)); клип по ширине.
static void Hint(const char* text) {
    const float s = g_uiScale;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = ImGui::GetFont();
    float w = ImGui::GetContentRegionAvail().x;
    float sz = 13.0f * s;
    ImGui::Dummy(ImVec2(0, sz + 6.0f * s));
    dl->AddText(f, sz, ImVec2(pos.x + 16.0f * s, pos.y + 2.0f * s), g_pal.textFaint,
                text, nullptr, w - 24.0f * s);
}

// Два цветовых свотча-точки (Visible / Behind wall) — компактная легенда.
static void LegendDot(ImU32 col, const char* text, bool sameLine) {
    const float s = g_uiScale;
    if (sameLine) ImGui::SameLine();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = ImGui::GetFont();
    float sz = 13.5f * s;
    ImVec2 tsz = f->CalcTextSizeA(sz, FLT_MAX, 0, text);
    float r = 5.0f * s;
    dl->AddCircleFilled(ImVec2(pos.x + 16.0f * s + r, pos.y + tsz.y * 0.5f + 2.0f * s), r, col);
    dl->AddText(f, sz, ImVec2(pos.x + 16.0f * s + r * 2 + 8.0f * s, pos.y + 2.0f * s), g_pal.textDim, text);
    ImGui::Dummy(ImVec2(16.0f * s + r * 2 + 12.0f * s + tsz.x, tsz.y + 6.0f * s));
}

// Кнопка-действие (например «Exit»). tone: 0 danger, 1 accent.
static bool ActionButton(const char* label, int tone) {
    const float s = g_uiScale;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = 50.0f * s;
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton(oxorany("##act"), ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = ImGui::GetFont();
    ImU32 base = (tone == 0) ? g_pal.danger : g_pal.accent;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), Fade(base, hov ? 0.30f : 0.18f), 14.0f * s);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), Fade(base, hov ? 0.95f : 0.55f), 14.0f * s, 0, 1.4f * s);
    ImVec2 tsz = f->CalcTextSizeA(16.0f * s, FLT_MAX, 0, label);
    dl->AddText(f, 16.0f * s, ImVec2(pos.x + w * 0.5f - tsz.x * 0.5f, pos.y + h * 0.5f - tsz.y * 0.5f),
                base, label);
    ImGui::PopID();
    return clicked;
}

// ColorEdit-обёртка: маленький свотч, тап открывает штатный пикер (popup).
static void ColorSwatch(const char* label, float col[3]) {
    const float s = g_uiScale;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float rowH = 46.0f * s;
    float sw = 34.0f * s;
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = ImGui::GetFont();
    // весь ряд — кликабельная зона, открывающая пикер (крупная тач-цель)
    bool clicked = ImGui::InvisibleButton(oxorany("##swrow"), ImVec2(w, rowH));
    if (clicked) ImGui::OpenPopup(oxorany("##swpop"));
    dl->AddText(f, 16.0f * s, ImVec2(pos.x + 16.0f * s, pos.y + rowH * 0.5f - 9.0f * s), g_pal.text, label);
    ImVec2 sa(pos.x + w - 16.0f * s - sw, pos.y + rowH * 0.5f - sw * 0.5f);
    ImVec2 sb(sa.x + sw, sa.y + sw);
    ImU32 cc = IM_COL32((int)(col[0] * 255), (int)(col[1] * 255), (int)(col[2] * 255), 255);
    dl->AddRectFilled(sa, sb, cc, 9.0f * s);
    dl->AddRect(sa, sb, g_pal.stroke, 9.0f * s, 0, 1.2f * s);
    // popup якорим у свотча
    ImGui::SetNextWindowPos(ImVec2(sa.x, sb.y + 6.0f * s), ImGuiCond_Appearing, ImVec2(0, 0));
    if (ImGui::BeginPopup(oxorany("##swpop"))) {
        ImGui::ColorPicker3(oxorany("##pick"), col,
            ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

} // namespace oxui

// Счётчик врагов сверху по центру — стеклянная пилюля в стиле Trial Engine.
// Аура-свечение когда враги > 0, крупное число + компактная подпись «ENEMIES»,
// пульс при смене количества. Тап = тумбл меню. Цвет = акцент текущей темы.
static void oxDrawCounter(const ImVec2& ds) {
    using namespace oxui;
    const float s = g_uiScale;
    const float W = 236.f * s, H = 76.f * s;
    float x = ds.x * 0.5f - W * 0.5f;
    float y = 22.f * s;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, H + 30.f * s), ImGuiCond_Always);   // запас под ауру
    ImGui::Begin(oxorany("##oxcounter"), nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings);

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(oxorany("##cnthit"), ImVec2(W, H));
    if (ImGui::IsItemClicked()) g_menuOpen = !g_menuOpen;
    bool hov = ImGui::IsItemHovered();

    int cnt = g_enemyCount;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiWindow* win = ImGui::GetCurrentWindow();

    // Пульс при смене количества: сохраняем прошлое число и время всплеска.
    ImGuiStorage* stg = ImGui::GetStateStorage();
    ImGuiID lastId = win->GetID(oxorany("##cntlast"));
    ImGuiID pulseId = win->GetID(oxorany("##cntpulse"));
    int last = stg->GetInt(lastId, cnt);
    if (last != cnt) { stg->SetInt(lastId, cnt); stg->SetFloat(pulseId, 1.0f); }
    float pulse = Anim(pulseId, 0.0f, 6.0f);

    // Цвет: враги рядом — danger, иначе акцент темы. Интенсивность ауры от cnt.
    ImU32 accent = (cnt > 0) ? g_pal.danger : g_pal.accent;
    ImU32 glow   = (cnt > 0) ? g_pal.danger : g_pal.accentHi;
    ImVec2 a(p.x, p.y), b(p.x + W, p.y + H);

    // аура-свечение (только когда есть враги или во время пульса)
    float auraK = (cnt > 0 ? 1.0f : 0.0f);
    auraK = auraK > pulse ? auraK : pulse;
    if (auraK > 0.01f) {
        for (int i = 5; i >= 1; --i) {
            float ex = (10.f + i * 6.f) * s;
            float al = (1.0f - (float)i / 5.0f);
            dl->AddRectFilled(ImVec2(a.x - ex, a.y - ex + 4 * s), ImVec2(b.x + ex, b.y + ex + 4 * s),
                              Fade(glow, 0.12f * al * auraK), 20.f * s + ex, 0);
        }
    }
    // мягкая тень + стеклянная панель + тонкая рамка
    SoftShadow(dl, a, b, 20.f * s, 14.f * s, IM_COL32(0, 0, 0, 130));
    dl->AddRectFilled(a, b, IM_COL32(20, 18, 30, hov ? 252 : 240), 20.f * s);
    // верхний глянец
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + H * 0.5f),
        IM_COL32(255, 255, 255, 16), IM_COL32(255, 255, 255, 16),
        IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
    dl->AddRect(a, b, Fade(accent, 0.85f), 20.f * s, 0, 2.0f * s);

    // Иконка-прицел слева (векторная, из идиомы Trial Engine).
    ImVec2 ic(a.x + 30.f * s, a.y + H * 0.5f);
    float ir = 11.f * s * (1.0f + pulse * 0.15f);
    dl->AddCircle(ic, ir, accent, 24, 2.0f * s);
    dl->AddLine(ImVec2(ic.x, ic.y - ir - 4 * s), ImVec2(ic.x, ic.y - ir + 3 * s), accent, 2.0f * s);
    dl->AddLine(ImVec2(ic.x, ic.y + ir - 3 * s), ImVec2(ic.x, ic.y + ir + 4 * s), accent, 2.0f * s);
    dl->AddLine(ImVec2(ic.x - ir - 4 * s, ic.y), ImVec2(ic.x - ir + 3 * s, ic.y), accent, 2.0f * s);
    dl->AddLine(ImVec2(ic.x + ir - 3 * s, ic.y), ImVec2(ic.x + ir + 4 * s, ic.y), accent, 2.0f * s);
    dl->AddCircleFilled(ic, 2.4f * s, accent);

    // Крупное число + мелкая подпись «ENEMIES». oxorany → копируем немедленно.
    ImFont* f = ImGui::GetFont();
    char num[16]; snprintf(num, sizeof(num), "%d", cnt);
    float big = ImGui::GetFontSize() * (1.9f + pulse * 0.12f);
    ImVec2 ns = f->CalcTextSizeA(big, FLT_MAX, 0, num);
    float numX = a.x + 54.f * s;
    dl->AddText(f, big, ImVec2(numX, a.y + H * 0.5f - ns.y * 0.5f),
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f, 0.94f, 0.98f, 1.f)), num);
    char lbl[16]; snprintf(lbl, sizeof(lbl), "%s", oxorany("ENEMIES"));
    float lblSz = ImGui::GetFontSize() * 0.5f;
    dl->AddText(f, lblSz, ImVec2(numX + ns.x + 12.f * s, a.y + H * 0.5f - lblSz * 0.5f),
                Fade(accent, 0.9f), lbl);

    g_window = ImGui::GetCurrentWindow();  // для orientation-хака в drawBegin
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ---- Содержимое вкладок (тот же набор привязок/границ, новый вид) ----
static void oxTabESP() {
    using namespace oxui;
    Section(oxorany("OVERLAY"));
    Toggle(oxorany("Enable ESP"), &espdraw);
    if (Toggle(oxorany("Hide teammates"), &espHideTeammates))
        cache_needs_update = true;
    // --- Видимость через стены (зелёный/синий) ---
    Toggle(oxorany("Color by visibility"), &espColorByVisibility, oxorany("Reads BuildingPiece.saveList (walls)"));
    if (espColorByVisibility) {
        LegendDot(IM_COL32(60, 230, 110, 255), oxorany("Visible"), false);
        LegendDot(IM_COL32(80, 150, 255, 255), oxorany("Behind wall"), true);
    }

    Section(oxorany("BOX"));
    Toggle(oxorany("Box"), &espbox);
    if (espbox) {
        SliderF(oxorany("Stroke"), &espstroke, 0.0f, 5.0f, "%.2f");
        Toggle(oxorany("Corner box"), &espCornerBox);
        if (espCornerBox)
            SliderF(oxorany("Corner length"), &espCornerLength, 0.16f, 0.45f, "%.2f");
        Toggle(oxorany("Filled"), &espfill);
        if (espfill) {
            SliderF(oxorany("Fill value"), &espfillp, 20, 80, "%.0f");
            Toggle(oxorany("Fill gradient"), &espgradient);
        }
        // Цвет бокса доступен только если не включена окраска по видимости —
        // иначе он переопределяется зелёным/синим.
        if (!espColorByVisibility)
            ColorSwatch(oxorany("Box color"), espboxcolor);
    }

    Section(oxorany("INFO"));
    Toggle(oxorany("Health bar"), &esphealth);
    if (esphealth) {
        SliderF(oxorany("Bar stroke"), &esphpsize, 0.0f, 10.0f, "%.1f");
        Toggle(oxorany("HP gradient"), &esphpgradient);
    }
    Toggle(oxorany("Distance"), &espdist);
    Toggle(oxorany("Name"),     &espname);
    Toggle(oxorany("Weapon"),   &espweapon);
    Toggle(oxorany("3D box"),   &ESP3D);
    Toggle(oxorany("ESP lines"), &esplines_enabled);
    if (esplines_enabled) {
        SliderF(oxorany("Line thickness"), &esplines_thickness, 1.0f, 6.0f, "%.1f");
        ColorSwatch(oxorany("Line color"), esplines_color);
        const char* origins[] = { "Bottom center", "Crosshair" };
        Segmented(oxorany("Line origin"), &esplines_position, origins, IM_ARRAYSIZE(origins));
    }
    Section(oxorany("RANGE"));
    SliderF(oxorany("Max distance (m)"), &espMaxDistance, 0.0f, 10000.0f, "%.0f");
}

static void oxTabAim() {
    using namespace oxui;
    Section(oxorany("AIMBOT"));
    Toggle(oxorany("Enable aimbot"), &aimm);
    if (!aimm) return;

    // --- bug #5: аим только в прицеле (ADS/scope) ---
    Toggle(oxorany("Only when aiming (ADS)"), &aimOnlyADS, oxorany("Aims only while scoped (live FOV zoom)."));

    Section(oxorany("TUNING"));
    SliderF(oxorany("Aim FOV (px)"), &aimFovPixels, 20.0f, 600.0f, "%.0f");
    SliderF(oxorany("Smoothing"), &aimSmooth, 1.0f, 20.0f, "%.1f");
    SliderF(oxorany("Max distance (m)"), &aimMaxDistance, 0.0f, 1000.0f, "%.0f");
    const char* bones[] = { "Head", "Neck", "Body" };
    Segmented(oxorany("Target point"), &aimcurbone, bones, IM_ARRAYSIZE(bones));
    Hint(oxorany("Smoothing 1 = instant target adjustment."));

    Section(oxorany("SAFETY"));
    // --- Crouch-safe (bug #4): ручной, т.к. авто-детект приседа недоступен ---
    Toggle(oxorany("Crouch-safe aim"), &aimCrouchSafe, oxorany("Crouch state not readable - manual."));
    if (aimCrouchSafe)
        SliderF(oxorany("Crouch drop (m)"), &aimCrouchDrop, 0.20f, 0.70f, "%.2f");
    // --- LOS-чек: аим не наводится через стены ---
    Toggle(oxorany("Check walls"), &aimCheckWalls, oxorany("BuildingPiece.saveList AABB raycast"));
    // --- Ignore teammates: аим не наводится на сокомандников ---
    Toggle(oxorany("Ignore teammates"), &aimIgnoreTeammates, oxorany("Skip players sharing team with local."));

    Section(oxorany("PREDICTION"));
    Toggle(oxorany("Movement prediction"), &aimPrediction);
    if (aimPrediction) {
        SliderF(oxorany("Projectile speed (m/s)"), &aimProjectileSpeed, 20.0f, 1000.0f, "%.0f");
        SliderF(oxorany("Latency (ms)"), &aimLatencyMs, 0.0f, 250.0f, "%.0f");
        SliderF(oxorany("Max lead time (s)"), &aimMaxLeadTime, 0.05f, 0.75f, "%.2f");
    }
    Toggle(oxorany("Show FOV circle"), &aimDrawFov);
}

static void oxTabCamera() {
    using namespace oxui;
    Section(oxorany("CAMERA / W2S"));
    Hint(oxorany("Basis from look angles. FOV read live from game (vertical, ADS-aware)."));
    Toggle(oxorany("Invert pitch"), &camInvertPitch);
    Toggle(oxorany("Invert yaw"),   &camInvertYaw);
    Section(oxorany("FALLBACK"));
    // Ползунок FOV теперь — ТОЛЬКО аварийный fallback (если FPManager не отдал
    // живой FOV в этот кадр). В норме проекция использует реальный верт. FOV игры.
    Hint(oxorany("Used only if live FOV unavailable."));
    SliderF(oxorany("Fallback H-FOV"), &camFov, 60.0f, 120.0f, "%.1f");
}

static void oxTabSettings() {
    using namespace oxui;
    Section(oxorany("APPEARANCE"));
    // Акцент-темы: полоса из свотчей. Меняет только цвет UI (визуал).
    {
        const float s = g_uiScale;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        float sw = 46.0f * s, sgap = 14.0f * s;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushID(oxorany("accents"));
        if (ImGui::InvisibleButton(oxorany("##sw"), ImVec2(w, sw))) {
            float mx = ImGui::GetIO().MousePos.x - (pos.x + 16.0f * s);
            int idx = (int)(mx / (sw + sgap));
            if (idx >= 0 && idx < kAccentCount) g_accentTheme = idx;
        }
        for (int i = 0; i < kAccentCount; ++i) {
            ImVec2 a(pos.x + 16.0f * s + i * (sw + sgap), pos.y);
            ImVec2 b(a.x + sw, a.y + sw);
            bool sel = (i == g_accentTheme);
            if (sel)
                dl->AddRectFilled(ImVec2(a.x - 3 * s, a.y - 3 * s), ImVec2(b.x + 3 * s, b.y + 3 * s),
                                  Fade(kAccents[i].b, 0.35f), 13.0f * s);
            dl->AddRectFilledMultiColor(a, b, kAccents[i].a, kAccents[i].b, kAccents[i].b, kAccents[i].a);
            dl->AddRect(a, b, sel ? IM_COL32(255, 255, 255, 255) : g_pal.stroke,
                        11.0f * s, 0, sel ? 2.5f * s : 1.2f * s);
            if (sel) dl->AddCircleFilled(ImVec2(b.x - 10 * s, a.y + 10 * s), 3.6f * s, IM_COL32(255, 255, 255, 255));
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, sw + 12.0f * s));
    }
    static float opacity = 1.0f;
    SliderF(oxorany("UI opacity"), &opacity, 0.35f, 1.0f, "%.2f");
    ImGui::GetStyle().Alpha = opacity;

    Section(oxorany("PERFORMANCE"));
    SliderI(oxorany("Heavy scan interval"), &g_cacheInterval, 4, 30);
    SliderI(oxorany("Position interval"), &g_positionInterval, 1, 4);
    Hint(oxorany("Default 10/2 reduces reads; camera projection stays per-frame."));

    Section(oxorany("SESSION"));
    if (ActionButton(oxorany("Exit"), 0)) main_thread_flag = false;
    ImGui::Dummy(ImVec2(0, 8.0f * g_uiScale));
    if (ActionButton(oxorany("Unload"), 0)) exit(0);
}

// Векторные глифы вкладок (порт идиомы Trial Engine). i: 0 ESP(глаз),
// 1 Aim(прицел), 2 Camera(объектив/глобус), 3 Settings(шестерня).
namespace oxui {
static void TabGlyph(ImDrawList* d, ImVec2 c, int i, ImU32 col, float k) {
    if (i == 0) {                 // глаз
        d->AddEllipse(ImVec2(c.x, c.y), 9 * k, 6 * k, col, 0, 24, 1.8f * k);
        d->AddCircleFilled(ImVec2(c.x, c.y), 2.7f * k, col);
    } else if (i == 1) {          // прицел
        d->AddCircle(c, 8 * k, col, 28, 1.8f * k);
        d->AddCircle(c, 3 * k, col, 16, 1.8f * k);
        d->AddLine(ImVec2(c.x, c.y - 11 * k), ImVec2(c.x, c.y - 5 * k), col, 1.8f * k);
        d->AddLine(ImVec2(c.x, c.y + 5 * k), ImVec2(c.x, c.y + 11 * k), col, 1.8f * k);
        d->AddLine(ImVec2(c.x - 11 * k, c.y), ImVec2(c.x - 5 * k, c.y), col, 1.8f * k);
        d->AddLine(ImVec2(c.x + 5 * k, c.y), ImVec2(c.x + 11 * k, c.y), col, 1.8f * k);
    } else if (i == 2) {          // объектив (камера)
        d->AddCircle(c, 8 * k, col, 28, 1.8f * k);
        d->AddEllipse(ImVec2(c.x, c.y), 3.4f * k, 8 * k, col, 0, 20, 1.6f * k);
        d->AddLine(ImVec2(c.x - 8 * k, c.y), ImVec2(c.x + 8 * k, c.y), col, 1.6f * k);
    } else {                      // шестерня
        d->AddCircle(c, 7 * k, col, 28, 1.8f * k);
        d->AddCircle(c, 2.7f * k, col, 16, 1.8f * k);
        for (int j = 0; j < 8; ++j) {
            float ang = j * 3.14159f / 4.0f;
            d->AddLine(ImVec2(c.x + cosf(ang) * 7 * k, c.y + sinf(ang) * 7 * k),
                       ImVec2(c.x + cosf(ang) * 10 * k, c.y + sinf(ang) * 10 * k), col, 1.8f * k);
        }
    }
}
static const char* kTabNames[4] = { "ESP", "Aim", "Camera", "Settings" };
static const char* kTabSubs[4]  = {
    "Entity overlays", "Aim controls", "Camera / W2S", "Theme & performance" };
} // namespace oxui

void Layout_tick_UI() {
    using namespace oxui;
    oxApplyStyle();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    if (ds.x < 1 || ds.y < 1) ds = ImVec2(1280, 720);

    // Палитра + масштаб этого кадра. Стиль уже отмасштабирован ×3 при init;
    // держим наш s около 3, чтобы пальце-размеры совпадали с остальным UI.
    g_pal = MakePalette(g_accentTheme);
    g_uiScale = 3.0f;
    const float s = g_uiScale;

    bool portrait = ds.y > ds.x * 1.05f;

    // ---- Большое центрированное меню ----
    if (g_menuOpen) {
        const float margin = 24.f;
        const float maxWidth = ds.x > margin * 2.f ? ds.x - margin * 2.f : ds.x;
        const float maxHeight = ds.y > margin * 2.f ? ds.y - margin * 2.f : ds.y;
        ImVec2 sz(portrait ? ds.x * 0.92f : ds.x * 0.68f, ds.y * 0.88f);
        if (sz.x < 640.f && maxWidth >= 640.f) sz.x = 640.f;
        if (sz.x > maxWidth) sz.x = maxWidth;
        if (sz.y > maxHeight) sz.y = maxHeight;
        ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin(oxorany("##oxmenu"), nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 wsz = ImGui::GetWindowSize();
        ImVec2 wmin = wp, wmax(wp.x + wsz.x, wp.y + wsz.y);
        const float RND = ::clamp<float>(wsz.y * 0.045f, 22.f, 42.f);

        // ── затемнение всего экрана позади панели (dim) ─────────────────────
        dl->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(0, 0, 0, 150));

        // ── слой 1: фон-арт (если есть), приглушённый, cover-fit ────────────
        int iw = 0, ih = 0;
        ImTextureID bgtex = oxGetMenuBg(&iw, &ih);
        if (bgtex && iw > 0 && ih > 0) {
            float wr = wsz.x / wsz.y, ir = (float)iw / (float)ih;
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (ir > wr) { float t = (wr / ir) * 0.5f; uv0.x = 0.5f - t; uv1.x = 0.5f + t; }
            else         { float t = (ir / wr) * 0.5f; uv0.y = 0.5f - t; uv1.y = 0.5f + t; }
            dl->AddImageRounded(bgtex, wmin, wmax, uv0, uv1, IM_COL32(255, 255, 255, 70), RND);
        }
        // ── слой 2: тёмный стеклянный градиент ──────────────────────────────
        dl->AddRectFilledMultiColor(wmin, wmax, g_pal.bg0, g_pal.bg0, g_pal.bg1, g_pal.bg1);
        dl->AddRectFilled(wmin, wmax, IM_COL32(10, 8, 18, 120), RND);
        // ── слой 3: аура-свечение акцента (верхний левый угол) ──────────────
        for (int i = 6; i >= 1; --i) {
            float r = (wsz.x * 0.22f) * (float)i / 6.0f + 40.f * s;
            dl->AddCircleFilled(ImVec2(wmin.x + wsz.x * 0.16f, wmin.y + wsz.y * 0.12f),
                                r, Fade(g_pal.accent, 0.030f), 48);
        }
        // второй ореол у правого-нижнего для глубины
        for (int i = 5; i >= 1; --i) {
            float r = (wsz.x * 0.16f) * (float)i / 5.0f + 30.f * s;
            dl->AddCircleFilled(ImVec2(wmin.x + wsz.x * 0.86f, wmin.y + wsz.y * 0.88f),
                                r, Fade(g_pal.accentHi, 0.022f), 48);
        }
        // ── рамка + мягкое внешнее свечение + верхний глянец ────────────────
        SoftShadow(dl, wmin, wmax, RND, 24.f * s * 0.34f + 8.f, IM_COL32(0, 0, 0, 150));
        dl->AddRect(wmin, wmax, Fade(g_pal.accent, 0.9f), RND, 0, 2.2f * s * 0.34f + 1.0f);
        dl->AddRect(ImVec2(wmin.x - 1, wmin.y - 1), ImVec2(wmax.x + 1, wmax.y + 1),
                    Fade(g_pal.accentHi, 0.30f), RND, 0, 4.f);
        dl->AddRectFilledMultiColor(wmin, ImVec2(wmax.x, wmin.y + 64 * s * 0.34f + 12),
            IM_COL32(255, 255, 255, 14), IM_COL32(255, 255, 255, 14),
            IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));

        // ── геометрия: шапка / навигация / контент ──────────────────────────
        float headerH = 66.f * s * 0.34f + 30.f;
        float navW    = 210.f * s * 0.34f + 90.f;   // sidebar (landscape)
        float navH    = 78.f  * s * 0.34f + 40.f;   // bottom bar (portrait)
        ImFont* hf = ImGui::GetFont();

        // ── шапка: логотип-значок + название + FPS-чип ──────────────────────
        {
            ImVec2 h0(wmin.x, wmin.y);
            ImVec2 lc(h0.x + 34 * s * 0.34f + 14, h0.y + headerH * 0.5f);
            float lr = 16 * s * 0.34f + 6;
            dl->AddCircleFilled(lc, lr, Fade(g_pal.accent, 0.20f));
            dl->AddCircle(lc, lr, g_pal.accent, 32, 1.6f * s * 0.34f + 0.6f);
            // стилизованная «O» ядро
            dl->AddCircleFilled(lc, lr * 0.42f, g_pal.accentHi);
            float titleSz = ImGui::GetFontSize() * 1.35f;
            char t1[8]; snprintf(t1, sizeof(t1), "%s", oxorany("Eclips"));
            char t2[8]; snprintf(t2, sizeof(t2), "%s", oxorany(" Oxide"));
            ImVec2 tp(h0.x + lc.x - wmin.x + lr + 14 * s * 0.34f + 6, h0.y + headerH * 0.5f - titleSz * 0.5f - 6 * s * 0.34f);
            ImVec2 s1 = hf->CalcTextSizeA(titleSz, FLT_MAX, 0, t1);
            dl->AddText(hf, titleSz, tp, g_pal.text, t1);
            dl->AddText(hf, titleSz, ImVec2(tp.x + s1.x, tp.y), g_pal.accent, t2);
            dl->AddText(hf, ImGui::GetFontSize() * 0.5f, ImVec2(tp.x, tp.y + titleSz + 2 * s * 0.34f),
                        g_pal.textFaint, oxorany("survival island  v0.2"));

            // FPS-чип справа
            char fps[24]; snprintf(fps, sizeof(fps), "%.0f FPS", io.Framerate);
            float chipSz = ImGui::GetFontSize() * 0.62f;
            ImVec2 fs = hf->CalcTextSizeA(chipSz, FLT_MAX, 0, fps);
            float chipW = fs.x + 22.f, chipH = fs.y + 12.f;
            ImVec2 ca(wmax.x - 26.f - chipW, h0.y + headerH * 0.5f - chipH * 0.5f), cb(ca.x + chipW, ca.y + chipH);
            dl->AddRectFilled(ca, cb, g_pal.panel, chipH * 0.5f);
            dl->AddRect(ca, cb, Fade(g_pal.accent, 0.5f), chipH * 0.5f, 0, 1.2f);
            float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 3.0f);
            dl->AddCircleFilled(ImVec2(ca.x + 12.f, ca.y + chipH * 0.5f), 3.4f + pulse * 1.4f, Fade(g_pal.good, 0.4f));
            dl->AddCircleFilled(ImVec2(ca.x + 12.f, ca.y + chipH * 0.5f), 2.8f, g_pal.good);
            dl->AddText(hf, chipSz, ImVec2(ca.x + 22.f, ca.y + chipH * 0.5f - fs.y * 0.5f), g_pal.text, fps);

            dl->AddLine(ImVec2(wmin.x, wmin.y + headerH), ImVec2(wmax.x, wmin.y + headerH), g_pal.stroke, 1.0f);
        }

        // ── регионы контента / навигации ────────────────────────────────────
        ImVec2 bodyA, bodyB;
        if (portrait) {
            bodyA = ImVec2(wmin.x, wmin.y + headerH);
            bodyB = ImVec2(wmax.x, wmax.y - navH);
        } else {
            bodyA = ImVec2(wmin.x + navW, wmin.y + headerH);
            bodyB = ImVec2(wmax.x, wmax.y);
        }

        // ── навигация: sidebar (landscape) или bottom bar (portrait) ─────────
        if (portrait) {
            float y0 = wmax.y - navH, y1 = wmax.y;
            dl->AddLine(ImVec2(wmin.x, y0), ImVec2(wmax.x, y0), g_pal.stroke, 1.0f);
            float cellW = (wmax.x - wmin.x) / 4.0f;
            ImGui::SetCursorScreenPos(ImVec2(wmin.x, y0));
            ImGui::PushID(oxorany("navbar"));
            if (ImGui::InvisibleButton(oxorany("##nav"), ImVec2(wmax.x - wmin.x, navH))) {
                float mx = io.MousePos.x - wmin.x;
                int idx = (int)(mx / cellW); if (idx < 0) idx = 0; if (idx > 3) idx = 3;
                g_activeTab = idx;
            }
            ImGui::PopID();
            float curp = Anim(ImGui::GetCurrentWindow()->GetID(oxorany("navind_p")), (float)g_activeTab, 18.0f);
            float ix = wmin.x + (curp + 0.5f) * cellW;
            dl->AddRectFilledMultiColor(ImVec2(ix - 18 * s * 0.34f - 4, y0), ImVec2(ix + 18 * s * 0.34f + 4, y0 + 3 * s * 0.34f + 1),
                g_pal.accent, g_pal.accentHi, g_pal.accentHi, g_pal.accent);
            for (int i = 0; i < 4; ++i) {
                float cxp = wmin.x + (i + 0.5f) * cellW;
                bool sel = (i == g_activeTab);
                ImU32 col = sel ? g_pal.accent : g_pal.textFaint;
                TabGlyph(dl, ImVec2(cxp, y0 + navH * 0.40f), i, col, 0.85f * s * 0.34f + 0.6f);
                float lsz = ImGui::GetFontSize() * 0.46f;
                ImVec2 tsz = hf->CalcTextSizeA(lsz, FLT_MAX, 0, kTabNames[i]);
                dl->AddText(hf, lsz, ImVec2(cxp - tsz.x * 0.5f, y0 + navH - 20 * s * 0.34f - 6), col, kTabNames[i]);
            }
        } else {
            float x0 = wmin.x, x1 = wmin.x + navW;
            dl->AddLine(ImVec2(x1, wmin.y + headerH), ImVec2(x1, wmax.y), g_pal.stroke, 1.0f);
            float itemH = 60.f * s * 0.34f + 18;
            float startY = wmin.y + headerH + 16 * s * 0.34f + 6;
            ImGui::SetCursorScreenPos(ImVec2(x0 + 14 * s * 0.34f + 4, startY));
            ImGui::PushID(oxorany("navside"));
            if (ImGui::InvisibleButton(oxorany("##nav"), ImVec2(navW - 28 * s * 0.34f - 8, itemH * 4))) {
                float my = io.MousePos.y - startY;
                int idx = (int)(my / itemH); if (idx < 0) idx = 0; if (idx > 3) idx = 3;
                g_activeTab = idx;
            }
            ImGui::PopID();
            float curp = Anim(ImGui::GetCurrentWindow()->GetID(oxorany("navind_l")), (float)g_activeTab, 18.0f);
            ImVec2 sa(x0 + 14 * s * 0.34f + 4, startY + curp * itemH + 5 * s * 0.34f);
            ImVec2 sbb(x1 - 14 * s * 0.34f - 4, sa.y + itemH - 10 * s * 0.34f);
            dl->AddRectFilled(sa, sbb, Fade(g_pal.accent, 0.16f), 14 * s * 0.34f + 4);
            dl->AddRectFilled(sa, ImVec2(sa.x + 4.f * s * 0.34f + 1, sbb.y), g_pal.accent, 2 * s * 0.34f + 1);
            for (int i = 0; i < 4; ++i) {
                float iy = startY + i * itemH;
                bool sel = (i == g_activeTab);
                ImU32 col = sel ? g_pal.text : g_pal.textDim;
                TabGlyph(dl, ImVec2(x0 + 34 * s * 0.34f + 12, iy + itemH * 0.5f), i,
                         sel ? g_pal.accent : g_pal.textFaint, 0.9f * s * 0.34f + 0.7f);
                float lsz = ImGui::GetFontSize() * 0.62f;
                dl->AddText(hf, lsz, ImVec2(x0 + 56 * s * 0.34f + 20, iy + itemH * 0.5f - lsz * 0.5f), col, kTabNames[i]);
            }
            dl->AddText(hf, ImGui::GetFontSize() * 0.44f, ImVec2(x0 + 16 * s * 0.34f + 6, wmax.y - 26 * s * 0.34f - 8),
                        g_pal.textFaint, oxorany("v4 glass ui"));
        }

        // ── заголовок страницы над контентом ────────────────────────────────
        dl->AddRectFilledMultiColor(ImVec2(bodyA.x + 8 * s * 0.34f + 2, bodyA.y + 10 * s * 0.34f + 4),
            ImVec2(bodyA.x + 12 * s * 0.34f + 4, bodyA.y + 42 * s * 0.34f + 4),
            g_pal.accent, g_pal.accent, g_pal.accentHi, g_pal.accentHi);
        dl->AddText(hf, ImGui::GetFontSize() * 0.92f, ImVec2(bodyA.x + 24 * s * 0.34f + 8, bodyA.y + 10 * s * 0.34f + 2),
                    g_pal.text, kTabNames[g_activeTab]);
        dl->AddText(hf, ImGui::GetFontSize() * 0.5f, ImVec2(bodyA.x + 24 * s * 0.34f + 8, bodyA.y + 34 * s * 0.34f + 4),
                    g_pal.textFaint, kTabSubs[g_activeTab]);
        float contentTop = bodyA.y + 56 * s * 0.34f + 8;

        // ── прокручиваемый контент + fade между страницами ──────────────────
        ImGui::SetCursorScreenPos(ImVec2(bodyA.x + 8 * s * 0.34f + 2, contentTop));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 10.f * s * 0.34f + 4);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(g_pal.stroke));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImGui::ColorConvertU32ToFloat4(g_pal.accent));
        ImGui::BeginChild(oxorany("##oxcontent"),
            ImVec2(bodyB.x - bodyA.x - 16 * s * 0.34f - 4, bodyB.y - contentTop - 10 * s * 0.34f - 4),
            false, ImGuiWindowFlags_NoBackground);

        ImGuiID pid = ImGui::GetCurrentWindow()->GetID(oxorany("pagefade"));
        ImGuiStorage* pst = ImGui::GetStateStorage();
        int lastPage = pst->GetInt(pid, -1);
        if (lastPage != g_activeTab) { pst->SetInt(pid, g_activeTab); pst->SetFloat(pid + 1, 0.0f); }
        float appear = Anim(pid + 1, 1.0f, 14.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.0f - appear) * 12.0f * s * 0.34f);

        switch (g_activeTab) {
            case 0: oxTabESP();      break;
            case 1: oxTabAim();      break;
            case 2: oxTabCamera();   break;
            default: oxTabSettings(); break;
        }
        ImGui::Dummy(ImVec2(0, 16 * s * 0.34f + 6));
        ImGui::EndChild();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        // ── watermark-чип в правом-нижнем углу (компактный) ─────────────────
        {
            char wm[40]; snprintf(wm, sizeof(wm), "%s  %.0f", oxorany("Eclips Oxide"), io.Framerate);
            float wmSz = ImGui::GetFontSize() * 0.5f;
            ImVec2 wsz2 = hf->CalcTextSizeA(wmSz, FLT_MAX, 0, wm);
            ImVec2 wp2(wmax.x - wsz2.x - 22.f, wmax.y - wsz2.y - 20.f);
            if (!portrait) {   // в портрете снизу — bottom bar, чип бы наехал
                dl->AddRectFilled(ImVec2(wp2.x - 10, wp2.y - 6), ImVec2(wp2.x + wsz2.x + 10, wp2.y + wsz2.y + 6),
                                  IM_COL32(14, 12, 20, 180), 9.f);
                dl->AddRect(ImVec2(wp2.x - 10, wp2.y - 6), ImVec2(wp2.x + wsz2.x + 10, wp2.y + wsz2.y + 6),
                            Fade(g_pal.accent, 0.55f), 9.f, 0, 1.0f);
                dl->AddText(hf, wmSz, wp2, g_pal.textDim, wm);
            }
        }

        ImGui::End();
        ImGui::PopStyleVar();   // WindowPadding
    }

    // ---- Плавающая кнопка-счётчик (поверх) ----
    oxDrawCounter(ds);
}
