#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "nvs.h"
#include "floppies.h"
#include "sdcard.h"
#include "st.h"

#define FOLDER      SDCARD_MOUNT_POINT "/" CONFIG_ST_FLOPPY_FOLDER
#define NAMESPACE   "floppy"
#define MAX_IMAGES  500
#define NAME_LEN    96

static const char *TAG = "floppies";
static const char *const drive_keys[2] = { "disk_a", "disk_b" };

static bool s_ready;
static char s_names[2][NAME_LEN];

static bool has_extension(const char *name, const char *ext)
{
    size_t len = strlen(name), elen = strlen(ext);
    return len > elen && strcasecmp(name + len - elen, ext) == 0;
}

static bool is_image(const char *name)
{
    return name[0] != '.' && (has_extension(name, ".st") || has_extension(name, ".msa"));
}

static void remember(int drive)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, drive_keys[drive], s_names[drive]);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool floppies_init(void)
{
    struct stat st;
    if (stat(SDCARD_MOUNT_POINT, &st) != 0) {
        return false;
    }
    if (stat(FOLDER, &st) != 0 && mkdir(FOLDER, 0777) != 0) {
        ESP_LOGW(TAG, "can't make %s", FOLDER);
        return false;
    }
    s_ready = true;
    return true;
}

bool floppies_ready(void)
{
    return s_ready;
}

void floppies_restore(void)
{
    nvs_handle_t h;
    if (!s_ready || nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (int d = 0; d < 2; d++) {
        char name[NAME_LEN];
        size_t len = sizeof(name);
        if (nvs_get_str(h, drive_keys[d], name, &len) == ESP_OK && name[0]) {
            if (!floppies_insert(d, name)) {
                ESP_LOGW(TAG, "%c: %s is gone", 'A' + d, name);
            }
        }
    }
    nvs_close(h);
}

static int compare_names(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

int floppies_list(char ***names)
{
    *names = NULL;
    DIR *dir = s_ready ? opendir(FOLDER) : NULL;
    if (dir == NULL) {
        return 0;
    }
    char **list = malloc(MAX_IMAGES * sizeof(char *));
    int n = 0;
    struct dirent *e;
    while (list != NULL && n < MAX_IMAGES && (e = readdir(dir)) != NULL) {
        if (e->d_type != DT_DIR && is_image(e->d_name)) {
            list[n] = strdup(e->d_name);
            if (list[n] != NULL) {
                n++;
            }
        }
    }
    closedir(dir);
    if (list != NULL) {
        qsort(list, n, sizeof(char *), compare_names);
    }
    *names = list;
    return n;
}

void floppies_free_list(char **names, int count)
{
    for (int i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

bool floppies_insert(int drive, const char *name)
{
    char path[sizeof(FOLDER) + NAME_LEN + 1];
    snprintf(path, sizeof(path), "%s/%s", FOLDER, name);
    if (!s_ready || !st_floppy_insert(drive, path)) {
        return false;
    }
    snprintf(s_names[drive], NAME_LEN, "%s", name);
    remember(drive);
    return true;
}

void floppies_eject(int drive)
{
    st_floppy_eject(drive);
    s_names[drive][0] = 0;
    remember(drive);
}

const char *floppies_name(int drive)
{
    return st_floppy_path(drive) != NULL ? s_names[drive] : NULL;
}

const char *floppies_folder(void)
{
    return FOLDER;
}

bool floppies_valid_name(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len >= NAME_LEN - 1 || !is_image(name)) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = name[i];
        if (c < 0x20 || strchr("/\\:*?\"<>|", c) != NULL) {
            return false;
        }
    }
    return true;
}

void floppies_remove_unpacked(const char *name)
{
    char path[sizeof(FOLDER) + NAME_LEN + 1], copy[sizeof(path) + 8];
    snprintf(path, sizeof(path), "%s/%s", FOLDER, name);
    st_floppy_unpacked_path(path, copy, sizeof(copy));
    remove(copy);
}

bool floppies_in_use(const char *name)
{
    for (int d = 0; d < 2; d++) {
        if (floppies_name(d) != NULL && strcasecmp(floppies_name(d), name) == 0) {
            return true;
        }
    }
    return false;
}

/* EMPTYnnnn.ST: one more than the highest number ever made (kept in NVS,
 * so deleting the newest image doesn't bring its name back) or in the folder
 */
bool floppies_create(int drive, char *name, size_t len)
{
    if (!s_ready) {
        return false;
    }
    uint16_t last = 0;
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, "empty_n", &last);
        nvs_close(h);
    }
    DIR *dir = opendir(FOLDER);
    struct dirent *e;
    while (dir != NULL && (e = readdir(dir)) != NULL) {
        unsigned n;
        if (strncasecmp(e->d_name, "EMPTY", 5) == 0 && sscanf(e->d_name + 5, "%4u", &n) == 1 &&
            n > last) {
            last = n;
        }
    }
    if (dir != NULL) {
        closedir(dir);
    }
    if (last >= 9999) {
        return false;
    }

    snprintf(name, len, "EMPTY%04u.ST", last + 1);
    char path[sizeof(FOLDER) + NAME_LEN + 1];
    snprintf(path, sizeof(path), "%s/%s", FOLDER, name);
    if (!st_floppy_create(path)) {
        return false;
    }
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "empty_n", last + 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "made %s", name);
    return floppies_insert(drive, name);
}
