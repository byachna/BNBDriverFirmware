#pragma once
/* Byte-stream framing for the UART transport between master and a
 * locally-wired ESP32-C5 scanner. Not used by the ESP-NOW transport. */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "wardrive_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  type;
    uint16_t len;
    uint8_t  payload[WD_UART_MAX_PAYLOAD];
} wd_uart_frame_t;

typedef enum {
    WD_PARSE_SYNC0 = 0,
    WD_PARSE_SYNC1,
    WD_PARSE_LEN_LO,
    WD_PARSE_LEN_HI,
    WD_PARSE_TYPE,
    WD_PARSE_PAYLOAD,
    WD_PARSE_CRC_LO,
    WD_PARSE_CRC_HI,
} wd_parse_state_t;

typedef struct {
    wd_parse_state_t state;
    uint16_t len;
    uint16_t idx;
    uint8_t  type;
    uint8_t  payload[WD_UART_MAX_PAYLOAD];
    uint8_t  crc_lo;
} wd_uart_parser_t;

void wd_uart_parser_reset(wd_uart_parser_t *p);

/* Feed one received byte. Returns true and fills *out when a full frame with
 * a valid CRC has been assembled. Automatically resyncs on any framing or
 * CRC error. */
bool wd_uart_parser_feed(wd_uart_parser_t *p, uint8_t byte, wd_uart_frame_t *out);

/* Encode a frame into out_buf. Returns the number of bytes written, or -1 if
 * out_buf_size is too small / payload_len exceeds WD_UART_MAX_PAYLOAD. */
int wd_uart_encode_frame(uint8_t type, const void *payload, uint16_t payload_len,
                          uint8_t *out_buf, size_t out_buf_size);

#ifdef __cplusplus
}
#endif
