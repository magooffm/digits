#include "tx_voice.h"
#include "decimate.h"
#include "microphone.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_aec.h"
#include "esp_agc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#define TAG "tx_voice"
#define PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STEP (TX_VOICE_RATE / 100) // The gain takes 10 ms.
#define AEC_FILTER_LENGTH 4        // ESP-SR's recommended value.
// FD low cost: on 2026-10-28 it removed the echo about as well as the VoIP
// modes for half their CPU and internal RAM (11 KB instead of 26 KB).
#define AEC_MODE AEC_MODE_FD_LOW_COST
#define IN_FRAMES 4                // 48 kHz frames waiting for the worker.
#define OUT_SAMPLES 2048           // Processed 16 kHz samples, a power of two.
#define WORKER_STACK_BYTES 3072 // Measured: about 1 KB used.
#define WORKER_PRIORITY 5          // Like the audio workers, below the peer task (6).
#define WAIT_TICKS 6               // ms for a late worker before sending silence.

struct tx_voice {
    // Encoder to worker: mic and reference frames at 48 kHz.
    int16_t *in48;
    uint32_t in_head, in_tail;
    // Worker only.
    decimate_t mic_down, reference_down;
    aec_handle_t *aec;
    int chunk;                                  // AEC samples per call.
    int16_t *in_mic, *in_reference, *chunk_out; // 16-byte aligned for ESP-SR.
    size_t in_fill;
    // Worker to encoder: processed 16 kHz samples.
    int16_t *out;
    uint32_t out_head, out_tail;
    // Encoder only.
    void *agc;
    int applied_db;
    uint32_t slowest_us;  // Worker: longest echo cancellation of one chunk.
    uint32_t dropped, late, owed; // owed: samples replaced by silence, skipped later.
    TaskHandle_t worker;
    SemaphoreHandle_t stopped;
    bool stop;
};

#define NVS_NAMESPACE "digits_voice"
static int gain_db = CONFIG_DIGITS_VOICE_GAIN_DB;
static bool echo_cancel = true;

void tx_voice_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t value;
    if (nvs_get_u8(nvs, "gain_db", &value) == ESP_OK && value <= TX_VOICE_GAIN_MAX_DB) gain_db = value;
    nvs_close(nvs);
}

int tx_voice_gain_db(void)
{
    return __atomic_load_n(&gain_db, __ATOMIC_RELAXED);
}

esp_err_t tx_voice_set_gain_db(int db)
{
    if (db < 0 || db > TX_VOICE_GAIN_MAX_DB) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, "gain_db", (uint8_t)db);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) __atomic_store_n(&gain_db, db, __ATOMIC_RELAXED);
    return err;
}

bool tx_voice_echo_cancel(void)
{
    return __atomic_load_n(&echo_cancel, __ATOMIC_RELAXED);
}

void tx_voice_set_echo_cancel(bool on)
{
    __atomic_store_n(&echo_cancel, on, __ATOMIC_RELAXED);
}

static size_t gcd(size_t a, size_t b)
{
    while (b) {
        size_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

static void out_write(tx_voice_t *voice, const int16_t *samples, size_t count)
{
    uint32_t head = __atomic_load_n(&voice->out_head, __ATOMIC_RELAXED);
    for (size_t i = 0; i < count; ++i) voice->out[(head + i) % OUT_SAMPLES] = samples[i];
    __atomic_store_n(&voice->out_head, head + count, __ATOMIC_RELEASE);
}

static void worker(void *argument)
{
    tx_voice_t *voice = argument;
    const size_t frame = DIGITS_MIC_FRAME_SAMPLES, chunk = voice->chunk;
    while (!__atomic_load_n(&voice->stop, __ATOMIC_ACQUIRE)) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t tail = voice->in_tail;
        while (tail != __atomic_load_n(&voice->in_head, __ATOMIC_ACQUIRE)) {
            const int16_t *mic = voice->in48 + (tail % IN_FRAMES) * 2 * frame;
            decimate_process(&voice->mic_down, mic, frame, voice->in_mic + voice->in_fill);
            decimate_process(&voice->reference_down, mic + frame, frame, voice->in_reference + voice->in_fill);
            __atomic_store_n(&voice->in_tail, ++tail, __ATOMIC_RELEASE);
            voice->in_fill += TX_VOICE_SAMPLES;
            while (voice->in_fill >= chunk) {
                // Always fed, so it keeps tracking the room while switched off.
                int64_t started = esp_timer_get_time();
                aec_process(voice->aec, voice->in_mic, voice->in_reference, voice->chunk_out);
                uint32_t took = (uint32_t)(esp_timer_get_time() - started);
                if (took > voice->slowest_us) voice->slowest_us = took;
                out_write(voice, tx_voice_echo_cancel() ? voice->chunk_out : voice->in_mic, chunk);
                voice->in_fill -= chunk;
                memmove(voice->in_mic, voice->in_mic + chunk, voice->in_fill * sizeof(int16_t));
                memmove(voice->in_reference, voice->in_reference + chunk, voice->in_fill * sizeof(int16_t));
            }
        }
    }
    ESP_LOGI(TAG, "Worker stopped: stack minimum free %u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(voice->stopped);
    vTaskDeleteWithCaps(NULL);
}

tx_voice_t *tx_voice_create(void)
{
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    tx_voice_t *voice = heap_caps_calloc(1, sizeof(*voice), PSRAM);
    if (!voice) return NULL;
    decimate_init(&voice->mic_down);
    decimate_init(&voice->reference_down);
    aec_config_t config = {
        .mic_num = 1, .ref_num = 1, .out_num = 1, .filter_length = AEC_FILTER_LENGTH,
        .sample_rate = TX_VOICE_RATE, .caps = PSRAM, .mode = AEC_MODE, .nlp_level = AEC_NLP_LEVEL_NORMAL,
    };
    // ESP-SR allocates its small structures with malloc, which keeps blocks
    // under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL in internal RAM (11 KB here).
    // Internal RAM is what calls run short of: send them to PSRAM too.
    heap_caps_malloc_extmem_enable(16);
    voice->aec = aec_create_from_config(&config);
    voice->agc = esp_agc_open(AGC_MODE_3, TX_VOICE_RATE);
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    voice->stopped = xSemaphoreCreateBinary();
    if (!voice->aec || !voice->agc || !voice->stopped) goto fail;
    voice->applied_db = -1;
    voice->chunk = aec_get_chunksize(voice->aec);
    if (voice->chunk <= 0 || voice->chunk + TX_VOICE_SAMPLES > OUT_SAMPLES / 2) goto fail;
    size_t in_bytes = (voice->chunk + TX_VOICE_SAMPLES) * sizeof(int16_t);
    voice->in48 = heap_caps_malloc(IN_FRAMES * 2 * DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t), PSRAM);
    voice->in_mic = heap_caps_aligned_calloc(16, 1, in_bytes, PSRAM);
    voice->in_reference = heap_caps_aligned_calloc(16, 1, in_bytes, PSRAM);
    voice->chunk_out = heap_caps_aligned_calloc(16, 1, voice->chunk * sizeof(int16_t), PSRAM);
    voice->out = heap_caps_calloc(OUT_SAMPLES, sizeof(int16_t), PSRAM);
    if (!voice->in48 || !voice->in_mic || !voice->in_reference || !voice->chunk_out || !voice->out) goto fail;
    // Start with enough silence that each frame finds its samples: output
    // comes in whole chunks, and frame n is still being processed while the
    // encoder takes the samples of earlier frames.
    voice->out_head = voice->chunk - gcd(voice->chunk, TX_VOICE_SAMPLES) + TX_VOICE_SAMPLES;
    if (xTaskCreatePinnedToCoreWithCaps(worker, "digits_tx_voice", WORKER_STACK_BYTES, voice, WORKER_PRIORITY,
                                        &voice->worker, tskNO_AFFINITY, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) goto fail;
    ESP_LOGI(TAG, "Ready: %s echo canceller, chunk %d samples, delay %u ms, microphone gain %d dB; "
             "internal RAM used %d bytes",
             aec_get_mode_string(config.mode), voice->chunk, (unsigned)(voice->out_head * 1000 / TX_VOICE_RATE),
             tx_voice_gain_db(),
             (int)(internal_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    return voice;
fail:
    ESP_LOGE(TAG, "Cannot create the voice processing (aec=%p agc=%p)", voice->aec, voice->agc);
    tx_voice_destroy(voice);
    return NULL;
}

void tx_voice_destroy(tx_voice_t *voice)
{
    if (!voice) return;
    if (voice->worker) {
        __atomic_store_n(&voice->stop, true, __ATOMIC_RELEASE);
        xTaskNotifyGive(voice->worker);
        xSemaphoreTake(voice->stopped, portMAX_DELAY);
        ESP_LOGI(TAG, "Call ended: %u frames dropped (worker behind), %u sent as silence (worker late); "
                 "slowest chunk %u us", (unsigned)voice->dropped, (unsigned)voice->late, (unsigned)voice->slowest_us);
    }
    if (voice->aec) aec_destroy(voice->aec);
    if (voice->agc) esp_agc_close(voice->agc);
    if (voice->stopped) vSemaphoreDelete(voice->stopped);
    heap_caps_free(voice->in48);
    heap_caps_free(voice->in_mic);
    heap_caps_free(voice->in_reference);
    heap_caps_free(voice->chunk_out);
    heap_caps_free(voice->out);
    heap_caps_free(voice);
}

void tx_voice_process(tx_voice_t *voice, const int16_t *mic, const int16_t *reference, int16_t *out)
{
    const size_t frame = DIGITS_MIC_FRAME_SAMPLES;
    uint32_t head = voice->in_head;
    if (head - __atomic_load_n(&voice->in_tail, __ATOMIC_ACQUIRE) < IN_FRAMES) {
        int16_t *slot = voice->in48 + (head % IN_FRAMES) * 2 * frame;
        memcpy(slot, mic, frame * sizeof(int16_t));
        memcpy(slot + frame, reference, frame * sizeof(int16_t));
        __atomic_store_n(&voice->in_head, head + 1, __ATOMIC_RELEASE);
        xTaskNotifyGive(voice->worker);
    } else {
        ++voice->dropped;
    }
    uint32_t tail = voice->out_tail;
    if (voice->owed && __atomic_load_n(&voice->out_head, __ATOMIC_ACQUIRE) - tail >= voice->owed + TX_VOICE_SAMPLES) {
        tail += voice->owed; // Back to the planned delay.
        voice->owed = 0;
    }
    for (int wait = 0; __atomic_load_n(&voice->out_head, __ATOMIC_ACQUIRE) - tail < TX_VOICE_SAMPLES; ++wait) {
        if (wait == WAIT_TICKS) {
            ++voice->late;
            voice->owed += TX_VOICE_SAMPLES;
            voice->out_tail = tail;
            memset(out, 0, TX_VOICE_SAMPLES * sizeof(int16_t));
            return;
        }
        vTaskDelay(1);
    }
    for (size_t i = 0; i < TX_VOICE_SAMPLES; ++i) out[i] = voice->out[(tail + i) % OUT_SAMPLES];
    __atomic_store_n(&voice->out_tail, tail + TX_VOICE_SAMPLES, __ATOMIC_RELEASE);
    int db = tx_voice_gain_db();
    if (!db) return;
    if (db != voice->applied_db) {
        set_agc_config(voice->agc, db, 1, 3); // Limiter at -3 dBFS.
        voice->applied_db = db;
    }
    for (int at = 0; at < TX_VOICE_SAMPLES; at += STEP) {
        int16_t step[STEP];
        esp_agc_process(voice->agc, out + at, step, STEP, TX_VOICE_RATE);
        memcpy(out + at, step, sizeof(step));
    }
}

int tx_voice_console(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "aec") && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        tx_voice_set_echo_cancel(!strcmp(argv[2], "on"));
    } else if (argc == 3 && !strcmp(argv[1], "gain")) {
        char *end;
        long db = strtol(argv[2], &end, 10);
        if (*end || tx_voice_set_gain_db((int)db) != ESP_OK) {
            printf("Gain must be 0 to %d dB (saving failed otherwise)\n", TX_VOICE_GAIN_MAX_DB);
            return 1;
        }
    } else if (argc > 2 || (argc == 2 && strcmp(argv[1], "status"))) {
        printf("Usage: voice [status] | voice aec on|off | voice gain DB\n");
        return 1;
    }
    printf("Echo cancellation %s (until restart), microphone gain %d dB (saved)\n",
           tx_voice_echo_cancel() ? "on" : "off", tx_voice_gain_db());
    return 0;
}
