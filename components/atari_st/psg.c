/*
 * YM2149 PSG ($FF8800-$FF88FF, mirrored every 4 bytes). Port A drives
 * floppy side/drive select, port B the printer.
 *
 * The emulation side keeps the register file the CPU sees; sound register
 * writes go through the audio event queue (audio.c) to the synthesiser
 * below, which has its own copy of the registers.
 *
 * The chip runs from 2 MHz; everything inside it advances in steps of 8
 * clocks (250 kHz "ticks"): tone counters toggle after `period` ticks, noise
 * shifts every 2 * period ticks and the 32-step envelope moves every
 * `period` ticks. Output samples average all ticks inside them.
 */
#include <string.h>
#include "st_internal.h"

#define YM_TICK_HZ      250000

static const uint8_t reg_mask[16] = {
    0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
    0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
};

/* ---- emulation side: the registers the CPU sees ------------------------ */

static struct {
    uint8_t reg[16];
    uint8_t select;
} psg;

void psg_reset(void)
{
    memset(&psg, 0, sizeof(psg));
    psg.reg[7] = 0xFF;
    psg.reg[14] = 0xFF;
    for (int r = 0; r < 14; r++) {
        psg_sound_write(r, psg.reg[r]);
    }
}

uint8_t psg_port_a(void)
{
    return psg.reg[14];
}

uint8_t psg_read(uint32_t a)
{
    if ((a & 3) == 0) {
        return psg.reg[psg.select];
    }
    return 0xFF;
}

void psg_write(uint32_t a, uint8_t v)
{
    switch (a & 3) {
    case 0:
        if (v < 16) {
            psg.select = v;
        }
        break;
    case 2:
        psg.reg[psg.select] = v & reg_mask[psg.select];
        if (psg.select < 14) {
            psg_sound_write(psg.select, psg.reg[psg.select]);
        }
        break;
    }
}

/* ---- synthesiser (render side) ------------------------------------------ */

static struct {
    uint8_t reg[14];
    uint16_t tone_count[3];
    uint8_t tone_out[3];
    uint16_t noise_count;
    uint32_t lfsr;
    uint8_t noise_out;
    uint32_t env_count;
    uint8_t env_pos;            /* 0..31 within the current ramp */
    uint8_t env_attack;         /* ramping up */
    uint8_t env_holding;
    uint8_t env_hold_value;
    uint32_t tick_frac;         /* ticks per output sample, 16.16 */
} ym = { .lfsr = 1 };

/* YM2149 5-bit DAC, ~1.5 dB per step, full scale 8191 per channel */
static const uint16_t ym_level[32] = {
       0,   40,   48,   57,   68,   81,   97,  115,  137,  163,  194,  231,
     274,  326,  388,  461,  548,  652,  775,  921, 1096, 1303, 1549, 1842,
    2190, 2604, 3097, 3683, 4379, 5207, 6192, 8191,
};

void psg_synth_write(uint8_t reg, uint8_t value)
{
    ym.reg[reg] = value;
    if (reg == 13) {
        ym.env_count = 0;
        ym.env_pos = 0;
        ym.env_attack = (value & 0x04) != 0;
        ym.env_holding = 0;
    }
}

static inline uint8_t env_value(void)
{
    if (ym.env_holding) {
        return ym.env_hold_value;
    }
    return ym.env_attack ? ym.env_pos : 31 - ym.env_pos;
}

/* Envelope: one step every `period` ticks */
static inline void env_advance(uint32_t ticks)
{
    if (ym.env_holding) {
        return;
    }
    uint32_t ep = ym.reg[11] | (ym.reg[12] << 8);
    ep = ep ? ep : 1;
    ym.env_count += ticks;
    while (ym.env_count >= ep && !ym.env_holding) {
        ym.env_count -= ep;
        if (++ym.env_pos > 31) {
            uint8_t shape = ym.reg[13];
            bool cont = shape & 0x08, att = shape & 0x04;
            bool alt = shape & 0x02, hold = shape & 0x01;
            ym.env_pos = 0;
            if (!cont) {
                ym.env_holding = 1;
                ym.env_hold_value = 0;
            } else if (hold) {
                ym.env_holding = 1;
                ym.env_hold_value = (alt ^ att) ? 31 : 0;
            } else if (alt) {
                ym.env_attack ^= 1;
            }
        }
    }
}

/* Noise: the 17-bit LFSR shifts every 2 * period ticks */
static inline void noise_advance(uint32_t ticks)
{
    uint32_t np = ym.reg[6] ? ym.reg[6] * 2 : 2;
    ym.noise_count += ticks;
    while (ym.noise_count >= np) {
        ym.noise_count -= np;
        uint32_t bit = (ym.lfsr ^ (ym.lfsr >> 3)) & 1;
        ym.lfsr = (ym.lfsr >> 1) | (bit << 16);
    }
    ym.noise_out = ym.lfsr & 1;
}

/* Tone channel: ticks spent high during the next `ticks` ticks */
static inline uint32_t tone_advance(int ch, uint32_t ticks)
{
    uint32_t period = ym.reg[ch * 2] | (ym.reg[ch * 2 + 1] << 8);
    period = period ? period : 1;
    uint32_t count = ym.tone_count[ch];
    if (count >= period) {
        count = period - 1;     /* period lowered: toggle on the next tick */
    }
    uint8_t out = ym.tone_out[ch];
    uint32_t high = 0;
    while (ticks) {
        uint32_t to_toggle = period - count;
        if (to_toggle > ticks) {
            high += out ? ticks : 0;
            count += ticks;
            break;
        }
        high += out ? to_toggle : 0;
        ticks -= to_toggle;
        count = 0;
        out ^= 1;
    }
    ym.tone_count[ch] = count;
    ym.tone_out[ch] = out;
    return high;
}

/* Mono samples, sum of the 3 channels (0 .. 3 * 8191). Tones are averaged
 * over the ticks in each sample; noise and envelope are sampled.
 */
void ST_HOT psg_render(int32_t *out, int count, int sample_rate)
{
    uint32_t step = ((uint64_t)YM_TICK_HZ << 16) / sample_rate;
    uint8_t mixer = ym.reg[7];

    for (int i = 0; i < count; i++) {
        ym.tick_frac += step;
        uint32_t ticks = ym.tick_frac >> 16;
        ym.tick_frac &= 0xFFFF;
        if (ticks == 0) {
            out[i] = i ? out[i - 1] : 0;
            continue;
        }
        env_advance(ticks);
        noise_advance(ticks);
        uint8_t env = env_value();

        int32_t sum = 0;
        for (int ch = 0; ch < 3; ch++) {
            bool tone_off = (mixer >> ch) & 1;
            bool noise_off = (mixer >> (ch + 3)) & 1;
            uint32_t high = tone_off ? ticks : tone_advance(ch, ticks);
            if (!(noise_off || ym.noise_out) || high == 0) {
                continue;
            }
            uint8_t vol = ym.reg[8 + ch];
            uint8_t fixed = vol & 0x0F;
            uint32_t level = ym_level[(vol & 0x10) ? env : fixed ? fixed * 2 + 1 : 0];
            sum += level * high;
        }
        out[i] = sum / ticks;
    }
}
