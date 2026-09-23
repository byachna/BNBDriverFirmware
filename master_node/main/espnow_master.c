#include <string.h>
#include "espnow_master.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"

static const char *TAG = "wd_espnow_master";
static wd_espnow_recv_cb_t s_recv_cb;
static const uint8_t s_broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* Adds an ESP-NOW peer for the configured control channel unless it exists. */
static esp_err_t add_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = CONFIG_WD_MASTER_ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer);
}

/* Decodes an incoming message and forwards it without its type byte. */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 1) {
        return;
    }
    add_peer(info->src_addr);
    if (s_recv_cb) {
        s_recv_cb(info->src_addr, data[0], data + 1, (uint16_t)(len - 1));
    }
}

/* Logs failed ESP-NOW transmissions; delivery is otherwise asynchronous. */
static void on_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "esp_now_send failed");
    }
}

/* Initializes ESP-NOW on the master and installs receive/send callbacks. */
esp_err_t wd_espnow_master_init(wd_espnow_recv_cb_t cb)
{
    s_recv_cb = cb;

    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_WD_MASTER_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));
    ESP_ERROR_CHECK(add_peer(s_broadcast_mac));

    ESP_LOGI(TAG, "ESP-NOW master ready on channel %d", CONFIG_WD_MASTER_ESPNOW_CHANNEL);
    return ESP_OK;
}

/* Prepends a message type and sends one message to a specific scanner node. */
esp_err_t wd_espnow_master_send(const uint8_t node_mac[6], uint8_t msg_type,
                                 const void *payload, uint16_t len)
{
    if (len > ESP_NOW_MAX_DATA_LEN - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    add_peer(node_mac);
    uint8_t buf[ESP_NOW_MAX_DATA_LEN];
    buf[0] = msg_type;
    if (len > 0 && payload) {
        memcpy(&buf[1], payload, len);
    }
    return esp_now_send(node_mac, buf, (size_t)len + 1);
}

/* Sends a typed message to every scanner listening on the ESP-NOW channel. */
esp_err_t wd_espnow_master_broadcast(uint8_t msg_type, const void *payload, uint16_t len)
{
    return wd_espnow_master_send(s_broadcast_mac, msg_type, payload, len);
}
