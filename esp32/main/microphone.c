#include "microphone.h"
#include "microphone_pcm.h"
#include "audio_board.h"
#include <string.h>
#include "esp_attr.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define FRAME_QUEUE_LENGTH 4
#define READ_TIMEOUT_MS 100
#define WARMUP_FRAMES 10 // Discard the first 200 ms after ADC startup.

typedef struct {
    microphone_pcm_stats_t levels;
    uint32_t errors;
    esp_err_t last_error;
    uint32_t overruns;
    bool diagnostic;
    bool paused;
} microphone_report_t;

#if CONFIG_DIGITS_MIC_MONO_RIGHT
#define MONO_SLOT MICROPHONE_PCM_RIGHT
#define MONO_NAME "right/MIC2"
#elif CONFIG_DIGITS_MIC_MONO_AVERAGE
#define MONO_SLOT MICROPHONE_PCM_AVERAGE
#define MONO_NAME "average(MIC1,MIC2)"
#else
#define MONO_SLOT MICROPHONE_PCM_LEFT
#define MONO_NAME "left/MIC1"
#endif

static struct {
    const audio_codec_ctrl_if_t *ctrl;
    const audio_codec_if_t *codec;
    QueueHandle_t frames;
    QueueHandle_t reports;
    TaskHandle_t report_task;
    SemaphoreHandle_t control_lock;
    int16_t stereo[DIGITS_MIC_FRAME_SAMPLES * 2];
    digits_microphone_frame_t frame;
    microphone_pcm_stats_t levels;
    bool diagnostic;
    uint32_t dma_overruns;
    bool paused;
    bool requested_pause;
    uint32_t requested_control;
    uint32_t acknowledged_control;
    esp_err_t control_result;
} mic;
static portMUX_TYPE mic_mux = portMUX_INITIALIZER_UNLOCKED;

static bool IRAM_ATTR receive_overflow(i2s_chan_handle_t channel,
                                      i2s_event_data_t *event, void *context)
{
    portENTER_CRITICAL_ISR(&mic_mux);
    ++mic.dma_overruns;
    portEXIT_CRITICAL_ISR(&mic_mux);
    return false;
}

static uint32_t overflow_count(void)
{
    portENTER_CRITICAL(&mic_mux);
    uint32_t count = mic.dma_overruns;
    portEXIT_CRITICAL(&mic_mux);
    return count;
}

static bool diagnostic_enabled(void)
{
    portENTER_CRITICAL(&mic_mux);
    bool enabled = mic.diagnostic;
    portEXIT_CRITICAL(&mic_mux);
    return enabled;
}

void digits_microphone_set_diagnostic(bool enabled)
{
    portENTER_CRITICAL(&mic_mux);
    mic.diagnostic = enabled;
    portEXIT_CRITICAL(&mic_mux);
}

static void report_levels(const microphone_report_t *report, unsigned silent_windows[3])
{
    const microphone_pcm_stats_t *levels = &report->levels;
    const microphone_pcm_level_t *mono = &levels->mono;
    double rms = microphone_pcm_rms(mono);
    double db = microphone_pcm_dbfs(mono);
    char meter[21];
    int filled = (int)((db + 70.0) * 20.0 / 70.0);
    if (filled < 0) filled = 0;
    if (filled > 20) filled = 20;
    for (unsigned i = 0; i < 20; ++i) meter[i] = i < filled ? '#' : '.';
    meter[20] = '\0';
    ESP_LOGI("microphone", "mono RMS=%.1f (%.1f dBFS) peak=%u (%.1f%%) [%s]; L RMS=%.1f peak=%u; R RMS=%.1f peak=%u; samples=%u",
             rms, db, (unsigned)mono->peak, mono->peak / 327.68, meter,
             microphone_pcm_rms(&levels->left), (unsigned)levels->left.peak,
             microphone_pcm_rms(&levels->right), (unsigned)levels->right.peak,
             (unsigned)mono->sample_count);
    if (!mono->sample_count) {
        memset(silent_windows, 0, 3 * sizeof(*silent_windows));
        ESP_LOGW("microphone", "No complete PCM frames in diagnostic window");
    } else {
        const microphone_pcm_level_t *channels[] = {&levels->left, &levels->right, mono};
        const char *names[] = {"MIC1/left", "MIC2/right", "mono"};
        for (unsigned i = 0; i < 3; ++i) {
            if (microphone_pcm_rms(channels[i]) < 2.0) {
                if (++silent_windows[i] >= 5)
                    ESP_LOGW("microphone", "Persistent near-silence on %s (%u windows, RMS < 2); check mic selection and ADC",
                             names[i], silent_windows[i]);
            } else silent_windows[i] = 0;
        }
    }
    if (levels->left.clipped_count || levels->right.clipped_count)
        ESP_LOGW("microphone", "Clipping: L=%u R=%u mono=%u samples at |PCM| >= 32760; lower microphone gain",
                 (unsigned)levels->left.clipped_count, (unsigned)levels->right.clipped_count,
                 (unsigned)mono->clipped_count);
    if (report->errors)
        ESP_LOGE("microphone", "I2S read errors=%u, last=%s; partial reads retained, frames marked discontinuous",
                 (unsigned)report->errors, esp_err_to_name(report->last_error));
    if (report->overruns)
        ESP_LOGW("microphone", "I2S RX DMA overruns=%u; frames marked discontinuous", (unsigned)report->overruns);
}

static void report_worker(void *arg)
{
    microphone_report_t report;
    unsigned silent_windows[3] = {0};
    bool was_diagnostic = false;
    bool was_paused = false;
    for (;;) {
        xQueueReceive(mic.reports, &report, portMAX_DELAY);
        if (report.paused) {
            if (!was_paused) ESP_LOGI("microphone", "Input muted for recorded playback; level reports paused");
            memset(silent_windows, 0, sizeof(silent_windows));
            was_paused = true;
            continue;
        }
        was_paused = false;
        if (report.diagnostic != was_diagnostic) memset(silent_windows, 0, sizeof(silent_windows));
        was_diagnostic = report.diagnostic;
        if (report.diagnostic) report_levels(&report, silent_windows);
        else if (report.errors || report.overruns)
            ESP_LOGE("microphone", "Capture errors: I2S reads=%u (%s), DMA overruns=%u",
                     (unsigned)report.errors, esp_err_to_name(report.last_error), (unsigned)report.overruns);
    }
}

static void capture_worker(void *arg)
{
    i2s_chan_handle_t rx = digits_audio_rx_channel();
    size_t filled = 0;
    unsigned byte_phase = 0, align_skip = 0;
    uint8_t alignment_bytes[sizeof(int16_t) * 2];
    unsigned warmup = WARMUP_FRAMES;
    uint32_t previous_overruns = overflow_count(), window_overruns = 0, errors = 0;
    esp_err_t last_error = ESP_OK;
    bool discontinuity = false, was_diagnostic = false;
    int64_t report_at = esp_timer_get_time() + 1000000;
    for (;;) {
        portENTER_CRITICAL(&mic_mux);
        uint32_t control = mic.requested_control;
        bool requested_pause = mic.requested_pause;
        portEXIT_CRITICAL(&mic_mux);
        if (control != mic.acknowledged_control) {
            // Codec control stays in this task; the caller waits for this ACK
            // before enabling the amplifier. Never stop the shared RX clock.
            esp_err_t control_result = mic.codec->mute_mic(mic.codec, requested_pause) == ESP_CODEC_DEV_OK
                ? ESP_OK : ESP_FAIL;
            int mute12, mute34;
            if (control_result == ESP_OK &&
                (mic.codec->get_reg(mic.codec, 0x14, &mute12) != ESP_CODEC_DEV_OK ||
                 mic.codec->get_reg(mic.codec, 0x15, &mute34) != ESP_CODEC_DEV_OK ||
                 (mute12 & 3) != (requested_pause ? 3 : 0) ||
                 (mute34 & 3) != (requested_pause ? 3 : 0)))
                control_result = ESP_FAIL;
            discontinuity = true;
            if (control_result == ESP_OK) {
                mic.paused = requested_pause;
                filled = 0;
                align_skip = (sizeof(alignment_bytes) - byte_phase) % sizeof(alignment_bytes);
                warmup = requested_pause ? 0 : WARMUP_FRAMES;
                xQueueReset(mic.frames);
                microphone_pcm_stats_reset(&mic.levels);
                errors = window_overruns = 0;
            }
            portENTER_CRITICAL(&mic_mux);
            mic.control_result = control_result;
            mic.acknowledged_control = control;
            portEXIT_CRITICAL(&mic_mux);
        }
        size_t bytes = 0;
        bool aligning = align_skip != 0;
        void *destination = aligning ? alignment_bytes : (uint8_t *)mic.stereo + filled;
        size_t capacity = aligning ? align_skip : sizeof(mic.stereo) - filled;
        esp_err_t err = i2s_channel_read(rx, destination, capacity, &bytes, READ_TIMEOUT_MS);
        // Keep partial bytes, including partial words, so a timeout never
        // changes slot/sample alignment. Convert only a complete stereo frame.
        byte_phase = (byte_phase + bytes) % sizeof(alignment_bytes);
        if (aligning) align_skip -= bytes;
        else filled += bytes;
        if (err != ESP_OK || !bytes) {
            ++errors;
            last_error = err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
            discontinuity = true;
            // Prevent a busy loop if the channel unexpectedly stops.
            vTaskDelay(1);
        }
        uint32_t current_overruns = overflow_count();
        window_overruns += current_overruns - previous_overruns;
        if (current_overruns != previous_overruns) {
            discontinuity = true;
            // A partially assembled frame must not span a lost DMA block.
            // Lost DMA blocks contain whole stereo samples. If a partial read
            // ended inside a word, finish discarding that stereo sample before
            // assembling the next frame; never silently swap slots/byte order.
            align_skip = (sizeof(alignment_bytes) - byte_phase) % sizeof(alignment_bytes);
            filled = 0;
        }
        previous_overruns = current_overruns;
        bool diagnostic = diagnostic_enabled();
        if (diagnostic != was_diagnostic) {
            microphone_pcm_stats_reset(&mic.levels);
            errors = window_overruns = 0;
            report_at = esp_timer_get_time() + 1000000;
            was_diagnostic = diagnostic;
        }
        if (filled == sizeof(mic.stereo)) {
            filled = 0;
            if (warmup) {
                --warmup;
            } else if (!mic.paused) {
                microphone_pcm_convert(mic.stereo, mic.frame.pcm, DIGITS_MIC_FRAME_SAMPLES,
                                       MONO_SLOT, diagnostic ? &mic.levels : NULL);
                mic.frame.sequence++;
                mic.frame.flags = discontinuity ? DIGITS_MIC_FRAME_DISCONTINUITY : 0;
                mic.frame.captured_at_us = esp_timer_get_time();
                // Drop the oldest frame, never wait for a slow/absent consumer.
                // A reader receives its own copy; no DMA buffer is shared.
                if (xQueueSend(mic.frames, &mic.frame, 0) != pdTRUE) {
                    digits_microphone_frame_t discarded;
                    if (xQueueReceive(mic.frames, &discarded, 0) == pdTRUE)
                        mic.frame.flags |= DIGITS_MIC_FRAME_DISCONTINUITY;
                    xQueueSend(mic.frames, &mic.frame, 0);
                }
                discontinuity = false;
            }
        }
        int64_t now = esp_timer_get_time();
        if (now >= report_at) {
            microphone_report_t report = {
                .levels = mic.levels, .errors = errors, .last_error = last_error,
                .overruns = window_overruns, .diagnostic = diagnostic,
                .paused = mic.paused,
            };
            // Serial formatting/writes run at lower priority, outside capture.
            // A slow/unplugged monitor cannot block RX: keep only latest stats.
            xQueueOverwrite(mic.reports, &report);
            microphone_pcm_stats_reset(&mic.levels);
            errors = window_overruns = 0;
            report_at = now + 1000000;
        }
    }
}

static void release_microphone(void)
{
    if (mic.report_task) vTaskDelete(mic.report_task);
    if (mic.codec) audio_codec_delete_codec_if(mic.codec);
    if (mic.ctrl) audio_codec_delete_ctrl_if(mic.ctrl);
    if (mic.frames) vQueueDelete(mic.frames);
    if (mic.reports) vQueueDelete(mic.reports);
    if (mic.control_lock) vSemaphoreDelete(mic.control_lock);
    portENTER_CRITICAL(&mic_mux);
    memset(&mic, 0, sizeof(mic));
    portEXIT_CRITICAL(&mic_mux);
}

esp_err_t digits_microphone_init(void)
{
    if (mic.frames) return ESP_ERR_INVALID_STATE;
    i2c_master_bus_handle_t bus = digits_audio_i2c_bus();
    i2s_chan_handle_t rx = digits_audio_rx_channel();
    if (!bus || !rx) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_FAIL;
    // Both address straps are low: 7-bit 0x40. This driver takes 8-bit 0x80.
    audio_codec_i2c_cfg_t control = {
        .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = bus, .clock_speed_hz = 100000,
    };
    mic.ctrl = audio_codec_new_i2c_ctrl(&control);
    if (!mic.ctrl) { err = ESP_ERR_NO_MEM; goto fail; }
    es7210_codec_cfg_t codec = {
        .ctrl_if = mic.ctrl, .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
        .mclk_src = ES7210_MCLK_FROM_PAD, .mclk_div = 256,
    };
    // MIC3 is the ES8311 echo reference, MIC4 is unused. Exactly two selected
    // ADCs mean ordinary Philips stereo, not the Waveshare demo's 4-ch TDM.
    mic.codec = es7210_codec_new(&codec);
    if (!mic.codec) goto fail;
    esp_codec_dev_sample_info_t sample = {
        .sample_rate = DIGITS_AUDIO_SAMPLE_RATE, .channel = 2, .channel_mask = 3,
        .bits_per_sample = 16, .mclk_multiple = 256,
    };
    // Use checked low-level codec calls: the device convenience wrappers in
    // pinned esp_codec_dev 1.6.2 can discard gain/mute I2C errors.
    if (mic.codec->set_fs(mic.codec, &sample) != ESP_CODEC_DEV_OK ||
        mic.codec->enable(mic.codec, true) != ESP_CODEC_DEV_OK ||
        mic.codec->set_mic_gain(mic.codec, CONFIG_DIGITS_MIC_GAIN_DB) != ESP_CODEC_DEV_OK ||
        mic.codec->mute_mic(mic.codec, false) != ESP_CODEC_DEV_OK) goto fail;
    // Gain must be applied AFTER enable: ES7210 startup rewrites gain bits.
    int format, routing, mode;
    if (mic.codec->get_reg(mic.codec, 0x11, &format) != ESP_CODEC_DEV_OK ||
        mic.codec->get_reg(mic.codec, 0x12, &routing) != ESP_CODEC_DEV_OK ||
        mic.codec->get_reg(mic.codec, 0x08, &mode) != ESP_CODEC_DEV_OK) goto fail;
    if (format != 0x60 || routing != 0x00 || (mode & 1)) {
        ESP_LOGE("microphone", "Unexpected ADC format: SDP1=0x%02x SDP2=0x%02x MODE=0x%02x", format, routing, mode);
        err = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    mic.frames = xQueueCreate(FRAME_QUEUE_LENGTH, sizeof(digits_microphone_frame_t));
    mic.reports = xQueueCreate(1, sizeof(microphone_report_t));
    mic.control_lock = xSemaphoreCreateMutex();
    if (!mic.frames || !mic.reports || !mic.control_lock) { err = ESP_ERR_NO_MEM; goto fail; }
    if (xTaskCreate(report_worker, "digits_mic_log", 4096, NULL, 3, &mic.report_task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = receive_overflow};
    err = i2s_channel_register_event_callback(rx, &callbacks, NULL);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(rx);
    if (err != ESP_OK) goto fail;
#ifdef CONFIG_DIGITS_MIC_DIAGNOSTIC
    digits_microphone_set_diagnostic(true);
#endif
    if (xTaskCreate(capture_worker, "digits_mic", 6144, NULL, 5, NULL) != pdPASS) {
        // Do not stop native RX while TX runs on ESP32-S3. Unregister the
        // callback only on a disabled channel; leave it harmlessly counting.
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI("microphone", "Capture ready: ES7210 MIC1+MIC2, 48000 Hz, 16-bit Philips stereo -> mono %s, gain=%d dB; diagnostics=%s",
             MONO_NAME, CONFIG_DIGITS_MIC_GAIN_DB, diagnostic_enabled() ? "on" : "off");
    return ESP_OK;
fail:
    release_microphone();
    return err;
}

esp_err_t digits_microphone_read(digits_microphone_frame_t *frame, uint32_t timeout_ms)
{
    if (!frame || timeout_ms > 1000) return ESP_ERR_INVALID_ARG;
    if (!mic.frames) return ESP_ERR_INVALID_STATE;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms && !ticks) ticks = 1;
    return xQueueReceive(mic.frames, frame, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t digits_microphone_pause(bool paused, uint32_t timeout_ms)
{
    if (!timeout_ms || timeout_ms > 1000) return ESP_ERR_INVALID_ARG;
    if (!mic.control_lock) return ESP_ERR_INVALID_STATE;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    if (xSemaphoreTake(mic.control_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&mic_mux);
    uint32_t request = ++mic.requested_control;
    mic.requested_pause = paused;
    portEXIT_CRITICAL(&mic_mux);
    esp_err_t result = ESP_ERR_TIMEOUT;
    do {
        portENTER_CRITICAL(&mic_mux);
        bool acknowledged = mic.acknowledged_control == request;
        esp_err_t control_result = mic.control_result;
        portEXIT_CRITICAL(&mic_mux);
        if (acknowledged) { result = control_result; break; }
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);
    xSemaphoreGive(mic.control_lock);
    // On timeout the request may still execute. A later resume request
    // supersedes it and matching generation IDs reject stale acknowledgements.
    return result;
}
