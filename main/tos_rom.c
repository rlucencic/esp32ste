#include "esp_log.h"
#include "esp_partition.h"
#include "tos_rom.h"

#define TOS_SIZE            (256 * 1024)
#define TOS_PARTITION_TYPE  0x40

static const char *TAG = "tos_rom";

/* Header country codes (word at +0x1C, bits 15..1) */
static const char *const countries[] = {
    "US", "DE", "FR", "UK", "ES", "IT", "SE", "CH (FR)", "CH (DE)",
    "TR", "FI", "NO", "DK", "SA", "NL", "CZ", "HU",
};

static uint16_t rd16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)rd16(p) << 16) | rd16(p + 2);
}

esp_err_t tos_rom_map(const char *label, tos_rom_t *rom)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           TOS_PARTITION_TYPE, label);
    if (part == NULL) {
        ESP_LOGE(TAG, "%s: partition not found", label);
        return ESP_ERR_NOT_FOUND;
    }

    const void *ptr;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, TOS_SIZE, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: mmap failed: %s", label, esp_err_to_name(err));
        return err;
    }

    const uint8_t *hdr = ptr;
    uint16_t version = rd16(hdr + 0x02);
    uint32_t base = rd32(hdr + 0x08);
    if (rd16(hdr) != 0x602E || (base != 0xE00000 && base != 0xFC0000)) {
        ESP_LOGE(TAG, "%s: no TOS image (header %04x, base %06lx); flash with idf.py flash",
                 label, rd16(hdr), (unsigned long)base);
        esp_partition_munmap(handle);
        return ESP_ERR_INVALID_VERSION;
    }

    uint16_t country = rd16(hdr + 0x1C);
    unsigned int country_id = country >> 1;
    ESP_LOGI(TAG, "%s: TOS %x.%02x, %02x.%02x.%04x, %s, %s, base $%06lx", label,
             version >> 8, version & 0xFF,
             hdr[0x19], hdr[0x18], rd16(hdr + 0x1A),
             country_id < sizeof(countries) / sizeof(countries[0]) ? countries[country_id] : "??",
             (country & 1) ? "PAL" : "NTSC", (unsigned long)base);

    rom->data = ptr;
    rom->size = TOS_SIZE;
    rom->version = version;
    rom->base = base;
    rom->handle = handle;
    return ESP_OK;
}

void tos_rom_unmap(tos_rom_t *rom)
{
    if (rom->data != NULL) {
        esp_partition_munmap(rom->handle);
        rom->data = NULL;
    }
}
