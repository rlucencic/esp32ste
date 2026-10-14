#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What main.c knows about the running machine, for status displays.
 * Updated about once a second; safe to read from any task.
 */
typedef enum {
    EMU_SETUP,              /* setup screen before the first start */
    EMU_RUNNING,
    EMU_PAUSED,             /* setup screen while the ST waits */
    EMU_HALTED,             /* the 68000 stopped (double bus fault) until a reset */
} emu_state_t;

typedef struct {
    emu_state_t state;
    uint16_t tos_version;   /* of the running TOS, e.g. 0x0206; 0 before the start */
    uint16_t ram_kb;
    float fps;              /* frames emulated per second, last second */
    int speed;              /* % of real speed */
    float load[2];          /* % busy, cores 0 and 1, last second */
    float temperature;      /* chip, degrees C, as last measured; NAN if never */
    uint32_t session_s;     /* seconds since the ST was (re)started */
    bool hard_disk;         /* SD card as C: */
} emu_info_t;

void emulator_info(emu_info_t *info);

/* Have the chip temperature measured (at most every 5 s; the value shows up
 * in emulator_info() a moment later)
 */
void emulator_measure_temperature(void);
