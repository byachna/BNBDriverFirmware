#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Invoked whenever a full, validated message arrives from the master,
 * regardless of which physical transport is in use. */
typedef void (*wd_transport_recv_cb_t)(uint8_t msg_type, const uint8_t *payload, uint16_t len);

esp_err_t wd_transport_init(wd_transport_recv_cb_t recv_cb);

/* Send a message to the master. Blocking on UART, best-effort on ESP-NOW. */
esp_err_t wd_transport_send(uint8_t msg_type, const void *payload, uint16_t len);

/* Called after each channel hop during scanning so the transport can
 * re-sync (e.g. ESP-NOW must hop back to its control channel). No-op on
 * the wired UART transport. */
void wd_transport_resync(void);

/* True if the master is currently reachable (ESP-NOW: master MAC learned;
 * UART: always true since the link is point-to-point wired). */
bool wd_transport_is_connected(void);

#ifdef __cplusplus
}
#endif
