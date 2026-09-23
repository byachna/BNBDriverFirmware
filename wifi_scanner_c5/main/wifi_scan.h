#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types_generic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wd_ap_found_cb_t)(const wifi_ap_record_t *rec);

esp_err_t wd_wifi_scan_init(void);

/* Switches the radio's band mode (2.4GHz-only / 5GHz-only / auto) so it
 * covers the given channel, if it isn't already. Channel 0 means "any"
 * (auto). Used both before scanning a channel and before returning the
 * radio to the ESP-NOW control channel, since a channel change silently
 * fails if the current band mode excludes it. */
esp_err_t wd_wifi_ensure_band_for_channel(uint8_t channel);

/* Blocking active scan of a single Wi-Fi channel (1-14 = 2.4GHz,
 * 36+ = 5GHz on chips that support it, e.g. ESP32-C5). Automatically
 * switches band mode as needed. Invokes cb once per discovered AP. */
esp_err_t wd_wifi_scan_channel(uint8_t channel, uint32_t dwell_ms, wd_ap_found_cb_t cb);

/* Blocking scan of every channel supported by the current band mode
 * (used when no explicit channel assignment has been received yet). */
esp_err_t wd_wifi_scan_all(uint32_t dwell_ms, wd_ap_found_cb_t cb);

#ifdef __cplusplus
}
#endif
