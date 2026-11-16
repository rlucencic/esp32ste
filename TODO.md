# esp32ste: to do

In order: "what's next" is the first item here. Each one says why, where,
how, how it's checked, and roughly how big it is. Done items leave this
list (what they taught goes into CLAUDE.md); the others keep their
numbers. Smaller open points are in CLAUDE.md, "Open items".

## 1. New hard disk images with a driver installed

Why: the web page's "New" makes only floppies. A new hard disk needs
partitions, filesystems and a driver with its boot loader, and the driver
(AHDI, HDDRIVER) can't be in this public MIT repo. Until now the only way
to a hard disk was an image made elsewhere; on the boards it must also
boot from its own root sector, or the SD card's folder keeps C:. AHDI
6.061 now works (about 14 times faster writes than HDDRIVER's demo), and
a bootable AHDI disk can be made by hand. Details, measurements and the
layout: `docs/hard-disks.md`.

Where: `main/main.c` (`disks_config_t`: hard disk sizes, `takes_blank()`,
`default_size()`, `extension_for()`, `create()`), a formatter beside
`components/atari_st/acsi.c`.

How (recommended, A in the doc): "New" for ACSI 0/1 writes the root
sector, partitions and empty FAT16 filesystems itself, then copies the
loader (root sector `$000`-`$1BF`, checksum made again to `$1234`) and
`SHDRIVER.SYS` from a bootable AHDI disk the user keeps in the `floppies`
folder. Without one there, it offers floppies only and says why. The
alternative (B): a loader of our own (MIT, ~400 bytes of hand-assembled
68000) loading `AHDI.PRG` from the card: no ready-made disk needed, more
work.

To decide: the sizes offered (e.g. 32, 128, 512 MB), one partition or
several (up to 256 MB each with TOS 1.x, 512 MB with 2.0x), how the
source disk is chosen if there are several, and A or B.

Checked: host build, a new disk of each size boots TOS 1.62 and 2.06,
AHDI mounts every partition, the folder comes after them, a 256 KB write
and read pass, the free space is right; then from the web page on the
Tab5 and the MaTouch.

Size: about a session for A (most of it the formatter and its tests),
more for B.
