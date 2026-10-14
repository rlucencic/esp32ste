/*
 * RGB panel bring-up, after the Makerfabs MaTouch 7" driver in wmbus_hub.
 * One framebuffer in PSRAM, streamed to the panel through bounce buffers,
 * so the CPU can write it directly.
 */
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include "lcd.h"

static const char *TAG = "lcd";

static void reset_panel_and_touch(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_LCD_GPIO_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_LCD_GPIO_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_LCD_GPIO_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_GPIO_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
}

static void backlight_set(bool on)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_LCD_GPIO_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_LCD_GPIO_BL, on ? 0 : 1);
}

uint16_t *lcd_init(void)
{
    backlight_set(false);
    reset_panel_and_touch();

    const esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = BOARD_LCD_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = BOARD_LCD_HSYNC_PW,
            .hsync_back_porch = BOARD_LCD_HSYNC_BP,
            .hsync_front_porch = BOARD_LCD_HSYNC_FP,
            .vsync_pulse_width = BOARD_LCD_VSYNC_PW,
            .vsync_back_porch = BOARD_LCD_VSYNC_BP,
            .vsync_front_porch = BOARD_LCD_VSYNC_FP,
            .flags = {
                .pclk_active_neg = BOARD_LCD_PCLK_ACTIVE_NEG,
                .pclk_idle_high = true,
            },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * 10,
        .dma_burst_size = 64,
        .hsync_gpio_num = BOARD_LCD_GPIO_HSYNC,
        .vsync_gpio_num = BOARD_LCD_GPIO_VSYNC,
        .de_gpio_num = BOARD_LCD_GPIO_DE,
        .pclk_gpio_num = BOARD_LCD_GPIO_PCLK,
        .disp_gpio_num = BOARD_LCD_GPIO_DISP,
        .data_gpio_nums = {
            BOARD_LCD_GPIO_B0, BOARD_LCD_GPIO_B1, BOARD_LCD_GPIO_B2,
            BOARD_LCD_GPIO_B3, BOARD_LCD_GPIO_B4,
            BOARD_LCD_GPIO_G0, BOARD_LCD_GPIO_G1, BOARD_LCD_GPIO_G2,
            BOARD_LCD_GPIO_G3, BOARD_LCD_GPIO_G4, BOARD_LCD_GPIO_G5,
            BOARD_LCD_GPIO_R0, BOARD_LCD_GPIO_R1, BOARD_LCD_GPIO_R2,
            BOARD_LCD_GPIO_R3, BOARD_LCD_GPIO_R4,
        },
        .flags = {
            .fb_in_psram = true,
        },
    };

    esp_lcd_panel_handle_t panel;
    esp_err_t err = esp_lcd_new_rgb_panel(&panel_config, &panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel);
    }
    void *fb = NULL;
    if (err == ESP_OK) {
        err = esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB panel init failed: %s", esp_err_to_name(err));
        return NULL;
    }

    backlight_set(true);
    ESP_LOGI(TAG, "RGB panel %dx%d ready", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return fb;
}
