#pragma once
/*
 * Shared wire protocol between the master node (ESP32-WROOM-32E) and the
 * Wi-Fi scanner nodes (ESP32-C5), whether attached locally via UART or
 * wirelessly via ESP-NOW.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_PROTO_VERSION      1
#define WD_MAX_SSID_LEN       32
#define WD_MAX_CHANNELS       40
#define WD_MAC_LEN            6

/* UART framing constants (not used on the ESP-NOW transport). */
#define WD_UART_SYNC_0        0xAA
#define WD_UART_SYNC_1        0x55
#define WD_UART_MAX_PAYLOAD   240

typedef enum {
    WD_BAND_2G4 = 0,
    WD_BAND_5G  = 1,
} wd_band_t;

/* Message types exchanged between master <-> scanner node. */
typedef enum {
    WD_MSG_HELLO_REQ        = 0x01, /* master -> node: identify yourself */
    WD_MSG_HELLO_RESP       = 0x02, /* node -> master: identity/capabilities */
    WD_MSG_ASSIGN_CHANNELS  = 0x03, /* master -> node: channel/band assignment */
    WD_MSG_START_SCAN       = 0x04, /* master -> node */
    WD_MSG_STOP_SCAN        = 0x05, /* master -> node */
    WD_MSG_AP_RECORD        = 0x06, /* node -> master: one discovered AP */
    WD_MSG_SCAN_CYCLE_DONE  = 0x07, /* node -> master: finished a full sweep */
    WD_MSG_HEARTBEAT        = 0x08, /* node -> master: keep-alive */
} wd_msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t  bssid[WD_MAC_LEN];
    char     ssid[WD_MAX_SSID_LEN + 1];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  band;        /* wd_band_t */
    uint8_t  authmode;    /* wifi_auth_mode_t value, kept as raw int to avoid
                              pulling esp_wifi headers into this shared file */
    uint8_t  node_id[WD_MAC_LEN]; /* originating node MAC, all-zero = locally
                                      UART-attached node */
} wd_ap_record_t;

typedef struct __attribute__((packed)) {
    uint8_t  band;                    /* wd_band_t requested */
    uint8_t  num_channels;
    uint8_t  channels[WD_MAX_CHANNELS];
    uint32_t dwell_ms;                /* time to spend per channel */
} wd_channel_assignment_t;

typedef struct __attribute__((packed)) {
    uint8_t  proto_version;
    uint8_t  mac[WD_MAC_LEN];
    uint8_t  supports_5g;
} wd_hello_resp_t;

/* 2.4GHz channels 1-13 plus the commonly-used 5GHz UNII channels (20MHz
 * spacing). Shared by the master (for splitting channels across nodes)
 * and scanner nodes (for the default full scan before any assignment
 * arrives), so both sides always agree on the full channel set. */
static const uint8_t WD_ALL_CHANNELS[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
};
#define WD_ALL_CHANNELS_COUNT (sizeof(WD_ALL_CHANNELS) / sizeof(WD_ALL_CHANNELS[0]))

/* CRC16/CCITT-FALSE, used to guard UART frames. */
uint16_t wd_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
