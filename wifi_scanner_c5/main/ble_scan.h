#pragma once

#include "esp_err.h"
#include "wardrive_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wd_ble_scan_record_cb_t)(const wd_ap_record_t *record);

/* Initializes the NimBLE observer and registers the destination for BLE records. */
esp_err_t wd_ble_scan_init(wd_ble_scan_record_cb_t record_cb);

/* Starts or stops continuous BLE advertisement discovery. */
void wd_ble_scan_start(void);
void wd_ble_scan_stop(void);

#ifdef __cplusplus
}
#endif