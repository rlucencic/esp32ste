/*
 * Patches applied to the RAM copy of the TOS ROM (the image in flash is
 * never changed). Each patch checks the original bytes first and is skipped
 * as a whole if they don't match, e.g. for another language's TOS.
 */
#include <string.h>
#include "st_internal.h"

typedef struct {
    uint32_t offset;            /* from the ROM start, even */
    uint16_t len;               /* bytes, even */
    const uint8_t *expect;      /* big-endian, as in the ROM image */
    const uint8_t *value;
} patch_block_t;

#define BLOCK(off, exp, val) { off, sizeof(exp), exp, val }

/* ---- AES: choose the resolution GEM (re)starts in -----------------------
 *
 * The same routine in TOS 1.62 and 2.06. The original only goes Low <-> Medium
 * and never leaves High. Replacement:
 *
 *     cur = Getrez(); request may be replaced by the emulator (first start
 *     after a reset: the remembered resolution)
 *     want = request - 2   (request 2/3/4 = Low/Medium/High)
 *     if (want is 0..2 && want != cur) { Setscreen(-1, -1, want); cur = want; }
 *     report cur to the emulator (remembered for the next boot)
 *     Setpalette(cur ? pal_mono : pal_low); return cur + 1;
 *
 * Templates with zero placeholders for the version-specific addresses.
 */
#define AES_REZ_LEN         0x5E
#define AES_OLD_REQUEST     0x12        /* movea.l <request>.l,a0 */
#define AES_OLD_PAL_LOW     0x42        /* pea <low palette> */
#define AES_NEW_REQUEST     0x10
#define AES_NEW_ADJUST      0x16        /* native call: may replace the request in d0 */
#define AES_NEW_REPORT      0x3C        /* native call: d2 = resolution GEM starts in */
#define AES_NEW_PAL_MONO    0x42
#define AES_NEW_PAL_LOW     0x4C

static const uint8_t aes_rez_old[AES_REZ_LEN] = {
    0x3f, 0x3c, 0x00, 0x04, 0x4e, 0x4e, 0x54, 0x8f, 0x14, 0x00, 0xb4, 0x3c, 0x00, 0x02, 0x67, 0x6c,
    0x20, 0x79, 0x00, 0x00, 0x00, 0x00, 0x30, 0x10, 0xb0, 0x7c, 0x00, 0x01, 0x66, 0x06, 0x4a, 0x02,
    0x67, 0x1e, 0x60, 0x46, 0xb0, 0x7c, 0x00, 0x03, 0x67, 0x28, 0x4a, 0x02, 0x67, 0x12, 0x42, 0x67,
    0x70, 0xff, 0x2f, 0x00, 0x2f, 0x00, 0x3f, 0x3c, 0x00, 0x05, 0x4e, 0x4e, 0x4f, 0xef, 0x00, 0x0c,
    0x48, 0x79, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x3c, 0x00, 0x06, 0x4e, 0x4e, 0x5c, 0x8f, 0x70, 0x01,
    0x4e, 0x75, 0x70, 0x01, 0xb4, 0x00, 0x67, 0x12, 0x3f, 0x00, 0x70, 0xff, 0x2f, 0x00,
};

static const uint8_t aes_rez_new[AES_REZ_LEN] = {
    0x3f, 0x3c, 0x00, 0x04,             /* 00  move.w  #4,-(a7)        Getrez */
    0x4e, 0x4e,                         /* 04  trap    #14 */
    0x54, 0x8f,                         /* 06  addq.l  #2,a7 */
    0x34, 0x00,                         /* 08  move.w  d0,d2           cur */
    0xc4, 0x7c, 0x00, 0x03,             /* 0a  and.w   #3,d2 */
    0x20, 0x79, 0x00, 0x00, 0x00, 0x00, /* 0e  movea.l <request>.l,a0 */
    0x30, 0x10,                         /* 14  move.w  (a0),d0 */
    0x00, 0x09,                         /* 16  native ADJUST_REQUEST */
    0x55, 0x40,                         /* 18  subq.w  #2,d0           want */
    0x6b, 0x20,                         /* 1a  bmi.s   palette */
    0xb0, 0x7c, 0x00, 0x02,             /* 1c  cmp.w   #2,d0 */
    0x6e, 0x1a,                         /* 20  bgt.s   palette */
    0xb0, 0x42,                         /* 22  cmp.w   d2,d0 */
    0x67, 0x16,                         /* 24  beq.s   palette */
    0x3f, 0x00,                         /* 26  move.w  d0,-(a7)        keep want */
    0x3f, 0x00,                         /* 28  move.w  d0,-(a7)        Setscreen(-1, -1, want) */
    0x72, 0xff,                         /* 2a  moveq   #-1,d1 */
    0x2f, 0x01,                         /* 2c  move.l  d1,-(a7) */
    0x2f, 0x01,                         /* 2e  move.l  d1,-(a7) */
    0x3f, 0x3c, 0x00, 0x05,             /* 30  move.w  #5,-(a7) */
    0x4e, 0x4e,                         /* 34  trap    #14 */
    0x4f, 0xef, 0x00, 0x0c,             /* 36  lea     12(a7),a7 */
    0x34, 0x1f,                         /* 3a  move.w  (a7)+,d2 */
    0x00, 0x0E,                         /* 3c palette: native REPORT_REZ */
    0x3f, 0x02,                         /* 3e  move.w  d2,-(a7) */
    0x48, 0x79, 0x00, 0x00, 0x00, 0x00, /* 40  pea     <medium/high palette> */
    0x4a, 0x42,                         /* 46  tst.w   d2 */
    0x66, 0x06,                         /* 48  bne.s   setpal */
    0x2e, 0xbc, 0x00, 0x00, 0x00, 0x00, /* 4a  move.l  #<low palette>,(a7) */
    0x3f, 0x3c, 0x00, 0x06,             /* 50 setpal: move.w #6,-(a7)  Setpalette */
    0x4e, 0x4e,                         /* 54  trap    #14 */
    0x5c, 0x8f,                         /* 56  addq.l  #6,a7 */
    0x30, 0x1f,                         /* 58  move.w  (a7)+,d0 */
    0x52, 0x40,                         /* 5a  addq.w  #1,d0 */
    0x4e, 0x75,                         /* 5c  rts */
};

/* ---- Patch sets ----------------------------------------------------------- */

typedef enum { FEATURE_ANY_RESOLUTION, FEATURE_NO_HD_SCAN, FEATURE_NO_HD_WAIT, FEATURE_ALWAYS } feature_t;

typedef struct {
    uint16_t version;
    feature_t feature;
    const char *what;
    const patch_block_t *blocks;        /* fixed patches */
    int count;
    /* AES resolution routine (FEATURE_ANY_RESOLUTION), 0 for none */
    uint32_t aes_offset;
    uint32_t aes_request;               /* pointer to the requested resolution */
    uint32_t aes_pal_low;               /* TOS palette tables */
    uint32_t aes_pal_mono;
} tos_patch_t;

/* TOS 1.62 desktop: the resolution buttons the monitor can't show are
 * disabled with MOVE.W #DISABLED,(A0); make that MOVE.W #0,(A0).
 */
static const uint8_t t162_disable_old[] = { 0x30, 0xBC, 0x00, 0x08 };
static const uint8_t t162_disable_new[] = { 0x30, 0xBC, 0x00, 0x00 };

static const patch_block_t tos162_blocks[] = {
    BLOCK(0x19262, t162_disable_old, t162_disable_new),  /* High, when in Low or Medium */
    BLOCK(0x19272, t162_disable_old, t162_disable_new),  /* Low, when in High */
    BLOCK(0x1927E, t162_disable_old, t162_disable_new),  /* Medium, when in High */
};

/* TOS 2.06 desktop: on an ST it offers ST High alone when in High, else ST
 * Low and ST Medium. NOP both branches so all three are offered.
 */
static const uint8_t t206_bne_old[] = { 0x66, 0x0A };
static const uint8_t t206_bra_old[] = { 0x60, 0x10 };
static const uint8_t nop[]          = { 0x4E, 0x71 };

static const patch_block_t tos206_blocks[] = {
    BLOCK(0x303CC, t206_bne_old, nop),
    BLOCK(0x303D6, t206_bra_old, nop),
};

/* With the GEMDOS drive and no hard disk image there is nothing on the ACSI
 * bus worth looking for: TOS 1.62 skips the DMA boot scan (8 devices with
 * timeouts, ~10 s). TOS 2.06 waits for hard disks to spin up (until 80 s
 * after power-on, the memory test and a bar a key skips, asking no drive),
 * which an image never needs: skipped with either. Applied or undone at
 * each reset (tos_patch_hd()), as images come and go.
 */
static const uint8_t t162_dmaboot_old[] = { 0x7E, 0x00, 0x2F, 0x39 };   /* moveq #0,d7 ... */
static const uint8_t t162_dmaboot_new[] = { 0x4E, 0x75, 0x2F, 0x39 };   /* rts */
static const uint8_t t206_hdwait_old[]  = { 0x2E, 0x3C, 0x00, 0x00, 0x3E, 0x80 };  /* move.l #16000,d7 */
static const uint8_t t206_hdwait_new[]  = { 0x2E, 0x3C, 0x00, 0x00, 0x00, 0x00 };  /* move.l #0,d7 */

static const patch_block_t tos162_hd_blocks[] = {
    BLOCK(0x0065C, t162_dmaboot_old, t162_dmaboot_new),
};

static const patch_block_t tos206_hd_blocks[] = {
    BLOCK(0x0097A, t206_hdwait_old, t206_hdwait_new),
};

/* TOS 2.06 checks both ROM chips' CRCs in the first 80 s after power-on
 * (hz_200 < 16000, i.e. every emulated boot) and complains about the patched
 * copy with "BAD ROM CRC IN CHIP E/O". Skip it: BCC -> BRA.
 */
static const uint8_t t206_crc_old[] = { 0x64, 0x00, 0x00, 0x9C };   /* bcc.w no_check */
static const uint8_t t206_crc_new[] = { 0x60, 0x00, 0x00, 0x9C };   /* bra.w no_check */

static const patch_block_t tos206_crc_blocks[] = {
    BLOCK(0x007F6, t206_crc_old, t206_crc_new),
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const tos_patch_t patches[] = {
    { 0x0162, FEATURE_ANY_RESOLUTION, "all resolutions in Set Preferences",
      tos162_blocks, COUNT(tos162_blocks), 0x0B550, 0x2B24, 0xE0B5E6, 0xE0B5DE },
    { 0x0206, FEATURE_ANY_RESOLUTION, "all resolutions in Set Preferences",
      tos206_blocks, COUNT(tos206_blocks), 0x0705E, 0x290C, 0xE070F4, 0xE070EC },
    { 0x0162, FEATURE_NO_HD_SCAN, "no ACSI boot scan",
      tos162_hd_blocks, COUNT(tos162_hd_blocks), 0, 0, 0, 0 },
    { 0x0206, FEATURE_NO_HD_WAIT, "no hard disk spin-up wait",
      tos206_hd_blocks, COUNT(tos206_hd_blocks), 0, 0, 0, 0 },
    { 0x0206, FEATURE_ALWAYS, "no ROM CRC check",
      tos206_crc_blocks, COUNT(tos206_crc_blocks), 0, 0, 0, 0 },
};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

/* The working ROM copy keeps each word in host order */
static bool block_matches(const patch_block_t *b)
{
    if (b->offset + b->len > st.rom_size) {
        return false;
    }
    for (int i = 0; i < b->len; i += 2) {
        uint16_t word = (b->expect[i] << 8) | b->expect[i + 1];
        if (*(uint16_t *)(st.rom + b->offset + i) != word) {
            return false;
        }
    }
    return true;
}

static void block_write(const patch_block_t *b)
{
    for (int i = 0; i < b->len; i += 2) {
        *(uint16_t *)(st.rom + b->offset + i) = (b->value[i] << 8) | b->value[i + 1];
    }
}

/* The other way: a fixed patch's blocks hold the new bytes, and get back
 * the old ones
 */
static patch_block_t reversed(const patch_block_t *b)
{
    patch_block_t r = { b->offset, b->len, b->value, b->expect };
    return r;
}

void tos_patch_hd(uint16_t version, bool no_scan, bool no_wait)
{
    for (size_t i = 0; i < COUNT(patches); i++) {
        const tos_patch_t *p = &patches[i];
        if (p->version != version || (p->feature != FEATURE_NO_HD_SCAN && p->feature != FEATURE_NO_HD_WAIT)) {
            continue;
        }
        bool skip = p->feature == FEATURE_NO_HD_SCAN ? no_scan : no_wait;
        bool done = true, undone = true;
        for (int b = 0; b < p->count; b++) {
            patch_block_t r = reversed(&p->blocks[b]);
            done &= block_matches(&r);
            undone &= block_matches(&p->blocks[b]);
        }
        if (skip ? done : undone) {
            continue;                   /* as wanted already */
        }
        if (!(skip ? undone : done)) {
            ST_LOGW("TOS patch skipped, code differs: %s", p->what);
            continue;
        }
        for (int b = 0; b < p->count; b++) {
            patch_block_t r = reversed(&p->blocks[b]);
            block_write(skip ? &p->blocks[b] : &r);
        }
        ST_LOGI("TOS patch%s: %s", skip ? "" : " undone (a hard disk image)", p->what);
    }
}

/* Locate GEMDOS's tick handler by its code. The RAM addresses and the
 * handler's ROM offset can differ between language versions. */
static void patch_clock(void)
{
    static const uint16_t prefix[] = {
        0x4e56, 0x0000, 0x48e7, 0x0300, 0x302e, 0x0008, 0x48c0, 0xd1b9,
    };
    static const uint16_t tail[] = { 0x4a9f, 0x4cdf, 0x0080, 0x4e5e, 0x4e75 };
    enum { CLOCK_TICK = 0x14, CLOCK_EXIT = 0x136 };
    uint32_t found = 0, clock_time = 0, clock_date = 0;
    for (uint32_t off = 0; off + CLOCK_EXIT + sizeof(tail) <= st.rom_size; off += 2) {
        uint32_t pc = st.rom_base + off;
        if (memcmp(st.rom + off, prefix, sizeof(prefix)) != 0 ||
            st_peek32(pc + CLOCK_TICK) != 0x302e0008 || st_peek16(pc + 0x18) != 0xd179 ||
            st_peek32(pc + 0x1e) != 0x0c7907d0 || st_peek32(pc + 0x26) != 0x6d00010e ||
            st_peek16(pc + 0x32) != 0x5279 || st_peek16(pc + 0x94) != 0x3039 ||
            memcmp(st.rom + off + CLOCK_EXIT, tail, sizeof(tail)) != 0) {
            continue;
        }
        uint32_t t = st_peek32(pc + 0x34), d = st_peek32(pc + 0x96);
        if (t < 0x800 || d < 0x800 || t == d || ((t | d) & 1) ||
            !st_ram_range(t, 2) || !st_ram_range(d, 2) ||
            st_peek32(pc + 0x3a) != t || st_peek32(pc + 0xa6) != d) {
            continue;
        }
        if (found) {
            ST_LOGW("TOS clock patch skipped: multiple tick handlers match");
            return;
        }
        found = pc + CLOCK_TICK;
        clock_time = t;
        clock_date = d;
    }
    if (!found) {
        ST_LOGW("TOS clock patch skipped: tick handler differs");
        return;
    }

    /* Keep the tick handler's scheduler update. Replace its separate
     * time-of-day increment with a native call and branch to the epilogue. */
    uint16_t patch[] = { ST_OP_SYNC_CLOCK, 0x6000, CLOCK_EXIT - (CLOCK_TICK + 4) };
    memcpy(st.rom + (found - st.rom_base), patch, sizeof(patch));
    st.clock_sync_pc = found;
    st.clock_time = clock_time;
    st.clock_date = clock_date;
    ST_LOGI("TOS patch: wall-clock time for GEMDOS");
}

static uint32_t unique_words(const uint16_t *pattern, size_t bytes)
{
    uint32_t address = 0;
    for (uint32_t off = 0; off + bytes <= st.rom_size; off += 2) {
        if (memcmp(st.rom + off, pattern, bytes) == 0) {
            if (address) {
                return 0;
            }
            address = st.rom_base + off;
        }
    }
    return address;
}

/* Keep the IKBD protocol as-is. Fix the stock BIOS's year conversion on
 * both sides so XBIOS and GEMDOS agree across the year 2000 boundary. */
static void patch_clock_year(void)
{
    static const uint16_t decode[] = {
        0x6100, 0x00ea, 0x0400, 0x0050, 0x1400, 0xe982, 0x6100, 0x00de, 0xd400,
    };
    static const uint16_t encode[] = {
        0x1002, 0x0200, 0x007f, 0x6122, 0x0610, 0x0080, 0x123c, 0x001b,
    };
    uint32_t rd = unique_words(decode, sizeof(decode));
    uint32_t wr = unique_words(encode, sizeof(encode));
    if (!rd || !wr) {
        ST_LOGW("TOS clock patch skipped: XBIOS year conversion differs");
        return;
    }
    st.clock_decode_pc = rd + 4;
    st.clock_encode_pc = wr + 8;
    const uint16_t native_call[] = { ST_OP_SYNC_CLOCK, 0x4e71 };
    memcpy(st.rom + (st.clock_decode_pc - st.rom_base), native_call, sizeof(native_call));
    memcpy(st.rom + (st.clock_encode_pc - st.rom_base), native_call, sizeof(native_call));
}

void tos_patch_apply(uint16_t version, bool any_resolution)
{
    for (size_t i = 0; i < COUNT(patches); i++) {
        const tos_patch_t *p = &patches[i];
        bool wanted = p->feature == FEATURE_ALWAYS ||
                      (p->feature == FEATURE_ANY_RESOLUTION && any_resolution);
        if (p->version != version || !wanted) {
            continue;
        }

        uint8_t aes_old[AES_REZ_LEN], aes_new[AES_REZ_LEN];
        memcpy(aes_old, aes_rez_old, AES_REZ_LEN);
        memcpy(aes_new, aes_rez_new, AES_REZ_LEN);
        put32(aes_old + AES_OLD_REQUEST, p->aes_request);
        put32(aes_old + AES_OLD_PAL_LOW, p->aes_pal_low);
        put32(aes_new + AES_NEW_REQUEST, p->aes_request);
        put32(aes_new + AES_NEW_PAL_MONO, p->aes_pal_mono);
        put32(aes_new + AES_NEW_PAL_LOW, p->aes_pal_low);
        patch_block_t aes = { p->aes_offset, AES_REZ_LEN, aes_old, aes_new };

        bool match = p->aes_offset == 0 || block_matches(&aes);
        for (int b = 0; b < p->count; b++) {
            match &= block_matches(&p->blocks[b]);
        }
        if (!match) {
            ST_LOGW("TOS patch skipped, code differs: %s", p->what);
            continue;
        }
        if (p->aes_offset) {
            block_write(&aes);
            st.rez_report_pc = st.rom_base + p->aes_offset + AES_NEW_REPORT;
            st.rez_adjust_pc = st.rom_base + p->aes_offset + AES_NEW_ADJUST;
        }
        for (int b = 0; b < p->count; b++) {
            block_write(&p->blocks[b]);
        }
        ST_LOGI("TOS patch: %s", p->what);
    }
    if (version == 0x0162 || version == 0x0206) {
        patch_clock();
        patch_clock_year();
    }
}
