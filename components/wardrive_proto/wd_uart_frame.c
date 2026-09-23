#include <string.h>
#include "wd_uart_frame.h"

void wd_uart_parser_reset(wd_uart_parser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->state = WD_PARSE_SYNC0;
}

bool wd_uart_parser_feed(wd_uart_parser_t *p, uint8_t byte, wd_uart_frame_t *out)
{
    switch (p->state) {
    case WD_PARSE_SYNC0:
        if (byte == WD_UART_SYNC_0) {
            p->state = WD_PARSE_SYNC1;
        }
        break;

    case WD_PARSE_SYNC1:
        if (byte == WD_UART_SYNC_1) {
            p->state = WD_PARSE_LEN_LO;
        } else {
            p->state = WD_PARSE_SYNC0;
        }
        break;

    case WD_PARSE_LEN_LO:
        p->len = byte;
        p->state = WD_PARSE_LEN_HI;
        break;

    case WD_PARSE_LEN_HI:
        p->len |= (uint16_t)byte << 8;
        if (p->len > WD_UART_MAX_PAYLOAD) {
            /* Corrupt or unsupported length, resync. */
            p->state = WD_PARSE_SYNC0;
        } else {
            p->state = WD_PARSE_TYPE;
        }
        break;

    case WD_PARSE_TYPE:
        p->type = byte;
        p->idx = 0;
        p->state = (p->len == 0) ? WD_PARSE_CRC_LO : WD_PARSE_PAYLOAD;
        break;

    case WD_PARSE_PAYLOAD:
        p->payload[p->idx++] = byte;
        if (p->idx >= p->len) {
            p->state = WD_PARSE_CRC_LO;
        }
        break;

    case WD_PARSE_CRC_LO:
        p->crc_lo = byte;
        p->state = WD_PARSE_CRC_HI;
        break;

    case WD_PARSE_CRC_HI: {
        uint16_t recv_crc = (uint16_t)p->crc_lo | ((uint16_t)byte << 8);
        uint8_t crc_buf[1 + WD_UART_MAX_PAYLOAD];
        crc_buf[0] = p->type;
        memcpy(&crc_buf[1], p->payload, p->len);
        uint16_t calc_crc = wd_crc16(crc_buf, (size_t)p->len + 1);

        p->state = WD_PARSE_SYNC0;
        if (recv_crc == calc_crc) {
            out->type = p->type;
            out->len = p->len;
            memcpy(out->payload, p->payload, p->len);
            return true;
        }
        break;
    }
    }

    return false;
}

int wd_uart_encode_frame(uint8_t type, const void *payload, uint16_t payload_len,
                          uint8_t *out_buf, size_t out_buf_size)
{
    if (payload_len > WD_UART_MAX_PAYLOAD) {
        return -1;
    }
    size_t needed = 2 /*sync*/ + 2 /*len*/ + 1 /*type*/ + payload_len + 2 /*crc*/;
    if (out_buf_size < needed) {
        return -1;
    }

    uint8_t crc_buf[1 + WD_UART_MAX_PAYLOAD];
    crc_buf[0] = type;
    if (payload_len > 0 && payload != NULL) {
        memcpy(&crc_buf[1], payload, payload_len);
    }
    uint16_t crc = wd_crc16(crc_buf, (size_t)payload_len + 1);

    size_t off = 0;
    out_buf[off++] = WD_UART_SYNC_0;
    out_buf[off++] = WD_UART_SYNC_1;
    out_buf[off++] = (uint8_t)(payload_len & 0xFF);
    out_buf[off++] = (uint8_t)(payload_len >> 8);
    out_buf[off++] = type;
    if (payload_len > 0 && payload != NULL) {
        memcpy(&out_buf[off], payload, payload_len);
        off += payload_len;
    }
    out_buf[off++] = (uint8_t)(crc & 0xFF);
    out_buf[off++] = (uint8_t)(crc >> 8);

    return (int)off;
}
