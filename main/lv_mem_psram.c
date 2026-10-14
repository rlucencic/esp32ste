/*
 * LVGL's heap in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC): the setup screen's
 * objects stay out of the internal RAM that WiFi and Bluetooth need, however
 * long the lists get.
 */
#include <string.h>
#include "esp_heap_caps.h"
#include "lvgl.h"

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    memset(mon_p, 0, sizeof(*mon_p));
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
