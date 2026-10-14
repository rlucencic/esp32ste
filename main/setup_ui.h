#pragma once

#include <stdint.h>

#include <stdbool.h>

/* Setup screen: WiFi, internet time, TOS/RAM, Bluetooth devices, floppy
 * disks. Draws into the LCD framebuffer with LVGL and returns once the user
 * starts (or with `running`, returns to) the Atari; LVGL is shut down again.
 * With `running`, true if the user asked for a reset of the ST.
 */
bool setup_ui_run(uint16_t *fb, const char *drive_info, bool running);
