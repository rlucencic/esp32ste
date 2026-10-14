#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"

/* A TOS image memory-mapped from its flash partition (big-endian, read-only) */
typedef struct {
    const uint8_t *data;
    size_t size;
    uint16_t version;       /* e.g. 0x0162 */
    uint32_t base;          /* ROM base address in the ST memory map */
    esp_partition_mmap_handle_t handle;
} tos_rom_t;

/* Map partition `label` (tos162, tos206) and check its TOS header */
esp_err_t tos_rom_map(const char *label, tos_rom_t *rom);

/* Release the mapping (st_init() keeps its own copy of the ROM) */
void tos_rom_unmap(tos_rom_t *rom);
