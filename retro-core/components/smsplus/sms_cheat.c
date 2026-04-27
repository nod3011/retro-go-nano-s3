#include "sms_cheat.h"
#include "shared.h"
#include <ctype.h>
#include <stdlib.h>

#define MAX_CHEATS 64

typedef struct {
    char name[64];
    uint32_t addr;
    uint8_t val;
    bool status;
} sms_cheat_t;

static sms_cheat_t cheats[MAX_CHEATS];
static int cheat_count = 0;

void sms_cheat_init(void) {
    memset(cheats, 0, sizeof(cheats));
    cheat_count = 0;
}

void sms_cheat_reset(void) {
    cheat_count = 0;
}

void sms_cheat_add(const char *name, uint32_t addr, uint8_t val, bool status) {
    if (cheat_count >= MAX_CHEATS) return;
    
    strncpy(cheats[cheat_count].name, name, 63);
    cheats[cheat_count].addr = addr;
    cheats[cheat_count].val = val;
    cheats[cheat_count].status = status;
    cheat_count++;
}

void sms_cheat_del(uint32_t index) {
    if (index >= cheat_count) return;
    for (int i = index; i < cheat_count - 1; i++) {
        cheats[i] = cheats[i+1];
    }
    cheat_count--;
}

void sms_cheat_set(uint32_t index, bool status) {
    if (index < cheat_count) {
        cheats[index].status = status;
    }
}

bool sms_cheat_get(uint32_t index, char **name, uint32_t *addr, uint8_t *val, bool *status) {
    if (index >= cheat_count) return false;
    if (name) *name = cheats[index].name;
    if (addr) *addr = cheats[index].addr;
    if (val) *val = cheats[index].val;
    if (status) *status = cheats[index].status;
    return true;
}

void sms_cheat_apply(void) {
    for (int i = 0; i < cheat_count; i++) {
        if (!cheats[i].status) continue;

        uint32_t addr = cheats[i].addr & 0xFFFF;
        uint8_t val = cheats[i].val;

        // 1. Direct RAM patching (SMS RAM is 8KB mirrored at 0xC000-0xFFFF)
        if (addr >= 0xC000) {
            sms.wram[addr & 0x1FFF] = val;
        }
        
        // 2. Aggressive Page Patching (in case it's mapped ROM or specialized RAM)
        // SMSPlus uses 1KB pages (64 pages for 64KB)
        uint8_t page = addr >> 10;
        uint8_t offset = addr & 0x03FF;
        if (cpu_readmap[page] && cpu_readmap[page] != (unsigned char *)dummy_memory) {
            cpu_readmap[page][offset] = val;
        }
        if (cpu_writemap[page] && cpu_writemap[page] != (unsigned char *)dummy_memory) {
            cpu_writemap[page][offset] = val;
        }
    }
}

bool sms_cheat_decode_par(const char *code, uint32_t *addr, uint8_t *val) {
    if (!code || !addr || !val) return false;

    char clean[16];
    int j = 0;
    for (int i = 0; code[i] && j < 15; i++) {
        if (isxdigit((int)code[i])) clean[j++] = code[i];
    }
    clean[j] = 0;

    if (j == 6) { // XXXXXX -> XXXX:YY
        uint32_t raw = strtoul(clean, NULL, 16);
        *addr = (raw >> 8) & 0xFFFF;
        *val = raw & 0xFF;
        return true;
    } else if (j == 8) { // XXXXXXXX -> ???
        // Some PAR codes are 8 digits, but usually 6 for 8-bit systems.
        // We'll treat it as 24-bit addr + 8-bit val if 8 digits.
        uint32_t raw = strtoul(clean, NULL, 16);
        *addr = (raw >> 8) & 0xFFFFFF;
        *val = raw & 0xFF;
        return true;
    }

    return false;
}
