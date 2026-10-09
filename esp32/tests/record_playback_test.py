from pathlib import Path
import subprocess
import tempfile

# Compile the production one-shot transaction with finite capture/DMA mocks.
# This tests ownership, complete recordings and failure cleanup, not hardware.
repo = Path(__file__).resolve().parents[2]
source = (repo / "esp32/main/record_playback_test.c").read_text()
transaction = source[
    source.index("static esp_err_t run_record_playback(void)"):
    source.index("static void record_worker(void *arg)")
]

prefix = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "microphone.h"
#include "record_playback_pcm.h"
#define configTICK_RATE_HZ 100
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define ESP_LOGI(tag, ...) log_message(__VA_ARGS__)
#define ESP_LOGW(tag, ...) log_message(__VA_ARGS__)
#define ESP_LOGE(tag, ...) log_message(__VA_ARGS__)

typedef enum {
    NORMAL, LOW_LEVEL, WRAP_SEQUENCE, ALLOC_FAIL, ACQUIRE_FAIL, RECORD_TIMEOUT,
    RECORD_DEADLINE, RECORD_GAP, RECORD_DISCONTINUITY, PAUSE_FAIL,
    PAUSE_TIMEOUT, SPEAKER_START_FAIL, WRITE_TIMEOUT, WRITE_PARTIAL,
    PLAYBACK_DEADLINE, SILENCE_FAIL, RESUME_FAIL,
} scenario_t;

static scenario_t scenario;
static int64_t now_us, capture_start_us, speaker_start_us, gate_off_us;
static unsigned allocations, frees, acquisitions, releases, reads, live_frames;
static unsigned pause_calls, resume_calls, starts, writes, silences, drains;
static unsigned discontinuity_logs, resume_failure_logs, success_logs;
static bool locked, muted, amplifier;
static unsigned char *allocation;
static int16_t *recorded;
static int16_t expected[RECORD_PLAYBACK_SAMPLES];
static size_t expected_samples;
static record_playback_gain_t expected_gain;
static record_playback_output_t expected_output;

static void log_message(const char *format, ...)
{
    if (strstr(format, "Recording 2s now")) capture_start_us = now_us;
    if (strstr(format, "Recording discontinuity")) ++discontinuity_logs;
    if (strstr(format, "Capture resume failed")) ++resume_failure_logs;
    if (strstr(format, "Playback finished")) ++success_logs;
    if (strstr(format, "Playback PCM:")) {
        va_list values;
        va_start(values, format);
        assert(va_arg(values, unsigned) == record_playback_output_rms(&expected_output));
        assert(va_arg(values, unsigned) == expected_output.peak);
        assert(va_arg(values, unsigned) == expected_output.limited_samples);
        assert(va_arg(values, unsigned) == expected_samples);
        va_end(values);
        assert(writes == 200 && expected_output.samples == expected_samples);
        if (scenario == LOW_LEVEL) assert(record_playback_output_rms(&expected_output) > 1900);
    }
}

static const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "mock_error";
}

static int64_t esp_timer_get_time(void) { return now_us; }

static void *heap_caps_malloc(size_t bytes, unsigned caps)
{
    assert(bytes == 192000 && bytes == RECORD_PLAYBACK_SAMPLES * sizeof(int16_t));
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(!allocations++);
    if (scenario == ALLOC_FAIL) return NULL;
    allocation = malloc(bytes + 32);
    assert(allocation);
    memset(allocation, 0xa5, bytes + 32);
    recorded = (int16_t *)(allocation + 16);
    return recorded;
}

static void heap_caps_free(void *memory)
{
    assert(memory == recorded && !locked && !frees++);
    for (unsigned i = 0; i < 16; ++i) {
        assert(allocation[i] == 0xa5);
        assert(allocation[16 + RECORD_PLAYBACK_SAMPLES * sizeof(int16_t) + i] == 0xa5);
    }
    free(allocation);
    allocation = NULL;
    recorded = NULL;
}

static bool digits_audio_speaker_acquire(uint32_t timeout_ms)
{
    assert(recorded && !locked && timeout_ms == 1500 && !acquisitions++);
    if (scenario == ACQUIRE_FAIL) {
        now_us += 1500000;
        return false;
    }
    locked = true;
    return true;
}

static void digits_audio_speaker_release(void)
{
    assert(locked && !amplifier && !releases++);
    locked = false;
}

static int16_t sample_value(uint32_t sequence, size_t sample)
{
    if (scenario == LOW_LEVEL) return (int16_t)(140 + (sample % 2 ? 200 : -200));
    uint32_t bits = sequence * 104729U + (uint32_t)sample * 173U;
    return (int16_t)((int32_t)(bits & 65535U) - 32768);
}

esp_err_t digits_microphone_read(digits_microphone_frame_t *frame,
                               uint32_t timeout_ms)
{
    assert(locked && !muted && !amplifier && timeout_ms == 100);
    assert(++reads < 200); // A bad/stale input cannot make the worker loop forever.
    if (scenario == RECORD_TIMEOUT && reads == 8) {
        now_us += 100000;
        return ESP_ERR_TIMEOUT;
    }
    frame->flags = 0;
    if (reads <= 2) {
        // Already queued before recording: return immediately without time advancing.
        frame->captured_at_us = capture_start_us - (int64_t)reads * 20000;
        frame->sequence = reads;
    } else {
        now_us += 20000;
        ++live_frames;
        frame->captured_at_us = now_us;
        frame->sequence = scenario == WRAP_SEQUENCE ? UINT32_MAX - 3U + live_frames : 99U + live_frames;
        // The first fresh frame can carry an old queue-overwrite flag. Discard it.
        if (live_frames == 1 || scenario == RECORD_DEADLINE)
            frame->flags = DIGITS_MIC_FRAME_DISCONTINUITY;
        if (live_frames == 32 && scenario == RECORD_DISCONTINUITY)
            frame->flags = DIGITS_MIC_FRAME_DISCONTINUITY;
        if (live_frames == 32 && scenario == RECORD_GAP) ++frame->sequence;
    }
    for (size_t i = 0; i < DIGITS_MIC_FRAME_SAMPLES; ++i)
        frame->pcm[i] = sample_value(frame->sequence, i);
    if (reads > 2 && !frame->flags && expected_samples < RECORD_PLAYBACK_SAMPLES) {
        memcpy(expected + expected_samples, frame->pcm, sizeof(frame->pcm));
        expected_samples += DIGITS_MIC_FRAME_SAMPLES;
    }
    return ESP_OK;
}

esp_err_t digits_microphone_pause(bool paused, uint32_t timeout_ms)
{
    assert(locked && !amplifier && timeout_ms == 1000);
    if (paused) {
        assert(!pause_calls++ && expected_samples == RECORD_PLAYBACK_SAMPLES);
        assert(!memcmp(recorded, expected, sizeof(expected)));
        muted = true;
        now_us += 5000;
        if (scenario == PAUSE_FAIL) return ESP_FAIL;
        if (scenario == PAUSE_TIMEOUT) {
            // Model a late ADC mute committing after the caller's timeout.
            now_us += 1000000;
            return ESP_ERR_TIMEOUT;
        }
    } else {
        assert(pause_calls == 1 && !resume_calls++);
        if (scenario == RESUME_FAIL) return ESP_FAIL;
        muted = false;
        now_us += 5000;
    }
    return ESP_OK;
}

static esp_err_t digits_audio_speaker_start(void)
{
    assert(locked && muted && !amplifier && !starts++);
    assert(expected_samples == RECORD_PLAYBACK_SAMPLES);
    assert(record_playback_gain_prepare(expected, expected_samples, &expected_gain));
    if (scenario == LOW_LEVEL) {
        assert(expected_gain.dc_offset == 140 && expected_gain.ac_rms == 200);
        assert(expected_gain.gain_q8 > 2500 && expected_gain.gain_q8 < 2700);
    }
    amplifier = true;
    speaker_start_us = now_us;
    return scenario == SPEAKER_START_FAIL ? ESP_FAIL : ESP_OK;
}

static esp_err_t digits_audio_speaker_write(const int16_t *stereo, size_t bytes,
                                          size_t *written, uint32_t timeout_ms)
{
    assert(locked && muted && amplifier && timeout_ms == 50);
    assert(bytes == 2 * DIGITS_AUDIO_DMA_FRAMES * sizeof(int16_t));
    assert(writes < 200);
    int16_t expected_stereo[DIGITS_AUDIO_DMA_FRAMES * 2];
    record_playback_render_gain(expected_stereo, expected, expected_samples,
        writes * DIGITS_AUDIO_DMA_FRAMES, DIGITS_AUDIO_DMA_FRAMES, &expected_gain, &expected_output);
    assert(!memcmp(stereo, expected_stereo, sizeof(expected_stereo)));
    for (size_t i = 0; i < DIGITS_AUDIO_DMA_FRAMES; ++i) {
        assert(stereo[2 * i] == stereo[2 * i + 1]);
        assert(stereo[2 * i] >= -8192 && stereo[2 * i] <= 8192);
    }
    ++writes;
    if (scenario == WRITE_TIMEOUT && writes == 8) {
        now_us += 50000;
        *written = 0;
        return ESP_ERR_TIMEOUT;
    }
    if (scenario == WRITE_PARTIAL && writes == 200) {
        *written = bytes - 4;
        return ESP_OK;
    }
    // Two available descriptors let the writer run 20 ms ahead of the speaker.
    if (scenario == PLAYBACK_DEADLINE) now_us += 20000;
    else if (writes > 2) now_us += 10000;
    *written = bytes;
    return ESP_OK;
}

static void vTaskDelay(unsigned ticks)
{
    assert(locked && muted && amplifier && writes == 200 && !drains++);
    assert(ticks >= 3 && ticks <= 4); // Two DMA blocks plus scheduling margin.
    now_us += (int64_t)ticks * 10000;
}

static bool digits_audio_speaker_silence(void)
{
    assert(locked && !silences++);
    amplifier = false;
    gate_off_us = now_us;
    now_us += 20000;
    return scenario != SILENCE_FAIL;
}
'''

suffix = r'''
static void reset(scenario_t next)
{
    scenario = next;
    now_us = 1000000;
    capture_start_us = speaker_start_us = gate_off_us = 0;
    allocations = frees = acquisitions = releases = reads = live_frames = 0;
    pause_calls = resume_calls = starts = writes = silences = drains = 0;
    discontinuity_logs = resume_failure_logs = success_logs = 0;
    locked = muted = amplifier = false;
    allocation = NULL;
    recorded = NULL;
    expected_samples = 0;
    memset(&expected_output, 0, sizeof(expected_output));
    memset(expected, 0, sizeof(expected));
}

static void check(scenario_t next, esp_err_t expected_error)
{
    reset(next);
    assert(run_record_playback() == expected_error);
    assert(!locked && !amplifier && !recorded && !allocation);
    assert(allocations == 1);
    if (next == ALLOC_FAIL) {
        assert(!frees && !acquisitions && !releases && !silences && !reads);
        return;
    }
    assert(frees == 1 && acquisitions == 1);
    if (next == ACQUIRE_FAIL) {
        assert(!releases && !silences && !reads);
        return;
    }
    assert(releases == 1 && silences == 1);
    if (next == RECORD_TIMEOUT || next == RECORD_DEADLINE ||
        next == RECORD_GAP || next == RECORD_DISCONTINUITY) {
        assert(!pause_calls && !resume_calls && !starts && !writes);
        if (next == RECORD_DEADLINE) assert(reads <= 153);
        if (next == RECORD_GAP || next == RECORD_DISCONTINUITY)
            assert(discontinuity_logs == 1);
    } else {
        assert(pause_calls == 1 && resume_calls == 1);
        assert(expected_samples == 96000 && reads == 103);
        if (next == PAUSE_FAIL || next == PAUSE_TIMEOUT) assert(!starts && !writes);
        else if (next == SPEAKER_START_FAIL) assert(starts == 1 && !writes);
        else if (next == WRITE_TIMEOUT) assert(starts == 1 && writes == 8 && !drains);
        else if (next == WRITE_PARTIAL) assert(starts == 1 && writes == 200 && !drains);
        else if (next == PLAYBACK_DEADLINE) assert(starts == 1 && writes == 150 && !drains);
        else {
            assert(starts == 1 && writes == 200 && drains == 1);
            // The saved 96000 samples finish at the speaker before its gate closes.
            assert(gate_off_us - speaker_start_us >= 2000000);
            assert(gate_off_us - speaker_start_us <= 2040000);
        }
    }
    if (next == RESUME_FAIL) {
        assert(muted && resume_failure_logs == 1 && !success_logs);
    } else {
        assert(!muted && !resume_failure_logs);
        assert(success_logs == (expected_error == ESP_OK ? 1U : 0U));
    }
}

int main(void)
{
    check(NORMAL, ESP_OK);
    check(LOW_LEVEL, ESP_OK);
    check(WRAP_SEQUENCE, ESP_OK);
    check(ALLOC_FAIL, ESP_ERR_NO_MEM);
    check(ACQUIRE_FAIL, ESP_ERR_TIMEOUT);
    check(RECORD_TIMEOUT, ESP_ERR_TIMEOUT);
    check(RECORD_DEADLINE, ESP_ERR_TIMEOUT);
    check(RECORD_GAP, ESP_ERR_INVALID_STATE);
    check(RECORD_DISCONTINUITY, ESP_ERR_INVALID_STATE);
    check(PAUSE_FAIL, ESP_FAIL);
    check(PAUSE_TIMEOUT, ESP_ERR_TIMEOUT);
    check(SPEAKER_START_FAIL, ESP_FAIL);
    check(WRITE_TIMEOUT, ESP_ERR_TIMEOUT);
    check(WRITE_PARTIAL, ESP_ERR_INVALID_SIZE);
    check(PLAYBACK_DEADLINE, ESP_ERR_TIMEOUT);
    check(SILENCE_FAIL, ESP_FAIL);
    check(RESUME_FAIL, ESP_FAIL);
    puts("PASS: exactly 96000 fresh samples/200 stereo DMA blocks, bounded fixed gain "
         "for quiet offset PCM, accurate output metrics, stale/initial-loss "
         "discard, sequence wrap, saved-audio playback, DMA drain, bounded deadlines, "
         "PSRAM-only allocation, ownership and cleanup across 14 failure paths");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="digits-record-playback-") as directory:
    directory = Path(directory)
    (directory / "esp_err.h").write_text(
        "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n"
        "#define ESP_ERR_TIMEOUT 0x107\n#define ESP_ERR_NO_MEM 0x101\n"
        "#define ESP_ERR_INVALID_STATE 0x103\n#define ESP_ERR_INVALID_SIZE 0x104\n"
    )
    test = directory / "record_playback_transaction_test.c"
    test.write_text(prefix + transaction + suffix)
    binary = test.with_suffix("")
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
         "-I", str(directory), "-I", str(repo / "esp32/main"), str(test),
         str(repo / "esp32/main/record_playback_pcm.c"), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
