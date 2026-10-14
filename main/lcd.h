#pragma once

#include <stdint.h>

/* Bring up the 1024x600 RGB panel and backlight. Returns the RGB565
 * framebuffer (PSRAM, BOARD_LCD_H_RES pixels per row) or NULL on failure.
 * Pixels written there show up on the next panel refresh.
 */
uint16_t *lcd_init(void);
