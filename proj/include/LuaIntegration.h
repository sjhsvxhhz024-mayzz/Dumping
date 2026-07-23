#ifndef LUA_INTEGRATION_H
#define LUA_INTEGRATION_H

#include "lua/lua-5.4.7/src/lua.h"
#include "lua/lua-5.4.7/src/lauxlib.h"
#include "lua/lua-5.4.7/src/lualib.h"
#include <vector>
#include <string>

extern lua_State* L;

bool InitLua();
void CloseLua();
bool LoadLuaScript(const char* path);
void ExecuteLuaScripts();
void RegisterLuaAPI(lua_State* L);

// для меню
void ScanLuaScripts();
std::vector<std::string> GetLuaScriptList();
void SetActiveScript(int idx);
std::string GetActiveScriptName();

// новые функции для репозитория
void FetchRepoScripts(); // загружает список скриптов из репозитория
void DownloadRepoScript(int idx); // скачивает выбранный скрипт
std::vector<std::string> GetRepoScriptList(); // возвращает список скриптов из репы
std::string GetRepoStatus(); // возвращает статус загрузки
bool IsFetchingRepo(); // проверяет, идет ли загрузка

#endif