/*
 * Floppy disk images for drives A: and B:.
 *
 *   .ST   raw sectors, track by track (side 0, side 1, ...). Kept open and
 *         read and written sector by sector; write-protected if the file is
 *         read-only.
 *   .MSA  Magic Shadow Archiver: run-length coded tracks. Unpacked once
 *         into a hidden raw copy next to it (".NAME.MSA.st"), used
 *         write-protected (changes could not be saved in the MSA).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "diskspace.h"
#include "st_internal.h"

#define SECTOR          512
#define MAX_TRACKS      86
#define MAX_SPT         22      /* up to HD geometry */

typedef struct {
    char path[200];
    FILE *file;                 /* .ST */
    uint8_t *data;              /* .MSA, unpacked */
    bool write_protected;
    int tracks, sides, spt;
} disk_t;

static disk_t disks[2];

static uint16_t le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static uint16_t be16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

static bool has_extension(const char *path, const char *ext)
{
    size_t len = strlen(path), elen = strlen(ext);
    if (len < elen) {
        return false;
    }
    for (size_t i = 0; i < elen; i++) {
        if (tolower((unsigned char)path[len - elen + i]) != ext[i]) {
            return false;
        }
    }
    return true;
}

/* Geometry of a raw image: the boot sector's BPB if it fits the file size,
 * else the usual formats by size.
 */
static bool st_geometry(const uint8_t *boot, long size, disk_t *d)
{
    int spt = le16(boot + 0x18), sides = le16(boot + 0x1A);
    if (spt >= 8 && spt <= MAX_SPT && sides >= 1 && sides <= 2 &&
        size % (SECTOR * spt * sides) == 0) {
        int tracks = size / (SECTOR * spt * sides);
        if (tracks >= 1 && tracks <= MAX_TRACKS) {
            d->spt = spt;
            d->sides = sides;
            d->tracks = tracks;
            return true;
        }
    }
    static const int spts[] = { 9, 10, 11, 18, 8 };
    for (sides = 2; sides >= 1; sides--) {
        for (size_t i = 0; i < sizeof(spts) / sizeof(spts[0]); i++) {
            long track_bytes = (long)SECTOR * spts[i] * sides;
            if (size % track_bytes == 0 && size / track_bytes >= 78 &&
                size / track_bytes <= MAX_TRACKS) {
                d->spt = spts[i];
                d->sides = sides;
                d->tracks = size / track_bytes;
                return true;
            }
        }
    }
    return false;
}

static bool open_st(disk_t *d, const char *path)
{
    d->file = fopen(path, "r+b");
    if (d->file == NULL) {
        d->file = fopen(path, "rb");
        d->write_protected = true;
    }
    if (d->file == NULL) {
        return false;
    }
    uint8_t boot[SECTOR];
    fseek(d->file, 0, SEEK_END);
    long size = ftell(d->file);
    fseek(d->file, 0, SEEK_SET);
    if (size < SECTOR || fread(boot, 1, SECTOR, d->file) != SECTOR || !st_geometry(boot, size, d)) {
        ST_LOGW("%s: not a floppy image (%ld bytes)", path, size);
        fclose(d->file);
        d->file = NULL;
        return false;
    }
    return true;
}

/* Where an MSA image's unpacked copy goes: ".<name>.st" next to it (hidden
 * from the disk lists and from C:)
 */
void st_floppy_unpacked_path(const char *path, char *out, size_t len)
{
    const char *slash = strrchr(path, '/');
    int dir = slash ? (int)(slash - path) + 1 : 0;
    snprintf(out, len, "%.*s.%s.st", dir, path, path + dir);
}

/* Unpack one track: $E5 <byte> <count.w> repeats a byte, anything else is
 * literal; a track of full length is stored as is
 */
static void msa_unpack(const uint8_t *packed, int len, uint8_t *out, int track_bytes)
{
    if (len == track_bytes) {
        memcpy(out, packed, track_bytes);
        return;
    }
    int o = 0;
    for (int i = 0; i < len && o < track_bytes;) {
        if (packed[i] == 0xE5 && i + 3 < len) {
            int n = be16(packed + i + 2);
            while (n-- > 0 && o < track_bytes) {
                out[o++] = packed[i + 1];
            }
            i += 4;
        } else {
            out[o++] = packed[i++];
        }
    }
    memset(out + o, 0, track_bytes - o);
}

/* MSA: unpacked once into a raw copy on the same card and read from there
 * (an unpacked disk would take up to 900 KB of memory); in memory only if
 * that copy can't be written
 */
static bool open_msa(disk_t *d, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    uint8_t hdr[10];
    bool ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && be16(hdr) == 0x0E0F;
    int spt = be16(hdr + 2), sides = be16(hdr + 4) + 1;
    int first = be16(hdr + 6), last = be16(hdr + 8);
    ok = ok && spt >= 1 && spt <= MAX_SPT && sides <= 2 && first <= last && last < MAX_TRACKS;
    if (!ok) {
        ST_LOGW("%s: not an MSA image", path);
        fclose(f);
        return false;
    }
    d->spt = spt;
    d->sides = sides;
    d->tracks = last + 1;
    d->write_protected = true;
    int track_bytes = spt * SECTOR;
    long size = (long)d->tracks * sides * track_bytes;

    char copy[sizeof(d->path) + 8];
    st_floppy_unpacked_path(path, copy, sizeof(copy));
    struct stat ms, cs;
    if (stat(path, &ms) == 0 && stat(copy, &cs) == 0 && cs.st_size == size &&
        cs.st_mtime >= ms.st_mtime) {
        fclose(f);
        d->file = fopen(copy, "rb");
        return d->file != NULL;
    }

    FILE *out = fopen(copy, "wb");
    if (out == NULL) {
        d->data = calloc(d->tracks * sides, track_bytes);
    }
    uint8_t *packed = malloc(track_bytes);
    uint8_t *track = malloc(track_bytes);
    ok = (out != NULL || d->data != NULL) && packed != NULL && track != NULL;

    for (int t = 0; ok && t <= last; t++) {
        for (int s = 0; ok && s < sides; s++) {
            memset(track, 0, track_bytes);
            if (t >= first) {
                uint8_t lenb[2];
                int len = fread(lenb, 1, 2, f) == 2 ? be16(lenb) : -1;
                if (len <= 0 || len > track_bytes || (int)fread(packed, 1, len, f) != len) {
                    ok = false;
                    break;
                }
                msa_unpack(packed, len, track, track_bytes);
            }
            if (out != NULL) {
                ok = fwrite(track, 1, track_bytes, out) == (size_t)track_bytes;
            } else {
                memcpy(d->data + (t * sides + s) * track_bytes, track, track_bytes);
            }
        }
    }
    free(packed);
    free(track);
    fclose(f);
    if (out != NULL) {
        ok = fclose(out) == 0 && ok;
        if (ok) {
            d->file = fopen(copy, "rb");
            ok = d->file != NULL;
        } else {
            remove(copy);
        }
    }
    if (!ok) {
        ST_LOGW("%s: damaged MSA image", path);
        free(d->data);
        d->data = NULL;
        return false;
    }
    return true;
}

void st_floppy_eject(int drive)
{
    if (drive < 0 || drive > 1) {
        return;
    }
    disk_t *d = &disks[drive];
    bool had = d->file != NULL || d->data != NULL;
    if (d->file) {
        fclose(d->file);
    }
    free(d->data);
    memset(d, 0, sizeof(*d));
    if (had) {
        fdc_disk_changed(drive);
    }
}

bool st_floppy_insert(int drive, const char *path)
{
    if (drive < 0 || drive > 1) {
        return false;
    }
    st_floppy_eject(drive);
    disk_t *d = &disks[drive];
    bool ok = has_extension(path, ".msa") ? open_msa(d, path) : open_st(d, path);
    if (!ok) {
        memset(d, 0, sizeof(*d));
        return false;
    }
    snprintf(d->path, sizeof(d->path), "%s", path);
    ST_LOGI("drive %c: %s, %d tracks, %d side(s), %d sectors%s", 'A' + drive, path,
            d->tracks, d->sides, d->spt, d->write_protected ? ", write-protected" : "");
    fdc_disk_changed(drive);
    return true;
}

const char *st_floppy_path(int drive)
{
    if (drive < 0 || drive > 1 || disks[drive].path[0] == 0) {
        return NULL;
    }
    return disks[drive].path;
}

bool st_floppy_write_protected(int drive)
{
    return drive >= 0 && drive <= 1 && floppy_present(drive) && disks[drive].write_protected;
}

/* ---- for the controller ------------------------------------------------- */

bool floppy_present(int drive)
{
    return disks[drive].file != NULL || disks[drive].data != NULL;
}

bool floppy_write_protected(int drive)
{
    return disks[drive].write_protected;
}

int floppy_sectors_per_track(int drive)
{
    return floppy_present(drive) ? disks[drive].spt : 0;
}

static long sector_offset(const disk_t *d, int track, int side, int sector)
{
    if (track < 0 || track >= d->tracks || side >= d->sides || sector < 1 || sector > d->spt) {
        return -1;
    }
    return ((long)(track * d->sides + side) * d->spt + sector - 1) * SECTOR;
}

bool floppy_read_sector(int drive, int track, int side, int sector, uint8_t *buf)
{
    disk_t *d = &disks[drive];
    long off = floppy_present(drive) ? sector_offset(d, track, side, sector) : -1;
    if (off < 0) {
        return false;
    }
    if (d->data) {
        memcpy(buf, d->data + off, SECTOR);
        return true;
    }
    return fseek(d->file, off, SEEK_SET) == 0 && fread(buf, 1, SECTOR, d->file) == SECTOR;
}

bool floppy_write_sector(int drive, int track, int side, int sector, const uint8_t *buf)
{
    disk_t *d = &disks[drive];
    long off = floppy_present(drive) ? sector_offset(d, track, side, sector) : -1;
    if (off < 0 || d->write_protected) {
        return false;
    }
    if (d->data) {
        memcpy(d->data + off, buf, SECTOR);
        return true;
    }
    bool ok = fseek(d->file, off, SEEK_SET) == 0 && fwrite(buf, 1, SECTOR, d->file) == SECTOR;
    fflush(d->file);
    fsync(fileno(d->file));
    return ok;
}

/* ---- free space --------------------------------------------------------- */

static bool image_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    const disk_t *d = ctx;
    uint64_t size = (uint64_t)d->tracks * d->sides * d->spt * SECTOR;
    if (offset > size || len > size - offset) {
        return false;
    }
    if (d->data) {
        memcpy(buf, d->data + offset, len);
        return true;
    }
    return fseek(d->file, (long)offset, SEEK_SET) == 0 && fread(buf, 1, len, d->file) == len;
}

/* The disk's FAT counted now (the floppy code seeks before each sector) */
bool st_floppy_space(int drive, uint64_t *free_bytes, uint64_t *size)
{
    if (drive < 0 || drive > 1 || !floppy_present(drive)) {
        return false;
    }
    disk_t *d = &disks[drive];
    diskspace_disk_t disk = { image_read, d, (uint64_t)d->tracks * d->sides * d->spt * SECTOR };
    diskspace_t s;
    if (!diskspace_fat(&disk, 0, &s)) {
        return false;
    }
    *free_bytes = s.free;
    *size = s.size;
    return true;
}

/* ---- blank disks -------------------------------------------------------- */

/* 720 KB: 80 tracks, 2 sides, 9 sectors, as the TOS desktop formats it */
bool st_floppy_create(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ST_LOGW("%s: can't create", path);
        return false;
    }
    static bool seeded;
    if (!seeded) {
        srand((unsigned)time(NULL) ^ (unsigned)clock());
        seeded = true;
    }

    uint8_t s[SECTOR];
    bool ok = true;
    for (int n = 0; n < 1440 && ok; n++) {
        memset(s, 0, sizeof(s));
        if (n == 0) {
            static const uint8_t bpb[] = {
                0x00, 0x02,     /* 0b bytes per sector */
                0x02,           /* 0d sectors per cluster */
                0x01, 0x00,     /* 0e reserved sectors */
                0x02,           /* 10 FATs */
                0x70, 0x00,     /* 11 root directory entries: 112 */
                0xA0, 0x05,     /* 13 sectors: 1440 */
                0xF9,           /* 15 media */
                0x05, 0x00,     /* 16 sectors per FAT */
                0x09, 0x00,     /* 18 sectors per track */
                0x02, 0x00,     /* 1a sides */
                0x00, 0x00,     /* 1c hidden sectors */
            };
            s[0] = 0xE9;        /* not a 68000 BRA: not executable */
            memcpy(s + 2, "ESP32 ", 6);
            int serial = rand();
            s[8] = serial;
            s[9] = serial >> 8;
            s[10] = serial >> 16;
            memcpy(s + 0x0B, bpb, sizeof(bpb));
            /* TOS runs a boot sector whose words sum to $1234: avoid that */
            uint16_t sum = 0;
            for (int i = 0; i < SECTOR; i += 2) {
                sum += be16(s + i);
            }
            if (sum == 0x1234) {
                s[0x1E] ^= 1;
            }
        } else if (n == 1 || n == 6) {
            s[0] = 0xF9;        /* FAT 1 and 2: media byte, cluster 1 reserved */
            s[1] = 0xFF;
            s[2] = 0xFF;
        }
        ok = fwrite(s, 1, SECTOR, f) == SECTOR;
    }
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        ST_LOGW("%s: write failed", path);
        remove(path);
    }
    return ok;
}
