# Hard disks (ACSI): drivers and new hard disk images

The STE has two ACSI hard disks, `ACSI 0` and `ACSI 1`, each an image file
(`.IMG`, `.HD`) in the SD card's `floppies` folder (`components/atari_st/acsi.c`).
The web page uploads and inserts them. The "New" button makes only 720 KB
floppies, though. This page says why, what works today, and how new hard
disks could come with a driver already installed (TODO.md, item 1).

## Why "New" makes no hard disks

A new hard disk image needs two things before TOS can use it:

1. **Partitions and filesystems**: an Atari root sector with a partition
   table, and an empty FAT16 filesystem in each partition. The emulator
   could write these itself.
2. **A driver**: TOS has none for hard disks. At power-on, TOS runs the
   code in the first root sector whose words add up to `$1234`: the
   driver's small loader, which loads the driver from the disk. The driver
   then mounts the partitions as C:, D:, ...

The drivers and their loaders are other people's software. AHDI and the
loader that its HINSTALL writes belong to Atari; HDDRIVER belongs to
Uwe Seimet. This project is public and MIT-licensed, so the firmware can't
carry them (for the same reason the TOS images aren't in the repo). Until
2026-11-16 the only driver tried was HDDRIVER's demo, which can't format
or partition and writes slowly on purpose, so `main.c` offers blank disks
for the floppy drives only (`disk_takes_blank()`).

## Drivers tried (2026-11-16)

Both work with the ACSI emulation. Measured in the host build, in
emulated frames (50 Hz), with a small test program that writes 256 KB to
`C:\TEST.DAT` in 32 KB pieces and reads it back:

| Driver | Writes | Reads | Notes |
|---|---|---|---|
| AHDI 6.061 (Atari) | 256 KB in 13-16 frames, ~1 MB/s | 9-10 frames | One 64-sector WRITE(6) per 32 KB piece. Desktop at frame 110 (TOS 1.62), booting from the disk |
| HDDRIVER 13.02 demo | ~19 frames per 32 KB, ~70 KB/s | 5 frames | The same commands, with deliberate waits: its download page says writing "is significantly slower than with the full version". Writes only to C: and its RAM disk. Desktop at frame 248 with its 10 s banner skipped |

AHDI 6.061 booted from its own disk in TOS 1.62 and 2.06 (4 MB too) in
the host build, and in TOS 1.62 on the Tab5. There, C: showed
`SHDRIVER.SYS`, Save Desktop wrote `C:\DESKTOP.INF`, and the SD card's
folder was D:.

Where they come from: AHDI 6.061 (`AHDI.PRG`) and the AHDI 5.00 package
(HDX, HINSTALL) from the Atari Wiki's
[AHDI page](https://atari-wiki.co.uk/index.php?title=AHDI); the HDDRIVER
demo image (`hddrdemo130_scsi.img`) from
[hddriver.net](https://hddriver.net/en/demo.html).

## The SD card's folder and C:

On the boards the SD card is always a GEMDOS drive too (`gemdos.c`, the
cartridge). When a hard disk image boots, the emulator counts its
partitions and gives the folder the next letter
(`acsi_boot_partitions()`, `st_hd_drive()`). When no image boots, the
folder is C:. So AHDI started from a boot floppy's AUTO folder mounts its
partition, but C: stays the folder: a file written to C: lands on the SD
card. **On the boards a hard disk must boot from its own root sector.**

## Making a bootable AHDI disk by hand

How the image in the tests above was made:

1. **The image, on the Mac.** A 16 MB file (32768 sectors), all zero,
   then:

   | Where | What |
   |---|---|
   | Sector 0, `$1C2` | Total sectors (long, big-endian): 32768 |
   | Sector 0, `$1C6` | Partition 0: flag `$01` (exists), ID `GEM`, first sector 2, size 32766 (longs, big-endian). HINSTALL makes the flag `$81` (boots) |
   | Sector 0, `$1D2`, `$1DE`, `$1EA` | Partitions 1-3: empty |
   | Sector 0, `$1F6`, `$1FA` | Bad sector list: first sector 1, length 1 |
   | Sector 1 | An empty bad sector list (its format was a guess; AHDI didn't check it) |
   | Sector 2 | The partition's boot sector, little-endian BPB at `$0B`: 512 bytes a sector, 2 sectors a cluster, 1 reserved, 2 FATs, 256 root entries, 32766 sectors, media `$F8`, 64 sectors a FAT |
   | Sectors 3 and 67 | The two FATs, each starting `F8 FF FF FF` (FAT16: TOS reads every hard disk partition with 16-bit FATs) |

   `GEM` partitions are under 16 MB (32768 sectors of 512 bytes). Bigger
   ones are `BGM`, with logical sectors of 1024 to 8192 bytes.

2. **AHDI from a floppy.** A 720 KB floppy with `AUTO\AHDI.PRG` (6.061),
   and at its root HINSTALL.PRG, HINSTALL.RSC and `SHDRIVER.RAW`. The last
   is the driver HINSTALL copies; in the 5.00 package it's the same file as
   AHDI.PRG, so here it's AHDI 6.061 renamed. Boot TOS from it with the
   image in ACSI 0 and no SD folder (host build: no `ST_HD`). AHDI mounts
   the partition as C:.
3. **HINSTALL.** Start it from the floppy's window, then File › Install,
   select C:, OK, and OK again at "About to install a bootable driver in
   logical drive C:". It writes its loader into the root sector (the words
   then add up to `$1234`), marks partition 0 bootable, and copies
   `SHDRIVER.RAW` to `C:\SHDRIVER.SYS`.

Scripting it in the host build: the drive buttons in HINSTALL's dialog are
radio buttons, and the AES shows a click only ~20 frames later (it waits
for a possible double click); a click on OK within that time is lost.

## New hard disks with a driver: two ways

### A. Copy the driver from a bootable disk on the card (recommended)

"New" for ACSI 0/1 formats the image itself (step 1 above, in C) and then
copies the driver from a bootable AHDI disk the user already has in the
`floppies` folder (like the one made above):

- the root sector's loader (bytes `$000`-`$1BF`), with the checksum word
  made again so the words add up to `$1234`; partition 0 flagged `$81`;
- `SHDRIVER.SYS` from that disk's boot partition into the new one's.

The loader reads the partition table when it runs, so it works with any
partitions written the same way; HINSTALL itself writes the same bytes
every time. No Atari code goes into the repo: the user uploads one
bootable AHDI disk once. Without one on the card, the page says so and
offers floppies only.

The same could take HDDRIVER's loader and `HDDRIVER.SYS` from a full
version's disk (untried).

### B. A loader of our own

A root sector loader written for this project (MIT; about 400 bytes of
68000 code: read sectors through the DMA chip, find the driver in the
boot partition's FAT16 root directory, load and relocate it, run it),
with `AHDI.PRG` taken from the SD card. It needs no ready-made disk, and
it boots on a real ST too, but it's clearly more work: written and
assembled by hand (no 68000 assembler is installed), and tested against
what TOS's DMA boot passes to it.

### Either way

- **Where**: `main.c` (`disks_config_t`: hard disk sizes in `sizes`,
  `takes_blank()` true for ACSI, `default_size()`, `extension_for()` giving
  `.IMG`, `create()` choosing floppy or hard disk by size) and a formatter
  in `components/atari_st` (beside `acsi.c`), so the host build can test
  it.
- **Sizes**: what TOS allows per partition, from the Atari Wiki's AHDI
  page: 256 MB with TOS 1.x, 512 MB with TOS 2.0x. The images live on the
  boards' SD cards, which have room; files over 2 GB are untried there
  (32-bit `off_t`).
- **Checks**: in the host build, a new disk of each size boots TOS 1.62
  and 2.06, AHDI mounts every partition, the folder comes after them, the
  write test passes and the free space is right (`st_floppy_space()`'s
  counterpart for hard disks, if the page shows it); then on the Tab5 and
  the MaTouch from the web page.
