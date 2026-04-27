#include "cheat.h"
#include "hw.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static gb_cheat_t *cheats = NULL;

static void refresh_cheat_mask(void) {
    GB.cheat_mask = 0;
    gb_cheat_t *cur = cheats;
    while (cur) {
        if (cur->status && cur->compare != -1) {
            GB.cheat_mask |= (1 << (cur->addr >> 12));
        }
        cur = cur->next;
    }
}

void gb_cheat_init(void) {
    cheats = NULL;
    GB.cheat_mask = 0;
}

void gb_cheat_reset(void) {
    gb_cheat_t *cur = cheats;
    while (cur) {
        gb_cheat_t *next = cur->next;
        if (cur->name) free(cur->name);
        free(cur);
        cur = next;
    }
    cheats = NULL;
    refresh_cheat_mask();
}

int gb_cheat_add(const char *name, uint16_t addr, uint8_t val, int compare, bool status) {
    gb_cheat_t *temp = (gb_cheat_t *)malloc(sizeof(gb_cheat_t));
    if (!temp) return -1;

    temp->name = strdup(name ? name : "Cheat");
    temp->addr = addr;
    temp->val = val;
    temp->compare = compare;
    temp->status = status;
    temp->next = NULL;

    if (!cheats) {
        cheats = temp;
    } else {
        gb_cheat_t *last = cheats;
        while (last->next) last = last->next;
        last->next = temp;
    }

    refresh_cheat_mask();

    int index = 0;
    gb_cheat_t *cur = cheats;
    while (cur != temp) {
        index++;
        cur = cur->next;
    }
    return index;
}

bool gb_cheat_del(uint32_t index) {
    gb_cheat_t *prev = NULL;
    gb_cheat_t *cur = cheats;
    uint32_t i = 0;

    while (cur) {
        if (i == index) {
            if (prev) prev->next = cur->next;
            else cheats = cur->next;

            if (cur->name) free(cur->name);
            free(cur);
            refresh_cheat_mask();
            return true;
        }
        prev = cur;
        cur = cur->next;
        i++;
    }
    return false;
}

bool gb_cheat_get(uint32_t index, char **name, uint16_t *addr, uint8_t *val, bool *status) {
    gb_cheat_t *cur = cheats;
    uint32_t i = 0;

    while (cur) {
        if (i == index) {
            if (name) *name = cur->name;
            if (addr) *addr = cur->addr;
            if (val) *val = cur->val;
            if (status) *status = cur->status;
            return true;
        }
        cur = cur->next;
        i++;
    }
    return false;
}

bool gb_cheat_set(uint32_t index, bool status) {
    gb_cheat_t *cur = cheats;
    uint32_t i = 0;

    while (cur) {
        if (i == index) {
            cur->status = status;
            refresh_cheat_mask();
            return true;
        }
        cur = cur->next;
        i++;
    }
    return false;
}

void gb_cheat_apply(void) {
    gb_cheat_t *cur = cheats;
    while (cur) {
        if (cur->status && cur->compare == -1) {
            // GameShark: Directly write to RAM
            gb_hw_write(cur->addr, cur->val);
        }
        cur = cur->next;
    }
}

uint8_t gb_cheat_check(uint16_t addr, uint8_t val) {
    gb_cheat_t *cur = cheats;
    while (cur) {
        if (cur->status && cur->compare != -1 && cur->addr == addr) {
            if (cur->compare == val) return cur->val;
        }
        cur = cur->next;
    }
    return val;
}

bool gb_cheat_decode_gs(const char *code, uint16_t *addr, uint8_t *val) {
    if (!code || strlen(code) != 8) return false;

    for (int i = 0; i < 8; i++) {
        if (!isxdigit((unsigned char)code[i])) return false;
    }

    unsigned int type, v, a_low, a_high;
    if (sscanf(code, "%02x%02x%02x%02x", &type, &v, &a_low, &a_high) != 4) {
        return false;
    }

    *val = (uint8_t)v;
    *addr = (uint16_t)((a_high << 8) | a_low);

    return true;
}

static int hex2int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

bool gb_cheat_decode_gg(const char *code, uint16_t *addr, uint8_t *val, int *compare) {
    char clean[16];
    int len = 0;
    for (int i = 0; code[i]; i++) {
        if (isxdigit((int)code[i])) clean[len++] = code[i];
    }
    
    if (len != 6 && len != 9) return false;

    int n[9] = {0};
    for (int i = 0; i < len; i++) {
        n[i] = hex2int(clean[i]);
    }

    // Standard Game Boy Game Genie Decoding (ABC-DEF-GHI)
    // Value = AB
    *val = (n[0] << 4) | n[1];
    // Address = (F^0xF)CDE (Digit 6, 3, 4, 5)
    *addr = ((n[5] ^ 0x0F) << 12) | (n[2] << 8) | (n[3] << 4) | n[4];

    if (len == 9) {
        // Compare = GI (Digit 7, 9) -> Rotate Right 2 -> XOR 0xBA
        int comp = (n[6] << 4) | n[8];
        comp = ((comp >> 2) | (comp << 6)) & 0xFF;
        comp ^= 0xBA;
        *compare = comp;
    } else {
        *compare = -1; // No compare for 6-digit codes
    }

    return true;
}
