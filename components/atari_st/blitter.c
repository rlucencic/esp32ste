/*
 * STE blitter ($FF8A00-$FF8A3D).
 *
 * A blit runs to completion as soon as BUSY is set; the CPU is charged
 * 4 cycles per bus access the blitter makes (as in HOG mode).
 *
 *   $00-$1F  halftone RAM, 16 words      $2E  destination X increment
 *   $20      source X increment          $30  destination Y increment
 *   $22      source Y increment          $32  destination address (long)
 *   $24      source address (long)       $36  X count (words per line)
 *   $28-$2C  end masks 1, 2, 3           $38  Y count (lines)
 *   $3A      HOP (halftone op)           $3B  OP (logic op)
 *   $3C      BUSY, HOG, SMUDGE, line number
 *   $3D      FXSR, NFSR, skew
 */
#include <string.h>
#include "st_internal.h"

#define CTRL_BUSY       0x80
#define CTRL_SMUDGE     0x20
#define SKEW_FXSR       0x80
#define SKEW_NFSR       0x40

#define ACCESS_CYCLES   4

/* Musashi's cycle counter: blitter bus time is taken from the CPU */
extern int m68ki_remaining_cycles;

static uint8_t regs[0x40];
bool st_debug_blitter;

static uint16_t reg16(int off)
{
    return (regs[off] << 8) | regs[off + 1];
}

static void set_reg16(int off, uint16_t v)
{
    regs[off] = v >> 8;
    regs[off + 1] = v;
}

static uint32_t reg_addr(int off)
{
    return (((uint32_t)regs[off + 1] << 16) | (regs[off + 2] << 8) | regs[off + 3]) & 0xFFFFFE;
}

static void set_reg_addr(int off, uint32_t a)
{
    regs[off] = 0;
    regs[off + 1] = a >> 16;
    regs[off + 2] = a >> 8;
    regs[off + 3] = a;
}

/* The blitter is a bus master: plain RAM/ROM access, no protection */
static inline uint16_t bus_read(uint32_t a)
{
    a &= 0xFFFFFE;
    if (a < st.ram_size) {
        return *(const uint16_t *)(st.ram + a);
    }
    return st_peek16(a);
}

static inline void bus_write(uint32_t a, uint16_t v)
{
    a &= 0xFFFFFE;
    if (a < st.ram_size) {
        *(uint16_t *)(st.ram + a) = v;
    }
}

static inline uint16_t logic_op(int op, uint16_t s, uint16_t d)
{
    switch (op) {
    case 0:  return 0;
    case 1:  return s & d;
    case 2:  return s & ~d;
    case 3:  return s;
    case 4:  return ~s & d;
    case 5:  return d;
    case 6:  return s ^ d;
    case 7:  return s | d;
    case 8:  return ~s & ~d;
    case 9:  return ~s ^ d;
    case 10: return ~d;
    case 11: return s | ~d;
    case 12: return ~s;
    case 13: return ~s | d;
    case 14: return ~s | ~d;
    default: return 0xFFFF;
    }
}

static void blit(void)
{
    int16_t src_xinc = reg16(0x20) & ~1, src_yinc = reg16(0x22) & ~1;
    int16_t dst_xinc = reg16(0x2E) & ~1, dst_yinc = reg16(0x30) & ~1;
    uint32_t src = reg_addr(0x24), dst = reg_addr(0x32);
    uint16_t em1 = reg16(0x28), em2 = reg16(0x2A), em3 = reg16(0x2C);
    uint32_t xcount = reg16(0x36) ? reg16(0x36) : 65536;
    uint32_t ycount = reg16(0x38);
    int hop = regs[0x3A] & 3, op = regs[0x3B] & 15;
    uint8_t ctrl = regs[0x3C], skew_reg = regs[0x3D];
    int line = ctrl & 15, skew = skew_reg & 15;
    bool smudge = ctrl & CTRL_SMUDGE;
    bool fxsr = skew_reg & SKEW_FXSR, nfsr = skew_reg & SKEW_NFSR;

    bool use_src = (hop & 2) || (hop == 1 && smudge);
    bool use_dst = !(op == 0 || op == 3 || op == 12 || op == 15);
    bool backwards = src_xinc < 0;
    uint32_t accesses = 0;
    uint32_t buffer = 0;
    uint16_t bus_word = 0;          /* last word on the bus */

    /* The source buffer holds two words; going backwards it shifts the
     * other way and new words enter at the top.
     */
    #define SRC_SHIFT() (buffer = backwards ? buffer >> 16 : buffer << 16)
    #define SRC_ENTER(w) (buffer |= backwards ? (uint32_t)(w) << 16 : (w))

    if (st_debug_blitter) {
        ST_LOGI("BLIT hop %d op %d skew %d%s%s x %lu y %lu sxi %d syi %d dxi %d dyi %d "
                "src %06lx dst %06lx em %04x %04x %04x ctrl %02x",
                hop, op, skew, fxsr ? " FXSR" : "", nfsr ? " NFSR" : "",
                (unsigned long)xcount, (unsigned long)ycount, src_xinc, src_yinc,
                dst_xinc, dst_yinc, (unsigned long)src, (unsigned long)dst, em1, em2, em3, ctrl);
    }
    mfp_set_gpip(MFP_GPIP_BLITTER, false);
    for (; ycount; ycount--) {
        bool skip_final = false;    /* NFSR: no read for the last word */
        if (use_src && fxsr) {
            SRC_SHIFT();
            bus_word = bus_read(src);
            SRC_ENTER(bus_word);
            src += src_xinc;
            accesses++;
        }
        for (uint32_t x = xcount; x; x--) {
            bool first = x == xcount, last = x == 1;
            bool fetched = false;
            if (use_src && !skip_final) {
                SRC_SHIFT();
                bus_word = bus_read(src);
                SRC_ENTER(bus_word);
                fetched = true;
                accesses++;
            }

            uint16_t mask = first ? em1 : last ? em3 : em2;
            uint16_t d = 0;
            if (use_dst || mask != 0xFFFF) {
                d = bus_read(dst);
                bus_word = d;
                accesses++;
            }
            if (nfsr && last) {
                /* The last word takes what is on the bus instead */
                SRC_SHIFT();
                SRC_ENTER(bus_word);
            }

            uint16_t s = buffer >> skew;
            uint16_t ht = reg16((smudge ? (s & 15) : line) * 2);
            uint16_t h;
            switch (hop) {
            case 0:  h = 0xFFFF; break;
            case 1:  h = ht; break;
            case 2:  h = s; break;
            default: h = s & ht; break;
            }
            uint16_t r = logic_op(op, h, d);
            bus_word = (r & mask) | (d & ~mask);
            bus_write(dst, bus_word);
            accesses++;
            if (nfsr && last) {
                SRC_SHIFT();
                SRC_ENTER(bus_word);
            }

            /* Second-to-last word: the final source read will be skipped */
            if (x == 2 && nfsr) {
                skip_final = true;
            }
            if (fetched) {
                src += (last || skip_final) ? src_yinc : src_xinc;
            }
            dst += last ? dst_yinc : dst_xinc;
        }
        line = (line + (dst_yinc < 0 ? -1 : 1)) & 15;
    }
    #undef SRC_SHIFT
    #undef SRC_ENTER
    mfp_set_gpip(MFP_GPIP_BLITTER, true);

    set_reg_addr(0x24, src & 0xFFFFFE);
    set_reg_addr(0x32, dst & 0xFFFFFE);
    set_reg16(0x38, 0);
    regs[0x3C] = (ctrl & ~(CTRL_BUSY | 15)) | line;
    m68ki_remaining_cycles -= accesses * ACCESS_CYCLES;
}

void blitter_reset(void)
{
    memset(regs, 0, sizeof(regs));
}

uint8_t blitter_read(uint32_t a)
{
    return regs[(a - 0xFF8A00) & 0x3F];
}

static void check_start(int off)
{
    if (off == 0x3C && (regs[0x3C] & CTRL_BUSY)) {
        /* Setting BUSY with lines left starts (or restarts) the blit */
        if (reg16(0x38)) {
            blit();
        } else {
            regs[0x3C] &= ~CTRL_BUSY;
        }
    }
}

void blitter_write(uint32_t a, uint8_t v)
{
    int off = (a - 0xFF8A00) & 0x3F;
    regs[off] = v;
    check_start(off);
}

/* Word writes land as a whole: control and skew are often set together */
void blitter_write16(uint32_t a, uint16_t v)
{
    int off = (a - 0xFF8A00) & 0x3E;
    regs[off] = v >> 8;
    regs[off + 1] = v;
    check_start(off);
}
