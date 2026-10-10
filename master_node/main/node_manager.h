#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "wardrive_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int64_t (*now_us)(void);
	void (*uart_send)(uint8_t msg_type, const void *payload, uint16_t len);
	void (*espnow_send)(const uint8_t mac[6], uint8_t msg_type,
						const void *payload, uint16_t len);
	void (*espnow_broadcast)(uint8_t msg_type, const void *payload, uint16_t len);
} wd_node_manager_ports_t;

/* Installs platform ports used for time and node command delivery. */
void wd_node_manager_init(const wd_node_manager_ports_t *ports);

/* Registers a wireless (ESP-NOW) scanner node. Returns true if this MAC
 * was not previously known. Triggers a channel redistribution. */
bool wd_node_manager_register(const uint8_t mac[6]);

/* Records that a message was just received from a node, for liveness
 * logging. Registers the node first if it isn't already known. */
void wd_node_manager_touch(const uint8_t mac[6]);

/* Records that a message was just received from the locally-wired UART
 * node. Returns true the first time it's heard from (i.e. it just became
 * connected), so the caller can trigger a redistribution. */
bool wd_node_manager_touch_local(void);

/* Splits the full Wi-Fi channel list equally across every currently
 * connected node (the locally-wired UART node, once heard from, plus
 * every registered ESP-NOW node) and pushes the channel assignment to
 * each of them. Only also sends a start-scan command if scanning is
 * currently enabled (see wd_node_manager_start_all()); otherwise nodes
 * are assigned channels but stay idle until the phone app starts them. */
void wd_node_manager_redistribute(void);

/* Enables/disables scanning cluster-wide (driven by the BLE Start/Stop
 * commands) and pushes WD_MSG_START_SCAN / WD_MSG_STOP_SCAN to every
 * connected node accordingly. */
void wd_node_manager_start_all(void);
void wd_node_manager_stop_all(void);

/* Drops any node (local UART or ESP-NOW) that hasn't been heard from
 * within the stale timeout. Returns true if a node was dropped, in which
 * case the caller should call wd_node_manager_redistribute() to re-split
 * the channels among the remaining connected nodes. */
bool wd_node_manager_prune_stale(void);

/* Total connected scanner nodes, including the locally-wired one once
 * it has been heard from. */
uint32_t wd_node_manager_count(void);

/* Logs (ESP_LOGI) a one-line status for the local UART node and every
 * known wireless node: address and seconds since last message, or "no
 * data yet" if nothing has been heard from it. */
void wd_node_manager_log_status(void);

#ifdef __cplusplus
}
#endif
