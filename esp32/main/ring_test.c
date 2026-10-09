#include "ring_test.h"
#include "ring_tone.h"
#include "audio_board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define PCM_BLOCK_FRAMES DIGITS_AUDIO_DMA_FRAMES
#define TEST_DURATION_US 1000000LL
#define TEST_VOLUME 50

static struct { QueueHandle_t requests; } audio;

static bool silence_output(void)
{
    return digits_audio_speaker_silence();
}

static void ring_worker(void *arg)
{
    int16_t pcm[PCM_BLOCK_FRAMES * 2];
    // IDF converts write milliseconds to ticks, rounding down. Allow one DMA
    // block plus a whole tick of scheduling margin before admitting a write.
    const uint32_t dma_ticks = (PCM_BLOCK_FRAMES * configTICK_RATE_HZ +
        RING_TONE_SAMPLE_RATE - 1) / RING_TONE_SAMPLE_RATE;
    const uint32_t min_write_ms = ((dma_ticks + 1) * 1000 +
        configTICK_RATE_HZ - 1) / configTICK_RATE_HZ;
    const uint32_t max_write_ms = min_write_ms > 50 ? min_write_ms : 50;
    int64_t previous_deadline = 0;
    for (;;) {
        int64_t requested_at;
        xQueueReceive(audio.requests, &requested_at, portMAX_DELAY);
        // Pi schedules an independent STOP for every request. Its earliest
        // stop truncates overlapping tests; do not extend our active window.
        if (requested_at < previous_deadline) {
            ESP_LOGI("ring_test", "Overlapping test coalesced; one-second stop retained");
            continue;
        }
        if (!digits_audio_speaker_acquire(0)) {
            ESP_LOGW("ring_test", "Speaker reserved by microphone record/playback test; Ring Test skipped");
            continue;
        }
        int64_t deadline = esp_timer_get_time() + TEST_DURATION_US;
        previous_deadline = deadline;
        esp_err_t err = digits_audio_speaker_start();
        if (err == ESP_OK) {
            ESP_LOGI("ring_test", "Playing 1s speaker test (440+480 Hz, volume=%d)", TEST_VOLUME);
            for (size_t frame = 0; frame < RING_TONE_FRAMES; frame += PCM_BLOCK_FRAMES) {
                ring_tone_render(pcm, frame, PCM_BLOCK_FRAMES);
                // Recheck after rendering. Near the stop deadline, let DMA
                // drain instead of polling for another buffer with zero ticks.
                int64_t remaining = deadline - esp_timer_get_time();
                if (remaining < (int64_t)min_write_ms * 1000) break;
                size_t written = 0;
                // Direct bounded I2S writes, rather than codec's long default
                // wait. The WebSocket task never performs audio work.
                uint32_t timeout_ms = (uint32_t)(remaining / 1000);
                if (timeout_ms > max_write_ms) timeout_ms = max_write_ms;
                err = digits_audio_speaker_write(pcm, sizeof(pcm), &written, timeout_ms);
                if (err != ESP_OK || written != sizeof(pcm)) {
                    ESP_LOGE("ring_test", "Playback write failed: %s (%u bytes)",
                             esp_err_to_name(err), (unsigned)written);
                    err = ESP_FAIL;
                    break;
                }
            }
            while (err == ESP_OK && esp_timer_get_time() < deadline)
                vTaskDelay(1);
        } else {
            ESP_LOGE("ring_test", "Cannot enable speaker output: %s", esp_err_to_name(err));
        }
        if (silence_output())
            ESP_LOGI("ring_test", "Speaker test stopped; amplifier disabled");
        else
            ESP_LOGE("ring_test", "Speaker test finished; amplifier gate failed, DAC mute and PCM silence requested");
        digits_audio_speaker_release();
    }
}

static void release_audio(void)
{
    if (audio.requests) vQueueDelete(audio.requests);
    audio.requests = NULL;
}

esp_err_t digits_ring_test_init(void)
{
    if (audio.requests) return ESP_ERR_INVALID_STATE;
    if (!digits_audio_rx_channel()) return ESP_ERR_INVALID_STATE;
    audio.requests = xQueueCreate(1, sizeof(int64_t));
    if (!audio.requests || xTaskCreate(ring_worker, "digits_ring", 4096, NULL, 4, NULL) != pdPASS) {
        release_audio();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void digits_ring_test_request(void)
{
    if (!audio.requests) {
        ESP_LOGE("ring_test", "Ring Test unavailable: audio initialization failed");
        return;
    }
    int64_t requested_at = esp_timer_get_time();
    // Single pending command, no accumulating audio jobs and no callback wait.
    xQueueOverwrite(audio.requests, &requested_at);
}
