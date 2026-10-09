#include "button_record_playback.h"
#include "audio_board.h"
#include "microphone.h"
#include "record_button.h"
#include "record_playback_pcm.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifdef CONFIG_DIGITS_MIC_BUTTON_RECORD_PLAYBACK
#define RECORD_SECONDS 10
#define RECORD_CAPACITY (DIGITS_AUDIO_SAMPLE_RATE * RECORD_SECONDS)
#define RECORD_FRAMES (RECORD_CAPACITY / DIGITS_MIC_FRAME_SAMPLES)
_Static_assert(RECORD_CAPACITY % DIGITS_MIC_FRAME_SAMPLES == 0, "Whole capture frames required");
_Static_assert(RECORD_CAPACITY * sizeof(int16_t) % _Alignof(int64_t) == 0, "Timestamp alignment required");

static esp_err_t run_button_recording(const digits_record_button_state_t *press)
{
    // PCM and frame timestamps share one bounded PSRAM allocation. Keeping
    // timestamps lets us remove frames captured during release debounce,
    // without assuming evenly spaced task/read completion timestamps.
    const size_t pcm_bytes = RECORD_CAPACITY * sizeof(int16_t);
    int16_t *recording = heap_caps_aligned_alloc(_Alignof(int64_t),
        pcm_bytes + RECORD_FRAMES * sizeof(int64_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recording) {
        ESP_LOGE("record_test", "No PSRAM buffer for KEY1 recording");
        return ESP_ERR_NO_MEM;
    }
    int64_t *captured_at = (int64_t *)((uint8_t *)recording + pcm_bytes);
    if (!digits_audio_speaker_acquire(0)) {
        ESP_LOGW("record_test", "Speaker busy; release KEY1 and try again");
        heap_caps_free(recording);
        return ESP_ERR_TIMEOUT;
    }
    bool speaker_owned = true, pause_requested = false;
    esp_err_t err = ESP_OK;
    size_t frames = 0;
    uint32_t sequence = 0;
    digits_microphone_frame_t frame;
    digits_record_button_state_t button;
    // Only record whole frames acquired after the speaker reservation. Older
    // queued frames may contain a Ring Test that ended while KEY1 debounced.
    const int64_t fresh_after = esp_timer_get_time() + 20000;
    const int64_t initial_deadline = esp_timer_get_time() + 1000000;
    ESP_LOGI("record_test", "KEY1 held: recording now (maximum 10s); release to play");
    while (frames < RECORD_FRAMES) {
        digits_record_button_snapshot(&button);
        if (!button.valid) { err = button.error; goto finish; }
        if (button.press_sequence != press->press_sequence) { err = ESP_ERR_INVALID_STATE; goto finish; }
        if (!button.pressed) break;
        if (!frames && esp_timer_get_time() >= initial_deadline) { err = ESP_ERR_TIMEOUT; goto finish; }
        err = digits_microphone_read(&frame, 100);
        if (err == ESP_ERR_TIMEOUT && !frames && esp_timer_get_time() < initial_deadline)
            continue; // ADC may still be in its 200 ms resume warmup.
        if (err != ESP_OK) goto finish;
        digits_record_button_snapshot(&button);
        if (!button.valid) { err = button.error; goto finish; }
        if (button.press_sequence != press->press_sequence) { err = ESP_ERR_INVALID_STATE; goto finish; }
        if (!button.pressed && frame.captured_at_us > button.released_at_us) break;
        if (!frames && (frame.captured_at_us < fresh_after || frame.flags)) continue;
        if (frame.flags || (frames && frame.sequence != sequence + 1)) {
            ESP_LOGE("record_test", "KEY1 recording discontinuity; recording discarded");
            err = ESP_ERR_INVALID_STATE;
            goto finish;
        }
        memcpy(recording + frames * DIGITS_MIC_FRAME_SAMPLES, frame.pcm, sizeof(frame.pcm));
        captured_at[frames++] = frame.captured_at_us;
        sequence = frame.sequence;
    }
    // Remove any tail recorded while the release edge was being debounced.
    // Whole 20 ms frames only: completion times are not calibrated ADC times.
    if (!button.pressed) {
        while (frames && captured_at[frames - 1] > button.released_at_us) --frames;
    }
    if (!frames) {
        err = ESP_OK;
        ESP_LOGI("record_test", "KEY1 tap contained no complete microphone frame; nothing to play");
        goto finish;
    }
    pause_requested = true;
    err = digits_microphone_pause(true, 1000);
    if (err != ESP_OK) goto finish;
    if (button.pressed) {
        ESP_LOGI("record_test", "10s recording limit reached; release KEY1 to play");
        // A stuck/long-held button must not reserve the speaker indefinitely.
        // Recording is complete and ADC muted; Ring Test may use TX while we wait.
        if (!digits_audio_speaker_silence()) { err = ESP_FAIL; goto finish; }
        digits_audio_speaker_release();
        speaker_owned = false;
        do {
            vTaskDelay(pdMS_TO_TICKS(10));
            digits_record_button_snapshot(&button);
            if (!button.valid) { err = button.error; goto finish; }
            if (button.press_sequence != press->press_sequence) { err = ESP_ERR_INVALID_STATE; goto finish; }
        } while (button.pressed);
        if (!digits_audio_speaker_acquire(0)) {
            ESP_LOGW("record_test", "Speaker busy after release; recording discarded");
            err = ESP_ERR_TIMEOUT;
            goto finish;
        }
        speaker_owned = true;
    }
    while (frames && captured_at[frames - 1] > button.released_at_us) --frames;
    if (!frames) {
        ESP_LOGI("record_test", "No complete frame before KEY1 release; nothing to play");
        goto finish;
    }
    const size_t samples = frames * DIGITS_MIC_FRAME_SAMPLES;
    record_playback_gain_t gain;
    record_playback_output_t output = {0};
    if (!record_playback_gain_prepare(recording, samples, &gain)) {
        err = ESP_ERR_INVALID_SIZE;
        goto finish;
    }
    ESP_LOGI("record_test", "Saved PCM: RMS=%u peak=%u DC=%d; AC RMS=%u; playback gain=%.2fx (peak cap=8192)",
             (unsigned)gain.raw_rms, (unsigned)gain.input_peak, (int)gain.dc_offset,
             (unsigned)gain.ac_rms, (double)gain.gain_q8 / 256.0);
    ESP_LOGI("record_test", "KEY1 released: playing %u ms (%u samples) with bounded gain",
             (unsigned)(samples * 1000 / DIGITS_AUDIO_SAMPLE_RATE), (unsigned)samples);
    err = digits_audio_speaker_start();
    if (err != ESP_OK) goto finish;
    const int64_t playback_deadline = esp_timer_get_time() +
        (int64_t)samples * 1000000 / DIGITS_AUDIO_SAMPLE_RATE + 1000000;
    int16_t stereo[DIGITS_AUDIO_DMA_FRAMES * 2];
    for (size_t first = 0; first < samples; first += DIGITS_AUDIO_DMA_FRAMES) {
        if (esp_timer_get_time() >= playback_deadline) { err = ESP_ERR_TIMEOUT; goto finish; }
        size_t count = samples - first;
        if (count > DIGITS_AUDIO_DMA_FRAMES) count = DIGITS_AUDIO_DMA_FRAMES;
        record_playback_render_gain(stereo, recording, samples, first, count, &gain, &output);
        size_t written = 0, bytes = count * 2 * sizeof(int16_t);
        err = digits_audio_speaker_write(stereo, bytes, &written, 50);
        if (err != ESP_OK || written != bytes) {
            if (err == ESP_OK) err = ESP_ERR_INVALID_SIZE;
            goto finish;
        }
    }
    ESP_LOGI("record_test", "Playback PCM: RMS=%u peak=%u limited=%u/%u samples",
             (unsigned)record_playback_output_rms(&output), (unsigned)output.peak,
             (unsigned)output.limited_samples, (unsigned)output.samples);
    vTaskDelay((2 * DIGITS_AUDIO_DMA_FRAMES * configTICK_RATE_HZ +
                DIGITS_AUDIO_SAMPLE_RATE - 1) / DIGITS_AUDIO_SAMPLE_RATE + 1);
finish:
    if (speaker_owned && !digits_audio_speaker_silence()) err = ESP_FAIL;
    if (pause_requested) {
        // A timed-out pause can commit late. Resume supersedes that generation.
        esp_err_t resume_err = digits_microphone_pause(false, 1000);
        if (resume_err != ESP_OK) {
            ESP_LOGE("record_test", "Capture resume failed: %s", esp_err_to_name(resume_err));
            err = resume_err;
        }
    }
    if (speaker_owned) digits_audio_speaker_release();
    heap_caps_free(recording);
    if (err == ESP_OK && frames)
        ESP_LOGI("record_test", "KEY1 playback finished; amplifier disabled, capture resumed");
    else if (err != ESP_OK)
        ESP_LOGE("record_test", "KEY1 test stopped: %s", esp_err_to_name(err));
    return err;
}

static void button_record_worker(void *arg)
{
    bool armed = false;
    uint32_t sequence = 0;
    ESP_LOGI("record_test", "Hold KEY1 to record, release to play (maximum 10s); waiting for released key");
    for (;;) {
        digits_record_button_state_t button;
        digits_record_button_snapshot(&button);
        if (!button.valid) armed = false;
        else if (!armed && !button.pressed) {
            sequence = button.press_sequence;
            armed = true;
        } else if (armed && button.pressed && sequence != button.press_sequence) {
            run_button_recording(&button);
            // Ignore all presses made during playback. A released key must be
            // observed again before another fresh press can start recording.
            armed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif

esp_err_t digits_button_record_playback_start(void)
{
#ifdef CONFIG_DIGITS_MIC_BUTTON_RECORD_PLAYBACK
    static bool started;
    if (started) return ESP_ERR_INVALID_STATE;
    esp_err_t err = digits_record_button_start();
    if (err != ESP_OK) return err;
    if (xTaskCreate(button_record_worker, "digits_button_record", 8192, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    started = true;
#endif
    return ESP_OK;
}
