#include "call_audio.h"
#include "audio_board.h"
#include "decimate.h"
#include "microphone.h"
#include "tx_voice.h"
#include "voice_style.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TX_QUEUE_LENGTH 3
#define RX_QUEUE_LENGTH 8
// esp_audio_codec 2.6.2 README's ESP32-S3 guidance excludes task stacks from
// codec heap figures: about 40 KiB for encoding, 20 KiB for decoding. With
// 48/24 KiB, full calls measured 27,136/16,012 bytes never used (README), so
// about 22/8.5 KiB in use. These sizes keep a third or more spare; the 32 KiB
// difference let the internal heap fit both stacks again once more features
// were running (2026-10-27). Opus at 16 kHz with echo cancellation (tx_voice.h)
// measured 20.7 KiB in use (2026-10-28): 28 KiB keeps the third spare.
#define ENCODER_WORKER_STACK_BYTES (28U * 1024U)
#define DECODER_WORKER_STACK_BYTES (16U * 1024U)
#define FRAME_US (CALL_AUDIO_FRAME_MS * 1000LL)
#define CAPTURE_MAX_AGE_US 60000LL
#define WRITE_TIMEOUT_MS 30
#define DMA_WINDOW_US (2LL * DIGITS_AUDIO_DMA_FRAMES * 1000000 / DIGITS_AUDIO_SAMPLE_RATE)
#define PLAYBACK_FEED_AHEAD_US 10000LL
#define MONITOR_FRAMES 6 // Greeting frames on their way from the encoder to the speaker.

struct digits_call_audio {
    portMUX_TYPE lock;
    digits_call_audio_stats_t stats;
    digits_call_audio_mode_t mode;
    digits_call_audio_unattended_t hooks;
    bool unattended;
    // Greeting for the speaker: the encoder writes, the playback worker reads.
    int16_t *monitor;
    uint32_t monitor_head, monitor_tail;
    bool copper;                 // Wanted: set by the peer worker.
    voice_style_copper_t filter; // Encoder task only.
    bool copper_active;
    decimate_t *greeting_down;   // The greeting to 16 kHz for Opus.
    int16_t *tx_pcm;             // What Opus encodes: TX_VOICE_SAMPLES.
    bool stop_requested, tx_ready, rx_ready, tx_done, rx_done;
    QueueHandle_t tx_packets, rx_packets;
    StaticQueue_t tx_queue_control, rx_queue_control;
    uint8_t *tx_queue_storage, *rx_queue_storage;
    digits_microphone_frame_t *capture;
    call_audio_packet_t *encoder_packet, *receive_packet, *playback_packet;
    call_audio_jitter_t *jitter;
    int16_t *mono, *stereo;
    int64_t next_pop_us, last_pop_us;
};

static void *audio_calloc(size_t size)
{
    void *memory = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory ? memory : heap_caps_calloc(1, size, MALLOC_CAP_8BIT);
}

static bool stopping(digits_call_audio_t *audio)
{
    portENTER_CRITICAL(&audio->lock);
    bool stop = audio->stop_requested;
    portEXIT_CRITICAL(&audio->lock);
    return stop;
}

static bool unattended(digits_call_audio_t *audio)
{
    portENTER_CRITICAL(&audio->lock);
    bool value = audio->unattended;
    portEXIT_CRITICAL(&audio->lock);
    return value;
}

static bool greeting_done(digits_call_audio_t *audio)
{
    return !audio->hooks.greeting_done || audio->hooks.greeting_done();
}

// Single producer (encoder), single consumer (playback): no lock needed.
static void monitor_push(digits_call_audio_t *audio, const int16_t *mono)
{
    uint32_t head = __atomic_load_n(&audio->monitor_head, __ATOMIC_RELAXED);
    if (head - __atomic_load_n(&audio->monitor_tail, __ATOMIC_ACQUIRE) >= MONITOR_FRAMES) return; // Full: drop.
    memcpy(audio->monitor + (head % MONITOR_FRAMES) * DIGITS_MIC_FRAME_SAMPLES, mono,
           DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t));
    __atomic_store_n(&audio->monitor_head, head + 1, __ATOMIC_RELEASE);
}

static bool monitor_pop(digits_call_audio_t *audio, int16_t *mono)
{
    uint32_t tail = __atomic_load_n(&audio->monitor_tail, __ATOMIC_RELAXED);
    if (tail == __atomic_load_n(&audio->monitor_head, __ATOMIC_ACQUIRE)) return false;
    memcpy(mono, audio->monitor + (tail % MONITOR_FRAMES) * DIGITS_MIC_FRAME_SAMPLES,
           DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t));
    __atomic_store_n(&audio->monitor_tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}

static void failure(digits_call_audio_t *audio, esp_err_t error)
{
    portENTER_CRITICAL(&audio->lock);
    if (audio->stats.error == ESP_OK) audio->stats.error = error;
    audio->stop_requested = true;
    portEXIT_CRITICAL(&audio->lock);
}

static void maximum(uint32_t *value, int64_t measured)
{
    if (measured > 0 && (uint64_t)measured > *value)
        *value = measured > UINT32_MAX ? UINT32_MAX : (uint32_t)measured;
}

static void encoder_worker(void *argument)
{
    digits_call_audio_t *audio = argument;
    void *encoder = NULL;
    tx_voice_t *voice = NULL;
    bool microphone_owned = digits_microphone_acquire(0);
    if (!microphone_owned) {
        failure(audio, ESP_ERR_INVALID_STATE);
        goto finished;
    }
    // Echo cancellation runs at 16 kHz only; Opus then encodes wideband,
    // cheaper than 48 kHz. The remote decodes at 48 kHz as before.
    voice = tx_voice_create();
    if (!voice) {
        failure(audio, ESP_ERR_NO_MEM);
        goto finished;
    }
    esp_opus_enc_config_t configuration = ESP_OPUS_ENC_CONFIG_DEFAULT();
    configuration.sample_rate = TX_VOICE_RATE;
    configuration.channel = 1;
    configuration.bits_per_sample = 16;
    configuration.bitrate = 32000;
    configuration.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    // Audio, not VoIP: at 16 kHz the VoIP mode's SILK coder took 8-9 ms per
    // frame on average, audio mode 4-5 ms (2026-10-28); with echo
    // cancellation running too, VoIP left the CPU short.
    configuration.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    configuration.complexity = 0;
    configuration.enable_dtx = false;
    configuration.enable_fec = false;
    configuration.enable_vbr = false;
    esp_audio_err_t error = esp_opus_enc_open(&configuration, sizeof(configuration), &encoder);
    int input_size = 0, output_size = 0;
    if (error != ESP_AUDIO_ERR_OK ||
        esp_opus_enc_get_frame_size(encoder, &input_size, &output_size) != ESP_AUDIO_ERR_OK ||
        input_size != (int)(TX_VOICE_SAMPLES * sizeof(int16_t)) || output_size <= 0 ||
        output_size > (int)CALL_AUDIO_OPUS_MAX_BYTES) {
        ESP_LOGE("call_audio", "Cannot create 16 kHz mono 20 ms Opus encoder (error=%d, input=%d output=%d)",
                 error, input_size, output_size);
        failure(audio, error == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL);
        goto finished;
    }
    unsigned opened_stack_minimum = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("call_audio", "Opus encoder opened: stack=%u minimum_free=%u bytes; 16000 Hz mono 20 ms 32000 bps",
             (unsigned)ENCODER_WORKER_STACK_BYTES, opened_stack_minimum);
    portENTER_CRITICAL(&audio->lock);
    audio->stats.tx_stack_min_bytes = opened_stack_minimum;
    audio->tx_ready = true;
    portEXIT_CRITICAL(&audio->lock);
    bool have_sequence = false;
    uint32_t first_sequence = 0, previous_sequence = 0;
    unsigned capture_timeouts = 0;
    int64_t stack_report_at = 0;
    bool encode_failure_logged = false;
    while (!stopping(audio)) {
        if (digits_microphone_read(audio->capture, 40) != ESP_OK) {
            portENTER_CRITICAL(&audio->lock);
            ++audio->stats.capture_timeouts;
            portEXIT_CRITICAL(&audio->lock);
            if (++capture_timeouts >= 25 && !stopping(audio)) failure(audio, ESP_ERR_TIMEOUT);
            continue;
        }
        capture_timeouts = 0;
        // Capture and networking schedules are independent. If a consumer was
        // delayed, retain only current PCM and count every skipped sequence.
        for (unsigned i = 1; i < 4; ++i)
            if (digits_microphone_read(audio->capture, 0) != ESP_OK) break;
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&audio->lock);
        ++audio->stats.capture_frames;
        if (have_sequence) {
            uint32_t gap = audio->capture->sequence - previous_sequence;
            if (gap > 1) audio->stats.capture_gaps += gap - 1;
        }
        if (audio->capture->flags & DIGITS_MIC_FRAME_DISCONTINUITY)
            ++audio->stats.capture_discontinuities;
        maximum(&audio->stats.max_capture_age_us, now - audio->capture->captured_at_us);
        portEXIT_CRITICAL(&audio->lock);
        if (!have_sequence) {
            first_sequence = audio->capture->sequence;
            have_sequence = true;
        }
        previous_sequence = audio->capture->sequence;
        if (now - audio->capture->captured_at_us >= CAPTURE_MAX_AGE_US) {
            portENTER_CRITICAL(&audio->lock);
            ++audio->stats.capture_stale;
            portEXIT_CRITICAL(&audio->lock);
            continue;
        }
        // Capture keeps running for timing; its PCM is replaced, never sent.
        int64_t voice_at = esp_timer_get_time();
        if (unattended(audio)) {
            audio->hooks.fill(audio->capture->pcm, DIGITS_MIC_FRAME_SAMPLES);
            if (audio->monitor && !greeting_done(audio)) monitor_push(audio, audio->capture->pcm);
            decimate_process(audio->greeting_down, audio->capture->pcm, DIGITS_MIC_FRAME_SAMPLES, audio->tx_pcm);
        } else {
            tx_voice_process(voice, audio->capture->pcm, audio->capture->reference, audio->tx_pcm);
            if (__atomic_load_n(&audio->copper, __ATOMIC_RELAXED)) {
                // Copper Wire: the voice band only, like the Pi. A fresh start
                // each time it is chosen; the greeting above is never filtered.
                if (!audio->copper_active) voice_style_copper_reset(&audio->filter);
                audio->copper_active = true;
                voice_style_copper_process(&audio->filter, audio->tx_pcm, TX_VOICE_SAMPLES);
            } else {
                audio->copper_active = false;
            }
        }
        esp_audio_enc_in_frame_t input = {
            .buffer = (uint8_t *)audio->tx_pcm, .len = TX_VOICE_SAMPLES * sizeof(int16_t),
        };
        esp_audio_enc_out_frame_t output = {
            .buffer = audio->encoder_packet->data, .len = CALL_AUDIO_OPUS_MAX_BYTES,
        };
        int64_t encode_at = esp_timer_get_time();
        error = esp_opus_enc_process(encoder, &input, &output);
        int64_t encoded_at = esp_timer_get_time();
        portENTER_CRITICAL(&audio->lock);
        maximum(&audio->stats.max_voice_us, encode_at - voice_at);
        maximum(&audio->stats.max_encode_us, encoded_at - encode_at);
        if (error != ESP_AUDIO_ERR_OK || !output.encoded_bytes ||
            output.encoded_bytes > CALL_AUDIO_OPUS_MAX_BYTES) ++audio->stats.encode_errors;
        else ++audio->stats.encoded_frames;
        portEXIT_CRITICAL(&audio->lock);
        if (error != ESP_AUDIO_ERR_OK || !output.encoded_bytes ||
            output.encoded_bytes > CALL_AUDIO_OPUS_MAX_BYTES) {
            if (!encode_failure_logged) {
                ESP_LOGE("call_audio", "First encode failure: error=%d input=%u encoded=%u bytes",
                         error, (unsigned)input.len, (unsigned)output.encoded_bytes);
                encode_failure_logged = true;
            }
            continue;
        }
        audio->encoder_packet->pts_ms = (audio->capture->sequence - first_sequence) * CALL_AUDIO_FRAME_MS;
        audio->encoder_packet->captured_at_us = audio->capture->captured_at_us;
        audio->encoder_packet->size = output.encoded_bytes;
        if (xQueueSend(audio->tx_packets, audio->encoder_packet, 0) != pdTRUE) {
            // One producer and one consumer. Removing one oldest item is safe
            // even when the peer concurrently drains the queue.
            call_audio_packet_t discarded;
            if (xQueueReceive(audio->tx_packets, &discarded, 0) == pdTRUE) {
                portENTER_CRITICAL(&audio->lock);
                ++audio->stats.tx_queue_drops;
                portEXIT_CRITICAL(&audio->lock);
            }
            if (xQueueSend(audio->tx_packets, audio->encoder_packet, 0) != pdTRUE) {
                portENTER_CRITICAL(&audio->lock);
                ++audio->stats.tx_queue_drops;
                portEXIT_CRITICAL(&audio->lock);
            }
        }
        if (encoded_at >= stack_report_at) {
            unsigned minimum = uxTaskGetStackHighWaterMark(NULL);
            portENTER_CRITICAL(&audio->lock);
            audio->stats.tx_stack_min_bytes = minimum;
            portEXIT_CRITICAL(&audio->lock);
            stack_report_at = encoded_at + 1000000;
        }
    }
finished:
    if (encoder) esp_opus_enc_close(encoder);
    tx_voice_destroy(voice);
    if (microphone_owned) digits_microphone_release();
    unsigned minimum = uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&audio->lock);
    audio->stats.tx_stack_min_bytes = minimum;
    audio->tx_done = true;
    portEXIT_CRITICAL(&audio->lock);
    // No access to audio after this acknowledgement; owner may now free it.
    vTaskDelete(NULL);
}

static void copy_jitter_stats(digits_call_audio_t *audio)
{
    portENTER_CRITICAL(&audio->lock);
    audio->stats.rx_late = audio->jitter->late;
    audio->stats.rx_duplicate = audio->jitter->duplicate;
    audio->stats.rx_missing = audio->jitter->missing;
    audio->stats.rx_discontinuities = audio->jitter->discontinuities;
    audio->stats.rx_local_skips = audio->jitter->local_skips;
    audio->stats.rx_jitter_overruns = audio->jitter->overflow;
    portEXIT_CRITICAL(&audio->lock);
}

static bool decode_frame(digits_call_audio_t *audio, void *decoder,
                         call_audio_jitter_result_t selection)
{
    bool conceal = selection == CALL_AUDIO_JITTER_MISSING;
    esp_audio_dec_in_raw_t input = {
        // The pinned wrapper rejects a NULL buffer before calling libopus.
        // A non-NULL buffer with len=0 reaches libopus's PLC path; the generic
        // frame_recover flag alone is not implemented by this Opus wrapper.
        .buffer = audio->playback_packet->data,
        .len = conceal ? 0 : audio->playback_packet->size,
        .frame_recover = conceal ? ESP_AUDIO_DEC_RECOVERY_PLC : ESP_AUDIO_DEC_RECOVERY_NONE,
    };
    esp_audio_dec_out_frame_t output = {
        .buffer = (uint8_t *)audio->mono,
        .len = DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t),
    };
    esp_audio_dec_info_t information = {0};
    int64_t started_at = esp_timer_get_time();
    esp_audio_err_t error = esp_opus_dec_decode(decoder, &input, &output, &information);
    int64_t finished_at = esp_timer_get_time();
    bool valid = error == ESP_AUDIO_ERR_OK &&
        output.decoded_size == DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t) &&
        information.sample_rate == DIGITS_AUDIO_SAMPLE_RATE && information.channel == 1 &&
        information.bits_per_sample == 16;
    bool first_failure = false;
    portENTER_CRITICAL(&audio->lock);
    maximum(&audio->stats.max_decode_us, finished_at - started_at);
    if (!conceal) maximum(&audio->stats.max_receive_age_us,
                         finished_at - audio->playback_packet->captured_at_us);
    if (valid) {
        if (conceal) ++audio->stats.plc_frames;
        else ++audio->stats.decoded_frames;
    } else {
        first_failure = audio->stats.decode_errors == 0;
        ++audio->stats.decode_errors;
        ++audio->stats.silence_frames;
    }
    portEXIT_CRITICAL(&audio->lock);
    if (first_failure)
        ESP_LOGE("call_audio", "First decode failure: error=%d input=%u decoded=%u bytes rate=%u channels=%u bits=%u PLC=%u",
                 error, (unsigned)input.len, (unsigned)output.decoded_size,
                 (unsigned)information.sample_rate, (unsigned)information.channel,
                 (unsigned)information.bits_per_sample, (unsigned)conceal);
    if (!valid) memset(audio->mono, 0, DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t));
    return valid;
}

// One 20 ms mono frame of the greeting to the speaker. False on a write error.
static bool play_monitor_frame(digits_call_audio_t *audio, bool *amplifier_enabled)
{
    call_audio_pcm_stereo(audio->mono, audio->stereo, DIGITS_MIC_FRAME_SAMPLES);
    if (!*amplifier_enabled) {
        if (digits_audio_speaker_start() != ESP_OK) return false;
        *amplifier_enabled = true;
    }
    for (unsigned block = 0; block < 2 && !stopping(audio); ++block) {
        size_t written = 0;
        if (digits_audio_speaker_write(audio->stereo + block * DIGITS_AUDIO_DMA_FRAMES * 2,
                                       DIGITS_AUDIO_DMA_FRAMES * 2 * sizeof(int16_t), &written,
                                       WRITE_TIMEOUT_MS) != ESP_OK ||
            written != DIGITS_AUDIO_DMA_FRAMES * 2 * sizeof(int16_t))
            return false;
    }
    return true;
}

static void playback_worker(void *argument)
{
    digits_call_audio_t *audio = argument;
    void *decoder = NULL;
    bool speaker_owned = digits_audio_speaker_acquire(0);
    if (!speaker_owned) {
        failure(audio, ESP_ERR_INVALID_STATE);
        goto finished;
    }
    esp_opus_dec_cfg_t configuration = ESP_OPUS_DEC_CONFIG_DEFAULT();
    configuration.sample_rate = DIGITS_AUDIO_SAMPLE_RATE;
    configuration.channel = 1;
    configuration.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    configuration.self_delimited = false;
    esp_audio_err_t error = esp_opus_dec_open(&configuration, sizeof(configuration), &decoder);
    if (error != ESP_AUDIO_ERR_OK) {
        ESP_LOGE("call_audio", "Cannot create Opus playback decoder (error=%d)", error);
        failure(audio, error == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL);
        goto finished;
    }
    unsigned opened_stack_minimum = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("call_audio", "Opus decoder opened: stack=%u minimum_free=%u bytes; 48000 Hz mono 20 ms",
             (unsigned)DECODER_WORKER_STACK_BYTES, opened_stack_minimum);
    portENTER_CRITICAL(&audio->lock);
    audio->stats.rx_stack_min_bytes = opened_stack_minimum;
    audio->rx_ready = true;
    portEXIT_CRITICAL(&audio->lock);
    bool amplifier_enabled = false;
    int64_t previous_write_at = 0;
    int64_t stack_report_at = 0;
    while (!stopping(audio)) {
        // Answering machine: the greeting, then the caller, or silence.
        if (unattended(audio)) {
            bool greeting = !greeting_done(audio);
            if (greeting && audio->monitor) {
                if (!monitor_pop(audio, audio->mono)) {
                    vTaskDelay(1);
                } else if (!play_monitor_frame(audio, &amplifier_enabled)) {
                    digits_audio_speaker_silence();
                    amplifier_enabled = false;
                }
                continue;
            }
            if (greeting || !audio->hooks.hear_caller) {
                if (amplifier_enabled) {
                    digits_audio_speaker_silence();
                    amplifier_enabled = false;
                }
                vTaskDelay(1);
                continue;
            }
            // After the greeting the caller plays like a normal call.
        }
        for (unsigned i = 0; i < RX_QUEUE_LENGTH; ++i) {
            if (xQueueReceive(audio->rx_packets, audio->receive_packet, 0) != pdTRUE) break;
            int64_t received_at = esp_timer_get_time();
            if (received_at - audio->receive_packet->captured_at_us >= CAPTURE_MAX_AGE_US) {
                portENTER_CRITICAL(&audio->lock);
                ++audio->stats.rx_queue_stale;
                portEXIT_CRITICAL(&audio->lock);
                continue;
            }
            call_audio_jitter_push(audio->jitter, audio->receive_packet, received_at);
        }
        // Prime the initial frame for 60 ms. Then feed subsequent frames one
        // 10 ms descriptor early; waiting for their actual playout deadline
        // would let both hardware descriptors empty before decoding/writing.
        int64_t selection_at = esp_timer_get_time() +
            (audio->jitter->playing ? PLAYBACK_FEED_AHEAD_US : 0);
        call_audio_jitter_result_t selection = call_audio_jitter_pop(
            audio->jitter, selection_at, audio->playback_packet);
        copy_jitter_stats(audio);
        if (selection == CALL_AUDIO_JITTER_WAIT) {
            if (amplifier_enabled && !audio->jitter->primed) {
                // A prolonged outage mutes output until a new 60 ms window.
                digits_audio_speaker_silence();
                amplifier_enabled = false;
                previous_write_at = 0;
                esp_opus_dec_reset(decoder);
            }
            vTaskDelay(1);
            continue;
        }
        decode_frame(audio, decoder, selection);
        if (stopping(audio)) break;
        call_audio_pcm_stereo(audio->mono, audio->stereo, DIGITS_MIC_FRAME_SAMPLES);
        if (!amplifier_enabled) {
            if (digits_audio_speaker_start() != ESP_OK) {
                failure(audio, ESP_FAIL);
                break;
            }
            amplifier_enabled = true;
        }
        int64_t write_at = esp_timer_get_time();
        if (previous_write_at && write_at - previous_write_at > DMA_WINDOW_US) {
            // A producer gap beyond the configured 20 ms DMA capacity. This
            // is a local feed-gap diagnostic, not a network loss measurement.
            portENTER_CRITICAL(&audio->lock);
            ++audio->stats.playback_feed_gaps;
            portEXIT_CRITICAL(&audio->lock);
        }
        bool written_ok = true;
        unsigned completed_blocks = 0;
        for (unsigned block = 0; block < 2 && !stopping(audio); ++block) {
            size_t written = 0;
            esp_err_t result = digits_audio_speaker_write(
                audio->stereo + block * DIGITS_AUDIO_DMA_FRAMES * 2,
                DIGITS_AUDIO_DMA_FRAMES * 2 * sizeof(int16_t), &written, WRITE_TIMEOUT_MS);
            if (result != ESP_OK || written != DIGITS_AUDIO_DMA_FRAMES * 2 * sizeof(int16_t)) {
                written_ok = false;
                break;
            }
            ++completed_blocks;
        }
        int64_t write_end = esp_timer_get_time();
        portENTER_CRITICAL(&audio->lock);
        maximum(&audio->stats.max_write_us, write_end - write_at);
        if (completed_blocks == 2) ++audio->stats.played_frames;
        else if (!written_ok) ++audio->stats.playback_errors;
        portEXIT_CRITICAL(&audio->lock);
        previous_write_at = write_end;
        if (!written_ok) {
            // Flush a partial stereo block before decoding the next frame.
            digits_audio_speaker_silence();
            amplifier_enabled = false;
            previous_write_at = 0;
            for (unsigned i = 0; i < CALL_AUDIO_JITTER_SLOTS; ++i)
                audio->jitter->slots[i].occupied = false;
            audio->jitter->primed = false;
            ++audio->jitter->discontinuities;
            esp_opus_dec_reset(decoder);
        }
        if (write_end >= stack_report_at) {
            unsigned minimum = uxTaskGetStackHighWaterMark(NULL);
            portENTER_CRITICAL(&audio->lock);
            audio->stats.rx_stack_min_bytes = minimum;
            portEXIT_CRITICAL(&audio->lock);
            stack_report_at = write_end + 1000000;
        }
    }
finished:
    if (speaker_owned) {
        if (!digits_audio_speaker_silence()) failure(audio, ESP_FAIL);
        digits_audio_speaker_release();
    }
    if (decoder) esp_opus_dec_close(decoder);
    unsigned minimum = uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&audio->lock);
    audio->stats.rx_stack_min_bytes = minimum;
    audio->rx_done = true;
    portEXIT_CRITICAL(&audio->lock);
    vTaskDelete(NULL);
}

esp_err_t digits_call_audio_start(digits_call_audio_mode_t mode, digits_call_audio_t **out)
{
    return digits_call_audio_start_unattended(mode, NULL, out);
}

esp_err_t digits_call_audio_start_unattended(digits_call_audio_mode_t mode,
                                             const digits_call_audio_unattended_t *hooks,
                                             digits_call_audio_t **out)
{
    if (!out || mode < DIGITS_CALL_AUDIO_TX || mode > DIGITS_CALL_AUDIO_FULL ||
        (hooks && (!hooks->fill || !hooks->record)))
        return ESP_ERR_INVALID_ARG;
    *out = NULL;
    digits_call_audio_t *audio = calloc(1, sizeof(*audio));
    if (!audio) return ESP_ERR_NO_MEM;
    audio->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    audio->mode = mode;
    voice_style_copper_init(&audio->filter, TX_VOICE_RATE);
    if (hooks) {
        audio->hooks = *hooks;
        audio->unattended = true; // Before any worker exists.
        if (hooks->hear_greeting && (mode & DIGITS_CALL_AUDIO_RX)) {
            audio->monitor = audio_calloc(MONITOR_FRAMES * DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t));
            if (!audio->monitor) goto no_memory;
        }
    }
    audio->tx_ready = audio->tx_done = !(mode & DIGITS_CALL_AUDIO_TX);
    audio->rx_ready = audio->rx_done = !(mode & DIGITS_CALL_AUDIO_RX);
    if (mode & DIGITS_CALL_AUDIO_TX) {
        audio->tx_queue_storage = audio_calloc(TX_QUEUE_LENGTH * sizeof(call_audio_packet_t));
        audio->capture = audio_calloc(sizeof(*audio->capture));
        audio->encoder_packet = audio_calloc(sizeof(*audio->encoder_packet));
        audio->greeting_down = audio_calloc(sizeof(*audio->greeting_down));
        audio->tx_pcm = audio_calloc(TX_VOICE_SAMPLES * sizeof(int16_t));
        if (!audio->tx_queue_storage || !audio->capture || !audio->encoder_packet ||
            !audio->greeting_down || !audio->tx_pcm) goto no_memory;
        decimate_init(audio->greeting_down);
        audio->tx_packets = xQueueCreateStatic(TX_QUEUE_LENGTH, sizeof(call_audio_packet_t),
                                              audio->tx_queue_storage, &audio->tx_queue_control);
        if (!audio->tx_packets) goto no_memory;
    }
    if (mode & DIGITS_CALL_AUDIO_RX) {
        audio->rx_queue_storage = audio_calloc(RX_QUEUE_LENGTH * sizeof(call_audio_packet_t));
        audio->receive_packet = audio_calloc(sizeof(*audio->receive_packet));
        audio->playback_packet = audio_calloc(sizeof(*audio->playback_packet));
        audio->jitter = audio_calloc(sizeof(*audio->jitter));
        audio->mono = audio_calloc(DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t));
        audio->stereo = audio_calloc(DIGITS_MIC_FRAME_SAMPLES * 2 * sizeof(int16_t));
        if (!audio->rx_queue_storage || !audio->receive_packet || !audio->playback_packet ||
            !audio->jitter || !audio->mono || !audio->stereo) goto no_memory;
        audio->rx_packets = xQueueCreateStatic(RX_QUEUE_LENGTH, sizeof(call_audio_packet_t),
                                              audio->rx_queue_storage, &audio->rx_queue_control);
        if (!audio->rx_packets) goto no_memory;
    }
    // Both task stacks remain in internal RAM; bulk PCM/queue data prefer PSRAM.
    if ((mode & DIGITS_CALL_AUDIO_TX) &&
        xTaskCreate(encoder_worker, "digits_call_tx", ENCODER_WORKER_STACK_BYTES,
                    audio, 5, NULL) != pdPASS) {
        audio->tx_done = true;
        audio->rx_done = true;
        failure(audio, ESP_ERR_NO_MEM);
    } else if ((mode & DIGITS_CALL_AUDIO_RX) &&
        xTaskCreate(playback_worker, "digits_call_rx", DECODER_WORKER_STACK_BYTES,
                    audio, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&audio->lock);
        audio->rx_done = true;
        portEXIT_CRITICAL(&audio->lock);
        failure(audio, ESP_ERR_NO_MEM);
    }
    *out = audio;
    return ESP_OK; // Failure after task creation is reported via async status.
no_memory:
    audio->tx_done = audio->rx_done = true;
    digits_call_audio_destroy(audio);
    return ESP_ERR_NO_MEM;
}

void digits_call_audio_get_stats(digits_call_audio_t *audio, digits_call_audio_stats_t *stats)
{
    if (!audio || !stats) return;
    portENTER_CRITICAL(&audio->lock);
    *stats = audio->stats;
    if (audio->stats.error != ESP_OK) stats->state = DIGITS_CALL_AUDIO_FAILED;
    else if (audio->tx_done && audio->rx_done) stats->state = DIGITS_CALL_AUDIO_STOPPED;
    else if (audio->stop_requested) stats->state = DIGITS_CALL_AUDIO_STOPPING;
    else if (audio->tx_ready && audio->rx_ready) stats->state = DIGITS_CALL_AUDIO_READY;
    else stats->state = DIGITS_CALL_AUDIO_STARTING;
    portEXIT_CRITICAL(&audio->lock);
}

bool digits_call_audio_pop_encoded(digits_call_audio_t *audio, call_audio_packet_t *packet)
{
    if (!audio || !packet || !audio->tx_packets || stopping(audio)) return false;
    int64_t now = esp_timer_get_time();
    if (now < audio->next_pop_us ||
        (audio->last_pop_us && now - audio->last_pop_us < FRAME_US / 2)) return false;
    unsigned count = 0;
    for (unsigned i = 0; i < TX_QUEUE_LENGTH; ++i) {
        if (xQueueReceive(audio->tx_packets, packet, 0) != pdTRUE) break;
        ++count;
    }
    if (!count) return false;
    bool stale = now - packet->captured_at_us >= CAPTURE_MAX_AGE_US;
    portENTER_CRITICAL(&audio->lock);
    audio->stats.tx_pop_drops += count - 1 + (stale ? 1 : 0);
    portEXIT_CRITICAL(&audio->lock);
    if (stale) return false;
    // Anchor to the capture clock, advancing whole slots rather than rebasing
    // each deadline to a late peer poll. Otherwise a 6 ms polling period would
    // turn the 20 ms stream into 24 ms and systematically discard speech.
    // Drain backlog above and retain a 10 ms minimum spacing after a stall.
    if (!audio->next_pop_us) audio->next_pop_us = packet->captured_at_us;
    audio->next_pop_us += ((now - audio->next_pop_us) / FRAME_US + 1) * FRAME_US;
    audio->last_pop_us = now;
    return true;
}

void digits_call_audio_record_send(digits_call_audio_t *audio, const call_audio_packet_t *packet,
                                   bool success, bool blocked)
{
    if (!audio || !packet) return;
    int64_t age = esp_timer_get_time() - packet->captured_at_us;
    portENTER_CRITICAL(&audio->lock);
    if (success) ++audio->stats.sent_frames;
    else if (blocked) ++audio->stats.send_blocked;
    else ++audio->stats.send_errors;
    maximum(&audio->stats.max_send_age_us, age);
    portEXIT_CRITICAL(&audio->lock);
}

bool digits_call_audio_receive(digits_call_audio_t *audio, uint32_t pts_ms,
                               const uint8_t *data, size_t size)
{
    if (!audio || !audio->rx_packets || stopping(audio)) return false;
    if (!data || !size || size > CALL_AUDIO_OPUS_MAX_BYTES) {
        portENTER_CRITICAL(&audio->lock);
        ++audio->stats.rx_invalid;
        portEXIT_CRITICAL(&audio->lock);
        return false;
    }
    if (unattended(audio)) {
        audio->hooks.record(data, size);
        if (!audio->hooks.hear_caller || !greeting_done(audio)) {
            portENTER_CRITICAL(&audio->lock);
            ++audio->stats.received_frames;
            portEXIT_CRITICAL(&audio->lock);
            return true;
        }
        // Also to the speaker, below.
    }
    // Callback stack holds one bounded packet; no allocator or wait here.
    call_audio_packet_t packet = {
        .pts_ms = pts_ms, .size = size, .captured_at_us = esp_timer_get_time(),
    };
    memcpy(packet.data, data, size);
    bool queued = xQueueSend(audio->rx_packets, &packet, 0) == pdTRUE;
    portENTER_CRITICAL(&audio->lock);
    ++audio->stats.received_frames;
    if (!queued) ++audio->stats.rx_queue_drops;
    portEXIT_CRITICAL(&audio->lock);
    return queued;
}

void digits_call_audio_set_copper(digits_call_audio_t *audio, bool copper)
{
    if (audio) __atomic_store_n(&audio->copper, copper, __ATOMIC_RELAXED);
}

void digits_call_audio_attend(digits_call_audio_t *audio)
{
    if (!audio) return;
    portENTER_CRITICAL(&audio->lock);
    audio->unattended = false;
    portEXIT_CRITICAL(&audio->lock);
}

void digits_call_audio_request_stop(digits_call_audio_t *audio)
{
    if (!audio) return;
    portENTER_CRITICAL(&audio->lock);
    audio->stop_requested = true;
    portEXIT_CRITICAL(&audio->lock);
}

bool digits_call_audio_is_stopped(digits_call_audio_t *audio)
{
    if (!audio) return true;
    portENTER_CRITICAL(&audio->lock);
    bool done = audio->tx_done && audio->rx_done;
    portEXIT_CRITICAL(&audio->lock);
    return done;
}

esp_err_t digits_call_audio_destroy(digits_call_audio_t *audio)
{
    if (!audio) return ESP_OK;
    if (!digits_call_audio_is_stopped(audio)) return ESP_ERR_INVALID_STATE;
    if (audio->tx_packets) vQueueDelete(audio->tx_packets);
    if (audio->rx_packets) vQueueDelete(audio->rx_packets);
    free(audio->tx_queue_storage);
    free(audio->rx_queue_storage);
    free(audio->capture);
    free(audio->encoder_packet);
    free(audio->greeting_down);
    free(audio->tx_pcm);
    free(audio->receive_packet);
    free(audio->playback_packet);
    free(audio->jitter);
    free(audio->mono);
    free(audio->stereo);
    free(audio->monitor);
    free(audio);
    return ESP_OK;
}
