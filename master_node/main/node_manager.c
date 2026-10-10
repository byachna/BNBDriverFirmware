#include <string.h>
#include "node_manager.h"
#include "wardrive_proto.h"
#include "wd_channels.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "wd_node_mgr";

#define WD_MAX_ESPNOW_NODES 8
#define WD_SCAN_DWELL_MS    250
#define WD_NODE_STALE_US    (15LL * 1000 * 1000) /* no message in 15s = stale */

typedef struct {
    uint8_t mac[6];
    int64_t last_seen_us;
} wd_espnow_node_t;

static wd_espnow_node_t s_nodes[WD_MAX_ESPNOW_NODES];
static uint32_t s_node_count = 0;
static int64_t s_local_last_seen_us = -1; /* -1 = never heard from local UART node */
static bool s_scan_enabled = false; /* only true once the phone app sends Start */
static wd_node_manager_ports_t s_ports;

/* Resets the connected-node table, timestamps, and scan command state. */
void wd_node_manager_init(const wd_node_manager_ports_t *ports)
{
    s_ports = *ports;
    s_node_count = 0;
    s_local_last_seen_us = -1;
    s_scan_enabled = false;
}

/* Finds a wireless scanner by MAC address and returns its index, or -1. */
static int find_node(const uint8_t mac[6])
{
    for (uint32_t i = 0; i < s_node_count; i++) {
        if (memcmp(s_nodes[i].mac, mac, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Adds a wireless scanner to the table and timestamps its first observation. */
bool wd_node_manager_register(const uint8_t mac[6])
{
    if (find_node(mac) >= 0) {
        return false;
    }
    if (s_node_count >= WD_MAX_ESPNOW_NODES) {
        ESP_LOGW(TAG, "Node table full, ignoring new node");
        return false;
    }
    memcpy(s_nodes[s_node_count].mac, mac, 6);
    s_nodes[s_node_count].last_seen_us = s_ports.now_us();
    s_node_count++;
    ESP_LOGI(TAG, "Registered ESP-NOW scanner node #%u ("MACSTR")", (unsigned)s_node_count, MAC2STR(mac));
    return true;
}

/* Refreshes a wireless scanner's last-seen time, registering it if necessary. */
void wd_node_manager_touch(const uint8_t mac[6])
{
    int idx = find_node(mac);
    if (idx < 0) {
        wd_node_manager_register(mac);
        idx = find_node(mac);
    }
    if (idx >= 0) {
        s_nodes[idx].last_seen_us = s_ports.now_us();
    }
}

/* Marks the wired scanner as heard and reports whether this is its first contact. */
bool wd_node_manager_touch_local(void)
{
    bool was_connected = (s_local_last_seen_us >= 0);
    s_local_last_seen_us = s_ports.now_us();
    return !was_connected;
}

/* Reports whether the wired scanner has sent any message since initialization. */
static bool local_connected(void)
{
    return s_local_last_seen_us >= 0;
}

/* Returns the wired scanner plus all currently registered wireless scanners. */
uint32_t wd_node_manager_count(void)
{
    return s_node_count + (local_connected() ? 1 : 0);
}

/* target_index is an index into the set of currently connected nodes: the
 * local UART node (if connected) occupies index 0, followed by the
 * registered ESP-NOW nodes. */
/* Builds the channel slice for one node and sends its current scan commands. */
static void build_and_send(uint32_t target_index, uint32_t total_targets, bool have_local)
{
    wd_channel_assignment_t assign = {0};
    assign.band = WD_BAND_2G4; /* informational only; node picks band per-channel */
    assign.dwell_ms = WD_SCAN_DWELL_MS;

    for (uint32_t i = target_index; i < WD_ALL_CHANNELS_COUNT; i += total_targets) {
        if (assign.num_channels >= WD_MAX_CHANNELS) {
            break;
        }
        assign.channels[assign.num_channels++] = WD_ALL_CHANNELS[i];
    }

    if (have_local && target_index == 0) {
        s_ports.uart_send(WD_MSG_ASSIGN_CHANNELS, &assign, sizeof(assign));
        if (s_scan_enabled) {
            s_ports.uart_send(WD_MSG_START_SCAN, NULL, 0);
        }
    } else {
        uint32_t node_idx = have_local ? target_index - 1 : target_index;
        const uint8_t *mac = s_nodes[node_idx].mac;
        s_ports.espnow_send(mac, WD_MSG_ASSIGN_CHANNELS, &assign, sizeof(assign));
        if (s_scan_enabled) {
            s_ports.espnow_send(mac, WD_MSG_START_SCAN, NULL, 0);
        }
    }
}

/* Recomputes an even channel split and sends assignments to every node. */
void wd_node_manager_redistribute(void)
{
    bool have_local = local_connected();
    uint32_t total = wd_node_manager_count();
    if (total == 0) {
        ESP_LOGW(TAG, "No connected nodes yet; deferring channel redistribution");
        return;
    }
    ESP_LOGI(TAG, "Redistributing %u channels equally across %u connected node(s)",
             (unsigned)WD_ALL_CHANNELS_COUNT, (unsigned)total);
    for (uint32_t t = 0; t < total; t++) {
        build_and_send(t, total, have_local);
    }
}

/* Enables scanning and broadcasts a start command to all connected transports. */
void wd_node_manager_start_all(void)
{
    s_scan_enabled = true;
    s_ports.uart_send(WD_MSG_START_SCAN, NULL, 0);
    s_ports.espnow_broadcast(WD_MSG_START_SCAN, NULL, 0);
}

/* Disables scanning and broadcasts a stop command to all connected transports. */
void wd_node_manager_stop_all(void)
{
    s_scan_enabled = false;
    s_ports.uart_send(WD_MSG_STOP_SCAN, NULL, 0);
    s_ports.espnow_broadcast(WD_MSG_STOP_SCAN, NULL, 0);
}

/* Reports whether a timestamp is older than the node liveness threshold. */
static bool is_stale(int64_t last_seen_us, int64_t now)
{
    return (now - last_seen_us) >= WD_NODE_STALE_US;
}

/* Removes nodes that have missed the liveness timeout and reports any change. */
bool wd_node_manager_prune_stale(void)
{
    int64_t now = s_ports.now_us();
    bool changed = false;

    if (local_connected() && is_stale(s_local_last_seen_us, now)) {
        ESP_LOGW(TAG, "local UART node went stale; dropping from channel split");
        s_local_last_seen_us = -1;
        changed = true;
    }

    for (uint32_t i = 0; i < s_node_count; ) {
        if (is_stale(s_nodes[i].last_seen_us, now)) {
            ESP_LOGW(TAG, "wireless node ("MACSTR") went stale; dropping from channel split",
                     MAC2STR(s_nodes[i].mac));
            s_nodes[i] = s_nodes[s_node_count - 1];
            s_node_count--;
            changed = true;
        } else {
            i++;
        }
    }

    return changed;
}

/* Logs liveness and last-seen age for every known scanner node. */
void wd_node_manager_log_status(void)
{
    int64_t now = s_ports.now_us();

    if (s_local_last_seen_us < 0) {
        ESP_LOGI(TAG, "local UART node: no data yet");
    } else {
        int64_t age_s = (now - s_local_last_seen_us) / 1000000;
        ESP_LOGI(TAG, "local UART node: %s (last message %lld s ago)",
                 (now - s_local_last_seen_us) < WD_NODE_STALE_US ? "alive" : "STALE", age_s);
    }

    for (uint32_t i = 0; i < s_node_count; i++) {
        int64_t age_s = (now - s_nodes[i].last_seen_us) / 1000000;
        ESP_LOGI(TAG, "wireless node %u ("MACSTR"): %s (last message %lld s ago)",
                 (unsigned)(i + 1), MAC2STR(s_nodes[i].mac),
                 (now - s_nodes[i].last_seen_us) < WD_NODE_STALE_US ? "alive" : "STALE", age_s);
    }
}
