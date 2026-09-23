#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wardrive_proto.h"
#include "aggregator.h"
#include "node_manager.h"
#include "uart_master.h"
#include "espnow_master.h"
#include "ble_gatt.h"

static const char *TAG = "wd_master";

/* Forwards newly observed or refreshed AP records to the BLE notification path. */
static void on_agg_update(const wd_ap_record_t *rec, bool is_new)
{
    (void)is_new;
    wd_ble_notify_ap(rec);
}

/* Handles messages from the locally wired scanner and tracks its liveness. */
static void on_uart_msg(uint8_t msg_type, const uint8_t *payload, uint16_t len)
{
    /* Registers the local UART node as connected on first contact, and
     * folds it into the channel split (same as ESP-NOW nodes below). */
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
    /* Registers the node on first contact if needed, and always refreshes
     * its last-seen timestamp for status logging. */
    wd_node_manager_touch(node_mac);

    switch (msg_type) {
    case WD_MSG_HELLO_RESP:
        if (wd_node_manager_register(node_mac)) {
            wd_node_manager_redistribute();
        }
        break;

    case WD_MSG_AP_RECORD:
        if (len >= sizeof(wd_ap_record_t)) {
            /* Node was already known if it's sending scan data; register it
             * defensively in case its HELLO_RESP was lost. */
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

/* Supplies the current unique AP count to the BLE status characteristic. */
static uint32_t ap_count_provider(void)
{
    return wd_aggregator_count();
}

/* Supplies the current connected scanner count to the BLE status characteristic. */
static uint32_t node_count_provider(void)
{
    return wd_node_manager_count();
}

/* Periodically discovers scanners and the wired node by sending HELLO requests. */
static void discovery_task(void *arg)
{
    /* Periodically send a HELLO so any powered-on wireless ESP32-C5 scanner
     * node (over ESP-NOW) or the locally-wired one (over UART) can
     * announce itself and get a channel assignment; nodes no longer start
     * scanning on their own until they receive one. */
    while (1) {
        wd_espnow_master_broadcast(WD_MSG_HELLO_REQ, NULL, 0);
        wd_uart_master_send(WD_MSG_HELLO_REQ, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* Prunes stale nodes, refreshes assignments, and logs system status periodically. */
static void status_log_task(void *arg)
{
    /* Serial log: liveness of every connected node plus a running count
     * of unique APs discovered so far. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        wd_node_manager_prune_stale();
        /* Periodically re-push the current channel split (idempotent) so a
         * node whose initial ASSIGN_CHANNELS was missed over ESP-NOW (e.g.
         * it was mid-scan and off the control channel when a new peer
         * connected) still converges to the correct assignment. */
        wd_node_manager_redistribute();
        ESP_LOGI(TAG, "--- status (%u node(s) known) ---", (unsigned)wd_node_manager_count());
        wd_node_manager_log_status();
        ESP_LOGI(TAG, "APs scanned so far: %u", (unsigned)wd_aggregator_count());
    }
}

/* Initializes the station interface used by the master's ESP-NOW transport. */
static void wifi_init_for_espnow(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* Initializes storage, transports, aggregation, BLE, and the master tasks. */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_for_espnow();

    wd_aggregator_init();
    wd_aggregator_set_update_cb(on_agg_update);

    wd_node_manager_init();

    ESP_ERROR_CHECK(wd_uart_master_init(on_uart_msg));
    ESP_ERROR_CHECK(wd_espnow_master_init(on_espnow_msg));

    wd_ble_set_control_cb(on_ble_control);
    wd_ble_set_status_providers(ap_count_provider, node_count_provider);
    wd_ble_init();

    /* Nodes scan all channels by default until connected; the channel list
     * gets equally re-split across the local UART node and every wireless
     * node as each one is heard from (see on_uart_msg/on_espnow_msg). */
    wd_node_manager_redistribute();

    xTaskCreate(discovery_task, "wd_discovery", 3072, NULL, 4, NULL);
    xTaskCreate(status_log_task, "wd_status_log", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "Wardrive master node ready");
}
