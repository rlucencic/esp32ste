#pragma once

#include <stdint.h>

/* Machine settings kept in NVS (defaults from menuconfig) */
typedef struct {
    uint8_t tos;            /* 0 = TOS 1.62, 1 = TOS 2.06 */
    uint16_t ram_kb;        /* 512, 1024, 4096 */
    int8_t resolution;      /* last desktop resolution: 0 low, 1 medium, 2 high, -1 none */
} settings_t;

extern settings_t settings;

void settings_load(void);
void settings_save(void);

/* Partition name of the chosen TOS */
const char *settings_tos_partition(void);
