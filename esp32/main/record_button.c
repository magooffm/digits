#include "record_button.h"
#include "audio_board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POLL_INTERVAL_MS 10
#define DEBOUNCE_US 30000LL
#define ERROR_LOG_INTERVAL_US 1000000LL

typedef struct {
    digits_record_button_state_t state;
    bool stable_known;
    bool candidate_known;
    bool candidate_pressed;
    int64_t candidate_at_us;
} record_button_filter_t;

static digits_record_button_state_t button_state = {.error = ESP_ERR_INVALID_STATE};
static portMUX_TYPE button_mux = portMUX_INITIALIZER_UNLOCKED;
static bool started;

// Retain the last confirmed edge across a transient failure. Recovery needs
// fresh stable samples but must not fabricate another press for the same hold.
static void filter_sample(record_button_filter_t *filter, bool pressed,
                          esp_err_t error, int64_t now_us)
{
    if (error != ESP_OK) {
        filter->state.valid = false;
        filter->state.error = error;
        filter->candidate_known = false;
        return;
    }
    if (!filter->candidate_known || pressed != filter->candidate_pressed) {
        filter->candidate_known = true;
        filter->candidate_pressed = pressed;
        filter->candidate_at_us = now_us;
    }
    if (now_us - filter->candidate_at_us < DEBOUNCE_US) return;
    if (!filter->stable_known || pressed != filter->state.pressed) {
        if (pressed) {
            ++filter->state.press_sequence;
            filter->state.pressed_at_us = filter->candidate_at_us;
        } else {
            filter->state.released_at_us = filter->candidate_at_us;
        }
        filter->state.pressed = pressed;
        filter->stable_known = true;
    }
    filter->state.valid = true;
    filter->state.error = ESP_OK;
}

static void publish_state(const digits_record_button_state_t *state)
{
    portENTER_CRITICAL(&button_mux);
    button_state = *state;
    portEXIT_CRITICAL(&button_mux);
}

void digits_record_button_snapshot(digits_record_button_state_t *state)
{
    if (!state) return;
    portENTER_CRITICAL(&button_mux);
    *state = button_state;
    portEXIT_CRITICAL(&button_mux);
}

static void button_worker(void *arg)
{
    (void)arg;
    record_button_filter_t filter = {.state = {.error = ESP_ERR_INVALID_STATE}};
    int64_t next_error_log = 0;
    TickType_t poll_ticks = pdMS_TO_TICKS(POLL_INTERVAL_MS);
    if (!poll_ticks) poll_ticks = 1;
    for (;;) {
        bool pressed = false;
        esp_err_t error = digits_audio_record_button_read(&pressed);
        int64_t now_us = esp_timer_get_time();
        filter_sample(&filter, pressed, error, now_us);
        // Publish before serial logging; consumers see a failure immediately.
        publish_state(&filter.state);
        if (error != ESP_OK && now_us >= next_error_log) {
            ESP_LOGE("record_button", "K1 read failed: %s; button state invalid",
                     esp_err_to_name(error));
            next_error_log = now_us + ERROR_LOG_INTERVAL_US;
        }
        // The bounded I2C operation and this wait run outside signaling/audio
        // capture. Delay relative to completion also avoids a retry busy loop.
        vTaskDelay(poll_ticks);
    }
}

esp_err_t digits_record_button_start(void)
{
    portENTER_CRITICAL(&button_mux);
    if (started) {
        portEXIT_CRITICAL(&button_mux);
        return ESP_ERR_INVALID_STATE;
    }
    started = true;
    portEXIT_CRITICAL(&button_mux);

    esp_err_t error = digits_audio_record_button_init();
    if (error == ESP_OK &&
        xTaskCreate(button_worker, "digits_record_key", 3072, NULL, 3, NULL) != pdPASS)
        error = ESP_ERR_NO_MEM;
    if (error != ESP_OK) {
        portENTER_CRITICAL(&button_mux);
        started = false;
        button_state.valid = false;
        button_state.error = error;
        portEXIT_CRITICAL(&button_mux);
        return error;
    }
    ESP_LOGI("record_button", "K1 ready: TCA9555 P11 (Extend_IO9), active-low; debounce=30 ms");
    return ESP_OK;
}
