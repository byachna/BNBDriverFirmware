idf_component_register(
    SRCS "main.c" "wifi_scan.c" "espnow_transport.c" "uart_transport.c"
    INCLUDE_DIRS "."
    REQUIRES wardrive_proto esp_wifi nvs_flash esp_driver_uart esp_netif esp_event esp_timer
)
