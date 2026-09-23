#include "sdkconfig.h"
#if CONFIG_WD_TRANSPORT_UART

#include <string.h>
#include "transport.h"
#include "wd_uart_frame.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wd_uart_tx";
static wd_transport_recv_cb_t s_recv_cb;

#define UART_PORT      (CONFIG_WD_UART_PORT_NUM)
#define UART_RX_BUF_SZ 2048

/* Reads framed UART bytes forever and forwards complete messages. */
static void uart_rx_task(void *arg)
{
    wd_uart_parser_t parser;
    wd_uart_parser_reset(&parser);
    wd_uart_frame_t frame;
    uint8_t byte;

    while (1) {
        int n = uart_read_bytes(UART_PORT, &byte, 1, portMAX_DELAY);
        if (n <= 0) {
            continue;
        }
        if (wd_uart_parser_feed(&parser, byte, &frame)) {
            if (s_recv_cb) {
                s_recv_cb(frame.type, frame.payload, frame.len);
            }
        }
    }
}

/* Configures the scanner UART and starts its receive task. */
esp_err_t wd_transport_init(wd_transport_recv_cb_t recv_cb)
{
    s_recv_cb = recv_cb;

    uart_config_t uart_cfg = {
        .baud_rate = CONFIG_WD_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF_SZ, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, CONFIG_WD_UART_TX_GPIO, CONFIG_WD_UART_RX_GPIO,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(uart_rx_task, "wd_uart_rx", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "UART transport ready on port %d (tx=%d rx=%d @ %d baud)",
             UART_PORT, CONFIG_WD_UART_TX_GPIO, CONFIG_WD_UART_RX_GPIO, CONFIG_WD_UART_BAUD);
    return ESP_OK;
}

/* Encodes and writes one protocol frame over the scanner UART. */
esp_err_t wd_transport_send(uint8_t msg_type, const void *payload, uint16_t len)
{
    uint8_t buf[8 + WD_UART_MAX_PAYLOAD];
    int n = wd_uart_encode_frame(msg_type, payload, len, buf, sizeof(buf));
    if (n < 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    int written = uart_write_bytes(UART_PORT, (const char *)buf, n);
    return (written == n) ? ESP_OK : ESP_FAIL;
}

/* Does nothing because UART communication is independent of Wi-Fi channels. */
void wd_transport_resync(void)
{
    /* Nothing to do: the UART link is unaffected by Wi-Fi channel hops. */
}

/* Treats the wired point-to-point transport as permanently connected. */
bool wd_transport_is_connected(void)
{
    /* Wired point-to-point link; treat as always connected. */
    return true;
}

#endif /* CONFIG_WD_TRANSPORT_UART */
