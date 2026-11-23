/* Create a bootable AHDI disk from the loader and driver on a user's image. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "st.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_partition.h"
#endif

#define SECTOR 512u
#define MIB (1024u * 1024u)
#define PART_TABLE 0x1c6
#define FIRST_SECTOR 16u
#define ROOT_ENTRIES 256u
#define DRIVER_NAME "SHDRIVERSYS"
#define ZERO_CHUNK (8u * 1024u)

typedef struct {
    uint32_t sector_bytes, cluster_bytes, fat_sectors, root_entries, clusters;
    uint32_t fat, root, data;            /* byte offsets in the image */
} fat16_t;

typedef struct {
    uint8_t root[SECTOR], boot[SECTOR], entry[32];
    fat16_t fat;
} template_t;

typedef struct {
    FILE *file;
    uint32_t size;
#ifdef ESP_PLATFORM
    const esp_partition_t *partition;
#endif
} source_t;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t le16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (uint32_t)le16(p + 2) << 16;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static uint16_t boot_sum(const uint8_t *sector)
{
    uint16_t sum = 0;
    for (unsigned i = 0; i < SECTOR; i += 2) {
        sum += (uint16_t)sector[i] << 8 | sector[i + 1];
    }
    return sum;
}

static void set_boot_sum(uint8_t *sector)
{
    sector[510] = sector[511] = 0;
    uint16_t last = 0x1234 - boot_sum(sector);
    sector[510] = last >> 8;
    sector[511] = last;
}

static bool read_source(const source_t *source, uint32_t offset, void *buf, size_t len)
{
    if (offset > source->size || len > source->size - offset) {
        return false;
    }
#ifdef ESP_PLATFORM
    if (source->partition) {
        return esp_partition_read(source->partition, offset, buf, len) == ESP_OK;
    }
#endif
    return fseeko(source->file, offset, SEEK_SET) == 0 && fread(buf, 1, len, source->file) == len;
}

static bool write_at(FILE *file, uint32_t offset, const void *buf, size_t len)
{
    return fseeko(file, offset, SEEK_SET) == 0 && fwrite(buf, 1, len, file) == len;
}

static bool fat_layout(const uint8_t *boot, uint32_t base, uint32_t bytes, fat16_t *fat)
{
    uint32_t sector = le16(boot + 11), spc = boot[13], reserved = le16(boot + 14);
    uint32_t copies = boot[16], entries = le16(boot + 17);
    uint32_t total = le16(boot + 19), fat_sectors = le16(boot + 22);
    if (sector < SECTOR || sector > 8192 || (sector & (sector - 1)) ||
        spc == 0 || (spc & (spc - 1)) || spc * sector > 65536 ||
        reserved == 0 || copies == 0 || copies > 2 || entries == 0 || fat_sectors == 0 ||
        total == 0 || total > bytes / sector) {
        return false;
    }
    uint32_t root = reserved + copies * fat_sectors;
    uint32_t data = root + (entries * 32 + sector - 1) / sector;
    if (data >= total) {
        return false;
    }
    uint32_t clusters = (total - data) / spc;
    if (clusters == 0 || clusters >= 0xffee || (clusters + 2) * 2 > fat_sectors * sector) {
        return false;
    }
    *fat = (fat16_t){ sector, spc * sector, fat_sectors, entries, clusters,
                      base + reserved * sector, base + root * sector, base + data * sector };
    return true;
}

static bool find_driver(const source_t *source, template_t *tpl)
{
    for (uint32_t n = 0; n < tpl->fat.root_entries; n++) {
        uint8_t *e = tpl->entry;
        if (!read_source(source, tpl->fat.root + n * 32, e, 32) || e[0] == 0) {
            break;
        }
        if (!(e[11] & 0x18) && memcmp(e, DRIVER_NAME, 11) == 0) {
            uint32_t bytes = le32(e + 28);
            uint16_t cluster = le16(e + 26);
            return cluster >= 2 && cluster < tpl->fat.clusters + 2 &&
                   bytes > 0 && bytes <= 256 * 1024;
        }
    }
    return false;
}

static bool read_template(const source_t *source, template_t *tpl)
{
    if (source->size < 3 * SECTOR ||
        !read_source(source, 0, tpl->root, SECTOR) || boot_sum(tpl->root) != 0x1234) {
        return false;
    }
    uint32_t sectors = source->size / SECTOR;
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = tpl->root + PART_TABLE + 12 * i;
        uint32_t start = be32(p + 4), count = be32(p + 8);
        if ((p[0] & 0x81) == 0x81 && (!memcmp(p + 1, "GEM", 3) || !memcmp(p + 1, "BGM", 3)) &&
            start > 0 && start < sectors && count > 0 && count <= sectors - start &&
            read_source(source, start * SECTOR, tpl->boot, SECTOR) && boot_sum(tpl->boot) == 0x1234 &&
            fat_layout(tpl->boot, start * SECTOR, count * SECTOR, &tpl->fat) && find_driver(source, tpl)) {
            return true;
        }
    }
    return false;
}

static bool open_template(source_t *source, const char *path, template_t *tpl)
{
    *source = (source_t){0};
#ifdef ESP_PLATFORM
    source->partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "ahdi");
    if (source->partition) {
        source->size = source->partition->size;
        if (read_template(source, tpl)) {
            return true;
        }
        source->partition = NULL;
    }
#endif
    source->file = fopen(path, "rb");
    if (!source->file) {
        return false;
    }
    off_t size = fseeko(source->file, 0, SEEK_END) == 0 ? ftello(source->file) : -1;
    if (size >= 0 && size <= INT32_MAX) {
        source->size = size;
        if (read_template(source, tpl)) {
            return true;
        }
    }
    fclose(source->file);
    return false;
}

static void close_template(const source_t *source)
{
    if (source->file) {
        fclose(source->file);
    }
}

bool st_ahdi_template(const char *path)
{
    source_t source;
    template_t tpl;
    if (!open_template(&source, path, &tpl)) {
        return false;
    }
    close_template(&source);
    return true;
}

static bool copy_driver(const source_t *in, const template_t *tpl, FILE *out, const fat16_t *dest, uint8_t *buf)
{
    uint32_t cluster = le16(tpl->entry + 26), left = le32(tpl->entry + 28);
    uint32_t to = dest->data;
    while (left > 0) {
        if (cluster < 2 || cluster >= tpl->fat.clusters + 2) {
            return false;
        }
        uint32_t offset = tpl->fat.data + (cluster - 2) * tpl->fat.cluster_bytes;
        uint32_t remaining = left < tpl->fat.cluster_bytes ? left : tpl->fat.cluster_bytes;
        while (remaining > 0) {
            size_t count = remaining < ZERO_CHUNK ? remaining : ZERO_CHUNK;
            if (!read_source(in, offset, buf, count) || !write_at(out, to, buf, count)) {
                return false;
            }
            offset += count;
            to += count;
            left -= count;
            remaining -= count;
        }
        uint8_t next[2];
        if (!read_source(in, tpl->fat.fat + cluster * 2, next, sizeof(next))) {
            return false;
        }
        cluster = le16(next);
    }
    return cluster >= 0xfff8;
}

static bool format_disk(FILE *out, const source_t *source, const template_t *tpl, uint32_t bytes, uint8_t *buf)
{
    const uint32_t base = FIRST_SECTOR * SECTOR;
    /* Keep two logical sectors per cluster and a 16-bit logical sector count. */
    uint32_t sector = bytes < 32 * MIB ? SECTOR : bytes / 32768, spc = 2;
    uint32_t total = (bytes - base) / sector;
    uint32_t fat_sectors = ((total / spc + 2) * 2 + sector - 1) / sector;
    uint8_t root[SECTOR] = {0}, boot[SECTOR];
    memcpy(root, tpl->root, 0x1c2);
    put_be32(root + 0x1c2, bytes / SECTOR);
    root[PART_TABLE] = 0x81;
    memcpy(root + PART_TABLE + 1, sector == SECTOR ? "GEM" : "BGM", 3);
    put_be32(root + PART_TABLE + 4, FIRST_SECTOR);
    put_be32(root + PART_TABLE + 8, total * (sector / SECTOR));
    put_be32(root + 0x1f6, 1);            /* One empty bad-sector-list sector. */
    put_be32(root + 0x1fa, 1);
    set_boot_sum(root);

    memcpy(boot, tpl->boot, sizeof(boot));
    put_le16(boot + 11, sector);
    boot[13] = spc;
    put_le16(boot + 14, 1);
    boot[16] = 2;
    put_le16(boot + 17, ROOT_ENTRIES);
    put_le16(boot + 19, total);
    boot[21] = 0xf8;
    put_le16(boot + 22, fat_sectors);
    memset(boot + 24, 0, 6);            /* Hard disks have no floppy geometry. */
    set_boot_sum(boot);

    fat16_t fat;
    if (!fat_layout(boot, base, bytes - base, &fat) ||
        !write_at(out, 0, root, sizeof(root)) || !write_at(out, base, boot, sizeof(boot))) {
        return false;
    }

    uint8_t entry[32];
    memcpy(entry, tpl->entry, sizeof(entry));
    put_le16(entry + 26, 2);
    uint32_t count = (le32(entry + 28) + fat.cluster_bytes - 1) / fat.cluster_bytes;
    size_t chain_bytes = ((count + 2) * 2 + SECTOR - 1) / SECTOR * SECTOR;
    memset(buf, 0, chain_bytes);
    put_le16(buf, 0xfff8);
    put_le16(buf + 2, 0xffff);
    for (uint32_t i = 0; i < count; i++) {
        put_le16(buf + (i + 2) * 2, i + 1 == count ? 0xffff : i + 3);
    }
    return write_at(out, fat.fat, buf, chain_bytes) &&
           write_at(out, fat.fat + fat.fat_sectors * fat.sector_bytes, buf, chain_bytes) &&
           write_at(out, fat.root, entry, sizeof(entry)) &&
           copy_driver(source, tpl, out, &fat, buf);
}

static uint8_t *zero_buffer(void)
{
#ifdef ESP_PLATFORM
    /* The S3 writes a PSRAM buffer to the SD card one sector at a time. */
    uint8_t *dma = heap_caps_calloc(1, ZERO_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (dma) {
        return dma;
    }
#endif
    return calloc(1, ZERO_CHUNK);
}

static bool create_disk(const char *path, const char *source_path, uint32_t bytes)
{
    source_t source;
    template_t tpl;
    if (!open_template(&source, source_path, &tpl)) {
        return false;
    }
    uint8_t *buf = zero_buffer();
    if (!buf) {
        close_template(&source);
        return false;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    bool ok = fd >= 0;
    /* Write whole runs of SD sectors, bypassing stdio's small buffer. */
    for (uint32_t offset = 0; ok && offset < bytes; offset += ZERO_CHUNK) {
        ok = write(fd, buf, ZERO_CHUNK) == ZERO_CHUNK;
    }
    FILE *out = ok ? fdopen(fd, "wb") : NULL;
    if (out) {
        ok = format_disk(out, &source, &tpl, bytes, buf);
        ok = fclose(out) == 0 && ok;
    } else if (fd >= 0) {
        close(fd);
        ok = false;
    }
    if (fd >= 0 && !ok) {
        remove(path);
    }
    free(buf);
    close_template(&source);
    return ok;
}

bool st_ahdi_create(const char *path, const char *source_path, uint32_t bytes)
{
    return (bytes == 32 * MIB || bytes == 128 * MIB || bytes == 256 * MIB) &&
           create_disk(path, source_path, bytes);
}

#ifndef ESP_PLATFORM
bool st_ahdi_prepare_template(const char *path, const char *source_path)
{
    return create_disk(path, source_path, 512 * 1024);
}
#endif
