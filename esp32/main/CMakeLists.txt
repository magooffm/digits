idf_component_register(SRCS "app_main.c" "credentials.c" "wifi.c" "signaling.c" "audio_board.c"
    "ring_test.c" "ring_tone.c" "incoming_ring.c" "line_settings.c" "microphone.c" "microphone_pcm.c"
    "webrtc.c" "webrtc_console.c" "call_audio.c" "call_audio_jitter.c"
    "record_playback_test.c" "record_playback_pcm.c" "button_record_playback.c" "record_button.c"
    "phone.c" "phone_fsm.c" "tone_gen.c" "call_tones.c" "ringtones.c" "ringtone_pack.c" "ring_select.c"
    "sd_ringtone_util.c" "sd_ringtones.c" "sd_card.c" "silence.c"
    "ogg_opus.c" "voicemail_store.c" "voicemail_prompt.c" "voicemail.c" "wifi_setup_form.c" "setup.c" "status.c" "log_ring.c" "web_config.c" "greeting_pcm.c" "greeting_record.c" "voice_style.c"
    INCLUDE_DIRS "."
    # The Pi's default greeting, used when the card has no greeting.mp3.
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../../pi/tones/voicemail_greeting.wav"
    EMBED_TXTFILES "setup_page.html" "status_page.html" "logs_page.html"
    REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_timer json
    esp_websocket_client esp_codec_dev esp_audio_codec esp_peer esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_partition esp_adc console
    fatfs sdmmc esp_driver_sdmmc esp_audio_effects esp_http_server dns_server app_update mbedtls)
