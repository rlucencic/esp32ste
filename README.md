<h1 align="center">esp32ste</h1>

<p align="center">
  <b>An Atari STE on a 7-inch ESP32-S3 touch-screen board or an M5Stack Tab5.</b><br>
  68000 at 8 MHz (on the Tab5 also 16 MHz or as fast as it goes), TOS 1.62 / 2.06, up to 4 MB, floppies, hard disk, Bluetooth keyboard and mouse.
</p>

<p align="center">
  <img alt="ESP32-S3" src="https://img.shields.io/badge/ESP32--S3-N16R8-e7352c">
  <img alt="ESP32-P4" src="https://img.shields.io/badge/ESP32--P4-M5Stack%20Tab5-e7352c">
  <img alt="ESP-IDF" src="https://img.shields.io/badge/ESP--IDF-v5.5-blue">
  <img alt="LVGL" src="https://img.shields.io/badge/LVGL-9.4-5e48e8">
  <img alt="68000" src="https://img.shields.io/badge/CPU-Musashi%2068000-555">
  <img alt="MIT licence" src="https://img.shields.io/badge/licence-MIT-green">
</p>

<p align="center">
  <img src="docs/images/setup.png" width="49%" alt="Setup screen: TOS and memory choice, WiFi, Bluetooth and floppy buttons">
  <img src="docs/images/gembench.png" width="49%" alt="GEMBench 6.31 at 8 MHz: 107% of a real STE">
  <br>
  <img src="docs/images/gembench-16mhz.png" width="49%" alt="GEMBench 6.31 with the 68000 at 16 MHz on the Tab5: 205% of a real STE">
  <img src="docs/images/gembench-fastest.png" width="49%" alt="GEMBench 6.31 on Fastest on the Tab5: 385% of a real STE">
  <br>
  <sub>GEMBench 6.31 (TOS 1.62, high resolution, 1 MB): 107% of an STE at 8 MHz, 205% at
  16 MHz and 385% on Fastest, on the M5Stack Tab5 in real time.</sub>
</p>

esp32ste turns the Makerfabs MaTouch ESP32-S3 Parallel TFT with Touch 7", or an
M5Stack Tab5, into an Atari STE. The ST picture fills the screen's height, a Bluetooth
keyboard and mouse stand in for the ST's own, the microSD card is hard disk
C:, and floppy disk images go in drives A: and B:. A setup screen on the
touch panel, and a web page on your network, handle everything else.

It's the sibling of [esp32mac](https://github.com/rlucencic/esp32mac)
(Macintosh Plus, SE, Classic), [esp32cpc](https://github.com/rlucencic/esp32cpc)
(Amstrad CPC), [esp32zx](https://github.com/rlucencic/esp32zx) (ZX
Spectrum), [esp32cbm](https://github.com/rlucencic/esp32cbm) (Commodore
64 and 128) and [esp32xl](https://github.com/rlucencic/esp32xl) (Atari
800XL and 130XE), on the same boards; all six are built on the shared
runtime [esp32emu](https://github.com/rlucencic/esp32emu).

## Features

**The machine**
- 68000 ([Musashi](https://github.com/kstenerud/Musashi) core) at **8 MHz** like an STE. On the
  Tab5 also faster, like an accelerator board: **16 MHz**, or **Fastest**, as
  fast as it keeps real time (16–20 MHz in GEM, far more in tight loops). The
  rest of the machine keeps the STE's timing; the choice applies at once,
  no restart. The MaTouch has no time to spare for it and stays at 8 MHz
- Real-time speed at 8 MHz: 100% on the Tab5, about 80–85% on the MaTouch
- TOS 1.62 or 2.06, chosen on the setup screen
- 512 KB, 1 MB or 4 MB of RAM
- Low, medium and high resolution on one screen, like a multisync monitor;
  Set Preferences offers all three, and the last one is remembered
- Blitter, YM2149 and STE DMA sound, MFP 68901, IKBD with the clock set from the internet

**Disks**
- **Hard disk C:** the SD card, at file level: copy programs onto the card
  from any computer, no disk image or driver needed
- **Floppy drives A: and B:** `.ST` images (read and written) and `.MSA`
  images (read-only), with a WD1772 controller that TOS can format disks on
- **New blank disks** from the setup screen: `EMPTY0001.ST`, `EMPTY0002.ST`, …
- A disk in A: at power-on boots, as games expect

**Around it**
- **Setup screen** before the ST starts, and any time while it runs (hold
  the top-right corner for 2 seconds): WiFi, Bluetooth pairing, TOS, RAM,
  CPU speed (Tab5), floppy disks, reset
- **Bluetooth keyboard and mouse** (BLE HID), with battery levels; on the
  Tab5 also M5Stack's Tab5 Keyboard
- **Web page** at `http://atari-ste.local`: drop `.ST`, `.MSA` or `.ZIP` files
  onto it to put them on the SD card, and watch the machine, drives, board
  and Bluetooth devices live
- **Sound** on the board's speaker: on the Tab5 its built-in one (on by
  default), on the MaTouch an optional speaker on its amplifier output
  (`menuconfig`)

<p align="center">
  <img src="docs/images/web.png" width="80%" alt="The web page: Atari, drives, board and Bluetooth status above the floppy disk list">
  <br><sub>The web page (example data)</sub>
</p>

## Hardware

| | Makerfabs MaTouch ESP32-S3 7" | M5Stack Tab5 |
|---|---|---|
| Chip | ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB octal PSRAM | ESP32-P4 (RISC-V, 360 MHz): 16 MB flash, 32 MB PSRAM; an ESP32-C6 for WiFi and Bluetooth |
| Screen | 7" 1024×600 RGB, GT911 capacitive touch | 5" 1280×720 MIPI-DSI, capacitive touch, held landscape |
| Storage | microSD card (FAT32 or exFAT) | microSD card (FAT32 or exFAT) |
| Input | any BLE keyboard and mouse | any BLE keyboard and mouse, or the Tab5 Keyboard |
| Sound | a speaker on the MAX98357A output (optional, `menuconfig`) | the built-in speaker (ES8388) |

On the MaTouch the ST picture is 960×600, on the Tab5 1152×720 (high
resolution: 640×400 shown 1.8 times), centred on the panel. The setup
screen is the same on both, laid out for each panel; the Tab5's has the CPU
speed choice too.

<p align="center">
  <img src="docs/images/tab5-setup.png" width="49%" alt="The setup screen on the Tab5's 1280×720 panel: TOS, memory and CPU speed choice">
  <img src="docs/images/tab5-desktop.png" width="49%" alt="The ST's high-resolution desktop on the Tab5, 1152×720 between black borders">
  <br><sub>On the M5Stack Tab5: the setup screen, and the ST's desktop on the whole 1280×720 picture</sub>
</p>

## Getting started

### 1. What you need

- [ESP-IDF](https://github.com/espressif/esp-idf) v5.5 (developed with v5.5.4)
- TOS ROM images, which are copyrighted and **not included**: dumps of your
  own STE's TOS 1.62 and/or TOS 2.06 (256 KB each). Put them in `roms/` as
  `tos162.img` and `tos206.img`.
- A microSD card, FAT32 or exFAT
- [esp32emu](https://github.com/rlucencic/esp32emu), the shared board runtime, checked out next to
  this project (`../esp32emu`)

### 2. Build and flash

```sh
git clone https://github.com/rlucencic/esp32emu.git     # the shared runtime, next to it
git clone https://github.com/rlucencic/esp32ste.git && cd esp32ste
idf.py build
idf.py -p /dev/cu.usbmodem… flash      # app and both TOS images

# or for the M5Stack Tab5 (its own build folder and settings):
idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-tab5/sdkconfig build
idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=build-tab5/sdkconfig -p /dev/cu.usbmodem… flash
```

Later updates only need `idf.py -p … app-flash` (on the Tab5 with the same
`-B build-tab5 …` options). The Tab5 keeps the ST's settings and disks in its
own flash; WiFi networks are remembered by its ESP32-C6.

### 3. First start

1. The board starts on the **setup screen**. Tap **WiFi** to join your
   network: the clock is set from the internet, and the web page becomes
   reachable.
2. Tap **Bluetooth**, put your keyboard or mouse into pairing mode, and tap
   it in the list. Paired devices reconnect by themselves from then on.
3. Choose **TOS**, **memory** and, on the Tab5, the **CPU** speed, then tap **Start**.

## Using it

| To | Do |
|---|---|
| Get back to the setup screen | Hold the top-right corner of the screen for 2 s |
| Change TOS or RAM | On the setup screen; **Restart** applies it |
| Change the CPU speed (Tab5) | On the setup screen, at once: 8 MHz for exact STE timing (games, demos), 16 MHz or Fastest for GEM and applications |
| Insert a floppy | Setup screen → **Floppy** → **Insert disk**, or **Insert into…** on the web page |
| Make a blank floppy | Setup screen → **Floppy** → **New empty disk** |
| Copy disk images from a computer | Open `http://atari-ste.local` and drop them onto the page (ZIP files are unpacked) |
| …or from the command line | `curl -T game.st http://atari-ste.local/disks/` |
| Use programs from the SD card | Copy them anywhere on the card; they're on drive C: |

Disk images live in the `floppies` folder on the SD card. The inserted disks
are remembered across restarts.

<p align="center">
  <img src="docs/images/floppy.png" width="60%" alt="Floppy page: drives A: and B: with Insert disk, New empty disk and Eject">
</p>

## Configuration

`idf.py menuconfig` → **Atari STE emulator**:

| Option | Default | |
|---|---|---|
| TOS, RAM | 1.62, 1 MB | Starting values; the setup screen's choice is remembered |
| Monochrome monitor | off | Start in high resolution |
| Offer all resolutions | on | Low, medium and high in Set Preferences |
| SD card as hard disk C: | on | Optionally only a folder of the card; boot from C: |
| Floppy folder | `floppies` | Where disk images are kept |

and → **Emulator board** (the shared runtime):

| Option | Default | |
|---|---|---|
| WiFi, setup screen | on | Time zone for the clock (POSIX TZ) |
| Start by itself | off | Countdown on the setup screen |
| Web page, network name | on, `atari-ste` | `http://<name>.local` |
| BLE keyboard and mouse | on | Mouse speed divider |
| Sound on the speaker | MaTouch off, Tab5 on | Volume; on the MaTouch it moves the console to the UART port |

## How it works

```mermaid
flowchart LR
  subgraph core1 [Core 1]
    EMU["Emulation<br>68000 · MFP · shifter · blitter<br>WD1772 · YM2149 · DMA sound · IKBD"]
  end
  subgraph core0 [Core 0]
    PRES[Presenter<br>ST screen → 960×600 / 1152×720]
    UI[Setup screen<br>LVGL]
    NET[WiFi · web page · mDNS]
    BLE[Bluetooth HID]
  end
  PSRAM[(PSRAM<br>ST RAM · framebuffer)]
  SD[(microSD<br>C: · floppies/)]
  EMU <--> PSRAM
  PRES --> LCD[1024×600 or 1280×720 panel]
  PSRAM --> PRES
  BLE -- keys, mouse --> EMU
  EMU <--> SD
  NET <--> SD
```

- **`components/atari_st`**: the STE, platform independent. It also builds
  on a desktop (`components/atari_st/host`) for testing with scripted mouse
  and keyboard input, screenshots and CPU traces.
- **`main`**: the STE on the board: memory, TOS, settings, keyboard map,
  floppy drives and status.
- **[esp32emu](https://github.com/rlucencic/esp32emu)** (shared with
  [esp32mac](https://github.com/rlucencic/esp32mac),
  [esp32cpc](https://github.com/rlucencic/esp32cpc),
  [esp32zx](https://github.com/rlucencic/esp32zx),
  [esp32cbm](https://github.com/rlucencic/esp32cbm) and
  [esp32xl](https://github.com/rlucencic/esp32xl)): the board runtime for
  both boards (display, touch, SD card, WiFi, Bluetooth, setup screen, web
  page, serial console; on the Tab5 its ESP32-C6 radio, keyboard and
  charging) and the 68000 core, Musashi from
  [evansm7's fork](https://github.com/evansm7/Musashi) with static opcode
  tables and the 68000 bus error frame.

On the Tab5 the 68000 can run faster than the rest of the machine: per scanline it gets
the STE's 8 MHz worth of cycles times the CPU speed, while video, timers,
sound, the blitter and the floppy keep counting the STE's bus cycles. On
Fastest the board measures each frame and raises the speed as long as the
frame still fits in its time.

TOS is patched in RAM, with every patch checked against the expected bytes:
all resolutions in Set Preferences, no wait for an ACSI hard disk when C: is
the SD card, and no ROM checksum test. Drive C: works at GEMDOS level through
a small cartridge program, so programs on the SD card run without a disk
image or hard disk driver.

## Development

```sh
make -C components/atari_st/host                     # desktop build of the STE
components/atari_st/host/build/st_host roms/tos206.img 600 out.ppm 1024
ST_CPU=16 components/atari_st/host/build/st_host roms/tos206.img 600 out.ppm   # the 68000 at 16 MHz

python ../esp32emu/tools/emu_console.py /dev/cu.usbmodem… lcd screen.png   # screenshot of the panel
python ../esp32emu/tools/emu_console.py /dev/cu.usbmodem… log 10           # board log
curl http://atari-ste.local/api/status                        # live status as JSON
```

`CLAUDE.md` has the details: architecture, memory budget, debugging options
and the lessons learned along the way.

## Limitations

- The M5Stack Tab5 port is new: GEMBench runs there in real time at 8 and
  16 MHz, but sound, touch, Bluetooth and the Tab5 Keyboard have not been
  tried on it by hand yet.
- `.STX` (Pasti) images of copy-protected originals don't work; `.MSA`
  images are read-only.
- No FPU, ACSI, MIDI or Mega STE hardware.
- Floppy access is faster than a real drive, which a few loaders may notice.
- On the MaTouch, about 80–85% of real speed on the desktop, and demanding
  demos run slower. It has no time to spare for a faster 68000 (16 MHz ran
  at about 45% of real time), so it stays at 8 MHz. The Tab5 keeps 100% at
  8 and 16 MHz.
- 16 MHz and Fastest (Tab5) change a program's timing as an accelerator would:
  games and demos that count on an 8 MHz 68000 can misbehave.

## Credits

- [Musashi](https://github.com/kstenerud/Musashi) by Karl Stenerud (MIT
  licence), via Matt Evans' [fork](https://github.com/evansm7/Musashi) for
  embedded use
- [LVGL](https://lvgl.io) for the setup screen
- [ESP-IDF](https://github.com/espressif/esp-idf) and NimBLE
- The Atari ST community's documentation of the hardware, and
  [Hatari](https://www.hatari-emu.org) as the reference for how it behaves

## Licence

[MIT](LICENSE). The Musashi core in esp32emu is MIT licensed too (see
`../esp32emu/components/musashi/musashi/readme.txt`). LVGL (MIT) and ESP-IDF (Apache 2.0)
are fetched at build time and keep their own licences.

Atari, ST and TOS are trademarks of their respective owners. This project
contains no Atari code; you need your own TOS ROM images.
