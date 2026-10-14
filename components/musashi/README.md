# musashi — 68000 CPU core

[Musashi](https://github.com/kstenerud/Musashi) (MIT licence, see `musashi/readme.txt`)
configured as a plain 68000 for the Atari STE emulator.

## Source

Vendored from Matt Evans' fork, branch `umac-hacks`
(https://github.com/evansm7/Musashi, commit `b3144f1`). The fork adds embedded
options on top of upstream; pico-mac uses it to run a Mac on an RP2040:

- `M68K_DYNAMIC_INSTR_TABLES OFF`: the 64K-entry opcode jump table is generated
  at build time and kept in flash, not built at runtime in 256 KB of RAM
- `M68K_FIXED_CPU_TYPE`: the CPU model is fixed at compile time, so code for
  other models is dropped
- `M68K_FAST_FUNC`: an attribute hook, mapped to `IRAM_ATTR` here

## Local patches (`patches/`, applied on top of `umac-hacks`)

1. `0001` upstream fix b2425b5: `m68k_execute()` returned the wrong cycle count
   after `m68k_end_timeslice()`.
2. `0002` changes for the emulator:
   - `m68kmake` also generates a static `m68ki_static_cycles` table, so cycle
     counting works with static tables. The 68000 row is identical to the
     table upstream builds at runtime.
   - Fixed the fork's runtime builder (`SET_CPU_CYCLECOUNT` wrote to the wrong
     index).
   - 68000 bus errors push the 7-word group 0 frame (the fork only had the 68010
     frame). They also skip the per-instruction register snapshot, because a
     68000 does not restore registers.
   - New `m68k_bus_error(address, write)` raises a bus error from inside a
     memory callback.
   - The fast opcode fetch raises an address error on an odd PC again.
3. `0003` a double bus/address fault halts the CPU without the MAME `$FFFF01`
   read. On the ST that address raises another bus error, so the halt
   recursed until the stack overflowed.

### Changed in place, not yet in `patches/`

- `M68K_HALT_CALLBACK()` (in `m68kcpu.h`, both double-fault halts): tells
  the ST that the CPU halted (`st_cpu_halted()`), so the emulation stops
  running it and logs the fault once.
- `M68K_VECTOR_CALLBACK(v)` (in `m68ki_jump_vector()`): every exception,
  after its frame is pushed. The ST uses it to find GEMDOS's memory block
  (`Getmpb`, trap #13) for the free RAM display.
- `m68k_use_table_copies(buf)` (in `m68kcpu.c`, needs `<string.h>`): runs
  from copies of the opcode jump table and the 68000 cycle table in `buf`
  (0x50000 bytes). On the ESP32 read-only data stays in flash, so the
  tables are copied to PSRAM at start; reading them from flash cost about
  25% of the emulation speed.

`m68kconf.h` turns on `M68K_INSTRUCTION_HOOK` only when built with
`-DM68K_INSTRUCTION_HOOK=2`. The host harness in `components/atari_st/host`
uses this for its trace options.

## Layout

| Path | What |
|---|---|
| `m68kconf.h` | Emulator configuration (replaces upstream's) |
| `musashi/` | Vendored core sources, `m68k_in.c` and the `m68kmake.c` generator |
| `generated/` | `m68kops.c/h` produced by `m68kmake`, committed |
| `tools/regen_ops.sh` | Rebuild `generated/` after changing `m68k_in.c` or `m68kmake.c` |
| `test/` | Host tests, run with `make -C components/musashi/test` |

## Host interface

The application provides the memory callbacks declared in `m68k.h`:
`m68k_read_memory_8/16/32`, `m68k_write_memory_8/16/32` and
`m68k_read_disassembler_16/32`. It must also set:

- `m68k_set_int_ack_callback()`: return a vector number (MFP) or
  `M68K_INT_ACK_AUTOVECTOR` (HBL/VBL)
- `m68k_set_reset_instr_callback()`: the `RESET` instruction
