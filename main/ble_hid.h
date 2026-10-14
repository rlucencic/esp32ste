#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * BLE HID host (NimBLE central): a keyboard and a mouse at the same time,
 * fed to the ST's IKBD. Paired devices reconnect by themselves; new ones are
 * paired from the setup screen.
 *
 * Needs nvs_flash_init() first (pairings are stored in NVS).
 */
bool ble_hid_start(void);

enum { HID_KIND_UNKNOWN = 0, HID_KIND_KEYBOARD, HID_KIND_MOUSE };

typedef struct {
    char name[32];
    uint8_t kind;           /* HID_KIND_* */
    int8_t rssi;            /* candidates only */
    bool connected;         /* paired devices only */
    int8_t battery;         /* connected paired devices: %, -1 = unknown */
} ble_hid_device_t;

/* Paired devices */
int ble_hid_devices(ble_hid_device_t *out, int max);

/* Pairing mode: list HID devices that are in pairing mode nearby */
void ble_hid_set_pairing(bool on);
int ble_hid_candidates(ble_hid_device_t *out, int max);
void ble_hid_pair(int index);

/* Remove a paired device (disconnects it and deletes the bond) */
void ble_hid_forget(int index);

/* Last pairing/forgetting outcome for the screen, "" if none */
const char *ble_hid_status(void);
