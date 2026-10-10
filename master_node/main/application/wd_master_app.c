#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wardrive_proto.h"
#include "aggregator.h"
#include "node_manager.h"
#include "uart_master.h"
#include "espnow_master.h"
#include "ble_gatt.h"
#include "wd_master_app.h"

static const char *TAG = "wd_master";

static int64_t node_manager_now_us(void)
{
    return esp_timer_get_time();
}

static void node_manager_uart_send(uint8_t msg_type, const void *payload, uint16_t len)
{
    (void)wd_uart_master_send(msg_type, payload, len);
}

static void node_manager_espnow_send(const uint8_t mac[6], uint8_t msg_type,
                                     const void *payload, uint16_t len)
{
    (void)wd_espnow_master_send(mac, msg_type, payload, len);
}

static void node_manager_espnow_broadcast(uint8_t msg_type, const void *payload, uint16_t len)
{
    (void)wd_espnow_master_broadcast(msg_type, payload, len);
}

static const wd_node_manager_ports_t s_node_manager_ports = {
    .now_us = node_manager_now_us,
    .uart_send = node_manager_uart_send,
    .espnow_send = node_manager_espnow_send,
    .espnow_broadcast = node_manager_espnow_broadcast,
};

/* Forwards newly observed or refreshed AP records to the BLE notification path. */
static void on_agg_update(const wd_ap_record_t *rec, bool is_new)
{
    (void)is_new;
    wd_ble_notify_ap(rec);
}

/* Handles messages from the locally wired scanner and tracks its liveness. */
static void on_uart_msg(uint8_t msg_type, const uint8_t *payload, uint16_t len)
{
    if (wd_node_manager_touch_local()) {
        wd_node_manager_redistribute();
    }

    switch (msg_type) {
    case WD_MSG_HELLO_RESP:
        ESP_LOGD(TAG, "local (UART) node replied to HELLO");
        break;
    case WD_MSG_AP_RECORD:
        if (len >= sizeof(wd_ap_record_t)) {
            wd_aggregator_add((const wd_ap_record_t *)payload);
        }
        break;
    case WD_MSG_SCAN_CYCLE_DONE:
        ESP_LOGD(TAG, "local (UART) node finished a scan sweep");
        break;
    default:
        ESP_LOGW(TAG, "unhandled UART message 0x%02x", msg_type);
        break;
    }
}

/* Handles messages from wireless scanners and updates their liveness and data. */
static void on_espnow_msg(const uint8_t node_mac[6], uint8_t msg_type, const uint8_t *payload, uint16_t len)
{
    wd_node_manager_touch(node_mac);

    switch (msg_type) {
    case WD_MSG_HELLO_RESP:
        if (wd_node_manager_register(node_mac)) {
            wd_node_manager_redistribute();
        }
        break;

    case WD_MSG_AP_RECORD:
        if (len >= sizeof(wd_ap_record_t)) {
            if (wd_node_manager_register(node_mac)) {
                wd_node_manager_redistribute();
            }
            wd_aggregator_add((const wd_ap_record_t *)payload);
        }
        break;

    case WD_MSG_SCAN_CYCLE_DONE:
        ESP_LOGD(TAG, "wireless node finished a scan sweep");
        break;

    default:
        ESP_LOGW(TAG, "unhandled ESP-NOW message 0x%02x", msg_type);
        break;
    }
}

/* Translates a BLE start/stop command into a broadcast to all scanners. */
static void on_ble_control(wd_ble_cmd_t cmd)
{
    switch (cmd) {
    case WD_BLE_CMD_START_SCAN:
        ESP_LOGI(TAG, "phone requested start scan");
        wd_node_manager_start_all();
        break;
    case WD_BLE_CMD_STOP_SCAN:
        ESP_LOGI(TAG, "phone requested stop scan");
        wd_node_manager_stop_all();
        break;
    }
}

static uint32_t ap_count_provider(void)
{
    return wd_aggregator_count();
}

static uint32_t node_count_provider(void)
{
    return wd_node_manager_count();
}

/* Periodically discovers scanners and the wired node by sending HELLO requests. */
static void discovery_task(void *arg)
{
    (void)arg;
    while (1) {
        wd_espnow_master_broadcast(WD_MSG_HELLO_REQ, NULL, 0);
        wd_uart_master_send(WD_MSG_HELLO_REQ, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* Prunes stale nodes, refreshes assignments, and logs system status periodically. */
static void status_log_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        wd_node_manager_prune_stale();
        wd_node_manager_redistribute();
        ESP_LOGI(TAG, "--- status (%u node(s) known) ---", (unsigned)wd_node_manager_count());
        wd_node_manager_log_status();
        ESP_LOGI(TAG, "APs scanned so far: %u", (unsigned)wd_aggregator_count());
    }
}

/* Composes the master use cases from the transport and state services. */
void wd_master_app_start(void)
{
    wd_aggregator_init();
    wd_aggregator_set_update_cb(on_agg_update);
    wd_node_manager_init(&s_node_manager_ports);

    ESP_ERROR_CHECK(wd_uart_master_init(on_uart_msg));
    ESP_ERROR_CHECK(wd_espnow_master_init(on_espnow_msg));

    wd_ble_set_control_cb(on_ble_control);
    wd_ble_set_status_providers(ap_count_provider, node_count_provider);
    wd_ble_init();

    wd_node_manager_redistribute();

    xTaskCreate(discovery_task, "wd_discovery", 3072, NULL, 4, NULL);
    xTaskCreate(status_log_task, "wd_status_log", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "Wardrive master node ready");
}