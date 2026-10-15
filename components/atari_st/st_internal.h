/*
 * Shared state of the STE machine. Not part of the public API.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "st.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_log.h"
#define ST_HOT              IRAM_ATTR
#define ST_LOGI(...)        ESP_LOGI("st", __VA_ARGS__)
#define ST_LOGW(...)        ESP_LOGW("st", __VA_ARGS__)
#else
#include <stdio.h>
#define ST_HOT
#define ST_LOGI(fmt, ...)   printf("I st: " fmt "\n", ##__VA_ARGS__)
#define ST_LOGW(fmt, ...)   printf("W st: " fmt "\n", ##__VA_ARGS__)
#endif

#define ST_CPU_HZ           8010613     /* 32.084988 MHz / 4 */
#define ST_MFP_HZ           2457600

/* Interrupt levels from the GLUE */
#define ST_IRQ_HBL          2
#define ST_IRQ_VBL          4
#define ST_IRQ_MFP          6

/* MFP interrupt channels */
#define MFP_INT_GPIP0       0
#define MFP_INT_TIMER_D     4
#define MFP_INT_TIMER_C     5
#define MFP_INT_ACIA        6           /* GPIP4 */
#define MFP_INT_FDC         7           /* GPIP5 */
#define MFP_INT_TIMER_B     8
#define MFP_INT_TIMER_A     13

/* MFP GPIP input lines */
#define MFP_GPIP_BLITTER    3           /* low while the blitter runs */
#define MFP_GPIP_ACIA       4           /* active low */
#define MFP_GPIP_FDC        5           /* active low */
#define MFP_GPIP_MONO       7           /* 0 = monochrome monitor */

enum { MFP_TIMER_A, MFP_TIMER_B, MFP_TIMER_C, MFP_TIMER_D };

typedef struct {
    /* Memory. RAM and ROM are stored with each 16-bit word in host order,
     * so a word read is a plain load and byte n lives at index n ^ 1.
     */
    uint8_t *ram;
    uint32_t ram_size;
    uint8_t *rom;
    uint32_t rom_base;
    uint32_t rom_size;
    uint8_t *cart;                  /* cartridge at $FA0000, same word order */
    uint32_t cart_size;
    uint8_t mmu_config;             /* $FF8001 */
    st_monitor_t monitor;
    st_monitor_t boot_monitor;
    bool any_resolution;
    bool blitter;
    int start_resolution;
    uint32_t rez_report_pc;         /* native calls in the patched AES routine */
    uint32_t rez_adjust_pc;
    bool first_gem_start;
    int desktop_resolution;
    void (*resolution_changed)(int resolution);

    /* GLUE interrupts */
    bool hbl_pending;
    bool vbl_pending;
    int irq_level;

    /* Frame timing */
    int line;                       /* current scanline */
    int line_cycles;                /* CPU cycles per scanline */
    int lines_per_frame;
    int first_visible_line;
    int visible_lines;
    int cycle_in_line;              /* cycles run in the current line */
    bool cpu_running;               /* inside m68k_execute() */
    uint32_t frames;
    uint32_t bus_errors;
    bool halted;                    /* double bus fault: stopped until reset */
    uint32_t mpb;                   /* GEMDOS memory parameter block, 0 = not yet */
} st_machine_t;

extern st_machine_t st;

/* st.c */
void st_update_irq(void);
void st_bus_error(uint32_t address, bool write);
bool st_supervisor(void);

/* mem.c: ST memory access for the emulator itself (no side effects; writes
 * go to RAM only)
 */
uint8_t st_peek8(uint32_t address);
uint32_t st_peek32(uint32_t address);
bool st_ram_range(uint32_t address, uint32_t len);
void st_poke8(uint32_t address, uint8_t value);
void st_poke16(uint32_t address, uint16_t value);
void st_poke32(uint32_t address, uint32_t value);
void st_ram_write(uint32_t address, const uint8_t *src, uint32_t len);
void st_ram_read(uint32_t address, uint8_t *dst, uint32_t len);

/* gemdos.c: SD card / host folder as drive C: */
void gemdos_init(const char *root, bool boot);
void gemdos_reset(void);
int m68k_host_illegal(int opcode);

/* GEM (re)started in this resolution */
void st_desktop_resolution(int rez);

/* Switch the attached monitor (TOS notices through MFP GPIP 7) */
void st_set_monitor(st_monitor_t monitor);
void st_update_gpip7(void);

/* tos_patch.c */
void tos_patch_apply(uint16_t version, bool any_resolution, bool gemdos_drive);

/* io.c: $FF8000-$FFFFFF, byte accesses (words are split high byte first) */
uint8_t st_io_read8(uint32_t address);
void st_io_write8(uint32_t address, uint8_t value);
void st_io_write16(uint32_t address, uint16_t value);
void st_io_reset(void);

/* mfp.c */
void mfp_reset(void);
uint8_t mfp_read(uint32_t address);
void mfp_write(uint32_t address, uint8_t value);
void mfp_set_gpip(int line, bool level);
void mfp_advance(int cpu_cycles);
void mfp_timer_event(int timer);
bool mfp_irq(void);
int mfp_ack(void);

/* video.c */
void video_reset(void);
void video_start_frame(void);
uint8_t video_read(uint32_t address);
void video_write(uint32_t address, uint8_t value);

/* fdc.c */
void fdc_reset(void);
uint8_t fdc_read(uint32_t address);
void fdc_write(uint32_t address, uint8_t value);
void fdc_advance(int cpu_cycles);
void fdc_disk_changed(int drive);

/* floppy.c: disk images in drives 0 (A:) and 1 (B:) */
bool floppy_present(int drive);
bool floppy_write_protected(int drive);
int floppy_sectors_per_track(int drive);
bool floppy_read_sector(int drive, int track, int side, int sector, uint8_t *buf);
bool floppy_write_sector(int drive, int track, int side, int sector, const uint8_t *buf);

/* psg.c */
void psg_render(int32_t *out, int count, int sample_rate);
void psg_synth_write(uint8_t reg, uint8_t value);
void psg_reset(void);
uint8_t psg_read(uint32_t address);
void psg_write(uint32_t address, uint8_t value);
uint8_t psg_port_a(void);       /* bit 0 side, bit 1 drive A, bit 2 drive B (active low) */

/* audio.c: DMA sound, Microwire, mixing ($FF8900-$FF893F) */
void audio_init(void);
void audio_reset(void);
void psg_sound_write(uint8_t reg, uint8_t value);
void audio_sync(void);
void audio_frame_end(void);
uint8_t audio_read(uint32_t address);
void audio_write(uint32_t address, uint8_t value);
bool audio_dma_active(void);

/* blitter.c */
void blitter_reset(void);
uint8_t blitter_read(uint32_t address);
void blitter_write(uint32_t address, uint8_t value);
void blitter_write16(uint32_t address, uint16_t value);

/* acia.c */
void acia_reset(void);
uint8_t acia_read(uint32_t address);
void acia_write(uint32_t address, uint8_t value);
void acia_advance(int cpu_cycles);
