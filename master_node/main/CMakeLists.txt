idf_component_register(
    SRCS "main.c" "application/wd_master_app.c" "ble_gatt.c" "uart_master.c" "espnow_master.c" "node_manager.c" "aggregator.c"
    INCLUDE_DIRS "." "application"
    REQUIRES wardrive_proto bt nvs_flash esp_wifi esp_driver_uart esp_netif esp_event
)
