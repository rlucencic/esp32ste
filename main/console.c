/*
 * Line console for tools/st_console.py on the native USB port
 * (USB-Serial-JTAG), or on the USB-to-UART port when the speaker has taken
 * the native USB pins.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include "console.h"
#include "st.h"
#include "touch.h"

#define UART_BAUD       921600

static const char *TAG = "console";

#if CONFIG_ST_SPEAKER

/* UART0 (the board's USB-to-UART bridge) */
static void console_write(const char *data, size_t len)
{
    uart_write_bytes(UART_NUM_0, data, len);
}

static bool console_read_line(char *line, size_t size)
{
    size_t n = 0;
    for (;;) {
        char c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n' || n + 1 >= size) {
            line[n] = 0;
            return true;
        }
        if (c != '\r') {
            line[n++] = c;
        }
    }
}

/* ESP logs go to the UART too: the native USB console is gone */
static int log_vprintf(const char *fmt, va_list args)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    console_write(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
    return n;
}

static bool console_open(void)
{
    const uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(UART_NUM_0, 1024, 16384, 0, NULL, 0) != ESP_OK ||
        uart_param_config(UART_NUM_0, &cfg) != ESP_OK) {
        return false;
    }
    esp_log_set_vprintf(log_vprintf);
    ESP_LOGI(TAG, "console on UART0, %d baud", UART_BAUD);
    return true;
}

#else

/* USB-Serial-JTAG (native USB port) */
static void console_write(const char *data, size_t len)
{
    /* Straight to the driver in chunks that fit its TX ring; stdio would
     * drop data whenever the host is slower than 50 ms. Give up if the
     * host stops reading.
     */
    for (size_t off = 0; off < len;) {
        size_t chunk = len - off < 1024 ? len - off : 1024;
        int n = usb_serial_jtag_write_bytes(data + off, chunk, pdMS_TO_TICKS(2000));
        if (n <= 0) {
            break;
        }
        off += n;
    }
}

static bool console_read_line(char *line, size_t size)
{
    return fgets(line, size, stdin) != NULL;
}

static bool console_open(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        return false;
    }
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);
    return true;
}

#endif

static void console_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    console_write(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
}

static void base64_line(const uint8_t *in, size_t len, char *out)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = in[i] << 16;
        if (i + 1 < len) v |= in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = table[(v >> 18) & 63];
        out[o++] = table[(v >> 12) & 63];
        out[o++] = i + 1 < len ? table[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? table[v & 63] : '=';
    }
    out[o] = 0;
}

static uint16_t *s_lcd_fb;

/* Screen as big-endian RGB565, built as one block: the ST's screen at its
 * own resolution, or with `lcd` the whole panel
 */
/* A few rows at a time (a whole screen and its base64 would take 2.8 MB):
 * 12 rows of any width the ST or the panel has are whole 96-byte lines
 */
#define DUMP_ROWS   12

static void dump_screen(bool lcd)
{
    st_screen_mode_t mode;
    if (lcd) {
        if (s_lcd_fb == NULL) {
            return;
        }
        mode = (st_screen_mode_t){ BOARD_LCD_H_RES, BOARD_LCD_V_RES, 0, 0 };
    } else {
        st_get_screen_mode(&mode);
    }
    size_t row_bytes = mode.width * sizeof(uint16_t);
    size_t chunk = row_bytes * DUMP_ROWS;
    uint16_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM);
    char *text = heap_caps_malloc(chunk / 96 * 130 + 160, MALLOC_CAP_SPIRAM);
    if (buf == NULL || text == NULL) {
        console_printf("STSCREEN ERROR\n");
        free(buf);
        free(text);
        return;
    }

    /* stdout stays locked so log lines from other tasks wait meanwhile */
    fflush(stdout);
    flockfile(stdout);
    size_t len = sprintf(text, "\nSTSCREEN %d %d\n", mode.width, mode.height);
    console_write(text, len);
    for (int y = 0; y < mode.height; y += DUMP_ROWS) {
        int rows = mode.height - y < DUMP_ROWS ? mode.height - y : DUMP_ROWS;
        for (int r = 0; r < rows; r++) {
            uint16_t *row = buf + r * mode.width;
            if (lcd) {
                memcpy(row, s_lcd_fb + (y + r) * BOARD_LCD_H_RES, row_bytes);
            } else {
                st_render_line_rgb565(y + r, row);
            }
            for (int x = 0; x < mode.width; x++) {
                row[x] = __builtin_bswap16(row[x]);
            }
        }
        size_t bytes = rows * row_bytes;
        len = 0;
        for (size_t off = 0; off < bytes; off += 96) {
            size_t n = bytes - off < 96 ? bytes - off : 96;
            base64_line((const uint8_t *)buf + off, n, text + len);
            len += strlen(text + len);
            text[len++] = '\n';
        }
        console_write(text, len);
    }
    console_write("STSCREEN END\n", 13);
    funlockfile(stdout);
    free(buf);
    free(text);
}

/* Machine state for debugging */
static void print_info(void)
{
    st_status_t s;
    st_screen_mode_t mode;
    st_get_status(&s);
    st_get_screen_mode(&mode);
    uint32_t v_bas_ad = ((uint32_t)st_peek16(0x44E) << 16) | st_peek16(0x450);
    uint32_t sum = 0;
    for (uint32_t a = v_bas_ad; a < v_bas_ad + 32000; a += 2) {
        sum += st_peek16(a);
    }
    console_printf("STINFO frames %lu PC $%06lx SR $%04x hz_200 %lu mode %dx%d %d Hz "
           "v_bas_ad $%06lx screen sum %08lx signature %08lx\n",
           (unsigned long)s.frames, (unsigned long)s.pc, s.sr, (unsigned long)s.hz_200,
           mode.width, mode.height, mode.hz, (unsigned long)v_bas_ad,
           (unsigned long)sum, (unsigned long)st_screen_signature());
}

static void console_task(void *arg)
{
    char line[128];
    for (;;) {
        if (!console_read_line(line, sizeof(line))) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        switch (line[0]) {
        case 's':
            dump_screen(false);
            break;
        case 'L':
            dump_screen(true);
            break;
        case 'k': {
            unsigned code = strtoul(line + 1, NULL, 16);
            st_ikbd_key(code, true);
            vTaskDelay(pdMS_TO_TICKS(60));
            st_ikbd_key(code, false);
            break;
        }
        case 'm': {
            int dx = 0, dy = 0, buttons = 0;
            sscanf(line + 1, "%d %d %d", &dx, &dy, &buttons);
            st_ikbd_mouse(dx, dy, buttons & 1, buttons & 2);
            break;
        }
        case 'r':
            emulator_request_reset();
            break;
        case 'R': {
            int tos = 0, ram_kb = 0;
            if (sscanf(line + 1, "%d %d", &tos, &ram_kb) == 2) {
                emulator_restart(tos, ram_kb);
            }
            break;
        }
        case 'F': {
            int drive = -1, at = 0;
            if (sscanf(line + 1, " %d %n", &drive, &at) >= 1) {
                char *name = line + 1 + at;
                name[strcspn(name, "\r\n")] = 0;
                emulator_floppy(drive, name);
            }
            break;
        }
        case 't': {
            int x = 0, y = 0, ms = 150;
            if (sscanf(line + 1, "%d %d %d", &x, &y, &ms) >= 2) {
                touch_inject(x, y, ms);
            }
            break;
        }
        case 'i':
            print_info();
            break;
        default:
            break;
        }
    }
}

void console_start(uint16_t *lcd_fb)
{
    s_lcd_fb = lcd_fb;
    if (!console_open()) {
        ESP_LOGE(TAG, "console driver install failed");
        return;
    }
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 3, NULL, 0);
}
