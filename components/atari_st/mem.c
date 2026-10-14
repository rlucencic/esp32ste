/*
 * STE memory map and the Musashi bus callbacks.
 *
 *   $000000-$000007  ROM (first 8 bytes: reset SSP and PC), writes bus error
 *   $000008-$0007FF  RAM, supervisor only
 *   $000800-RAM end  RAM
 *   RAM end-$3FFFFF  no memory: reads $FF, writes ignored
 *   $400000-$DFFFFF  bus error
 *   $E00000-$E3FFFF  TOS 2.x/1.62 (256 KB), or $FC0000-$FEFFFF for 192 KB TOS
 *   $FA0000-$FBFFFF  cartridge: the GEMDOS drive hook when enabled, else $FF
 *   $FF8000-$FFFFFF  I/O, supervisor only
 */
#include "m68k.h"
#include "st_internal.h"

#define LOW_MEM_END     0x800
#define RAM_SPACE_END   0x400000
#define CART_START      0xFA0000
#define CART_END        0xFC0000
#define IO_START        0xFF8000

static inline uint16_t rd16(const uint8_t *mem, uint32_t offset)
{
    return *(const uint16_t *)(mem + offset);
}

static inline void wr16(uint8_t *mem, uint32_t offset, uint16_t value)
{
    *(uint16_t *)(mem + offset) = value;
}

static inline bool in_rom(uint32_t a)
{
    return a - st.rom_base < st.rom_size;
}

static inline bool in_cart(uint32_t a)
{
    return a - CART_START < st.cart_size;
}

static uint8_t slow_read8(uint32_t a)
{
    if (a < LOW_MEM_END) {
        if (!st_supervisor()) {
            st_bus_error(a, false);
        }
        return a < 8 ? st.rom[a ^ 1] : st.ram[a ^ 1];
    }
    if (a < st.ram_size) {
        return st.ram[a ^ 1];
    }
    if (a < RAM_SPACE_END) {
        return 0xFF;
    }
    if (in_rom(a)) {
        return st.rom[(a - st.rom_base) ^ 1];
    }
    if (in_cart(a)) {
        return st.cart[(a - CART_START) ^ 1];
    }
    if (a >= CART_START && a < CART_END) {
        return 0xFF;
    }
    if (a >= IO_START) {
        if (!st_supervisor()) {
            st_bus_error(a, false);
        }
        return st_io_read8(a);
    }
    st_bus_error(a, false);
    return 0xFF;
}

static uint16_t slow_read16(uint32_t a)
{
    if (a < LOW_MEM_END) {
        if (!st_supervisor()) {
            st_bus_error(a, false);
        }
        return a < 8 ? rd16(st.rom, a) : rd16(st.ram, a);
    }
    if (a < st.ram_size) {
        return rd16(st.ram, a);
    }
    if (a < RAM_SPACE_END) {
        return 0xFFFF;
    }
    if (in_rom(a)) {
        return rd16(st.rom, a - st.rom_base);
    }
    if (in_cart(a)) {
        return rd16(st.cart, a - CART_START);
    }
    if (a >= CART_START && a < CART_END) {
        return 0xFFFF;
    }
    if (a >= IO_START) {
        if (!st_supervisor()) {
            st_bus_error(a, false);
        }
        uint16_t hi = st_io_read8(a);
        return (hi << 8) | st_io_read8(a + 1);
    }
    st_bus_error(a, false);
    return 0xFFFF;
}

static void slow_write8(uint32_t a, uint8_t v)
{
    if (a < LOW_MEM_END) {
        if (a < 8 || !st_supervisor()) {
            st_bus_error(a, true);
        }
        st.ram[a ^ 1] = v;
        return;
    }
    if (a < st.ram_size) {
        st.ram[a ^ 1] = v;
        return;
    }
    if (a < RAM_SPACE_END) {
        return;
    }
    if (a >= IO_START) {
        if (!st_supervisor()) {
            st_bus_error(a, true);
        }
        st_io_write8(a, v);
        return;
    }
    st_bus_error(a, true);
}

static void slow_write16(uint32_t a, uint16_t v)
{
    if (a < LOW_MEM_END) {
        if (a < 8 || !st_supervisor()) {
            st_bus_error(a, true);
        }
        wr16(st.ram, a, v);
        return;
    }
    if (a < st.ram_size) {
        wr16(st.ram, a, v);
        return;
    }
    if (a < RAM_SPACE_END) {
        return;
    }
    if (a >= IO_START) {
        if (!st_supervisor()) {
            st_bus_error(a, true);
        }
        st_io_write16(a, v);
        return;
    }
    st_bus_error(a, true);
}

/* ---- Musashi callbacks: fast path for plain RAM ---- */

unsigned int ST_HOT m68k_read_memory_8(unsigned int a)
{
    if (a - LOW_MEM_END < st.ram_size - LOW_MEM_END) {
        return st.ram[a ^ 1];
    }
    return slow_read8(a);
}

unsigned int ST_HOT m68k_read_memory_16(unsigned int a)
{
    if (a - LOW_MEM_END < st.ram_size - LOW_MEM_END) {
        return rd16(st.ram, a);
    }
    return slow_read16(a);
}

unsigned int ST_HOT m68k_read_memory_32(unsigned int a)
{
    if (a - LOW_MEM_END < st.ram_size - LOW_MEM_END - 2) {
        return ((uint32_t)rd16(st.ram, a) << 16) | rd16(st.ram, a + 2);
    }
    uint32_t hi = slow_read16(a);
    return (hi << 16) | slow_read16((a + 2) & 0xFFFFFF);
}

void ST_HOT m68k_write_memory_8(unsigned int a, unsigned int v)
{
    if (a - LOW_MEM_END < st.ram_size - LOW_MEM_END) {
        st.ram[a ^ 1] = v;
        return;
    }
    slow_write8(a, v);
}

void ST_HOT m68k_write_memory_16(unsigned int a, unsigned int v)
{
    if (a - LOW_MEM_END < st.ram_size - LOW_MEM_END) {
        wr16(st.ram, a, v);
        return;
    }
    slow_write16(a, v);
}

void ST_HOT m68k_write_memory_32(unsigned int a, unsigned int v)
{
    if (a - LOW_MEM_END < st.ram_size - LOW_MEM_END - 2) {
        wr16(st.ram, a, v >> 16);
        wr16(st.ram, a + 2, v & 0xFFFF);
        return;
    }
    slow_write16(a, v >> 16);
    slow_write16((a + 2) & 0xFFFFFF, v & 0xFFFF);
}

uint16_t st_peek16(uint32_t a)
{
    a &= 0xFFFFFE;
    if (a < 8) {
        return rd16(st.rom, a);
    }
    if (a < st.ram_size) {
        return rd16(st.ram, a);
    }
    if (in_rom(a)) {
        return rd16(st.rom, a - st.rom_base);
    }
    if (in_cart(a)) {
        return rd16(st.cart, a - CART_START);
    }
    return 0;
}

uint8_t st_peek8(uint32_t a)
{
    uint16_t w = st_peek16(a & ~1u);
    return (a & 1) ? (w & 0xFF) : (w >> 8);
}

uint32_t st_peek32(uint32_t a)
{
    return ((uint32_t)st_peek16(a) << 16) | st_peek16(a + 2);
}

bool st_ram_range(uint32_t a, uint32_t len)
{
    return a < st.ram_size && len <= st.ram_size - a;
}

void st_poke8(uint32_t a, uint8_t v)
{
    if (a < st.ram_size) {
        st.ram[a ^ 1] = v;
    }
}

void st_poke16(uint32_t a, uint16_t v)
{
    if (a + 1 < st.ram_size) {
        wr16(st.ram, a & ~1u, v);
    }
}

void st_poke32(uint32_t a, uint32_t v)
{
    st_poke16(a, v >> 16);
    st_poke16(a + 2, v & 0xFFFF);
}

void st_ram_write(uint32_t a, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        st.ram[(a + i) ^ 1] = src[i];
    }
}

void st_ram_read(uint32_t a, uint8_t *dst, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = st.ram[(a + i) ^ 1];
    }
}

unsigned int m68k_read_disassembler_16(unsigned int a)
{
    return st_peek16(a);
}

unsigned int m68k_read_disassembler_32(unsigned int a)
{
    return ((uint32_t)st_peek16(a) << 16) | st_peek16(a + 2);
}
