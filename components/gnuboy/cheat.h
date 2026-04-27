#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct gb_cheat_s {
    struct gb_cheat_s *next;
    char *name;
    uint16_t addr;
    uint8_t val;
    int compare; // -1 for GS, 0-255 for GG
    bool status;
} gb_cheat_t;

void gb_cheat_init(void);
void gb_cheat_reset(void);
int  gb_cheat_add(const char *name, uint16_t addr, uint8_t val, int compare, bool status);
bool gb_cheat_del(uint32_t index);
bool gb_cheat_get(uint32_t index, char **name, uint16_t *addr, uint8_t *val, bool *status);
bool gb_cheat_set(uint32_t index, bool status);
void gb_cheat_apply(void);
uint8_t gb_cheat_check(uint16_t addr, uint8_t val);
bool gb_cheat_decode_gs(const char *code, uint16_t *addr, uint8_t *val);
bool gb_cheat_decode_gg(const char *code, uint16_t *addr, uint8_t *val, int *compare);
