#pragma once

#include <stdbool.h>

/* GT911 touch panel. Call after lcd_init() (it pulses the shared reset). */
bool touch_init(void);

/* First touch point in panel pixels (1024x600); false if no touch panel */
bool touch_read(int *x, int *y, bool *pressed);

/* Pretend a finger is at (x, y) for `ms` (console 't': remote testing) */
void touch_inject(int x, int y, int ms);
