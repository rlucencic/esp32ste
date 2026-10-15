/*
 * STE machine: reset, frame loop and the GLUE interrupt logic.
 */
#include <string.h>
#include "m68k.h"
#include "st_internal.h"

/* TOS system variables */
#define SV_MEMVALID     0x420
#define SV_MEMCNTLR     0x424
#define SV_PHYSTOP      0x42E
#define SV_MEMVAL2      0x43A
#define SV_VBCLOCK      0x462
#define SV_HZ_200       0x4BA
#define SV_MEMVAL3      0x51A

#define BUS_ERROR_ADDRESSES     24      /* distinct addresses logged */
#define BUS_ERROR_REPORT_FRAMES 250     /* summary at most every ~5 s */

st_machine_t st;

static void write_long(uint32_t a, uint32_t v)
{
    *(uint16_t *)(st.ram + a) = v >> 16;
    *(uint16_t *)(st.ram + a + 2) = v & 0xFFFF;
}

static uint32_t read_long(uint32_t a)
{
    return ((uint32_t)st_peek16(a) << 16) | st_peek16(a + 2);
}

/* MMU bank configuration for the RAM size: bank 0 in bits 3-2, bank 1 in
 * bits 1-0, each %00 = 128 KB, %01 = 512 KB, %10 = 2 MB.
 */
static uint8_t memory_controller_byte(uint32_t ram_size)
{
    switch (ram_size) {
    case 512 * 1024:        return 0x04;
    case 1024 * 1024:       return 0x05;
    case 2048 * 1024:       return 0x08;
    case 2560 * 1024:       return 0x09;
    default:                return 0x0A;    /* 4 MB */
    }
}

bool st_supervisor(void)
{
    return (m68k_get_reg(NULL, M68K_REG_SR) & 0x2000) != 0;
}

/* Bus errors: each new address once (up to BUS_ERROR_ADDRESSES), then a
 * summary of the rest now and then, so a program probing for hardware in a
 * loop doesn't flood the log but every kind of access shows up.
 */
static struct {
    uint32_t seen[BUS_ERROR_ADDRESSES];
    int seen_count;
    uint32_t reported;              /* bus_errors at the last summary */
    uint32_t report_frame;
} berr;

void st_bus_error(uint32_t address, bool write)
{
    st.bus_errors++;
    uint32_t pc = m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF;
    uint32_t key = (address & 0xFFFFFE) | write;
    bool fresh = true;
    for (int i = 0; i < berr.seen_count; i++) {
        if (berr.seen[i] == key) {
            fresh = false;
            break;
        }
    }
    if (fresh && berr.seen_count < BUS_ERROR_ADDRESSES) {
        berr.seen[berr.seen_count++] = key;
        ST_LOGW("bus error: %s $%06lx at PC $%06lx, SSP $%06lx", write ? "write" : "read",
                (unsigned long)address, (unsigned long)pc,
                (unsigned long)m68k_get_reg(NULL, M68K_REG_ISP));
        berr.reported = st.bus_errors;
        berr.report_frame = st.frames;
    } else if (st.frames - berr.report_frame >= BUS_ERROR_REPORT_FRAMES) {
        ST_LOGW("%lu more bus errors, last: %s $%06lx at PC $%06lx",
                (unsigned long)(st.bus_errors - berr.reported), write ? "write" : "read",
                (unsigned long)address, (unsigned long)pc);
        berr.reported = st.bus_errors;
        berr.report_frame = st.frames;
    }
    m68k_bus_error(address, write);
}

/* GEMDOS asks the BIOS for its memory parameter block once at boot
 * (Getmpb, trap #13 function 0): remember where it is, to read the free
 * memory list from it
 */
void m68k_host_exception(unsigned int vector)
{
    if (vector != 45 || st.mpb != 0) {
        return;
    }
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint32_t args = (st_peek16(sp) & 0x2000) ? sp + 6 : m68k_get_reg(NULL, M68K_REG_USP);
    if (st_peek16(args) == 0) {
        st.mpb = st_peek32(args + 2);
    }
}

bool st_ram_free(uint32_t *total, uint32_t *largest)
{
    *total = *largest = 0;
    if (st.mpb == 0) {
        return false;
    }
    /* Memory descriptors: link, start, length, owner */
    uint32_t md = st_peek32(st.mpb);
    for (int n = 0; md != 0 && n < 1000; n++) {
        if (!st_ram_range(md, 16)) {
            return false;
        }
        uint32_t len = st_peek32(md + 8);
        *total += len;
        if (len > *largest) {
            *largest = len;
        }
        md = st_peek32(md);
    }
    return true;
}

/* Double bus fault: the 68000 stops until the next reset */
void m68k_host_halted(void)
{
    if (st.halted) {
        return;
    }
    st.halted = true;
    ST_LOGW("CPU halted (double bus fault): PC $%06lx, SR $%04x, SSP $%06lx, USP $%06lx, "
            "A0 $%08lx, D0 $%08lx, %lu bus errors",
            (unsigned long)(m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF),
            m68k_get_reg(NULL, M68K_REG_SR), (unsigned long)m68k_get_reg(NULL, M68K_REG_ISP),
            (unsigned long)m68k_get_reg(NULL, M68K_REG_USP),
            (unsigned long)m68k_get_reg(NULL, M68K_REG_A0),
            (unsigned long)m68k_get_reg(NULL, M68K_REG_D0), (unsigned long)st.bus_errors);
}

void st_update_irq(void)
{
    int level = mfp_irq() ? ST_IRQ_MFP :
                st.vbl_pending ? ST_IRQ_VBL :
                st.hbl_pending ? ST_IRQ_HBL : 0;
    if (level != st.irq_level) {
        bool raised = level > st.irq_level;
        st.irq_level = level;
        m68k_set_irq(level);
        /* Musashi only samples the IRQ lines when a timeslice starts */
        if (raised) {
            m68k_end_timeslice();
        }
    }
}

static int int_ack(int level)
{
    int vector = M68K_INT_ACK_AUTOVECTOR;

    switch (level) {
    case ST_IRQ_MFP:
        vector = mfp_ack();
        break;
    case ST_IRQ_VBL:
        st.vbl_pending = false;
        break;
    case ST_IRQ_HBL:
        st.hbl_pending = false;
        break;
    default:
        vector = M68K_INT_ACK_SPURIOUS;
        break;
    }
    st_update_irq();
    return vector;
}

/* RESET instruction: resets the peripherals, not the CPU */
static void reset_instruction(void)
{
    st_io_reset();
}

bool st_init(const st_config_t *config)
{
    const uint8_t *tos = config->tos;
    uint32_t base = ((uint32_t)tos[8] << 24) | (tos[9] << 16) | (tos[10] << 8) | tos[11];

    if (tos[0] != 0x60 || (base != 0xE00000 && base != 0xFC0000) ||
        (config->tos_size != 192 * 1024 && config->tos_size != 256 * 1024)) {
        ST_LOGW("unsupported TOS image (base $%06lx, %lu bytes)",
                (unsigned long)base, (unsigned long)config->tos_size);
        return false;
    }

    memset(&st, 0, sizeof(st));
    st.ram = config->ram;
    st.ram_size = config->ram_size;
    st.rom = config->rom;
    st.rom_base = base;
    st.rom_size = config->tos_size;
    st.boot_monitor = config->monitor;
    st.any_resolution = config->any_resolution;
    st.blitter = !config->no_blitter;
    st.start_resolution = config->start_resolution;
    st.resolution_changed = config->resolution_changed;
    st.desktop_resolution = -1;

    /* Word-swapped working copy of the ROM */
    for (uint32_t i = 0; i < st.rom_size; i += 2) {
        *(uint16_t *)(st.rom + i) = (tos[i] << 8) | tos[i + 1];
    }

    tos_patch_apply((tos[2] << 8) | tos[3], st.any_resolution, config->hd_path != NULL);
    audio_init();
    gemdos_init(config->hd_path, config->hd_boot);

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    if (config->opcode_tables != NULL) {
        m68k_use_table_copies(config->opcode_tables);
    }
    m68k_set_int_ack_callback(int_ack);
    m68k_set_reset_instr_callback(reset_instruction);

    ST_LOGI("TOS %x.%02x at $%06lx, %lu KB RAM, %s monitor%s", tos[2], tos[3],
            (unsigned long)base, (unsigned long)(st.ram_size / 1024),
            st.boot_monitor == ST_MONITOR_MONO ? "mono" : "color",
            st.any_resolution ? ", any resolution" : "");

    st_reset();
    return true;
}

/* STE: GPIP 7 is "monochrome monitor" (0 = mono) XOR "DMA sound active".
 * TOS reads the DMA sound state along with it and undoes the XOR.
 */
void st_update_gpip7(void)
{
    mfp_set_gpip(MFP_GPIP_MONO, (st.monitor != ST_MONITOR_MONO) ^ audio_dma_active());
}

void st_desktop_resolution(int rez)
{
    if (rez != st.desktop_resolution) {
        st.desktop_resolution = rez;
        ST_LOGI("desktop resolution %s", rez == 0 ? "low" : rez == 1 ? "medium" : "high");
        if (st.resolution_changed) {
            st.resolution_changed(rez);
        }
    }
}

void st_set_monitor(st_monitor_t monitor)
{
    if (monitor != st.monitor) {
        st.monitor = monitor;
        st_update_gpip7();
    }
}

void st_reset(void)
{
    memset(st.ram, 0, st.ram_size);
    st.first_gem_start = true;
    st.monitor = st.boot_monitor;

    /* TOS skips its memory sizing when these are valid, as after a warm
     * reset, so the MMU bank mirroring need not be emulated.
     */
    st.mmu_config = memory_controller_byte(st.ram_size);
    write_long(SV_MEMVALID, 0x752019F3);
    write_long(SV_MEMVAL2, 0x237698AA);
    write_long(SV_MEMVAL3, 0x5555AAAA);
    st.ram[SV_MEMCNTLR ^ 1] = st.mmu_config;
    write_long(SV_PHYSTOP, st.ram_size);

    st.hbl_pending = false;
    st.vbl_pending = false;
    st.irq_level = 0;
    st.frames = 0;
    st.bus_errors = 0;
    st.halted = false;
    st.mpb = 0;
    memset(&berr, 0, sizeof(berr));
    gemdos_reset();
    st_io_reset();
    m68k_set_irq(0);
    m68k_pulse_reset();
}

static void run_cycles(int cycles)
{
    if (st.halted) {
        /* A halted 68000 does nothing, not even take interrupts, until a
         * reset (Musashi would keep faulting on them)
         */
        st.cycle_in_line = 0;
        return;
    }
    /* cycle_in_line carries the overshoot of the previous line */
    while (st.cycle_in_line < cycles) {
        st.cpu_running = true;
        int used = m68k_execute(cycles - st.cycle_in_line);
        st.cpu_running = false;
        st.cycle_in_line += used;
    }
    st.cycle_in_line -= cycles;
}

void st_run_frame(void)
{
    video_start_frame();
    st.vbl_pending = true;

    for (st.line = 0; st.line < st.lines_per_frame; st.line++) {
        st.hbl_pending = true;
        st_update_irq();

        run_cycles(st.line_cycles);

        /* Display enable falls at the end of each visible line: Timer B event */
        int visible = st.line - st.first_visible_line;
        if (visible >= 0 && visible < st.visible_lines) {
            mfp_timer_event(MFP_TIMER_B);
        }
        mfp_advance(st.line_cycles);
        acia_advance(st.line_cycles);
        fdc_advance(st.line_cycles);
        audio_sync();
    }
    audio_frame_end();
    st.frames++;
}

void st_get_status(st_status_t *status)
{
    status->frames = st.frames;
    status->pc = m68k_get_reg(NULL, M68K_REG_PC);
    status->sr = m68k_get_reg(NULL, M68K_REG_SR);
    status->hz_200 = read_long(SV_HZ_200);
    status->vbclock = read_long(SV_VBCLOCK);
    status->bus_errors = st.bus_errors;
    status->halted = st.halted;
}
