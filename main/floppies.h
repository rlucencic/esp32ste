#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Floppy disk images in the floppies folder on the SD card
 * (CONFIG_ST_FLOPPY_FOLDER), for drives A: (0) and B: (1). The inserted
 * disks are remembered in NVS and go back in at the next start.
 *
 * Insert, eject and create only while the emulation is paused or not
 * started yet.
 */

/* Make the folder if needed; false without an SD card */
bool floppies_init(void);

bool floppies_ready(void);

/* Put the remembered disks back in */
void floppies_restore(void);

/* Image file names (.ST, .MSA) in the folder, sorted; free with
 * floppies_free_list()
 */
int floppies_list(char ***names);
void floppies_free_list(char **names, int count);

bool floppies_insert(int drive, const char *name);
void floppies_eject(int drive);

/* File name of the disk in the drive, NULL when empty */
const char *floppies_name(int drive);

/* Path of the floppies folder (no trailing slash) */
const char *floppies_folder(void);

/* `name` is a plain file name of a disk image (.ST, .MSA): no folders */
bool floppies_valid_name(const char *name);

/* `name` is in drive A: or B: */
bool floppies_in_use(const char *name);

/* Remove the unpacked copy of an .MSA image (when it's deleted or replaced) */
void floppies_remove_unpacked(const char *name);

/* Blank formatted disk EMPTYnnnn.ST (next free number) put into the
 * drive; its name in `name`
 */
bool floppies_create(int drive, char *name, size_t len);
