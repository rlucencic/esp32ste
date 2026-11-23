# Local Atari ROMs and AHDI driver

Put your own TOS dumps here (they are copyrighted and not part of this
repository):

| File | TOS | Size |
|---|---|---|
| `tos162.img` | 1.62 | 256 KB |
| `tos206.img` | 2.06 | 256 KB |

`idf.py flash` writes them to the `tos162` and `tos206` flash partitions.
Either one is enough, but then choose that TOS on the setup screen: it
always offers both, and the ST won't start from a missing image.

## Atari AHDI 6.061

To create bootable ACSI disks with Atari's driver, prepare
`roms/ahdi.img` from an AHDI 6.061 disk with the boot driver already
installed. The preparation command copies the boot code and driver into
a 512 KiB image. See the [hard disk guide](../docs/hard-disks.md#making-a-bootable-ahdi-disk-by-hand)
for the HINSTALL steps.

Run these commands from the repository root.

```sh
make -C components/atari_st/host build/st_host
components/atari_st/host/build/st_host --prepare-ahdi /path/to/AHDI6061.IMG roms/ahdi.img
idf.py -p PORT flash
```

Use the same build directory and board options as for your normal flash.
A full flash writes the local file to the `ahdi` data partition.
The image is ignored by Git and stays separate from the application binary.
The firmware uses the flash copy first and falls back to a bootable
`st/AHDI.IMG` on the SD card.
