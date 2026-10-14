#pragma once

/*
 * Line commands on the USB-Serial-JTAG console (tools/st_console.py):
 *
 *   s                  dump the ST screen (RGB565, base64)
 *   L                  dump the whole LCD (1024x600)
 *   k <hex scancode>   tap a key
 *   m <dx> <dy> <btn>  move the mouse, btn bit 0 left, bit 1 right
 *   r                  cold reset
 *   R <tos> <ram KB>   restart with TOS 0 = 1.62, 1 = 2.06 and that much RAM
 *   F <drive> [name]   insert floppies/<name> into drive 0 (A:) or 1 (B:),
 *                      eject without a name, "+" for a new blank disk
 *   t <x> <y> [ms]     touch the screen there (default 150 ms)
 *   i                  print machine state
 */
#include <stdint.h>

void console_start(uint16_t *lcd_fb);

/* Provided by main.c: reset the ST from the emulation task */
void emulator_request_reset(void);

/* Provided by main.c: restart the ST with another TOS / RAM size (saved) */
void emulator_restart(int tos, int ram_kb);

/* Provided by main.c: floppy insert / eject / new disk (see 'F') */
void emulator_floppy(int drive, const char *name);
