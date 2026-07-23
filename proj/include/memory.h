#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "Vector3.h"
#include "utils.h"

struct String {
    uint64_t klass;
    uint64_t monitor_data;
    int length;
    char first_char[256];
    
    std::string Get();
};

struct PlayerInfo {
    uint64_t address;
    Vector3 position;
    int health;
    int armor;
    std::string name;
    std::string weapon;
    bool isVisible;
};

extern int game_pid;
extern uint64_t il2cpp_base;
extern int abs_ScreenX, abs_ScreenY;

// вспомогательные функции
pid_t get_pid(const char *pkgname);
int open_proccess_memory(const pid_t pid);
bool pvm(void *address, void *buffer, size_t size, bool iswrite);
bool vm_readv(unsigned long address, void *buffer, size_t size);
bool vm_writev(unsigned long address, const void *buffer, size_t size);
bool mem_addr_virtophy(unsigned long vaddr);

// ТОЛЬКО ЧТЕНИЕ
template<typename T>
T rpm(uint64_t addr) {
    T data{};
    if (addr < 0x1000) {
        return data;
    }
    vm_readv(addr, &data, sizeof(T));
    return data;
}

template<typename T>
bool wpm(uint64_t addr, const T& value) {
    if (addr < 0x1000) {
        return false;
    }
    return vm_writev(addr, &value, sizeof(T));
}

inline uint64_t get_photon(uint64_t player) {
    return rpm<uint64_t>(player + 0x158);
}

inline std::string get_name(uint64_t player) {
    auto photon = get_photon(player);
    String name = rpm<String>(rpm<uint64_t>(photon + 0x20));
    return name.Get();
}

inline std::string get_weapon_name(uint64_t player) {
    uint64_t weapry = rpm<uint64_t>(player + 0x88);
    if(weapry) {
        uint64_t weap = rpm<uint64_t>(weapry + 0x98);
        if(weap) {
            uint64_t weapp = rpm<uint64_t>(weap + 0xA0);
            if(weapp) {
                String name = rpm<String>(rpm<uint64_t>(weapp + 0x20));
                return name.Get();
            }
        }
    }
    return "";
}

inline int get_health(uint64_t player) {
    uint64_t photon = get_photon(player);
    uint64_t props = rpm<uint64_t>(photon + 0x38);
    if(props) {
        int size = rpm<int>(props + 0x20);
        for(int i = 0; i < size; i++) {
            uint64_t propkey = rpm<uint64_t>(rpm<uint64_t>(props+0x18) + 0x28 + 0x18*i);
            uint64_t propval = rpm<uint64_t>(rpm<uint64_t>(props+0x18) + 0x30 + 0x18*i);
            if(propkey != 0) {
                std::string keyVal = rpm<String>(propkey).Get();
                if(keyVal.find("health") != std::string::npos) {
                    return rpm<int>(propval + 0x10);
                }
            }
        }
    }
    return 0;
}

inline int get_armor(uint64_t player) {
    uint64_t photon = get_photon(player);
    uint64_t props = rpm<uint64_t>(photon + 0x38);
    if(props) {
        int size = rpm<int>(props + 0x20);
        for(int i = 0; i < size; i++) {
            uint64_t propkey = rpm<uint64_t>(rpm<uint64_t>(props+0x18) + 0x28 + 0x18*i);
            uint64_t propval = rpm<uint64_t>(rpm<uint64_t>(props+0x18) + 0x30 + 0x18*i);
            if(propkey != 0) {
                std::string keyVal = rpm<String>(propkey).Get();
                if(keyVal.find("armor") != std::string::npos) {
                    return rpm<int>(propval + 0x10);
                }
            }
        }
    }
    return 0;
}

// новые функции для работы с игроками
std::vector<PlayerInfo> getPlayers();
Vector3 getPlayerPosition(uint64_t player);
Matrix getViewMatrix();
uint64_t getLocalPlayer();
uint64_t getPlayerManager();

#endif
