# Hard disks (ACSI): drivers and new hard disk images

The STE has two ACSI hard disks, `ACSI 0` and `ACSI 1`, each an image file
(`.IMG`, `.HD`) in the SD card's `st` folder (`components/atari_st/acsi.c`).
The web page uploads, inserts and creates them. Each new hard disk has one
FAT16 partition with an installed AHDI driver.

## Create a bootable hard disk

1. Prepare and flash a local copy of Atari's AHDI 6.061 as
   [`roms/ahdi.img`](../roms/README.md#atari-ahdi-6061). If the board
   has no valid flash template, it uses a bootable `AHDI.IMG` in the
   SD card's `st` folder.
2. Open or reload the web page. Choose 32, 128 or 256 MiB under **New blank disk**
   and click **Make**. The new file appears as `EMPTYnnnn.IMG`. The
   setup screen's **New** button on ACSI 0 or 1 makes a 32 MiB image and
   mounts it in that drive.
3. Insert the image in an ACSI drive and reset the emulator. Use ACSI 0
   for the boot disk. When ACSI 0 boots, a second image on ACSI 1 takes
   the next hard disk letter.

The formatter copies the loader from both the disk root sector and the
partition boot sector. It writes a new partition table and FAT16 BIOS
parameter block, recalculates both boot checksums, and copies
`SHDRIVER.SYS` by following the source FAT chain. It reads the template
from the dedicated flash data partition or from the SD card. Without a
valid template, the web page explains how to provide one and offers only
floppies. [Atari hard disk layout reference](https://info-coach.fr/atari/documents/_mydoc/Atari_HD_File_Sytem_Reference_Guide_v1.1b.pdf)

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

## Validation

The 32, 128 and 256 MiB images booted from ACSI 0 under TOS 1.62 and
2.06. In each case a guest program wrote and read back 256 KiB, and the
change in TOS's free-space reading matched the file size. The host
directory followed the ACSI partition as D:. A 128 MiB disk on ACSI 1
also passed as D: with an ACSI 0 boot disk and the host directory at E:.
With TOS 2.06 and an empty ACSI 0, booting from ACSI 1 halted with both
the original AHDI template and the generated image. The formatter also
copied a fragmented template driver byte-for-byte and removed the
incomplete output when given a cyclic FAT chain.

The 512 KiB flash template itself booted under TOS 1.62 and 2.06.
All three disk sizes made from it matched the output from the
original 16 MiB Atari AHDI template byte-for-byte.

With no template available, the disk creation API offered only floppies.
With the template in flash and no SD copy, both boards offered all three
ACSI sizes and produced a 32 MiB image that matched the host byte-for-byte.
The Tab5 setup screen also created and mounted a 32 MiB disk on ACSI 1
with the flash template alone.

In the SD-template tests, the Tab5 made a 32 MiB image in 12 seconds over
the web and the result matched the host output byte-for-byte. The setup
screen **New** button created and mounted a second 32 MiB image on ACSI 1.
Both TOS versions booted with two ACSI partitions and the SD folder at E:.

The MaTouch made a 128 MiB image in 80 seconds over the web and created
and mounted a 32 MiB image on ACSI 1 in 21 seconds from setup. TOS 2.06
booted from the 128 MiB disk on ACSI 0 and mapped the SD folder to E:.
The formatter writes from internal DMA-capable memory to avoid the
ESP32-S3's sector-at-a-time fallback for PSRAM buffers.
