/*
 * Atari STE machine: 68000 (Musashi), memory map, MFP 68901, GLUE/MMU,
 * shifter, WD1772 + DMA with two floppy drives, ACSI hard disks, YM2149,
 * ACIAs, blitter, DMA sound.
 *
 * Platform independent: the caller provides all memory and drives the
 * emulation one video frame at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ST_MONITOR_COLOR,       /* SC1224: low/medium resolution */
    ST_MONITOR_MONO,        /* SM124: high resolution */
} st_monitor_t;

#define ST_OPCODE_TABLES_SIZE   0x50000

typedef struct {
    uint8_t *ram;           /* ram_size bytes */
    uint32_t ram_size;      /* 512 KB, 1, 2, 2.5 or 4 MB */
    const uint8_t *tos;     /* TOS image as dumped (big-endian) */
    uint32_t tos_size;      /* 192 KB (at $FC0000) or 256 KB (at $E00000) */
    uint8_t *rom;           /* tos_size bytes: working copy of the ROM */
    st_monitor_t monitor;   /* monitor at power-on */

    /* Offer Low, Medium and High resolution whatever the monitor: the
     * desktop's Set Preferences enables all three (TOS patch), and the
     * monitor follows the mode TOS sets, like a multisync monitor.
     */
    bool any_resolution;

    /* Host folder shown to the ST as drive C: (GEMDOS level, no disk image
     * or driver needed), NULL for none. With hd_boot, C: is the boot drive:
     * AUTO folder and DESKTOP.INF come from there, unless a disk is in
     * drive A: or a hard disk image boots (then the folder is the drive
     * after its partitions, st_hd_drive()).
     */
    const char *hd_path;
    bool hd_boot;

    bool no_blitter;        /* leave out the blitter (TOS then draws in software) */

    /* ST_OPCODE_TABLES_SIZE bytes for copies of the CPU core's opcode tables,
     * or NULL: on a host where the built-in ones are slow to read (ESP32
     * with read-only data in flash, this in PSRAM)
     */
    void *opcode_tables;

    /* With any_resolution: 0 Low, 1 Medium, 2 High to start the desktop in
     * (a DESKTOP.INF still overrides it), -1 for TOS's default. GEM reports
     * the resolution it (re)starts in through resolution_changed, called
     * from the emulation task, so it can be remembered.
     */
    int start_resolution;
    void (*resolution_changed)(int resolution);
} st_config_t;

typedef struct {
    int width;              /* 320, 640 */
    int height;             /* 200, 400 */
    int planes;             /* 4, 2, 1 */
    int hz;                 /* 50, 60, 71 */
} st_screen_mode_t;

typedef struct {
    uint32_t frames;        /* video frames emulated */
    uint32_t pc;
    uint16_t sr;
    uint32_t hz_200;        /* TOS _hz_200 counter */
    uint32_t vbclock;       /* TOS _vbclock counter */
    uint32_t bus_errors;
    bool halted;            /* CPU stopped by a double bus fault until a reset */
    uint8_t mmu_config;     /* $FF8001: bank 0 in bits 3-2, bank 1 in 1-0 */
    uint32_t rom_base;      /* TOS: $FC0000 (192 KB) or $E00000 (256 KB) */
    uint32_t rom_size;
} st_status_t;

/* Set up the machine and cold-reset it. False if the TOS image is unusable. */
bool st_init(const st_config_t *config);

/* Cold reset: clears RAM, as power-on */
void st_reset(void);

/* Emulate one video frame (VBL to VBL) */
void st_run_frame(void);

/* The 68000's speed in 1/256 of the STE's 8 MHz: 256 is an STE (the
 * default after st_init()), 512 16 MHz. Only the CPU goes faster, like an
 * accelerator board: video, timers, sound, the blitter and the floppy keep
 * the STE's timing. Between frames.
 */
#define ST_CPU_SPEED_8MHZ   256
void st_set_cpu_speed(unsigned speed);
unsigned st_cpu_speed(void);

void st_get_status(st_status_t *status);

/* ---- The chips' state, for a status page ----
 *
 * Plain values copied from the emulation, safe to call from another task
 * (they may be a frame old); no side effects.
 */
typedef struct {
    uint8_t mode;           /* $FF8260 as this frame shows it: 0 low, 1 medium, 2 high */
    uint8_t hz;             /* 50, 60, 71 */
    uint32_t base;          /* this frame's video base address */
    uint8_t line_offset;    /* $FF820F: words skipped after each line */
    uint8_t hscroll;        /* $FF8265: pixels scrolled */
    uint16_t palette[16];   /* $FF8240-$FF825E: $0RGB, each nibble's bit 3 its lowest bit */
} st_video_status_t;

void st_get_video_status(st_video_status_t *status);

typedef struct {
    uint8_t ym[14];         /* the YM2149's sound registers as the 68000 wrote them */
    uint8_t dma_control;    /* $FF8901: bit 0 playing, bit 1 repeat */
    uint8_t dma_mode;       /* $FF8921: bits 0-1 rate (6258 Hz x 2^n), bit 7 mono */
    uint32_t dma_start, dma_end;
    /* The LMC1992 as the Microwire set it: master 0 to -80 dB, left and
     * right 0 to -40 dB; mix 0 the YM at -12 dB, 1 mixed, 2 no YM
     */
    int8_t master_db, left_db, right_db;
    uint8_t mix;
} st_sound_status_t;

void st_get_sound_status(st_sound_status_t *status);

typedef struct {
    uint16_t enabled;       /* MFP channels enabled and unmasked, bit n channel n */
    uint8_t timer_mode[4];  /* A-D: 0 stopped, 1-7 delay (prescaler 4 .. 200), 8 event count */
    uint8_t timer_data[4];  /* reload values, 0 = 256 */
} st_mfp_status_t;

void st_get_mfp_status(st_mfp_status_t *status);

typedef struct {
    uint8_t mouse_mode;     /* 0 relative, 1 absolute, 2 keycodes, 3 off */
    uint8_t joystick_mode;  /* 0 events, 1 interrogated, 2 off */
    bool paused;            /* output paused ($13) */
    uint8_t buttons;        /* bit 1 left, bit 0 right */
    int64_t clock;          /* the IKBD's clock now: the host's time_t plus the ST's offset */
} st_ikbd_status_t;

void st_get_ikbd_status(st_ikbd_status_t *status);

/* Free ST RAM as GEMDOS sees it (Malloc(-1) gives the largest block), in
 * bytes; false before GEMDOS is up. Safe to call from another task.
 */
bool st_ram_free(uint32_t *total, uint32_t *largest);

/* Current shifter mode */
void st_get_screen_mode(st_screen_mode_t *mode);

/* Render the visible screen (as latched at the last VBL) to RGB565,
 * mode->width x mode->height pixels, `stride` pixels per destination row.
 */
void st_render_rgb565(uint16_t *dst, int stride);

/* Render screen line y (0 .. mode->height - 1), mode->width pixels */
void st_render_line_rgb565(int y, uint16_t *dst);

/* Hash of the visible screen memory, palette and mode: changes when the
 * picture changes
 */
uint32_t st_screen_signature(void);

/* Read ST memory without side effects (RAM and ROM only, else 0) */
uint16_t st_peek16(uint32_t address);

/* Provided by the platform: size and free space of the file system holding
 * `path` (the C: folder). False if unknown.
 */
bool st_host_disk_free(const char *path, uint64_t *total, uint64_t *free_bytes);

/* ---- Sound: 16-bit stereo at ST_AUDIO_HZ ----
 *
 * The emulation only records sound register writes; the samples are made
 * by st_audio_render(), meant to run in another task (the audio output).
 */

#define ST_AUDIO_HZ     25033       /* 4 x the lowest STE DMA sound rate */

/* Turn sound rendering on before st_init() if anything will call
 * st_audio_render(); otherwise sound costs (almost) nothing.
 */
void st_audio_enable(bool enable);

/* Render up to `frames` stereo frames (L, R interleaved), never past the
 * emulated time; returns the number rendered. Call from one task only.
 */
int st_audio_render(int16_t *dst, int frames);

/* Emulated sound not yet rendered, in frames: the emulation's lead */
int st_audio_pending(void);

/* ---- Floppy drives: 0 = A:, 1 = B: ----
 *
 * Disk images: .ST (raw sectors, written back as the ST writes) and .MSA
 * (read-only). Not while st_run_frame() runs; the ST notices the change like
 * a real disk swap. Disks stay in through st_reset() and st_init().
 */
bool st_floppy_insert(int drive, const char *path);
void st_floppy_eject(int drive);
const char *st_floppy_path(int drive);     /* NULL when empty */
bool st_floppy_write_protected(int drive);

/* The unpacked copy an .MSA image gets on first use: ".<name>.st" in the
 * same folder (delete it along with the image)
 */
void st_floppy_unpacked_path(const char *path, char *out, size_t len);

/* Write a blank formatted 720 KB .ST image (80 tracks, 9 sectors, 2 sides) */
bool st_floppy_create(const char *path);

/* The disk's free space and size in bytes as its FAT counts them (the data
 * area), read from the image now: not while st_run_frame() runs on another
 * task. False when empty or not a FAT disk.
 */
bool st_floppy_space(int drive, uint64_t *free_bytes, uint64_t *size);

/* Drive activity, safe to read from another task (values may be a frame old) */
typedef struct {
    int light;              /* drive whose light is on (selected, motor running), -1 none */
    int track[2];           /* head position of A: and B: */
    uint32_t reads[2];      /* sectors read and written since power-on */
    uint32_t writes[2];
} st_floppy_status_t;

void st_floppy_status(st_floppy_status_t *status);

/* ---- ACSI hard disks: devices 0 and 1 on the hard disk port ----
 *
 * Raw images of a whole disk, 512-byte sectors (.IMG, .HD, as Hatari's and
 * the MiSTer's), read and written straight through to the file
 * (write-protected if it's read-only). With a hard disk driver in an
 * image's root sector (AHDI, HDDRIVER, ICD, PPDriver) TOS boots from it,
 * and the driver mounts the partitions from C: on; the host folder's drive
 * (hd_path) then comes after them. Not while st_run_frame() runs; the bus
 * has them at once, TOS and its driver look for them at a reset. They stay
 * in through st_reset() and st_init().
 */
#define ST_ACSI_DEVICES     2
bool st_acsi_insert(int device, const char *path);
void st_acsi_eject(int device);
const char *st_acsi_path(int device);      /* NULL when empty */
bool st_acsi_write_protected(int device);
uint64_t st_acsi_size(int device);          /* bytes, 0 when empty */

/* Activity, safe to read from another task (values may be a frame old) */
typedef struct {
    int light;              /* device that ran a command lately, -1 none */
    uint32_t reads[ST_ACSI_DEVICES];        /* sectors read and written since power-on */
    uint32_t writes[ST_ACSI_DEVICES];
} st_acsi_status_t;

void st_acsi_status(st_acsi_status_t *status);

/* The host folder's drive letter: 'C', or after the partitions of a hard
 * disk image TOS boots (known once the ST has started)
 */
char st_hd_drive(void);

/* The last file the ST opened, created or started on the host folder's
 * drive, as "C:\GAMES\X.PRG". Returns "opened", "created", "started", or
 * NULL (path "") for none yet. Safe to call from another task.
 */
const char *st_hd_last_file(char *path, size_t len);

/* ---- IKBD input: safe to call from another task ---- */

/* ST keyboard scancode (make code, $01-$72) */
void st_ikbd_key(uint8_t scancode, bool pressed);

/* Relative mouse motion and button state */
void st_ikbd_mouse(int dx, int dy, bool left, bool right);

/* Joystick port 0 or 1: bit 0 up, 1 down, 2 left, 3 right, 7 fire */
void st_ikbd_joystick(int port, uint8_t state);

/* The IKBD time-of-day clock (TOS reads it, an STE has no RTC) follows the
 * host's local time: set the system clock and TZ and the ST has the time.
 */
