/*
 * BLE HID keyboard + mouse -> Atari ST IKBD.
 *
 * Connection handling follows esp32_basic's ble_hid_host.c: connect,
 * encrypt/bond, ask for Boot Protocol, subscribe to the input reports and
 * route them by length (8 bytes keyboard, 3-4 mouse).
 *
 * Only paired devices (kept in NVS, with name and kind) connect by
 * themselves. New devices are paired from the setup screen: while pairing
 * is on, HID devices in pairing mode are listed and the user picks one.
 */
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "ble_hid.h"
#include "st.h"

#define TAG "ble_hid"

#define HID_SVC_UUID16      0x1812
#define CHR_PROTOCOL_MODE   0x2A4E
#define BATTERY_SVC_UUID16  0x180F
#define CHR_BATTERY_LEVEL   0x2A19
#define APPEARANCE_KEYBOARD 0x03C1
#define APPEARANCE_MOUSE    0x03C2

#define MAX_NOTIF_CHRS      8
#define MAX_HID_CONNS       2       /* one keyboard + one mouse */
#define MAX_KNOWN           4
#define MAX_CANDIDATES      8
#define NVS_NAMESPACE       "blehid"

typedef struct {
    uint16_t conn_handle;           /* BLE_HS_CONN_HANDLE_NONE when free */
    uint8_t kind;
    uint16_t svc_start, svc_end;
    uint16_t notif_handles[MAX_NOTIF_CHRS];
    int notif_count;
    int sub_idx;
    uint16_t protomode_handle;
    uint8_t prev_report[8];         /* last keyboard report, for press/release edges */
    int mouse_rem_x, mouse_rem_y;   /* motion left over from the divider */
    bool pairing;                   /* new device: remember it once bonded */
    char name[32];
    uint16_t batt_start, batt_end;  /* Battery service, if the device has one */
    uint16_t batt_handle;
    bool batt_notify;
    int8_t battery;                 /* %, -1 = unknown */
} hid_conn_t;

typedef struct {
    ble_addr_t addr;
    uint8_t kind;
    char name[32];
} known_t;

typedef struct {
    ble_addr_t addr;
    uint8_t kind;
    int8_t rssi;
    char name[32];
} candidate_t;

static hid_conn_t s_conns[MAX_HID_CONNS];
static uint8_t s_own_addr_type;
static bool s_synced;
static bool s_connecting;
static uint8_t s_pending_kind;
static bool s_pending_pair;
static char s_pending_name[32];

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static known_t s_known[MAX_KNOWN];
static int s_known_count;
static candidate_t s_cand[MAX_CANDIDATES];
static int s_cand_count;
static bool s_pairing;
static char s_status[64];

/* ---- HID usage -> ST scancode ------------------------------------------- */

/* HID keyboard usages 0x00-0x67 (US positions). 0 = no ST key. */
static const uint8_t usage_to_st[0x68] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,     /* a b c d */
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,     /* e f g h */
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,     /* i j k l */
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,     /* m n o p */
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,     /* q r s t */
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,     /* u v w x */
    [0x1C] = 0x15, [0x1D] = 0x2C,                                   /* y z */
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05,     /* 1 2 3 4 */
    [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,     /* 5 6 7 8 */
    [0x26] = 0x0A, [0x27] = 0x0B,                                   /* 9 0 */
    [0x28] = 0x1C,      /* Return */
    [0x29] = 0x01,      /* Esc */
    [0x2A] = 0x0E,      /* Backspace */
    [0x2B] = 0x0F,      /* Tab */
    [0x2C] = 0x39,      /* Space */
    [0x2D] = 0x0C,      /* - */
    [0x2E] = 0x0D,      /* = */
    [0x2F] = 0x1A,      /* [ */
    [0x30] = 0x1B,      /* ] */
    [0x31] = 0x2B,      /* \ */
    [0x32] = 0x2B,      /* non-US # */
    [0x33] = 0x27,      /* ; */
    [0x34] = 0x28,      /* ' */
    [0x35] = 0x29,      /* ` */
    [0x36] = 0x33,      /* , */
    [0x37] = 0x34,      /* . */
    [0x38] = 0x35,      /* / */
    [0x39] = 0x3A,      /* Caps Lock */
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,     /* F1-F4 */
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,     /* F5-F8 */
    [0x42] = 0x43, [0x43] = 0x44,                                   /* F9 F10 */
    [0x44] = 0x61,      /* F11 -> Undo */
    [0x45] = 0x62,      /* F12 -> Help */
    [0x49] = 0x52,      /* Insert */
    [0x4A] = 0x47,      /* Home -> Clr/Home */
    [0x4B] = 0x62,      /* Page Up -> Help */
    [0x4C] = 0x53,      /* Delete */
    [0x4E] = 0x61,      /* Page Down -> Undo */
    [0x4F] = 0x4D,      /* Right */
    [0x50] = 0x4B,      /* Left */
    [0x51] = 0x50,      /* Down */
    [0x52] = 0x48,      /* Up */
    [0x53] = 0x63,      /* Num Lock -> keypad ( */
    [0x54] = 0x65,      /* keypad / */
    [0x55] = 0x66,      /* keypad * */
    [0x56] = 0x4A,      /* keypad - */
    [0x57] = 0x4E,      /* keypad + */
    [0x58] = 0x72,      /* keypad Enter */
    [0x59] = 0x6D, [0x5A] = 0x6E, [0x5B] = 0x6F,                    /* keypad 1 2 3 */
    [0x5C] = 0x6A, [0x5D] = 0x6B, [0x5E] = 0x6C,                    /* keypad 4 5 6 */
    [0x5F] = 0x67, [0x60] = 0x68, [0x61] = 0x69,                    /* keypad 7 8 9 */
    [0x62] = 0x70,      /* keypad 0 */
    [0x63] = 0x71,      /* keypad . */
    [0x64] = 0x60,      /* non-US \ (ISO key left of Z) */
    [0x67] = 0x64,      /* keypad = -> keypad ) */
};

/* Modifier byte bits: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI */
static const uint8_t modifier_to_st[8] = { 0x1D, 0x2A, 0x38, 0, 0x1D, 0x36, 0x38, 0 };

static bool report_has(const uint8_t *report, uint8_t usage)
{
    for (int i = 2; i < 8; i++) {
        if (report[i] == usage) {
            return true;
        }
    }
    return false;
}

/* ST Control and Alternate have one key each: down while either side is */
static bool modifier_down(uint8_t mods, int bit)
{
    uint8_t code = modifier_to_st[bit];
    for (int i = 0; i < 8; i++) {
        if (modifier_to_st[i] == code && (mods & (1u << i))) {
            return true;
        }
    }
    return false;
}

static void send_keyboard_edges(const uint8_t *prev, const uint8_t *cur)
{
    for (int bit = 0; bit < 8; bit++) {
        uint8_t code = modifier_to_st[bit];
        bool was = modifier_down(prev[0], bit);
        bool now = modifier_down(cur[0], bit);
        /* Only the first bit of a shared key reports it */
        bool first = true;
        for (int i = 0; i < bit; i++) {
            first &= modifier_to_st[i] != code;
        }
        if (code && first && was != now) {
            st_ikbd_key(code, now);
        }
    }
    for (int i = 2; i < 8; i++) {
        uint8_t u = prev[i];
        if (u > 1 && u < sizeof(usage_to_st) && usage_to_st[u] && !report_has(cur, u)) {
            st_ikbd_key(usage_to_st[u], false);
        }
    }
    for (int i = 2; i < 8; i++) {
        uint8_t u = cur[i];
        if (u > 1 && u < sizeof(usage_to_st) && usage_to_st[u] && !report_has(prev, u)) {
            st_ikbd_key(usage_to_st[u], true);
        }
    }
}

static void release_all_keys(hid_conn_t *c)
{
    static const uint8_t none[8];
    send_keyboard_edges(c->prev_report, none);
    memset(c->prev_report, 0, sizeof(c->prev_report));
}

/* ---- known devices (NVS) ------------------------------------------------ */

static int gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);

static void known_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "known", s_known, sizeof(known_t) * s_known_count);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void known_load(void)
{
    nvs_handle_t h;
    size_t size = sizeof(s_known);
    s_known_count = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "known", s_known, &size) == ESP_OK) {
            s_known_count = size / sizeof(known_t);
        }
        nvs_close(h);
    }
}

static int known_find(const ble_addr_t *addr)
{
    for (int i = 0; i < s_known_count; i++) {
        if (ble_addr_cmp(&s_known[i].addr, addr) == 0) {
            return i;
        }
    }
    return -1;
}

static void known_add(const ble_addr_t *addr, uint8_t kind, const char *name)
{
    portENTER_CRITICAL(&s_lock);
    int i = known_find(addr);
    if (i < 0) {
        if (s_known_count == MAX_KNOWN) {
            memmove(&s_known[0], &s_known[1], sizeof(known_t) * (MAX_KNOWN - 1));
            s_known_count--;
        }
        i = s_known_count++;
    }
    s_known[i].addr = *addr;
    s_known[i].kind = kind;
    snprintf(s_known[i].name, sizeof(s_known[i].name), "%s", name[0] ? name : "HID device");
    portEXIT_CRITICAL(&s_lock);
    known_save();
}

/* ---- connection slots --------------------------------------------------- */

static hid_conn_t *conn_find(uint16_t handle)
{
    for (int i = 0; i < MAX_HID_CONNS; i++) {
        if (s_conns[i].conn_handle == handle) {
            return &s_conns[i];
        }
    }
    return NULL;
}

static hid_conn_t *conn_alloc(uint16_t handle, uint8_t kind)
{
    hid_conn_t *c = conn_find(BLE_HS_CONN_HANDLE_NONE);
    if (c == NULL) {
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->conn_handle = handle;
    c->kind = kind;
    c->battery = -1;
    return c;
}

static void conn_free(hid_conn_t *c)
{
    memset(c, 0, sizeof(*c));
    c->conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static bool have_free_slot(void)
{
    return conn_find(BLE_HS_CONN_HANDLE_NONE) != NULL;
}

static hid_conn_t *conn_of_peer(const ble_addr_t *addr)
{
    for (int i = 0; i < MAX_HID_CONNS; i++) {
        struct ble_gap_conn_desc desc;
        if (s_conns[i].conn_handle != BLE_HS_CONN_HANDLE_NONE &&
            ble_gap_conn_find(s_conns[i].conn_handle, &desc) == 0 &&
            (ble_addr_cmp(&desc.peer_id_addr, addr) == 0 ||
             ble_addr_cmp(&desc.peer_ota_addr, addr) == 0)) {
            return &s_conns[i];
        }
    }
    return NULL;
}

static bool peer_connected(const ble_addr_t *addr)
{
    return conn_of_peer(addr) != NULL;
}

/* Scan while pairing, or while a known device is missing */
static bool need_scan(void)
{
    if (!have_free_slot()) {
        return false;
    }
    if (s_pairing) {
        return true;
    }
    for (int i = 0; i < s_known_count; i++) {
        if (!peer_connected(&s_known[i].addr)) {
            return true;
        }
    }
    return false;
}

/* ---- scanning ----------------------------------------------------------- */

static void start_scan(void)
{
    if (!s_synced || s_connecting || !need_scan() || ble_gap_disc_active()) {
        return;
    }
    struct ble_gap_disc_params params = {
        .filter_duplicates = 0,     /* keep RSSI and names fresh while pairing */
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
    }
}

/* HID kind by appearance or service UUID, -1 if not a HID device */
static int classify_adv(const struct ble_hs_adv_fields *fields)
{
    if (fields->appearance_is_present) {
        if (fields->appearance == APPEARANCE_KEYBOARD) return HID_KIND_KEYBOARD;
        if (fields->appearance == APPEARANCE_MOUSE)    return HID_KIND_MOUSE;
    }
    for (int i = 0; i < fields->num_uuids16; i++) {
        if (ble_uuid_u16(&fields->uuids16[i].u) == HID_SVC_UUID16) {
            return HID_KIND_UNKNOWN;
        }
    }
    return -1;
}

static void candidate_update(const ble_addr_t *addr, int kind, int8_t rssi,
                             const struct ble_hs_adv_fields *fields)
{
    portENTER_CRITICAL(&s_lock);
    int i = 0;
    while (i < s_cand_count && ble_addr_cmp(&s_cand[i].addr, addr) != 0) {
        i++;
    }
    if (i == s_cand_count) {
        if (kind < 0 || s_cand_count == MAX_CANDIDATES) {
            portEXIT_CRITICAL(&s_lock);
            return;     /* scan responses only update devices already listed */
        }
        memset(&s_cand[i], 0, sizeof(s_cand[i]));
        s_cand[i].addr = *addr;
        s_cand_count++;
    }
    if (kind >= 0) {
        s_cand[i].kind = kind;
    }
    s_cand[i].rssi = rssi;
    if (fields->name != NULL && fields->name_len) {
        int n = fields->name_len < 31 ? fields->name_len : 31;
        memcpy(s_cand[i].name, fields->name, n);
        s_cand[i].name[n] = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void connect_to(const ble_addr_t *addr, uint8_t kind, bool pairing, const char *name)
{
    ble_gap_disc_cancel();
    s_connecting = true;
    s_pending_kind = kind;
    s_pending_pair = pairing;
    snprintf(s_pending_name, sizeof(s_pending_name), "%s", name);
    int rc = ble_gap_connect(s_own_addr_type, addr, 30000, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect rc=%d", rc);
        s_connecting = false;
        snprintf(s_status, sizeof(s_status), "Could not connect");
        start_scan();
    }
}

/* ---- GATT discovery + subscribe ----------------------------------------- */

static void subscribe_next(hid_conn_t *c);

/* ---- battery level (optional: after the HID reports are set up) -------- */

static int on_battery_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    hid_conn_t *c = arg;
    uint8_t level;
    if (error->status == 0 && attr != NULL && os_mbuf_copydata(attr->om, 0, 1, &level) == 0) {
        c->battery = level > 100 ? 100 : level;
        ESP_LOGI(TAG, "%s battery %d%%", c->name, c->battery);
    }
    if (c->batt_notify) {
        static const uint8_t on[2] = { 0x01, 0x00 };
        ble_gattc_write_flat(conn_handle, c->batt_handle + 1, on, sizeof(on), NULL, NULL);
    }
    return 0;
}

static int on_battery_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                          const struct ble_gatt_chr *chr, void *arg)
{
    hid_conn_t *c = arg;
    if (error->status == 0 && chr != NULL) {
        c->batt_handle = chr->val_handle;
        c->batt_notify = (chr->properties & BLE_GATT_CHR_PROP_NOTIFY) != 0;
    } else if (error->status == BLE_HS_EDONE && c->batt_handle != 0) {
        ble_gattc_read(conn_handle, c->batt_handle, on_battery_read, c);
    }
    return 0;
}

static int on_battery_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                          const struct ble_gatt_svc *svc, void *arg)
{
    hid_conn_t *c = arg;
    if (error->status == 0 && svc != NULL) {
        c->batt_start = svc->start_handle;
        c->batt_end = svc->end_handle;
    } else if (error->status == BLE_HS_EDONE && c->batt_start != 0) {
        ble_gattc_disc_chrs_by_uuid(conn_handle, c->batt_start, c->batt_end,
                                    BLE_UUID16_DECLARE(CHR_BATTERY_LEVEL), on_battery_chr, c);
    }
    return 0;
}

static int on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    hid_conn_t *c = arg;
    if (error->status != 0) {
        ESP_LOGW(TAG, "CCCD write %d failed: status=%d", c->sub_idx, error->status);
    }
    c->sub_idx++;
    subscribe_next(c);
    return 0;
}

static void subscribe_next(hid_conn_t *c)
{
    if (c->sub_idx >= c->notif_count) {
        ESP_LOGI(TAG, "%s ready (%d report characteristic(s))", c->name, c->notif_count);
        ble_gattc_disc_svc_by_uuid(c->conn_handle, BLE_UUID16_DECLARE(BATTERY_SVC_UUID16),
                                   on_battery_svc, c);
        return;
    }
    uint8_t value[2] = { 0x01, 0x00 };
    uint16_t cccd_handle = c->notif_handles[c->sub_idx] + 1;
    int rc = ble_gattc_write_flat(c->conn_handle, cccd_handle, value, sizeof(value),
                                  on_subscribe, c);
    if (rc != 0) {
        ESP_LOGW(TAG, "subscribe write rc=%d for handle %u", rc, cccd_handle);
        c->sub_idx++;
        subscribe_next(c);
    }
}

/* Boot Protocol gives fixed 8-byte keyboard and 3-4 byte mouse reports */
static void request_boot_protocol(hid_conn_t *c)
{
    if (c->protomode_handle != 0) {
        uint8_t boot = 0x00;
        int rc = ble_gattc_write_no_rsp_flat(c->conn_handle, c->protomode_handle, &boot, 1);
        if (rc != 0) {
            ESP_LOGW(TAG, "protocol mode write rc=%d", rc);
        }
    }
    c->sub_idx = 0;
    subscribe_next(c);
}

static int on_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    hid_conn_t *c = arg;
    if (error->status == 0 && chr != NULL) {
        if (chr->uuid.u.type == BLE_UUID_TYPE_16 &&
            ble_uuid_u16(&chr->uuid.u) == CHR_PROTOCOL_MODE) {
            c->protomode_handle = chr->val_handle;
        }
        if ((chr->properties & BLE_GATT_CHR_PROP_NOTIFY) && c->notif_count < MAX_NOTIF_CHRS) {
            c->notif_handles[c->notif_count++] = chr->val_handle;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (c->notif_count == 0) {
            ESP_LOGW(TAG, "no notifiable HID report characteristics");
            return 0;
        }
        request_boot_protocol(c);
    } else {
        ESP_LOGW(TAG, "characteristic discovery failed: status=%d", error->status);
    }
    return 0;
}

static int on_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *svc, void *arg)
{
    hid_conn_t *c = arg;
    if (error->status == 0 && svc != NULL) {
        c->svc_start = svc->start_handle;
        c->svc_end = svc->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE && c->svc_start != 0) {
        int rc = ble_gattc_disc_all_chrs(conn_handle, c->svc_start, c->svc_end, on_chr, c);
        if (rc != 0) {
            ESP_LOGW(TAG, "disc_all_chrs rc=%d", rc);
        }
    } else {
        ESP_LOGW(TAG, "HID service not found (status=%d)", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

/* ---- reports ------------------------------------------------------------ */

/* The kind of a device that didn't say becomes known from its reports */
static void note_kind(hid_conn_t *c, uint8_t kind)
{
    if (c->kind == kind) {
        return;
    }
    c->kind = kind;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(c->conn_handle, &desc) == 0 && known_find(&desc.peer_id_addr) >= 0) {
        known_add(&desc.peer_id_addr, kind, c->name);
    }
}

/* Boot keyboard report: [modifiers][reserved][6 key usages] */
static void decode_keyboard(hid_conn_t *c, const uint8_t *report)
{
    note_kind(c, HID_KIND_KEYBOARD);
    if (report[2] == 0x01) {
        return;     /* phantom state: too many keys down */
    }
    send_keyboard_edges(c->prev_report, report);
    memcpy(c->prev_report, report, sizeof(c->prev_report));
}

/* Boot mouse report: [buttons][dx][dy][wheel?] */
static void decode_mouse(hid_conn_t *c, const uint8_t *report, int len)
{
    note_kind(c, HID_KIND_MOUSE);
    int dx = (int8_t)report[1] + c->mouse_rem_x;
    int dy = (int8_t)report[2] + c->mouse_rem_y;
    c->mouse_rem_x = dx % CONFIG_ST_MOUSE_DIVIDER;
    c->mouse_rem_y = dy % CONFIG_ST_MOUSE_DIVIDER;
    st_ikbd_mouse(dx / CONFIG_ST_MOUSE_DIVIDER, dy / CONFIG_ST_MOUSE_DIVIDER,
                  report[0] & 0x01, report[0] & 0x02);
}

static void decode_report(hid_conn_t *c, const uint8_t *report, int len)
{
    if (len == 8) {
        decode_keyboard(c, report);
    } else if (len == 3 || len == 4) {
        decode_mouse(c, report, len);
    }
}

/* ---- GAP events --------------------------------------------------------- */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &event->disc;
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) != 0) {
            return 0;
        }
        int kind = classify_adv(&fields);
        bool directed = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
        int known = known_find(&d->addr);

        /* A paired device asking to reconnect (directed advertising is
         * only aimed at a bonded central), or advertising again
         */
        if ((directed || known >= 0) && !peer_connected(&d->addr) && have_free_slot() &&
            !s_connecting) {
            ESP_LOGI(TAG, "reconnecting %s", known >= 0 ? s_known[known].name : "bonded device");
            connect_to(&d->addr, known >= 0 ? s_known[known].kind : HID_KIND_UNKNOWN, false,
                       known >= 0 ? s_known[known].name : "");
            return 0;
        }
        if (s_pairing && known < 0) {
            candidate_update(&d->addr, d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP ? -1 : kind,
                             d->rssi, &fields);
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status == 0) {
            hid_conn_t *c = conn_alloc(event->connect.conn_handle, s_pending_kind);
            if (c == NULL) {
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
            c->pairing = s_pending_pair;
            snprintf(c->name, sizeof(c->name), "%s", s_pending_name[0] ? s_pending_name : "HID device");
            ESP_LOGI(TAG, "connected to %s, securing link", c->name);
            if (c->pairing) {
                snprintf(s_status, sizeof(s_status), "Pairing with %s ...", c->name);
            }
            if (ble_gap_security_initiate(c->conn_handle) != 0) {
                ble_gattc_disc_svc_by_uuid(c->conn_handle, BLE_UUID16_DECLARE(HID_SVC_UUID16),
                                           on_svc, c);
            }
        } else {
            ESP_LOGW(TAG, "connect failed: status=%d", event->connect.status);
            if (s_pending_pair) {
                snprintf(s_status, sizeof(s_status), "Could not connect - is it in pairing mode?");
            }
        }
        start_scan();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        hid_conn_t *c = conn_find(event->enc_change.conn_handle);
        if (c == NULL) {
            return 0;
        }
        struct ble_gap_conn_desc desc;
        if (event->enc_change.status == 0 && c->pairing &&
            ble_gap_conn_find(c->conn_handle, &desc) == 0) {
            known_add(&desc.peer_id_addr, c->kind, c->name);
            snprintf(s_status, sizeof(s_status), "Paired with %s", c->name);
            c->pairing = false;
        } else if (event->enc_change.status != 0 && c->pairing) {
            snprintf(s_status, sizeof(s_status), "Pairing with %s failed", c->name);
        }
        ble_gattc_disc_svc_by_uuid(c->conn_handle, BLE_UUID16_DECLARE(HID_SVC_UUID16), on_svc, c);
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The device lost its bond with us: forget ours and pair again */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        hid_conn_t *c = conn_find(event->notify_rx.conn_handle);
        if (c == NULL) {
            return 0;
        }
        uint8_t buf[16];
        int len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (c->batt_handle != 0 && event->notify_rx.attr_handle == c->batt_handle) {
            uint8_t level;
            if (os_mbuf_copydata(event->notify_rx.om, 0, 1, &level) == 0) {
                c->battery = level > 100 ? 100 : level;
            }
            return 0;
        }
        if (len > (int)sizeof(buf)) {
            len = sizeof(buf);
        }
        if (os_mbuf_copydata(event->notify_rx.om, 0, len, buf) == 0) {
            decode_report(c, buf, len);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        hid_conn_t *c = conn_find(event->disconnect.conn.conn_handle);
        ESP_LOGI(TAG, "%s disconnected (reason %d)", c ? c->name : "device",
                 event->disconnect.reason);
        if (c != NULL) {
            if (c->kind == HID_KIND_KEYBOARD) {
                release_all_keys(c);
            } else if (c->kind == HID_KIND_MOUSE) {
                st_ikbd_mouse(0, 0, false, false);
            }
            conn_free(c);
        }
        start_scan();
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        start_scan();
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no BLE address");
        return;
    }
    s_synced = true;
    start_scan();
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "BLE host reset: reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* NimBLE's NVS bond store */
extern void ble_store_config_init(void);

bool ble_hid_start(void)
{
    for (int i = 0; i < MAX_HID_CONNS; i++) {
        s_conns[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    known_load();

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE HID host started, %d paired device(s)", s_known_count);
    return true;
}

/* ---- API for the setup screen ---------------------------------------------- */

int ble_hid_devices(ble_hid_device_t *out, int max)
{
    known_t copy[MAX_KNOWN];
    portENTER_CRITICAL(&s_lock);
    int n = s_known_count < max ? s_known_count : max;
    memcpy(copy, s_known, sizeof(known_t) * n);
    portEXIT_CRITICAL(&s_lock);
    for (int i = 0; i < n; i++) {
        snprintf(out[i].name, sizeof(out[i].name), "%s", copy[i].name);
        out[i].kind = copy[i].kind;
        hid_conn_t *c = conn_of_peer(&copy[i].addr);
        out[i].connected = c != NULL;
        out[i].battery = c != NULL ? c->battery : -1;
    }
    return n;
}

void ble_hid_set_pairing(bool on)
{
    portENTER_CRITICAL(&s_lock);
    s_pairing = on;
    s_cand_count = 0;
    portEXIT_CRITICAL(&s_lock);
    s_status[0] = 0;
    if (on) {
        start_scan();
    }
}

int ble_hid_candidates(ble_hid_device_t *out, int max)
{
    portENTER_CRITICAL(&s_lock);
    int n = s_cand_count < max ? s_cand_count : max;
    for (int i = 0; i < n; i++) {
        snprintf(out[i].name, sizeof(out[i].name), "%s",
                 s_cand[i].name[0] ? s_cand[i].name : "(no name)");
        out[i].kind = s_cand[i].kind;
        out[i].rssi = s_cand[i].rssi;
        out[i].connected = false;
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

void ble_hid_pair(int index)
{
    portENTER_CRITICAL(&s_lock);
    if (index < 0 || index >= s_cand_count) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    candidate_t c = s_cand[index];
    portEXIT_CRITICAL(&s_lock);
    if (!have_free_slot()) {
        snprintf(s_status, sizeof(s_status), "Two devices already connected");
        return;
    }
    connect_to(&c.addr, c.kind, true, c.name[0] ? c.name : "HID device");
}

void ble_hid_forget(int index)
{
    portENTER_CRITICAL(&s_lock);
    if (index < 0 || index >= s_known_count) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    known_t k = s_known[index];
    memmove(&s_known[index], &s_known[index + 1], sizeof(known_t) * (s_known_count - index - 1));
    s_known_count--;
    portEXIT_CRITICAL(&s_lock);
    known_save();

    for (int i = 0; i < MAX_HID_CONNS; i++) {
        struct ble_gap_conn_desc desc;
        if (s_conns[i].conn_handle != BLE_HS_CONN_HANDLE_NONE &&
            ble_gap_conn_find(s_conns[i].conn_handle, &desc) == 0 &&
            ble_addr_cmp(&desc.peer_id_addr, &k.addr) == 0) {
            ble_gap_terminate(s_conns[i].conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    ble_store_util_delete_peer(&k.addr);
    snprintf(s_status, sizeof(s_status), "Forgot %s", k.name);
}

const char *ble_hid_status(void)
{
    return s_status;
}
