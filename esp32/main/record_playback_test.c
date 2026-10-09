#include "record_playback_test.h"
#include "button_record_playback.h"
#include "record_playback_pcm.h"
#include "audio_board.h"
#include "microphone.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifdef CONFIG_DIGITS_MIC_RECORD_PLAYBACK
static esp_err_t run_record_playback(void)
{
    // Never consume 192 KB of internal memory needed by Wi-Fi and signaling.
    int16_t *recording = heap_caps_malloc(RECORD_PLAYBACK_SAMPLES * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recording) {
        ESP_LOGE("record_test", "No PSRAM recording buffer; check Octal PSRAM configuration");
        return ESP_ERR_NO_MEM;
    }
    if (!digits_audio_speaker_acquire(1500)) {
        ESP_LOGW("record_test", "Speaker busy; recording test skipped");
        heap_caps_free(recording);
        return ESP_ERR_TIMEOUT;
    }
    bool pause_requested = false;
    esp_err_t err = ESP_OK;
    digits_microphone_frame_t frame;
    int16_t stereo[DIGITS_AUDIO_DMA_FRAMES * 2];
    uint32_t sequence = 0;
    size_t samples = 0;
    // Reserve the speaker throughout recording, so Ring Test cannot become
    // part of the recording. Playback uses only the completed saved buffer.
    ESP_LOGI("record_test", "Recording 2s now: speak near the microphone");
    int64_t fresh_after = esp_timer_get_time() + 20000;
    int64_t deadline = fresh_after + 3000000;
    while (samples < RECORD_PLAYBACK_SAMPLES) {
        if (esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; goto finish; }
        err = digits_microphone_read(&frame, 100);
        if (err != ESP_OK) goto finish;
        if (!samples && (frame.captured_at_us < fresh_after || frame.flags)) continue;
        if (frame.flags || (samples && frame.sequence != sequence + 1)) {
            ESP_LOGE("record_test", "Recording discontinuity; incomplete audio will not be played");
            err = ESP_ERR_INVALID_STATE;
            goto finish;
        }
        memcpy(recording + samples, frame.pcm, sizeof(frame.pcm));
        samples += DIGITS_MIC_FRAME_SAMPLES;
        sequence = frame.sequence;
    }
    // Stop publication and mute input before enabling the speaker. Native RX
    // still drains because stopping its shared clock could affect TX.
    pause_requested = true;
    err = digits_microphone_pause(true, 1000);
    if (err != ESP_OK) goto finish;
    record_playback_gain_t gain;
    record_playback_output_t output = {0};
    if (!record_playback_gain_prepare(recording, samples, &gain)) {
        err = ESP_ERR_INVALID_SIZE;
        goto finish;
    }
    ESP_LOGI("record_test", "Saved PCM: RMS=%u peak=%u DC=%d; AC RMS=%u; playback gain=%.2fx (peak cap=8192)",
             (unsigned)gain.raw_rms, (unsigned)gain.input_peak, (int)gain.dc_offset,
             (unsigned)gain.ac_rms, (double)gain.gain_q8 / 256.0);
    ESP_LOGI("record_test", "Recording stopped: 96000 samples; playing saved 2s audio with bounded gain");
    err = digits_audio_speaker_start();
    if (err != ESP_OK) goto finish;
    deadline = esp_timer_get_time() + 3000000;
    for (size_t first = 0; first < RECORD_PLAYBACK_SAMPLES; first += DIGITS_AUDIO_DMA_FRAMES) {
        if (esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; goto finish; }
        record_playback_render_gain(stereo, recording, samples, first, DIGITS_AUDIO_DMA_FRAMES,
                                    &gain, &output);
        size_t written = 0;
        err = digits_audio_speaker_write(stereo, sizeof(stereo), &written, 50);
        if (err != ESP_OK || written != sizeof(stereo)) {
            if (err == ESP_OK) err = ESP_ERR_INVALID_SIZE;
            goto finish;
        }
    }
    ESP_LOGI("record_test", "Playback PCM: RMS=%u peak=%u limited=%u/%u samples",
             (unsigned)record_playback_output_rms(&output), (unsigned)output.peak,
             (unsigned)output.limited_samples, (unsigned)output.samples);
    // Writes copy ahead into DMA. Let both 10 ms descriptors finish, with a
    // scheduler tick of margin, before gating off the amplifier/final fade.
    vTaskDelay((2 * DIGITS_AUDIO_DMA_FRAMES * configTICK_RATE_HZ +
                DIGITS_AUDIO_SAMPLE_RATE - 1) / DIGITS_AUDIO_SAMPLE_RATE + 1);
finish:
    if (!digits_audio_speaker_silence()) err = ESP_FAIL;
    if (pause_requested) {
        // Also send resume after a pause timeout: a late mute must not leave
        // capture paused. Generation acknowledgements prevent stale success.
        esp_err_t resume_err = digits_microphone_pause(false, 1000);
        if (resume_err != ESP_OK) {
            ESP_LOGE("record_test", "Capture resume failed: %s", esp_err_to_name(resume_err));
            err = resume_err;
        }
    }
    digits_audio_speaker_release();
    heap_caps_free(recording);
    if (err == ESP_OK) ESP_LOGI("record_test", "Playback finished; amplifier disabled, microphone capture resumed");
    else ESP_LOGE("record_test", "Test stopped: %s (recorded %u samples)", esp_err_to_name(err), (unsigned)samples);
    return err;
}

static void record_worker(void *arg)
{
    ESP_LOGI("record_test", "One-shot microphone test in 5s; speak when 'Recording 2s now' appears");
    vTaskDelay(pdMS_TO_TICKS(5000));
    run_record_playback();
    vTaskDelete(NULL);
}
#endif

esp_err_t digits_record_playback_test_start(void)
{
#ifdef CONFIG_DIGITS_MIC_BUTTON_RECORD_PLAYBACK
    return digits_button_record_playback_start();
#endif
#ifdef CONFIG_DIGITS_MIC_RECORD_PLAYBACK
    static bool started;
    if (started) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(record_worker, "digits_record", 8192, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    started = true;
#endif
    return ESP_OK;
}
