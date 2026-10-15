/*
 * GEMDOS drive C: backed by a host folder (the SD card on the board).
 *
 * A small cartridge at $FA0000 runs before TOS boots from disk. It sets
 * TOS's date and time from the host clock (an STE has no RTC and TOS
 * otherwise starts at its build date). With a C: folder it also adds C: to
 * _drvbits, can make it the boot drive, and hooks the GEMDOS trap. Each
 * GEMDOS call then traps into the emulator (an illegal opcode inside the
 * cartridge): calls on C: or on our file handles are answered here from the
 * host file system, everything else continues to TOS.
 *
 * Programs on C: are started with TOS's help: Pexec mode 5 creates the
 * basepage, the emulator loads and relocates the program into it, mode 6
 * runs it as the caller's own call (see native_pexec_load).
 * Long host names show up as upper-case 8.3 names.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "m68k.h"
#include "st_internal.h"

#define HD_DRIVE            2           /* C: */
#define CART_BASE           0xFA0000

/* Cartridge layout (offsets) */
#define CART_INIT           0x28
#define CART_TRAP1          0x50
#define CART_RTE            0x52
#define CART_PEXEC          0x54
#define CART_PEXEC_FAIL     0x6A

/* Native calls: illegal opcodes, honoured only inside the cartridge */
#define OP_GEMDOS           0x000A
#define OP_INSTALL          0x000B
#define OP_PEXEC_LOAD       0x000C
#define OP_GETTIME          0x000D
/* From the patched AES routine in ROM (tos_patch.c) */
#define OP_ADJUST_REQUEST   0x0009
#define OP_REPORT_REZ       0x000E

/* GEMDOS errors */
#define E_OK        0
#define EFILNF      (-33)
#define EPTHNF      (-34)
#define ENHNDL      (-35)
#define EACCDN      (-36)
#define EIHNDL      (-37)
#define ENSMEM      (-39)
#define EIMBA       (-40)
#define ENMFIL      (-49)
#define EPLFMT      (-66)

/* TOS system variables */
#define SV_TRAP1    0x84
#define SV_BOOTDEV  0x446
#define SV_DRVBITS  0x4C2
#define SV_SYSBASE  0x4F2

/* Basepage */
#define BP_HITPA    0x04
#define BP_TBASE    0x08
#define BP_TLEN     0x0C
#define BP_DBASE    0x10
#define BP_DLEN     0x14
#define BP_BBASE    0x18
#define BP_BLEN     0x1C
#define BP_DTA      0x20
#define BP_DEFDRV   0x37

/* DTA */
#define DTA_MAGIC   0x47484453          /* "GHDS" in the reserved area */
#define DTA_SLOT    4
#define DTA_ATTRIB  21
#define DTA_TIME    22
#define DTA_DATE    24
#define DTA_LENGTH  26
#define DTA_NAME    30

#define FA_RDONLY   0x01
#define FA_LABEL    0x08
#define FA_DIR      0x10

#define BASE_HANDLE 64
#define MAX_FILES   32
#define MAX_SEARCHES 8
#define PATH_MAX_ST 128
#define HOST_PATH   384

static const uint8_t cart_code[] = {
    0xAB, 0xCD, 0xEF, 0x42,             /* 00 magic */
    0x00, 0x00, 0x00, 0x00,             /* 04 ca_next */
    0x08, 0xFA, 0x00, CART_INIT,        /* 08 ca_init: run before disk boot */
    0x00, 0xFA, 0x00, CART_INIT + 0x26, /* 0c ca_run: just rts */
    0x00, 0x00, 0x00, 0x00,             /* 10 ca_time, ca_date */
    0x00, 0x00, 0x00, 0x00,             /* 14 ca_size */
    'S', 'T', 'E', 'H', 'O', 'S', 'T', '.', 'T', 'O', 'S', 0, 0, 0, /* 18 ca_name */
    0x00, 0x00,                         /* 26 */
    0x00, 0x0B,                         /* 28 init:  native INSTALL (C: hook) */
    0x00, 0x0D,                         /* 2a        native GETTIME: d0 = date.w:time.w */
    0x2F, 0x00,                         /* 2c        move.l d0,-(sp) */
    0x3F, 0x00,                         /* 2e        move.w d0,-(sp) */
    0x3F, 0x3C, 0x00, 0x2D,             /* 30        move.w #$2d,-(sp)  Tsettime */
    0x4E, 0x41,                         /* 34        trap   #1 */
    0x58, 0x8F,                         /* 36        addq.l #4,sp */
    0x20, 0x1F,                         /* 38        move.l (sp)+,d0 */
    0x48, 0x40,                         /* 3a        swap   d0 */
    0x3F, 0x00,                         /* 3c        move.w d0,-(sp) */
    0x3F, 0x3C, 0x00, 0x2B,             /* 3e        move.w #$2b,-(sp)  Tsetdate */
    0x4E, 0x41,                         /* 42        trap   #1 */
    0x58, 0x8F,                         /* 44        addq.l #4,sp */
    0x4E, 0x75,                         /* 46        rts */
    0x4E, 0x75,                         /* 48        rts (ca_run lands at 4e: keep aligned) */
    0x4E, 0x75,                         /* 4a */
    0x4E, 0x75,                         /* 4c */
    0x4E, 0x75,                         /* 4e ca_run */
    0x00, 0x0A,                         /* 50 trap1: native GEMDOS (sets PC) */
    0x4E, 0x73,                         /* 52 rte:   rte */
    0x2F, 0x09,                         /* 54 pexec: move.l a1,-(sp)    env */
    0x2F, 0x08,                         /* 56        move.l a0,-(sp)    cmdline */
    0x42, 0xA7,                         /* 58        clr.l  -(sp)       fname */
    0x3F, 0x3C, 0x00, 0x05,             /* 5a        move.w #5,-(sp)    create basepage */
    0x3F, 0x3C, 0x00, 0x4B,             /* 5e        move.w #$4b,-(sp)  Pexec */
    0x4E, 0x41,                         /* 62        trap   #1 */
    0x4F, 0xEF, 0x00, 0x10,             /* 64        lea    16(sp),sp */
    0x00, 0x0C,                         /* 68        native PEXEC_LOAD: mode 0 goes on
                                         *            as the caller's own Pexec 6 */
    0x2F, 0x00,                         /* 6a fail:  move.l d0,-(sp)    error */
    0x2F, 0x08,                         /* 6c        move.l a0,-(sp)    basepage */
    0x2F, 0x28, 0x00, 0x2C,             /* 6e        move.l 44(a0),-(sp) environment */
    0x3F, 0x3C, 0x00, 0x49,             /* 72        move.w #$49,-(sp)  Mfree */
    0x4E, 0x41,                         /* 76        trap   #1 */
    0x5C, 0x8F,                         /* 78        addq.l #6,sp */
    0x3F, 0x3C, 0x00, 0x49,             /* 7a        move.w #$49,-(sp)  Mfree basepage */
    0x4E, 0x41,                         /* 7e        trap   #1 */
    0x5C, 0x8F,                         /* 80        addq.l #6,sp */
    0x20, 0x1F,                         /* 82        move.l (sp)+,d0 */
    0x4E, 0x73,                         /* 84        rte */
};

typedef struct {
    FILE *f;
    uint32_t owner;                     /* basepage of the opening process */
} hd_file_t;

typedef struct {
    char name[14];                      /* 8.3, upper case */
    uint8_t attr;
    uint16_t time, date;
    uint32_t size;
} hd_entry_t;

typedef struct {
    hd_entry_t *entries;
    int count, next;
    uint32_t dta;
} hd_search_t;

static struct {
    bool enabled;
    bool boot;
    char root[HOST_PATH];
    char cwd[PATH_MAX_ST];              /* current folder on C:, "" = root, "A\B" */
    uint8_t cart[sizeof(cart_code)];
    uint32_t old_trap1;
    hd_file_t files[MAX_FILES];
    hd_search_t searches[MAX_SEARCHES];
    int next_search;
    int pexec_mode;
    uint32_t pexec_args;                /* the caller's Pexec arguments */
    char pexec_path[HOST_PATH];
} hd;

/* Last file the ST opened, made or started on C:, for status displays.
 * Written by the emulation, read by other tasks: seq is odd while it changes.
 */
static struct {
    volatile uint32_t seq;
    const char *verb;
    char path[96];
} last;

static void note_file(const char *verb, const char *host)
{
    size_t root = strlen(hd.root);
    const char *rel = strncmp(host, hd.root, root) == 0 ? host + root : host;
    last.seq++;
    __sync_synchronize();
    last.verb = verb;
    int n = snprintf(last.path, sizeof(last.path), "C:%s%s", *rel == '/' ? "" : "\\", rel);
    for (int i = 2; i < n && i < (int)sizeof(last.path); i++) {
        if (last.path[i] == '/') {
            last.path[i] = '\\';
        }
    }
    __sync_synchronize();
    last.seq++;
}

const char *st_hd_last_file(char *path, size_t len)
{
    for (;;) {
        uint32_t seq = last.seq;
        __sync_synchronize();
        const char *verb = last.verb;
        snprintf(path, len, "%s", last.path);
        __sync_synchronize();
        if (!(seq & 1) && seq == last.seq) {
            return verb;
        }
    }
}

/* ---- ST side helpers ------------------------------------------------------ */

static uint32_t current_basepage(void)
{
    uint32_t os = st_peek32(SV_SYSBASE);
    uint32_t run = st_peek32(os + 0x28);
    return st_peek32(run);
}

static int current_drive(void)
{
    return st_peek8(current_basepage() + BP_DEFDRV);
}

static void read_st_string(uint32_t a, char *buf, size_t size)
{
    size_t i = 0;
    for (; i + 1 < size; i++) {
        buf[i] = st_peek8(a + i);
        if (buf[i] == 0) {
            return;
        }
    }
    buf[i] = 0;
}

/* ---- names ---------------------------------------------------------------- */

/* Host name -> ST 8.3 name in upper case */
static void name_to_83(const char *host, char *out)
{
    const char *dot = strrchr(host, '.');
    if (dot == host) {
        dot = NULL;
    }
    int n = 0;
    for (const char *p = host; *p && p != dot && n < 8; p++) {
        unsigned char c = *p;
        if (c == ' ') {
            continue;
        }
        out[n++] = strchr(".+,;=[]*?\\/:\"<>|", c) || c > 127 ? '_' : toupper(c);
    }
    if (dot && dot[1]) {
        out[n++] = '.';
        int e = 0;
        for (const char *p = dot + 1; *p && e < 3; p++, e++) {
            unsigned char c = *p;
            out[n++] = strchr(".+,;=[]*?\\/:\"<>| ", c) || c > 127 ? '_' : toupper(c);
        }
    }
    out[n] = 0;
}

static bool skip_host_entry(const char *name)
{
    return name[0] == '.' || strcmp(name, "System Volume Information") == 0;
}

/* Expand an 8.3 name or pattern to 11 FCB characters, '*' -> '?' */
static void to_fcb(const char *name, char *fcb)
{
    memset(fcb, ' ', 11);
    const char *dot = strchr(name, '.');
    for (int i = 0, n = 0; name[i] && &name[i] != dot && n < 8; i++) {
        if (name[i] == '*') {
            while (n < 8) fcb[n++] = '?';
            break;
        }
        fcb[n++] = toupper((unsigned char)name[i]);
    }
    if (dot) {
        for (int i = 1, n = 8; dot[i] && n < 11; i++) {
            if (dot[i] == '*') {
                while (n < 11) fcb[n++] = '?';
                break;
            }
            fcb[n++] = toupper((unsigned char)dot[i]);
        }
    }
}

static bool wildcard_match(const char *pattern, const char *name)
{
    char p[11], n[11];
    to_fcb(strcmp(pattern, "*") == 0 ? "*.*" : pattern, p);
    to_fcb(name, n);
    for (int i = 0; i < 11; i++) {
        if (p[i] != '?' && p[i] != n[i]) {
            return false;
        }
    }
    return true;
}

/* Find the host entry in `dir` that shows up as ST name `st_name` */
static bool find_host_name(const char *dir, const char *st_name, char *out, size_t size)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return false;
    }
    bool found = false;
    char n83[14];
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        if (skip_host_entry(e->d_name)) {
            continue;
        }
        name_to_83(e->d_name, n83);
        if (strcasecmp(n83, st_name) == 0 || strcasecmp(e->d_name, st_name) == 0) {
            snprintf(out, size, "%s", e->d_name);
            found = true;
        }
    }
    closedir(d);
    return found;
}

/*
 * ST path -> host path. Returns 1 when the path is on C: and resolved,
 * 0 when it isn't ours, or a GEMDOS error. With `create` the last component
 * may be missing (it is used as given). `st_abs` gets the absolute ST path
 * on C: without drive or leading backslash.
 */
static int resolve(const char *st_path, char *host, bool create, char *st_abs)
{
    const char *p = st_path;
    int drive;
    if (p[0] && p[1] == ':') {
        drive = toupper((unsigned char)p[0]) - 'A';
        p += 2;
    } else {
        if (strchr(p, ':')) {
            return 0;           /* device name such as CON: */
        }
        drive = current_drive();
    }
    if (drive != HD_DRIVE) {
        return 0;
    }

    /* Absolute component list */
    char abs[PATH_MAX_ST * 2] = "";
    if (*p == '\\' || *p == '/') {
        p++;
    } else {
        snprintf(abs, sizeof(abs), "%s", hd.cwd);
    }
    char comp[PATH_MAX_ST];
    while (*p) {
        size_t n = strcspn(p, "\\/");
        snprintf(comp, sizeof(comp), "%.*s", (int)n, p);
        p += n;
        if (*p) {
            p++;
        }
        if (comp[0] == 0 || strcmp(comp, ".") == 0) {
            continue;
        }
        if (strcmp(comp, "..") == 0) {
            char *last = strrchr(abs, '\\');
            if (last) {
                *last = 0;
            } else {
                abs[0] = 0;
            }
            continue;
        }
        size_t len = strlen(abs);
        snprintf(abs + len, sizeof(abs) - len, "%s%s", len ? "\\" : "", comp);
    }
    if (st_abs) {
        snprintf(st_abs, PATH_MAX_ST, "%s", abs);
    }

    /* Map each component to the host name */
    snprintf(host, HOST_PATH, "%s", hd.root);
    char *c = abs;
    while (*c) {
        size_t n = strcspn(c, "\\");
        bool last = c[n] == 0;
        snprintf(comp, sizeof(comp), "%.*s", (int)n, c);
        c += n + (last ? 0 : 1);

        char name[256];
        size_t hl = strlen(host);
        if (find_host_name(host, comp, name, sizeof(name))) {
            snprintf(host + hl, HOST_PATH - hl, "/%s", name);
        } else if (last && create) {
            snprintf(host + hl, HOST_PATH - hl, "/%s", comp);
        } else {
            return last ? EFILNF : EPTHNF;
        }
    }
    return 1;
}

static void dos_datetime(time_t t, uint16_t *time_out, uint16_t *date_out)
{
    struct tm tm;
    localtime_r(&t, &tm);
    int year = tm.tm_year + 1900;
    if (year < 1980) {
        year = 1980;
    }
    *time_out = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
    *date_out = ((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
}

static uint8_t host_attr(const struct stat *s)
{
    uint8_t a = 0;
    if (S_ISDIR(s->st_mode)) {
        a |= FA_DIR;
    }
    if (!(s->st_mode & S_IWUSR)) {
        a |= FA_RDONLY;
    }
    return a;
}

/* ---- files ---------------------------------------------------------------- */

static hd_file_t *file_from_handle(int handle)
{
    int i = handle - BASE_HANDLE;
    if (i < 0 || i >= MAX_FILES || hd.files[i].f == NULL) {
        return NULL;
    }
    return &hd.files[i];
}

static bool is_our_handle(int handle)
{
    return handle >= BASE_HANDLE && handle < BASE_HANDLE + MAX_FILES;
}

static int32_t open_file(const char *host, const char *mode)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (hd.files[i].f == NULL) {
            FILE *f = fopen(host, mode);
            if (f == NULL) {
                return errno == EACCES ? EACCDN : EFILNF;
            }
            hd.files[i].f = f;
            hd.files[i].owner = current_basepage();
            return BASE_HANDLE + i;
        }
    }
    return ENHNDL;
}

static void close_files_of(uint32_t basepage)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (hd.files[i].f && hd.files[i].owner == basepage) {
            fclose(hd.files[i].f);
            hd.files[i].f = NULL;
        }
    }
}

static int32_t file_transfer(int handle, uint32_t count, uint32_t buf, bool write)
{
    hd_file_t *hf = file_from_handle(handle);
    if (hf == NULL) {
        return EIHNDL;
    }
    if (!st_ram_range(buf, count)) {
        return EIMBA;
    }
    uint8_t chunk[1024];
    uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done < sizeof(chunk) ? count - done : sizeof(chunk);
        size_t got;
        if (write) {
            st_ram_read(buf + done, chunk, n);
            got = fwrite(chunk, 1, n, hf->f);
        } else {
            got = fread(chunk, 1, n, hf->f);
            st_ram_write(buf + done, chunk, got);
        }
        done += got;
        if (got < n) {
            break;
        }
    }
    if (write) {
        fflush(hf->f);
    }
    return done;
}

/* ---- directory search ---------------------------------------------------- */

static void fill_dta(uint32_t dta, const hd_entry_t *e)
{
    st_poke8(dta + DTA_ATTRIB, e->attr);
    st_poke16(dta + DTA_TIME, e->time);
    st_poke16(dta + DTA_DATE, e->date);
    st_poke32(dta + DTA_LENGTH, e->size);
    for (int i = 0; i < 14; i++) {
        st_poke8(dta + DTA_NAME + i, e->name[i]);
    }
}

static int32_t search_next(uint32_t dta)
{
    if (st_peek32(dta) != DTA_MAGIC) {
        return ENMFIL;
    }
    int slot = st_peek8(dta + DTA_SLOT);
    hd_search_t *s = &hd.searches[slot % MAX_SEARCHES];
    if (s->entries == NULL || s->dta != dta || s->next >= s->count) {
        return ENMFIL;
    }
    fill_dta(dta, &s->entries[s->next++]);
    return E_OK;
}

static int32_t search_first(const char *spec, uint16_t attr, uint32_t dta, const char *host_dir)
{
    if (attr == FA_LABEL) {
        return EFILNF;          /* no volume label */
    }
    const char *pattern = strrchr(spec, '\\');
    pattern = pattern ? pattern + 1 : (spec[1] == ':' ? spec + 2 : spec);
    if (*pattern == 0) {
        pattern = "*.*";
    }

    int slot = hd.next_search++ % MAX_SEARCHES;
    hd_search_t *s = &hd.searches[slot];
    free(s->entries);
    memset(s, 0, sizeof(*s));
    s->dta = dta;

    DIR *d = opendir(host_dir);
    if (d == NULL) {
        return EPTHNF;
    }
    int capacity = 0;
    bool root = strcmp(host_dir, hd.root) == 0;
    if (!root && (attr & FA_DIR)) {
        /* "." and ".." like TOS */
        capacity = 16;
        s->entries = calloc(capacity, sizeof(hd_entry_t));
        for (int i = 0; i < 2; i++) {
            hd_entry_t *e = &s->entries[s->count];
            strcpy(e->name, i ? ".." : ".");
            if (wildcard_match(pattern, e->name)) {
                e->attr = FA_DIR;
                s->count++;
            }
        }
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (skip_host_entry(de->d_name)) {
            continue;
        }
        char n83[14];
        name_to_83(de->d_name, n83);
        if (!wildcard_match(pattern, n83)) {
            continue;
        }
        char full[HOST_PATH + 256];
        snprintf(full, sizeof(full), "%s/%s", host_dir, de->d_name);
        struct stat stbuf;
        if (stat(full, &stbuf) != 0) {
            continue;
        }
        uint8_t a = host_attr(&stbuf);
        if ((a & FA_DIR) && !(attr & FA_DIR)) {
            continue;
        }
        if (s->count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            s->entries = realloc(s->entries, capacity * sizeof(hd_entry_t));
        }
        hd_entry_t *e = &s->entries[s->count++];
        snprintf(e->name, sizeof(e->name), "%s", n83);
        e->attr = a;
        e->size = (a & FA_DIR) ? 0 : (uint32_t)stbuf.st_size;
        dos_datetime(stbuf.st_mtime, &e->time, &e->date);
    }
    closedir(d);

    st_poke32(dta, DTA_MAGIC);
    st_poke8(dta + DTA_SLOT, slot);
    if (s->count == 0) {
        return EFILNF;
    }
    return search_next(dta);
}

/* ---- Pexec ------------------------------------------------------------------- */

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* Load the program into the basepage Pexec 5 created; 0 or a GEMDOS error */
static int32_t load_program(uint32_t bp)
{
    FILE *f = fopen(hd.pexec_path, "rb");
    if (f == NULL) {
        return EFILNF;
    }
    uint8_t h[28];
    if (fread(h, 1, sizeof(h), f) != sizeof(h) || h[0] != 0x60 || h[1] != 0x1A) {
        fclose(f);
        return EPLFMT;
    }
    uint32_t tlen = rd_be32(h + 2), dlen = rd_be32(h + 6);
    uint32_t blen = rd_be32(h + 10), slen = rd_be32(h + 14);
    bool absolute = h[26] || h[27];
    uint32_t text = bp + 256;
    uint32_t hitpa = st_peek32(bp + BP_HITPA);
    if (text + tlen + dlen + blen > hitpa || !st_ram_range(text, tlen + dlen + blen)) {
        fclose(f);
        return ENSMEM;
    }

    uint8_t chunk[1024];
    for (uint32_t done = 0; done < tlen + dlen;) {
        uint32_t n = tlen + dlen - done < sizeof(chunk) ? tlen + dlen - done : sizeof(chunk);
        if (fread(chunk, 1, n, f) != n) {
            fclose(f);
            return EPLFMT;
        }
        st_ram_write(text + done, chunk, n);
        done += n;
    }

    /* Relocation table: first offset, then byte steps (1 = skip 254) */
    if (!absolute && fseek(f, 28 + tlen + dlen + slen, SEEK_SET) == 0 &&
        fread(chunk, 1, 4, f) == 4) {
        uint32_t offset = rd_be32(chunk);
        if (offset) {
            uint32_t a = text + offset;
            int c = 0;
            do {
                if (a & 1 || a + 4 > text + tlen + dlen) {
                    fclose(f);
                    return EPLFMT;
                }
                st_poke32(a, st_peek32(a) + text);
                while ((c = fgetc(f)) == 1) {
                    a += 254;
                }
                a += c > 0 ? c : 0;
            } while (c > 0);
        }
    }
    fclose(f);

    for (uint32_t i = 0; i < blen; i++) {
        st_poke8(text + tlen + dlen + i, 0);
    }
    st_poke32(bp + BP_TBASE, text);
    st_poke32(bp + BP_TLEN, tlen);
    st_poke32(bp + BP_DBASE, text + tlen);
    st_poke32(bp + BP_DLEN, dlen);
    st_poke32(bp + BP_BBASE, text + tlen + dlen);
    st_poke32(bp + BP_BLEN, blen);
    return E_OK;
}

/* ---- GEMDOS dispatch ---------------------------------------------------- */

typedef enum { PASS, DONE, PEXEC } dispatch_t;

static uint16_t arg16(uint32_t args, int off)
{
    return st_peek16(args + off);
}

static uint32_t arg32(uint32_t args, int off)
{
    return st_peek32(args + off);
}

/* Handle the call when it's ours; result in *ret */
static dispatch_t gemdos_call(uint32_t args, int32_t *ret)
{
    uint16_t fn = arg16(args, 0);
    char path[PATH_MAX_ST], path2[PATH_MAX_ST];
    char host[HOST_PATH], host2[HOST_PATH];
    char st_abs[PATH_MAX_ST];
    int r;

    switch (fn) {
    case 0x00:              /* Pterm0 */
    case 0x31:              /* Ptermres */
    case 0x4C:              /* Pterm */
        close_files_of(current_basepage());
        return PASS;

    case 0x36: {            /* Dfree(buf, drive) */
        int drive = arg16(args, 6);
        drive = drive ? drive - 1 : current_drive();
        if (drive != HD_DRIVE) {
            return PASS;
        }
        uint64_t total = 0, free_bytes = 0;
        if (!st_host_disk_free(hd.root, &total, &free_bytes)) {
            total = free_bytes = 0;
        }
        uint32_t buf = arg32(args, 2);
        const uint64_t cluster = 1024, max = 0x7FFFFFFF / cluster;
        st_poke32(buf, free_bytes / cluster > max ? max : free_bytes / cluster);
        st_poke32(buf + 4, total / cluster > max ? max : total / cluster);
        st_poke32(buf + 8, 512);
        st_poke32(buf + 12, 2);
        *ret = E_OK;
        return DONE;
    }

    case 0x39:              /* Dcreate(path) */
    case 0x3A:              /* Ddelete(path) */
        read_st_string(arg32(args, 2), path, sizeof(path));
        r = resolve(path, host, fn == 0x39, NULL);
        if (r <= 0) {
            if (r == 0) return PASS;
            *ret = r;
            return DONE;
        }
        if (fn == 0x39) {
            *ret = mkdir(host, 0777) == 0 ? E_OK : EACCDN;
        } else {
            *ret = rmdir(host) == 0 ? E_OK : EACCDN;
        }
        return DONE;

    case 0x3B:              /* Dsetpath(path) */
        read_st_string(arg32(args, 2), path, sizeof(path));
        r = resolve(path, host, false, st_abs);
        if (r == 0) {
            return PASS;
        }
        if (r < 0) {
            *ret = EPTHNF;
            return DONE;
        }
        {
            struct stat sb;
            if (stat(host, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
                *ret = EPTHNF;
                return DONE;
            }
        }
        snprintf(hd.cwd, sizeof(hd.cwd), "%s", st_abs);
        *ret = E_OK;
        return DONE;

    case 0x47: {            /* Dgetpath(buf, drive) */
        int drive = arg16(args, 6);
        drive = drive ? drive - 1 : current_drive();
        if (drive != HD_DRIVE) {
            return PASS;
        }
        uint32_t buf = arg32(args, 2);
        int n = 0;
        if (hd.cwd[0]) {
            st_poke8(buf + n++, '\\');
        }
        for (const char *c = hd.cwd; *c; c++) {
            st_poke8(buf + n++, *c);
        }
        st_poke8(buf + n, 0);
        *ret = E_OK;
        return DONE;
    }

    case 0x3C:              /* Fcreate(fname, attr) */
    case 0x3D:              /* Fopen(fname, mode) */
        read_st_string(arg32(args, 2), path, sizeof(path));
        r = resolve(path, host, fn == 0x3C, NULL);
        if (r <= 0) {
            if (r == 0) return PASS;
            *ret = r;
            return DONE;
        }
        if (fn == 0x3C) {
            *ret = open_file(host, "w+b");
        } else {
            static const char *const modes[] = { "rb", "r+b", "r+b", "rb" };
            *ret = open_file(host, modes[arg16(args, 6) & 3]);
        }
        if (*ret >= 0) {
            note_file(fn == 0x3C ? "created" : "opened", host);
        }
        return DONE;

    case 0x3E: {            /* Fclose(handle) */
        int h = (int16_t)arg16(args, 2);
        if (!is_our_handle(h)) {
            return PASS;
        }
        hd_file_t *hf = file_from_handle(h);
        if (hf) {
            fclose(hf->f);
            hf->f = NULL;
            *ret = E_OK;
        } else {
            *ret = EIHNDL;
        }
        return DONE;
    }

    case 0x3F:              /* Fread(handle, count, buf) */
    case 0x40: {            /* Fwrite(handle, count, buf) */
        int h = (int16_t)arg16(args, 2);
        if (!is_our_handle(h)) {
            return PASS;
        }
        *ret = file_transfer(h, arg32(args, 4), arg32(args, 8), fn == 0x40);
        return DONE;
    }

    case 0x41:              /* Fdelete(fname) */
        read_st_string(arg32(args, 2), path, sizeof(path));
        r = resolve(path, host, false, NULL);
        if (r <= 0) {
            if (r == 0) return PASS;
            *ret = r;
            return DONE;
        }
        *ret = unlink(host) == 0 ? E_OK : EACCDN;
        return DONE;

    case 0x42: {            /* Fseek(offset, handle, mode) */
        int h = (int16_t)arg16(args, 6);
        if (!is_our_handle(h)) {
            return PASS;
        }
        hd_file_t *hf = file_from_handle(h);
        if (hf == NULL) {
            *ret = EIHNDL;
            return DONE;
        }
        static const int whence[] = { SEEK_SET, SEEK_CUR, SEEK_END };
        int mode = arg16(args, 8);
        if (mode > 2 || fseek(hf->f, (int32_t)arg32(args, 2), whence[mode]) != 0) {
            *ret = -64;     /* ERANGE */
        } else {
            *ret = ftell(hf->f);
        }
        return DONE;
    }

    case 0x43: {            /* Fattrib(fname, wflag, attr) */
        read_st_string(arg32(args, 2), path, sizeof(path));
        r = resolve(path, host, false, NULL);
        if (r <= 0) {
            if (r == 0) return PASS;
            *ret = r;
            return DONE;
        }
        struct stat sb;
        if (stat(host, &sb) != 0) {
            *ret = EFILNF;
            return DONE;
        }
        if (arg16(args, 6)) {
            uint8_t attr = arg16(args, 8);
            chmod(host, (attr & FA_RDONLY) ? (sb.st_mode & ~0222) : (sb.st_mode | 0200));
        }
        *ret = host_attr(&sb);
        return DONE;
    }

    case 0x4B: {            /* Pexec(mode, fname, cmdline, env) */
        int mode = arg16(args, 2);
        if (mode != 0 && mode != 3) {
            return PASS;
        }
        read_st_string(arg32(args, 4), path, sizeof(path));
        r = resolve(path, host, false, NULL);
        if (r <= 0) {
            if (r == 0) return PASS;
            *ret = r;
            return DONE;
        }
        hd.pexec_mode = mode;
        snprintf(hd.pexec_path, sizeof(hd.pexec_path), "%s", host);
        note_file("started", host);
        m68k_set_reg(M68K_REG_A0, arg32(args, 8));
        m68k_set_reg(M68K_REG_A1, arg32(args, 12));
        return PEXEC;
    }

    case 0x4E: {            /* Fsfirst(fspec, attr) */
        read_st_string(arg32(args, 2), path, sizeof(path));
        /* Resolve the folder part, the rest is the pattern */
        snprintf(path2, sizeof(path2), "%s", path);
        char *slash = strrchr(path2, '\\');
        if (slash) {
            slash[1] = 0;
        } else if (path2[0] && path2[1] == ':') {
            path2[2] = 0;
        } else {
            path2[0] = 0;
        }
        if (path2[0] == 0) {
            if (current_drive() != HD_DRIVE) {
                return PASS;
            }
            snprintf(path2, sizeof(path2), ".");
        }
        r = resolve(path2, host, false, NULL);
        if (r == 0) {
            return PASS;
        }
        if (r < 0) {
            *ret = EPTHNF;
            return DONE;
        }
        uint32_t dta = st_peek32(current_basepage() + BP_DTA);
        *ret = search_first(path, arg16(args, 6), dta, host);
        return DONE;
    }

    case 0x4F: {            /* Fsnext() */
        uint32_t dta = st_peek32(current_basepage() + BP_DTA);
        if (st_peek32(dta) != DTA_MAGIC) {
            return PASS;
        }
        *ret = search_next(dta);
        return DONE;
    }

    case 0x56:              /* Frename(0, old, new) */
        read_st_string(arg32(args, 4), path, sizeof(path));
        read_st_string(arg32(args, 8), path2, sizeof(path2));
        r = resolve(path, host, false, NULL);
        if (r <= 0) {
            if (r == 0) return PASS;
            *ret = r;
            return DONE;
        }
        r = resolve(path2, host2, true, NULL);
        if (r <= 0) {
            *ret = r == 0 ? -46 : r;    /* EDRIVE: across drives */
            return DONE;
        }
        *ret = rename(host, host2) == 0 ? E_OK : EACCDN;
        return DONE;

    case 0x57: {            /* Fdatime(timeptr, handle, wflag) */
        int h = (int16_t)arg16(args, 6);
        if (!is_our_handle(h)) {
            return PASS;
        }
        hd_file_t *hf = file_from_handle(h);
        if (hf == NULL) {
            *ret = EIHNDL;
            return DONE;
        }
        if (arg16(args, 8) == 0) {
            struct stat sb;
            uint16_t t = 0, d = 0;
            if (fstat(fileno(hf->f), &sb) == 0) {
                dos_datetime(sb.st_mtime, &t, &d);
            }
            uint32_t ptr = arg32(args, 2);
            st_poke16(ptr, t);
            st_poke16(ptr + 2, d);
        }
        *ret = E_OK;
        return DONE;
    }

    default:
        return PASS;
    }
}

/* ---- native calls from the cartridge -------------------------------------- */

/* Host local time as GEMDOS date (high word) and time (low word) */
static void native_gettime(void)
{
    uint16_t t, d;
    dos_datetime(time(NULL), &t, &d);
    m68k_set_reg(M68K_REG_D0, ((uint32_t)d << 16) | t);
}

static void native_install(void)
{
    if (!hd.enabled) {
        return;
    }
    uint32_t trap1 = st_peek32(SV_TRAP1);
    if (trap1 != CART_BASE + CART_TRAP1) {
        hd.old_trap1 = trap1;
        st_poke32(SV_TRAP1, CART_BASE + CART_TRAP1);
    }
    st_poke32(SV_DRVBITS, st_peek32(SV_DRVBITS) | (1u << HD_DRIVE));
    /* A disk in drive A: boots from there, as games expect */
    bool boot = hd.boot && !floppy_present(0);
    if (boot) {
        st_poke16(SV_BOOTDEV, HD_DRIVE);
        /* The AUTO folder process inherits the boot process's default
         * drive, set from _bootdev before cartridges run. _sysbase isn't
         * set up yet: find the process through the ROM's OS header.
         */
        uint32_t bp = st_peek32(st_peek32(st.rom_base + 0x28));
        if (st_ram_range(bp, 256)) {
            st_poke8(bp + BP_DEFDRV, HD_DRIVE);
        }
    }
    ST_LOGI("GEMDOS drive C: on %s%s", hd.root, boot ? ", boot drive" : "");
}

static void native_gemdos(void)
{
    uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7);
    uint16_t sr = st_peek16(sp);
    uint32_t args = (sr & 0x2000) ? sp + 6 : m68k_get_reg(NULL, M68K_REG_USP);
    int32_t ret = 0;

    switch (gemdos_call(args, &ret)) {
    case PASS:
        m68k_set_reg(M68K_REG_PC, hd.old_trap1);
        break;
    case DONE:
        m68k_set_reg(M68K_REG_D0, ret);
        m68k_set_reg(M68K_REG_PC, CART_BASE + CART_RTE);
        break;
    case PEXEC:
        hd.pexec_args = args;
        m68k_set_reg(M68K_REG_PC, CART_BASE + CART_PEXEC);
        break;
    }
}

static void native_pexec_load(void)
{
    int32_t bp = m68k_get_reg(NULL, M68K_REG_D0);
    if (bp < 0) {
        m68k_set_reg(M68K_REG_PC, CART_BASE + CART_RTE);     /* Pexec 5 failed */
        return;
    }
    int32_t err = load_program(bp);
    if (err) {
        m68k_set_reg(M68K_REG_D0, err);
        m68k_set_reg(M68K_REG_A0, bp);
        m68k_set_reg(M68K_REG_PC, CART_BASE + CART_PEXEC_FAIL);
        return;
    }
    if (hd.pexec_mode == 3) {
        m68k_set_reg(M68K_REG_PC, CART_BASE + CART_RTE);     /* load only: d0 = basepage */
        return;
    }
    /* Mode 0: turn the caller's own call into Pexec(6, -, basepage, -) and
     * hand it to TOS with the caller's exception frame back on top of the
     * stack. TOS then saves and restores the caller's registers where it
     * always does: from a nested call they'd sit on a stack the child's
     * interrupts overwrite.
     */
    st_poke16(hd.pexec_args + 2, 6);
    st_poke32(hd.pexec_args + 4, 0);
    st_poke32(hd.pexec_args + 8, bp);
    st_poke32(hd.pexec_args + 12, 0);
    m68k_set_reg(M68K_REG_PC, hd.old_trap1);
}

/* GEM's first start after a reset goes to the remembered resolution */
static void native_adjust_request(void)
{
    if (st.first_gem_start) {
        st.first_gem_start = false;
        int rez = st.start_resolution;
        if (rez >= 0 && rez <= 2) {
            m68k_set_reg(M68K_REG_D0, rez + 2);
        }
    }
}

int m68k_host_illegal(int opcode)
{
    /* 24-bit address bus: TOS jumps to ca_init with the flag byte still in
     * the top of the PC
     */
    uint32_t pc = m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF;
    if (opcode == OP_REPORT_REZ && pc == st.rez_report_pc && pc) {
        st_desktop_resolution(m68k_get_reg(NULL, M68K_REG_D2) & 3);
        return 1;
    }
    if (opcode == OP_ADJUST_REQUEST && pc == st.rez_adjust_pc && pc) {
        native_adjust_request();
        return 1;
    }
    if (pc < CART_BASE || pc >= CART_BASE + sizeof(cart_code)) {
        return 0;
    }
    switch (opcode) {
    case OP_GEMDOS:     native_gemdos(); return 1;
    case OP_INSTALL:    native_install(); return 1;
    case OP_PEXEC_LOAD: native_pexec_load(); return 1;
    case OP_GETTIME:    native_gettime(); return 1;
    default:            return 0;
    }
}

/* ---- setup ------------------------------------------------------------------ */

void gemdos_reset(void)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (hd.files[i].f) {
            fclose(hd.files[i].f);
            hd.files[i].f = NULL;
        }
    }
    for (int i = 0; i < MAX_SEARCHES; i++) {
        free(hd.searches[i].entries);
        hd.searches[i].entries = NULL;
    }
    hd.cwd[0] = 0;
    hd.old_trap1 = 0;
}

void gemdos_init(const char *root, bool boot)
{
    gemdos_reset();     /* restart: close what the previous session left open */
    memset(&hd, 0, sizeof(hd));
    last.seq += 2;
    last.verb = NULL;
    last.path[0] = 0;
    if (root != NULL) {
        hd.enabled = true;
        hd.boot = boot;
        snprintf(hd.root, sizeof(hd.root), "%s", root);
        size_t len = strlen(hd.root);
        if (len > 1 && hd.root[len - 1] == '/') {
            hd.root[len - 1] = 0;
        }
    }
    for (size_t i = 0; i < sizeof(cart_code); i += 2) {
        *(uint16_t *)(hd.cart + i) = (cart_code[i] << 8) | cart_code[i + 1];
    }
    st.cart = hd.cart;
    st.cart_size = sizeof(cart_code);
}
