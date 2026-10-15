# esp32ste: Atari STE emulator on the Makerfabs MaTouch ESP32-S3 7"

An Atari STE (68000, TOS 1.62 / 2.06, up to 4 MB) on an ESP32-S3 board with
a 1024x600 touch LCD. Bluetooth keyboard and mouse, the SD card as hard disk
C:, floppy disk images in drives A: and B:, an LVGL setup screen, and a web
page for disk images and live status. ESP-IDF v5.5.4, C.

The board side (tasks, LCD, touch, SD, WiFi, BLE, setup screen, web page,
console) and the 68000 core are the shared library in `../esp32emu`, also
used by `../esp32mac` (Macintosh). See its CLAUDE.md; changes there need
both projects built.

## Hardware

Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7", ESP32-S3-WROOM-1-N16R8:
16 MB flash (QIO 80 MHz), 8 MB octal PSRAM (80 MHz), 1024x600 RGB565 panel
(16 MHz pixel clock, no frame memory of its own), GT911 touch (I2C 0x5D),
microSD (SDMMC 1-bit), MAX98357A I2S amplifier. Pins are in
`../esp32emu/components/emu/include/emu/board.h`.
The board's native USB port is the console (USB-Serial-JTAG), port
`/dev/cu.usbmodem3101` on the user's Mac.

## Build, flash, debug

The shell's default Python environment doesn't match the one the build was
configured with, so always build like this:

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py build && idf.py -p /dev/cu.usbmodem3101 app-flash'
```

- `app-flash` writes only the app. `idf.py flash` also writes the TOS images
  from `roms/` (not in the repo) into partitions `tos162` and `tos206`.
- After editing `sdkconfig.defaults`, also edit `sdkconfig` (it exists and
  wins), then `idf.py reconfigure`.
- `../esp32emu/tools/emu_console.py PORT log N | screenshot F.png | lcd F.png | send "cmd" ... | info | reset`.
  Run it with that venv's Python (`.../venv/bin/python -I`); system Python
  has no pyserial. Opening the port keeps DTR high so the board doesn't reset.
- `CONFIG_ST_*` options for WiFi, web, setup screen, BLE and speaker became
  `CONFIG_EMU_*` (menu "Emulator board"); `main/sdkconfig.rename` carries
  old values over.
- Status from the board over WiFi: `curl http://atari-ste.local/api/status`.
- Flashing restarts the board and ends the user's ST session (disks and
  settings come back). The user is often using the board: say so first.

Console commands (`../esp32emu/components/emu/include/emu/console.h`): `s` /
`L` dump the ST screen / the whole LCD (base64, read by the tool), `r` cold
reset, `F <drive> [name|+]` insert / eject / make a blank disk, `t x y [ms]`
simulated touch (use it to drive the setup screen remotely; one tap per
`send` call), `i` machine state; the ST's own (in `main/main.c`): `k <hex>`
tap a key, `m dx dy btn` mouse, `R <tos 0|1> <ram KB>` restart with that
TOS/RAM. Sending several commands needs ~0.1 s between them through the
tool; a GEM double-click needs them faster (write to the port directly).

## Layout

| Path | What |
|---|---|
| `main/main.c` | The STE as an `emu_machine_t`: memory, TOS, settings, key map, floppies, status, console commands |
| `main/settings.c`, `tos_rom.c` | NVS settings, TOS partitions |
| `components/atari_st/` | The STE itself, platform independent (`include/st.h`) |
| `components/atari_st/host/` | Host build of the STE core for testing on the Mac |
| `../esp32emu/components/emu/` | Board runtime: tasks, LCD, touch, SD, WiFi, BLE, setup screen, web page, console |
| `../esp32emu/components/musashi/` | 68000 core (Musashi, evansm7 fork); see its README |
| `../esp32emu/tools/emu_console.py` | Console client |
| `roms/` | `tos162.img`, `tos206.img` (copyrighted, not distributed) |
| `partitions.csv` | nvs, phy, factory 8 MB, tos162 + tos206 (256 KB each, type 0x40), storage |

## Architecture

- The runtime in `../esp32emu` (`emu_run()`) owns the tasks: emulation on
  core 1 (`st_run_frame()` per frame, paced by `esp_timer` or the speaker),
  presenter (ST screen scaled to 960x600 at x=32, nearest pixel, only when
  `st_screen_signature()` changes), console, stats, supervisor, WiFi,
  NimBLE and HTTP on core 0. See its CLAUDE.md.
- `main/main.c` plugs the ST in: `reserve_memory()` (settings, ST RAM + ROM
  copy + opcode tables, first thing), `storage()` (C: on the SD card),
  `start()` (TOS from its partition, `st_init()`), the setup screen choices
  (TOS, RAM), the floppy drives (`disks_config_t`), key map (HID usages ->
  ST scancodes, both Controls/Alternates on one ST key), status rows and
  console commands; `every_second()` saves the desktop resolution.
- **Setup screen** (shared, LVGL 9.4): status (TOS, RAM, WiFi, time),
  networks, password, Bluetooth, pairing, floppy, disk images. While the ST
  runs it's opened by the corner long-press, the emulator pauses, and
  changing TOS/RAM restarts the ST ("Restart Atari"), "Reset Atari"
  cold-resets it. `CONFIG_EMU_AUTOSTART` is off by default.

### The emulator core (`components/atari_st`)

- RAM/ROM stored with each 16-bit word in host order (byte n at index n^1).
- `st.c` frame loop: per scanline run CPU cycles, Timer B, MFP, ACIA, FDC,
  audio. GLUE interrupts, bus errors, halt handling, `st_ram_free()`.
- `mem.c` memory map, `io.c` I/O map (unmapped I/O raises bus errors; TOS
  probes hardware that way), `mfp.c`, `acia.c` (IKBD, clock from `time()`),
  `video.c` (multisync: the monitor follows the mode), `blitter.c`,
  `psg.c` + `audio.c` (YM and DMA sound, rendered on another task),
  `fdc.c` (WD1772 + DMA, timed commands, write track for formatting,
  disk-change via the write-protect sensor), `floppy.c` (images),
  `gemdos.c` (C: drive), `tos_patch.c`.
- **TOS patches** (`tos_patch.c`, on the RAM copy, each checked against the
  expected bytes first): all resolutions in Set Preferences (TOS 1.62 and
  2.06, with an AES routine using native opcodes to report and adjust the
  resolution), no ACSI boot scan (1.62) / no hard disk spin-up wait (2.06)
  when C: is present, no ROM CRC check (2.06: the patched copy failed it).
- **C: drive** (`gemdos.c`): a cartridge at `$FA0000` (init with ca_init bit
  3) installs a GEMDOS trap hook; native calls are illegal opcodes
  `$000A`-`$000E` honoured only inside the cartridge. Files map to the SD
  card with 8.3 names. Programs start with TOS's help: Pexec 5 (basepage) +
  native load/relocate, then the caller's own call is rewritten into
  Pexec 6 and handed to TOS (see the lessons). A disk in A: at power-on
  makes A: the boot drive instead of C:.
- **Floppies** (`floppy.c`): `.ST` read and written through to the file
  (write-protected if read-only); `.MSA` unpacked once into a hidden copy
  `.<name>.st` next to it and used read-only. Blank disks: 720 KB,
  random serial.

### Persistent state

- NVS `atari`: `tos` (0/1), `ram_kb`, `rez` (last desktop resolution).
- NVS `floppy`: `disk_a`, `disk_b` (file names), `empty_n` (last EMPTYnnnn).
- NVS `blehid`: `known` (paired devices); NimBLE keeps bonds itself. Shared
  with the Mac firmware (same NVS partition): pairings and WiFi survive
  switching between them.
- WiFi credentials: ESP-IDF's own NVS storage.
- SD card: C: is the whole card (`CONFIG_ST_HD_FOLDER`), disk images in
  `floppies/` (made if missing), `EMPTY0001.ST` ... for new blank disks.

### Web (shared, http://atari-ste.local)

See `../esp32emu/README.md` for the API: status, upload / download /
delete (`/disks/<name>`, `/floppies/<name>` too), insert and eject
(`/api/drive/<n>`), blank disks. Uploads are limited to 4 MB for the ST.
mDNS hostname from `CONFIG_EMU_HOSTNAME`. The page unpacks `.zip` in the
browser (`DecompressionStream`).

## Memory budget (measured)

- PSRAM: 8 MB; code copied to PSRAM takes ~1.3 MB, leaving a 6,784 KB heap.
  ST RAM (up to 4 MB) + ROM copy (256 KB) + opcode tables (320 KB) are
  allocated first. The rest needs ~1.7 MB at most (LCD framebuffer 1.23 MB,
  WiFi, BLE, LVGL while the setup screen is open); `PSRAM_FOR_THE_REST` is
  2 MB. With 4 MB: ~750 KB free while running, ~600 KB with the setup
  screen open. `reserve_memory()` caps the RAM to what fits; switching to a
  bigger size later restarts the board, which then reserves it first and
  goes straight into the ST (`s_start_after_restart` in RTC memory).
- Internal RAM is tight (67-100 KB free seen): WiFi before BLE, WiFi IRAM
  options off, few static RX buffers, `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`,
  80 KB reserved for DMA/internal. LVGL's heap is PSRAM (`lv_mem_psram.c`,
  `CONFIG_LV_USE_CUSTOM_MALLOC`).

## Lessons (each one cost a debugging session)

Board-level ones (core 0 interrupts and the LCD, internal RAM, mDNS, SD
buffers, screen dumps, the serial port) are in `../esp32emu/CLAUDE.md`.

- **Read-only data is in flash, not PSRAM** (`SPIRAM_RODATA` off) to make
  room for 4 MB. Musashi's opcode tables are then slow (60% speed instead
  of 85%), hence `m68k_use_table_copies()` with a PSRAM buffer. Also, flash
  writes (NVS) now briefly stall both cores.
- **ST RAM must be allocated first** at power-on; later, PSRAM is too
  fragmented for 4 MB in one block. Don't overwrite `s_cfg` wholesale after
  `reserve_memory()` (that caused a restart loop).
- **Musashi `m68k_set_reg(M68K_REG_SR)` does not swap stack pointers.** Set
  ISP/USP/A7 explicitly when changing mode from outside.
- **GEMDOS saves a supervisor-mode caller's registers on the system stack,
  which the child process's interrupts overwrite.** Starting a program with
  a nested Pexec from the cartridge corrupted the parent's context (GEMBench
  4.03's float test via `BIN\AUTOFPU.BIN`, and quitting any C: program).
  Fixed by rewriting the caller's own Pexec into mode 6 and passing it on.
- **A halted CPU must stay halted**: Musashi kept taking interrupts and
  faulting; the ST now stops running the CPU (`st.halted`) until a reset.
- **Bus error logging**: each new address once (24), then a summary every
  ~5 s. A probe loop otherwise floods the log.
- **TOS 2.06 checks its ROM CRC** in the first 80 s after power-on (always,
  in emulation): patched out, else "BAD ROM CRC" twice.

## Testing

- **Host build** of the STE core: `make -C components/atari_st/host`, then
  `build/st_host <tos.img> [frames] [out.ppm] [ram_kb] [mono]`. Runs far
  faster than real time. Environment options: `ST_ANYRES`, `ST_REZ=0|1|2`,
  `ST_HD=<folder>` (C:), `ST_FLOPPY_A/B=<image>`, `ST_NEWDISK=<path>`,
  `ST_NOBLIT`, `ST_WAV=<file>`, `ST_QUIET`, `ST_SCRIPT=<file>`; debugging:
  `ST_GEMDOS=<frame>` (trace GEMDOS calls), `ST_TRACE=<pc>:<n>` +
  `ST_TRACE_FRAME`, `ST_REGS`, `ST_STOP=<pc>[:<frame>]`, `ST_WATCH=<addr>` +
  `ST_WATCH_TRAIL`, `ST_RING=<n>`, `ST_DIS=<addr>`, `ST_VEC=<n>`,
  `ST_BLITLOG`. Script lines: `<frame> mouse dx dy btn | key <hex> 0|1 |
  shot <file.ppm> | dump <file> | insert <drive> <path> | eject <drive>`
  (arguments up to 63 characters, up to 4096 lines). The mouse is relative:
  move by -900,-900 first to reach the top-left corner; GEM drags need slow
  movement.
- **GEMBench** (6.31 from exxosforum.co.uk/atari/last/GB6/, 4.03 on the
  "Delta Labs 165" disk at downloads.atari-home.de) runs in the host build
  from C: or A:. GEMBench 6.31, TOS 1.62, high res: 107% average.
- **On the board:** status via the web API, screenshots via the console,
  the setup screen via `t x y` touches. Things that only the user can see
  (the panel itself, touch feel, audio) need them to check.
- Speed: about 83-85% of a real STE on the TOS 2.06 desktop in high
  resolution, 77-81% with TOS 1.62.

## Open items

- `.STX` (Pasti) images are not supported; `.MSA` is read-only.
- No FPU, no ACSI, no MIDI, no Mega STE features.
- Musashi changes from the last session are not yet in `patches/` (see its
  README).
- Floppy timing is about 10x faster than a real drive (2 ms per sector).
- Not a git repository yet. Licence: MIT (`LICENSE`), like Musashi.

## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing.
