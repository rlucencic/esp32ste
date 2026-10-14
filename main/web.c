/*
 * Web page for the floppy disk images: http://<CONFIG_ST_HOSTNAME>.local
 *
 *   GET    /                    the page (web/index.html)
 *   GET    /api/floppies        {"free": bytes, "disks": [{"name", "size", "drive"}]}
 *   GET    /api/status          machine, drives, board and Bluetooth status
 *   PUT    /floppies/<name>     upload (raw body; also POST), replaces a disk
 *                               of that name unless it's in a drive
 *   GET    /floppies/<name>     download
 *   DELETE /floppies/<name>     delete, unless it's in a drive
 *
 * Uploads go to a temporary file first, so a broken transfer leaves no
 * half disk behind.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "ble_hid.h"
#include "emulator.h"
#include "floppies.h"
#include "net.h"
#include "st.h"
#include "web.h"

#define PREFIX          "/floppies/"
#define CHUNK           8192
#define MAX_UPLOAD      (4 * 1024 * 1024)

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* "%20" etc. in place; false if malformed */
static bool url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '%') {
            unsigned v;
            if (!s[1] || !s[2] || sscanf(s + 1, "%2x", &v) != 1) {
                return false;
            }
            *out++ = v;
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = 0;
    return true;
}

/* Disk name from the URI, checked; false sends the error */
static bool disk_name(httpd_req_t *req, char *name, size_t len)
{
    const char *p = req->uri + strlen(PREFIX);
    snprintf(name, len, "%.*s", (int)strcspn(p, "?#"), p);
    if (!url_decode(name) || !floppies_valid_name(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a .ST or .MSA file name");
        return false;
    }
    return true;
}

static void disk_path(char *path, size_t len, const char *name)
{
    snprintf(path, len, "%s/%s", floppies_folder(), name);
}

static esp_err_t send_conflict(httpd_req_t *req, const char *name)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "%.100s is in a drive: eject it on the Atari's setup screen first", name);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

/* JSON string: quotes and backslashes escaped, control characters dropped */
static void json_string(httpd_req_t *req, const char *s)
{
    char buf[200];
    int n = 0;
    buf[n++] = '"';
    for (; *s && n < (int)sizeof(buf) - 3; s++) {
        if (*s == '"' || *s == '\\') {
            buf[n++] = '\\';
            buf[n++] = *s;
        } else if ((unsigned char)*s >= 0x20) {
            buf[n++] = *s;
        }
    }
    buf[n++] = '"';
    buf[n] = 0;
    httpd_resp_sendstr_chunk(req, buf);
}

static esp_err_t list_get(httpd_req_t *req)
{
    char **names;
    int n = floppies_list(&names);
    uint64_t total = 0, free_bytes = 0;
    st_host_disk_free(floppies_folder(), &total, &free_bytes);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ready\":%s,\"free\":%llu,\"disks\":[",
             floppies_ready() ? "true" : "false", (unsigned long long)free_bytes);
    httpd_resp_sendstr_chunk(req, buf);
    for (int i = 0; i < n; i++) {
        char path[160];
        struct stat st;
        disk_path(path, sizeof(path), names[i]);
        long size = stat(path, &st) == 0 ? (long)st.st_size : 0;
        const char *drive = "";
        for (int d = 0; d < 2; d++) {
            if (floppies_name(d) != NULL && strcmp(floppies_name(d), names[i]) == 0) {
                drive = d ? "B" : "A";
            }
        }
        httpd_resp_sendstr_chunk(req, i ? ",{\"name\":" : "{\"name\":");
        json_string(req, names[i]);
        snprintf(buf, sizeof(buf), ",\"size\":%ld,\"drive\":\"%s\"}", size, drive);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    floppies_free_list(names, n);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---- status -------------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t len, size;
} json_t;

static void jprintf(json_t *j, const char *fmt, ...)
{
    if (j->len >= j->size) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, j->size - j->len, fmt, ap);
    va_end(ap);
    j->len += n > 0 ? n : 0;
}

/* "key":"escaped string" (or null) */
static void jstr(json_t *j, const char *key, const char *s)
{
    jprintf(j, "\"%s\":", key);
    if (s == NULL) {
        jprintf(j, "null");
        return;
    }
    jprintf(j, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            jprintf(j, "\\%c", *s);
        } else if ((unsigned char)*s >= 0x20) {
            jprintf(j, "%c", *s);
        }
    }
    jprintf(j, "\"");
}

static void jnum(json_t *j, const char *key, double v)
{
    if (isnan(v)) {
        jprintf(j, "\"%s\":null", key);
    } else {
        jprintf(j, "\"%s\":%.1f", key, v);
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    char *buf = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    json_t j = { buf, 0, 3072 };
    emu_info_t info;
    emulator_measure_temperature();
    emulator_info(&info);
    static const char *const states[] = { "setup", "running", "paused", "halted" };

    /* The machine */
    jprintf(&j, "{\"machine\":{");
    jstr(&j, "state", states[info.state]);
    if (info.tos_version) {
        char tos[8];
        snprintf(tos, sizeof(tos), "%x.%02x", info.tos_version >> 8, info.tos_version & 0xFF);
        jprintf(&j, ",");
        jstr(&j, "tos", tos);
        jprintf(&j, ",\"ram_kb\":%u,", info.ram_kb);
        st_screen_mode_t mode;
        st_get_screen_mode(&mode);
        jprintf(&j, "\"width\":%d,\"height\":%d,\"colours\":%d,\"hz\":%d,",
                mode.width, mode.height, 1 << mode.planes, mode.hz);
        uint32_t ram_free, ram_largest;
        if (st_ram_free(&ram_free, &ram_largest)) {
            jprintf(&j, "\"ram_free\":%lu,\"ram_largest\":%lu,",
                    (unsigned long)ram_free, (unsigned long)ram_largest);
        }
        jnum(&j, "fps", info.fps);
        jprintf(&j, ",\"speed\":%d,\"session\":%lu", info.speed, (unsigned long)info.session_s);
    }
    time_t now = time(NULL);
    struct tm tm;
    char clock[32];
    localtime_r(&now, &tm);
    strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M:%S", &tm);
    jprintf(&j, ",");
    jstr(&j, "clock", clock);
    jprintf(&j, ",\"clock_set\":%s}", net_time_synced() ? "true" : "false");

    /* Drives */
    st_floppy_status_t fs;
    st_floppy_status(&fs);
    jprintf(&j, ",\"drives\":{\"light\":%d,\"floppy\":[", fs.light);
    for (int d = 0; d < 2; d++) {
        jprintf(&j, d ? ",{" : "{");
        jstr(&j, "name", floppies_name(d));
        jprintf(&j, ",\"read_only\":%s,\"track\":%d,\"reads\":%lu,\"writes\":%lu}",
                st_floppy_write_protected(d) ? "true" : "false", fs.track[d],
                (unsigned long)fs.reads[d], (unsigned long)fs.writes[d]);
    }
    char last[100];
    const char *verb = st_hd_last_file(last, sizeof(last));
    jprintf(&j, "],\"hard_disk\":%s,", info.hard_disk ? "true" : "false");
    jstr(&j, "last_file", verb ? last : NULL);
    jprintf(&j, ",");
    jstr(&j, "last_verb", verb);
    jprintf(&j, "}");

    /* The board */
    char ip[20] = "";
    wifi_ap_record_t ap;
    bool connected = net_state() == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (connected) {
        net_ip(ip, sizeof(ip));
    }
    uint64_t total = 0, free_bytes = 0;
    bool sd = floppies_ready() && st_host_disk_free(floppies_folder(), &total, &free_bytes);
    jprintf(&j, ",\"board\":{\"uptime\":%lu,\"wifi\":{",
            (unsigned long)(esp_timer_get_time() / 1000000));
    jstr(&j, "ssid", connected ? net_ssid() : NULL);
    jprintf(&j, ",");
    jstr(&j, "ip", connected ? ip : NULL);
    jprintf(&j, ",\"rssi\":%d},", connected ? ap.rssi : 0);
    jprintf(&j, "\"internal_free\":%u,\"internal_min\":%u,\"internal_largest\":%u,"
                "\"psram_free\":%u,\"psram_total\":%u,",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    jprintf(&j, "\"load\":[%.0f,%.0f],", info.load[0], info.load[1]);
    jnum(&j, "temperature", info.temperature);
    jprintf(&j, ",\"sd_total\":%llu,\"sd_free\":%llu}",
            (unsigned long long)(sd ? total : 0), (unsigned long long)(sd ? free_bytes : 0));

    /* Bluetooth */
    jprintf(&j, ",\"bluetooth\":[");
#if CONFIG_ST_BLE_HID
    ble_hid_device_t dev[4];
    int n = ble_hid_devices(dev, 4);
    static const char *const kinds[] = { "device", "keyboard", "mouse" };
    for (int i = 0; i < n; i++) {
        jprintf(&j, i ? ",{" : "{");
        jstr(&j, "name", dev[i].name);
        jprintf(&j, ",");
        jstr(&j, "kind", kinds[dev[i].kind < 3 ? dev[i].kind : 0]);
        jprintf(&j, ",\"connected\":%s,\"battery\":", dev[i].connected ? "true" : "false");
        jprintf(&j, dev[i].battery >= 0 ? "%d}" : "null}", dev[i].battery);
    }
#endif
    jprintf(&j, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, j.len < j.size ? j.len : j.size - 1);
    free(buf);
    return err;
}

static esp_err_t disk_put(httpd_req_t *req)
{
    char name[100], path[160], tmp[170];
    if (!disk_name(req, name, sizeof(name))) {
        return ESP_OK;
    }
    if (!floppies_ready()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No SD card");
        return ESP_OK;
    }
    if (floppies_in_use(name)) {
        return send_conflict(req, name);
    }
    if (req->content_len == 0 || req->content_len > MAX_UPLOAD) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or too big for a floppy disk");
        return ESP_OK;
    }

    disk_path(path, sizeof(path), name);
    snprintf(tmp, sizeof(tmp), "%s/upload.tmp", floppies_folder());
    FILE *f = fopen(tmp, "wb");
    /* DMA-capable: the SD driver then writes whole runs of sectors at once
     * instead of copying them one by one (several times faster)
     */
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_DMA);
    if (f == NULL || buf == NULL) {
        if (f) {
            fclose(f);
        }
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Can't write to the SD card");
        return ESP_OK;
    }

    size_t left = req->content_len;
    bool ok = true;
    while (left > 0 && ok) {
        int n = httpd_req_recv(req, buf, left < CHUNK ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        ok = n > 0 && fwrite(buf, 1, n, f) == (size_t)n;
        left -= n > 0 ? n : 0;
    }
    free(buf);
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path);
        floppies_remove_unpacked(name);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        ESP_LOGW(TAG, "upload of %s failed", name);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "received %s, %u bytes", name, (unsigned)req->content_len);
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t disk_get(httpd_req_t *req)
{
    char name[100], path[160];
    if (!disk_name(req, name, sizeof(name))) {
        return ESP_OK;
    }
    disk_path(path, sizeof(path), name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such disk");
        return ESP_OK;
    }
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    char disposition[140];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%.100s\"", name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, CHUNK, f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    free(buf);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t disk_delete(httpd_req_t *req)
{
    char name[100], path[160];
    if (!disk_name(req, name, sizeof(name))) {
        return ESP_OK;
    }
    if (floppies_in_use(name)) {
        return send_conflict(req, name);
    }
    disk_path(path, sizeof(path), name);
    floppies_remove_unpacked(name);
    if (remove(path) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such disk");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "deleted %s", name);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

void web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 0;
    config.stack_size = 6144;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "server start failed: %s", esp_err_to_name(err));
        return;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/",              .method = HTTP_GET,    .handler = page_get },
        { .uri = "/api/floppies",  .method = HTTP_GET,    .handler = list_get },
        { .uri = "/api/status",    .method = HTTP_GET,    .handler = status_get },
        { .uri = PREFIX "*",       .method = HTTP_PUT,    .handler = disk_put },
        { .uri = PREFIX "*",       .method = HTTP_POST,   .handler = disk_put },
        { .uri = PREFIX "*",       .method = HTTP_GET,    .handler = disk_get },
        { .uri = PREFIX "*",       .method = HTTP_DELETE, .handler = disk_delete },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(CONFIG_ST_HOSTNAME);
        mdns_instance_name_set("Atari STE");
        mdns_service_add("Atari STE floppies", "_http", "_tcp", 80, NULL, 0);
    }
    ESP_LOGI(TAG, "floppy disk page on http://%s.local", CONFIG_ST_HOSTNAME);
}
