idf_component_register(SRCS "app_main.c" "credentials.c" "wifi.c" "signaling.c" "ring_test.c" "ring_tone.c"
    INCLUDE_DIRS "." REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_timer json
    esp_websocket_client esp_codec_dev esp_driver_i2c esp_driver_i2s)
