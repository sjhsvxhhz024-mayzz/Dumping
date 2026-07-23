#include "memory.h"
#include "utils.h"
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <algorithm>

#ifndef SYS_process_vm_readv
#ifdef __aarch64__
#define SYS_process_vm_readv 270
#define SYS_process_vm_writev 271
#else
#define SYS_process_vm_readv 270
#define SYS_process_vm_writev 271
#endif
#endif

// глобальные переменные из main.cpp
extern int game_pid;
extern uint64_t il2cpp_base;
extern int abs_ScreenX, abs_ScreenY;

pid_t get_pid(const char *pkgname) {
    pid_t ret(0);
    for(;ret<99999;ret++) {
        char buffer[255]={0};
        sprintf(buffer,"/proc/%d/cmdline\0",ret);
        int fd = open(buffer,O_RDONLY);
        char pkg[255]={0};
        read(fd,pkg,255);
        close(fd);
        if(!strcmp(pkg,pkgname))break;
    }
    if(ret == 99999)return 0;
    return ret;
}

int open_proccess_memory(const pid_t pid) {
    char name[255]={0};
    sprintf(name,"/proc/%d/mem\0",pid);
    return open(name,O_RDWR);
}

bool pvm(void *address, void *buffer, size_t size, bool iswrite) {
    struct iovec local[1];
    struct iovec remote[1];
    local[0].iov_base = buffer;
    local[0].iov_len = size;
    remote[0].iov_base = address;
    remote[0].iov_len = size;
    if (game_pid < 0) {
        return false;
    }
    long syscallNumber = iswrite ? SYS_process_vm_writev : SYS_process_vm_readv;
    ssize_t bytes = syscall(syscallNumber, game_pid, local, 1, remote, 1, 0);
    return bytes == size;
}

bool vm_readv(unsigned long address, void *buffer, size_t size) {
    return pvm(reinterpret_cast < void *>(address), buffer, size, false);
}

bool vm_writev(unsigned long address, const void *buffer, size_t size) {
    return pvm(reinterpret_cast<void *>(address), const_cast<void *>(buffer), size, true);
}

bool mem_addr_virtophy(unsigned long vaddr) {
    static int pageSize = getpagesize();
    static char filename[32];
    static int fd = -1;
    if (fd < 0) {
        snprintf(filename, sizeof(filename), "/proc/%d/pagemap", game_pid);
        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            return false;
        }
    }
    unsigned long v_pageIndex = vaddr / pageSize;
    unsigned long pfn_item_offset = v_pageIndex * sizeof(uint64_t);
    uint64_t item;
    if (lseek(fd, pfn_item_offset, SEEK_SET) < 0) {
        return false;
    }
    if (read(fd, &item, sizeof(uint64_t)) != sizeof(uint64_t)) {
        return false;
    }
    if (!(item & (1ULL << 63))) {
        return false;
    }
    return true;
}

// реализации игровых функций
std::string String::Get() {
    int safeLength = std::max(0, std::min(length, (int)(sizeof(first_char) / sizeof(char16_t))));
    // упрощенная конвертация, можно использовать твою utf16le_to_utf8
    std::string result;
    for (int i = 0; i < safeLength; i++) {
        char16_t c = ((char16_t*)first_char)[i];
        if (c < 0x80) {
            result += (char)c;
        }
    }
    return result;
}

// новые функции
uint64_t getPlayerManager() {
    if (!il2cpp_base) return 0;
    return rpm<uint64_t>(rpm<uint64_t>(rpm<uint64_t>(rpm<uint64_t>(il2cpp_base + 135621384) + 0x100) + 0x130) + 0x0);
}

std::vector<PlayerInfo> getPlayers() {
    std::vector<PlayerInfo> players;
    
    uint64_t playermanager = getPlayerManager();
    if (!playermanager) return players;
    
    uint64_t playersList = rpm<uint64_t>(playermanager + 0x28);
    uint64_t localPlayer = rpm<uint64_t>(playermanager + 0x70);
    
    if (!playersList || !localPlayer) return players;
    
    int playersListSize = rpm<int>(playersList + 0x20);
    for (int i = 0; i < playersListSize; i++) {
        uint64_t player = rpm<uint64_t>(rpm<uint64_t>(playersList + 0x18) + 0x30 + 0x18 * i);
        if (!player) continue;
        
        PlayerInfo info;
        info.address = player;
        info.health = get_health(player);
        if (info.health <= 0) continue;
        
        info.position = getPlayerPosition(player);
        info.armor = get_armor(player);
        info.name = get_name(player);
        info.weapon = get_weapon_name(player);
        
        // проверка видимости (упрощенно)
        info.isVisible = true;
        
        players.push_back(info);
    }
    
    return players;
}

Vector3 getPlayerPosition(uint64_t player) {
    uint64_t view = rpm<uint64_t>(player + 0x98);
    if (!view) return Vector3::Zero();
    return rpm<Vector3>(rpm<uint64_t>(view + 0xB0) + 0x44);
}

Matrix getViewMatrix() {
    uint64_t localPlayer = getLocalPlayer();
    if (!localPlayer) return {};
    
    return rpm<Matrix>(rpm<uint64_t>(rpm<uint64_t>(rpm<uint64_t>(localPlayer + 0xE0) + 0x20) + 0x10) + 0x100);
}

uint64_t getLocalPlayer() {
    uint64_t playermanager = getPlayerManager();
    if (!playermanager) return 0;
    return rpm<uint64_t>(playermanager + 0x70);
}
