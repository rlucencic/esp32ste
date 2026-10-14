/*
 * Musashi configuration for the Atari STE emulator (68000 @ 8 MHz).
 *
 * Replaces Musashi's stock m68kconf.h. See musashi/m68kconf.h upstream for
 * the full description of every switch.
 *
 * Musashi is Copyright Karl Stenerud, MIT licence (see musashi/readme.txt).
 */
#ifndef M68KCONF__HEADER
#define M68KCONF__HEADER

#define OPT_OFF             0
#define OPT_ON              1
#define OPT_SPECIFY_HANDLER 2

#define M68K_COMPILE_FOR_MAME       OPT_OFF

/* ---- CPU model: plain 68000 only, selected at compile time ---- */
#define M68K_EMULATE_010            OPT_OFF
#define M68K_EMULATE_EC020          OPT_OFF
#define M68K_EMULATE_020            OPT_OFF
#define M68K_EMULATE_030            OPT_OFF
#define M68K_EMULATE_040            OPT_OFF
#define M68K_EMULATE_PMMU           OPT_OFF
#define M68K_FIXED_CPU_TYPE         CPU_TYPE_000

/* ---- Bus ---- */
#define M68K_SEPARATE_READS         OPT_OFF
#define M68K_SIMULATE_PD_WRITES     OPT_OFF
#define M68K_EMULATE_FC             OPT_OFF
#define M68K_SET_FC_CALLBACK(A)     your_set_fc_handler_function(A)

/* ---- Exceptions ---- */
/* MFP (level 6) uses vectored interrupts, HBL/VBL (2/4) are autovectored:
 * the host answers through m68k_set_int_ack_callback().
 */
#define M68K_EMULATE_INT_ACK        OPT_ON
#define M68K_INT_ACK_CALLBACK(A)    your_int_ack_handler_function(A)

/* RESET instruction resets the ST peripherals: m68k_set_reset_instr_callback() */
#define M68K_EMULATE_RESET          OPT_ON
#define M68K_RESET_CALLBACK()       your_reset_handler_function()

/* Trace mode is used by debuggers and some copy protections (e.g. Copylock) */
#define M68K_EMULATE_TRACE          OPT_ON

/* Odd-address word/long accesses raise address errors ("3 bombs") */
#define M68K_EMULATE_ADDRESS_ERROR  OPT_ON

/* Bus errors raised from memory callbacks via m68k_bus_error() ("2 bombs") */
#define M68K_BUS_ERR_ENABLE         OPT_ON

#define M68K_EMULATE_BKPT_ACK       OPT_OFF
#define M68K_BKPT_ACK_CALLBACK()    your_bkpt_ack_handler_function()
#define M68K_EMULATE_PREFETCH       OPT_OFF

/* ---- Hooks (all off: they cost time on every instruction) ---- */
#define M68K_CMPILD_HAS_CALLBACK    OPT_OFF
#define M68K_CMPILD_CALLBACK(v,r)   your_cmpild_handler_function(v,r)
#define M68K_RTE_HAS_CALLBACK       OPT_OFF
#define M68K_RTE_CALLBACK()         your_rte_handler_function()
#define M68K_TAS_HAS_CALLBACK       OPT_OFF
#define M68K_TAS_CALLBACK()         your_tas_handler_function()
/* Illegal opcodes $000A-$000C inside the GEMDOS cartridge call the emulator */
#define M68K_ILLG_HAS_CALLBACK      OPT_SPECIFY_HANDLER
#define M68K_ILLG_CALLBACK(opcode)  st_illegal_opcode(opcode)

/* A double bus/address fault halts the CPU, as on a real 68000: tell the ST */
#define M68K_HALT_CALLBACK()        st_cpu_halted()
void st_cpu_halted(void);

/* Every exception, once its stack frame is pushed (the ST watches traps) */
#define M68K_VECTOR_CALLBACK(v)     st_exception(v)
void st_exception(unsigned int vector);
int st_illegal_opcode(int opcode);
#define M68K_MONITOR_PC             OPT_OFF
#define M68K_SET_PC_CALLBACK(A)     your_pc_changed_handler_function(A)
/* Host debug builds turn this on with -DM68K_INSTRUCTION_HOOK=2 */
#ifndef M68K_INSTRUCTION_HOOK
#define M68K_INSTRUCTION_HOOK       OPT_OFF
#endif
#define M68K_INSTRUCTION_CALLBACK(pc) st_instruction_hook(pc)
void st_instruction_hook(unsigned int pc);

#define M68K_LOG_ENABLE             OPT_OFF
#define M68K_LOG_1010_1111          OPT_OFF
#define M68K_LOG_FILEHANDLE         some_file_handle

/* ---- Build options ---- */
#define M68K_DASM_ENABLE            OPT_ON

/* Decode and cycle tables are generated at build time (const, in flash)
 * instead of 320 KB of RAM at runtime.
 */
#define M68K_DYNAMIC_INSTR_TABLES   OPT_OFF
#define M68K_CYCLE_COUNTING         OPT_ON

#define M68K_USE_64_BIT             OPT_ON

/* Hot core functions run from IRAM */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define M68K_FAST_FUNC(x)           IRAM_ATTR x
#endif

#endif /* M68KCONF__HEADER */
