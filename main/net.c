/*
 * WiFi station + NTP. Credentials are stored by ESP-IDF itself (WiFi config
 * in NVS); the link is re-established whenever it drops, so it stays up
 * while the ST runs.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "net.h"

#define MAX_SCAN    24

static const char *TAG = "net";

static volatile net_state_t s_state = NET_OFF;
static char s_ssid[33];
static esp_netif_t *s_netif;
static volatile bool s_scan_done;
static volatile bool s_paused;      /* no reconnects while picking a network */
static volatile bool s_time_synced;
static TimerHandle_t s_retry_timer;
static int s_retries;

static void time_synced(struct timeval *tv)
{
    s_time_synced = true;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    ESP_LOGI(TAG, "time set: %s", buf);
}

static void retry(TimerHandle_t t)
{
    if (s_ssid[0] && !s_paused && s_state != NET_CONNECTED) {
        s_state = NET_CONNECTING;
        esp_wifi_connect();
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) {
            s_state = NET_CONNECTING;
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        /* An IPv6 link-local address too: macOS asks <name>.local for one
         * and waits 5 s for an answer that never comes otherwise
         */
        esp_netif_create_ip6_linklocal(s_netif);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (s_state == NET_CONNECTED) {
            ESP_LOGW(TAG, "link lost (reason %d), reconnecting", d->reason);
            s_retries = 0;
        }
        if (s_paused) {
            s_state = s_ssid[0] ? NET_FAILED : NET_OFF;
            return;
        }
        s_state = ++s_retries > 3 ? NET_FAILED : NET_CONNECTING;
        /* Back off: 1 s at first, then every 10 s */
        xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(s_retries > 3 ? 10000 : 1000), 0);
        xTimerStart(s_retry_timer, 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        s_scan_done = true;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "connected to %s, " IPSTR, s_ssid, IP2STR(&e->ip_info.ip));
        s_state = NET_CONNECTED;
        s_retries = 0;
        esp_netif_sntp_start();
    }
}

bool net_init(void)
{
    setenv("TZ", CONFIG_ST_TIMEZONE, 1);
    tzset();

    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network stack init failed: %s", esp_err_to_name(err));
        return false;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, CONFIG_ST_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return false;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(1000), pdFALSE, NULL, retry);

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
                                 ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    sntp.start = false;
    sntp.sync_cb = time_synced;
    esp_netif_sntp_init(&sntp);

    /* The saved network, if any */
    wifi_config_t wc;
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0]) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", (const char *)wc.sta.ssid);
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

net_state_t net_state(void)
{
    return s_state;
}

const char *net_ssid(void)
{
    return s_ssid;
}

void net_ip(char *buf, int size)
{
    esp_netif_ip_info_t ip;
    if (s_state == NET_CONNECTED && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(buf, size, IPSTR, IP2STR(&ip.ip));
    } else {
        snprintf(buf, size, "-");
    }
}

void net_scan_start(void)
{
    s_scan_done = false;
    if (s_state != NET_CONNECTED) {
        /* A connect attempt in progress blocks scanning */
        s_paused = true;
        xTimerStop(s_retry_timer, 0);
        esp_wifi_disconnect();
    }
    wifi_scan_config_t scan = { .show_hidden = false };
    if (esp_wifi_scan_start(&scan, false) != ESP_OK) {
        s_scan_done = true;
    }
}

bool net_scan_done(void)
{
    return s_scan_done;
}

int net_scan_results(net_ap_t *aps, int max)
{
    uint16_t n = MAX_SCAN;
    wifi_ap_record_t *rec = calloc(MAX_SCAN, sizeof(wifi_ap_record_t));
    if (rec == NULL || esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) {
        free(rec);
        return 0;
    }
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        if (rec[i].ssid[0] == 0) {
            continue;
        }
        /* Strongest first from the driver; drop duplicate SSIDs */
        bool dup = false;
        for (int j = 0; j < count; j++) {
            dup |= strcmp(aps[j].ssid, (const char *)rec[i].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        snprintf(aps[count].ssid, sizeof(aps[count].ssid), "%s", (const char *)rec[i].ssid);
        aps[count].rssi = rec[i].rssi;
        aps[count].secure = rec[i].authmode != WIFI_AUTH_OPEN;
        count++;
    }
    free(rec);
    return count;
}

void net_resume(void)
{
    if (s_paused) {
        s_paused = false;
        s_retries = 0;
        retry(NULL);
    }
}

void net_connect(const char *ssid, const char *password)
{
    s_paused = false;
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", password);
    wc.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    s_retries = 0;
    s_state = NET_CONNECTING;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
}

bool net_time_synced(void)
{
    return s_time_synced;
}
