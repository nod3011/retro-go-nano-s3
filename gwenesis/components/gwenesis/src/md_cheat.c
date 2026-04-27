#include "md_cheat.h"
#include "m68k.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static md_cheat_t *cheats = NULL;

void md_cheat_init(void) {
    md_cheat_reset();
}

void md_cheat_reset(void) {
    md_cheat_t *cur = cheats;
    while (cur) {
        md_cheat_t *next = cur->next;
        if (cur->name) free(cur->name);
        free(cur);
        cur = next;
    }
    cheats = NULL;
}

int md_cheat_add(const char *name, uint32_t addr, uint16_t val, bool status) {
    md_cheat_t *temp = (md_cheat_t *)malloc(sizeof(md_cheat_t));
    if (!temp) return -1;

    temp->name = strdup(name ? name : "Cheat");
    temp->addr = addr;
    temp->val = val;
    temp->status = status;
    temp->next = NULL;

    if (!cheats) {
        cheats = temp;
    } else {
        md_cheat_t *last = cheats;
        while (last->next) last = last->next;
        last->next = temp;
    }

    int index = 0;
    md_cheat_t *cur = cheats;
    while (cur != temp) {
        index++;
        cur = cur->next;
    }
    return index;
}

bool md_cheat_del(uint32_t index) {
    md_cheat_t *prev = NULL;
    md_cheat_t *cur = cheats;
    uint32_t i = 0;

    while (cur) {
        if (i == index) {
            if (prev) prev->next = cur->next;
            else cheats = cur->next;

            if (cur->name) free(cur->name);
            free(cur);
            return true;
        }
        prev = cur;
        cur = cur->next;
        i++;
    }
    return false;
}

bool md_cheat_get(uint32_t index, char **name, uint32_t *addr, uint16_t *val, bool *status) {
    md_cheat_t *cur = cheats;
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

bool md_cheat_set(uint32_t index, bool status) {
    md_cheat_t *cur = cheats;
    uint32_t i = 0;

    while (cur) {
        if (i == index) {
            cur->status = status;
            return true;
        }
        cur = cur->next;
        i++;
    }
    return false;
}

extern unsigned char *ROM_DATA;

void md_cheat_apply(void) {
    md_cheat_t *cur = cheats;
    while (cur) {
        if (cur->status) {
            uint32_t addr = cur->addr;
            uint16_t val = cur->val;

            if (addr < 0x400000) { // ROM patching
                if (ROM_DATA) {
                    // Gwenesis uses byte-swapping for ROM: byte at addr A is at ROM_DATA[A ^ 1]
                    ROM_DATA[addr ^ 1] = (val >> 8);
                    ROM_DATA[(addr + 1) ^ 1] = (val & 0xFF);
                }
            } else if ((addr & 0xFF0000) == 0xFF0000) { // RAM patching
                m68k_write_memory_16(addr, val);
            }
        }
        cur = cur->next;
    }
}

bool md_cheat_decode_par(const char *code, uint32_t *addr, uint16_t *val) {
    if (!code) return false;
    
    char clean[16] = {0};
    int j = 0;
    for (int i = 0; code[i] && j < 15; i++) {
        if (isxdigit((unsigned char)code[i])) {
            clean[j++] = code[i];
        }
    }
    
    if (j == 8) { // XXXXXXYY -> 24-bit addr, 8-bit val
        unsigned int a, v;
        if (sscanf(clean, "%06x%02x", &a, &v) == 2) {
            *addr = a;
            *val = (uint16_t)v;
            return true;
        }
    } else if (j == 10) { // XXXXXXYYYY -> 24-bit addr, 16-bit val
        unsigned int a, v;
        if (sscanf(clean, "%06x%04x", &a, &v) == 2) {
            *addr = a;
            *val = (uint16_t)v;
            return true;
        }
    }
    
    return false;
}

