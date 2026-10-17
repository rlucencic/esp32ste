#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define NAMESPACE   "atari"

static const char *TAG = "settings";

settings_t settings;

void settings_load(void)
{
#if CONFIG_ST_TOS_206
    settings.tos = 1;
#else
    settings.tos = 0;
#endif
    settings.ram_kb = CONFIG_ST_RAM_KB;
    settings.resolution = -1;
    settings.cpu = SETTINGS_CPU_8MHZ;

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    uint16_t u16;
    int8_t i8;
    if (nvs_get_u8(h, "tos", &u8) == ESP_OK && u8 <= 1) {
        settings.tos = u8;
    }
    if (nvs_get_u16(h, "ram_kb", &u16) == ESP_OK &&
        (u16 == 512 || u16 == 1024 || u16 == 2048 || u16 == 4096)) {
        settings.ram_kb = u16;
    }
    if (nvs_get_i8(h, "rez", &i8) == ESP_OK && i8 >= -1 && i8 <= 2) {
        settings.resolution = i8;
    }
    if (SETTINGS_CPU_CHOICE && nvs_get_u8(h, "cpu", &u8) == ESP_OK && u8 <= SETTINGS_CPU_FASTEST) {
        settings.cpu = u8;
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(h, "tos", settings.tos);
    nvs_set_u16(h, "ram_kb", settings.ram_kb);
    nvs_set_i8(h, "rez", settings.resolution);
    nvs_set_u8(h, "cpu", settings.cpu);
    nvs_commit(h);
    nvs_close(h);
}

const char *settings_tos_partition(void)
{
    return settings.tos ? "tos206" : "tos162";
}
