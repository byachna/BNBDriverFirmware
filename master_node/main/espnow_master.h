#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wd_espnow_recv_cb_t)(const uint8_t node_mac[6], uint8_t msg_type,
                                     const uint8_t *payload, uint16_t len);

esp_err_t wd_espnow_master_init(wd_espnow_recv_cb_t cb);
esp_err_t wd_espnow_master_send(const uint8_t node_mac[6], uint8_t msg_type,
                                 const void *payload, uint16_t len);
esp_err_t wd_espnow_master_broadcast(uint8_t msg_type, const void *payload, uint16_t len);

#ifdef __cplusplus
}
#endif
