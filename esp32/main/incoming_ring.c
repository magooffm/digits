#include "incoming_ring.h"
#include "audio_board.h"
#include "ring_tone.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RING_ON_US 2000000LL
#define RING_PERIOD_US 6000000LL
#define RING_RETRY_MS 20U
#define RING_WRITE_MS 20U
#define RING_WORKER_STACK_BYTES 8192
#define PCM_BLOCK_FRAMES DIGITS_AUDIO_DMA_FRAMES

typedef struct {
    uint32_t call_id;
    uint32_t revision;
    int64_t started_us;
    bool active;
    bool silent;
    bool latched;
    bool stopped;
} ring_state_t;

static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
static ring_state_t state = {.stopped = true};
static TaskHandle_t worker;

static ring_state_t snapshot(void)
{
    portENTER_CRITICAL(&state_mux);
    ring_state_t copy = state;
    portEXIT_CRITICAL(&state_mux);
    return copy;
}

static bool still_audible(uint32_t revision)
{
    portENTER_CRITICAL(&state_mux);
    bool audible = state.revision == revision && state.active && !state.latched;
    portEXIT_CRITICAL(&state_mux);
    return audible;
}

static TickType_t wait_ticks(uint32_t milliseconds)
{
    // Round up: a low tick rate must never turn contention retries into a spin.
    return (TickType_t)(((uint64_t)milliseconds * configTICK_RATE_HZ + 999) / 1000);
}

static void acknowledge_release(void)
{
    portENTER_CRITICAL(&state_mux);
    state.stopped = true;
    portEXIT_CRITICAL(&state_mux);
}

static void release_speaker(void)
{
    // Only this worker's reservation authorizes muting/flush. The board gates
    // the amplifier before its three bounded 50 ms silence writes; signaling
    // never waits here. A failed gate is logged, not claimed as physical mute.
    if (!digits_audio_speaker_silence())
        ESP_LOGE("incoming_ring", "Amplifier disable failed; DAC mute and silence requested");
    digits_audio_speaker_release();
    acknowledge_release();
}

static void ring_worker(void *arg)
{
    (void)arg;
    static int16_t pcm[PCM_BLOCK_FRAMES * 2];
    bool owned = false;
    uint32_t owned_revision = 0, failed_revision = 0;
    size_t tone_frame = 0;
    for (;;) {
        ring_state_t current = snapshot();
        int64_t elapsed = esp_timer_get_time() - current.started_us;
        int64_t phase = elapsed >= 0 ? elapsed % RING_PERIOD_US : 0;
        bool enabled = current.active && !current.latched;
        bool on = enabled && phase < RING_ON_US && current.revision != failed_revision;

        if (owned && (!on || current.revision != owned_revision)) {
            release_speaker();
            owned = false;
            // Cleanup can block while a newer command arrives. Always take a
            // fresh snapshot before waiting or touching the speaker again.
            continue;
        }
        if (!on) {
            // No acquire is in flight. A stale snapshot cannot acknowledge a
            // newer start and then sleep through it; its notification remains.
            portENTER_CRITICAL(&state_mux);
            if (state.revision == current.revision) state.stopped = true;
            portEXIT_CRITICAL(&state_mux);
            TickType_t delay = portMAX_DELAY;
            if (enabled && current.revision != failed_revision)
                delay = wait_ticks((uint32_t)((RING_PERIOD_US - phase + 999) / 1000));
            ulTaskNotifyTake(pdTRUE, delay);
            continue;
        }

        if (!owned) {
            // Reserve the acknowledgment before leaving the critical section.
            // stop()+is_stopped() can never race an old snapshot into a later
            // acquire after reporting completion.
            portENTER_CRITICAL(&state_mux);
            bool acquire = state.revision == current.revision && state.active && !state.latched;
            if (acquire) state.stopped = false;
            portEXIT_CRITICAL(&state_mux);
            if (!acquire) continue;
            if (!digits_audio_speaker_acquire(0)) {
                acknowledge_release();
                ulTaskNotifyTake(pdTRUE, wait_ticks(RING_RETRY_MS));
                continue;
            }
            owned = true;
            owned_revision = current.revision;
            tone_frame = 0;
            // stop/silent/new call can arrive even during a zero-time acquire.
            if (!still_audible(owned_revision)) continue;
            esp_err_t error = digits_audio_speaker_start();
            if (error != ESP_OK) {
                ESP_LOGE("incoming_ring", "Speaker start failed: %s", esp_err_to_name(error));
                failed_revision = owned_revision;
                continue;
            }
        }
        if (!still_audible(owned_revision)) continue;
        // ring_tone_render deliberately stops after one second. Wrap the
        // source cursor to repeat it through the two-second cadence window.
        ring_tone_render(pcm, tone_frame, PCM_BLOCK_FRAMES);
        for (size_t i = 0; i < PCM_BLOCK_FRAMES * 2; ++i) pcm[i] /= 2;
        if (!still_audible(owned_revision)) continue;
        size_t written = 0;
        esp_err_t error = digits_audio_speaker_write(pcm, sizeof(pcm), &written, RING_WRITE_MS);
        if (error != ESP_OK || written != sizeof(pcm)) {
            ESP_LOGE("incoming_ring", "Playback write failed: %s (%u bytes)",
                     esp_err_to_name(error), (unsigned)written);
            failed_revision = owned_revision;
        }
        tone_frame = (tone_frame + PCM_BLOCK_FRAMES) % RING_TONE_FRAMES;
    }
}

esp_err_t digits_incoming_ring_init(void)
{
    if (worker) return ESP_ERR_INVALID_STATE;
    if (!digits_audio_rx_channel()) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(ring_worker, "digits_in_ring", RING_WORKER_STACK_BYTES,
                    NULL, 4, &worker) != pdPASS) {
        worker = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void digits_incoming_ring_start(uint32_t call_id)
{
    if (!call_id) return;
    portENTER_CRITICAL(&state_mux);
    bool duplicate = state.active && state.call_id == call_id;
    if (!duplicate) {
        state.call_id = call_id;
        ++state.revision;
        // Avoid the worker's initial failed_revision sentinel on wraparound.
        if (!state.revision) ++state.revision;
        state.started_us = esp_timer_get_time();
        state.active = true;
        state.latched = state.silent;
        state.stopped = worker == NULL;
    }
    bool silent = state.latched;
    portEXIT_CRITICAL(&state_mux);
    if (duplicate) return;
    ESP_LOGI("incoming_ring", "Incoming call %u: %s", (unsigned)call_id,
             silent ? "silent ringing" : "speaker ringing requested");
    if (worker) xTaskNotifyGive(worker);
    else ESP_LOGW("incoming_ring", "Ringtone unavailable; incoming indication retained");
}

void digits_incoming_ring_stop(void)
{
    portENTER_CRITICAL(&state_mux);
    if (state.active) {
        state.active = false;
        ++state.revision;
        if (!state.revision) ++state.revision;
    }
    portEXIT_CRITICAL(&state_mux);
    if (worker) xTaskNotifyGive(worker);
}

void digits_incoming_ring_set_silent(bool silent)
{
    portENTER_CRITICAL(&state_mux);
    state.silent = silent;
    if (silent && state.active && !state.latched) {
        state.latched = true;
        ++state.revision;
        if (!state.revision) ++state.revision;
    }
    // Disabling silent mode does not un-latch this call.
    portEXIT_CRITICAL(&state_mux);
    if (worker) xTaskNotifyGive(worker);
}

bool digits_incoming_ring_is_stopped(void)
{
    return snapshot().stopped;
}
