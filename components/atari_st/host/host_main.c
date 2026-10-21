/*
 * Run the STE on the host: boot a TOS image for a number of frames, log
 * progress and write the final screen as a PPM image.
 *
 *   ./st_host <tos.img> [frames] [out.ppm] [ram_kb] [mono]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include "st.h"
#include "m68k.h"
#include "emudbg.h"

/* Disassemble `count` instructions at `pc` */
static void disassemble(uint32_t pc, int count)
{
    char buf[100];
    for (int i = 0; i < count; i++) {
        unsigned int len = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
        printf("  $%06lx  %s\n", (unsigned long)pc, buf);
        pc += len;
    }
}

static uint32_t vec_trail[40];
static uint32_t vec_frame, vec_frame_now;

/* ST_TRACE=<pc hex>:<count>: once execution reaches pc, print the next
 * count instructions
 */
static uint32_t trace_pc = 0xFFFFFFFF;
static int trace_left;


/* ST_RING=n: print the last n instructions when the run ends */
#define RING_SIZE 4096
static uint32_t ring[RING_SIZE];
static uint32_t ring_pos;

void m68k_host_instruction(unsigned int pc)
{
    /* ST_WATCH=<addr hex>: report each change of the word there, with the
     * instruction that made it
     */
    static uint32_t watch = 1;
    static uint16_t watch_val;
    if (watch == 1) {
        watch = getenv("ST_WATCH") ? strtoul(getenv("ST_WATCH"), NULL, 16) : 0;
        watch_val = st_peek16(watch);
    }
    if (watch) {
        uint16_t v = st_peek16(watch);
        if (v != watch_val) {
            printf("-- $%06lx: %04x -> %04x by\n", (unsigned long)watch, watch_val, v);
            /* ST_WATCH_TRAIL=n: also show the n instructions before */
            int trail = getenv("ST_WATCH_TRAIL") ? atoi(getenv("ST_WATCH_TRAIL")) : 1;
            for (uint32_t i = ring_pos - trail; i != ring_pos; i++) {
                disassemble(ring[i % RING_SIZE], 1);
            }
            watch_val = v;
        }
    }

    ring[ring_pos++ % RING_SIZE] = pc;

    /* ST_STOP=<pc hex>[:<after frame>]: on reaching pc, show how we got there and exit */
    static uint32_t stop_pc = 1;
    static int stop_after;
    if (stop_pc == 1) {
        const char *e = getenv("ST_STOP");
        stop_pc = e ? strtoul(e, NULL, 16) : 0;
        stop_after = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 0;
    }
    if (stop_pc && pc == stop_pc && (int)vec_frame_now > stop_after) {
        printf("-- reached $%06x at frame %lu after:\n", pc, (unsigned long)vec_frame_now);
        for (uint32_t i = ring_pos - 80; i != ring_pos; i++) {
            disassemble(ring[i % RING_SIZE], 1);
        }
        exit(0);
    }

    /* ST_GEMDOS=<frame>: log GEMDOS calls (TRAP #1) after that frame */
    static int gemdos_after = -2;
    if (gemdos_after == -2) {
        gemdos_after = getenv("ST_GEMDOS") ? atoi(getenv("ST_GEMDOS")) : -1;
    }
    if (gemdos_after >= 0 && (int)vec_frame_now > gemdos_after && st_peek16(pc) == 0x4E41) {
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
        uint16_t fn = st_peek16(sp);
        char name[64] = "";
        if (fn == 0x3D || fn == 0x4E || fn == 0x3C || fn == 0x41 || fn == 0x3B || fn == 0x4B) {
            uint32_t p = ((uint32_t)st_peek16(sp + 2) << 16) | st_peek16(sp + 4);
            if (fn == 0x4B || fn == 0x3C) {
                p = ((uint32_t)st_peek16(sp + 4) << 16) | st_peek16(sp + 6);
            }
            for (int i = 0; i < 63; i++) {
                uint16_t w = st_peek16((p + i) & ~1);
                char c = ((p + i) & 1) ? (w & 0xFF) : (w >> 8);
                name[i] = c;
                if (c == 0) {
                    break;
                }
            }
        }
        printf("-- GEMDOS $%02x %s (frame %lu, PC $%06x)\n", fn, name,
               (unsigned long)vec_frame_now, pc);
    }

    /* ST_PCLOG=<pc hex>:<frame>: D0-D7 each time pc is reached after that frame */
    static uint32_t pclog_pc = 1;
    static int pclog_after;
    if (pclog_pc == 1) {
        const char *e = getenv("ST_PCLOG");
        pclog_pc = e ? strtoul(e, NULL, 16) : 0;
        pclog_after = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 0;
    }
    if (pclog_pc && pc == pclog_pc && (int)vec_frame_now > pclog_after) {
        printf("-- $%06x frame %lu:", pc, (unsigned long)vec_frame_now);
        for (int r = M68K_REG_D0; r <= M68K_REG_D7; r++) {
            printf(" %x", m68k_get_reg(NULL, r));
        }
        uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
        printf("  stack");
        for (int i = 0; i < 6; i++) {
            printf(" %04x", st_peek16(sp + 2 * i));
        }
        /* ST_PCLOG_MEM=a,b,...: longs there too */
        const char *m = getenv("ST_PCLOG_MEM");
        while (m && *m) {
            uint32_t addr = strtoul(m, (char **)&m, 16);
            printf("  [%x]=%04x%04x", addr, st_peek16(addr), st_peek16(addr + 2));
            if (*m == ',') {
                m++;
            }
        }
        printf("\n");
    }

    /* ST_AES=<frame>: log AES calls (TRAP #2, D0 = 200) after that frame:
     * opcode, int_in[0..1]
     */
    static int aes_after = -2;
    if (aes_after == -2) {
        aes_after = getenv("ST_AES") ? atoi(getenv("ST_AES")) : -1;
    }
    if (aes_after >= 0 && (int)vec_frame_now > aes_after && st_peek16(pc) == 0x4E42 &&
        (m68k_get_reg(NULL, M68K_REG_D0) & 0xFFFF) == 200) {
        uint32_t pb = m68k_get_reg(NULL, M68K_REG_D1);
        uint32_t control = ((uint32_t)st_peek16(pb) << 16) | st_peek16(pb + 2);
        uint32_t int_in = ((uint32_t)st_peek16(pb + 8) << 16) | st_peek16(pb + 10);
        printf("-- AES %3u (%u, %u) frame %lu\n", st_peek16(control), st_peek16(int_in),
               st_peek16(int_in + 2), (unsigned long)vec_frame_now);
    }

    /* ST_VEC=n: remember what led into exception vector n's handler, the
     * last time it was entered
     */
    static int vec = -2;
    if (vec == -2) {
        vec = getenv("ST_VEC") ? atoi(getenv("ST_VEC")) : -1;
    }
    if (vec >= 0) {
        uint32_t handler = ((uint32_t)st_peek16(vec * 4) << 16) | st_peek16(vec * 4 + 2);
        if (pc == handler && handler != 0) {
            for (int i = 0; i < 40; i++) {
                vec_trail[i] = ring[(ring_pos - 40 + i) % RING_SIZE];
            }
            vec_frame = vec_frame_now;
        }
    }
    /* ST_TRACE_FRAME=n: only start tracing after frame n */
    static int trace_after = -1;
    if (trace_after < 0) {
        trace_after = getenv("ST_TRACE_FRAME") ? atoi(getenv("ST_TRACE_FRAME")) : 0;
    }
    if (pc == trace_pc && trace_left == 0 && (int)vec_frame_now > trace_after) {
        trace_left = -1;
    }
    if (trace_left < 0) {
        static int count;
        if (count == 0) {
            count = atoi(strchr(getenv("ST_TRACE"), ':') + 1);
        }
        disassemble(pc, 1);
        if (getenv("ST_REGS")) {
            printf("            D0=%08x D1=%08x D2=%08x A0=%08x A4=%08x A5=%08x A6=%08x A7=%08x\n",
                   m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
                   m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_A0),
                   m68k_get_reg(NULL, M68K_REG_A4), m68k_get_reg(NULL, M68K_REG_A5),
                   m68k_get_reg(NULL, M68K_REG_A6), m68k_get_reg(NULL, M68K_REG_A7));
        }
        if (--count == 0) {
            trace_left = 1;
        }
    }
}

bool st_host_disk_free(const char *path, uint64_t *total, uint64_t *free_bytes)
{
    struct statvfs v;
    if (statvfs(path, &v) != 0) {
        return false;
    }
    *total = (uint64_t)v.f_blocks * v.f_frsize;
    *free_bytes = (uint64_t)v.f_bavail * v.f_frsize;
    return true;
}

static uint8_t *load_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*size);
    if (fread(data, 1, *size, f) != *size) {
        perror(path);
        exit(1);
    }
    fclose(f);
    return data;
}

static void write_ppm(const char *path)
{
    st_screen_mode_t mode;
    st_get_screen_mode(&mode);
    uint16_t *pixels = malloc(mode.width * mode.height * sizeof(uint16_t));
    st_render_rgb565(pixels, mode.width);

    /* Medium resolution pixels are twice as tall as wide */
    int yscale = mode.height == 200 && mode.width == 640 ? 2 : 1;
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", mode.width, mode.height * yscale);
    for (int y = 0; y < mode.height; y++) {
        for (int r = 0; r < yscale; r++) {
            for (int x = 0; x < mode.width; x++) {
                uint16_t p = pixels[y * mode.width + x];
                uint8_t rgb[3] = {
                    (p >> 11) << 3 | (p >> 13),
                    ((p >> 5) & 0x3F) << 2 | ((p >> 9) & 3),
                    (p & 0x1F) << 3 | ((p >> 2) & 7),
                };
                fwrite(rgb, 1, 3, f);
            }
        }
    }
    fclose(f);
    free(pixels);
    printf("screen %dx%d (%d planes, %d Hz) -> %s\n", mode.width, mode.height,
           mode.planes, mode.hz, path);
}

/* ST_SCRIPT=<file>: one command per line, run before the given frame:
 *   <frame> mouse <dx> <dy> <buttons>   buttons bit 0 left, bit 1 right
 *   <frame> key <hex scancode> <0|1>
 *   <frame> shot <file.ppm>
 *   <frame> dump <file>                  RAM as big-endian bytes
 */
typedef struct {
    int frame;
    char cmd[8];
    char arg[3][64];
} script_line_t;

static script_line_t script[4096];
static uint32_t ram_size_for_dump;
static int script_len;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        exit(1);
    }
    char line[256];
    while (fgets(line, sizeof(line), f) && script_len < 4096) {
        script_line_t *l = &script[script_len];
        if (sscanf(line, "%d %7s %63s %63s %63s", &l->frame, l->cmd, l->arg[0], l->arg[1],
                   l->arg[2]) >= 2) {
            script_len++;
        }
    }
    fclose(f);
}

static void run_script(int frame)
{
    for (int i = 0; i < script_len; i++) {
        script_line_t *l = &script[i];
        if (l->frame != frame) {
            continue;
        }
        if (strcmp(l->cmd, "mouse") == 0) {
            int b = atoi(l->arg[2]);
            st_ikbd_mouse(atoi(l->arg[0]), atoi(l->arg[1]), b & 1, b & 2);
        } else if (strcmp(l->cmd, "key") == 0) {
            st_ikbd_key(strtoul(l->arg[0], NULL, 16), atoi(l->arg[1]));
        } else if (strcmp(l->cmd, "shot") == 0) {
            write_ppm(l->arg[0]);
        } else if (strcmp(l->cmd, "insert") == 0) {
            st_floppy_insert(atoi(l->arg[0]), l->arg[1]);
        } else if (strcmp(l->cmd, "eject") == 0) {
            st_floppy_eject(atoi(l->arg[0]));
        } else if (strcmp(l->cmd, "dump") == 0) {
            FILE *f = fopen(l->arg[0], "wb");
            for (uint32_t a = 0; a < ram_size_for_dump; a += 2) {
                uint16_t w = st_peek16(a);
                uint8_t b[2] = { w >> 8, w & 0xFF };
                fwrite(b, 1, 2, f);
            }
            fclose(f);
        }
    }
}

/* ST_WAV=<file>: record the sound output */
static FILE *wav;
static uint32_t wav_frames;

static void wav_write_header(void)
{
    uint32_t bytes = wav_frames * 4;
    uint8_t h[44] = { 'R', 'I', 'F', 'F' };
    uint32_t v;
    v = 36 + bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t pcm = 1, channels = 2, bits = 16, align = 4;
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &channels, 2);
    v = ST_AUDIO_HZ; memcpy(h + 24, &v, 4);
    v = ST_AUDIO_HZ * 4; memcpy(h + 28, &v, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), wav);
}

static void drain_audio(void)
{
    int16_t buf[1024 * 2];
    int n;
    while ((n = st_audio_render(buf, 1024)) > 0) {
        if (wav) {
            fwrite(buf, 4, n, wav);
            wav_frames += n;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tos.img> [frames] [out.ppm] [ram_kb] [mono]\n", argv[0]);
        return 1;
    }
    int frames = argc > 2 ? atoi(argv[2]) : 600;
    const char *out = argc > 3 ? argv[3] : "screen.ppm";
    uint32_t ram_size = (argc > 4 ? atoi(argv[4]) : 1024) * 1024;

    st_config_t cfg = {
        .ram = malloc(ram_size),
        .ram_size = ram_size,
        .monitor = argc > 5 && strcmp(argv[5], "mono") == 0 ? ST_MONITOR_MONO : ST_MONITOR_COLOR,
        .any_resolution = getenv("ST_ANYRES") != NULL,
        .hd_path = getenv("ST_HD"),             /* folder for drive C: */
        .hd_boot = getenv("ST_HD") != NULL,
        .no_blitter = getenv("ST_NOBLIT") != NULL,
        .start_resolution = getenv("ST_REZ") ? atoi(getenv("ST_REZ")) : -1,
    };
    cfg.tos = load_file(argv[1], &cfg.tos_size);
    if (cfg.tos == NULL) {
        return 1;
    }
    cfg.rom = malloc(cfg.tos_size);
    st_audio_enable(getenv("ST_WAV") != NULL);
    if (!st_init(&cfg)) {
        return 1;
    }
    /* ST_CPU=<MHz>: the 68000 faster (or slower) than the STE's 8 MHz */
    if (getenv("ST_CPU")) {
        st_set_cpu_speed((unsigned)(atof(getenv("ST_CPU")) * ST_CPU_SPEED_8MHZ / 8 + 0.5));
    }

    /* ST_NEWDISK=path: write a blank disk; ST_FLOPPY_A/B=image: insert */
    if (getenv("ST_NEWDISK") && !st_floppy_create(getenv("ST_NEWDISK"))) {
        return 1;
    }
    if (getenv("ST_FLOPPY_A")) {
        st_floppy_insert(0, getenv("ST_FLOPPY_A"));
    }
    if (getenv("ST_FLOPPY_B")) {
        st_floppy_insert(1, getenv("ST_FLOPPY_B"));
    }

    if (getenv("ST_TRACE")) {
        trace_pc = strtoul(getenv("ST_TRACE"), NULL, 16);
    }

    /* ST_KEY=frame: tap the space bar at that frame */
    int key_frame = getenv("ST_KEY") ? atoi(getenv("ST_KEY")) : -1;

    ram_size_for_dump = ram_size;
    if (getenv("ST_SCRIPT")) {
        load_script(getenv("ST_SCRIPT"));
    }
    extern bool st_debug_blitter;
    st_debug_blitter = getenv("ST_BLITLOG") != NULL;
    bool quiet = getenv("ST_QUIET") != NULL;
    /* ST_DEBUG=<port>: serve the debugger (esp32emu's docs/debugger.md) on
     * 127.0.0.1:<port> and run in real time until killed
     */
    emudbg_host_run("ST_DEBUG", st_run_frame, 50);

    if (getenv("ST_WAV")) {
        wav = fopen(getenv("ST_WAV"), "wb");
        wav_write_header();
    }

    clock_t start = clock();
    for (int i = 1; i <= frames; i++) {
        if (i == key_frame) {
            st_ikbd_key(0x39, true);
        } else if (i == key_frame + 5) {
            st_ikbd_key(0x39, false);
        }
        run_script(i);
        vec_frame_now = i;
        st_run_frame();
        drain_audio();
        if ((!quiet && i % 60 == 0) || i == frames) {
            st_status_t s;
            st_get_status(&s);
            printf("frame %4lu: PC $%06lx SR $%04x hz_200 %lu vbclock %lu bus errors %lu\n",
                   (unsigned long)s.frames, (unsigned long)s.pc, s.sr,
                   (unsigned long)s.hz_200, (unsigned long)s.vbclock,
                   (unsigned long)s.bus_errors);
        }
    }
    if (wav) {
        wav_write_header();
        fclose(wav);
        printf("sound: %lu frames -> %s\n", (unsigned long)wav_frames, getenv("ST_WAV"));
    }
    uint32_t ram_free, ram_largest;
    if (st_ram_free(&ram_free, &ram_largest)) {
        printf("free ST RAM: %lu bytes, largest block %lu\n",
               (unsigned long)ram_free, (unsigned long)ram_largest);
    }
    for (int d = 0; d < 2; d++) {
        uint64_t free_bytes, size;
        if (st_floppy_space(d, &free_bytes, &size)) {
            printf("%c: %llu bytes free of %llu\n", 'A' + d, (unsigned long long)free_bytes,
                   (unsigned long long)size);
        }
    }
    double secs = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("%d frames in %.2f s host time\n", frames, secs);

    if (vec_frame) {
        printf("-- last entry into vector %s at frame %lu:\n", getenv("ST_VEC"),
               (unsigned long)vec_frame);
        for (int i = 0; i < 40; i++) {
            disassemble(vec_trail[i], 1);
        }
    }

    if (getenv("ST_RING")) {
        uint32_t n = atoi(getenv("ST_RING"));
        n = n > RING_SIZE ? RING_SIZE : n;
        for (uint32_t i = ring_pos - n; i != ring_pos; i++) {
            disassemble(ring[i % RING_SIZE], 1);
        }
    }

    const char *dis = getenv("ST_DIS");
    if (dis != NULL) {
        disassemble(strtoul(dis, NULL, 16), 24);
    }
    write_ppm(out);
    return 0;
}
