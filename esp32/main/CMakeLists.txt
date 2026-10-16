idf_component_register(SRCS "app_main.c" "credentials.c" "wifi.c" "signaling.c" "audio_board.c"
    "ring_test.c" "ring_tone.c" "incoming_ring.c" "line_settings.c" "microphone.c" "microphone_pcm.c"
    "webrtc.c" "webrtc_console.c" "call_audio.c" "call_audio_jitter.c"
    "record_playback_test.c" "record_playback_pcm.c" "button_record_playback.c" "record_button.c"
    INCLUDE_DIRS "." REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_timer json
    esp_websocket_client esp_codec_dev esp_audio_codec esp_peer esp_driver_i2c esp_driver_i2s console)
