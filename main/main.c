/*
 * Atari STE emulator on the Makerfabs MaTouch ESP32-S3 7" or the M5Stack Tab5.
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, setup screen, web
 * page, console) is the shared runtime in ../esp32emu; this file plugs the
 * STE into it: memory, TOS, settings, keyboard map, disks and status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "emu/console.h"
#include "emu/emu.h"
#include "emu/sdcard.h"
#include "settings.h"
#include "st.h"
#include "tos_rom.h"

static const char *TAG = "main";

static st_config_t s_cfg;
static uint16_t s_tos_version;      /* of the running TOS, e.g. 0x0206 */

/* ---- Memory ----
 *
 * The ST's RAM and ROM copy, first thing at power-on: 4 MB must be one
 * block of PSRAM, which it no longer is once the screen, WiFi and Bluetooth
 * have taken their pieces. PSRAM the rest of the board needs: screen buffer
 * (1.2 MB), WiFi, Bluetooth, the setup screen: 1.7 MB measured at most,
 * plus room.
 */
#define PSRAM_FOR_THE_REST      (2048 * 1024)

static uint16_t s_ram_limit_kb = 512;      /* the most ST RAM that fits */

/* ST RAM to use: the setting, or less if that doesn't fit */
static uint32_t ram_wanted(void)
{
    uint16_t kb = settings.ram_kb < s_ram_limit_kb ? settings.ram_kb : s_ram_limit_kb;
    return kb * 1024u;
}

static void reserve_memory(void)
{
    settings_load();

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

#if CONFIG_ST_MONO
    s_cfg.monitor = ST_MONITOR_MONO;
#else
    s_cfg.monitor = ST_MONITOR_COLOR;
#endif
#if CONFIG_ST_ANY_RESOLUTION
    s_cfg.any_resolution = true;
#endif
#if CONFIG_ST_HD_BOOT
    s_cfg.hd_boot = true;
#endif
}

/* The SD card is drive C: (all of it, or a folder), or the drive after a
 * hard disk image's partitions when one boots
 */
static void storage(bool sd)
{
    const char *hd = NULL;
#if CONFIG_ST_HD
    static char hd_path[128];
    if (sd) {
        snprintf(hd_path, sizeof(hd_path), "%s%s%s", SDCARD_MOUNT_POINT,
                 CONFIG_ST_HD_FOLDER[0] ? "/" : "", CONFIG_ST_HD_FOLDER);
        hd = hd_path;
    }
#endif
    s_cfg.hd_path = hd;
    emu_set_drive_info(hd ? "Hard disk C: on the SD card (D: or later if an ACSI image boots)" :
                       sd ? "SD card: disk images only" : "No SD card: no hard disk or disk images");
}

/* For the ST's Dfree() on drive C: */
bool st_host_disk_free(const char *path, uint64_t *total, uint64_t *free_bytes)
{
    return sdcard_space(total, free_bytes);
}

/* ---- Free space, for the web page ----
 *
 * A floppy's FAT is counted where its image is safe to read: at insert (the
 * emulation is paused then) and on the emulation task a second after the
 * drive's last write (each one goes straight through to the card). C: is
 * the SD card's, asked in every_second().
 */
typedef struct {
    volatile bool known;
    volatile uint64_t free, size;
    uint32_t writes;                /* the drive's write count when counted */
    bool waiting;                   /* written since: once the writes stop */
} space_t;

static space_t s_space[3];          /* A:, B:, C: */

static void space_set(int d, bool known, uint64_t free_bytes, uint64_t size)
{
    s_space[d].known = false;
    s_space[d].free = free_bytes;
    s_space[d].size = size;
    s_space[d].known = known;
}

/* The figures for the web page: the size, 0 if not known */
static uint64_t space_get(int d, uint64_t *free_bytes)
{
    *free_bytes = s_space[d].free;
    uint64_t size = s_space[d].size;
    return s_space[d].known ? size : 0;
}

static void floppy_count(int d)
{
    st_floppy_status_t fs;
    st_floppy_status(&fs);
    s_space[d].writes = fs.writes[d];
    s_space[d].waiting = false;
    uint64_t f = 0, size = 0;
    bool known = st_floppy_space(d, &f, &size);
    space_set(d, known, f, size);
}

/* On the emulation task, between frames */
static void floppy_space_check(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (now - last < 1000000) {
        return;
    }
    last = now;
    st_floppy_status_t fs;
    st_floppy_status(&fs);
    for (int d = 0; d < 2; d++) {
        if (fs.writes[d] != s_space[d].writes) {
            s_space[d].writes = fs.writes[d];
            s_space[d].waiting = true;      /* (the next second, if it stays so) */
        } else if (s_space[d].waiting) {
            floppy_count(d);
        }
    }
}

static bool floppy_insert(int drive, const char *path)
{
    bool ok = st_floppy_insert(drive, path);
    floppy_count(drive);
    return ok;
}

static void floppy_eject(int drive)
{
    st_floppy_eject(drive);
    space_set(drive, false, 0, 0);
}

/* GEM (re)started in a resolution: remembered for the next boot. Called
 * from the emulation task; the NVS write happens in every_second().
 */
static volatile int8_t s_desktop_rez = -2;

static void resolution_changed(int rez)
{
    s_desktop_rez = rez;
}

static void every_second(void)
{
    int8_t rez = s_desktop_rez;
    if (rez >= 0 && rez != settings.resolution) {
        settings.resolution = rez;
        settings_save();
    }
    uint64_t total, free_bytes;
    if (s_cfg.hd_path != NULL && sdcard_space(&total, &free_bytes)) {
        space_set(2, true, free_bytes, total);
    }
}

/* ---- 68000 speed ----
 *
 * 8 or 16 MHz. The MaTouch runs at 8 MHz only (SETTINGS_CPU_CHOICE).
 */
#define ST_MHZ(speed)   (8.010613f * (speed) / ST_CPU_SPEED_8MHZ)

/* The emulation task sets the speed before each frame: the setting may
 * change on another task (setup screen, console) at any time
 */
static void run_frame(void)
{
    floppy_space_check();
    st_set_cpu_speed(settings.cpu == SETTINGS_CPU_16MHZ ? 2 * ST_CPU_SPEED_8MHZ : ST_CPU_SPEED_8MHZ);
    st_run_frame();
}

/* (Re)start the ST with the current settings (TOS, RAM); the emulation is
 * paused or not started yet
 */
static bool start(void)
{
    tos_rom_t tos;
    if (tos_rom_map(settings_tos_partition(), &tos) != ESP_OK) {
        emu_set_message(settings.tos ? "No TOS 2.06 in flash: flash roms/tos206.img with idf.py flash."
                                     : "No TOS 1.62 in flash: flash roms/tos162.img with idf.py flash.");
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
            emu_reboot_into_machine();
        }
        s_cfg.ram_size = ram_size;
    }
    s_cfg.tos = tos.data;
    s_cfg.tos_size = tos.size;
    s_cfg.start_resolution = settings.resolution;
    s_cfg.resolution_changed = resolution_changed;

    bool ok = s_cfg.ram != NULL && s_cfg.rom != NULL && st_init(&s_cfg);
    if (ok) {
        s_tos_version = tos.version;
    }
    tos_rom_unmap(&tos);    /* st_init copied it */
    s_cfg.tos = NULL;
    if (!ok) {
        ESP_LOGE(TAG, "ST setup failed (%u KB RAM)", (unsigned)settings.ram_kb);
    }
    return ok;
}

static bool halted(void)
{
    st_status_t s;
    st_get_status(&s);
    return s.halted;
}

static void screen_mode(emu_screen_t *mode)
{
    st_screen_mode_t m;
    st_get_screen_mode(&m);
    mode->width = m.width;
    mode->height = m.height;
    mode->hz = m.hz;
}

/* ---- Keyboard and mouse ---- */

/* HID keyboard usages 0x00-0x67 (US positions). 0 = no ST key. */
static const uint8_t usage_to_st[0x68] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,     /* a b c d */
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,     /* e f g h */
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,     /* i j k l */
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,     /* m n o p */
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,     /* q r s t */
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,     /* u v w x */
    [0x1C] = 0x15, [0x1D] = 0x2C,                                   /* y z */
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05,     /* 1 2 3 4 */
    [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,     /* 5 6 7 8 */
    [0x26] = 0x0A, [0x27] = 0x0B,                                   /* 9 0 */
    [0x28] = 0x1C,      /* Return */
    [0x29] = 0x01,      /* Esc */
    [0x2A] = 0x0E,      /* Backspace */
    [0x2B] = 0x0F,      /* Tab */
    [0x2C] = 0x39,      /* Space */
    [0x2D] = 0x0C,      /* - */
    [0x2E] = 0x0D,      /* = */
    [0x2F] = 0x1A,      /* [ */
    [0x30] = 0x1B,      /* ] */
    [0x31] = 0x2B,      /* \ */
    [0x32] = 0x2B,      /* non-US # */
    [0x33] = 0x27,      /* ; */
    [0x34] = 0x28,      /* ' */
    [0x35] = 0x29,      /* ` */
    [0x36] = 0x33,      /* , */
    [0x37] = 0x34,      /* . */
    [0x38] = 0x35,      /* / */
    [0x39] = 0x3A,      /* Caps Lock */
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,     /* F1-F4 */
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,     /* F5-F8 */
    [0x42] = 0x43, [0x43] = 0x44,                                   /* F9 F10 */
    [0x44] = 0x61,      /* F11 -> Undo */
    [0x45] = 0x62,      /* F12 -> Help */
    [0x49] = 0x52,      /* Insert */
    [0x4A] = 0x47,      /* Home -> Clr/Home */
    [0x4B] = 0x62,      /* Page Up -> Help */
    [0x4C] = 0x53,      /* Delete */
    [0x4E] = 0x61,      /* Page Down -> Undo */
    [0x4F] = 0x4D,      /* Right */
    [0x50] = 0x4B,      /* Left */
    [0x51] = 0x50,      /* Down */
    [0x52] = 0x48,      /* Up */
    [0x53] = 0x63,      /* Num Lock -> keypad ( */
    [0x54] = 0x65,      /* keypad / */
    [0x55] = 0x66,      /* keypad * */
    [0x56] = 0x4A,      /* keypad - */
    [0x57] = 0x4E,      /* keypad + */
    [0x58] = 0x72,      /* keypad Enter */
    [0x59] = 0x6D, [0x5A] = 0x6E, [0x5B] = 0x6F,                    /* keypad 1 2 3 */
    [0x5C] = 0x6A, [0x5D] = 0x6B, [0x5E] = 0x6C,                    /* keypad 4 5 6 */
    [0x5F] = 0x67, [0x60] = 0x68, [0x61] = 0x69,                    /* keypad 7 8 9 */
    [0x62] = 0x70,      /* keypad 0 */
    [0x63] = 0x71,      /* keypad . */
    [0x64] = 0x60,      /* non-US \ (ISO key left of Z) */
    [0x67] = 0x64,      /* keypad = -> keypad ) */
};

/* Modifier usages $E0-$E7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI */
static const uint8_t modifier_to_st[8] = { 0x1D, 0x2A, 0x38, 0, 0x1D, 0x36, 0x38, 0 };

static uint8_t s_modifiers;     /* modifier usages held, bit n = $E0 + n */

static void hid_key(uint8_t usage, bool pressed)
{
    if (usage >= 0xE0 && usage <= 0xE7) {
        /* ST Control and Alternate have one key each: down while either side is */
        int bit = usage - 0xE0;
        uint8_t code = modifier_to_st[bit];
        uint8_t mask = 0;
        for (int i = 0; i < 8; i++) {
            mask |= modifier_to_st[i] == code ? 1u << i : 0;
        }
        bool was = (s_modifiers & mask) != 0;
        s_modifiers = pressed ? s_modifiers | (1u << bit) : s_modifiers & ~(1u << bit);
        bool now = (s_modifiers & mask) != 0;
        if (code && was != now) {
            st_ikbd_key(code, now);
        }
        return;
    }
    if (usage < sizeof(usage_to_st) && usage_to_st[usage]) {
        st_ikbd_key(usage_to_st[usage], pressed);
    }
}

/* A Bluetooth mouse, and the touch screen as a touchpad (the runtime's:
 * its moves come in the ST's pixels, and GEM moves the pointer a pixel a
 * count in every resolution)
 */
static void hid_mouse(int dx, int dy, uint8_t buttons)
{
    st_ikbd_mouse(dx, dy, buttons & 0x01, buttons & 0x02);
}

/* ---- Setup screen choices ---- */

static const char *const tos_names[] = { "TOS 1.62", "TOS 2.06", "" };
static const char *const ram_names[] = { "512 KB", "1 MB", "4 MB", "" };
static const uint16_t ram_sizes[] = { 512, 1024, 4096 };

static int tos_get(void)
{
    return settings.tos;
}

static void tos_set(int i)
{
    settings.tos = i ? 1 : 0;
    settings_save();
}

static int ram_get(void)
{
    return settings.ram_kb >= 4096 ? 2 : settings.ram_kb >= 1024 ? 1 : 0;
}

static void ram_set(int i)
{
    if (i >= 0 && i < 3) {
        settings.ram_kb = ram_sizes[i];
        settings_save();
    }
}

#if SETTINGS_CPU_CHOICE
/* The 68000's speed: changed right away, no restart */
static const char *const cpu_names[] = { "8 MHz", "16 MHz", "" };

static int cpu_get(void)
{
    return settings.cpu;
}

static void cpu_set(int i)
{
    if (i >= SETTINGS_CPU_8MHZ && i <= SETTINGS_CPU_16MHZ) {
        settings.cpu = i;       /* run_frame() takes it from here */
        settings_save();
    }
}
#endif

static const emu_choice_t choices[] = {
    { "System", tos_names, tos_get, tos_set, false },
    { "Memory", ram_names, ram_get, ram_set, false },
#if SETTINGS_CPU_CHOICE
    { "CPU", cpu_names, cpu_get, cpu_set, true },        /* live */
#endif
};

/* ---- Disks: floppies in A: and B:, hard disk images on ACSI 0 and 1 ---- */

#define FLOPPIES        2
#define DRIVES          (FLOPPIES + ST_ACSI_DEVICES)

static const char *const extensions[] = { ".st", ".msa", ".img", ".hd", NULL };
static const char *const drive_names[DRIVES] = { "A:", "B:", "ACSI 0", "ACSI 1" };
static const disks_size_t sizes[] = { { 737280, "720 KB" } };

static bool hard_disk_name(const char *name)
{
    size_t n = strlen(name);
    return (n > 4 && !strcasecmp(name + n - 4, ".img")) || (n > 3 && !strcasecmp(name + n - 3, ".hd"));
}

static bool disk_accepts(int drive, const char *name)
{
    return (drive >= FLOPPIES) == hard_disk_name(name);
}

/* Blank disks are floppies (an empty hard disk needs partitioning and a
 * driver first)
 */
static bool disk_takes_blank(int drive)
{
    return drive < FLOPPIES;
}

static bool disk_insert(int drive, const char *path)
{
    if (drive >= FLOPPIES) {
        return st_acsi_insert(drive - FLOPPIES, path);
    }
    return floppy_insert(drive, path);
}

static void disk_eject(int drive)
{
    if (drive >= FLOPPIES) {
        st_acsi_eject(drive - FLOPPIES);
    } else {
        floppy_eject(drive);
    }
}

static const char *disk_path(int drive)
{
    return drive >= FLOPPIES ? st_acsi_path(drive - FLOPPIES) : st_floppy_path(drive);
}

static bool disk_write_protected(int drive)
{
    return drive >= FLOPPIES ? st_acsi_write_protected(drive - FLOPPIES) : st_floppy_write_protected(drive);
}

static bool floppy_create(const char *path, uint32_t bytes)
{
    return st_floppy_create(path);
}

/* An .MSA image's unpacked copy goes with it */
static void floppy_removed(const char *path)
{
    char copy[200];
    st_floppy_unpacked_path(path, copy, sizeof(copy));
    remove(copy);
}

static const disks_config_t disks = {
    .folder = "st",
    .extensions = extensions,
    .description = "Disk images (.ST, .MSA; .IMG, .HD hard disks)",
    .title = "Disks",
    .button = "Disks",
    .nvs_namespace = "floppy",
    .drives = DRIVES,
    .drive_names = drive_names,
    .new_prefix = "EMPTY",
    .new_extension = ".ST",
    .sizes = sizes,
    .size_count = 1,
    .max_upload = 256 * 1024 * 1024,
    .insert = disk_insert,
    .eject = disk_eject,
    .path = disk_path,
    .write_protected = disk_write_protected,
    .create = floppy_create,
    .removed = floppy_removed,
    .accepts = disk_accepts,
    .takes_blank = disk_takes_blank,
};

/* ---- Status ----
 *
 * The machine card on the web page: the chips' state now, from the core's
 * status getters (plain values, read on the web server's task). The whole
 * /api/status reply has 4 KB (esp32emu's STATUS_SIZE): the rows are short.
 */

static uint32_t peek32(uint32_t address)
{
    return ((uint32_t)st_peek16(address) << 16) | st_peek16(address + 2);
}

/* An MFP timer: its rate, or what it counts */
static void timer_text(char *buf, size_t len, const st_mfp_status_t *m, int t)
{
    static const uint16_t prescale[8] = { 1, 4, 10, 16, 50, 64, 100, 200 };
    unsigned mode = m->timer_mode[t], data = m->timer_data[t] ? m->timer_data[t] : 256;
    char name = "ABCD"[t];
    if (mode == 0) {
        snprintf(buf, len, "Timer %c (stopped)", name);
    } else if (mode == 8 && t == 1) {
        snprintf(buf, len, data == 1 ? "Timer B each line" : "Timer B every %u lines", data);
    } else if (mode == 8) {
        snprintf(buf, len, "Timer %c every %u sound frames", name, data);
    } else {
        snprintf(buf, len, "Timer %c %lu Hz", name, (2457600ul / prescale[mode & 7] + data / 2) / data);
    }
}

static void status_rows(emu_json_t *j)
{
    static char text[1024];             /* (the web server's task: not on its stack) */
    size_t n;
    st_status_t s;
    st_get_status(&s);

    uint16_t ram_kb = s_cfg.ram_size / 1024;
    char ram[16];
    snprintf(ram, sizeof(ram), ram_kb >= 1024 ? "%u MB" : "%u KB", ram_kb >= 1024 ? ram_kb / 1024 : ram_kb);
    float mhz = ST_MHZ(st_cpu_speed());
    snprintf(text, sizeof(text), "Atari STE: a 68000 at %.0f MHz%s, %s RAM, blitter, YM2149, DMA sound", mhz,
             mhz > 8.5f ? " (8 MHz bus)" : "", ram);
    emu_json_row(j, "System", text);

    /* TOS's header: os_date $MMDDYYYY at $18, os_conf (country << 1 | PAL) at $1C */
    static const char *const countries[] = {
        "US", "German", "French", "UK", "Spanish", "Italian", "Swedish", "Swiss French", "Swiss German",
        "Turkish", "Finnish", "Norwegian", "Danish", "Saudi", "Dutch", "Czech", "Hungarian",
    };
    unsigned country = st_peek16(s.rom_base + 0x1C) >> 1;
    snprintf(text, sizeof(text), "TOS %x.%02x (%s%s%x), %lu KB at $%06lX", s_tos_version >> 8, s_tos_version & 0xFF,
             country < sizeof(countries) / sizeof(countries[0]) ? countries[country] : "",
             country < sizeof(countries) / sizeof(countries[0]) ? ", " : "", st_peek16(s.rom_base + 0x1A),
             (unsigned long)(s.rom_size / 1024), (unsigned long)s.rom_base);
    emu_json_row(j, "ROM", text);

    /* The MMU's banks (%00 128 KB, %01 512 KB, %10 2 MB), TOS's program area */
    static const char *const banks[4] = { "128 KB", "512 KB", "2 MB", "?" };
    static const uint32_t bank_kb[4] = { 128, 512, 2048, 0 };
    unsigned bank0 = (s.mmu_config >> 2) & 3, bank1 = s.mmu_config & 3;
    if (ram_kb <= bank_kb[bank0]) {
        n = snprintf(text, sizeof(text), "MMU $%02X: one %s bank", s.mmu_config, banks[bank0]);
    } else if (bank0 == bank1) {
        n = snprintf(text, sizeof(text), "MMU $%02X: two %s banks", s.mmu_config, banks[bank0]);
    } else {
        n = snprintf(text, sizeof(text), "MMU $%02X: %s and %s banks", s.mmu_config, banks[bank0], banks[bank1]);
    }
    uint32_t membot = peek32(0x432), memtop = peek32(0x436);
    if (memtop != 0 && n < sizeof(text)) {
        snprintf(text + n, sizeof(text) - n, "; programs $%06lX-$%06lX", (unsigned long)membot, (unsigned long)memtop);
    }
    emu_json_row(j, "Memory", text);
    uint32_t ram_free, ram_largest;
    if (st_ram_free(&ram_free, &ram_largest)) {
        n = snprintf(text, sizeof(text), "%lu KB of %s", (unsigned long)(ram_free / 1024), ram);
        if (ram_largest < ram_free) {
            snprintf(text + n, sizeof(text) - n, " <span class=\"dim\">(largest block %lu KB)</span>",
                     (unsigned long)(ram_largest / 1024));
        }
        emu_json_row_bar(j, "Free RAM", text, 100 - (int)((uint64_t)ram_free * 100 / s_cfg.ram_size));
    }

    static const char *const modes[3] = {
        "Low resolution: 320 × 200, 16 colours", "Medium resolution: 640 × 200, 4 colours",
        "High resolution: 640 × 400, black and white",
    };
    st_video_status_t v;
    st_get_video_status(&v);
    unsigned mode = v.mode < 3 ? v.mode : 0;
    n = snprintf(text, sizeof(text), "%s, %u Hz, at $%06lX", modes[mode], v.hz, (unsigned long)v.base);
    if (v.hscroll && n < sizeof(text)) {
        n += snprintf(text + n, sizeof(text) - n, "; scrolled %u pixels", v.hscroll);
    }
    if (v.line_offset && n < sizeof(text)) {
        snprintf(text + n, sizeof(text) - n, "%s%u words skipped a line", v.hscroll ? ", " : "; ", v.line_offset);
    }
    emu_json_row(j, "Screen", text);

    /* The colours in use as swatches: 4 bits a gun, its lowest bit on top */
    uint16_t colours[16];
    int count = mode == 0 ? 16 : 4;
    if (mode == 2) {
        bool white_paper = v.palette[0] & 1;       /* (high resolution: palette 0 bit 0) */
        colours[0] = white_paper ? 0x0FFF : 0;
        colours[1] = white_paper ? 0 : 0x0FFF;
        count = 2;
        n = snprintf(text, sizeof(text), "%s: ", white_paper ? "Black on white" : "White on black (inverted)");
    } else {
        memcpy(colours, v.palette, sizeof(colours));
        n = snprintf(text, sizeof(text), "%d of 4096 colours: ", count);
    }
    for (int i = 0; i < count && n < sizeof(text); i++) {
        unsigned c = colours[i];
        unsigned r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
        n += snprintf(text + n, sizeof(text) - n, "<b style=\"background:#%x%x%x\">&nbsp;&nbsp;&nbsp;</b> ",
                      ((r & 7) << 1) | (r >> 3), ((g & 7) << 1) | (g >> 3), ((b & 7) << 1) | (b >> 3));
    }
    emu_json_row(j, "Palette", text);

    /* The YM2149's channels sounding (2 MHz / 16 / period), the DMA sound,
     * the LMC1992's volume
     */
    st_sound_status_t a;
    st_get_sound_status(&a);
    n = snprintf(text, sizeof(text), "YM2149");
    int sounding = 0;
    for (int c = 0; c < 3 && n < sizeof(text); c++) {
        bool tone = !(a.ym[7] & (1 << c)), noise = !(a.ym[7] & (8 << c));
        unsigned volume = a.ym[8 + c] & 0x1F;
        if ((!tone && !noise) || volume == 0) {
            continue;
        }
        unsigned period = a.ym[c * 2] | ((a.ym[c * 2 + 1] & 15) << 8);
        char what[32], level[16];
        if (tone) {
            snprintf(what, sizeof(what), "%u Hz%s", 125000 / (period ? period : 1), noise ? " + noise" : "");
        } else {
            snprintf(what, sizeof(what), "noise");
        }
        snprintf(level, sizeof(level), volume & 0x10 ? "envelope" : "volume %u", volume);
        n += snprintf(text + n, sizeof(text) - n, "%s %c %s, %s", sounding++ ? ";" : ":", 'A' + c, what, level);
    }
    if (!sounding && n < sizeof(text)) {
        n += snprintf(text + n, sizeof(text) - n, " silent");
    }
    if ((a.dma_control & 1) && n < sizeof(text)) {
        static const char *const rates[4] = { "6.3", "12.5", "25", "50" };
        n += snprintf(text + n, sizeof(text) - n, "; DMA %s kHz %s%s", rates[a.dma_mode & 3],
                      (a.dma_mode & 0x80) ? "mono" : "stereo", (a.dma_control & 2) ? ", looping" : "");
    } else if (n < sizeof(text)) {
        n += snprintf(text + n, sizeof(text) - n, "; DMA off");
    }
    if ((a.master_db || a.left_db || a.right_db) && n < sizeof(text)) {
        n += snprintf(text + n, sizeof(text) - n, "; volume %d dB (L %d, R %d)", a.master_db, a.left_db,
                      a.right_db);
    }
    if (a.mix != 1 && n < sizeof(text)) {
        snprintf(text + n, sizeof(text) - n, a.mix == 2 ? "; YM off" : "; YM at -12 dB");
    }
    emu_json_row(j, "Sound", text);

    /* What the 68000 takes (HBL 2, VBL 4, the MFP 6) and the MFP's
     * enabled channels; the RS-232 ones as one
     */
    static const char *const channels[16] = {
        "printer", NULL, NULL, "blitter", NULL, NULL, "keyboard/MIDI", "disk",
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, "mono/DMA sound",
    };
    static const int8_t channel_timer[16] = { -1, -1, -1, -1, 3, 2, -1, -1, 1, -1, -1, -1, -1, 0, -1, -1 };
    st_mfp_status_t m;
    st_get_mfp_status(&m);
    unsigned ipl = (s.sr >> 8) & 7;
    n = snprintf(text, sizeof(text), "Mask %u (%s); MFP:", ipl,
                 ipl < 2 ? "HBL, VBL, MFP on" : ipl < 4 ? "VBL, MFP on" : ipl < 6 ? "MFP on" : "all off");
    int listed = 0;
    for (int ch = 0; ch < 16 && n < sizeof(text); ch++) {
        if (!(m.enabled & (1u << ch))) {
            continue;
        }
        char timer[48];
        const char *name = channels[ch];
        if (channel_timer[ch] >= 0) {
            timer_text(timer, sizeof(timer), &m, channel_timer[ch]);
            name = timer;
        }
        if (name) {
            n += snprintf(text + n, sizeof(text) - n, "%s %s", listed++ ? "," : "", name);
        }
    }
    if ((m.enabled & 0x5E06) && n < sizeof(text)) {
        n += snprintf(text + n, sizeof(text) - n, "%s RS-232", listed++ ? "," : "");
    }
    if (!listed && n < sizeof(text)) {
        snprintf(text + n, sizeof(text) - n, " none");
    }
    emu_json_row(j, "Interrupts", text);

    emu_json_row(j, "Keyboard", "By position, in TOS's layout; Alt Alternate, F11/Page Down Undo, F12/Page Up Help, "
                 "Home Clr/Home, Num Lock (, keypad = )");

    /* The keyboard processor: the mouse and joysticks as TOS asked, its clock */
    static const char *const mouse_modes[4] = { "relative", "absolute", "as cursor keys", "off" };
    static const char *const joystick_modes[3] = { "as events", "when asked", "off" };
    st_ikbd_status_t k;
    st_get_ikbd_status(&k);
    time_t when = (time_t)k.clock;
    struct tm tm;
    localtime_r(&when, &tm);
    snprintf(text, sizeof(text), "Mouse %s%s; joysticks %s%s; clock %04d-%02d-%02d %02d:%02d:%02d",
             mouse_modes[k.mouse_mode & 3],
             (k.buttons & 3) == 3 ? ", both buttons down" : (k.buttons & 2) ? ", left button down" :
             (k.buttons & 1) ? ", right button down" : "",
             joystick_modes[k.joystick_mode < 3 ? k.joystick_mode : 2], k.paused ? "; output paused" : "",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    emu_json_row(j, "IKBD", text);

    n = snprintf(text, sizeof(text), "A:, B: WD1772 (.ST; .MSA read-only); ACSI 0, 1 (.IMG, .HD)");
    if (s_cfg.hd_path != NULL) {
        snprintf(text + n, sizeof(text) - n, "; %c: the SD card (GEMDOS)", st_hd_drive());
    }
    emu_json_row(j, "Disks", text);
}

static void status(emu_json_t *j, int section)
{
    char text[160];
    if (section == EMU_STATUS_ROWS) {
        status_rows(j);
        return;
    }
    st_floppy_status_t fs;
    st_floppy_status(&fs);
    for (int d = 0; d < FLOPPIES; d++) {
        const char *name = disks_name(d);
        snprintf(text, sizeof(text), "track %d", fs.track[d]);
        uint64_t free_bytes, size = space_get(d, &free_bytes);
        emu_json_drive_space(j, drive_names[d], name, st_floppy_write_protected(d), fs.light == d,
                             fs.reads[d], fs.writes[d], name ? text : NULL, free_bytes, size);
    }
    st_acsi_status_t as;
    st_acsi_status(&as);
    for (int i = 0; i < ST_ACSI_DEVICES; i++) {
        const char *name = disks_name(FLOPPIES + i);
        if (name == NULL) {
            continue;
        }
        uint64_t bytes = st_acsi_size(i);
        snprintf(text, sizeof(text), "%llu %s hard disk", (unsigned long long)(bytes >> (bytes >= 1 << 20 ? 20 : 10)),
                 bytes >= 1 << 20 ? "MB" : "KB");
        emu_json_drive_space(j, drive_names[FLOPPIES + i], name, st_acsi_write_protected(i), as.light == i,
                             as.reads[i], as.writes[i], text, 0, 0);
    }
    if (s_cfg.hd_path != NULL) {
        char last[100], letter[3] = { st_hd_drive(), ':', 0 };
        const char *verb = st_hd_last_file(last, sizeof(last));
        if (verb) {
            snprintf(text, sizeof(text), "last %s: %s", verb, last);
        }
        uint64_t free_bytes, size = space_get(2, &free_bytes);
        emu_json_drive_space(j, letter, "SD card", false, false, 0, 0, verb ? text : "no files used yet",
                             free_bytes, size);
    }
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    st_status_t s;
    st_get_status(&s);
    uint32_t v_bas_ad = ((uint32_t)st_peek16(0x44E) << 16) | st_peek16(0x450);
    snprintf(buf, len, "frames %lu PC $%06lx SR $%04x hz_200 %lu v_bas_ad $%06lx bus errors %lu cpu %.1f MHz",
             (unsigned long)s.frames, (unsigned long)s.pc, s.sr, (unsigned long)s.hz_200,
             (unsigned long)v_bas_ad, (unsigned long)s.bus_errors, ST_MHZ(st_cpu_speed()));
}

/*   k <hex scancode>   tap a key
 *   m <dx> <dy> <btn>  move the mouse, btn bit 0 left, bit 1 right
 *   R <tos> <ram KB>   restart with TOS 0 = 1.62, 1 = 2.06 and that much RAM
 *   C <cpu>            68000 speed: 0 = 8 MHz, 1 = 16 MHz (Tab5)
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'k': {
        unsigned code = strtoul(line + 1, NULL, 16);
        st_ikbd_key(code, true);
        vTaskDelay(pdMS_TO_TICKS(60));
        st_ikbd_key(code, false);
        return true;
    }
    case 'm': {
        int dx = 0, dy = 0, buttons = 0;
        sscanf(line + 1, "%d %d %d", &dx, &dy, &buttons);
        st_ikbd_mouse(dx, dy, buttons & 1, buttons & 2);
        return true;
    }
#if SETTINGS_CPU_CHOICE
    case 'C':
        cpu_set(atoi(line + 1));
        return true;
#endif
    case 'R': {
        int tos = 0, ram_kb = 0;
        if (sscanf(line + 1, "%d %d", &tos, &ram_kb) == 2 && (tos == 0 || tos == 1) &&
            (ram_kb == 512 || ram_kb == 1024 || ram_kb == 2048 || ram_kb == 4096)) {
            settings.tos = tos;
            settings.ram_kb = ram_kb;
            settings_save();
            emu_restart();
        }
        return true;
    }
    default:
        return false;
    }
}

/* ---- The machine ---- */

static const emu_machine_t machine = {
    .name = "Atari STE",
    .short_name = "Atari",
    .view_width = 960,
    .scale = EMU_SCALE_NEAREST,
    .reserve_memory = reserve_memory,
    .storage = storage,
    .start = start,
    .reset = st_reset,
    .run_frame = run_frame,
    .halted = halted,
    .screen_mode = screen_mode,
    .screen_signature = st_screen_signature,
    .render_line = st_render_line_rgb565,
    .audio_hz = ST_AUDIO_HZ,
    .audio_enable = st_audio_enable,
    .audio_render = st_audio_render,
    .audio_pending = st_audio_pending,
    .hid_key = hid_key,
    .hid_mouse = hid_mouse,
    .touchpad = hid_mouse,
    .choices = choices,
    .choice_count = sizeof(choices) / sizeof(choices[0]),
    .disks = &disks,
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    emu_run(&machine);
}
