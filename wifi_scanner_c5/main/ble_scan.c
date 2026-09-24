#include <string.h>
#include <stdbool.h>
#include "ble_scan.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"

static const char *TAG = "wd_ble_scan";
static wd_ble_scan_record_cb_t s_record_cb;
static uint8_t s_own_addr_type;
static bool s_host_ready;
static bool s_scan_requested;
static bool s_scanning;

static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* Starts discovery after the NimBLE host has selected an address type. */
static void start_discovery(void)
{
    if (!s_host_ready || !s_scan_requested || s_scanning) {
        return;
    }

    struct ble_gap_disc_params params = {0};
    params.passive = 1;
    params.filter_duplicates = 1;

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event_handler, NULL);
    if (rc == 0) {
        s_scanning = true;
    } else {
        ESP_LOGW(TAG, "ble_gap_disc failed rc=%d", rc);
    }
}

/* Converts one BLE advertisement report into the shared discovery record. */
static void report_discovery(const struct ble_gap_disc_desc *desc)
{
    if (!s_record_cb) {
        return;
    }

    wd_ap_record_t record = {0};
    memcpy(record.bssid, desc->addr.val, WD_MAC_LEN);
    record.rssi = desc->rssi;
    record.band = WD_BAND_2G4;
    record.authmode = desc->addr.type;
    record.record_type = WD_RECORD_TYPE_BLE;

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) == 0 && fields.name) {
        size_t name_len = fields.name_len < WD_MAX_SSID_LEN ? fields.name_len : WD_MAX_SSID_LEN;
        memcpy(record.ssid, fields.name, name_len);
    }

    s_record_cb(&record);
}

/* Handles BLE advertisements and restarts discovery if the controller ends it. */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        report_discovery(&event->disc);
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        start_discovery();
        break;

    default:
        break;
    }
    return 0;
}

/* Resolves the local address type after NimBLE initialization completes. */
static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer address type rc=%d", rc);
        return;
    }
    s_host_ready = true;
    start_discovery();
}

/* Runs the NimBLE host loop in its own FreeRTOS task. */
static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Initializes the C5 as a BLE observer but does not begin discovery yet. */
esp_err_t wd_ble_scan_init(wd_ble_scan_record_cb_t record_cb)
{
    s_record_cb = record_cb;
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

/* Requests continuous BLE scanning, starting immediately when the host is ready. */
void wd_ble_scan_start(void)
{
    s_scan_requested = true;
    start_discovery();
}

/* Cancels the active BLE scan and prevents automatic restart. */
void wd_ble_scan_stop(void)
{
    s_scan_requested = false;
    if (s_scanning) {
        int rc = ble_gap_disc_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "ble_gap_disc_cancel failed rc=%d", rc);
        }
    }
}