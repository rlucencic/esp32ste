/*
 * STE sound: DMA sound ($FF8900-$FF8921), the LMC1992 volume chip on the
 * Microwire interface ($FF8922/$FF8924), mixing with the YM2149, output.
 *
 * Split across two sides so the emulation core does almost no sound work:
 *
 *  - Emulation side: keeps the registers, advances DMA sound in emulated
 *    time (frame ends clock MFP Timer A and flip GPIP 7) and records every
 *    sound register write in an event queue, stamped with its time in
 *    output samples.
 *  - Render side (st_audio_render, another task): replays the events at
 *    their timestamps into its own copies of the YM2149 and DMA engine and
 *    synthesises the samples, reading DMA sample data from ST RAM.
 *
 * Both sides run the same DMA engine code on the same events, so they agree
 * on where every frame ends.
 */
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
#include "st_internal.h"

#define EVENT_QUEUE         8192        /* power of two */
#define CHUNK               64

/* DMA sound control/mode bits */
#define DMA_PLAY            0x01
#define DMA_REPEAT          0x02
#define DMA_MONO            0x80

enum { EV_YM, EV_DMA_CONTROL, EV_DMA_MODE, EV_DMA_START, EV_DMA_END, EV_LMC };

typedef struct {
    uint64_t time;                      /* output sample index */
    uint8_t type;
    uint8_t reg;
    uint16_t value;
    uint32_t value32;
} audio_event_t;

typedef struct {
    uint8_t control, mode;
    uint32_t start, end;                /* as written */
    uint32_t frame_end;
    uint32_t pos;                       /* address counter */
    uint32_t frac;                      /* 16.16 output-to-DMA sample step */
} dma_t;

static const uint32_t dma_rates[4] = { 6258, 12517, 25033, 50066 };

/* ---- shared DMA engine -------------------------------------------------- */

static void dma_latch(dma_t *d)
{
    d->pos = d->start & ~1u;
    d->frame_end = d->end & ~1u;
}

static void dma_write_control(dma_t *d, uint8_t v)
{
    if ((v & DMA_PLAY) && !(d->control & DMA_PLAY)) {
        dma_latch(d);
        d->frac = 0;
    }
    d->control = v & 3;
}

/* Step one DMA sample; returns true at a frame end */
static inline bool dma_step(dma_t *d)
{
    d->pos += (d->mode & DMA_MONO) ? 1 : 2;
    if (d->pos < d->frame_end) {
        return false;
    }
    if (d->control & DMA_REPEAT) {
        dma_latch(d);
    } else {
        d->control &= ~DMA_PLAY;
    }
    return true;
}

/* DMA samples due for `samples` output samples */
static inline uint32_t dma_due(dma_t *d, uint32_t samples)
{
    uint32_t step = ((uint64_t)dma_rates[d->mode & 3] << 16) / ST_AUDIO_HZ;
    uint64_t f = (uint64_t)d->frac + (uint64_t)step * samples;
    d->frac = f & 0xFFFF;
    return f >> 16;
}

/* ---- emulation side ------------------------------------------------------ */

static struct {
    dma_t dma;
    uint16_t mw_data, mw_mask;
    uint64_t frame_base;                /* CPU cycles at the start of this frame */
    uint64_t now;                       /* output samples up to now */
    bool enabled;                       /* the render side consumes events */

    audio_event_t *queue;               /* EVENT_QUEUE entries, heap (PSRAM on the board) */
    volatile uint32_t head, tail;
    uint32_t dropped;
} emu;

static void push_event(uint8_t type, uint8_t reg, uint16_t value, uint32_t value32)
{
    if (!emu.enabled || emu.queue == NULL) {
        return;
    }
    uint32_t head = emu.head;
    if (head - __atomic_load_n(&emu.tail, __ATOMIC_ACQUIRE) >= EVENT_QUEUE) {
        emu.dropped++;
        return;
    }
    audio_event_t *e = &emu.queue[head % EVENT_QUEUE];
    e->time = emu.now;
    e->type = type;
    e->reg = reg;
    e->value = value;
    e->value32 = value32;
    __atomic_store_n(&emu.head, head + 1, __ATOMIC_RELEASE);
}

static uint64_t now_cycles(void)
{
    uint32_t c = st.line * st.line_cycles + st.cycle_in_line;
    if (st.cpu_running) {
        c += m68k_cycles_run();
    }
    return emu.frame_base + c;
}

bool audio_dma_active(void)
{
    return (emu.dma.control & DMA_PLAY) != 0;
}

/* Bring emulated sound time up to the current CPU cycle */
void audio_sync(void)
{
    uint64_t due = now_cycles() * ST_AUDIO_HZ / ST_CPU_HZ;
    if (due <= emu.now) {
        return;
    }
    uint32_t samples = due - emu.now;
    __atomic_store_n(&emu.now, due, __ATOMIC_RELEASE);

    dma_t *d = &emu.dma;
    if (!(d->control & DMA_PLAY)) {
        return;
    }
    for (uint32_t n = dma_due(d, samples); n; n--) {
        if (dma_step(d)) {
            mfp_timer_event(MFP_TIMER_A);   /* end of frame clocks Timer A */
            if (!(d->control & DMA_PLAY)) {
                st_update_gpip7();
                break;
            }
        }
    }
}

void audio_frame_end(void)
{
    audio_sync();
    emu.frame_base += (uint64_t)st.lines_per_frame * st.line_cycles;
}

void audio_reset(void)
{
    audio_sync();
    memset(&emu.dma, 0, sizeof(emu.dma));
    emu.mw_data = emu.mw_mask = 0;
    push_event(EV_DMA_CONTROL, 0, 0, 0);
}

void psg_sound_write(uint8_t reg, uint8_t value)
{
    audio_sync();
    push_event(EV_YM, reg, value, 0);
}

/* Shift out the Microwire data bits under the mask, MSB first */
static void microwire_send(void)
{
    uint16_t bits = 0;
    int n = 0;
    for (int b = 15; b >= 0; b--) {
        if (emu.mw_mask & (1u << b)) {
            bits = (bits << 1) | ((emu.mw_data >> b) & 1);
            n++;
        }
    }
    if (n >= 11) {
        push_event(EV_LMC, 0, bits & 0x7FF, 0);
    }
}

uint8_t audio_read(uint32_t a)
{
    dma_t *d = &emu.dma;
    switch (a) {
    case 0xFF8901: audio_sync(); return d->control;
    case 0xFF8903: return d->start >> 16;
    case 0xFF8905: return d->start >> 8;
    case 0xFF8907: return d->start;
    case 0xFF8909: audio_sync(); return d->pos >> 16;
    case 0xFF890B: audio_sync(); return d->pos >> 8;
    case 0xFF890D: audio_sync(); return d->pos;
    case 0xFF890F: return d->end >> 16;
    case 0xFF8911: return d->end >> 8;
    case 0xFF8913: return d->end;
    case 0xFF8921: return d->mode;
    case 0xFF8922: return 0;    /* transfer finished: data shifted out */
    case 0xFF8923: return 0;
    case 0xFF8924: return emu.mw_mask >> 8;
    case 0xFF8925: return emu.mw_mask;
    }
    return 0;
}

void audio_write(uint32_t a, uint8_t v)
{
    dma_t *d = &emu.dma;
    audio_sync();
    switch (a) {
    case 0xFF8901:
        dma_write_control(d, v);
        push_event(EV_DMA_CONTROL, 0, v, 0);
        st_update_gpip7();
        break;
    case 0xFF8903: d->start = (d->start & 0x00FFFF) | ((v & 0x3F) << 16); break;
    case 0xFF8905: d->start = (d->start & 0xFF00FF) | (v << 8); break;
    case 0xFF8907: d->start = (d->start & 0xFFFF00) | (v & 0xFE); break;
    case 0xFF890F: d->end = (d->end & 0x00FFFF) | ((v & 0x3F) << 16); break;
    case 0xFF8911: d->end = (d->end & 0xFF00FF) | (v << 8); break;
    case 0xFF8913: d->end = (d->end & 0xFFFF00) | (v & 0xFE); break;
    case 0xFF8921:
        d->mode = v & 0x83;
        push_event(EV_DMA_MODE, 0, d->mode, 0);
        break;
    case 0xFF8922: emu.mw_data = (emu.mw_data & 0x00FF) | (v << 8); break;
    case 0xFF8923:
        emu.mw_data = (emu.mw_data & 0xFF00) | v;
        microwire_send();
        break;
    case 0xFF8924: emu.mw_mask = (emu.mw_mask & 0x00FF) | (v << 8); break;
    case 0xFF8925: emu.mw_mask = (emu.mw_mask & 0xFF00) | v; break;
    }
    if (a >= 0xFF8903 && a <= 0xFF8907) {
        push_event(EV_DMA_START, 0, 0, d->start);
    } else if (a >= 0xFF890F && a <= 0xFF8913) {
        push_event(EV_DMA_END, 0, 0, d->end);
    }
}

/* ---- render side ------------------------------------------------------------- */

static struct {
    dma_t dma;
    int8_t left, right;                 /* current DMA sample */
    int32_t gain_master, gain_left, gain_right;  /* 16.16 */
    uint8_t mix;                        /* 0 YM -12 dB, 1 YM mixed, 2 no YM */
    int32_t dc_left, dc_right;          /* DC blocker state */
    uint64_t pos;                       /* output samples rendered */
} ren;

/* -2 dB per step */
static int32_t db_gain(int steps_down)
{
    int32_t g = 65536;
    for (int i = 0; i < steps_down; i++) {
        g = (int64_t)g * 52057 >> 16;       /* 10^(-0.1) */
    }
    return g;
}

static void lmc1992_command(uint16_t bits)
{
    if ((bits >> 9) != 2) {
        return;
    }
    int data = bits & 0x3F;
    switch ((bits >> 6) & 7) {
    case 0: ren.mix = data & 3; break;
    case 3: ren.gain_master = db_gain(40 - (data > 40 ? 40 : data)); break;
    case 4: ren.gain_right = db_gain(20 - (data > 20 ? 20 : data)); break;
    case 5: ren.gain_left = db_gain(20 - (data > 20 ? 20 : data)); break;
    default: break;     /* bass, treble */
    }
}

static void apply_event(const audio_event_t *e)
{
    switch (e->type) {
    case EV_YM:          psg_synth_write(e->reg, e->value); break;
    case EV_DMA_CONTROL: dma_write_control(&ren.dma, e->value); break;
    case EV_DMA_MODE:    ren.dma.mode = e->value; break;
    case EV_DMA_START:   ren.dma.start = e->value32; break;
    case EV_DMA_END:     ren.dma.end = e->value32; break;
    case EV_LMC:         lmc1992_command(e->value); break;
    }
}

static inline int8_t ram_s8(uint32_t a)
{
    return a < st.ram_size ? (int8_t)st.ram[a ^ 1] : 0;
}

static inline int16_t clip16(int32_t v)
{
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

/* Render `n` samples with the current state (no events inside) */
static void ST_HOT render_span(int16_t *dst, int n)
{
    int32_t ym[CHUNK];
    dma_t *d = &ren.dma;
    int64_t gl = (int64_t)ren.gain_master * ren.gain_left >> 16;
    int64_t gr = (int64_t)ren.gain_master * ren.gain_right >> 16;

    while (n > 0) {
        int c = n < CHUNK ? n : CHUNK;
        psg_render(ym, c, ST_AUDIO_HZ);
        for (int i = 0; i < c; i++) {
            if (d->control & DMA_PLAY) {
                for (uint32_t k = dma_due(d, 1); k; k--) {
                    if (d->mode & DMA_MONO) {
                        ren.left = ren.right = ram_s8(d->pos);
                    } else {
                        ren.left = ram_s8(d->pos);
                        ren.right = ram_s8(d->pos + 1);
                    }
                    dma_step(d);
                }
            } else {
                ren.left = ren.right = 0;
            }
            int32_t l = ren.left * 128;         /* +-16384 */
            int32_t r = ren.right * 128;

            int32_t y = ym[i] * 3 / 4;          /* 0 .. 18430 */
            if (ren.mix == 0) {
                y /= 4;
            } else if (ren.mix == 2) {
                y = 0;
            }
            l += y;
            r += y;

            /* Remove the YM's DC offset: one-pole high-pass, ~20 Hz */
            ren.dc_left += ((l << 8) - ren.dc_left) >> 10;
            ren.dc_right += ((r << 8) - ren.dc_right) >> 10;
            l -= ren.dc_left >> 8;
            r -= ren.dc_right >> 8;

            *dst++ = clip16(l * gl >> 16);
            *dst++ = clip16(r * gr >> 16);
        }
        n -= c;
    }
}

int ST_HOT st_audio_render(int16_t *dst, int frames)
{
    /* Only as far as the emulation has got */
    uint64_t limit = __atomic_load_n(&emu.now, __ATOMIC_ACQUIRE);
    if (ren.pos + frames > limit) {
        frames = limit > ren.pos ? limit - ren.pos : 0;
    }
    int done = 0;
    while (done < frames) {
        uint32_t tail = emu.tail;
        uint32_t head = __atomic_load_n(&emu.head, __ATOMIC_ACQUIRE);
        while (tail != head && emu.queue[tail % EVENT_QUEUE].time <= ren.pos) {
            apply_event(&emu.queue[tail % EVENT_QUEUE]);
            tail++;
        }
        __atomic_store_n(&emu.tail, tail, __ATOMIC_RELEASE);

        /* Up to the next event */
        int n = frames - done;
        if (tail != head) {
            uint64_t next = emu.queue[tail % EVENT_QUEUE].time;
            if (next - ren.pos < (uint64_t)n) {
                n = next - ren.pos;
            }
        }
        render_span(dst + done * 2, n);
        ren.pos += n;
        done += n;
    }
    return done;
}

int st_audio_pending(void)
{
    return __atomic_load_n(&emu.now, __ATOMIC_ACQUIRE) - ren.pos;
}

void st_audio_enable(bool enable)
{
    emu.enabled = enable;
}

void audio_init(void)
{
    bool enabled = emu.enabled;
    audio_event_t *queue = emu.queue;
    memset(&emu, 0, sizeof(emu));
    emu.enabled = enabled;
    if (enabled && queue == NULL) {
        queue = malloc(EVENT_QUEUE * sizeof(audio_event_t));
    }
    emu.queue = queue;
    memset(&ren, 0, sizeof(ren));
    ren.gain_master = ren.gain_left = ren.gain_right = 65536;
    ren.mix = 1;
}
