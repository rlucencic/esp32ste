/*
 * MC68901 MFP at $FFFA01-$FFFA2F (odd bytes): GPIP, interrupt controller,
 * timers A-D. Timers advance once per scanline. The USART is a stub that is
 * always ready to send.
 */
#include <string.h>
#include "st_internal.h"

enum {
    REG_GPIP, REG_AER, REG_DDR, REG_IERA, REG_IERB, REG_IPRA, REG_IPRB,
    REG_ISRA, REG_ISRB, REG_IMRA, REG_IMRB, REG_VR, REG_TACR, REG_TBCR,
    REG_TCDCR, REG_TADR, REG_TBDR, REG_TCDR, REG_TDDR, REG_SCR, REG_UCR,
    REG_RSR, REG_TSR, REG_UDR,
};

#define VR_SOFTWARE_EOI     0x08
#define TSR_BUFFER_EMPTY    0x80
#define TIMER_EVENT_MODE    8

static const uint16_t prescale[8] = { 0, 4, 10, 16, 50, 64, 100, 200 };
static const uint8_t timer_channel[4] = {
    MFP_INT_TIMER_A, MFP_INT_TIMER_B, MFP_INT_TIMER_C, MFP_INT_TIMER_D,
};
/* GPIP line -> interrupt channel */
static const uint8_t gpip_channel[8] = { 0, 1, 2, 3, 6, 7, 14, 15 };

typedef struct {
    uint8_t mode;           /* control register value, 0 = stopped */
    uint8_t reload;         /* data register (0 means 256) */
    uint8_t counter;
    uint32_t prescale_acc;  /* MFP clocks towards the next count */
} mfp_timer_t;

static struct {
    uint8_t gpip_in;        /* external input lines */
    uint8_t gpip_out;       /* output latch for DDR output bits */
    uint8_t aer, ddr, vr;
    uint16_t ier, ipr, isr, imr;    /* bit n = channel n */
    mfp_timer_t timer[4];
    uint8_t scr, ucr, rsr, tsr;
    uint64_t clock_frac;    /* CPU-to-MFP clock conversion remainder */
} mfp;

/* Highest pending, unmasked channel allowed through by the in-service bits */
static int pending_channel(void)
{
    uint16_t active = mfp.ipr & mfp.imr;
    if (active == 0) {
        return -1;
    }
    int ch = 31 - __builtin_clz(active);
    if ((mfp.vr & VR_SOFTWARE_EOI) && mfp.isr >= (1u << ch)) {
        return -1;
    }
    return ch;
}

bool mfp_irq(void)
{
    return pending_channel() >= 0;
}

int mfp_ack(void)
{
    int ch = pending_channel();
    if (ch < 0) {
        return 24;          /* spurious interrupt */
    }
    mfp.ipr &= ~(1u << ch);
    if (mfp.vr & VR_SOFTWARE_EOI) {
        mfp.isr |= 1u << ch;
    }
    return (mfp.vr & 0xF0) | ch;
}

static void request(int ch)
{
    if (mfp.ier & (1u << ch)) {
        mfp.ipr |= 1u << ch;
        st_update_irq();
    }
}

/* Inputs go through XOR with AER; a 1 -> 0 transition of the result
 * requests an interrupt.
 */
static void detect_edges(uint8_t old_in, uint8_t old_aer)
{
    uint8_t before = old_in ^ old_aer;
    uint8_t after = mfp.gpip_in ^ mfp.aer;
    uint8_t edges = before & ~after & ~mfp.ddr;
    for (int line = 0; edges; line++, edges >>= 1) {
        if (edges & 1) {
            request(gpip_channel[line]);
        }
    }
}

void mfp_set_gpip(int line, bool level)
{
    uint8_t old_in = mfp.gpip_in;
    if (level) {
        mfp.gpip_in |= 1u << line;
    } else {
        mfp.gpip_in &= ~(1u << line);
    }
    if (mfp.gpip_in != old_in) {
        detect_edges(old_in, mfp.aer);
    }
}

void mfp_reset(void)
{
    uint8_t lines = 0xFF;
    if (st.monitor == ST_MONITOR_MONO) {
        lines &= ~(1u << MFP_GPIP_MONO);
    }
    memset(&mfp, 0, sizeof(mfp));
    mfp.gpip_in = lines;
    mfp.tsr = TSR_BUFFER_EMPTY;
}

static uint8_t timer_read(int t)
{
    return mfp.timer[t].counter;
}

static void timer_set_mode(int t, uint8_t mode)
{
    mfp_timer_t *tm = &mfp.timer[t];
    if (tm->mode == 0 && mode != 0) {
        tm->prescale_acc = 0;
    }
    tm->mode = mode;
}

static void timer_write_data(int t, uint8_t value)
{
    mfp_timer_t *tm = &mfp.timer[t];
    tm->reload = value;
    if (tm->mode == 0) {
        tm->counter = value;
    }
}

/* Count `counts` decrements; on reaching zero reload and interrupt */
static void timer_count(int t, uint32_t counts)
{
    mfp_timer_t *tm = &mfp.timer[t];
    uint32_t counter = tm->counter ? tm->counter : 256;
    if (counts < counter) {
        tm->counter = counter - counts;
        return;
    }
    uint32_t period = tm->reload ? tm->reload : 256;
    counts -= counter;
    tm->counter = (uint8_t)(period - counts % period);
    request(timer_channel[t]);
}

void mfp_timer_event(int t)
{
    if (mfp.timer[t].mode == TIMER_EVENT_MODE) {
        timer_count(t, 1);
    }
}

void mfp_advance(int cpu_cycles)
{
    mfp.clock_frac += (uint64_t)cpu_cycles * ST_MFP_HZ;
    uint32_t ticks = mfp.clock_frac / ST_CPU_HZ;
    mfp.clock_frac %= ST_CPU_HZ;

    for (int t = 0; t < 4; t++) {
        mfp_timer_t *tm = &mfp.timer[t];
        if (tm->mode == 0 || tm->mode == TIMER_EVENT_MODE) {
            continue;
        }
        /* Delay mode (1-7) and pulse width mode (9-15, treated as delay) */
        uint32_t div = prescale[tm->mode & 7];
        tm->prescale_acc += ticks;
        uint32_t counts = tm->prescale_acc / div;
        tm->prescale_acc %= div;
        if (counts) {
            timer_count(t, counts);
        }
    }
}

uint8_t mfp_read(uint32_t a)
{
    if ((a & 1) == 0 || (a & 0x3F) > 0x2F) {
        return 0xFF;
    }
    switch ((a & 0x3F) >> 1) {
    case REG_GPIP:  return (mfp.gpip_in & ~mfp.ddr) | (mfp.gpip_out & mfp.ddr);
    case REG_AER:   return mfp.aer;
    case REG_DDR:   return mfp.ddr;
    case REG_IERA:  return mfp.ier >> 8;
    case REG_IERB:  return mfp.ier & 0xFF;
    case REG_IPRA:  return mfp.ipr >> 8;
    case REG_IPRB:  return mfp.ipr & 0xFF;
    case REG_ISRA:  return mfp.isr >> 8;
    case REG_ISRB:  return mfp.isr & 0xFF;
    case REG_IMRA:  return mfp.imr >> 8;
    case REG_IMRB:  return mfp.imr & 0xFF;
    case REG_VR:    return mfp.vr;
    case REG_TACR:  return mfp.timer[MFP_TIMER_A].mode;
    case REG_TBCR:  return mfp.timer[MFP_TIMER_B].mode;
    case REG_TCDCR: return (mfp.timer[MFP_TIMER_C].mode << 4) | mfp.timer[MFP_TIMER_D].mode;
    case REG_TADR:  return timer_read(MFP_TIMER_A);
    case REG_TBDR:  return timer_read(MFP_TIMER_B);
    case REG_TCDR:  return timer_read(MFP_TIMER_C);
    case REG_TDDR:  return timer_read(MFP_TIMER_D);
    case REG_SCR:   return mfp.scr;
    case REG_UCR:   return mfp.ucr;
    case REG_RSR:   return mfp.rsr;
    case REG_TSR:   return mfp.tsr;
    case REG_UDR:   return 0;
    }
    return 0xFF;
}

void mfp_write(uint32_t a, uint8_t v)
{
    if ((a & 1) == 0 || (a & 0x3F) > 0x2F) {
        return;
    }
    switch ((a & 0x3F) >> 1) {
    case REG_GPIP:
        mfp.gpip_out = v;
        break;
    case REG_AER: {
        uint8_t old_aer = mfp.aer;
        mfp.aer = v;
        detect_edges(mfp.gpip_in, old_aer);
        break;
    }
    case REG_DDR:
        mfp.ddr = v;
        break;
    case REG_IERA:
        mfp.ier = (mfp.ier & 0x00FF) | (v << 8);
        mfp.ipr &= mfp.ier;
        break;
    case REG_IERB:
        mfp.ier = (mfp.ier & 0xFF00) | v;
        mfp.ipr &= mfp.ier;
        break;
    case REG_IPRA:
        mfp.ipr &= (v << 8) | 0x00FF;
        break;
    case REG_IPRB:
        mfp.ipr &= 0xFF00 | v;
        break;
    case REG_ISRA:
        mfp.isr &= (v << 8) | 0x00FF;
        break;
    case REG_ISRB:
        mfp.isr &= 0xFF00 | v;
        break;
    case REG_IMRA:
        mfp.imr = (mfp.imr & 0x00FF) | (v << 8);
        break;
    case REG_IMRB:
        mfp.imr = (mfp.imr & 0xFF00) | v;
        break;
    case REG_VR:
        mfp.vr = v;
        if (!(v & VR_SOFTWARE_EOI)) {
            mfp.isr = 0;
        }
        break;
    case REG_TACR:
        timer_set_mode(MFP_TIMER_A, v & 0x0F);
        break;
    case REG_TBCR:
        timer_set_mode(MFP_TIMER_B, v & 0x0F);
        break;
    case REG_TCDCR:
        timer_set_mode(MFP_TIMER_C, (v >> 4) & 7);
        timer_set_mode(MFP_TIMER_D, v & 7);
        break;
    case REG_TADR:  timer_write_data(MFP_TIMER_A, v); break;
    case REG_TBDR:  timer_write_data(MFP_TIMER_B, v); break;
    case REG_TCDR:  timer_write_data(MFP_TIMER_C, v); break;
    case REG_TDDR:  timer_write_data(MFP_TIMER_D, v); break;
    case REG_SCR:   mfp.scr = v; break;
    case REG_UCR:   mfp.ucr = v; break;
    case REG_RSR:   mfp.rsr = v; break;
    case REG_TSR:   mfp.tsr = (v & 0x0F) | TSR_BUFFER_EMPTY; break;
    case REG_UDR:   break;
    }
    st_update_irq();
}
