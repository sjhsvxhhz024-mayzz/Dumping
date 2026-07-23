#include "LuaIntegration.h"
#include "memory.h"
#include "utils.h"
#include "oxorany/oxorany.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
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

lua_State* L = nullptr;
std::vector<std::string> luaScripts;
std::string activeScriptPath;

// для репозитория
std::vector<std::string> repoScripts;
std::string repoStatus = "Not fetched";
bool repoFetched = false;
bool isFetching = false;
std::mutex repoMutex;

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

// получаем список скриптов из репозитория (хардкод для примера)
void parseGitHubRepo() {
    std::vector<std::string> scripts;
    
    // временный файл для индекса
    std::string indexPath = "/sdcard/chuvashi/repo/_index.txt";
    
    // скачиваем индекс
    std::string url = oxorany("https://raw.githubusercontent.com/litvinchikkk/lua-repo-chuvashi/main/index.txt");
    
    // пробуем wget
    std::string cmd = "wget -q --timeout=10 -O " + indexPath + " '" + url + "' 2>/dev/null";
    int result = system(cmd.c_str());
    
    if (result != 0) {
        // пробуем curl
        cmd = "curl -s --connect-timeout 10 -o " + indexPath + " '" + url + "' 2>/dev/null";
        result = system(cmd.c_str());
    }
    
    if (result == 0) {
        // читаем индекс
        std::ifstream indexFile(indexPath);
        if (indexFile.is_open()) {
            std::string line;
            while (std::getline(indexFile, line)) {
                // убираем пробелы и пустые строки
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);
                
                if (!line.empty() && line.length() > 4 && 
                    line.substr(line.length() - 4) == ".lua") {
                    scripts.push_back(line);
                }
            }
            indexFile.close();
        }
        
        // удаляем временный файл
        remove(indexPath.c_str());
    }
    
    // если не удалось скачать индекс, используем хардкод как fallback
    if (scripts.empty()) {
        scripts.push_back("radar.lua");
    }
    
    {
        std::lock_guard<std::mutex> lock(repoMutex);
        repoScripts = scripts;
        
        if (repoScripts.empty()) {
            repoStatus = "No scripts found";
        } else {
            repoStatus = "Found " + std::to_string(repoScripts.size()) + " scripts";
        }
        
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
    
    // создаем папку если нет
    system("mkdir -p /sdcard/chuvashi/repo 2>/dev/null");
    
    // небольшая задержка
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    // парсим репозиторий
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
        if (idx < 0 || idx >= repoScripts.size()) {
            __android_log_print(ANDROID_LOG_ERROR, "Lua", "Invalid repo script index");
            return;
        }
        scriptName = repoScripts[idx];
    }
    
    __android_log_print(ANDROID_LOG_INFO, "Lua", "Downloading: %s", scriptName.c_str());
    
    // создаем папки
    system("mkdir -p /sdcard/chuvashi/repo 2>/dev/null");
    system("mkdir -p /sdcard/chuvashi/scripts 2>/dev/null");
    
    // пробуем разные методы скачивания
    std::string url = std::string(oxorany("https://raw.githubusercontent.com/litvinchikkk/lua-repo-chuvashi/main/")) + scriptName;
    std::string repoPath = "/sdcard/chuvashi/repo/" + scriptName;
    std::string scriptPath = "/sdcard/chuvashi/scripts/" + scriptName;
    
    bool downloaded = false;
    
    // метод 1: через wget (есть на большинстве рутованных устройств)
    std::string cmd1 = "wget -q --timeout=10 --tries=2 -O " + repoPath + " '" + url + "' 2>/dev/null";
    int result1 = system(cmd1.c_str());
    
    if (result1 == 0) {
        __android_log_print(ANDROID_LOG_INFO, "Lua", "Downloaded via wget");
        downloaded = true;
    } else {
        // метод 2: через curl (альтернатива)
        std::string cmd2 = "curl -s --connect-timeout 10 --max-time 30 -o " + repoPath + " '" + url + "' 2>/dev/null";
        int result2 = system(cmd2.c_str());
        
        if (result2 == 0) {
            __android_log_print(ANDROID_LOG_INFO, "Lua", "Downloaded via curl");
            downloaded = true;
        } else {
            // метод 3: через busybox wget (если busybox установлен)
            std::string cmd3 = "busybox wget -q -O " + repoPath + " '" + url + "' 2>/dev/null";
            int result3 = system(cmd3.c_str());
            
            if (result3 == 0) {
                __android_log_print(ANDROID_LOG_INFO, "Lua", "Downloaded via busybox wget");
                downloaded = true;
            }
        }
    }
    
    if (!downloaded) {
        __android_log_print(ANDROID_LOG_ERROR, "Lua", "Failed to download script");
        {
            std::lock_guard<std::mutex> lock(repoMutex);
            repoStatus = "Download failed (no network/wget/curl)";
        }
        return;
    }
    
    // проверяем что файл скачан и не пустой
    struct stat st;
    if (stat(repoPath.c_str(), &st) != 0 || st.st_size == 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Lua", "Downloaded file is empty or missing");
        {
            std::lock_guard<std::mutex> lock(repoMutex);
            repoStatus = "Download failed (empty file)";
        }
        return;
    }
    
    // копируем в основную папку
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
    
    // обновляем список локальных скриптов
    ScanLuaScripts();
    
    // находим и активируем скрипт
    for (int i = 0; i < luaScripts.size(); i++) {
        size_t pos = luaScripts[i].find_last_of('/');
        std::string name = (pos != std::string::npos) ? luaScripts[i].substr(pos + 1) : luaScripts[i];
        
        if (name == scriptName) {
            SetActiveScript(i);
            __android_log_print(ANDROID_LOG_INFO, "Lua", "Activated script: %s", scriptName.c_str());
            
            {
                std::lock_guard<std::mutex> lock(repoMutex);
                repoStatus = "Downloaded and activated: " + scriptName;
            }
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
        
        // позиция
        lua_pushstring(L, "x");
        lua_pushnumber(L, players[i].position.x);
        lua_settable(L, -3);
        
        lua_pushstring(L, "y");
        lua_pushnumber(L, players[i].position.y);
        lua_settable(L, -3);
        
        lua_pushstring(L, "z");
        lua_pushnumber(L, players[i].position.z);
        lua_settable(L, -3);
        
        // здоровье
        lua_pushstring(L, "health");
        lua_pushinteger(L, players[i].health);
        lua_settable(L, -3);
        
        lua_pushstring(L, "armor");
        lua_pushinteger(L, players[i].armor);
        lua_settable(L, -3);
        
        // имя
        lua_pushstring(L, "name");
        lua_pushstring(L, players[i].name.c_str());
        lua_settable(L, -3);
        
        // оружие
        lua_pushstring(L, "weapon");
        lua_pushstring(L, players[i].weapon.c_str());
        lua_settable(L, -3);
        
        // видимость
        lua_pushstring(L, "visible");
        lua_pushboolean(L, players[i].isVisible);
        lua_settable(L, -3);
        
        // адрес
        lua_pushstring(L, "address");
        lua_pushinteger(L, players[i].address);
        lua_settable(L, -3);
        
        // добавляем игрока в общую таблицу
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
    
    // проще передать как плоский массив
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

// --- Регистрация всех функций (без записи) ---
void RegisterLuaAPI(lua_State* L) {
    // базовые функции чтения
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
    
    // новые функции для ESP
    lua_register(L, "getPlayers", lua_getPlayers);
    lua_register(L, "worldToScreen", lua_worldToScreen);
    lua_register(L, "getViewMatrix", lua_getViewMatrix);
    lua_register(L, "getLocalPlayer", lua_getLocalPlayer);
    lua_register(L, "lerp", lua_lerp);
}

// --- Остальной код без изменений ---
bool InitLua() {
    L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);
    RegisterLuaAPI(L);
    
    // создаем папку для скриптов если нет
    system("mkdir -p /sdcard/chuvashi/scripts 2>/dev/null");
    system("mkdir -p /sdcard/chuvashi/repo 2>/dev/null");
    
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
    if (luaL_dofile(L, path) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        __android_log_print(ANDROID_LOG_ERROR, "Lua", "Load error: %s", err);
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
        if (name.length() > 4 && name.substr(name.length() - 4) == ".lua") {
            luaScripts.push_back("/sdcard/chuvashi/scripts/" + name);
        }
    }
    closedir(dir);
    
    // сортируем по имени
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
    if (idx >= 0 && idx < luaScripts.size()) {
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
            __android_log_print(ANDROID_LOG_ERROR, "Lua", "Update error: %s", err);
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}