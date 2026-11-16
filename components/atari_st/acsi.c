/*
 * ACSI hard disks: raw disk images (512-byte sectors) as devices 0 .. 1 on
 * the hard disk port, through the DMA chip (fdc.c).
 *
 * With the DMA chip's HDC select the 68000 writes a command a byte at a
 * time, the first one (A0 low) carrying the device number in its top three
 * bits. The device answers each byte with an interrupt (MFP GPIP 5, shared
 * with the floppy controller); after the last it moves the data through the
 * DMA and interrupts again, and the status byte is read back. A device with
 * no image doesn't answer, and the driver times out, as on an empty bus.
 *
 * Commands: the 6-byte SCSI ones ACSI drives knew (TEST UNIT READY, REQUEST
 * SENSE, READ, WRITE, INQUIRY, MODE SENSE, ...) and ICD's extended form (a
 * first byte of $1F, then a whole SCSI command), which drivers use for READ
 * CAPACITY and 10-byte READ and WRITE beyond 1 GB.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include "st_internal.h"

#define SECTOR          512
#define MS              (ST_CPU_HZ / 1000)
#define COMMAND_TIME    (MS / 8)        /* a command's own time */
#define BYTE_CYCLES     2               /* ~4 MB/s through the DMA */
#define LIGHT_FRAMES    10              /* a device's light stays on after a command */
#define ICD_EXTENDED    0x1F

/* Status bytes */
#define GOOD            0x00
#define CHECK_CONDITION 0x02

/* Errors: sense key and additional sense code (also the old 4-byte sense
 * format's error code)
 */
typedef enum {
    E_NONE, E_OPCODE, E_ADDRESS, E_FIELD, E_LUN, E_PROTECTED, E_READ, E_WRITE,
} acsi_error_t;

static const struct {
    uint8_t key, code;
} errors[] = {
    [E_NONE]      = { 0x0, 0x00 },
    [E_OPCODE]    = { 0x5, 0x20 },      /* invalid command */
    [E_ADDRESS]   = { 0x5, 0x21 },      /* block out of range */
    [E_FIELD]     = { 0x5, 0x24 },      /* invalid field in the command */
    [E_LUN]       = { 0x5, 0x25 },      /* no such logical unit */
    [E_PROTECTED] = { 0x7, 0x27 },      /* write-protected */
    [E_READ]      = { 0x3, 0x11 },      /* unrecovered read error */
    [E_WRITE]     = { 0x3, 0x0C },      /* write error */
};

typedef struct {
    char path[200];
    FILE *file;
    uint32_t sectors;
    bool write_protected;
    acsi_error_t sense;                      /* for the next REQUEST SENSE */
    uint32_t reads, writes;             /* sectors, since power-on (status displays) */
    uint32_t last_frame;                /* of the last command (the light) */
    bool used;
} disk_t;

static disk_t disks[ST_ACSI_DEVICES];

bool st_debug_acsi;                     /* log each command (the host build's ST_ACSILOG) */

/* The command on the bus */
static struct {
    int device;                         /* -1: none */
    uint8_t bytes[16];
    int count, length;                  /* received, expected (0: not known yet) */
    int busy;                           /* cycles to its end, 0 = idle */
    uint8_t status;
} cmd = { .device = -1 };

static uint8_t buffer[SECTOR];

/* ---- images ------------------------------------------------------------- */

void st_acsi_eject(int device)
{
    if (device < 0 || device >= ST_ACSI_DEVICES) {
        return;
    }
    disk_t *d = &disks[device];
    if (d->file) {
        fclose(d->file);
        ST_LOGI("ACSI %d: empty", device);
    }
    uint32_t reads = d->reads, writes = d->writes;
    memset(d, 0, sizeof(*d));
    d->reads = reads;
    d->writes = writes;
}

bool st_acsi_insert(int device, const char *path)
{
    if (device < 0 || device >= ST_ACSI_DEVICES) {
        return false;
    }
    st_acsi_eject(device);
    disk_t *d = &disks[device];
    bool write_protected = false;
    FILE *f = fopen(path, "r+b");
    if (f == NULL) {
        f = fopen(path, "rb");
        write_protected = true;
    }
    if (f == NULL) {
        return false;
    }
    off_t size = fseeko(f, 0, SEEK_END) == 0 ? ftello(f) : -1;
    if (size < SECTOR || size / SECTOR > 0xFFFFFFFF) {
        ST_LOGW("%s: not a hard disk image (%lld bytes)", path, (long long)size);
        fclose(f);
        return false;
    }
    d->file = f;
    d->sectors = size / SECTOR;
    d->write_protected = write_protected;
    snprintf(d->path, sizeof(d->path), "%s", path);
    ST_LOGI("ACSI %d: %s, %lu sectors (%lu MB)%s", device, path, (unsigned long)d->sectors,
            (unsigned long)(d->sectors / 2048), write_protected ? ", write-protected" : "");
    return true;
}

const char *st_acsi_path(int device)
{
    if (device < 0 || device >= ST_ACSI_DEVICES || disks[device].file == NULL) {
        return NULL;
    }
    return disks[device].path;
}

bool st_acsi_write_protected(int device)
{
    return st_acsi_path(device) != NULL && disks[device].write_protected;
}

uint64_t st_acsi_size(int device)
{
    return st_acsi_path(device) != NULL ? (uint64_t)disks[device].sectors * SECTOR : 0;
}

void st_acsi_status(st_acsi_status_t *s)
{
    s->light = -1;
    for (int i = 0; i < ST_ACSI_DEVICES; i++) {
        s->reads[i] = disks[i].reads;
        s->writes[i] = disks[i].writes;
        if (s->light < 0 && disks[i].used && st.frames - disks[i].last_frame < LIGHT_FRAMES) {
            s->light = i;
        }
    }
}

bool acsi_present(void)
{
    for (int i = 0; i < ST_ACSI_DEVICES; i++) {
        if (disks[i].file) {
            return true;
        }
    }
    return false;
}

static bool read_sector(disk_t *d, uint32_t lba, uint8_t *buf)
{
    return fseeko(d->file, (off_t)lba * SECTOR, SEEK_SET) == 0 && fread(buf, 1, SECTOR, d->file) == SECTOR;
}

static bool write_sector(disk_t *d, uint32_t lba, const uint8_t *buf)
{
    return fseeko(d->file, (off_t)lba * SECTOR, SEEK_SET) == 0 && fwrite(buf, 1, SECTOR, d->file) == SECTOR;
}

/* ---- the partitions a driver will mount --------------------------------- */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* An Atari partition entry (flag, id, start, size): 1 for a GEM or BGM
 * partition, 2 for an XGM link, 0 for anything else
 */
static int atari_entry(const uint8_t *e)
{
    if (!(e[0] & 1)) {
        return 0;
    }
    if (!memcmp(e + 1, "GEM", 3) || !memcmp(e + 1, "BGM", 3)) {
        return 1;
    }
    return !memcmp(e + 1, "XGM", 3) ? 2 : 0;
}

static int atari_partitions(disk_t *d, const uint8_t *root)
{
    int n = 0;
    uint8_t ext[SECTOR];
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = root + 0x1C6 + 12 * i;
        int kind = atari_entry(e);
        if (kind == 1) {
            n++;
        } else if (kind == 2) {
            /* An extended chain: each link a partition and the next link
             * (relative to the first)
             */
            uint32_t first = be32(e + 4), link = first;
            for (int k = 0; k < 64 && read_sector(d, link, ext); k++) {
                if (atari_entry(ext + 0x1C6) == 1) {
                    n++;
                }
                if (atari_entry(ext + 0x1C6 + 12) != 2) {
                    break;
                }
                link = first + be32(ext + 0x1C6 + 12 + 4);
            }
        }
    }
    /* ICD's eight more entries */
    for (int i = 0; n > 0 && i < 8; i++) {
        if (atari_entry(root + 0x156 + 12 * i) == 1) {
            n++;
        }
    }
    return n;
}

static bool fat_type(uint8_t type)
{
    return type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0E;
}

static int dos_partitions(disk_t *d, const uint8_t *mbr)
{
    if (mbr[0x1FE] != 0x55 || mbr[0x1FF] != 0xAA) {
        return 0;
    }
    int n = 0;
    uint8_t ebr[SECTOR];
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + 0x1BE + 16 * i;
        if (fat_type(e[4])) {
            n++;
        } else if (e[4] == 0x05 || e[4] == 0x0F) {
            uint32_t first = le32(e + 8), link = first;
            for (int k = 0; k < 64 && read_sector(d, link, ebr); k++) {
                if (fat_type(ebr[0x1BE + 4])) {
                    n++;
                }
                const uint8_t *next = ebr + 0x1BE + 16;
                if (next[4] != 0x05 && next[4] != 0x0F) {
                    break;
                }
                link = first + le32(next + 8);
            }
        }
    }
    return n;
}

/* TOS runs the first root sector whose words add up to $1234: a hard disk
 * driver's loader. That driver then mounts the partitions of every device,
 * from C: on. -1 when no image boots.
 */
int acsi_boot_partitions(void)
{
    uint8_t root[SECTOR];
    bool boots = false;
    int n = 0;
    for (int i = 0; i < ST_ACSI_DEVICES; i++) {
        disk_t *d = &disks[i];
        if (d->file == NULL || !read_sector(d, 0, root)) {
            continue;
        }
        uint16_t sum = 0;
        for (int k = 0; k < SECTOR; k += 2) {
            sum += (root[k] << 8) | root[k + 1];
        }
        boots |= sum == 0x1234;
        int atari = atari_partitions(d, root);
        n += atari > 0 ? atari : dos_partitions(d, root);
    }
    return boots ? n : -1;
}

/* ---- the bus ------------------------------------------------------------ */

void acsi_reset(void)
{
    cmd.device = -1;
    cmd.busy = 0;
    cmd.count = 0;
    fdc_set_hdc_irq(false);
}

/* Bytes in a SCSI command, by its group */
static int scsi_length(uint8_t opcode)
{
    switch (opcode >> 5) {
    case 1: case 2: return 10;
    case 5:         return 12;
    default:        return 6;
    }
}

/* The SCSI command itself: after ICD's first byte, or the whole ACSI one
 * with the device number cleared from its opcode
 */
static const uint8_t *scsi_command(uint8_t *op)
{
    if ((cmd.bytes[0] & 0x1F) == ICD_EXTENDED) {
        *op = cmd.bytes[1];
        return cmd.bytes + 1;
    }
    *op = cmd.bytes[0] & 0x1F;
    return cmd.bytes;
}

/* Bytes the command moves, for its time */
static uint32_t transfer_bytes(void)
{
    uint8_t op;
    const uint8_t *c = scsi_command(&op);
    switch (op) {
    case 0x08: case 0x0A:                       /* READ(6), WRITE(6) */
        return (c[4] ? c[4] : 256) * SECTOR;
    case 0x28: case 0x2A:                       /* READ(10), WRITE(10) */
        return ((c[7] << 8) | c[8]) * SECTOR;
    default:
        return 64;
    }
}

void acsi_command_byte(uint8_t byte, bool first)
{
    fdc_set_hdc_irq(false);
    if (first) {
        int device = byte >> 5;
        cmd.device = -1;
        cmd.busy = 0;
        if (device >= ST_ACSI_DEVICES || disks[device].file == NULL) {
            return;                     /* nobody there: no answer */
        }
        cmd.device = device;
        cmd.count = 0;
        cmd.length = (byte & 0x1F) == ICD_EXTENDED ? 0 : 6;
    } else if (cmd.device < 0 || cmd.busy || cmd.count >= (int)sizeof(cmd.bytes) ||
               (cmd.length && cmd.count >= cmd.length)) {
        return;
    }
    cmd.bytes[cmd.count++] = byte;
    if (cmd.length == 0 && cmd.count == 2) {
        cmd.length = 1 + scsi_length(byte);
    }
    if (cmd.length == 0 || cmd.count < cmd.length) {
        fdc_set_hdc_irq(true);          /* ready for the next byte */
        return;
    }
    cmd.busy = COMMAND_TIME + transfer_bytes() * BYTE_CYCLES;
}

uint8_t acsi_status(void)
{
    fdc_set_hdc_irq(false);
    return cmd.status;
}

/* ---- commands ----------------------------------------------------------- */

static uint8_t fail(disk_t *d, acsi_error_t e)
{
    d->sense = e;
    return CHECK_CONDITION;
}

/* Data for the 68000: no more than the command allows */
static void reply(const uint8_t *data, int len, int allowed)
{
    fdc_dma_to_ram(data, len < allowed ? len : allowed);
}

static uint8_t request_sense(disk_t *d, const uint8_t *c)
{
    uint8_t s[18] = { 0 };
    int len = c[4] ? c[4] : 4;
    if (len <= 4) {
        s[0] = errors[d->sense].code;   /* the old format: just the error */
    } else {
        s[0] = 0x70;
        s[2] = errors[d->sense].key;
        s[7] = 10;
        s[12] = errors[d->sense].code;
    }
    reply(s, len < (int)sizeof(s) ? len : (int)sizeof(s), len);
    d->sense = E_NONE;
    return GOOD;
}

static uint8_t inquiry(const uint8_t *c, int lun)
{
    uint8_t s[36] = { 0 };
    s[0] = lun ? 0x7F : 0x00;           /* direct access, or no such unit */
    s[2] = 0x01;
    s[3] = 0x01;
    s[4] = sizeof(s) - 5;
    memcpy(s + 8, "EMU     ", 8);
    memcpy(s + 16, "ACSI disk image ", 16);
    memcpy(s + 32, "1.0 ", 4);
    reply(s, sizeof(s), c[4]);
    return GOOD;
}

static uint8_t mode_sense(disk_t *d, const uint8_t *c)
{
    uint8_t s[36] = { 0 };
    int page = c[2] & 0x3F, n = 12;
    uint32_t blocks = d->sectors > 0xFFFFFF ? 0xFFFFFF : d->sectors;
    s[2] = d->write_protected ? 0x80 : 0x00;
    s[3] = 8;                           /* a block descriptor: */
    s[5] = blocks >> 16;
    s[6] = blocks >> 8;
    s[7] = blocks;
    s[10] = SECTOR >> 8;
    if (page == 0x04 || page == 0x3F) {
        /* Rigid disk geometry: 16 heads of 63 sectors */
        uint32_t cylinders = d->sectors / (16 * 63);
        s[n] = 0x04;
        s[n + 1] = 0x16;
        s[n + 2] = cylinders >> 16;
        s[n + 3] = cylinders >> 8;
        s[n + 4] = cylinders;
        s[n + 5] = 16;
        n += 24;
    }
    s[0] = n - 1;
    reply(s, n, c[4]);
    return GOOD;
}

static uint8_t read_capacity(disk_t *d)
{
    uint32_t last = d->sectors - 1;
    uint8_t s[8] = { last >> 24, last >> 16, last >> 8, last, 0, 0, SECTOR >> 8, 0 };
    reply(s, sizeof(s), sizeof(s));
    return GOOD;
}

static uint8_t read_sectors(disk_t *d, uint32_t lba, uint32_t count)
{
    if (lba + count > d->sectors || lba + count < lba) {
        return fail(d, E_ADDRESS);
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!read_sector(d, lba + i, buffer)) {
            return fail(d, E_READ);
        }
        d->reads++;
        if (!fdc_dma_to_ram(buffer, SECTOR)) {
            break;                      /* the DMA's count ran out */
        }
    }
    return GOOD;
}

static uint8_t write_sectors(disk_t *d, uint32_t lba, uint32_t count)
{
    if (lba + count > d->sectors || lba + count < lba) {
        return fail(d, E_ADDRESS);
    }
    if (d->write_protected) {
        return fail(d, E_PROTECTED);
    }
    uint8_t status = GOOD;
    for (uint32_t i = 0; i < count; i++) {
        if (!fdc_dma_from_ram(buffer, SECTOR)) {
            break;
        }
        if (!write_sector(d, lba + i, buffer)) {
            status = fail(d, E_WRITE);
            break;
        }
        d->writes++;
    }
    fflush(d->file);
    fsync(fileno(d->file));
    return status;
}

static uint8_t execute(disk_t *d)
{
    uint8_t op;
    const uint8_t *c = scsi_command(&op);
    int lun = c[1] >> 5;
    if (op == 0x03) {
        return request_sense(d, c);
    }
    if (op == 0x12) {
        return inquiry(c, lun);
    }
    if (lun != 0) {
        return fail(d, E_LUN);
    }
    switch (op) {
    case 0x00:                          /* TEST UNIT READY */
    case 0x01:                          /* REZERO UNIT */
    case 0x04:                          /* FORMAT UNIT: the image stays as it is */
    case 0x1B:                          /* START STOP UNIT */
    case 0x1E:                          /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x2F:                          /* VERIFY(10) */
    case 0x35:                          /* SYNCHRONIZE CACHE */
        return GOOD;
    case 0x08:                          /* READ(6) */
        return read_sectors(d, ((c[1] & 0x1F) << 16) | (c[2] << 8) | c[3], c[4] ? c[4] : 256);
    case 0x0A:                          /* WRITE(6) */
        return write_sectors(d, ((c[1] & 0x1F) << 16) | (c[2] << 8) | c[3], c[4] ? c[4] : 256);
    case 0x0B:                          /* SEEK(6) */
        return (uint32_t)(((c[1] & 0x1F) << 16) | (c[2] << 8) | c[3]) < d->sectors ? GOOD : fail(d, E_ADDRESS);
    case 0x15: {                        /* MODE SELECT(6): taken, not kept */
        uint8_t ignored[256];
        if (c[4]) {
            fdc_dma_from_ram(ignored, c[4]);
        }
        return GOOD;
    }
    case 0x1A:                          /* MODE SENSE(6) */
        return mode_sense(d, c);
    case 0x25:                          /* READ CAPACITY(10) */
        return read_capacity(d);
    case 0x28:                          /* READ(10) */
        return read_sectors(d, be32(c + 2), (c[7] << 8) | c[8]);
    case 0x2A:                          /* WRITE(10) */
        return write_sectors(d, be32(c + 2), (c[7] << 8) | c[8]);
    default:
        return fail(d, E_OPCODE);
    }
}

void acsi_advance(int cycles)
{
    if (cmd.busy <= 0) {
        return;
    }
    cmd.busy -= cycles;
    if (cmd.busy > 0) {
        return;
    }
    cmd.busy = 0;
    disk_t *d = &disks[cmd.device];
    if (d->file == NULL) {
        cmd.device = -1;                /* taken out meanwhile: no answer */
        return;
    }
    cmd.status = execute(d);
    if (st_debug_acsi) {
        char hex[3 * sizeof(cmd.bytes) + 1] = "";
        for (int i = 0; i < cmd.count; i++) {
            snprintf(hex + 3 * i, 4, " %02x", cmd.bytes[i]);
        }
        ST_LOGI("ACSI %d:%s -> %s", cmd.device, hex, cmd.status == GOOD ? "good" : "check condition");
    }
    d->used = true;
    d->last_frame = st.frames;
    cmd.device = -1;
    fdc_set_hdc_irq(true);
}
