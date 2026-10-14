/*
 * Setup screen (LVGL 9), shown before the ST starts:
 *
 *   status     WiFi and clock, TOS and RAM choice,
 *              [WiFi] [Bluetooth] [Floppy] [Start Atari]
 *   networks   scan results; tap one to connect
 *   password   text field + on-screen keyboard
 *   bluetooth  paired keyboard/mouse, [Forget] each, [Pair new device]
 *   pairing    HID devices in pairing mode nearby; tap one to pair
 *   floppy     drives A: and B: with [Insert disk] [New empty disk] [Eject]
 *   images     disk images on the SD card; tap one to insert it
 *
 * It waits for [Start Atari]. With CONFIG_ST_AUTOSTART it also starts the
 * ST after a countdown once the clock is set (or without saved WiFi); any
 * touch stops the countdown. Long-pressing the top-right corner while the
 * ST runs brings this screen back ("Back to Atari" then).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ble_hid.h"
#include "board.h"
#include "floppies.h"
#include "net.h"
#include "settings.h"
#include "setup_ui.h"
#include "st.h"
#include "touch.h"

#define BUF_LINES           60
#if CONFIG_ST_AUTOSTART
#define AUTOSTART_S         CONFIG_ST_AUTOSTART_SECONDS
#define OFFLINE_START_S     (CONFIG_ST_AUTOSTART_SECONDS * 2)
#endif
#define MAX_LIST            16

#define COLOR_BG            lv_color_hex(0x10141C)
#define COLOR_CARD          lv_color_hex(0x1E2533)
#define COLOR_TEXT          lv_color_hex(0xE8ECF2)
#define COLOR_DIM           lv_color_hex(0x8A94A6)
#define COLOR_OK            lv_color_hex(0x4CC38A)
#define COLOR_WARN          lv_color_hex(0xE5A54B)
#define COLOR_ROW           lv_color_hex(0x283143)
#define COLOR_ROW_PRESSED   lv_color_hex(0x34405A)

static const char *tos_names[] = { "TOS 1.62", "TOS 2.06", "" };
static const char *ram_names[] = { "512 KB", "1 MB", "4 MB", "" };
static const uint16_t ram_sizes[] = { 512, 1024, 4096 };

static struct {
    uint16_t *fb;
    bool running;                   /* the ST is paused behind this screen */
    uint8_t run_tos;                /* ... with this TOS and RAM */
    uint16_t run_ram_kb;
    bool done;
    bool reset;                     /* [Reset Atari] pressed */
    bool countdown_cancelled;
    int64_t countdown_end;          /* 0 = not counting */
    lv_obj_t *status_screen, *list_screen, *pass_screen, *bt_screen, *pair_screen;
    lv_obj_t *wifi_label, *time_label, *countdown_label, *start_label;
    lv_obj_t *list, *scan_label;
    bool scan_pending;
    lv_obj_t *pass_title, *pass_field;
    char chosen_ssid[33];
    lv_obj_t *bt_list, *bt_status, *pair_list, *pair_status;
    char bt_shown[512], pair_shown[512];   /* what the lists show now */
    lv_obj_t *floppy_screen, *images_screen;
    lv_obj_t *floppy_status, *drive_label[2], *drive_buttons[2][3];
    lv_obj_t *images_title, *images_count, *images_list;
    int images_drive;                       /* drive the chooser inserts into */
    char **images;
    int image_count;
} ui;

/* ---- display + input glue -------------------------------------------------- */

static uint32_t tick_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    int w = lv_area_get_width(area);
    const uint16_t *src = (const uint16_t *)px;
    for (int y = area->y1; y <= area->y2; y++, src += w) {
        memcpy(ui.fb + y * BOARD_LCD_H_RES + area->x1, src, w * sizeof(uint16_t));
    }
    lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    int x = 0, y = 0;
    bool pressed = false;
    touch_read(&x, &y, &pressed);
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        ui.countdown_cancelled = true;
    }
}

/* ---- helpers ---------------------------------------------------------------- */

static lv_obj_t *make_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, COLOR_BG, 0);
    lv_obj_set_style_text_color(s, COLOR_TEXT, 0);
    lv_obj_set_style_pad_all(s, 32, 0);
    return s;
}

static lv_obj_t *make_button_for(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int width,
                                 void *user_data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, width, 72);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int width)
{
    return make_button_for(parent, text, cb, width, NULL);
}

static lv_obj_t *make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, text);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    return t;
}

static lv_obj_t *make_list(lv_obj_t *parent, int y, int height)
{
    lv_obj_t *l = lv_list_create(parent);
    lv_obj_set_size(l, lv_pct(100), height);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_bg_color(l, COLOR_CARD, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    return l;
}

/* A list row in the dark theme */
static lv_obj_t *add_row(lv_obj_t *list, const char *icon, const char *text)
{
    lv_obj_t *b = lv_list_add_button(list, icon, text);
    lv_obj_set_height(b, 64);
    lv_obj_set_style_bg_color(b, COLOR_ROW, 0);
    lv_obj_set_style_bg_color(b, COLOR_ROW_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, COLOR_TEXT, 0);
    lv_obj_set_style_border_color(b, COLOR_CARD, 0);
    return b;
}

/* A row "label  [a][b][c]" with one choice checked */
static lv_obj_t *make_choice(lv_obj_t *parent, const char *label, const char **names,
                             int checked, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 60);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, COLOR_TEXT, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *m = lv_buttonmatrix_create(row);
    lv_buttonmatrix_set_map(m, names);
    lv_buttonmatrix_set_button_ctrl_all(m, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(m, true);
    lv_buttonmatrix_set_button_ctrl(m, checked, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(m, 560, 60);
    lv_obj_align(m, LV_ALIGN_LEFT_MID, 160, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_add_event_cb(m, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return m;
}

/* ---- status screen ---------------------------------------------------------- */

static void start_clicked(lv_event_t *e)
{
    ui.done = true;
}

static void reset_clicked(lv_event_t *e)
{
    ui.reset = true;
    ui.done = true;
}

static void back_to_status(lv_event_t *e)
{
    lv_screen_load(ui.status_screen);
}

/* Paused ST: "Back to Atari" resumes it, "Restart Atari" when TOS or RAM changed */
static void update_start_label(void)
{
    const char *text = LV_SYMBOL_PLAY "  Start Atari";
    if (ui.running) {
        bool changed = settings.tos != ui.run_tos || settings.ram_kb != ui.run_ram_kb;
        text = changed ? LV_SYMBOL_REFRESH "  Restart Atari" : LV_SYMBOL_PLAY "  Back to Atari";
    }
    lv_label_set_text(ui.start_label, text);
}

static void tos_changed(lv_event_t *e)
{
    settings.tos = lv_buttonmatrix_get_selected_button(lv_event_get_target(e)) ? 1 : 0;
    settings_save();
    update_start_label();
}

static void ram_changed(lv_event_t *e)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (i < 3) {
        settings.ram_kb = ram_sizes[i];
        settings_save();
        update_start_label();
    }
}

static void show_networks(lv_event_t *e);
static void show_floppy(lv_event_t *e);
#if CONFIG_ST_BLE_HID
static void show_bluetooth(lv_event_t *e);
#endif

static void build_status_screen(const char *drive_info)
{
    lv_obj_t *s = ui.status_screen = make_screen();
    make_title(s, LV_SYMBOL_HOME "  Atari STE");

    lv_obj_t *card = lv_obj_create(s);
    lv_obj_set_size(card, lv_pct(100), 380);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    ui.wifi_label = lv_label_create(card);
    ui.time_label = lv_label_create(card);

    int ram_index = settings.ram_kb >= 4096 ? 2 : settings.ram_kb >= 1024 ? 1 : 0;
    make_choice(card, "System", tos_names, settings.tos, tos_changed);
    make_choice(card, "Memory", ram_names, ram_index, ram_changed);

    lv_obj_t *info = lv_label_create(card);
    lv_label_set_text_fmt(info, "%s\n%s", drive_info,
                          ui.running ? "Changing the system or memory restarts the Atari."
                                     : "Hold the top-right corner of the screen for 2 s to come back here.");
    lv_obj_set_style_text_color(info, COLOR_DIM, 0);

    lv_obj_t *wifi_btn = make_button(s, LV_SYMBOL_WIFI "  WiFi", show_networks, 190);
    lv_obj_align(wifi_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(wifi_btn, COLOR_CARD, 0);
#if CONFIG_ST_BLE_HID
    lv_obj_t *bt_btn = make_button(s, LV_SYMBOL_BLUETOOTH "  Bluetooth", show_bluetooth, 240);
    lv_obj_align(bt_btn, LV_ALIGN_BOTTOM_LEFT, 206, 0);
    lv_obj_set_style_bg_color(bt_btn, COLOR_CARD, 0);
#endif
    lv_obj_t *floppy_btn = make_button(s, LV_SYMBOL_SAVE "  Floppy", show_floppy, 200);
    lv_obj_align(floppy_btn, LV_ALIGN_BOTTOM_LEFT, 462, 0);
    lv_obj_set_style_bg_color(floppy_btn, COLOR_CARD, 0);

    lv_obj_t *start_btn = make_button(s, "", start_clicked, 280);
    lv_obj_align(start_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    ui.start_label = lv_obj_get_child(start_btn, 0);
    update_start_label();

    if (ui.running) {
        lv_obj_t *reset = make_button(s, LV_SYMBOL_REFRESH "  Reset Atari", reset_clicked, 230);
        lv_obj_set_height(reset, 48);
        lv_obj_align(reset, LV_ALIGN_TOP_RIGHT, 0, -4);
        lv_obj_set_style_bg_color(reset, COLOR_CARD, 0);
    }

    ui.countdown_label = lv_label_create(s);
    lv_obj_set_style_text_color(ui.countdown_label, COLOR_DIM, 0);
    lv_obj_align(ui.countdown_label, LV_ALIGN_TOP_RIGHT, 0, 6);
    lv_label_set_text(ui.countdown_label, "");
}

static void update_status(void)
{
    char ip[20];
    switch (net_state()) {
    case NET_OFF:
        lv_label_set_text(ui.wifi_label, LV_SYMBOL_WIFI "  WiFi: not set up");
        lv_obj_set_style_text_color(ui.wifi_label, COLOR_WARN, 0);
        break;
    case NET_CONNECTING:
        lv_label_set_text_fmt(ui.wifi_label, LV_SYMBOL_WIFI "  WiFi: connecting to %s ...", net_ssid());
        lv_obj_set_style_text_color(ui.wifi_label, COLOR_TEXT, 0);
        break;
    case NET_CONNECTED:
        net_ip(ip, sizeof(ip));
        lv_label_set_text_fmt(ui.wifi_label, LV_SYMBOL_WIFI "  WiFi: %s  (%s)", net_ssid(), ip);
        lv_obj_set_style_text_color(ui.wifi_label, COLOR_OK, 0);
        break;
    case NET_FAILED:
        lv_label_set_text_fmt(ui.wifi_label, LV_SYMBOL_WARNING "  WiFi: can't connect to %s (retrying)",
                              net_ssid());
        lv_obj_set_style_text_color(ui.wifi_label, COLOR_WARN, 0);
        break;
    }

    if (net_time_synced()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[40];
        strftime(buf, sizeof(buf), "%Y-%m-%d  %H:%M:%S", &tm);
        lv_label_set_text_fmt(ui.time_label, LV_SYMBOL_REFRESH "  Time: %s", buf);
        lv_obj_set_style_text_color(ui.time_label, COLOR_OK, 0);
    } else {
        lv_label_set_text(ui.time_label, LV_SYMBOL_REFRESH "  Time: waiting for the internet");
        lv_obj_set_style_text_color(ui.time_label, COLOR_DIM, 0);
    }

#if CONFIG_ST_AUTOSTART
    /* Countdown: once the clock is set, or soon without any WiFi set up */
    int64_t now_us = esp_timer_get_time();
    if (ui.running || ui.countdown_cancelled) {
        ui.countdown_end = 0;
        lv_label_set_text(ui.countdown_label, "");
    } else if (ui.countdown_end == 0) {
        if (net_time_synced()) {
            ui.countdown_end = now_us + AUTOSTART_S * 1000000LL;
        } else if (net_state() == NET_OFF) {
            ui.countdown_end = now_us + OFFLINE_START_S * 1000000LL;
        }
    }
    if (ui.countdown_end) {
        int left = (ui.countdown_end - now_us + 999999) / 1000000;
        if (left <= 0) {
            ui.done = true;
        } else {
            lv_label_set_text_fmt(ui.countdown_label, "Starting in %d s - touch to stay", left);
        }
    }
#endif
}

/* ---- WiFi networks ------------------------------------------------------------ */

static void back_from_networks(lv_event_t *e)
{
    net_resume();
    lv_screen_load(ui.status_screen);
}

static void scan_clicked(lv_event_t *e)
{
    lv_obj_clean(ui.list);
    lv_label_set_text(ui.scan_label, "Scanning ...");
    ui.scan_pending = true;
    net_scan_start();
}

static void network_clicked(lv_event_t *e)
{
    const net_ap_t *ap = lv_event_get_user_data(e);
    snprintf(ui.chosen_ssid, sizeof(ui.chosen_ssid), "%s", ap->ssid);
    if (!ap->secure) {
        net_connect(ui.chosen_ssid, "");
        lv_screen_load(ui.status_screen);
        return;
    }
    lv_label_set_text_fmt(ui.pass_title, "Password for %s", ui.chosen_ssid);
    lv_textarea_set_text(ui.pass_field, "");
    lv_screen_load(ui.pass_screen);
}

static void show_networks(lv_event_t *e)
{
    lv_screen_load(ui.list_screen);
    scan_clicked(NULL);
}

static void build_list_screen(void)
{
    lv_obj_t *s = ui.list_screen = make_screen();
    make_title(s, "Choose a network");
    ui.scan_label = lv_label_create(s);
    lv_obj_set_style_text_color(ui.scan_label, COLOR_DIM, 0);
    lv_obj_align(ui.scan_label, LV_ALIGN_TOP_RIGHT, 0, 6);
    ui.list = make_list(s, 56, 340);

    lv_obj_t *back = make_button(s, LV_SYMBOL_LEFT "  Back", back_from_networks, 300);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(back, COLOR_CARD, 0);
    lv_obj_t *rescan = make_button(s, LV_SYMBOL_REFRESH "  Scan again", scan_clicked, 300);
    lv_obj_align(rescan, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void update_networks(void)
{
    static net_ap_t aps[MAX_LIST];
    if (!ui.scan_pending || !net_scan_done()) {
        return;
    }
    ui.scan_pending = false;
    int n = net_scan_results(aps, MAX_LIST);
    lv_obj_clean(ui.list);
    lv_label_set_text_fmt(ui.scan_label, "%d found", n);
    for (int i = 0; i < n; i++) {
        char text[64];
        snprintf(text, sizeof(text), "%.32s    %d dBm", aps[i].ssid, aps[i].rssi);
        lv_obj_t *b = add_row(ui.list, aps[i].secure ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_WIFI, text);
        lv_obj_add_event_cb(b, network_clicked, LV_EVENT_CLICKED, &aps[i]);
    }
}

/* ---- password ------------------------------------------------------------------ */

static void keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        net_connect(ui.chosen_ssid, lv_textarea_get_text(ui.pass_field));
        ui.countdown_end = 0;
        ui.countdown_cancelled = false;
        lv_screen_load(ui.status_screen);
    } else if (code == LV_EVENT_CANCEL) {
        lv_screen_load(ui.list_screen);
        scan_clicked(NULL);
    }
}

static void build_pass_screen(void)
{
    lv_obj_t *s = ui.pass_screen = make_screen();
    ui.pass_title = make_title(s, "");

    ui.pass_field = lv_textarea_create(s);
    lv_textarea_set_one_line(ui.pass_field, true);
    lv_textarea_set_placeholder_text(ui.pass_field, "password");
    lv_obj_set_width(ui.pass_field, lv_pct(100));
    lv_obj_align(ui.pass_field, LV_ALIGN_TOP_LEFT, 0, 56);

    lv_obj_t *kb = lv_keyboard_create(s);
    lv_obj_set_size(kb, BOARD_LCD_H_RES - 64, 330);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ui.pass_field);
    lv_obj_add_event_cb(kb, keyboard_event, LV_EVENT_ALL, NULL);
}

/* ---- Bluetooth ----------------------------------------------------------------- */

#if CONFIG_ST_BLE_HID

static const char *kind_symbol(uint8_t kind)
{
    return kind == HID_KIND_KEYBOARD ? LV_SYMBOL_KEYBOARD :
           kind == HID_KIND_MOUSE ? LV_SYMBOL_GPS : LV_SYMBOL_BLUETOOTH;
}

static void forget_clicked(lv_event_t *e)
{
    ble_hid_forget((int)(intptr_t)lv_event_get_user_data(e));
    ui.bt_shown[0] = 0;
}

static void pair_clicked(lv_event_t *e)
{
    ble_hid_pair((int)(intptr_t)lv_event_get_user_data(e));
}

static void show_bluetooth(lv_event_t *e)
{
    ble_hid_set_pairing(false);
    ui.bt_shown[0] = 0;
    lv_screen_load(ui.bt_screen);
}

static void show_pairing(lv_event_t *e)
{
    ble_hid_set_pairing(true);
    ui.pair_shown[0] = 0;
    lv_screen_load(ui.pair_screen);
}

static void build_bt_screens(void)
{
    lv_obj_t *s = ui.bt_screen = make_screen();
    make_title(s, "Bluetooth keyboard and mouse");
    ui.bt_status = lv_label_create(s);
    lv_obj_set_style_text_color(ui.bt_status, COLOR_DIM, 0);
    lv_obj_align(ui.bt_status, LV_ALIGN_TOP_LEFT, 0, 44);
    ui.bt_list = make_list(s, 84, 320);
    lv_obj_t *back = make_button(s, LV_SYMBOL_LEFT "  Back", back_to_status, 300);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(back, COLOR_CARD, 0);
    lv_obj_t *pair = make_button(s, LV_SYMBOL_PLUS "  Pair new device", show_pairing, 340);
    lv_obj_align(pair, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    s = ui.pair_screen = make_screen();
    make_title(s, "Pair a device");
    ui.pair_status = lv_label_create(s);
    lv_obj_set_style_text_color(ui.pair_status, COLOR_DIM, 0);
    lv_obj_align(ui.pair_status, LV_ALIGN_TOP_LEFT, 0, 44);
    ui.pair_list = make_list(s, 84, 320);
    lv_obj_t *done = make_button(s, LV_SYMBOL_LEFT "  Done", show_bluetooth, 300);
    lv_obj_align(done, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(done, COLOR_CARD, 0);
}

static void update_bluetooth(void)
{
    ble_hid_device_t dev[MAX_LIST];
    char shown[512];
    lv_obj_t *active = lv_screen_active();

    if (active == ui.bt_screen) {
        int n = ble_hid_devices(dev, MAX_LIST);
        int len = 0;
        for (int i = 0; i < n && len < (int)sizeof(shown) - 40; i++) {
            len += snprintf(shown + len, sizeof(shown) - len, "%s%d%d|", dev[i].name, dev[i].connected,
                            dev[i].battery);
        }
        shown[len] = 0;
        const char *status = ble_hid_status();
        lv_label_set_text(ui.bt_status, status[0] ? status :
                          n ? "Paired devices reconnect by themselves when switched on."
                            : "No keyboard or mouse paired yet.");
        if (strcmp(shown, ui.bt_shown) == 0) {
            return;
        }
        strcpy(ui.bt_shown, shown);
        lv_obj_clean(ui.bt_list);
        for (int i = 0; i < n; i++) {
            char text[80], battery[16] = "";
            if (dev[i].battery >= 0) {
                snprintf(battery, sizeof(battery), ", battery %d%%", dev[i].battery);
            }
            snprintf(text, sizeof(text), "%.31s  -  %s%s", dev[i].name,
                     dev[i].connected ? "connected" : "not connected", battery);
            lv_obj_t *row = add_row(ui.bt_list, kind_symbol(dev[i].kind), text);
            lv_obj_t *f = lv_button_create(row);
            lv_obj_set_size(f, 150, 48);
            lv_obj_add_event_cb(f, forget_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *l = lv_label_create(f);
            lv_label_set_text(l, LV_SYMBOL_TRASH "  Forget");
            lv_obj_center(l);
        }
    } else if (active == ui.pair_screen) {
        int n = ble_hid_candidates(dev, MAX_LIST);
        int len = 0;
        for (int i = 0; i < n && len < (int)sizeof(shown) - 40; i++) {
            len += snprintf(shown + len, sizeof(shown) - len, "%s%d|", dev[i].name, dev[i].kind);
        }
        shown[len] = 0;
        const char *status = ble_hid_status();
        lv_label_set_text(ui.pair_status, status[0] ? status :
                          "Put your keyboard or mouse into pairing mode, then tap it below.");
        if (strcmp(shown, ui.pair_shown) == 0) {
            return;
        }
        strcpy(ui.pair_shown, shown);
        lv_obj_clean(ui.pair_list);
        for (int i = 0; i < n; i++) {
            lv_obj_t *b = add_row(ui.pair_list, kind_symbol(dev[i].kind), dev[i].name);
            lv_obj_add_event_cb(b, pair_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }
}

#endif

/* ---- floppy disks ---------------------------------------------------------------- */

enum { BTN_INSERT, BTN_NEW, BTN_EJECT };

static void update_floppy(void)
{
    for (int d = 0; d < 2; d++) {
        const char *name = floppies_name(d);
        if (name != NULL) {
            lv_label_set_text_fmt(ui.drive_label[d], "%c:   %s%s", 'A' + d, name,
                                  st_floppy_write_protected(d) ? "   (read-only)" : "");
            lv_obj_set_style_text_color(ui.drive_label[d], COLOR_TEXT, 0);
        } else {
            lv_label_set_text_fmt(ui.drive_label[d], "%c:   no disk", 'A' + d);
            lv_obj_set_style_text_color(ui.drive_label[d], COLOR_DIM, 0);
        }
        for (int b = 0; b < 3; b++) {
            bool usable = floppies_ready() && (b != BTN_EJECT || name != NULL);
            if (usable) {
                lv_obj_remove_state(ui.drive_buttons[d][b], LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(ui.drive_buttons[d][b], LV_STATE_DISABLED);
            }
        }
    }
}

static void set_floppy_status(const char *text, lv_color_t color)
{
    lv_label_set_text(ui.floppy_status, text);
    lv_obj_set_style_text_color(ui.floppy_status, color, 0);
}

static void show_floppy(lv_event_t *e)
{
    if (floppies_ready()) {
        set_floppy_status("Disk images (.ST, .MSA) come from the \"" CONFIG_ST_FLOPPY_FOLDER
                          "\" folder on the SD card.", COLOR_DIM);
    } else {
        set_floppy_status("No SD card: no floppy disks.", COLOR_WARN);
    }
    update_floppy();
    lv_screen_load(ui.floppy_screen);
}

static void image_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int d = ui.images_drive;
    const char *name = ui.images[i];
    /* One image in one drive at a time: move it */
    const char *other = floppies_name(!d);
    if (other != NULL && strcmp(other, name) == 0) {
        floppies_eject(!d);
    }
    char text[160];
    if (floppies_insert(d, name)) {
        snprintf(text, sizeof(text), "%.100s is now in drive %c:", name, 'A' + d);
        set_floppy_status(text, COLOR_OK);
    } else {
        snprintf(text, sizeof(text), "%.100s is not a disk image the ST can use.", name);
        set_floppy_status(text, COLOR_WARN);
    }
    update_floppy();
    lv_screen_load(ui.floppy_screen);
}

static void insert_clicked(lv_event_t *e)
{
    int d = ui.images_drive = (int)(intptr_t)lv_event_get_user_data(e);
    floppies_free_list(ui.images, ui.image_count);
    ui.image_count = floppies_list(&ui.images);

    lv_label_set_text_fmt(ui.images_title, "Insert into drive %c:", 'A' + d);
    lv_label_set_text_fmt(ui.images_count, "%d disk%s", ui.image_count,
                          ui.image_count == 1 ? "" : "s");
    lv_obj_clean(ui.images_list);
    for (int i = 0; i < ui.image_count; i++) {
        const char *name = ui.images[i];
        char text[128];
        int in = -1;
        for (int k = 0; k < 2; k++) {
            if (floppies_name(k) != NULL && strcmp(floppies_name(k), name) == 0) {
                in = k;
            }
        }
        if (in >= 0) {
            snprintf(text, sizeof(text), "%.100s   (in %c:)", name, 'A' + in);
        } else {
            snprintf(text, sizeof(text), "%.100s", name);
        }
        lv_obj_t *b = add_row(ui.images_list, LV_SYMBOL_SAVE, text);
        lv_obj_add_event_cb(b, image_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (ui.image_count == 0) {
        lv_obj_t *l = lv_label_create(ui.images_list);
        lv_label_set_text(l, "No disk images yet. Copy .ST or .MSA files into the \""
                             CONFIG_ST_FLOPPY_FOLDER "\" folder on the SD card,\n"
                             "or make a new empty disk.");
        lv_obj_set_style_text_color(l, COLOR_DIM, 0);
        lv_obj_set_style_pad_all(l, 16, 0);
    }
    lv_screen_load(ui.images_screen);
}

static void new_clicked(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    char name[32], text[96];
    if (floppies_create(d, name, sizeof(name))) {
        snprintf(text, sizeof(text), "New empty disk %s is now in drive %c:", name, 'A' + d);
        set_floppy_status(text, COLOR_OK);
    } else {
        set_floppy_status("Couldn't make a new disk on the SD card.", COLOR_WARN);
    }
    update_floppy();
}

static void eject_clicked(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    floppies_eject(d);
    char text[48];
    snprintf(text, sizeof(text), "Drive %c: is now empty", 'A' + d);
    set_floppy_status(text, COLOR_DIM);
    update_floppy();
}

static void build_floppy_screens(void)
{
    lv_obj_t *s = ui.floppy_screen = make_screen();
    make_title(s, LV_SYMBOL_SAVE "  Floppy disks");
    ui.floppy_status = lv_label_create(s);
    lv_obj_set_width(ui.floppy_status, lv_pct(100));
    lv_label_set_long_mode(ui.floppy_status, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.floppy_status, LV_ALIGN_TOP_LEFT, 0, 44);

    static const char *const texts[3] = {
        LV_SYMBOL_DOWNLOAD "  Insert disk", LV_SYMBOL_PLUS "  New empty disk", LV_SYMBOL_EJECT "  Eject",
    };
    static const lv_event_cb_t cbs[3] = { insert_clicked, new_clicked, eject_clicked };
    static const int widths[3] = { 260, 300, 200 };
    for (int d = 0; d < 2; d++) {
        lv_obj_t *card = lv_obj_create(s);
        lv_obj_set_size(card, lv_pct(100), 160);
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 90 + d * 176);
        lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        ui.drive_label[d] = lv_label_create(card);
        lv_obj_set_width(ui.drive_label[d], lv_pct(100));
        lv_label_set_long_mode(ui.drive_label[d], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(ui.drive_label[d], &lv_font_montserrat_28, 0);
        lv_obj_align(ui.drive_label[d], LV_ALIGN_TOP_LEFT, 0, 0);

        int x = 0;
        for (int b = 0; b < 3; b++) {
            lv_obj_t *btn = make_button_for(card, texts[b], cbs[b], widths[b], (void *)(intptr_t)d);
            lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, x, 0);
            x += widths[b] + 16;
            ui.drive_buttons[d][b] = btn;
        }
    }
    lv_obj_t *back = make_button(s, LV_SYMBOL_LEFT "  Back", back_to_status, 300);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(back, COLOR_CARD, 0);
#if CONFIG_ST_WEB
    lv_obj_t *web = lv_label_create(s);
    lv_label_set_text(web, "Add disks from a computer:\nhttp://" CONFIG_ST_HOSTNAME ".local");
    lv_obj_set_style_text_color(web, COLOR_DIM, 0);
    lv_obj_set_style_text_align(web, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(web, LV_ALIGN_BOTTOM_RIGHT, 0, -6);
#endif

    s = ui.images_screen = make_screen();
    ui.images_title = make_title(s, "");
    ui.images_count = lv_label_create(s);
    lv_obj_set_style_text_color(ui.images_count, COLOR_DIM, 0);
    lv_obj_align(ui.images_count, LV_ALIGN_TOP_RIGHT, 0, 6);
    ui.images_list = make_list(s, 56, 380);
    back = make_button(s, LV_SYMBOL_LEFT "  Back", show_floppy, 300);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(back, COLOR_CARD, 0);
}

/* ---- main loop -------------------------------------------------------------- */

bool setup_ui_run(uint16_t *fb, const char *drive_info, bool running)
{
    memset(&ui, 0, sizeof(ui));
    ui.fb = fb;
    ui.running = running;
    ui.run_tos = settings.tos;
    ui.run_ram_kb = settings.ram_kb;

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    size_t buf_size = BOARD_LCD_H_RES * BUF_LINES * sizeof(uint16_t);
    void *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    build_status_screen(drive_info);
    build_list_screen();
    build_pass_screen();
#if CONFIG_ST_BLE_HID
    build_bt_screens();
#endif
    build_floppy_screens();
    lv_screen_load(ui.status_screen);

    int64_t next_update = 0;
    while (!ui.done) {
        lv_timer_handler();
        if (esp_timer_get_time() >= next_update) {
            update_status();
            update_networks();
#if CONFIG_ST_BLE_HID
            update_bluetooth();
#endif
            next_update = esp_timer_get_time() + 250000;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#if CONFIG_ST_BLE_HID
    ble_hid_set_pairing(false);
#endif

    floppies_free_list(ui.images, ui.image_count);

    /* Hand the panel back: clear it and shut LVGL down */
    lv_deinit();
    free(buf);
    memset(fb, 0, BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t));
    return ui.reset;
}
