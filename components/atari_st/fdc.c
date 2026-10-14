/*
 * DMA chip + WD1772 floppy controller ($FF8604-$FF860D), drives A: and B:
 * (disk images in floppy.c).
 *
 * Commands take time, as on the real drive but faster: a sector takes 2 ms
 * instead of ~20, steps take their programmed rate. Data moves through the
 * DMA a whole sector at a time when the sector is done.
 *
 * A disk change briefly blocks the write-protect sensor, which is how TOS
 * notices it. ACSI (hard disk) accesses get no answer and TOS times out.
 */
#include <string.h>
#include "st_internal.h"

#define MODE_A0             0x0002
#define MODE_A1             0x0004
#define MODE_HDC            0x0008
#define MODE_SECTOR_COUNT   0x0010
#define MODE_WRITE          0x0100

/* Status register: type I / type II+III meanings */
#define ST_MOTOR_ON         0x80
#define ST_WRITE_PROT       0x40
#define ST_SPUN_UP          0x20    /* type I */
#define ST_RNF              0x10    /* record not found / seek error */
#define ST_CRC              0x08
#define ST_TRACK0           0x04    /* type I */
#define ST_LOST_DATA        0x04    /* type II/III */
#define ST_INDEX            0x02    /* type I */
#define ST_BUSY             0x01

#define PSG_SIDE_0          0x01    /* port A: 1 = side 0 */
#define PSG_DRIVE_A         0x02    /* active low */
#define PSG_DRIVE_B         0x04

#define SECTOR              512
#define TRACK_BYTES         6250    /* raw MFM track at 250 kbit/s */

/* Times in CPU cycles */
#define MS                  (ST_CPU_HZ / 1000)
#define REVOLUTION          (200 * MS)      /* 300 rpm */
#define INDEX_PULSE         (2 * MS)
#define SECTOR_TIME         (2 * MS)
#define TRACK_TIME          (20 * MS)       /* read/write track */
#define COMMAND_TIME        (MS / 4)        /* minimum */
#define NOT_FOUND_TIME      REVOLUTION      /* real: 5 revolutions */
#define MOTOR_OFF_TIME      (9 * REVOLUTION)
#define DISK_CHANGE_TIME    (500 * MS)

static const int step_rates[4] = { 6 * MS, 12 * MS, 2 * MS, 3 * MS };

static struct {
    /* DMA */
    uint16_t mode;
    uint32_t dma_address;
    uint16_t sector_count;

    /* WD1772 registers */
    uint8_t command, status, track, sector, data;
    bool type1_status;          /* status shows the type I bits */
    int step_dir;

    /* Running command */
    int busy;                   /* cycles to the command's next event, 0 = idle */
    bool refused;               /* write to a protected disk: just ends */
    int drive;                  /* -1 = none selected */
    int side;
    int motor;                  /* cycles until the motor stops, 0 = off */
    uint32_t rotation;          /* cycles into the revolution */
    int id_sector;              /* next sector ID under the head */

    int head[2];                /* head position of each drive */
    int changing[2];            /* cycles left of a disk change */
} fdc;

/* Sectors moved, for status displays (survive resets) */
static uint32_t sectors_read[2], sectors_written[2];

static uint8_t buffer[TRACK_BYTES];

static void set_intrq(bool active)
{
    mfp_set_gpip(MFP_GPIP_FDC, !active);
}

void fdc_reset(void)
{
    int changing[2] = { fdc.changing[0], fdc.changing[1] };
    memset(&fdc, 0, sizeof(fdc));
    fdc.changing[0] = changing[0];
    fdc.changing[1] = changing[1];
    fdc.sector = 1;
    fdc.step_dir = 1;
    fdc.drive = -1;
    fdc.type1_status = true;
    set_intrq(false);
}

void fdc_disk_changed(int drive)
{
    fdc.changing[drive] = DISK_CHANGE_TIME;
}

static int selected_drive(void)
{
    uint8_t a = psg_port_a();
    return !(a & PSG_DRIVE_A) ? 0 : !(a & PSG_DRIVE_B) ? 1 : -1;
}

static bool write_protected(int drive)
{
    return fdc.changing[drive] > 0 || (floppy_present(drive) && floppy_write_protected(drive));
}

/* ---- DMA ---------------------------------------------------------------- */

/* The DMA moves data while its sector count is non-zero */
static bool dma_to_ram(const uint8_t *src, int len)
{
    if (fdc.sector_count == 0) {
        return false;
    }
    if (st_ram_range(fdc.dma_address, len)) {
        st_ram_write(fdc.dma_address, src, len);
    }
    fdc.dma_address = (fdc.dma_address + len) & 0xFFFFFF;
    fdc.sector_count -= (len + SECTOR - 1) / SECTOR;
    return true;
}

static bool dma_from_ram(uint8_t *dst, int len)
{
    if (fdc.sector_count == 0) {
        return false;
    }
    if (st_ram_range(fdc.dma_address, len)) {
        st_ram_read(fdc.dma_address, dst, len);
    } else {
        memset(dst, 0, len);
    }
    fdc.dma_address = (fdc.dma_address + len) & 0xFFFFFF;
    fdc.sector_count -= (len + SECTOR - 1) / SECTOR;
    return true;
}

/* ---- commands ----------------------------------------------------------- */

static void finish(void)
{
    fdc.busy = 0;
    fdc.status &= ~ST_BUSY;
    fdc.motor = MOTOR_OFF_TIME;
    set_intrq(true);
}

/* The sector the command wants is under the head of a disk */
static bool sector_exists(int sector)
{
    return fdc.drive >= 0 && floppy_present(fdc.drive) && fdc.track == fdc.head[fdc.drive] &&
           sector >= 1 && sector <= floppy_sectors_per_track(fdc.drive);
}

static uint16_t crc16(uint16_t crc, const uint8_t *p, int len)
{
    while (len--) {
        crc ^= *p++ << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

static uint16_t id_crc(const uint8_t *id4)
{
    static const uint8_t mark[] = { 0xA1, 0xA1, 0xA1, 0xFE };
    return crc16(crc16(0xFFFF, mark, 4), id4, 4);
}

/* Restore, seek, step, step in, step out: start */
static void type1_start(uint8_t cmd)
{
    int steps = 0;
    int target = fdc.drive >= 0 ? fdc.head[fdc.drive] : 0;
    switch (cmd >> 4) {
    case 0x0:                   /* restore */
        steps = target;
        target = 0;
        fdc.track = 0;
        break;
    case 0x1:                   /* seek to the data register's track */
        steps = fdc.data - fdc.track;
        target += steps;
        fdc.track = fdc.data;
        break;
    default:
        if ((cmd >> 5) == 2) {
            fdc.step_dir = 1;   /* step in */
        } else if ((cmd >> 5) == 3) {
            fdc.step_dir = -1;  /* step out */
        }
        steps = fdc.step_dir;
        target += steps;
        if (cmd & 0x10) {       /* update the track register */
            fdc.track += steps;
        }
        break;
    }
    /* The head stops at the end stops, the track register doesn't */
    target = target < 0 ? 0 : target > 85 ? 85 : target;
    if (fdc.drive >= 0) {
        fdc.head[fdc.drive] = target;
    }
    if (steps < 0) {
        steps = -steps;
    }
    fdc.busy = COMMAND_TIME + steps * step_rates[cmd & 3];
}

static void type1_end(void)
{
    uint8_t cmd = fdc.command;
    fdc.status &= ~ST_RNF;
    if (fdc.drive < 0 && (cmd >> 4) == 0x0) {
        fdc.status |= ST_RNF;       /* no drive: track 0 never comes */
    } else if (cmd & 0x04) {
        /* Verify: an ID with the track register's track must come by */
        if (fdc.drive < 0 || !floppy_present(fdc.drive) || fdc.track != fdc.head[fdc.drive]) {
            fdc.status |= ST_RNF;
        }
    }
    fdc.status |= ST_SPUN_UP;
    finish();
}

static void read_sector_event(void)
{
    if (!sector_exists(fdc.sector) ||
        !floppy_read_sector(fdc.drive, fdc.head[fdc.drive], fdc.side, fdc.sector, buffer)) {
        fdc.status |= ST_RNF;
        finish();
        return;
    }
    if (!dma_to_ram(buffer, SECTOR)) {
        fdc.status |= ST_LOST_DATA;
    }
    sectors_read[fdc.drive]++;
    if (fdc.command & 0x10) {       /* multiple sectors: on to the next */
        fdc.sector++;
        fdc.busy = sector_exists(fdc.sector) ? SECTOR_TIME : NOT_FOUND_TIME;
        return;
    }
    finish();
}

static void write_sector_event(void)
{
    if (!sector_exists(fdc.sector)) {
        fdc.status |= ST_RNF;
        finish();
        return;
    }
    if (!dma_from_ram(buffer, SECTOR)) {
        fdc.status |= ST_LOST_DATA;
        memset(buffer, 0, SECTOR);
    }
    if (!floppy_write_sector(fdc.drive, fdc.head[fdc.drive], fdc.side, fdc.sector, buffer)) {
        fdc.status |= ST_RNF;
        finish();
        return;
    }
    sectors_written[fdc.drive]++;
    if (fdc.command & 0x10) {
        fdc.sector++;
        fdc.busy = sector_exists(fdc.sector) ? SECTOR_TIME : NOT_FOUND_TIME;
        return;
    }
    finish();
}

static void read_address_event(void)
{
    int spt = fdc.drive >= 0 ? floppy_sectors_per_track(fdc.drive) : 0;
    if (spt == 0) {
        fdc.status |= ST_RNF;
        finish();
        return;
    }
    fdc.id_sector = fdc.id_sector % spt + 1;
    uint8_t id[6] = { fdc.head[fdc.drive], fdc.side, fdc.id_sector, 2 };
    uint16_t crc = id_crc(id);
    id[4] = crc >> 8;
    id[5] = crc;
    dma_to_ram(id, sizeof(id));
    fdc.sector = id[0];             /* the WD1772 puts the track there */
    finish();
}

/* A standard track as the WD1772 reads it back (gaps, IDs, data, CRCs) */
static int build_track(void)
{
    int n = 0, spt = floppy_sectors_per_track(fdc.drive);
    int track = fdc.head[fdc.drive];
    memset(buffer, 0x4E, sizeof(buffer));
    n += 60;
    for (int s = 1; s <= spt && n + 614 <= TRACK_BYTES; s++) {
        memset(buffer + n, 0x00, 12);
        n += 12;
        memset(buffer + n, 0xA1, 3);
        n += 3;
        uint8_t *id = buffer + n;
        id[0] = 0xFE;
        id[1] = track;
        id[2] = fdc.side;
        id[3] = s;
        id[4] = 2;
        uint16_t crc = id_crc(id + 1);
        id[5] = crc >> 8;
        id[6] = crc;
        n += 7 + 22;
        memset(buffer + n, 0x00, 12);
        n += 12;
        memset(buffer + n, 0xA1, 3);
        n += 3;
        buffer[n++] = 0xFB;
        if (!floppy_read_sector(fdc.drive, track, fdc.side, s, buffer + n)) {
            memset(buffer + n, 0xE5, SECTOR);
        }
        crc = crc16(0xFFFF, buffer + n - 4, SECTOR + 4);
        n += SECTOR;
        buffer[n++] = crc >> 8;
        buffer[n++] = crc;
        n += 40;
    }
    return TRACK_BYTES;
}

static void read_track_event(void)
{
    if (fdc.drive < 0 || !floppy_present(fdc.drive)) {
        fdc.status |= ST_RNF;
        finish();
        return;
    }
    int len = build_track();
    for (int i = 0; i < len; i += SECTOR) {
        int chunk = len - i < SECTOR ? len - i : SECTOR;
        if (!dma_to_ram(buffer + i, chunk)) {
            break;
        }
    }
    finish();
}

/* Formatting: find the sectors in the raw track the program wrote
 * ($F5 = sync byte, $FE = ID mark, $FB = data mark, $F7 = CRC) and store
 * their data.
 */
static void write_track_event(void)
{
    if (fdc.drive < 0 || !floppy_present(fdc.drive)) {
        fdc.status |= ST_RNF;
        finish();
        return;
    }
    int len = 0;
    while (len < TRACK_BYTES) {
        int chunk = TRACK_BYTES - len < SECTOR ? TRACK_BYTES - len : SECTOR;
        if (!dma_from_ram(buffer + len, chunk)) {
            break;
        }
        len += chunk;
    }
    int sector = -1;
    for (int i = 1; i < len; i++) {
        if (buffer[i - 1] != 0xF5) {
            continue;
        }
        if (buffer[i] == 0xFE && i + 4 < len) {
            sector = buffer[i + 3];
            i += 4;
        } else if (buffer[i] == 0xFB && sector >= 0 && i + SECTOR < len) {
            if (floppy_write_sector(fdc.drive, fdc.head[fdc.drive], fdc.side, sector, buffer + i + 1)) {
                sectors_written[fdc.drive]++;
            }
            sector = -1;
            i += SECTOR;
        }
    }
    finish();
}

static void command_event(void)
{
    uint8_t cmd = fdc.command;
    if (cmd < 0x80) {
        type1_end();
    } else if (cmd < 0xA0) {
        read_sector_event();
    } else if (cmd < 0xC0) {
        write_sector_event();
    } else if (cmd < 0xD0) {
        read_address_event();
    } else if (cmd < 0xF0) {
        read_track_event();
    } else {
        write_track_event();
    }
}

static void fdc_command(uint8_t cmd)
{
    if ((cmd & 0xF0) == 0xD0) {
        /* Force interrupt: stop the command; $D8 interrupts at once */
        set_intrq(false);
        if (fdc.busy) {
            fdc.busy = 0;
            fdc.refused = false;
            fdc.status &= ~ST_BUSY;
        } else {
            fdc.type1_status = true;
            fdc.status = 0;
        }
        fdc.motor = MOTOR_OFF_TIME;
        if (cmd & 0x0C) {
            set_intrq(true);
        }
        return;
    }
    if (fdc.busy) {
        return;                     /* the WD1772 ignores commands while busy */
    }
    set_intrq(false);
    fdc.command = cmd;
    fdc.drive = selected_drive();
    fdc.side = (psg_port_a() & PSG_SIDE_0) ? 0 : 1;
    fdc.motor = MOTOR_OFF_TIME + 1;  /* on while busy */
    fdc.status = ST_BUSY;

    if (cmd < 0x80) {
        fdc.type1_status = true;
        type1_start(cmd);
        return;
    }
    fdc.type1_status = false;
    switch (cmd >> 4) {
    case 0x8: case 0x9:             /* read sector(s) */
        fdc.busy = sector_exists(fdc.sector) ? SECTOR_TIME : NOT_FOUND_TIME;
        break;
    case 0xA: case 0xB:             /* write sector(s) */
    case 0xF:                       /* write track */
        if (fdc.drive >= 0 && write_protected(fdc.drive)) {
            fdc.status |= ST_WRITE_PROT;
            fdc.busy = COMMAND_TIME;
            fdc.refused = true;
            break;
        }
        fdc.busy = (cmd >> 4) == 0xF ? TRACK_TIME :
                   sector_exists(fdc.sector) ? SECTOR_TIME : NOT_FOUND_TIME;
        break;
    case 0xC:                       /* read address */
        fdc.busy = fdc.drive >= 0 && floppy_present(fdc.drive) ? MS : NOT_FOUND_TIME;
        break;
    default:                        /* read track */
        fdc.busy = TRACK_TIME;
        break;
    }
}

void st_floppy_status(st_floppy_status_t *s)
{
    int d = selected_drive();
    s->light = d >= 0 && fdc.motor > 0 ? d : -1;
    for (int i = 0; i < 2; i++) {
        s->track[i] = fdc.head[i];
        s->reads[i] = sectors_read[i];
        s->writes[i] = sectors_written[i];
    }
}

void fdc_advance(int cycles)
{
    fdc.rotation = (fdc.rotation + cycles) % REVOLUTION;
    for (int d = 0; d < 2; d++) {
        if (fdc.changing[d] > 0) {
            fdc.changing[d] -= cycles;
        }
    }
    if (fdc.busy > 0) {
        fdc.busy -= cycles;
        if (fdc.busy <= 0) {
            fdc.busy = 0;
            if (fdc.refused) {
                fdc.refused = false;
                finish();
            } else {
                command_event();
            }
        }
    } else if (fdc.motor > 0) {
        fdc.motor -= cycles;
        if (fdc.motor < 0) {
            fdc.motor = 0;
        }
    }
}

static uint8_t read_status(void)
{
    set_intrq(false);
    uint8_t s = fdc.status & ~ST_MOTOR_ON;
    if (fdc.motor > 0) {
        s |= ST_MOTOR_ON;
    }
    if (fdc.type1_status) {
        /* Live sensor bits of the selected drive */
        s &= ~(ST_WRITE_PROT | ST_TRACK0 | ST_INDEX);
        int d = selected_drive();
        if (d >= 0) {
            if (write_protected(d)) {
                s |= ST_WRITE_PROT;
            }
            if (fdc.head[d] == 0) {
                s |= ST_TRACK0;
            }
            if (fdc.motor > 0 && floppy_present(d) && fdc.rotation < INDEX_PULSE) {
                s |= ST_INDEX;
            }
        }
    }
    return s;
}

static uint8_t fdc_register_read(void)
{
    switch ((fdc.mode & (MODE_A0 | MODE_A1)) >> 1) {
    case 0:  return read_status();
    case 1:  return fdc.track;
    case 2:  return fdc.sector;
    default: return fdc.data;
    }
}

static void fdc_register_write(uint8_t v)
{
    switch ((fdc.mode & (MODE_A0 | MODE_A1)) >> 1) {
    case 0:  fdc_command(v); break;
    case 1:  if (!fdc.busy) fdc.track = v; break;
    case 2:  if (!fdc.busy) fdc.sector = v; break;
    default: fdc.data = v; break;
    }
}

uint8_t fdc_read(uint32_t a)
{
    switch (a) {
    case 0xFF8605:
        if (fdc.mode & MODE_SECTOR_COUNT) {
            return fdc.sector_count;
        }
        if (fdc.mode & MODE_HDC) {
            return 0xFF;
        }
        return fdc_register_read();
    case 0xFF8607:
        /* DMA status: bit 0 no error, bit 1 sector count non-zero */
        return 0x01 | (fdc.sector_count ? 0x02 : 0);
    case 0xFF8609: return fdc.dma_address >> 16;
    case 0xFF860B: return fdc.dma_address >> 8;
    case 0xFF860D: return fdc.dma_address;
    case 0xFF8604:
    case 0xFF8606:
        return 0x00;
    }
    return 0xFF;
}

void fdc_write(uint32_t a, uint8_t v)
{
    switch (a) {
    case 0xFF8605:
        if (fdc.mode & MODE_SECTOR_COUNT) {
            fdc.sector_count = v;
        } else if (!(fdc.mode & MODE_HDC)) {
            fdc_register_write(v);
        }
        break;
    case 0xFF8606:
        if ((fdc.mode ^ (v << 8)) & MODE_WRITE) {
            fdc.sector_count = 0;   /* toggling the direction resets the DMA */
        }
        fdc.mode = (fdc.mode & 0x00FF) | (v << 8);
        break;
    case 0xFF8607:
        fdc.mode = (fdc.mode & 0xFF00) | v;
        break;
    case 0xFF8609:
        fdc.dma_address = (fdc.dma_address & 0x00FFFF) | ((v & 0x3F) << 16);
        break;
    case 0xFF860B:
        fdc.dma_address = (fdc.dma_address & 0xFF00FF) | (v << 8);
        break;
    case 0xFF860D:
        fdc.dma_address = (fdc.dma_address & 0xFFFF00) | (v & 0xFE);
        break;
    }
}
