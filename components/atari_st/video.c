/*
 * STE shifter + video timing ($FF8200-$FF82FF) and screen rendering.
 */
#include <string.h>
#include "m68k.h"
#include "st_internal.h"

static struct {
    uint8_t base_hi, base_mid, base_lo;     /* $FF8201, $FF8203, $FF820D */
    uint8_t sync;                           /* $FF820A: bit 1 = 50 Hz */
    uint8_t shift_mode;                     /* $FF8260: 0 low, 1 medium, 2 high */
    uint8_t line_offset;                    /* $FF820F: extra words per line */
    uint8_t hscroll;                        /* $FF8265 */
    uint16_t palette[16];                   /* $FF8240-$FF825F */

    /* Latched at the start of the frame */
    uint32_t frame_base;
    uint8_t frame_mode;
    uint8_t frame_line_offset;
    int hz;
} vid;

void video_reset(void)
{
    memset(&vid, 0, sizeof(vid));
    vid.shift_mode = st.monitor == ST_MONITOR_MONO ? 2 : 0;
    vid.sync = 0x02;
    video_start_frame();
}

static int bytes_per_line(uint8_t mode)
{
    return mode == 2 ? 80 : 160;
}

void video_start_frame(void)
{
    vid.frame_base = ((uint32_t)vid.base_hi << 16) | (vid.base_mid << 8) | (vid.base_lo & 0xFE);
    vid.frame_mode = vid.shift_mode & 3;
    vid.frame_line_offset = vid.line_offset;

    if (st.monitor == ST_MONITOR_MONO) {
        /* SM124: 71.2 Hz, 400 lines */
        st.line_cycles = 224;
        st.lines_per_frame = 501;
        st.first_visible_line = 34;
        st.visible_lines = 400;
        vid.hz = 71;
    } else if (vid.sync & 0x02) {
        st.line_cycles = 512;
        st.lines_per_frame = 313;
        st.first_visible_line = 63;
        st.visible_lines = 200;
        vid.hz = 50;
    } else {
        st.line_cycles = 508;
        st.lines_per_frame = 263;
        st.first_visible_line = 34;
        st.visible_lines = 200;
        vid.hz = 60;
    }
}

/* Video address counter: where the shifter is reading right now */
static uint32_t video_counter(void)
{
    int visible = st.line - st.first_visible_line;
    int stride = bytes_per_line(vid.frame_mode) + vid.frame_line_offset * 2;
    if (visible < 0) {
        return vid.frame_base;
    }
    if (visible >= st.visible_lines) {
        return vid.frame_base + st.visible_lines * stride;
    }
    int cycle = st_bus_cycle_in_line();
    int bytes = cycle * bytes_per_line(vid.frame_mode) / st.line_cycles;
    return vid.frame_base + visible * stride + (bytes & ~1);
}

uint8_t video_read(uint32_t a)
{
    if (a >= 0xFF8240 && a < 0xFF8260) {
        uint16_t c = vid.palette[(a - 0xFF8240) >> 1];
        return (a & 1) ? (c & 0xFF) : (c >> 8);
    }
    switch (a) {
    case 0xFF8201: return vid.base_hi;
    case 0xFF8203: return vid.base_mid;
    case 0xFF820D: return vid.base_lo;
    case 0xFF8205: return video_counter() >> 16;
    case 0xFF8207: return video_counter() >> 8;
    case 0xFF8209: return video_counter();
    case 0xFF820A: return vid.sync | 0xFC;
    case 0xFF820F: return vid.line_offset;
    case 0xFF8260: return vid.shift_mode | 0xFC;
    case 0xFF8265: return vid.hscroll;
    }
    return 0xFF;
}

void video_write(uint32_t a, uint8_t v)
{
    if (a >= 0xFF8240 && a < 0xFF8260) {
        uint16_t *c = &vid.palette[(a - 0xFF8240) >> 1];
        if (a & 1) {
            *c = (*c & 0xFF00) | v;
        } else {
            *c = (*c & 0x00FF) | ((v & 0x0F) << 8);
        }
        return;
    }
    switch (a) {
    case 0xFF8201:
        vid.base_hi = v & 0x3F;
        vid.base_lo = 0;        /* STE: writing the high/mid byte clears the low byte */
        break;
    case 0xFF8203:
        vid.base_mid = v;
        vid.base_lo = 0;
        break;
    case 0xFF820D:
        vid.base_lo = v & 0xFE;
        break;
    case 0xFF820A:
        vid.sync = v & 0x03;
        break;
    case 0xFF820F:
        vid.line_offset = v;
        break;
    case 0xFF8260: {
        uint8_t mode = (v & 3) == 3 ? 0 : (v & 3);
        if (st.any_resolution) {
            /* Multisync: the monitor follows the mode */
            st_set_monitor(mode == 2 ? ST_MONITOR_MONO : ST_MONITOR_COLOR);
            vid.shift_mode = mode;
        } else if (st.monitor != ST_MONITOR_MONO) {
            vid.shift_mode = mode;
        }
        break;
    }
    case 0xFF8265:
        vid.hscroll = v & 0x0F;
        break;
    }
}

void st_get_screen_mode(st_screen_mode_t *mode)
{
    switch (vid.frame_mode) {
    case 0:
        *mode = (st_screen_mode_t){ 320, 200, 4, vid.hz };
        break;
    case 1:
        *mode = (st_screen_mode_t){ 640, 200, 2, vid.hz };
        break;
    default:
        *mode = (st_screen_mode_t){ 640, 400, 1, vid.hz };
        break;
    }
}

/* STE colour nibble: bit 3 is the least significant bit of a 4-bit level */
static uint16_t ste_to_rgb565(uint16_t c)
{
    unsigned r = ((c >> 7) & 0x0E) | ((c >> 11) & 1);
    unsigned g = ((c >> 3) & 0x0E) | ((c >> 7) & 1);
    unsigned b = ((c << 1) & 0x0E) | ((c >> 3) & 1);
    return ((r << 1 | r >> 3) << 11) | ((g << 2 | g >> 2) << 5) | (b << 1 | b >> 3);
}

static inline uint16_t screen_word(uint32_t a)
{
    return a < st.ram_size ? *(const uint16_t *)(st.ram + a) : 0;
}

/* Palette as RGB565 for the current mode */
static void current_colors(uint16_t colors[16], int planes)
{
    if (planes == 1) {
        /* Mono: palette 0 bit 0 selects normal (white paper) or inverted */
        bool white_paper = vid.palette[0] & 1;
        colors[0] = white_paper ? 0xFFFF : 0x0000;
        colors[1] = white_paper ? 0x0000 : 0xFFFF;
    } else {
        for (int i = 0; i < 16; i++) {
            colors[i] = ste_to_rgb565(vid.palette[i]);
        }
    }
}

static void render_line(int y, uint16_t *out, const uint16_t colors[16], const st_screen_mode_t *mode)
{
    int line_stride = bytes_per_line(vid.frame_mode) + vid.frame_line_offset * 2;
    int planes = mode->planes;
    uint32_t addr = vid.frame_base + y * line_stride;

    for (int x = 0; x < mode->width; x += 16) {
        uint32_t group = addr + (x / 16) * planes * 2;
        uint16_t p[4] = { 0 };
        for (int i = 0; i < planes; i++) {
            p[i] = screen_word(group + i * 2);
        }
        for (int bit = 15; bit >= 0; bit--) {
            int index = ((p[0] >> bit) & 1) | (((p[1] >> bit) & 1) << 1) |
                        (((p[2] >> bit) & 1) << 2) | (((p[3] >> bit) & 1) << 3);
            *out++ = colors[index];
        }
    }
}

void st_render_line_rgb565(int y, uint16_t *dst)
{
    st_screen_mode_t mode;
    uint16_t colors[16];
    st_get_screen_mode(&mode);
    current_colors(colors, mode.planes);
    render_line(y, dst, colors, &mode);
}

void st_render_rgb565(uint16_t *dst, int stride)
{
    st_screen_mode_t mode;
    uint16_t colors[16];
    st_get_screen_mode(&mode);
    current_colors(colors, mode.planes);
    for (int y = 0; y < mode.height; y++, dst += stride) {
        render_line(y, dst, colors, &mode);
    }
}

uint32_t st_screen_signature(void)
{
    st_screen_mode_t mode;
    st_get_screen_mode(&mode);
    int line_stride = bytes_per_line(vid.frame_mode) + vid.frame_line_offset * 2;
    uint32_t end = vid.frame_base + mode.height * line_stride;
    if (end > st.ram_size) {
        end = st.ram_size;
    }

    /* FNV-1a over the screen memory, palette and mode */
    uint32_t h = 2166136261u ^ vid.frame_base ^ (vid.frame_mode << 24);
    for (int i = 0; i < 16; i++) {
        h = (h ^ vid.palette[i]) * 16777619u;
    }
    for (uint32_t a = vid.frame_base; a < end; a += 4) {
        h = (h ^ *(const uint32_t *)(st.ram + a)) * 16777619u;
    }
    return h;
}
