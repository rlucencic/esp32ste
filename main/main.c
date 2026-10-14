/*
 * Atari STE emulator on the Makerfabs MaTouch ESP32-S3 7".
 *
 *   core 1: emulation, paced to the ST frame rate
 *   core 0: LCD presenter, serial console, setup screen (also on demand:
 *           long-press the top-right corner while the ST runs)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "ble_hid.h"
#include "board.h"
#include "console.h"
#include "driver/temperature_sensor.h"
#include "emulator.h"
#include "floppies.h"
#include "lcd.h"
#include "net.h"
#include "sdcard.h"
#include "settings.h"
#include "setup_ui.h"
#include "speaker.h"
#include "st.h"
#include "tos_rom.h"
#include "touch.h"
#if CONFIG_ST_WEB
#include "web.h"
#endif

/* ST picture area on the panel: 960x600, centred */
#define VIEW_W          960
#define VIEW_H          BOARD_LCD_V_RES
#define VIEW_X          ((BOARD_LCD_H_RES - VIEW_W) / 2)
#define PRESENT_MS      40

/* Long-press here (top-right corner, mostly the black border) for setup */
#define SETUP_ZONE_X    (BOARD_LCD_H_RES - 100)
#define SETUP_ZONE_Y    100
#define SETUP_HOLD_MS   2000

static const char *TAG = "main";

static volatile bool reset_requested;
static volatile uint32_t emu_frames;     /* written by the emulator task only */

/* Pausing the emulator for the setup screen */
static volatile bool s_pause_request;
static volatile bool s_paused;
static volatile bool s_present_on = true;
static bool s_started;                  /* the emulator task runs */

static st_config_t s_cfg;

/* Set before restarting the board for a bigger ST RAM: start the ST right
 * away afterwards instead of showing the setup screen (survives a restart)
 */
#define START_AFTER_RESTART     0x53544152u
static RTC_NOINIT_ATTR uint32_t s_start_after_restart;
static emu_info_t s_info = { .temperature = NAN };
static int64_t s_session_start;
static uint16_t *s_fb;
static char s_drive_info[64];

void emulator_request_reset(void)
{
    reset_requested = true;
}

static void emulator_task(void *arg)
{
    int64_t next = esp_timer_get_time();

    for (;;) {
        if (s_pause_request) {
            s_paused = true;
            while (s_pause_request) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            s_paused = false;
            next = esp_timer_get_time();
        }
        if (reset_requested) {
            reset_requested = false;
            st_reset();
        }
        st_run_frame();
        emu_frames++;

#if CONFIG_ST_SPEAKER
        /* The speaker's I2S clock sets the pace: stay ~2 frames ahead of it */
        while (st_audio_pending() > ST_AUDIO_HZ / 25 && !s_pause_request) {
            vTaskDelay(1);
        }
        (void)next;
#else
        st_screen_mode_t mode;
        st_get_screen_mode(&mode);
        next += 1000000 / mode.hz;
        int64_t now = esp_timer_get_time();
        if (next > now) {
            vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
        } else if (now - next > 100000) {
            next = now;     /* too far behind: don't try to catch up */
        }
#endif
    }
}

/* GEM (re)started in a resolution: remembered for the next boot. Called
 * from the emulator task; the NVS write happens in stats_task.
 */
static volatile int8_t s_desktop_rez = -2;

static void resolution_changed(int rez)
{
    s_desktop_rez = rez;
}

void emulator_info(emu_info_t *info)
{
    *info = s_info;
    info->state = !s_started ? EMU_SETUP : s_paused ? EMU_PAUSED : EMU_RUNNING;
    if (info->state == EMU_RUNNING) {
        st_status_t s;
        st_get_status(&s);
        if (s.halted) {
            info->state = EMU_HALTED;
        }
    }
    info->session_s = s_started ? (esp_timer_get_time() - s_session_start) / 1000000 : 0;
}

/* The chip temperature, on request (the status page): reading the sensor
 * blocks interrupts on the reading core for a moment, which on core 0 made
 * the LCD's bounce buffer refill late (picture artefacts). So it's read on
 * core 1, above the emulator's priority, with the sensor powered only
 * while it's read.
 */
#define TEMPERATURE_EVERY_US    5000000

static TaskHandle_t s_temperature_task;

static void temperature_task(void *arg)
{
    temperature_sensor_handle_t tsens = NULL;
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &tsens) != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        float celsius;
        if (temperature_sensor_enable(tsens) == ESP_OK) {
            if (temperature_sensor_get_celsius(tsens, &celsius) == ESP_OK) {
                s_info.temperature = celsius;
            }
            temperature_sensor_disable(tsens);
        }
    }
}

void emulator_measure_temperature(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (s_temperature_task != NULL && (last == 0 || now - last >= TEMPERATURE_EVERY_US)) {
        last = now;
        xTaskNotifyGive(s_temperature_task);
    }
}

/* Every second: speed and core loads (from the idle tasks' run time) for
 * emulator_info(); the speed also goes to the log every 5 s (from core 0,
 * so a slow console never holds up the emulator). Also saves a changed
 * desktop resolution.
 */
static void stats_task(void *arg)
{
    uint32_t last_frames = emu_frames;
    int64_t last = esp_timer_get_time();
    uint32_t last_idle[2] = { ulTaskGetIdleRunTimeCounterForCore(0),
                              ulTaskGetIdleRunTimeCounterForCore(1) };
    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int8_t rez = s_desktop_rez;
        if (rez >= 0 && rez != settings.resolution) {
            settings.resolution = rez;
            settings_save();
        }

        uint32_t frames = emu_frames;
        int64_t now = esp_timer_get_time();
        float elapsed = now - last;
        for (int core = 0; core < 2; core++) {
            uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(core);
            float busy = 100.0f - (idle - last_idle[core]) * 100.0f / elapsed;
            s_info.load[core] = busy < 0 ? 0 : busy > 100 ? 100 : busy;
            last_idle[core] = idle;
        }

        if (s_started && !s_paused) {
            st_screen_mode_t mode;
            st_get_screen_mode(&mode);
            s_info.fps = (frames - last_frames) * 1e6f / elapsed;
            s_info.speed = (int)(s_info.fps * 100 / mode.hz + 0.5f);
            if (tick % 5 == 0) {
                st_status_t s;
                st_get_status(&s);
                ESP_LOGI(TAG, "%.1f fps (%d%% of %d Hz), PC $%06lx, hz_200 %lu, bus errors %lu",
                         s_info.fps, s_info.speed, mode.hz, (unsigned long)s.pc,
                         (unsigned long)s.hz_200, (unsigned long)s.bus_errors);
            }
        } else {
            s_info.fps = 0;
            s_info.speed = 0;
        }
        last_frames = frames;
        last = now;
    }
}

/* Scale the ST screen to 960x600: low res x3, medium x1.5/x3, high x1.5 */
static void present_task(void *arg)
{
    uint16_t *fb = arg;
    uint16_t *src = heap_caps_malloc(640 * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    uint16_t *row = heap_caps_malloc(VIEW_W * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    static uint16_t xmap[VIEW_W];
    uint32_t last_signature = 0;
    int last_width = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PRESENT_MS));
        if (!s_present_on) {
            last_signature = 0;     /* redraw everything afterwards */
            continue;
        }

        uint32_t signature = st_screen_signature();
        if (signature == last_signature) {
            continue;
        }
        last_signature = signature;

        st_screen_mode_t mode;
        st_get_screen_mode(&mode);
        if (mode.width != last_width) {
            for (int x = 0; x < VIEW_W; x++) {
                xmap[x] = x * mode.width / VIEW_W;
            }
            last_width = mode.width;
        }

        int last_y = -1;
        for (int r = 0; r < VIEW_H; r++) {
            int y = r * mode.height / VIEW_H;
            if (y != last_y) {
                st_render_line_rgb565(y, src);
                for (int x = 0; x < VIEW_W; x++) {
                    row[x] = src[xmap[x]];
                }
                last_y = y;
            }
            memcpy(fb + r * BOARD_LCD_H_RES + VIEW_X, row, VIEW_W * sizeof(uint16_t));
        }
    }
}

/* (Re)start the ST with the current settings (TOS, RAM). The emulator task
 * must not be running a frame.
 */
/* The ST's RAM and ROM copy, first thing at power-on: 4 MB must be one
 * block of PSRAM, which it no longer is once the screen, WiFi and Bluetooth
 * have taken their pieces
 */
/* PSRAM the rest of the board needs: screen buffer (1.2 MB), ROM copy,
 * WiFi, Bluetooth, the setup screen: 1.7 MB measured at most, plus room
 */
#define PSRAM_FOR_THE_REST      (2048 * 1024)

static uint16_t s_ram_limit_kb = 512;      /* the most ST RAM that fits */

uint16_t emulator_ram_limit_kb(void)
{
    return s_ram_limit_kb;
}

/* ST RAM to use: the setting, or less if that doesn't fit */
static uint32_t ram_wanted(void)
{
    uint16_t kb = settings.ram_kb < s_ram_limit_kb ? settings.ram_kb : s_ram_limit_kb;
    return kb * 1024u;
}

static void machine_memory(void)
{
    /* The CPU core's opcode tables, read for every instruction: from PSRAM,
     * not flash (read-only data stays in flash to leave room for 4 MB)
     */
    s_cfg.opcode_tables = heap_caps_malloc(ST_OPCODE_TABLES_SIZE, MALLOC_CAP_SPIRAM);

    static const uint16_t sizes[] = { 4096, 2048, 1024, 512 };
    size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    room = room > PSRAM_FOR_THE_REST ? room - PSRAM_FOR_THE_REST : 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        if (sizes[i] * 1024u <= room) {
            s_ram_limit_kb = sizes[i];
            break;
        }
    }
    uint32_t ram_size = ram_wanted();
    if (ram_size / 1024 != settings.ram_kb) {
        ESP_LOGW(TAG, "no room for %u KB of ST RAM, using %u KB", (unsigned)settings.ram_kb,
                 (unsigned)(ram_size / 1024));
    }
    s_cfg.ram = heap_caps_malloc(ram_size, MALLOC_CAP_SPIRAM);
    s_cfg.ram_size = ram_size;
    s_cfg.rom = heap_caps_malloc(256 * 1024, MALLOC_CAP_SPIRAM);
}

static bool machine_start(void)
{
    tos_rom_t tos;
    if (tos_rom_map(settings_tos_partition(), &tos) != ESP_OK) {
        return false;
    }
    uint32_t ram_size = ram_wanted();
    if (s_cfg.ram == NULL || s_cfg.ram_size != ram_size) {
        free(s_cfg.ram);
        s_cfg.ram = heap_caps_malloc(ram_size, MALLOC_CAP_SPIRAM);
        if (s_cfg.ram == NULL) {
            /* PSRAM is in pieces by now: restart the board, which takes the
             * memory first and goes straight into the ST (the setting is saved)
             */
            ESP_LOGW(TAG, "no %u KB block for the ST RAM now: restarting the board",
                     (unsigned)(ram_size / 1024));
            tos_rom_unmap(&tos);
            s_start_after_restart = START_AFTER_RESTART;
            esp_restart();
        }
        s_cfg.ram_size = ram_size;
    }
    s_cfg.tos = tos.data;
    s_cfg.tos_size = tos.size;
    s_cfg.start_resolution = settings.resolution;

    bool ok = s_cfg.ram != NULL && s_cfg.rom != NULL && st_init(&s_cfg);
    if (ok) {
        s_info.tos_version = tos.version;
        s_info.ram_kb = s_cfg.ram_size / 1024;
        s_session_start = esp_timer_get_time();
    }
    tos_rom_unmap(&tos);    /* st_init copied it */
    s_cfg.tos = NULL;
    if (!ok) {
        ESP_LOGE(TAG, "ST setup failed (%u KB RAM)", (unsigned)settings.ram_kb);
    }
    return ok;
}

/* Stop the emulator task between frames (and the presenter) */
static void emulator_pause(void)
{
    s_present_on = false;
    s_pause_request = true;
    while (!s_paused) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void emulator_resume(void)
{
    s_present_on = true;
    s_pause_request = false;
}

/* Restart the paused ST if TOS or RAM differ from `before` */
static void restart_if_changed(const settings_t *before)
{
    if (settings.tos != before->tos || settings.ram_kb != before->ram_kb) {
        ESP_LOGI(TAG, "restarting the ST: TOS %s, %u KB", settings.tos ? "2.06" : "1.62",
                 (unsigned)settings.ram_kb);
        if (!machine_start()) {
            /* e.g. that TOS partition was never flashed: back to the old one */
            settings.tos = before->tos;
            settings_save();
            machine_start();
        }
    }
}

void emulator_restart(int tos, int ram_kb)
{
    if (!s_started || s_pause_request || tos < 0 || tos > 1 ||
        (ram_kb != 512 && ram_kb != 1024 && ram_kb != 2048 && ram_kb != 4096)) {
        return;
    }
    emulator_pause();
    settings_t before = settings;
    settings.tos = tos;
    settings.ram_kb = ram_kb;
    settings_save();
    restart_if_changed(&before);
    emulator_resume();
}

/* Console: insert `name` from the floppies folder, "" ejects, "+" makes a
 * new blank disk
 */
void emulator_floppy(int drive, const char *name)
{
    if (drive < 0 || drive > 1 || (s_started && s_pause_request)) {
        return;
    }
    if (s_started) {
        emulator_pause();
    }
    char made[32];
    if (name[0] == 0) {
        floppies_eject(drive);
    } else if (strcmp(name, "+") == 0) {
        floppies_create(drive, made, sizeof(made));
    } else {
        floppies_insert(drive, name);
    }
    if (s_started) {
        emulator_resume();
    }
}

#if CONFIG_ST_SETUP_SCREEN

/* Pause the ST, show the setup screen, restart the ST if TOS or RAM changed */
static void setup_while_running(void)
{
    emulator_pause();
    settings_t before = settings;
    bool reset = setup_ui_run(s_fb, s_drive_info, true);
    restart_if_changed(&before);
    if (reset) {
        st_reset();     /* the emulator task is paused: safe here */
    }
    emulator_resume();
}

/* Watch for a long press in the top-right corner */
static void supervisor_task(void *arg)
{
    int64_t held_since = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int x = 0, y = 0;
        bool pressed = false;
        touch_read(&x, &y, &pressed);
        if (!pressed || x < SETUP_ZONE_X || y > SETUP_ZONE_Y) {
            held_since = 0;
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (held_since == 0) {
            held_since = now;
        } else if (now - held_since >= SETUP_HOLD_MS * 1000LL) {
            held_since = 0;
            /* Wait for the finger to lift so the setup screen doesn't see it */
            do {
                vTaskDelay(pdMS_TO_TICKS(50));
                touch_read(&x, &y, &pressed);
            } while (pressed);
            setup_while_running();
        }
    }
}

#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }

    settings_load();
    bool start_now = s_start_after_restart == START_AFTER_RESTART;
    s_start_after_restart = 0;
    machine_memory();

    const char *hd = NULL;
    bool sd = sdcard_mount();
#if CONFIG_ST_HD
    static char hd_path[128];
    if (sd) {
        snprintf(hd_path, sizeof(hd_path), "%s%s%s", SDCARD_MOUNT_POINT,
                 CONFIG_ST_HD_FOLDER[0] ? "/" : "", CONFIG_ST_HD_FOLDER);
        hd = hd_path;
    }
#endif
    s_info.hard_disk = hd != NULL;
    if (sd) {
        floppies_init();
        floppies_restore();     /* before the ST starts: a disk in A: boots */
    }
    snprintf(s_drive_info, sizeof(s_drive_info), "%s",
             hd ? "Hard disk C: on the SD card" :
             sd ? "SD card: floppy disks only" : "No SD card: no hard disk or floppy disks");

    s_fb = lcd_init();
    if (s_fb != NULL) {
        memset(s_fb, 0, BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t));
    }
    touch_init();
    console_start(s_fb);

#if CONFIG_ST_WIFI
    bool net = net_init();  /* before BLE: WiFi needs the larger internal RAM blocks */
#endif
#if CONFIG_ST_WEB
    if (net) {
        web_start();
    }
#endif
#if CONFIG_ST_BLE_HID
    if (err == ESP_OK) {
        ble_hid_start();
    }
#endif
    xTaskCreatePinnedToCore(stats_task, "stats", 3072, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(temperature_task, "temperature", 2560, NULL, 11,
                            &s_temperature_task, 1);
#if CONFIG_ST_SETUP_SCREEN
    if (s_fb != NULL && !start_now) {
        setup_ui_run(s_fb, s_drive_info, false);
    }
#endif

    /* (RAM and ROM buffers are already in s_cfg: machine_memory()) */
#if CONFIG_ST_MONO
    s_cfg.monitor = ST_MONITOR_MONO;
#else
    s_cfg.monitor = ST_MONITOR_COLOR;
#endif
#if CONFIG_ST_ANY_RESOLUTION
    s_cfg.any_resolution = true;
#endif
    s_cfg.resolution_changed = resolution_changed;
    s_cfg.hd_path = hd;
#if CONFIG_ST_HD_BOOT
    s_cfg.hd_boot = true;
#endif
#if CONFIG_ST_SPEAKER
    st_audio_enable(true);
#endif
    if (!machine_start()) {
        return;
    }

    if (s_fb != NULL) {
        xTaskCreatePinnedToCore(present_task, "present", 4096, s_fb, 5, NULL, 0);
    }
#if CONFIG_ST_SPEAKER
    speaker_start();
#endif
    xTaskCreatePinnedToCore(emulator_task, "emulator", 8192, NULL, 10, NULL, 1);
    s_started = true;
#if CONFIG_ST_SETUP_SCREEN
    if (s_fb != NULL) {
        xTaskCreatePinnedToCore(supervisor_task, "supervisor", 16384, NULL, 4, NULL, 0);
    }
#endif

    ESP_LOGI(TAG, "Free internal: %u bytes, PSRAM: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
