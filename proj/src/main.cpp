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

bool main_thread_flag = true;

int abs_ScreenX = 0;
int abs_ScreenY = 0;
int game_pid = -1;
uint64_t il2cpp_base = 0;

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

// LOS-чек (аим через стены): если ON — аимбот не наводится на цель, перекрытую
// building piece'ом по ходу луча. ESP при этом рисует цель СИНИМ (невидим) и
// ЗЕЛЁНЫМ (видим) — отдельно от команды/инга.
bool aimCheckWalls = true;
bool espColorByVisibility = true; // true — видимый=зелёный, за стеной=синий

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

// --- Настройки камеры для углового W2S (тюнятся в меню на устройстве) ---
float camFov = 98.607f;     // горизонтальный FOV (град) — откалибровано под игру
bool  camSwapAngles = false; // поменять местами yaw/pitch из mouseLook
bool  camInvertPitch = false;// инвертировать pitch
bool  camInvertYaw = false;  // инвертировать yaw
// ФИКС ESP: строить базис камеры из ПОЛНОГО мирового кватерниона m_LookRoot
// (в этом риге он несёт yaw+pitch). Отдельный pitch-угол (mouseLook+ML_PITCH)
// в новом дампе лежит по обфусцированному оффсету (0x60 = Vector2 kqP), поэтому
// доверять ему нельзя. По умолчанию ON. Выключить — если у камеры нет наклона.
bool  camUseLookRootPitch = true;

// --- Состояние кастомного меню ---
bool  g_menuOpen  = true;    // открыто ли большое меню (счётчик врагов тумблит)
int   g_activeTab = 0;       // 0=ESP 1=Aim 2=Camera 3=Settings

// --- Фильтры и производительность ---
int   g_enemyCount  = 0;     // сколько игроков сейчас в кэше (для счётчика)
int   g_cacheInterval = 10;  // heavy list/HP scan; positions are refreshed separately
int   g_positionInterval = 2;
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
    const size_t chunkSize = 1024 * 1024;
    std::vector<uint8_t> buffer(chunkSize + needleLen);
    for (const OxMapRange &map : maps) {
        size_t carry = 0;
        for (uint64_t at = map.start; at < map.end && matches.size() < maxMatches;) {
            size_t want = (size_t)std::min<uint64_t>(chunkSize, map.end - at);
            if (!vm_readv(at, buffer.data() + carry, want)) { carry = 0; at += want; continue; }
            size_t total = carry + want;
            for (size_t i = 0; i + needleLen <= total && matches.size() < maxMatches; ++i) {
                if (memcmp(buffer.data() + i, needle, needleLen) == 0)
                    matches.push_back(at - carry + i);
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
static uint64_t ox_scanClassByName(const char *className, const char *nameSpace,
                                   const std::vector<OxMapRange> &scanMaps,
                                   uint64_t strLo, uint64_t strHi) {
    const size_t chunkSize = 1024 * 1024;
    std::vector<uint8_t> buffer(chunkSize);
    for (const OxMapRange &map : scanMaps) {
        if ((map.end - map.start) > 128ULL * 1024 * 1024) continue; // пропускаем GC-кучу
        for (uint64_t at = map.start; at < map.end;) {
            size_t want = (size_t)std::min<uint64_t>(chunkSize, map.end - at);
            if (!vm_readv(at, buffer.data(), want)) { at += want; continue; }
            for (size_t i = 0; i + sizeof(uint64_t) <= want; i += sizeof(uint64_t)) {
                uint64_t namePtr = 0;
                memcpy(&namePtr, buffer.data() + i, sizeof(namePtr));
                if (namePtr < strLo || namePtr >= strHi) continue; // должен вести в пул строк
                if (at + i < ox::CLASS_NAME) continue;
                if (!ox_asciiEquals(namePtr, className)) continue;
                uint64_t klass = at + i - ox::CLASS_NAME;
                uint64_t nsPtr = rpm<uint64_t>(klass + ox::CLASS_NAMESPACE);
                if (nameSpace && !ox_asciiEquals(nsPtr, nameSpace)) continue;
                return klass;
            }
            at += want;
        }
    }
    return 0;
}

// Finds the writable libil2cpp TypeInfo slot containing klass and converts its
// runtime address to an RVA from il2cpp_base.
static uint64_t ox_findTypeInfoRVA(uint64_t klass, const std::vector<OxMapRange> &libRwMaps) {
    const size_t chunkSize = 1024 * 1024;
    std::vector<uint8_t> buffer(chunkSize);
    for (const OxMapRange &map : libRwMaps) {
        for (uint64_t at = map.start; at < map.end;) {
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
static std::string g_metadataDumpStatus = "No metadata dump yet";

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

static void ox_dumpDecryptedMetadata() {
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
    snprintf(outputPath, sizeof(outputPath),
             "/sdcard/Download/global-metadata_%04d%02d%02d_%02d%02d%02d.dat",
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
        // Скан анонимной памяти дорогой — троттлим, чтобы не морозить оверлей до матча.
        static double s_lastTry = -1000.0;
        double now = ImGui::GetTime();
        if (now - s_lastTry >= 2.5) {
            s_lastTry = now;
            OXLOGD("[AUTO] попытка резолва классов (скан анон-памяти)...");
            ox_autoResolveTypeInfos(full);
        }
    }
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
    Vector3 wPos(0, 0, 0);
    uint64_t transform = rpm<uint64_t>(player + ox::PM_TRANSFORM);
    if (full) oxlog::ptrstep("player + TRANSFORM(0x68)", player + ox::PM_TRANSFORM, transform);
    ox_transformWorld(transform, &wPos, nullptr, full, "  transform + m_CachedPtr(0x10)");
    if (full) OXLOGT("  worldPos = (%.2f, %.2f, %.2f)", wPos.x, wPos.y, wPos.z);
    return wPos;
}

// Полный базис камеры. m_LookRoot (mouseLook+0x28) даёт ТОЛЬКО yaw (мировой
// кватернион, forward.y всегда 0). PITCH хранится отдельным углом в градусах
// на mouseLook+0x60 (подтверждено PDUMP: при взгляде вниз ~+80° у клампа).
// Итог: fullRot = yawQuat * pitchQuat(вокруг X). Инверты — на стороне вызова.
static bool ox_readLookBasis(uint64_t player, Vector3 &right, Vector3 &up,
                             Vector3 &fwd, Vector3 *eyePos, bool full) {
    // yaw-кватернион из m_LookRoot, fallback — worldCameraRoot.
    // eyePos (если задан) = мировая позиция m_LookRoot = истинный глаз камеры.
    Quat yawQ; bool haveYaw = false;
    uint64_t mouseLook = rpm<uint64_t>(player + ox::PM_MOUSELOOK);
    float pitchDeg = 0.f; bool havePitch = false;
    if (ox_isPtr(mouseLook)) {
        uint64_t lookRoot = rpm<uint64_t>(mouseLook + ox::ML_LOOKROOT);
        Vector3 lrPos;
        if (ox_transformWorld(lookRoot, &lrPos, &yawQ, false, nullptr)) {
            float n2 = yawQ.x*yawQ.x + yawQ.y*yawQ.y + yawQ.z*yawQ.z + yawQ.w*yawQ.w;
            if (n2 > 0.8f && n2 < 1.2f) { haveYaw = true; if (eyePos) *eyePos = lrPos; }
        }
        pitchDeg = rpm<float>(mouseLook + ox::ML_PITCH);
        if (pitchDeg > -89.f && pitchDeg < 89.f) havePitch = true;
    }
    if (!haveYaw) {
        uint64_t camRoot = rpm<uint64_t>(player + ox::PM_TRANSFORM);
        if (!ox_transformWorld(camRoot, nullptr, &yawQ, false, nullptr)) return false;
        float n2 = yawQ.x*yawQ.x + yawQ.y*yawQ.y + yawQ.z*yawQ.z + yawQ.w*yawQ.w;
        if (n2 < 0.8f || n2 > 1.2f) return false;
    }

    // ФИКС ESP: базис камеры строим из ПОЛНОЙ мировой ориентации m_LookRoot.
    // В этом билде рига ECM пичит сам lookRoot, поэтому его кватернион уже
    // содержит yaw+pitch. Отдельный pitch-угол (mouseLook+ML_PITCH) в новом
    // дампе лежит по обфусцированному оффсету (0x60 = Vector2 kqP) -> мусор,
    // из-за него боксы «плыли» по вертикали. Используем его ТОЛЬКО как fallback,
    // если lookRoot оказался чисто-yaw (forward.y ~ 0).
    Quat full_q = yawQ;

    // Несёт ли lookRoot уже наклон камеры? forward.y заметно отличается от 0.
    Vector3 fwd0 = quatRotate(yawQ, Vector3(0, 0, 1));
    bool lookRootHasPitch = fabsf(fwd0.y) > 0.02f;

    if (camUseLookRootPitch && lookRootHasPitch) {
        // lookRoot уже содержит полный наклон — берём кватернион как есть.
        full_q = yawQ;
    } else if (havePitch) {
        // Fallback: складываем отдельный угол pitch вокруг локальной оси X.
        float a = pitchDeg * DEG2RAD;
        Quat pQ{ sinf(a * 0.5f), 0.f, 0.f, cosf(a * 0.5f) };
        full_q = quatMul(yawQ, pQ);   // сначала yaw (мир), затем pitch (локальный X)
    }

    right = quatRotate(full_q, Vector3(1, 0, 0));
    up    = quatRotate(full_q, Vector3(0, 1, 0));
    fwd   = quatRotate(full_q, Vector3(0, 0, 1));
    if (full) OXLOGT("  lookBasis yaw=%d pitchDeg=%.2f(have=%d) lrPitch=%d fwd=(%.3f,%.3f,%.3f)",
                     (int)haveYaw, pitchDeg, (int)havePitch, (int)lookRootHasPitch, fwd.x, fwd.y, fwd.z);
    return true;
}

// Строит камеру из КОНКРЕТНОГО (локального) игрока: позиция = его позиция +
// высота глаз, базис (right/up/forward) = yaw(m_LookRoot) * pitch(mouseLook+0x10).
// Дёшево (читает только один трансформ) — можно звать каждый кадр.
static CameraView ox_buildCameraFromPlayer(uint64_t player, bool full) {
    CameraView cam;
    cam.valid = false;
    cam.fovH  = camFov;
    if (!ox_isPtr(player)) return cam;

    Vector3 body = ox_readPos(player, full);

    Vector3 right{1,0,0}, up{0,1,0}, fwd{0,0,1};
    Vector3 eyePos{0,0,0}; bool haveEye = false;
    bool haveDir = ox_readLookBasis(player, right, up, fwd, &eyePos, full);
    // Истинный глаз камеры = позиция m_LookRoot (тот же трансформ, что даёт yaw).
    // Тело (worldCameraRoot +0x68) в этом билде даёт неверный Y (rig-высота),
    // поэтому позицию камеры берём из m_LookRoot, а не из body+eye.
    haveEye = (eyePos.x != 0.f || eyePos.y != 0.f || eyePos.z != 0.f);
    cam.pos = haveEye ? eyePos : (body + Vector3(0, ox::EYE_HEIGHT, 0));

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
        OXLOGI("  body(+0x68)=(%.2f,%.2f,%.2f) eye(m_LookRoot)=(%.2f,%.2f,%.2f) haveEye=%d",
               body.x, body.y, body.z, eyePos.x, eyePos.y, eyePos.z, (int)haveEye);
        OXLOGI("  cam.pos=(%.3f, %.3f, %.3f)", cam.pos.x, cam.pos.y, cam.pos.z);
        OXLOGI("  forward=(%.3f,%.3f,%.3f) haveBasis=%d", fwd.x, fwd.y, fwd.z, (int)haveDir);
        OXLOGI("  итог: yaw=%.4f pitch=%.4f fov=%.1f (invP=%d invY=%d)",
               cam.yaw, cam.pitch, cam.fovH, (int)camInvertPitch, (int)camInvertYaw);
    }
    return cam;
}

// Находит локального игрока в списке, запоми�����ет g_localPlayer и строит камеру.
static CameraView ox_getCamera(uint64_t elems, int count, bool full) {
    if (full) oxlog::section("CAMERA (local player)");
    for (int i = 0; i < count; i++) {
        uint64_t player = rpm<uint64_t>(elems + i * 8);
        if (!ox_isPtr(player)) continue;
        // В этом билде НЕТ прямого флага isLocalPlayer (0x188 = чужой bool k__BackingField).
        // Локального детектим по наличию активной связки камеры: mouseLook + raycastManager
        // ��аполнены только у своего игрока.
        uint64_t mouseLook = rpm<uint64_t>(player + ox::PM_MOUSELOOK);
        uint64_t raycast   = rpm<uint64_t>(player + ox::PM_RAYCASTMANAGER);
        bool isLocal = ox_isPtr(mouseLook) && ox_isPtr(raycast);
        if (full)
            OXLOGT("player[%d]=0x%llx isLocal=%d mouseLook=0x%llx raycast=0x%llx",
                   i, (unsigned long long)player, (int)isLocal,
                   (unsigned long long)mouseLook, (unsigned long long)raycast);
        if (!isLocal) continue;
        g_localPlayer = player;
        g_localTeamName = ox_readManagedString(rpm<uint64_t>(player + ox::PM_TEAM_NAME));
        if (full) OXLOGI("  local teamName='%s'", g_localTeamName.c_str());
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

// Читает Bounds в мировую AABB. Инфлейтит на LOS_AABB_INFLATE со всех сторон.
static bool ox_readWorldAABB(uint64_t building, WorldAABB &out) {
    out = WorldAABB{Vector3(0,0,0), Vector3(0,0,0)};
    if (!ox_isPtr(building)) return false;

    // World-позиция постройк�� (сервер-авторитетная).
    Vector3 worldPos = rpm<Vector3>(building + ox::BP_CORRECT_POSITION);
    // Bounds хранятся локально к pivot'у префаба. Если у постройки поворот 0,
    // центр можно просто сдвинуть на worldPos. Иначе нужен поворот — но т.к.
    // большинство стен в Oxide имеют осе-ориентированныйtation (кратный 90°),
    // экстенты всё равно корректны после переноса.
    // Читаем Bounds (m_Center 0x0, m_Extents 0xC) на building+BP_BOUNDS.
    uint64_t bAddr = building + ox::BP_BOUNDS;
    Vector3 center = rpm<Vector3>(bAddr + ox::BOUNDS_CENTER);
    Vector3 extents = rpm<Vector3>(bAddr + ox::BOUNDS_EXTENTS);

    // Валидность: extents должны быть положительными и не больше ~100м (sanity).
    if (extents.x < 0.f || extents.y < 0.f || extents.z < 0.f) return false;
    if (extents.x > 100.f || extents.y > 100.f || extents.z > 100.f) return false;
    if (!isfinite(center.x) || !isfinite(center.y) || !isfinite(center.z)) return false;
    if (!isfinite(extents.x) || !isfinite(extents.y) || !isfinite(extents.z)) return false;

    // World-space center = local center + world pos
    Vector3 worldCenter = Vector3(center.x + worldPos.x,
                                  center.y + worldPos.y,
                                  center.z + worldPos.z);
    float infl = ox::LOS_AABB_INFLATE;
    out.min = Vector3(worldCenter.x - extents.x - infl,
                      worldCenter.y - extents.y - infl,
                      worldCenter.z - extents.z - infl);
    out.max = Vector3(worldCenter.x + extents.x + infl,
                      worldCenter.y + extents.y + infl,
                      worldCenter.z + extents.z + infl);
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
    uint64_t saveList = rpm<uint64_t>(statics + ox::STATIC_BUILDING_SAVELIST);
    if (!ox_isPtr(saveList)) { if (full) OXLOGW("[LOS] saveList невалиден"); return; }

    uint64_t elems; int count;
    if (!ox_readList(saveList, elems, count)) { if (full) OXLOGW("[LOS] saveList пуст/мусор"); return; }
    if (full) OXLOGI("[LOS] BuildingPiece.saveList count=%d", count);

    std::vector<BuildingCacheEntry> newCache;
    newCache.reserve(count);
    int valid = 0;
    for (int i = 0; i < count; i++) {
        uint64_t b = rpm<uint64_t>(elems + i * 8);
        if (!ox_isPtr(b)) continue;
        BuildingCacheEntry e;
        e.address = b;
        e.valid = ox_readWorldAABB(b, e.bounds);
        if (e.valid) {
            // Доп. фильтр:Building еще стоит (health > 0)
            float hp = rpm<float>(b + ox::BP_HEALTH);
            if (hp <= 0.f || !isfinite(hp)) continue;
            newCache.push_back(e);
            valid++;
        }
    }

    if (full) OXLOGI("[LOS] кэш построек: %d/%d валидны", valid, count);
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

    std::lock_guard<std::mutex> lock(g_buildingMutex);
    for (const BuildingCacheEntry &e : g_buildingCache) {
        if (!e.valid) continue;
        float tNear, tFar;
        if (!ox_rayAABB(origin, dirN, e.bounds, tNear, tFar)) continue;
        // Пересечение в диапазоне [0, distance - eps]
        if (tNear < distance - ox::LOS_TARGET_EPS) {
            // Стена между камерой и целью (или цель внутри стены — редкость).
            if (tFar > 0.f) return false;
        }
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
static std::string g_runtimeDumpStatus = "No runtime dump yet";

static void ox_dumpRuntimeProfile() {
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
    uint64_t saveList = buildingStatics
        ? rpm<uint64_t>(buildingStatics + ox::STATIC_BUILDING_SAVELIST)
        : 0;
    uint64_t buildingElems = 0;
    int buildingCount = 0;
    bool buildingListOk = ox_readList(saveList, buildingElems, buildingCount);
    OXLOGI("[DUMP] BuildingPiece: statics=0x%llx saveList=0x%llx elems=0x%llx count=%d ok=%d",
           (unsigned long long)buildingStatics,
           (unsigned long long)saveList,
           (unsigned long long)buildingElems,
           buildingCount,
           (int)buildingListOk);

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

static Vector3 ox_aimPoint(Vector3 playerPosition) {
    switch (aimcurbone) {
        case 0: return playerPosition + Vector3(0.f, ox::BOX_HEAD * 0.5f, 0.f); // head
        case 1: return playerPosition - Vector3(0.f, 0.20f, 0.f);                 // neck
        default: return playerPosition - Vector3(0.f, 0.85f, 0.f);                // body
    }
}

// Selects the target nearest the crosshair and adjusts the local MouseLook angles.
static void ox_runAimbot(const CameraView& cam, const std::vector<PlayerCacheData>& players) {
    if (!aimm || !cam.valid || !ox_isPtr(g_localPlayer)) return;

    const ImVec2 screenCenter((float)abs_ScreenX * 0.5f, (float)abs_ScreenY * 0.5f);
    const float maxFovSquared = aimFovPixels * aimFovPixels;
    Vector3 bestPoint;
    float bestFovSquared = maxFovSquared;
    bool foundTarget = false;

    for (const PlayerCacheData& player : players) {
        if (!ox_isPtr(player.address)) continue;
        if (!g_localTeamName.empty() && player.teamName == g_localTeamName) continue;
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

        std::string teamName = ox_readManagedString(rpm<uint64_t>(player + ox::PM_TEAM_NAME));
        if (espHideTeammates && !g_localTeamName.empty() && teamName == g_localTeamName) {
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
        data.teamName = teamName;
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

        if (!data.isVisibleBottom && !data.isVisibleTop) { skippedVis++; continue; }

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

	// Полный стартовый диагностический дамп в файл лога (Download).
	ox_logStartupDiagnostics();
    
    Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);
    
    while (main_thread_flag) {
        drawBegin();
        Layout_tick_UI();
        
        ExecuteLuaScripts();
        
        // обновляем кэш только раз в N кадров (интервал регулируется в меню)
        bool cacheUpdatedThisFrame = false;
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
                // Сохраняем флаг в player.losBlocked — aimbot и color его переиспользуют.
                if (espColorByVisibility || aimCheckWalls) {
                    if (liveCam.valid) {
                        player.losBlocked = !ox_isTargetVisible(liveCam.pos, livePos);
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

// ---- Палитра меню (единый источник цвета для стиля и виджетов) ----
// Палитра «Eclips Oxide»: магента-свечение (как магический шар на арте) +
// глубокий сине-чёрный фон с фиолетовым подтоном. 5 цветов, без лишнего.
namespace oxui {
    static const ImVec4 BG       = ImVec4(0.055f, 0.045f, 0.085f, 0.97f); // глубокий фиолет-чёрный
    static const ImVec4 PANEL    = ImVec4(0.090f, 0.075f, 0.130f, 0.86f); // полупрозр. — фон-арт виден
    static const ImVec4 PANEL2   = ImVec4(0.140f, 0.115f, 0.200f, 0.92f);
    static const ImVec4 PANEL3   = ImVec4(0.200f, 0.165f, 0.280f, 0.96f);
    static const ImVec4 ACCENT   = ImVec4(0.880f, 0.380f, 0.870f, 1.00f); // магента-свечение
    static const ImVec4 ACCENT_D = ImVec4(0.600f, 0.250f, 0.600f, 1.00f);
    static const ImVec4 DANGER   = ImVec4(0.920f, 0.300f, 0.360f, 1.00f);
    static const ImVec4 DANGER_D = ImVec4(0.700f, 0.210f, 0.260f, 1.00f);
    static const ImVec4 TEXT     = ImVec4(0.950f, 0.930f, 0.970f, 1.00f);
    static const ImVec4 TEXT_DIM = ImVec4(0.620f, 0.570f, 0.700f, 1.00f);
}

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

// Статичный счётчик врагов сверху по цент��у. Тап по нему открывает/закрывает
// меню. Показывает «ENEMIES N»; цвет — акцент когда есть враги, тускло если 0.
static void oxDrawCounter(const ImVec2& ds) {
    using namespace oxui;
    const float W = 300.f, H = 82.f;
    float x = ds.x * 0.5f - W * 0.5f;
    float y = 24.f;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
    ImGui::Begin(oxorany("##oxcounter"), nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings);

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(oxorany("##cnthit"), ImVec2(W, H));
    if (ImGui::IsItemClicked()) g_menuOpen = !g_menuOpen;   // тап = тумбл меню
    bool hov = ImGui::IsItemHovered();

    int cnt = g_enemyCount;
    ImVec4 accV = cnt > 0 ? DANGER : ACCENT;               // красный если враги рядом
    ImU32 accent = ImGui::ColorConvertFloat4ToU32(accV);
    ImU32 shadow = IM_COL32(0, 0, 0, 150);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a(p.x, p.y), b(p.x + W, p.y + H);
    // тень + панель + акцентная рамка
    dl->AddRectFilled(ImVec2(a.x, a.y + 5), ImVec2(b.x, b.y + 5), shadow, 18.f);
    dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(
        ImVec4(BG.x, BG.y, BG.z, hov ? 1.0f : 0.94f)), 18.f);
    dl->AddRect(a, b, accent, 18.f, 0, 2.5f);
    // левый акцентный маркер
    dl->AddRectFilled(a, ImVec2(a.x + 8, b.y), accent, 18.f, ImDrawFlags_RoundCornersLeft);

    // Подпись «ENEMIES» слева (с тенью для читаемости) + большое число справа.
    // ВАЖНО: oxorany(...) возвращает ВРЕМЕННЫЙ буфер — копируем сразу, иначе висячий указатель = мусор.
    ImFont* f = ImGui::GetFont();
    char lbl[16]; snprintf(lbl, sizeof(lbl), "%s", oxorany("ENEMIES"));
    ImVec2 ls = ImGui::CalcTextSize(lbl);
    float ly = p.y + H * 0.5f - ls.y * 0.5f;
    dl->AddText(ImVec2(p.x + 27, ly + 1), IM_COL32(0, 0, 0, 180), lbl);  // тень
    dl->AddText(ImVec2(p.x + 26, ly), accent, lbl);                       // сам текст
    char num[16]; snprintf(num, sizeof(num), "%d", cnt);
    float big = ImGui::GetFontSize() * 2.1f;
    ImVec2 ns = f->CalcTextSizeA(big, FLT_MAX, 0, num);
    dl->AddText(f, big, ImVec2(p.x + W - ns.x - 25, p.y + H * 0.5f - ns.y * 0.5f + 1),
                IM_COL32(0, 0, 0, 180), num);
    dl->AddText(f, big, ImVec2(p.x + W - ns.x - 26, p.y + H * 0.5f - ns.y * 0.5f),
                ImGui::ColorConvertFloat4ToU32(TEXT), num);

    g_window = ImGui::GetCurrentWindow();  // для orientation-хака в drawBegin
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// Вкладка-«пилюля»: активная — залита акцентом со свечением и точкой-индикатором,
// неактивная — полупрозрачная стеклянная (арт слегка просвечивает).
static bool oxTabButton(const char* label, int idx, float w) {
    using namespace oxui;
    bool active = (g_activeTab == idx);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.f);   // полная пилюля
    ImGui::PushStyleColor(ImGuiCol_Button,        active ? ACCENT : ImVec4(PANEL2.x, PANEL2.y, PANEL2.z, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? ACCENT : PANEL3);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ACCENT_D);
    ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.06f, 0.04f, 0.10f, 1) : TEXT);
    bool clicked = ImGui::Button(label, ImVec2(w, 0));
    if (clicked) g_activeTab = idx;
    // свечение вокруг активной пилюли + точка-индикатор под ней
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) {
        dl->AddRect(ImVec2(mn.x - 2, mn.y - 2), ImVec2(mx.x + 2, mx.y + 2),
                    IM_COL32(225, 100, 220, 90), 999.f, 0, 3.f);
        dl->AddCircleFilled(ImVec2((mn.x + mx.x) * 0.5f, mx.y + 8.f), 3.f,
                            ImGui::ColorConvertFloat4ToU32(ACCENT), 12);
    } else {
        dl->AddRect(mn, mx, IM_COL32(225, 100, 220, 40), 999.f, 0, 1.f);
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    return clicked;
}

// ---- Содержимое вкладок ----
static void oxTabESP() {
    using namespace oxui;
    ImGui::Checkbox(oxorany("Enable ESP"), &espdraw);
    if (ImGui::Checkbox(oxorany("Hide teammates"), &espHideTeammates))
        cache_needs_update = true;
    // --- Видимость через стены (зелёный/синий) ---
    ImGui::Checkbox(oxorany("Color by visibility"), &espColorByVisibility);
    if (espColorByVisibility) {
        ImGui::TextColored(ImVec4(60/255.f, 230/255.f, 110/255.f, 1.f), oxorany("  Visible  "));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(80/255.f, 150/255.f, 255/255.f, 1.f), oxorany("Behind wall"));
        ImGui::TextColored(TEXT_DIM, oxorany("Reads BuildingPiece.saveList (walls)"));
    }
    ImGui::Checkbox(oxorany("Box"), &espbox);
    if (espbox) {
        ImGui::SliderFloat(oxorany("Stroke"), &espstroke, 0.0f, 5.0f);
        ImGui::Checkbox(oxorany("Corner box"), &espCornerBox);
        if (espCornerBox)
            ImGui::SliderFloat(oxorany("Corner length"), &espCornerLength, 0.16f, 0.45f);
        ImGui::Checkbox(oxorany("Filled##box"), &espfill);
        if (espfill) {
            ImGui::SliderFloat(oxorany("Fill value##box"), &espfillp, 20, 80);
            ImGui::Checkbox(oxorany("Fill gradient##box"), &espgradient);
        }
        // Цвет бокса доступен только если не включена окраска по видимости —
        // иначе он переопределяется зелёным/синим.
        if (!espColorByVisibility)
            ImGui::ColorEdit3(oxorany("Box color"), espboxcolor, ImGuiColorEditFlags_NoInputs);
    }
    ImGui::Checkbox(oxorany("Health bar"), &esphealth);
    if (esphealth) {
        ImGui::SliderFloat(oxorany("Stroke##hp"), &esphpsize, 0.0f, 10.0f);
        ImGui::Checkbox(oxorany("Gradient##hp"), &esphpgradient);
    }
    ImGui::Checkbox(oxorany("Distance"), &espdist);
    ImGui::Checkbox(oxorany("Name"),     &espname);
    ImGui::Checkbox(oxorany("Weapon"),   &espweapon);
    ImGui::Checkbox(oxorany("3D box"),   &ESP3D);
    ImGui::Checkbox(oxorany("ESP lines"), &esplines_enabled);
    if (esplines_enabled) {
        ImGui::SliderFloat(oxorany("Line thickness"), &esplines_thickness, 1.0f, 6.0f);
        ImGui::ColorEdit3(oxorany("Line color"), esplines_color, ImGuiColorEditFlags_NoInputs);
        const char* origins[] = { "Bottom center", "Crosshair" };
        ImGui::Combo(oxorany("Line origin"), &esplines_position, origins, IM_ARRAYSIZE(origins));
    }
    ImGui::SliderFloat(oxorany("Max distance (m)"), &espMaxDistance, 0.0f, 10000.0f, "%.0f");
}

static void oxTabAim() {
    using namespace oxui;
    ImGui::Checkbox(oxorany("Enable aimbot"), &aimm);
    if (!aimm) return;

    ImGui::SliderFloat(oxorany("Aim FOV (px)"), &aimFovPixels, 20.0f, 600.0f, "%.0f");
    ImGui::SliderFloat(oxorany("Smoothing"), &aimSmooth, 1.0f, 20.0f, "%.1f");
    ImGui::SliderFloat(oxorany("Max distance (m)##aim"), &aimMaxDistance, 0.0f, 1000.0f, "%.0f");
    const char* bones[] = { "Head", "Neck", "Body" };
    ImGui::Combo(oxorany("Target point"), &aimcurbone, bones, IM_ARRAYSIZE(bones));
    // --- LOS-чек: аим не наводится через стены ---
    ImGui::Checkbox(oxorany("Check walls (no aim through)"), &aimCheckWalls);
    if (aimCheckWalls) {
        ImGui::TextColored(TEXT_DIM, oxorany("Uses BuildingPiece.saveList AABB raycast"));
    }
    ImGui::Checkbox(oxorany("Movement prediction"), &aimPrediction);
    if (aimPrediction) {
        ImGui::SliderFloat(oxorany("Projectile speed (m/s)"), &aimProjectileSpeed, 20.0f, 1000.0f, "%.0f");
        ImGui::SliderFloat(oxorany("Latency (ms)"), &aimLatencyMs, 0.0f, 250.0f, "%.0f");
        ImGui::SliderFloat(oxorany("Max lead time (s)"), &aimMaxLeadTime, 0.05f, 0.75f, "%.2f");
    }
    ImGui::Checkbox(oxorany("Show FOV circle"), &aimDrawFov);
    ImGui::TextColored(TEXT_DIM, oxorany("Smoothing 1 = instant target adjustment."));
}

static void oxTabCamera() {
    using namespace oxui;
    ImGui::TextColored(ACCENT, oxorany("Camera / W2S tuning"));
    ImGui::Spacing();
    ImGui::SliderFloat(oxorany("FOV"), &camFov, 30.0f, 120.0f, "%.3f");
    ImGui::TextColored(TEXT_DIM, oxorany("Calibrated default: 98.607"));
    ImGui::Spacing();
    ImGui::Checkbox(oxorany("Invert pitch"), &camInvertPitch);
    ImGui::Checkbox(oxorany("Invert yaw"),   &camInvertYaw);
    ImGui::Checkbox(oxorany("Swap yaw/pitch"), &camSwapAngles);
    ImGui::Spacing();
    ImGui::Checkbox(oxorany("Use LookRoot full pitch (ESP fix)"), &camUseLookRootPitch);
    ImGui::TextColored(TEXT_DIM, oxorany("ON: read camera tilt from LookRoot (recommended). OFF: legacy pitch offset."));
}

static void oxTabSettings() {
    using namespace oxui;
    static float opacity = 1.0f;
    ImGui::SliderFloat(oxorany("UI Opacity"), &opacity, 0.35f, 1.0f);
    ImGui::GetStyle().Alpha = opacity;
    ImGui::Text(oxorany("FPS: %.0f"), ImGui::GetIO().Framerate);

    ImGui::Separator();
    ImGui::TextColored(ACCENT, oxorany("Performance"));
    ImGui::SliderInt(oxorany("Heavy scan interval"), &g_cacheInterval, 4, 30);
    ImGui::SliderInt(oxorany("Position interval"), &g_positionInterval, 1, 4);
    ImGui::TextColored(TEXT_DIM, oxorany("Default 10/2 reduces reads while camera projection stays per-frame."));

    ImGui::Separator();
    ImGui::TextColored(ACCENT, oxorany("Runtime offsets"));
    ImGui::TextColored(TEXT_DIM, oxorany("Dump PlayerManager, MouseLook, camera, players and buildings."));
    if (ImGui::Button(oxorany("Runtime dump to Download"), ImVec2(-1, 0)))
        ox_dumpRuntimeProfile();
    ImGui::TextWrapped("%s", g_runtimeDumpStatus.c_str());
    if (ImGui::Button(oxorany("Extract decrypted metadata"), ImVec2(-1, 0)))
        ox_dumpDecryptedMetadata();
    ImGui::TextWrapped("%s", g_metadataDumpStatus.c_str());

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, DANGER);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, DANGER_D);
    if (ImGui::Button(oxorany("Exit Cheat"), ImVec2(-1, 0))) main_thread_flag = false;
    if (ImGui::Button(oxorany("Unload Cheat"), ImVec2(-1, 0))) exit(0);
    ImGui::PopStyleColor(2);
}

void Layout_tick_UI() {
    using namespace oxui;
    oxApplyStyle();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;

    // ---- Большое центрированное меню ----
    if (g_menuOpen) {
        const float margin = 24.f;
        const float maxWidth = ds.x > margin * 2.f ? ds.x - margin * 2.f : ds.x;
        const float maxHeight = ds.y > margin * 2.f ? ds.y - margin * 2.f : ds.y;
        ImVec2 sz(ds.x * 0.68f, ds.y * 0.88f);
        if (sz.x < 640.f && maxWidth >= 640.f) sz.x = 640.f;
        if (sz.x > maxWidth) sz.x = maxWidth;
        if (sz.y > maxHeight) sz.y = maxHeight;
        ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
        // NoBackground — фон рисуем сами (фото-арт + затемнение).
        ImGui::Begin(oxorany("##oxmenu"), nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoSavedSettings);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 wsz = ImGui::GetWindowSize();
        ImVec2 wmin = wp, wmax(wp.x + wsz.x, wp.y + wsz.y);
        const float RND = ::clamp<float>(wsz.y * 0.045f, 22.f, 42.f);

        // — Фон-арт: cover-fit (без искажений), скруглённые углы —
        int iw = 0, ih = 0;
        ImTextureID bg = oxGetMenuBg(&iw, &ih);
        if (bg && iw > 0 && ih > 0) {
            float wr = wsz.x / wsz.y, ir = (float)iw / (float)ih;
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (ir > wr) {                       // арт шире окна — обрезаем бока
                float s = (wr / ir) * 0.5f; uv0.x = 0.5f - s; uv1.x = 0.5f + s;
            } else {                             // арт выше окна — обрезаем верх/низ
                float s = (ir / wr) * 0.5f; uv0.y = 0.5f - s; uv1.y = 0.5f + s;
            }
            dl->AddImageRounded(bg, wmin, wmax, uv0, uv1, IM_COL32(255,255,255,255), RND);
        } else {
            dl->AddRectFilled(wmin, wmax, ImGui::ColorConvertFloat4ToU32(BG), RND);
        }
        // — Затемнение: базовое + вертикальный градиент вниз для читаемости —
        dl->AddRectFilled(wmin, wmax, IM_COL32(10, 8, 20, 150), RND);
        dl->AddRectFilledMultiColor(wmin, wmax,
            IM_COL32(8, 6, 16, 90),  IM_COL32(8, 6, 16, 90),
            IM_COL32(8, 6, 16, 220), IM_COL32(8, 6, 16, 220));
        // — Тонкая магента-рамка + мягкое свечение —
        dl->AddRect(wmin, wmax, ImGui::ColorConvertFloat4ToU32(ACCENT), RND, 0, 2.5f);
        dl->AddRect(ImVec2(wmin.x-1, wmin.y-1), ImVec2(wmax.x+1, wmax.y+1),
                    IM_COL32(225, 100, 220, 60), RND, 0, 4.f);

        // — Премиум-шапка: крупный свечёный логотип Eclips Oxide + слоган + FPS-чип —
        // ВАЖНО: oxorany(...) возвращает ВРЕМЕННЫЙ буфер — копируем в локальные массивы,
        // иначе указатель повисает и рисуется мусор.
        ImFont* hf = ImGui::GetFont();
        float titleSz = ImGui::GetFontSize() * 1.7f;
        ImVec2 hp0 = ImGui::GetCursorScreenPos();
        char t1[8]; snprintf(t1, sizeof(t1), "%s", oxorany("Eclips"));
        char t2[8]; snprintf(t2, sizeof(t2), "%s", oxorany(" Oxide"));
        ImVec2 s1 = hf->CalcTextSizeA(titleSz, FLT_MAX, 0, t1);
        // мягкое свечение под заголовком (несколько смещённых копий)
        ImU32 glowA = IM_COL32(225, 100, 220, 45);
        for (int gx = -2; gx <= 2; gx += 2) for (int gy = -2; gy <= 2; gy += 2)
            dl->AddText(hf, titleSz, ImVec2(hp0.x + gx, hp0.y + gy), glowA, t1);
        dl->AddText(hf, titleSz, hp0, ImGui::ColorConvertFloat4ToU32(ACCENT), t1);
        dl->AddText(hf, titleSz, ImVec2(hp0.x + s1.x, hp0.y),
                    ImGui::ColorConvertFloat4ToU32(TEXT), t2);
        // FPS-чип в пра��ом верхнем углу шапки
        {
            char fps[24]; snprintf(fps, sizeof(fps), "%.0f FPS", io.Framerate);
            ImVec2 fs = ImGui::CalcTextSize(fps);
            float chipW = fs.x + 20.f, chipH = fs.y + 10.f;
            ImVec2 ca(wmax.x - 30.f - chipW, hp0.y), cb(wmax.x - 30.f, hp0.y + chipH);
            dl->AddRectFilled(ca, cb, IM_COL32(20, 14, 32, 190), 8.f);
            dl->AddRect(ca, cb, ImGui::ColorConvertFloat4ToU32(ACCENT), 8.f, 0, 1.5f);
            dl->AddText(ImVec2(ca.x + 10.f, ca.y + 5.f),
                        ImGui::ColorConvertFloat4ToU32(ACCENT), fps);
        }
        ImGui::Dummy(ImVec2(0, titleSz + 4.f));
        // слоган + акцентный подчёркивающий бар
        ImGui::TextColored(TEXT_DIM, oxorany("Survival Island  ·  v0.2  ·  @xohyw"));
        {
            ImVec2 pmin = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(ImVec2(pmin.x, pmin.y + 7), ImVec2(pmin.x + 90, pmin.y + 11),
                              ImGui::ColorConvertFloat4ToU32(ACCENT), 4.f);
            dl->AddRectFilled(ImVec2(pmin.x + 96, pmin.y + 8), ImVec2(pmin.x + 116, pmin.y + 10),
                              IM_COL32(225, 100, 220, 120), 4.f);
        }
        // Compact live-status band so the header remains useful, not decorative.
        {
            ImVec2 sp = ImGui::GetCursorScreenPos();
            const float cardH = 42.f;
            const float gap = 10.f;
            const float cardW = (ImGui::GetContentRegionAvail().x - gap * 2.f) / 3.f;
            const ImVec4 states[] = {
                espdraw ? ACCENT : TEXT_DIM,
                aimm ? DANGER : TEXT_DIM,
                g_enemyCount > 0 ? DANGER : ACCENT,
            };
            char values[3][28];
            snprintf(values[0], sizeof(values[0]), "ESP  %s", espdraw ? "ON" : "OFF");
            snprintf(values[1], sizeof(values[1]), "AIM  %s", aimm ? "ON" : "OFF");
            snprintf(values[2], sizeof(values[2]), "TARGETS  %d", g_enemyCount);
            for (int i = 0; i < 3; ++i) {
                ImVec2 a(sp.x + (cardW + gap) * i, sp.y);
                ImVec2 b(a.x + cardW, a.y + cardH);
                ImU32 state = ImGui::ColorConvertFloat4ToU32(states[i]);
                dl->AddRectFilled(a, b, IM_COL32(15, 11, 26, 190), 12.f);
                dl->AddRect(a, b, IM_COL32(225, 100, 220, 70), 12.f, 0, 1.f);
                dl->AddCircleFilled(ImVec2(a.x + 16.f, a.y + cardH * 0.5f), 4.f, state, 12);
                dl->AddText(ImVec2(a.x + 28.f, a.y + cardH * 0.5f - ImGui::GetFontSize() * 0.5f),
                            ImGui::ColorConvertFloat4ToU32(TEXT), values[i]);
            }
            ImGui::Dummy(ImVec2(0, cardH + 18.f));
        }

        // — Ряд вкладок —
        float avail = ImGui::GetContentRegionAvail().x;
        float gap   = ImGui::GetStyle().ItemSpacing.x;
        float tw    = (avail - gap * 3.f) / 4.f;
        oxTabButton(oxorany("ESP"),      0, tw); ImGui::SameLine();
        oxTabButton(oxorany("Aim"),      1, tw); ImGui::SameLine();
        oxTabButton(oxorany("Camera"),   2, tw); ImGui::SameLine();
        oxTabButton(oxorany("Settings"), 3, tw);
        ImGui::Dummy(ImVec2(0, 12));

        // — Контент (прокручиваемый, «стеклянная» панель поверх арта) —
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(PANEL.x, PANEL.y, PANEL.z, 0.72f));
        ImGui::BeginChild(oxorany("##oxcontent"), ImVec2(0, 0), true);
        {   // тонкая светящаяся линия по верхнему краю панели
            ImVec2 cmn = ImGui::GetWindowPos();
            float cw = ImGui::GetWindowWidth();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(cmn.x + 18, cmn.y), ImVec2(cmn.x + cw - 18, cmn.y + 2),
                IM_COL32(225, 100, 220, 110), 2.f);
        }
        ImGui::Dummy(ImVec2(0, 4));
        switch (g_activeTab) {
            case 0: oxTabESP();      break;
            case 1: oxTabAim();      break;
            case 2: oxTabCamera();   break;
            default: oxTabSettings(); break;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();   // ChildBg
        ImGui::End();
    }

    // ---- Плавающ��я кнопка (поверх) ----
    oxDrawCounter(ds);
}
