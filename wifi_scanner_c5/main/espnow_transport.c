#include "sdkconfig.h"
#if CONFIG_WD_TRANSPORT_ESPNOW

#include <string.h>
#include <stdbool.h>
#include "transport.h"
#include "wifi_scan.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "wd_espnow_tx";
static wd_transport_recv_cb_t s_recv_cb;
static uint8_t s_master_mac[6];
static bool s_master_known = false;

static const uint8_t s_broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* Adds an ESP-NOW peer on the scanner's fixed control channel if needed. */
static esp_err_t add_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = CONFIG_WD_ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer);
}

/* Learns the master address, registers it as a peer, and forwards its message. */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 1) {
        return;
    }
    if (!s_master_known || memcmp(info->src_addr, s_master_mac, 6) != 0) {
        memcpy(s_master_mac, info->src_addr, 6);
        s_master_known = true;
        add_peer(s_master_mac);
        ESP_LOGI(TAG, "Master node MAC learned: " MACSTR, MAC2STR(s_master_mac));
    }
    if (s_recv_cb) {
        s_recv_cb(data[0], data + 1, (uint16_t)(len - 1));
    }
}

/* Logs failed asynchronous ESP-NOW transmissions. */
static void on_send(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "esp_now_send failed");
    }
}

/* Initializes the ESP-NOW scanner transport and its callbacks. */
esp_err_t wd_transport_init(wd_transport_recv_cb_t recv_cb)
{
    s_recv_cb = recv_cb;

    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_WD_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));
    ESP_ERROR_CHECK(add_peer(s_broadcast_mac));

    ESP_LOGI(TAG, "ESP-NOW transport ready on channel %d, waiting for master", CONFIG_WD_ESPNOW_CHANNEL);
    return ESP_OK;
}

/* Prepends a message type and sends a message to the learned master or broadcast. */
esp_err_t wd_transport_send(uint8_t msg_type, const void *payload, uint16_t len)
{
    if (len > ESP_NOW_MAX_DATA_LEN - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t buf[ESP_NOW_MAX_DATA_LEN];
    buf[0] = msg_type;
    if (len > 0 && payload) {
        memcpy(&buf[1], payload, len);
    }
    const uint8_t *dest = s_master_known ? s_master_mac : s_broadcast_mac;
    return esp_now_send(dest, buf, (size_t)len + 1);
}

/* Restores the Wi-Fi band and channel required for ESP-NOW control traffic. */
void wd_transport_resync(void)
{
    /* Scanning a 5GHz channel restricts the radio to WIFI_BAND_MODE_5G_ONLY,
     * which makes esp_wifi_set_channel() below silently fail to move back
     * to a 2.4GHz control channel (leaving the radio's actual/"home"a
     * channel mismatched from the ESP-NOW peer's configured channel, so
     * esp_now_send() then fails with "Peer channel is not equal to the
     * home channel"). Restore the right band mode first. */
    esp_err_t err = wd_wifi_ensure_band_for_channel(CONFIG_WD_ESPNOW_CHANNEL);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "band switch to ESP-NOW channel %d failed: 0x%x", CONFIG_WD_ESPNOW_CHANNEL, err);
    }
    err = esp_wifi_set_channel(CONFIG_WD_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_channel(%d) failed: 0x%x", CONFIG_WD_ESPNOW_CHANNEL, err);
    }
}

/* Reports whether an ESP-NOW master address has been learned. */
bool wd_transport_is_connected(void)
{
    return s_master_known;
}

#endif /* CONFIG_WD_TRANSPORT_ESPNOW */
