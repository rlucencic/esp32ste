/*
 * STE I/O area $FF8000-$FFFFFF. Unmapped addresses raise a bus error, which
 * TOS relies on to detect what hardware is present (Mega STE cache/VME,
 * RTC, ...).
 */
#include "st_internal.h"

static bool in_range(uint32_t a, uint32_t start, uint32_t end)
{
    return a >= start && a <= end;
}

void st_io_reset(void)
{
    mfp_reset();
    audio_reset();
    blitter_reset();
    video_reset();
    fdc_reset();
    psg_reset();
    acia_reset();
    st_update_irq();
}

uint8_t st_io_read8(uint32_t a)
{
    if (in_range(a, 0xFF8000, 0xFF8001)) {
        return a == 0xFF8001 ? st.mmu_config : 0xFF;
    }
    if (in_range(a, 0xFF8200, 0xFF82FF)) {
        return video_read(a);
    }
    if (in_range(a, 0xFF8600, 0xFF860F)) {
        return fdc_read(a);
    }
    if (in_range(a, 0xFF8800, 0xFF88FF)) {
        return psg_read(a);
    }
    if (in_range(a, 0xFF8900, 0xFF893F)) {
        return audio_read(a);
    }
    if (st.blitter && in_range(a, 0xFF8A00, 0xFF8A3F)) {
        return blitter_read(a);
    }
    if (in_range(a, 0xFF9200, 0xFF923F)) {
        return 0xFF;            /* joypads, paddles, light pen: nothing pressed */
    }
    if (in_range(a, 0xFFFA00, 0xFFFA3F)) {
        return mfp_read(a);
    }
    if (in_range(a, 0xFFFC00, 0xFFFC07)) {
        return acia_read(a);
    }
    st_bus_error(a, false);
    return 0xFF;
}

void st_io_write16(uint32_t a, uint16_t v)
{
    if (st.blitter && in_range(a, 0xFF8A00, 0xFF8A3F)) {
        blitter_write16(a, v);
        return;
    }
    st_io_write8(a, v >> 8);
    st_io_write8(a + 1, v & 0xFF);
}

void st_io_write8(uint32_t a, uint8_t v)
{
    if (in_range(a, 0xFF8000, 0xFF8001)) {
        if (a == 0xFF8001) {
            st.mmu_config = v;
        }
        return;
    }
    if (in_range(a, 0xFF8200, 0xFF82FF)) {
        video_write(a, v);
        return;
    }
    if (in_range(a, 0xFF8600, 0xFF860F)) {
        fdc_write(a, v);
        return;
    }
    if (in_range(a, 0xFF8800, 0xFF88FF)) {
        psg_write(a, v);
        return;
    }
    if (in_range(a, 0xFF8900, 0xFF893F)) {
        audio_write(a, v);
        return;
    }
    if (st.blitter && in_range(a, 0xFF8A00, 0xFF8A3F)) {
        blitter_write(a, v);
        return;
    }
    if (in_range(a, 0xFF9200, 0xFF923F)) {
        return;
    }
    if (in_range(a, 0xFFFA00, 0xFFFA3F)) {
        mfp_write(a, v);
        return;
    }
    if (in_range(a, 0xFFFC00, 0xFFFC07)) {
        acia_write(a, v);
        return;
    }
    st_bus_error(a, true);
}
