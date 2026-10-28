#include "greeting_record.h"
#include "audio_board.h"
#include "greeting_pcm.h"
#include "microphone.h"
#include "sd_ringtones.h"
#include "voicemail.h"
#include "webrtc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TAG "greeting"
#define PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MAX_SAMPLES (CONFIG_DIGITS_GREETING_RECORD_SECONDS * GREETING_PCM_RATE)
#define MIN_SAMPLES (GREETING_PCM_RATE)  // Under a second: nothing said.
#define BEEP_FRAMES (DIGITS_AUDIO_SAMPLE_RATE * 3 / 10)
#define BEEP_PEAK 6000
#define AFTER_BEEP_MS 200                // The speaker's tail must not be recorded.
#define TARGET_PEAK 26000                // About -2 dBFS.
#define MAX_GAIN 8                       // +18 dB at most.
#define STACK_BYTES (6 * 1024)           // PSRAM: SD card only, no flash writes.
#define TEMP_PATH "/sdcard/voicemail/greeting-new.tmp"

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static digits_greeting_record_status_t status = {.state = DIGITS_GREETING_RECORD_IDLE};
static bool stop_requested;

static void set_status(digits_greeting_record_state_t state, uint32_t tenths, const char *message)
{
    portENTER_CRITICAL(&mux);
    status.state = state;
    status.tenths = tenths;
    status.message = message;
    portEXIT_CRITICAL(&mux);
}

digits_greeting_record_status_t digits_greeting_record_status(void)
{
    portENTER_CRITICAL(&mux);
    digits_greeting_record_status_t copy = status;
    portEXIT_CRITICAL(&mux);
    return copy;
}

static bool line_idle(void)
{
    digits_webrtc_call_state_t call;
    digits_webrtc_call_state(&call);
    return call.phase == DIGITS_CALL_IDLE;
}

static void beep(void)
{
    if (!digits_audio_speaker_acquire(100)) return; // Recording still starts.
    int16_t stereo[DIGITS_AUDIO_DMA_FRAMES * 2];
    if (digits_audio_speaker_start() == ESP_OK) {
        for (unsigned done = 0; done < BEEP_FRAMES; done += DIGITS_AUDIO_DMA_FRAMES) {
            for (unsigned i = 0; i < DIGITS_AUDIO_DMA_FRAMES; ++i) {
                unsigned at = done + i, edge = at < BEEP_FRAMES - at ? at : BEEP_FRAMES - at;
                float fade = edge < 240 ? edge / 240.0f : 1.0f;
                int16_t v = (int16_t)(BEEP_PEAK * fade * sinf(2 * 3.14159265f * 1000 * at / DIGITS_AUDIO_SAMPLE_RATE));
                stereo[2 * i] = stereo[2 * i + 1] = at < BEEP_FRAMES ? v : 0;
            }
            size_t written;
            digits_audio_speaker_write(stereo, sizeof(stereo), &written, 50);
        }
        digits_audio_speaker_silence();
    }
    digits_audio_speaker_release();
}

static bool write_wav(const int16_t *pcm, size_t samples)
{
    uint8_t header[GREETING_PCM_HEADER_BYTES];
    greeting_pcm_wav_header(header, GREETING_PCM_RATE, (uint32_t)samples);
    FILE *file = fopen(TEMP_PATH, "wb");
    bool ok = file && fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
              fwrite(pcm, sizeof(int16_t), samples, file) == samples;
    if (file && fclose(file)) ok = false;
    unlink(DIGITS_GREETING_NEW_PATH);
    if (ok && rename(TEMP_PATH, DIGITS_GREETING_NEW_PATH)) ok = false;
    if (!ok) unlink(TEMP_PATH);
    return ok;
}

static void record_task(void *arg)
{
    (void)arg;
    int16_t *pcm = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), PSRAM);
    digits_microphone_frame_t *frame = heap_caps_malloc(sizeof(*frame), PSRAM);
    const char *failure = NULL;
    size_t samples = 0;
    if (!pcm || !frame) failure = "not enough memory";
    if (!failure) {
        beep();
        vTaskDelay(pdMS_TO_TICKS(AFTER_BEEP_MS));
        if (!digits_microphone_acquire(500)) failure = "the microphone is busy";
    }
    if (!failure) {
        set_status(DIGITS_GREETING_RECORD_RECORDING, 0, NULL);
        ESP_LOGI(TAG, "Recording a greeting (up to %u s)", (unsigned)CONFIG_DIGITS_GREETING_RECORD_SECONDS);
        while (samples + DIGITS_MIC_FRAME_SAMPLES / 3 <= MAX_SAMPLES) {
            portENTER_CRITICAL(&mux);
            bool stop = stop_requested;
            portEXIT_CRITICAL(&mux);
            if (stop) break;
            if (!line_idle()) {
                failure = "a call came in";
                break;
            }
            if (digits_microphone_read(frame, 100) != ESP_OK) continue;
            samples += greeting_pcm_downsample(frame->pcm, DIGITS_MIC_FRAME_SAMPLES, pcm + samples);
            set_status(DIGITS_GREETING_RECORD_RECORDING, (uint32_t)(samples * 10 / GREETING_PCM_RATE), NULL);
        }
        digits_microphone_release();
    }
    if (!failure && samples < MIN_SAMPLES) failure = "too short";
    if (!failure) {
        unsigned gain = greeting_pcm_normalize(pcm, samples, TARGET_PEAK, MAX_GAIN);
        ESP_LOGI(TAG, "Recorded %u.%u s, level x%u.%02u", (unsigned)(samples / GREETING_PCM_RATE),
                 (unsigned)(samples % GREETING_PCM_RATE * 10 / GREETING_PCM_RATE), gain / 16, gain % 16 * 100 / 16);
        if (!write_wav(pcm, samples)) failure = "writing to the SD card failed";
    }
    free(pcm);
    free(frame);
    if (failure) {
        ESP_LOGW(TAG, "Greeting recording failed: %s", failure);
        set_status(DIGITS_GREETING_RECORD_FAILED, 0, failure);
    } else {
        set_status(DIGITS_GREETING_RECORD_READY, (uint32_t)(samples * 10 / GREETING_PCM_RATE), NULL);
    }
    vTaskDeleteWithCaps(NULL);
}

esp_err_t digits_greeting_record_start(void)
{
    if (!digits_voicemail_ready()) return ESP_ERR_NOT_SUPPORTED;
    if (!line_idle()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&mux);
    bool busy = status.state == DIGITS_GREETING_RECORD_STARTING || status.state == DIGITS_GREETING_RECORD_RECORDING;
    if (!busy) {
        status = (digits_greeting_record_status_t){.state = DIGITS_GREETING_RECORD_STARTING};
        stop_requested = false;
    }
    portEXIT_CRITICAL(&mux);
    if (busy) return ESP_ERR_INVALID_STATE;
    digits_voicemail_stop(); // The beep needs the speaker.
    if (xTaskCreateWithCaps(record_task, "digits_greeting", STACK_BYTES, NULL, 4, NULL, PSRAM) != pdPASS) {
        set_status(DIGITS_GREETING_RECORD_FAILED, 0, "not enough memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void digits_greeting_record_stop(void)
{
    portENTER_CRITICAL(&mux);
    stop_requested = true;
    portEXIT_CRITICAL(&mux);
}

esp_err_t digits_greeting_record_keep(void)
{
    if (digits_greeting_record_status().state != DIGITS_GREETING_RECORD_READY) return ESP_ERR_INVALID_STATE;
    if (!line_idle()) return ESP_ERR_INVALID_STATE;
    unlink(DIGITS_VOICEMAIL_GREETING_WAV);
    if (rename(DIGITS_GREETING_NEW_PATH, DIGITS_VOICEMAIL_GREETING_WAV)) return ESP_FAIL;
    unlink(DIGITS_SD_GREETING_PATH); // One greeting: the recording replaces an uploaded one.
    digits_sd_greeting_reload();
    set_status(DIGITS_GREETING_RECORD_IDLE, 0, NULL);
    return digits_voicemail_greeting_reload();
}

void digits_greeting_record_discard(void)
{
    digits_greeting_record_stop();
    portENTER_CRITICAL(&mux);
    bool running = status.state == DIGITS_GREETING_RECORD_STARTING || status.state == DIGITS_GREETING_RECORD_RECORDING;
    portEXIT_CRITICAL(&mux);
    if (running) return; // Stopping; discard once it is ready.
    unlink(DIGITS_GREETING_NEW_PATH);
    set_status(DIGITS_GREETING_RECORD_IDLE, 0, NULL);
}
