#include "cheat.h"
#include "hw.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static gb_cheat_t *cheats = NULL;

void gb_cheat_init(void) {
    cheats = NULL;
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
}

int gb_cheat_add(const char *name, uint16_t addr, uint8_t val, bool status) {
    gb_cheat_t *temp = (gb_cheat_t *)malloc(sizeof(gb_cheat_t));
    if (!temp) return -1;

    temp->name = strdup(name ? name : "Cheat");
    temp->addr = addr;
    temp->val = val;
    temp->status = status;
    temp->next = NULL;

    if (!cheats) {
        cheats = temp;
    } else {
        gb_cheat_t *last = cheats;
        while (last->next) last = last->next;
        last->next = temp;
    }

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
        if (cur->status) {
            // GameShark: Directly write to RAM
            gb_hw_write(cur->addr, cur->val);
        }
        cur = cur->next;
    }
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
