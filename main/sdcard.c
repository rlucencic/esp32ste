/*
 * microSD card on the SDMMC peripheral, 1-bit bus (pins from board.h, as in
 * wmbus_hub), FAT with long file names.
 */
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "board.h"
#include "sdcard.h"
#include "st.h"

static const char *TAG = "sdcard";

bool sdcard_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = BOARD_SD_GPIO_CLK;
    slot.cmd = BOARD_SD_GPIO_CMD;
    slot.d0 = BOARD_SD_GPIO_D0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 16,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SD card (%s)", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "%s: %llu MB", card->cid.name,
             (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    return true;
}

/* For the ST's Dfree() on drive C: */
bool st_host_disk_free(const char *path, uint64_t *total, uint64_t *free_bytes)
{
    return esp_vfs_fat_info(SDCARD_MOUNT_POINT, total, free_bytes) == ESP_OK;
}
