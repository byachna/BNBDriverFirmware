#pragma once
#include <stdint.h>
#include "wardrive_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WD_BLE_CMD_START_SCAN = 0x01,
    WD_BLE_CMD_STOP_SCAN   = 0x02,
} wd_ble_cmd_t;

typedef void (*wd_ble_control_cb_t)(wd_ble_cmd_t cmd);
typedef uint32_t (*wd_ble_count_fn_t)(void);

/* Registers callbacks the GATT server uses to react to phone commands and
 * to report status; call before wd_ble_init(). */
void wd_ble_set_control_cb(wd_ble_control_cb_t cb);
void wd_ble_set_status_providers(wd_ble_count_fn_t ap_count_fn, wd_ble_count_fn_t node_count_fn);

/* Brings up NimBLE, starts the GATT server + advertising. Blocking calls
 * happen on the dedicated NimBLE host task, so this returns quickly. */
void wd_ble_init(void);

/* Streams one AP record to the phone if it is currently subscribed. */
void wd_ble_notify_ap(const wd_ap_record_t *rec);

#ifdef __cplusplus
}
#endif
