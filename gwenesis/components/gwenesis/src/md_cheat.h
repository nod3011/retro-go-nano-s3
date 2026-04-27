#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct md_cheat_s {
    struct md_cheat_s *next;
    char *name;
    uint32_t addr;
    uint16_t val;
    bool status;
} md_cheat_t;

void md_cheat_init(void);
void md_cheat_reset(void);
int  md_cheat_add(const char *name, uint32_t addr, uint16_t val, bool status);
bool md_cheat_del(uint32_t index);
bool md_cheat_get(uint32_t index, char **name, uint32_t *addr, uint16_t *val, bool *status);
bool md_cheat_set(uint32_t index, bool status);
void md_cheat_apply(void);
bool md_cheat_decode_par(const char *code, uint32_t *addr, uint16_t *val);


