# esp32ste: to do

In order: "what's next" is the first item here. Each one says why, where,
how, how it's checked, and roughly how big it is. Done items leave this
list (what they taught goes into CLAUDE.md); the others keep their
numbers. Smaller open points are in CLAUDE.md, "Open items".

1. **Extend the wall-clock regression across ROM variants.** Medium. Use the
   existing clock probe and `components/atari_st/tos_patch.c` with local non-US
   TOS 1.62 and 2.06 images. Verify patch signature handling, GEMDOS/XBIOS
   agreement, manual set across time sync and guest file timestamps under AHDI
   and the SD-folder drive.

2. **Define the ACSI image size boundary.** Small. Use sparse images around the
   board’s signed `off_t` limit to test the seek paths in
   `components/atari_st/acsi.c` and `components/atari_st/ahdi.c`. Verify clean
   rejection or correct last-sector access, unchanged existing bytes and
   agreement between the drive UI and web status.

3. **Compare 8 MHz floppy timing with a real loader.** Medium. Use a title with
   a repeatable disk-loading checkpoint to compare step, sector and index
   timing in `components/atari_st/fdc.c`. Keep GEMBench and AHDI boot as
   controls and check that the 16 MHz option preserves the peripherals’ timing.
