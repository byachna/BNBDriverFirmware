#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wd_uart_recv_cb_t)(uint8_t msg_type, const uint8_t *payload, uint16_t len);

esp_err_t wd_uart_master_init(wd_uart_recv_cb_t cb);
esp_err_t wd_uart_master_send(uint8_t msg_type, const void *payload, uint16_t len);

#ifdef __cplusplus
}
#endif
