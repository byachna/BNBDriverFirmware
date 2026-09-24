#include <string.h>
#include "wifi_scan.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

static const char *TAG = "wd_wifi_scan";
static wifi_band_mode_t s_band_mode = WIFI_BAND_MODE_AUTO;

/* Initializes NVS, the station interface, and the scanner's Wi-Fi radio. */
esp_err_t wd_wifi_scan_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Not associated to any AP; scanning + ESP-NOW only. */
    esp_err_t band_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (band_err == ESP_OK) {
        s_band_mode = WIFI_BAND_MODE_AUTO;
    } else {
        ESP_LOGW(TAG, "esp_wifi_set_band_mode(AUTO) failed (0x%x), chip may be single-band", band_err);
    }

    return ESP_OK;
}

/* Selects the radio band needed for a 2.4 GHz, 5 GHz, or automatic channel. */
esp_err_t wd_wifi_ensure_band_for_channel(uint8_t channel)
{
    wifi_band_mode_t needed = (channel == 0 || channel <= 14) ? WIFI_BAND_MODE_2G_ONLY : WIFI_BAND_MODE_5G_ONLY;
    if (channel == 0) {
        needed = WIFI_BAND_MODE_AUTO;
    }
    if (needed == s_band_mode) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_set_band_mode(needed);
    if (err == ESP_OK) {
        s_band_mode = needed;
    }
    return err;
}

/* Performs one blocking scan and passes each discovered AP to the callback. */
static esp_err_t do_scan(uint8_t channel, uint32_t dwell_ms, wd_ap_found_cb_t cb)
{
    esp_err_t err = wd_wifi_ensure_band_for_channel(channel);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "band switch failed for channel %u: 0x%x", channel, err);
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = channel,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    err = esp_wifi_scan_start(&scan_cfg, true /* block until done */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed on channel %u: 0x%x", channel, err);
        return err;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        return ESP_OK;
    }

    wifi_ap_record_t *records = calloc(num, sizeof(wifi_ap_record_t));
    if (!records) {
        return ESP_ERR_NO_MEM;
    }

    uint16_t got = num;
    err = esp_wifi_scan_get_ap_records(&got, records);
    if (err == ESP_OK && cb) {
        for (uint16_t i = 0; i < got; i++) {
            cb(&records[i]);
        }
    }
    free(records);
    return err;
}

/* Scans one channel using the requested dwell time. */
esp_err_t wd_wifi_scan_channel(uint8_t channel, uint32_t dwell_ms, wd_ap_found_cb_t cb)
{
    return do_scan(channel, dwell_ms, cb);
}

/* Scans across all supported bands using the Wi-Fi driver's channel selection. */
esp_err_t wd_wifi_scan_all(uint32_t dwell_ms, wd_ap_found_cb_t cb)
{
    return do_scan(0, dwell_ms, cb);
}
