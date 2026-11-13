idf_component_register(SRCS "app_main.c" "wifi_setup.c" "captive_dns.c" "audio.c" "rtc.c" "control.c" "ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server esp_http_client
             driver esp_timer mbedtls json lwip agora_iot_sdk esp_ringbuf)
