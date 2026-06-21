idf_component_register(
    SRCS "main.cpp" "d50_parse.c" "influx.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_client nvs_flash esp_netif esp_event
             usb driver esp_timer
    PRIV_REQUIRES esp-tls)
