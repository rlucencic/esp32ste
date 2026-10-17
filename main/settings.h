#pragma once

#include <stdint.h>
#include "sdkconfig.h"

/* A faster 68000 only on the Tab5: the MaTouch has no time to spare at
 * 8 MHz (16 MHz ran at ~45% of real time, Fastest never left 8 MHz), so
 * its build stays at 8 MHz, without the CPU choice
 */
#if CONFIG_IDF_TARGET_ESP32P4
#define SETTINGS_CPU_CHOICE 1
#else
#define SETTINGS_CPU_CHOICE 0
#endif

/* Machine settings kept in NVS (defaults from menuconfig) */
typedef struct {
    uint8_t tos;            /* 0 = TOS 1.62, 1 = TOS 2.06 */
    uint16_t ram_kb;        /* 512, 1024, 4096 */
    int8_t resolution;      /* last desktop resolution: 0 low, 1 medium, 2 high, -1 none */
    uint8_t cpu;            /* 68000 speed: SETTINGS_CPU_8MHZ, _16MHZ, _FASTEST */
} settings_t;

enum { SETTINGS_CPU_8MHZ, SETTINGS_CPU_16MHZ, SETTINGS_CPU_FASTEST };

extern settings_t settings;

void settings_load(void);
void settings_save(void);

/* Partition name of the chosen TOS */
const char *settings_tos_partition(void);
