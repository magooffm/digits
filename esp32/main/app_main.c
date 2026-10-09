#include "credentials.h"
#include "ring_test.h"
#include "audio_board.h"
#include "microphone.h"
#include "record_playback_test.h"
#include "signaling.h"
#include "wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"

void app_main(void)
{
    // Never erase NVS automatically: doing so could strand a paired device.
    ESP_ERROR_CHECK(nvs_flash_init());
    digits_wifi_start();
    digits_credentials_t credentials;
    ESP_ERROR_CHECK(credentials_load(&credentials));
    ESP_LOGI("digits", "Boot hardware_id=%s number=%s", credentials.hardware_id,
             credentials.device_token[0] ? credentials.number : "unpaired");
    esp_err_t err = digits_audio_board_init();
    if (err != ESP_OK)
        ESP_LOGE("digits", "Speaker initialization failed: %s; signaling continues", esp_err_to_name(err));
    else {
        err = digits_ring_test_init();
        if (err != ESP_OK)
            ESP_LOGE("digits", "Ring Test initialization failed: %s; signaling continues", esp_err_to_name(err));
        err = digits_microphone_init();
        if (err != ESP_OK)
            ESP_LOGE("digits", "Microphone initialization failed: %s; speaker and signaling continue", esp_err_to_name(err));
        else {
            err = digits_record_playback_test_start();
            if (err != ESP_OK)
                ESP_LOGE("digits", "Record/playback test initialization failed: %s; signaling continues", esp_err_to_name(err));
        }
    }
    digits_signaling_run(&credentials);
}
