from pathlib import Path
import subprocess
import tempfile

# Compile the production KEY1 recording transaction and trigger loop against
# finite capture/button/DMA models. Physical I2C and acoustics are not modeled.
repo = Path(__file__).resolve().parents[2]
source = (repo / "esp32/main/button_record_playback.c").read_text()
transaction = source[
    source.index("#define RECORD_SECONDS"):
    source.index("static void button_record_worker(void *arg)")
]
worker = source[
    source.index("static void button_record_worker(void *arg)"):
    source.index("\n#endif", source.index("static void button_record_worker(void *arg)"))
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
#include "record_button.h"
#include "record_playback_pcm.h"
#define configTICK_RATE_HZ 100
#define pdMS_TO_TICKS(ms) ((ms) / 10)
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define ESP_LOGI(tag, ...) log_message(__VA_ARGS__)
#define ESP_LOGW(tag, ...) log_message(__VA_ARGS__)
#define ESP_LOGE(tag, ...) log_message(__VA_ARGS__)
#define MAX_FRAMES 500
#define MAX_SAMPLES (MAX_FRAMES * DIGITS_MIC_FRAME_SAMPLES)

typedef enum {
    RELEASE, LOW_LEVEL, PRE_RESERVATION_FRAME, WARMUP_RELEASE, SHORT_TAP, WARMUP_TAP, WRAP_SEQUENCE,
    MAXIMUM_WAIT_RELEASE, MAXIMUM_DEBOUNCE_TRIM, REACQUIRE_BUSY,
    WAIT_BUTTON_ERROR, ALLOC_FAIL, ACQUIRE_FAIL, INITIAL_TIMEOUT,
    BUTTON_ERROR, PRESS_CHANGED, READ_ERROR, RECORD_GAP, RECORD_FLAG,
    PAUSE_FAIL, PAUSE_TIMEOUT, START_FAIL, WRITE_TIMEOUT, WRITE_PARTIAL,
    PLAYBACK_DEADLINE, SILENCE_FAIL, RESUME_FAIL,
} scenario_t;

static scenario_t scenario;
static int64_t now_us, raw_release_us, confirmed_release_us, started_us, stopped_us;
static unsigned allocations, frees, acquisitions, releases, snapshots, reads, live_frames;
static unsigned pauses, resumes, starts, writes, silences, drains, wait_ticks;
static unsigned candidate_frames, selected_frames, warmup_timeouts;
static unsigned short_logs, limit_logs, complete_logs, external_ring_tests;
static bool locked, muted, amplifier, external_busy;
static unsigned char *allocation;
static int16_t *recorded;
static int16_t expected[MAX_SAMPLES];
static int64_t candidate_times[MAX_FRAMES];
static digits_record_button_state_t press;
static record_playback_gain_t expected_gain;
static record_playback_output_t expected_output;

static void log_message(const char *format, ...)
{
    if (strstr(format, "nothing to play")) ++short_logs;
    if (strstr(format, "10s recording limit reached")) ++limit_logs;
    if (strstr(format, "playback finished")) ++complete_logs;
    if (strstr(format, "Playback PCM:")) {
        va_list values;
        va_start(values, format);
        assert(va_arg(values, unsigned) == record_playback_output_rms(&expected_output));
        assert(va_arg(values, unsigned) == expected_output.peak);
        assert(va_arg(values, unsigned) == expected_output.limited_samples);
        assert(va_arg(values, unsigned) == selected_frames * DIGITS_MIC_FRAME_SAMPLES);
        va_end(values);
        assert(writes == selected_frames * 2);
        assert(expected_output.samples == selected_frames * DIGITS_MIC_FRAME_SAMPLES);
        if (scenario == LOW_LEVEL) assert(record_playback_output_rms(&expected_output) > 1900);
    }
}

static const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "mock_error";
}

static int64_t esp_timer_get_time(void) { return now_us; }

static void *heap_caps_malloc(size_t bytes, unsigned caps)
{
    assert(bytes == MAX_SAMPLES * sizeof(int16_t) + MAX_FRAMES * sizeof(int64_t));
    assert(bytes == 964000 && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(!allocations++);
    if (scenario == ALLOC_FAIL) return NULL;
    allocation = malloc(bytes + 32);
    assert(allocation);
    memset(allocation, 0xa5, bytes + 32);
    recorded = (int16_t *)(allocation + 16);
    return recorded;
}

static void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, unsigned caps)
{
    assert(alignment == _Alignof(int64_t) && alignment == 8);
    void *memory = heap_caps_malloc(bytes, caps);
    assert((uintptr_t)memory % alignment == 0);
    return memory;
}

static void heap_caps_free(void *memory)
{
    assert(memory == recorded && !locked && !frees++);
    const size_t bytes = MAX_SAMPLES * sizeof(int16_t) + MAX_FRAMES * sizeof(int64_t);
    for (unsigned i = 0; i < 16; ++i) {
        assert(allocation[i] == 0xa5);
        assert(allocation[16 + bytes + i] == 0xa5);
    }
    free(allocation);
    allocation = NULL;
    recorded = NULL;
}

static bool digits_audio_speaker_acquire(uint32_t timeout_ms)
{
    assert(recorded && !locked && !amplifier && timeout_ms == 0);
    ++acquisitions;
    assert(acquisitions <= 2);
    if (scenario == ACQUIRE_FAIL || (scenario == REACQUIRE_BUSY && acquisitions == 2)) {
        external_busy = true;
        return false;
    }
    assert(!external_busy);
    locked = true;
    return true;
}

static void digits_audio_speaker_release(void)
{
    assert(locked && !amplifier && !external_busy);
    ++releases;
    locked = false;
}

void digits_record_button_snapshot(digits_record_button_state_t *button)
{
    assert(++snapshots < 1500);
    *button = press;
    button->pressed = now_us < confirmed_release_us;
    if (!button->pressed) button->released_at_us = raw_release_us;
    if (scenario == BUTTON_ERROR && now_us >= press.pressed_at_us + 270000) {
        button->valid = false;
        button->error = ESP_ERR_INVALID_STATE;
    }
    if (scenario == PRESS_CHANGED && now_us >= press.pressed_at_us + 270000)
        ++button->press_sequence;
    if (scenario == WAIT_BUTTON_ERROR && wait_ticks) {
        button->valid = false;
        button->error = ESP_ERR_INVALID_STATE;
    }
}

static int16_t sample_value(uint32_t sequence, size_t index)
{
    if (scenario == LOW_LEVEL) return (int16_t)(140 + (index % 2 ? 200 : -200));
    uint32_t bits = sequence * 104729U + (uint32_t)index * 173U;
    return (int16_t)((int32_t)(bits & 65535U) - 32768);
}

esp_err_t digits_microphone_read(digits_microphone_frame_t *frame, uint32_t timeout_ms)
{
    assert(locked && !muted && !amplifier && timeout_ms == 100);
    assert(++reads < 550);
    if (scenario == INITIAL_TIMEOUT ||
        ((scenario == WARMUP_RELEASE || scenario == WARMUP_TAP) && warmup_timeouts < 4)) {
        ++warmup_timeouts;
        now_us += 100000;
        return ESP_ERR_TIMEOUT;
    }
    if (scenario == READ_ERROR && reads == 12) {
        now_us += 100000;
        return ESP_FAIL;
    }
    frame->flags = 0;
    unsigned delivered = reads - warmup_timeouts;
    if (delivered <= 2) {
        frame->captured_at_us = press.pressed_at_us - (int64_t)delivered * 20000;
        frame->sequence = delivered;
    } else {
        if (live_frames) now_us += 20000;
        ++live_frames;
        frame->captured_at_us = now_us;
        frame->sequence = scenario == WRAP_SEQUENCE ? UINT32_MAX - 3U + live_frames : 99U + live_frames;
        // Initial overwrite flag can reflect the period without a consumer.
        if ((live_frames == 1 && scenario != PRE_RESERVATION_FRAME) ||
            (scenario == RECORD_FLAG && live_frames == 8))
            frame->flags = DIGITS_MIC_FRAME_DISCONTINUITY;
        if (scenario == RECORD_GAP && live_frames == 8) ++frame->sequence;
    }
    for (size_t i = 0; i < DIGITS_MIC_FRAME_SAMPLES; ++i)
        frame->pcm[i] = sample_value(frame->sequence, i);
    // Even a clean queued frame must contain only audio after speaker reservation.
    // Invocation is at 1030000 us, 30 ms after the original physical press.
    if (delivered > 2 && !frame->flags && frame->captured_at_us >= 1050000 &&
        candidate_frames < MAX_FRAMES) {
        memcpy(expected + candidate_frames * DIGITS_MIC_FRAME_SAMPLES,
               frame->pcm, sizeof(frame->pcm));
        candidate_times[candidate_frames++] = frame->captured_at_us;
    }
    return ESP_OK;
}

esp_err_t digits_microphone_pause(bool paused, uint32_t timeout_ms)
{
    assert(!amplifier && timeout_ms == 1000);
    if (paused) {
        assert(locked && !pauses++ && candidate_frames);
        muted = true;
        now_us += 5000;
        if (scenario == PAUSE_FAIL) return ESP_FAIL;
        if (scenario == PAUSE_TIMEOUT) {
            // A mute committed late must be superseded by cleanup's resume.
            now_us += 1000000;
            return ESP_ERR_TIMEOUT;
        }
    } else {
        assert(pauses == 1 && !resumes++);
        if (scenario == RESUME_FAIL) return ESP_FAIL;
        muted = false;
        now_us += 5000;
    }
    return ESP_OK;
}

static esp_err_t digits_audio_speaker_start(void)
{
    assert(locked && muted && !amplifier && !external_busy && !starts++);
    // Speaker playback starts only after a confirmed release, including at cap.
    assert(now_us >= confirmed_release_us);
    selected_frames = 0;
    while (selected_frames < candidate_frames && candidate_times[selected_frames] <= raw_release_us)
        ++selected_frames;
    assert(selected_frames > 0 && selected_frames <= MAX_FRAMES);
    assert(!memcmp(recorded, expected, selected_frames * DIGITS_MIC_FRAME_SAMPLES * sizeof(int16_t)));
    int64_t *saved_times = (int64_t *)((unsigned char *)recorded + MAX_SAMPLES * sizeof(int16_t));
    assert(!memcmp(saved_times, candidate_times, selected_frames * sizeof(int64_t)));
    assert(record_playback_gain_prepare(expected,
        selected_frames * DIGITS_MIC_FRAME_SAMPLES, &expected_gain));
    if (scenario == LOW_LEVEL) {
        assert(expected_gain.dc_offset == 140 && expected_gain.ac_rms == 200);
        assert(expected_gain.gain_q8 > 2500 && expected_gain.gain_q8 < 2700);
    }
    amplifier = true;
    started_us = now_us;
    return scenario == START_FAIL ? ESP_FAIL : ESP_OK;
}

static esp_err_t digits_audio_speaker_write(const int16_t *stereo, size_t bytes,
                                          size_t *written, uint32_t timeout_ms)
{
    assert(locked && muted && amplifier && !external_busy && timeout_ms == 50);
    assert(bytes == DIGITS_AUDIO_DMA_FRAMES * 2 * sizeof(int16_t));
    assert(writes < selected_frames * 2 && writes < 1000);
    int16_t expected_stereo[DIGITS_AUDIO_DMA_FRAMES * 2];
    record_playback_render_gain(expected_stereo, expected,
        selected_frames * DIGITS_MIC_FRAME_SAMPLES,
        writes * DIGITS_AUDIO_DMA_FRAMES, DIGITS_AUDIO_DMA_FRAMES, &expected_gain, &expected_output);
    assert(!memcmp(stereo, expected_stereo, sizeof(expected_stereo)));
    for (size_t i = 0; i < DIGITS_AUDIO_DMA_FRAMES; ++i) {
        assert(stereo[2 * i] == stereo[2 * i + 1]);
        assert(stereo[2 * i] >= -8192 && stereo[2 * i] <= 8192);
    }
    ++writes;
    if (scenario == WRITE_TIMEOUT && writes == 5) {
        now_us += 50000;
        *written = 0;
        return ESP_ERR_TIMEOUT;
    }
    if (scenario == WRITE_PARTIAL && writes == 5) {
        *written = bytes - 4;
        return ESP_OK;
    }
    if (scenario == PLAYBACK_DEADLINE) now_us += 50000;
    else if (writes > 2) now_us += 10000;
    *written = bytes;
    return ESP_OK;
}

static void vTaskDelay(unsigned ticks)
{
    if (ticks == 1) {
        assert(!locked && !amplifier && muted && !starts);
        assert(++wait_ticks < 200);
        // Simulate a separate Ring Test using TX while the capped recording waits.
        if (scenario != MAXIMUM_DEBOUNCE_TRIM) {
            if (wait_ticks == 1) { external_busy = true; ++external_ring_tests; }
            if (wait_ticks == 100) external_busy = false;
        }
    } else {
        assert(ticks >= 3 && ticks <= 4 && locked && muted && amplifier);
        assert(writes == selected_frames * 2 && !drains++);
    }
    now_us += (int64_t)ticks * 10000;
}

static bool digits_audio_speaker_silence(void)
{
    // Cleanup must never mute a Ring Test that owns TX after cap unlock.
    assert(locked && !external_busy);
    ++silences;
    amplifier = false;
    stopped_us = now_us;
    now_us += 20000;
    return scenario != SILENCE_FAIL;
}
'''

suffix = r'''
static bool capped(scenario_t value)
{
    return value == MAXIMUM_WAIT_RELEASE || value == MAXIMUM_DEBOUNCE_TRIM ||
           value == REACQUIRE_BUSY || value == WAIT_BUTTON_ERROR;
}

static void reset(scenario_t next)
{
    scenario = next;
    now_us = 1030000;
    raw_release_us = 1425000;
    if (capped(next)) raw_release_us = 11055000;
    if (next == MAXIMUM_WAIT_RELEASE) raw_release_us = 12555000;
    if (next == MAXIMUM_DEBOUNCE_TRIM) raw_release_us = 11015000;
    if (next == SHORT_TAP) raw_release_us = 1035000;
    if (next == WARMUP_TAP) raw_release_us = 1100000;
    if (next == WARMUP_RELEASE) raw_release_us = 2205000;
    if (next == INITIAL_TIMEOUT) raw_release_us = 3000000;
    confirmed_release_us = raw_release_us + 30000;
    started_us = stopped_us = 0;
    allocations = frees = acquisitions = releases = snapshots = reads = live_frames = 0;
    pauses = resumes = starts = writes = silences = drains = wait_ticks = 0;
    candidate_frames = selected_frames = warmup_timeouts = 0;
    short_logs = limit_logs = complete_logs = external_ring_tests = 0;
    locked = muted = amplifier = external_busy = false;
    allocation = NULL;
    recorded = NULL;
    memset(expected, 0, sizeof(expected));
    memset(candidate_times, 0, sizeof(candidate_times));
    memset(&expected_output, 0, sizeof(expected_output));
    press = (digits_record_button_state_t){
        .valid = true, .pressed = true, .press_sequence = 7,
        .pressed_at_us = 1000000, .released_at_us = 0, .error = ESP_OK,
    };
}

static void check(scenario_t next, esp_err_t expected_error)
{
    reset(next);
    assert(run_button_recording(&press) == expected_error);
    assert(!locked && !amplifier && !recorded && !allocation && allocations == 1);
    if (next == ALLOC_FAIL) {
        assert(!frees && !acquisitions && !releases && !silences && !reads);
        return;
    }
    assert(frees == 1);
    if (next == ACQUIRE_FAIL) {
        assert(acquisitions == 1 && external_busy && !releases && !silences && !reads);
        return;
    }
    assert(releases == acquisitions - (next == REACQUIRE_BUSY ? 1U : 0U));
    assert(silences == releases);
    if (next == SHORT_TAP || next == WARMUP_TAP) {
        assert(short_logs == 1 && !pauses && !resumes && !starts && !writes);
    } else if (next == INITIAL_TIMEOUT || next == BUTTON_ERROR || next == PRESS_CHANGED ||
               next == READ_ERROR || next == RECORD_GAP || next == RECORD_FLAG) {
        assert(!pauses && !resumes && !starts && !writes);
        if (next == INITIAL_TIMEOUT) assert(warmup_timeouts == 10);
    } else {
        assert(pauses == 1 && resumes == 1);
        if (next == PAUSE_FAIL || next == PAUSE_TIMEOUT || next == REACQUIRE_BUSY ||
            next == WAIT_BUTTON_ERROR) assert(!starts && !writes);
        else if (next == START_FAIL) assert(starts == 1 && !writes);
        else if (next == WRITE_TIMEOUT || next == WRITE_PARTIAL) assert(writes == 5 && !drains);
        else if (next == PLAYBACK_DEADLINE) {
            assert(writes < selected_frames * 2 && !drains);
            assert(now_us - started_us <= (int64_t)selected_frames * 20000 + 1100000);
        } else {
            assert(starts == 1 && writes == selected_frames * 2 && drains == 1);
            assert(stopped_us - started_us >= (int64_t)selected_frames * 20000);
            assert(stopped_us - started_us <= (int64_t)selected_frames * 20000 + 40000);
        }
    }
    if (capped(next)) {
        assert(candidate_frames == 500 && live_frames == 501 && limit_logs == 1);
        assert(wait_ticks > 0);
        assert(external_ring_tests == (next == MAXIMUM_DEBOUNCE_TRIM ? 0U : 1U));
        if (next == MAXIMUM_WAIT_RELEASE) assert(selected_frames == 500 && writes == 1000);
        if (next == MAXIMUM_DEBOUNCE_TRIM) assert(selected_frames == 499 && writes == 998);
        if (next == REACQUIRE_BUSY) assert(external_busy && silences == 1 && releases == 1);
    }
    if (next == RELEASE || next == LOW_LEVEL || next == PRE_RESERVATION_FRAME || next == WRAP_SEQUENCE) {
        assert(selected_frames == 19 && writes == 38);
        assert(candidate_frames > selected_frames); // Release-debounce tail actually removed.
    }
    if (next == WARMUP_RELEASE) assert(warmup_timeouts == 4 && writes > 0);
    if (next == RESUME_FAIL) assert(muted && !complete_logs);
    else assert(!muted);
    if (expected_error != ESP_OK) assert(!complete_logs);
}

int main(void)
{
    check(RELEASE, ESP_OK);
    check(LOW_LEVEL, ESP_OK);
    check(PRE_RESERVATION_FRAME, ESP_OK);
    check(WARMUP_RELEASE, ESP_OK);
    check(SHORT_TAP, ESP_OK);
    check(WARMUP_TAP, ESP_OK);
    check(WRAP_SEQUENCE, ESP_OK);
    check(MAXIMUM_WAIT_RELEASE, ESP_OK);
    check(MAXIMUM_DEBOUNCE_TRIM, ESP_OK);
    check(REACQUIRE_BUSY, ESP_ERR_TIMEOUT);
    check(WAIT_BUTTON_ERROR, ESP_ERR_INVALID_STATE);
    check(ALLOC_FAIL, ESP_ERR_NO_MEM);
    check(ACQUIRE_FAIL, ESP_ERR_TIMEOUT);
    check(INITIAL_TIMEOUT, ESP_ERR_TIMEOUT);
    check(BUTTON_ERROR, ESP_ERR_INVALID_STATE);
    check(PRESS_CHANGED, ESP_ERR_INVALID_STATE);
    check(READ_ERROR, ESP_FAIL);
    check(RECORD_GAP, ESP_ERR_INVALID_STATE);
    check(RECORD_FLAG, ESP_ERR_INVALID_STATE);
    check(PAUSE_FAIL, ESP_FAIL);
    check(PAUSE_TIMEOUT, ESP_ERR_TIMEOUT);
    check(START_FAIL, ESP_FAIL);
    check(WRITE_TIMEOUT, ESP_ERR_TIMEOUT);
    check(WRITE_PARTIAL, ESP_ERR_INVALID_SIZE);
    check(PLAYBACK_DEADLINE, ESP_ERR_TIMEOUT);
    check(SILENCE_FAIL, ESP_FAIL);
    check(RESUME_FAIL, ESP_FAIL);
    puts("PASS: KEY1 release-driven variable PCM, debounce-tail trimming, initial "
         "warmup/taps, sequence wrap, exact10s cap without autoplay, RingTest "
         "reservation handoff, pre-reservation PCM discard, quiet offset PCM gain, "
         "output metrics, conservative stereo, DMA drain and error cleanup");
    return 0;
}
'''

worker_prefix = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "record_button.h"
#define pdMS_TO_TICKS(ms) ((ms) / 10)
#define ESP_LOGI(...) ((void)0)
static jmp_buf finished;
static unsigned next_state, invocations;
static uint32_t seen[3];
static const struct { bool valid, pressed; uint32_t sequence; } states[] = {
    {true, true, 7}, {true, true, 7}, // Held at boot: require a release first.
    {true, false, 7}, {true, true, 8},
    {true, true, 9}, // Another press happened during playback: ignore it.
    {true, false, 9}, {true, true, 9}, {true, true, 10},
    {false, true, 11}, {true, true, 12}, // Recovery while held must not record.
    {true, false, 12}, {true, true, 13},
};
void digits_record_button_snapshot(digits_record_button_state_t *state)
{
    assert(next_state < sizeof(states) / sizeof(states[0]));
    *state = (digits_record_button_state_t){
        .valid = states[next_state].valid, .pressed = states[next_state].pressed,
        .press_sequence = states[next_state].sequence,
    };
    ++next_state;
}
static esp_err_t run_button_recording(const digits_record_button_state_t *state)
{
    assert(invocations < 3);
    seen[invocations++] = state->press_sequence;
    return state->press_sequence == 10 ? ESP_FAIL : ESP_OK;
}
static void vTaskDelay(unsigned ticks)
{
    assert(ticks == 1);
    if (next_state == sizeof(states) / sizeof(states[0])) longjmp(finished, 1);
}
'''

worker_suffix = r'''
int main(void)
{
    if (!setjmp(finished)) button_record_worker(NULL);
    assert(invocations == 3 && seen[0] == 8 && seen[1] == 10 && seen[2] == 13);
    puts("PASS: KEY1 worker requires release before arm, ignores presses during "
         "playback and recovers from invalid state/test errors without retriggering");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="digits-button-record-") as directory:
    directory = Path(directory)
    (directory / "esp_err.h").write_text(
        "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n"
        "#define ESP_ERR_TIMEOUT 0x107\n#define ESP_ERR_NO_MEM 0x101\n"
        "#define ESP_ERR_INVALID_STATE 0x103\n#define ESP_ERR_INVALID_SIZE 0x104\n"
    )
    for name, contents, renderer in (
        ("transaction", prefix + transaction + suffix, True),
        ("worker", worker_prefix + worker + worker_suffix, False),
    ):
        test = directory / f"button_record_{name}_test.c"
        test.write_text(contents)
        binary = test.with_suffix("")
        command = [
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-pedantic", "-I", str(directory), "-I", str(repo / "esp32/main"), str(test),
        ]
        if renderer:
            command.append(str(repo / "esp32/main/record_playback_pcm.c"))
        subprocess.run(command + ["-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
