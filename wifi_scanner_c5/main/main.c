#include <string.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "wardrive_proto.h"
#include "transport.h"
#include "wifi_scan.h"
#include "ble_scan.h"

static const char *TAG = "wd_scanner_c5";

#define WD_CONN_STATUS_LOG_INTERVAL_US (5 * 1000 * 1000)
#define WD_ESPNOW_FLUSH_INTERVAL_MS    500
#define WD_AP_QUEUE_LEN                64

/* s_assignment is written from the transport recv context (ESP-NOW/UART
 * rx) whenever the master (re)sends WD_MSG_ASSIGN_CHANNELS, and read from
 * scan_task; s_assignment_lock keeps that read/write pair from tearing,
 * which otherwise looked like nodes scanning an overlapping/garbled mix
 * of old and new channel sets. */
static wd_channel_assignment_t s_assignment;
static bool s_have_assignment = false;
static SemaphoreHandle_t s_assignment_lock;
static volatile bool s_scanning = false;
static TaskHandle_t s_scan_task_handle = NULL;

/* AP records found while scanning are pushed onto this queue instead of
 * being sent immediately; sending while the radio is still tuned to the
 * scanned channel makes esp_now_send() fail with "Peer channel is not
 * equal to the home channel". espnow_flush_task drains the queue after
 * switching to the ESP-NOW control channel. */
static QueueHandle_t s_ap_queue;

/* Serializes every operation that changes the Wi-Fi channel (scanning vs.
 * the flush task's resync) so they can never race on the single radio. */
static SemaphoreHandle_t s_radio_lock;

/* Queues one Wi-Fi or BLE discovery record for transport to the master. */
static void enqueue_discovery_record(const wd_ap_record_t *record)
{
    wd_ap_record_t queued_record = *record;
#if CONFIG_WD_TRANSPORT_ESPNOW
    esp_wifi_get_mac(WIFI_IF_STA, queued_record.node_id);
#endif
    if (xQueueSend(s_ap_queue, &queued_record, 0) != pdTRUE) {
        ESP_LOGW(TAG, "AP queue full, dropping record");
    }
}

/* Converts a Wi-Fi scan result to the shared wire format and queues it. */
static void enqueue_wifi_ap_record(const wifi_ap_record_t *rec)
{
    wd_ap_record_t out = {0};
    memcpy(out.bssid, rec->bssid, WD_MAC_LEN);
    memcpy(out.ssid, rec->ssid, WD_MAX_SSID_LEN);
    out.rssi = rec->rssi;
    out.channel = rec->primary;
    out.band = (rec->primary > 14) ? WD_BAND_5G : WD_BAND_2G4;
    out.authmode = (uint8_t)rec->authmode;
    out.record_type = WD_RECORD_TYPE_WIFI;
    enqueue_discovery_record(&out);
}

/* Periodically returns to the control channel and flushes queued AP records. */
static void espnow_flush_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WD_ESPNOW_FLUSH_INTERVAL_MS));

        xSemaphoreTake(s_radio_lock, portMAX_DELAY);
        wd_transport_resync();
        wd_ap_record_t rec;
        while (xQueueReceive(s_ap_queue, &rec, 0) == pdTRUE) {
            wd_transport_send(WD_MSG_AP_RECORD, &rec, sizeof(rec));
        }
        xSemaphoreGive(s_radio_lock);
    }
}

/* Scans the assigned channels until scanning is stopped, then reports completion. */
static void scan_task(void *arg)
{
    /* Only ever started once a channel assignment has been received (see
     * start_scanning()), so there's always a non-empty assignment here. */
    while (s_scanning) {
        /* Snapshot the assignment once per cycle instead of reading the
         * shared struct field-by-field, so a mid-cycle re-assignment from
         * the master can't be observed half-old/half-new. */
        wd_channel_assignment_t local_assign;
        xSemaphoreTake(s_assignment_lock, portMAX_DELAY);
        local_assign = s_assignment;
        xSemaphoreGive(s_assignment_lock);

        ESP_LOGI(TAG, "Scan cycle starting (master %sconnected)",
                 wd_transport_is_connected() ? "" : "NOT ");
        for (int i = 0; i < local_assign.num_channels && s_scanning; i++) {
            ESP_LOGI(TAG, "Scanning channel %u (dwell=%ums)",
                     local_assign.channels[i], (unsigned)local_assign.dwell_ms);
            xSemaphoreTake(s_radio_lock, portMAX_DELAY);
            wd_wifi_scan_channel(local_assign.channels[i], local_assign.dwell_ms, enqueue_wifi_ap_record);
            xSemaphoreGive(s_radio_lock);
        }
        ESP_LOGI(TAG, "Scan cycle done (master %sconnected)",
                 wd_transport_is_connected() ? "" : "NOT ");
        /* Resync to the control channel under s_radio_lock before this
         * direct send too - it doesn't go through the AP-record queue, so
         * without this it would fire while still tuned to the last
         * scanned channel and fail the same way esp_now_send() does for
         * AP records. */
        xSemaphoreTake(s_radio_lock, portMAX_DELAY);
        wd_transport_resync();
        wd_transport_send(WD_MSG_SCAN_CYCLE_DONE, NULL, 0);
        xSemaphoreGive(s_radio_lock);
    }

    s_scan_task_handle = NULL;
    vTaskDelete(NULL);
}

/* Logs the current assignment, or that the node is configured for a full scan. */
static void log_assigned_channels(void)
{
    xSemaphoreTake(s_assignment_lock, portMAX_DELAY);
    wd_channel_assignment_t local_assign = s_assignment;
    bool have_assignment = s_have_assignment;
    xSemaphoreGive(s_assignment_lock);

    if (!have_assignment || local_assign.num_channels == 0) {
        ESP_LOGI(TAG, "Channel assignment: all channels (full scan)");
        return;
    }

    char buf[3 * WD_MAX_CHANNELS + 1] = {0};
    size_t off = 0;
    for (int i = 0; i < local_assign.num_channels; i++) {
        int written = snprintf(buf + off, sizeof(buf) - off, "%s%u",
                                (i == 0) ? "" : ",", local_assign.channels[i]);
        if (written < 0 || (size_t)written >= sizeof(buf) - off) {
            break;
        }
        off += (size_t)written;
    }
    ESP_LOGI(TAG, "Channel assignment: [%s]", buf);
}

/* Starts one scan task after verifying that the master supplied channels. */
static void start_scanning(void)
{
    if (s_scanning) {
        return;
    }
    xSemaphoreTake(s_assignment_lock, portMAX_DELAY);
    bool ready = s_have_assignment && s_assignment.num_channels > 0;
    xSemaphoreGive(s_assignment_lock);
    if (!ready) {
        ESP_LOGW(TAG, "Ignoring start-scan request: no channel assignment received yet");
        return;
    }
    s_scanning = true;
    xTaskCreate(scan_task, "wd_scan", 4096, NULL, 5, &s_scan_task_handle);
}

/* Signals the scan task to finish at its next loop boundary. */
static void stop_scanning(void)
{
    s_scanning = false;
}

/* Logs master connectivity and the current scan scope at a fixed interval. */
static void connection_status_timer_cb(void *arg)
{
    xSemaphoreTake(s_assignment_lock, portMAX_DELAY);
    bool full_scan = !s_have_assignment || s_assignment.num_channels == 0;
    uint8_t num_channels = s_assignment.num_channels;
    xSemaphoreGive(s_assignment_lock);

    if (full_scan) {
        ESP_LOGI(TAG, "Master connection status: %sconnected, scanning all channels",
                 wd_transport_is_connected() ? "" : "NOT ");
    } else {
        ESP_LOGI(TAG, "Master connection status: %sconnected, scanning %u channel(s)",
                 wd_transport_is_connected() ? "" : "NOT ", num_channels);
    }
}

/* Processes master commands and updates the assignment or scan state. */
static void on_transport_msg(uint8_t msg_type, const uint8_t *payload, uint16_t len)
{
    ESP_LOGI(TAG, "Received msg 0x%02x (%u bytes), master %sconnected",
             msg_type, len, wd_transport_is_connected() ? "" : "NOT ");

    switch (msg_type) {
    case WD_MSG_HELLO_REQ: {
        wd_hello_resp_t resp = {0};
        resp.proto_version = WD_PROTO_VERSION;
        resp.supports_5g = 1;
#if CONFIG_WD_TRANSPORT_ESPNOW
        esp_wifi_get_mac(WIFI_IF_STA, resp.mac);
#endif
        wd_transport_send(WD_MSG_HELLO_RESP, &resp, sizeof(resp));
        break;
    }

    case WD_MSG_ASSIGN_CHANNELS:
        if (len >= sizeof(wd_channel_assignment_t)) {
            xSemaphoreTake(s_assignment_lock, portMAX_DELAY);
            memcpy(&s_assignment, payload, sizeof(wd_channel_assignment_t));
            s_have_assignment = true;
            xSemaphoreGive(s_assignment_lock);
            ESP_LOGI(TAG, "Assigned %u channel(s), band=%u, dwell=%ums",
                     ((const wd_channel_assignment_t *)payload)->num_channels,
                     ((const wd_channel_assignment_t *)payload)->band,
                     (unsigned)((const wd_channel_assignment_t *)payload)->dwell_ms);
            log_assigned_channels();
        }
        break;

    case WD_MSG_START_SCAN:
        ESP_LOGI(TAG, "Start scan requested");
        start_scanning();
        wd_ble_scan_start();
        break;

    case WD_MSG_STOP_SCAN:
        ESP_LOGI(TAG, "Stop scan requested");
        stop_scanning();
        wd_ble_scan_stop();
        break;

    default:
        ESP_LOGW(TAG, "Unhandled message type 0x%02x", msg_type);
        break;
    }
}

/* Creates scanner synchronization objects, initializes Wi-Fi and transport, and starts tasks. */
void app_main(void)
{
    s_assignment_lock = xSemaphoreCreateMutex();
    s_radio_lock = xSemaphoreCreateMutex();
    s_ap_queue = xQueueCreate(WD_AP_QUEUE_LEN, sizeof(wd_ap_record_t));

    ESP_ERROR_CHECK(wd_wifi_scan_init());
    ESP_ERROR_CHECK(wd_transport_init(on_transport_msg));
    ESP_ERROR_CHECK(wd_ble_scan_init(enqueue_discovery_record));

    ESP_LOGI(TAG, "ESP32-C5 Wi-Fi scanner ready (%s transport)",
#if CONFIG_WD_TRANSPORT_UART
             "UART"
#else
             "ESP-NOW"
#endif
    );

    const esp_timer_create_args_t conn_status_timer_args = {
        .callback = &connection_status_timer_cb,
        .name = "wd_conn_status",
    };
    esp_timer_handle_t conn_status_timer;
    ESP_ERROR_CHECK(esp_timer_create(&conn_status_timer_args, &conn_status_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(conn_status_timer, WD_CONN_STATUS_LOG_INTERVAL_US));

    xTaskCreate(espnow_flush_task, "wd_flush", 3072, NULL, 5, NULL);

    /* Wait for the master to send a channel assignment (and then a
     * WD_MSG_START_SCAN) before scanning; see start_scanning(). */
    log_assigned_channels();
}
