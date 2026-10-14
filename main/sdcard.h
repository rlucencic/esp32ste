#pragma once

#include <stdbool.h>

#define SDCARD_MOUNT_POINT  "/sdcard"

/* Mount the microSD card (SDMMC, 1-bit) at SDCARD_MOUNT_POINT */
bool sdcard_mount(void);
