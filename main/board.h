/*
 * Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7" — pin map
 * Module: ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM)
 *
 * Source: Makerfabs examples (github.com/Makerfabs/ESP32-S3-Parallel-TFT-with-Touch-7inch)
 */
#pragma once

#include "driver/gpio.h"

/* ---- LCD: 1024x600 IPS, RGB565 parallel ---- */
#define BOARD_LCD_H_RES         1024
#define BOARD_LCD_V_RES         600
#define BOARD_LCD_PCLK_HZ       (16 * 1000 * 1000)

#define BOARD_LCD_HSYNC_POL     1
#define BOARD_LCD_HSYNC_FP      40
#define BOARD_LCD_HSYNC_PW      48
#define BOARD_LCD_HSYNC_BP      128
#define BOARD_LCD_VSYNC_POL     1
#define BOARD_LCD_VSYNC_FP      13
#define BOARD_LCD_VSYNC_PW      3
#define BOARD_LCD_VSYNC_BP      45
#define BOARD_LCD_PCLK_ACTIVE_NEG 1

#define BOARD_LCD_GPIO_DE       GPIO_NUM_40
#define BOARD_LCD_GPIO_VSYNC    GPIO_NUM_41
#define BOARD_LCD_GPIO_HSYNC    GPIO_NUM_39
#define BOARD_LCD_GPIO_PCLK     GPIO_NUM_42
#define BOARD_LCD_GPIO_DISP     GPIO_NUM_NC
#define BOARD_LCD_GPIO_BL       GPIO_NUM_10     /* active low: 0 = backlight on */
#define BOARD_LCD_GPIO_RST      GPIO_NUM_38     /* shared panel + GT911 reset */

/* Data lines in esp_lcd order: D0..D4 = B0..B4, D5..D10 = G0..G5, D11..D15 = R0..R4 */
#define BOARD_LCD_GPIO_B0       GPIO_NUM_8
#define BOARD_LCD_GPIO_B1       GPIO_NUM_3
#define BOARD_LCD_GPIO_B2       GPIO_NUM_46
#define BOARD_LCD_GPIO_B3       GPIO_NUM_9
#define BOARD_LCD_GPIO_B4       GPIO_NUM_1
#define BOARD_LCD_GPIO_G0       GPIO_NUM_5
#define BOARD_LCD_GPIO_G1       GPIO_NUM_6
#define BOARD_LCD_GPIO_G2       GPIO_NUM_7
#define BOARD_LCD_GPIO_G3       GPIO_NUM_15
#define BOARD_LCD_GPIO_G4       GPIO_NUM_16
#define BOARD_LCD_GPIO_G5       GPIO_NUM_4
#define BOARD_LCD_GPIO_R0       GPIO_NUM_45
#define BOARD_LCD_GPIO_R1       GPIO_NUM_48
#define BOARD_LCD_GPIO_R2       GPIO_NUM_47
#define BOARD_LCD_GPIO_R3       GPIO_NUM_21
#define BOARD_LCD_GPIO_R4       GPIO_NUM_14

/* ---- Touch: GT911 on I2C (shared with Mabee I2C connector) ---- */
#define BOARD_I2C_GPIO_SDA      GPIO_NUM_17
#define BOARD_I2C_GPIO_SCL      GPIO_NUM_18
#define BOARD_I2C_FREQ_HZ       400000
#define BOARD_TOUCH_GPIO_RST    BOARD_LCD_GPIO_RST
#define BOARD_TOUCH_GPIO_INT    GPIO_NUM_NC
#define BOARD_TOUCH_ADDR        0x5D    /* or 0x14, depending on reset strapping */
#define BOARD_TOUCH_RAW_X_MAX   1024    /* GT911 raw range (Makerfabs calibration) */
#define BOARD_TOUCH_RAW_Y_MAX   750

/* ---- microSD: SDMMC 1-bit ---- */
#define BOARD_SD_GPIO_CLK       GPIO_NUM_12
#define BOARD_SD_GPIO_CMD       GPIO_NUM_11
#define BOARD_SD_GPIO_D0        GPIO_NUM_13

/* ---- Speaker: MAX98357A I2S amp, SPK connector ----
 * DOUT and BCLK share GPIO19/20 with the native USB port: using the speaker
 * disconnects it.
 */
#define BOARD_I2S_GPIO_DOUT     GPIO_NUM_19
#define BOARD_I2S_GPIO_BCLK     GPIO_NUM_20
#define BOARD_I2S_GPIO_LRCLK    GPIO_NUM_2

/* ---- Buttons ---- */
#define BOARD_BTN_BOOT          GPIO_NUM_0
