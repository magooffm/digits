#include "call_tones.h"
#include "audio_board.h"
#include "ring_tone.h"
#include "ringtones.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "call_tones"
#define BLOCK_FRAMES DIGITS_AUDIO_DMA_FRAMES
#define WRITE_TIMEOUT_MS 20U
#define RETRY_MS 20U
#define WORKER_STACK_BYTES 6144
#define WORKER_PRIORITY 4

static const char *const tone_names[] = {"none", "dial", "ringback", "busy", "reorder"};

static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
#define NO_PREVIEW (-1)

static struct {
    tone_gen_tone_t tone;
    int preview; // Ringtone being previewed, or NO_PREVIEW.
    uint32_t revision;
    bool stopped;
} state = {.preview = NO_PREVIEW, .stopped = true};

// Callers hold state_mux.
static void bump_revision(void)
{
    ++state.revision;
    if (!state.revision) ++state.revision; // Never collide with failed_revision 0.
}

static uint32_t preview_frames(int ringtone)
{
    return ringtone == RINGTONE_STANDARD ? 2 * RING_TONE_SAMPLE_RATE
                                         : ringtones_melody_frames((unsigned)ringtone);
}

static void render_preview(int ringtone, uint64_t position, int16_t *pcm)
{
    if (ringtone == RINGTONE_STANDARD) {
        ring_tone_render(pcm, (size_t)(position % RING_TONE_FRAMES), BLOCK_FRAMES);
        for (size_t i = 0; i < BLOCK_FRAMES * 2; ++i) pcm[i] /= 2; // Incoming ring level.
    } else {
        ringtones_render((unsigned)ringtone, (uint32_t)position, pcm, BLOCK_FRAMES);
    }
}
static TaskHandle_t worker;

static TickType_t wait_ticks(uint32_t milliseconds)
{
    // Round up so a low tick rate never turns a retry into a spin.
    return (TickType_t)(((uint64_t)milliseconds * configTICK_RATE_HZ + 999) / 1000);
}

static bool current(uint32_t revision)
{
    portENTER_CRITICAL(&state_mux);
    bool same = state.revision == revision;
    portEXIT_CRITICAL(&state_mux);
    return same;
}

// Only acknowledge for the revision this decision was made on; a newer
// request keeps stopped false until the worker has looked at it.
static void acknowledge_stopped(uint32_t revision)
{
    portENTER_CRITICAL(&state_mux);
    if (state.revision == revision) state.stopped = true;
    portEXIT_CRITICAL(&state_mux);
}

static void release_speaker(void)
{
    if (!digits_audio_speaker_silence())
        ESP_LOGE(TAG, "Amplifier disable failed; DAC mute and silence requested");
    digits_audio_speaker_release();
}

static void tone_worker(void *arg)
{
    (void)arg;
    static int16_t pcm[BLOCK_FRAMES * 2];
    bool owned = false;
    uint32_t owned_revision = 0, failed_revision = 0;
    uint64_t position = 0;
    for (;;) {
        portENTER_CRITICAL(&state_mux);
        tone_gen_tone_t tone = state.tone;
        int preview = state.preview;
        uint32_t revision = state.revision;
        portEXIT_CRITICAL(&state_mux);

        if ((tone == TONE_GEN_NONE && preview == NO_PREVIEW) || revision == failed_revision) {
            if (owned) {
                release_speaker();
                owned = false;
                continue; // Cleanup can block: decide again on fresh state.
            }
            acknowledge_stopped(revision);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (!owned) {
            // Withdraw "stopped" before acquiring, never after: call audio
            // must not see stopped while this worker already holds the speaker.
            portENTER_CRITICAL(&state_mux);
            bool go = state.revision == revision;
            if (go) state.stopped = false;
            portEXIT_CRITICAL(&state_mux);
            if (!go) continue;
            if (!digits_audio_speaker_acquire(0)) {
                // Ring Test, recording or call audio holds the speaker. Owning
                // nothing, report stopped so call audio is never held up.
                acknowledge_stopped(revision);
                ulTaskNotifyTake(pdTRUE, wait_ticks(RETRY_MS));
                continue;
            }
            owned = true;
            owned_revision = revision;
            position = 0;
            esp_err_t err = digits_audio_speaker_start();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Speaker start failed: %s", esp_err_to_name(err));
                failed_revision = revision;
                continue;
            }
        }
        if (revision != owned_revision) {
            // A different tone while holding the speaker: restart its cadence.
            owned_revision = revision;
            position = 0;
            portENTER_CRITICAL(&state_mux);
            if (state.revision == revision) state.stopped = false;
            portEXIT_CRITICAL(&state_mux);
        }
        if (preview != NO_PREVIEW) {
            if (position >= preview_frames(preview)) {
                // One cycle done: end the preview unless something newer arrived.
                portENTER_CRITICAL(&state_mux);
                if (state.revision == revision) {
                    state.preview = NO_PREVIEW;
                    bump_revision();
                }
                portEXIT_CRITICAL(&state_mux);
                continue;
            }
            render_preview(preview, position, pcm);
        } else {
            tone_gen_render(tone, position, pcm, BLOCK_FRAMES);
        }
        if (!current(revision)) continue;
        size_t written = 0;
        esp_err_t err = digits_audio_speaker_write(pcm, sizeof(pcm), &written, WRITE_TIMEOUT_MS);
        if (err != ESP_OK || written != sizeof(pcm)) {
            ESP_LOGE(TAG, "Tone write failed: %s (%u bytes)", esp_err_to_name(err), (unsigned)written);
            failed_revision = revision;
            continue;
        }
        position += BLOCK_FRAMES;
    }
}

esp_err_t digits_call_tones_init(void)
{
    if (worker) return ESP_ERR_INVALID_STATE;
    if (!digits_audio_rx_channel()) return ESP_ERR_INVALID_STATE;
    tone_gen_init();
    // Shared tables; the incoming ringer normally built them already.
    if (!ringtones_init()) ESP_LOGE(TAG, "Ringtone table invalid; previews may be silent");
    if (xTaskCreate(tone_worker, "digits_tones", WORKER_STACK_BYTES, NULL, WORKER_PRIORITY,
                    &worker) != pdPASS) {
        worker = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void digits_call_tones_play(tone_gen_tone_t tone)
{
    if (tone < TONE_GEN_NONE || tone > TONE_GEN_REORDER) tone = TONE_GEN_NONE;
    portENTER_CRITICAL(&state_mux);
    bool changed = state.tone != tone;
    if (changed) {
        state.tone = tone;
        state.preview = NO_PREVIEW;
        bump_revision();
        // Without a worker nothing can own the speaker: stay stopped.
        if (tone != TONE_GEN_NONE && worker) state.stopped = false;
    }
    portEXIT_CRITICAL(&state_mux);
    if (!changed) return;
    ESP_LOGI(TAG, "Tone: %s", tone_names[tone]);
    if (worker) xTaskNotifyGive(worker);
}

void digits_call_tones_preview(unsigned ringtone)
{
    if (!ringtones_valid(ringtone)) return;
    portENTER_CRITICAL(&state_mux);
    state.tone = TONE_GEN_NONE;
    state.preview = (int)ringtone;
    bump_revision();
    if (worker) state.stopped = false;
    portEXIT_CRITICAL(&state_mux);
    ESP_LOGI(TAG, "Preview: ringtone %u (%s)", ringtone, ringtones_name(ringtone));
    if (worker) xTaskNotifyGive(worker);
}

void digits_call_tones_stop(void)
{
    portENTER_CRITICAL(&state_mux);
    bool changed = state.tone != TONE_GEN_NONE || state.preview != NO_PREVIEW;
    if (changed) {
        state.tone = TONE_GEN_NONE;
        state.preview = NO_PREVIEW;
        bump_revision();
    }
    portEXIT_CRITICAL(&state_mux);
    if (changed && worker) xTaskNotifyGive(worker);
}

bool digits_call_tones_is_stopped(void)
{
    portENTER_CRITICAL(&state_mux);
    bool stopped = state.stopped;
    portEXIT_CRITICAL(&state_mux);
    return stopped;
}
