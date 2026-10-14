/*
 * GT911 capacitive touch on I2C (as in wmbus_hub / esp32_basic). Raw
 * coordinates use Makerfabs' 1024x750 range and are scaled to the panel.
 */
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board.h"
#include "touch.h"

#define GT911_ADDR_ALT      0x14
#define GT911_REG_PRODUCT   0x8140
#define GT911_REG_STATUS    0x814E
#define GT911_REG_POINT1    0x814F

static const char *TAG = "touch";
static i2c_master_dev_handle_t s_dev;

static esp_err_t gt911_read(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t r[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(s_dev, r, sizeof(r), data, len, 50);
}

static esp_err_t gt911_write8(uint16_t reg, uint8_t value)
{
    uint8_t buf[3] = { reg >> 8, reg & 0xFF, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static bool try_address(i2c_master_bus_handle_t bus, uint8_t addr)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        return false;
    }
    uint8_t id[4];
    if (gt911_read(GT911_REG_PRODUCT, id, sizeof(id)) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 at 0x%02X, product %c%c%c%c", addr, id[0], id[1], id[2], id[3]);
        return true;
    }
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

bool touch_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_GPIO_SDA,
        .scl_io_num = BOARD_I2C_GPIO_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed");
        return false;
    }
    if (try_address(bus, BOARD_TOUCH_ADDR) || try_address(bus, GT911_ADDR_ALT)) {
        return true;
    }
    ESP_LOGW(TAG, "no GT911 found");
    return false;
}

static int scale(int raw, int raw_max, int size)
{
    int v = raw * size / raw_max;
    return v < 0 ? 0 : v >= size ? size - 1 : v;
}

static volatile int64_t s_inject_until;
static volatile int s_inject_x, s_inject_y;

void touch_inject(int x, int y, int ms)
{
    s_inject_x = x;
    s_inject_y = y;
    s_inject_until = esp_timer_get_time() + ms * 1000LL;
}

bool touch_read(int *x, int *y, bool *pressed)
{
    if (esp_timer_get_time() < s_inject_until) {
        *x = s_inject_x;
        *y = s_inject_y;
        *pressed = true;
        return true;
    }
    if (s_dev == NULL) {
        return false;
    }
    uint8_t status = 0;
    *pressed = false;
    if (gt911_read(GT911_REG_STATUS, &status, 1) != ESP_OK) {
        return true;
    }
    if ((status & 0x80) && (status & 0x0F)) {
        uint8_t p[7];
        if (gt911_read(GT911_REG_POINT1, p, sizeof(p)) == ESP_OK) {
            *x = scale(p[1] | (p[2] << 8), BOARD_TOUCH_RAW_X_MAX, BOARD_LCD_H_RES);
            *y = scale(p[3] | (p[4] << 8), BOARD_TOUCH_RAW_Y_MAX, BOARD_LCD_V_RES);
            *pressed = true;
        }
    }
    if (status & 0x80) {
        gt911_write8(GT911_REG_STATUS, 0);
    }
    return true;
}
