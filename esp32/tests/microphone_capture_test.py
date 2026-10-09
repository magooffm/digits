from pathlib import Path
import argparse
import subprocess
import tempfile

# Compile the actual capture loop with finite host I2S reads and copy queues.
# This checks buffer ownership/continuity, not the physical codec or RTOS.
repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description="Microphone capture continuity regression")
parser.add_argument("--source", type=Path, default=repo / "esp32/main/microphone.c")
source = parser.parse_args().source.read_text()
defines = source[source.index("#define FRAME_QUEUE_LENGTH"):source.index("typedef struct {")]
state = source[source.index("typedef struct {"):source.index("static void report_levels(")]
worker = source[source.index("static void capture_worker("):source.index("static void release_microphone(")]
report_worker = source[source.index("static void report_worker("):source.index("static void capture_worker(")]
pause_api = source[source.index("esp_err_t digits_microphone_pause("):]

prefix = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "microphone.h"
#include "microphone_pcm.h"
typedef void audio_codec_ctrl_if_t;
typedef struct audio_codec_if audio_codec_if_t;
struct audio_codec_if {
    int (*mute_mic)(const audio_codec_if_t *, bool);
    int (*get_reg)(const audio_codec_if_t *, int, int *);
};
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *i2s_chan_handle_t;
typedef void i2s_event_data_t;
typedef int portMUX_TYPE;
#define IRAM_ATTR
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define portENTER_CRITICAL_ISR(mux) ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux) ((void)(mux))
#define CONFIG_DIGITS_MIC_MONO_LEFT 1
#define CONFIG_DIGITS_MIC_MONO_RIGHT 0
#define CONFIG_DIGITS_MIC_MONO_AVERAGE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) ((unsigned)(ms) / 10)
#define portMAX_DELAY 0xffffffffu
#define ESP_CODEC_DEV_OK 0
#define ESP_LOGI(...) log_info(__VA_ARGS__)
#define ESP_LOGE(...) log_error(__VA_ARGS__)
static int64_t now_us;
static jmp_buf finished;
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(unsigned ticks);
static int xSemaphoreTake(SemaphoreHandle_t, unsigned);
static void xSemaphoreGive(SemaphoreHandle_t);
static void log_info(const char *, const char *, ...);
static void log_error(const char *, const char *, ...);
static const char *esp_err_to_name(int error) { (void)error; return "fake"; }
static i2s_chan_handle_t digits_audio_rx_channel(void) { return (void *)3; }
static esp_err_t i2s_channel_read(i2s_chan_handle_t, void *, size_t, size_t *, unsigned);
static int xQueueSend(QueueHandle_t, const void *, unsigned);
static int xQueueReceive(QueueHandle_t, void *, unsigned);
static int xQueueOverwrite(QueueHandle_t, const void *);
static int xQueueReset(QueueHandle_t);
'''

suffix = r'''
#define RAW_BYTES (DIGITS_MIC_FRAME_SAMPLES * 2 * sizeof(int16_t))
static struct {
    size_t bytes;
    esp_err_t error;
    unsigned lost_frames;
} reads[96];
static int read_count, next_read;
static size_t source_byte;
static int16_t input[96 * DIGITS_MIC_FRAME_SAMPLES * 2];
static digits_microphone_frame_t queue[FRAME_QUEUE_LENGTH];
static size_t queue_head, queue_count, maximum_queue_count;
static unsigned reports_written;
static microphone_report_t last_report;
static microphone_report_t report_history[16];
static unsigned next_report, pause_logs, error_logs;
static int pause_at, resume_at, mute_calls, mute_result, mute_read_error;
static unsigned sequence_at_pause;
static bool codec_muted;
static bool rpc_mode, rpc_timeout, semaphore_available;
static int rpc_delays, semaphore_takes, semaphore_gives;

static int checked_mute(const audio_codec_if_t *codec, bool muted)
{
    assert(codec == mic.codec);
    ++mute_calls;
    if (mute_result == ESP_CODEC_DEV_OK) codec_muted = muted;
    return mute_result;
}

static int read_mute_register(const audio_codec_if_t *codec, int reg, int *value)
{
    assert(codec == mic.codec && (reg == 0x14 || reg == 0x15));
    *value = codec_muted ? 3 : 0;
    return mute_read_error;
}

static const audio_codec_if_t fake_codec = {
    .mute_mic = checked_mute, .get_reg = read_mute_register,
};

static void log_info(const char *tag, const char *format, ...)
{
    assert(!strcmp(tag, "microphone"));
    assert(strstr(format, "Input muted"));
    ++pause_logs;
}

static void log_error(const char *tag, const char *format, ...)
{
    assert(!strcmp(tag, "microphone"));
    assert(strstr(format, "Capture errors"));
    ++error_logs;
}

static void vTaskDelay(unsigned ticks)
{
    assert(ticks == 1);
    now_us += 10000;
    if (rpc_mode && !rpc_timeout) {
        ++rpc_delays;
        // A stale acknowledgement must not finish the current API request.
        mic.acknowledged_control = rpc_delays < 3 ? mic.requested_control - 1
                                                : mic.requested_control;
        mic.control_result = mute_result == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    }
}

static int xSemaphoreTake(SemaphoreHandle_t handle, unsigned wait)
{
    assert(handle == mic.control_lock && wait <= 100);
    ++semaphore_takes;
    return semaphore_available ? pdTRUE : 0;
}

static void xSemaphoreGive(SemaphoreHandle_t handle)
{
    assert(handle == mic.control_lock);
    ++semaphore_gives;
}

static esp_err_t i2s_channel_read(i2s_chan_handle_t rx, void *data,
                                size_t capacity, size_t *bytes, unsigned timeout)
{
    assert(rx == (void *)3 && timeout == READ_TIMEOUT_MS);
    if (next_read == read_count) longjmp(finished, 1);
    if (next_read == pause_at) {
        mic.requested_pause = true;
        ++mic.requested_control;
    }
    if (next_read == pause_at + 1) {
        assert(mic.acknowledged_control == mic.requested_control);
        bool success = mute_result == ESP_CODEC_DEV_OK && mute_read_error == ESP_CODEC_DEV_OK;
        assert(mic.control_result == (success ? ESP_OK : ESP_FAIL));
        if (success) {
            assert(mic.paused && codec_muted && !queue_count);
            sequence_at_pause = mic.frame.sequence;
        } else assert(!mic.paused && queue_count);
    }
    if (mute_result == ESP_CODEC_DEV_OK && mute_read_error == ESP_CODEC_DEV_OK &&
        next_read > pause_at && next_read <= resume_at) {
        assert(mic.paused && codec_muted && !queue_count);
        assert(mic.frame.sequence == sequence_at_pause);
    }
    if (next_read == resume_at) {
        mic.requested_pause = false;
        ++mic.requested_control;
    }
    if (next_read == resume_at + 1 && resume_at >= 0) {
        assert(mic.acknowledged_control == mic.requested_control);
        assert(mic.control_result == ESP_OK && !mic.paused && !codec_muted && !queue_count);
    }
    assert(reads[next_read].bytes <= capacity);
    if (reads[next_read].lost_frames) {
        source_byte += reads[next_read].lost_frames * 2 * sizeof(int16_t);
        receive_overflow(rx, NULL, NULL);
    }
    *bytes = reads[next_read].bytes;
    assert(source_byte + *bytes <= sizeof(input));
    memcpy(data, (const unsigned char *)input + source_byte, *bytes);
    source_byte += *bytes;
    now_us += 100000;
    return reads[next_read++].error;
}

static int xQueueSend(QueueHandle_t handle, const void *item, unsigned wait)
{
    assert(handle == mic.frames && wait == 0);
    if (queue_count == FRAME_QUEUE_LENGTH) return 0;
    queue[(queue_head + queue_count) % FRAME_QUEUE_LENGTH] =
        *(const digits_microphone_frame_t *)item;
    if (++queue_count > maximum_queue_count) maximum_queue_count = queue_count;
    return pdTRUE;
}

static int xQueueReceive(QueueHandle_t handle, void *item, unsigned wait)
{
    if (handle == mic.reports) {
        assert(wait == portMAX_DELAY);
        if (next_report == reports_written) longjmp(finished, 1);
        *(microphone_report_t *)item = report_history[next_report++];
        return pdTRUE;
    }
    assert(handle == mic.frames && wait == 0);
    if (!queue_count) return 0;
    *(digits_microphone_frame_t *)item = queue[queue_head];
    queue_head = (queue_head + 1) % FRAME_QUEUE_LENGTH;
    --queue_count;
    return pdTRUE;
}

static int xQueueOverwrite(QueueHandle_t handle, const void *item)
{
    assert(handle == mic.reports);
    last_report = *(const microphone_report_t *)item;
    assert(reports_written < sizeof(report_history) / sizeof(*report_history));
    report_history[reports_written] = last_report;
    ++reports_written;
    return pdTRUE;
}

static int xQueueReset(QueueHandle_t handle)
{
    assert(handle == mic.frames);
    queue_head = queue_count = 0;
    return pdTRUE;
}

static void add_read(size_t bytes, esp_err_t error, unsigned lost_frames)
{
    assert(read_count < 96);
    reads[read_count].bytes = bytes;
    reads[read_count].error = error;
    reads[read_count++].lost_frames = lost_frames;
}

static void reset(bool diagnostic)
{
    memset(&mic, 0, sizeof(mic));
    mic.frames = (void *)1;
    mic.reports = (void *)2;
    mic.codec = &fake_codec;
    mic.control_lock = (void *)4;
    digits_microphone_set_diagnostic(diagnostic);
    now_us = 0;
    read_count = next_read = 0;
    source_byte = queue_head = queue_count = maximum_queue_count = 0;
    reports_written = 0;
    next_report = level_reports = pause_logs = error_logs = 0;
    pause_at = resume_at = -100;
    mute_calls = mute_result = mute_read_error = 0;
    sequence_at_pause = 0;
    codec_muted = false;
    rpc_mode = rpc_timeout = false;
    rpc_delays = semaphore_takes = semaphore_gives = 0;
    semaphore_available = true;
    memset(report_history, 0, sizeof(report_history));
    memset(&last_report, 0, sizeof(last_report));
    // Slot signs differ; reordering a sample or swapping slots is observable.
    for (size_t i = 0; i < sizeof(input) / (2 * sizeof(*input)); ++i) {
        input[2 * i] = (int16_t)(i + 1);
        input[2 * i + 1] = (int16_t)-(int)(i + 1);
    }
    for (unsigned i = 0; i < WARMUP_FRAMES; ++i) add_read(RAW_BYTES, ESP_OK, 0);
}

static void run(void)
{
    if (!setjmp(finished)) capture_worker(NULL);
    assert(next_read == read_count);
}

static void assert_frame(const digits_microphone_frame_t *frame, unsigned sequence,
                         size_t first_sample, bool discontinuous)
{
    assert(frame->sequence == sequence);
    assert(frame->flags == (discontinuous ? DIGITS_MIC_FRAME_DISCONTINUITY : 0));
    assert(frame->captured_at_us > 0);
    for (size_t i = 0; i < DIGITS_MIC_FRAME_SAMPLES; ++i)
        assert(frame->pcm[i] == (int16_t)(first_sample + i));
}

int main(void)
{
    digits_microphone_frame_t frame;
    const size_t first_after_warmup = WARMUP_FRAMES * DIGITS_MIC_FRAME_SAMPLES + 1;
    // A timeout may return even a single byte. Keep it until a whole stereo
    // block is assembled; subsequent clean blocks retain their original phase.
    reset(true);
    add_read(1, ESP_ERR_TIMEOUT, 0);
    add_read(5, ESP_OK, 0);
    add_read(RAW_BYTES - 6, ESP_OK, 0);
    add_read(RAW_BYTES, ESP_OK, 0);
    run();
    assert(queue_count == 2);
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, 1, first_after_warmup, true);
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, 2, first_after_warmup + DIGITS_MIC_FRAME_SAMPLES, false);
    assert(mic.levels.mono.sample_count == 2 * DIGITS_MIC_FRAME_SAMPLES);

    // A DMA gap while assembling a block invalidates that entire block.
    // Only the next intact block is published, with the loss flagged once.
    reset(false);
    add_read(8, ESP_OK, 0);
    add_read(RAW_BYTES - 8, ESP_OK, DIGITS_AUDIO_DMA_FRAMES);
    add_read(RAW_BYTES, ESP_OK, 0);
    add_read(RAW_BYTES, ESP_OK, 0);
    run();
    assert(queue_count == 2);
    size_t after_gap = first_after_warmup + DIGITS_MIC_FRAME_SAMPLES + DIGITS_AUDIO_DMA_FRAMES;
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, 1, after_gap, true);
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, 2, after_gap + DIGITS_MIC_FRAME_SAMPLES, false);

    // An overrun following odd-byte timeouts must discard the trailing byte
    // of the damaged stereo sample before accepting a new clean PCM block.
    reset(false);
    add_read(1, ESP_ERR_TIMEOUT, 0);
    add_read(2, ESP_ERR_TIMEOUT, DIGITS_AUDIO_DMA_FRAMES);
    add_read(1, ESP_OK, 0);
    add_read(RAW_BYTES, ESP_OK, 0);
    add_read(RAW_BYTES, ESP_OK, 0);
    run();
    assert(queue_count == 2);
    after_gap = first_after_warmup + 1 + DIGITS_AUDIO_DMA_FRAMES;
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, 1, after_gap, true);
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, 2, after_gap + DIGITS_MIC_FRAME_SAMPLES, false);

    // No consumer: preserve the four newest independent copies without ever
    // waiting. A late reader observes sequence gaps and flagged replacements.
    reset(false);
    for (unsigned i = 0; i < FRAME_QUEUE_LENGTH + 2; ++i) add_read(RAW_BYTES, ESP_OK, 0);
    run();
    assert(queue_count == FRAME_QUEUE_LENGTH && maximum_queue_count == FRAME_QUEUE_LENGTH);
    for (unsigned sequence = 3; sequence <= FRAME_QUEUE_LENGTH + 2; ++sequence) {
        assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
        assert_frame(&frame, sequence,
                     first_after_warmup + (sequence - 1) * DIGITS_MIC_FRAME_SAMPLES,
                     sequence > FRAME_QUEUE_LENGTH);
    }
    assert(!queue_count && mic.frame.sequence == FRAME_QUEUE_LENGTH + 2);
    assert(reports_written > 0 && !last_report.diagnostic);
    assert(!mic.levels.left.sample_count && !mic.levels.right.sample_count &&
           !mic.levels.mono.sample_count && !last_report.levels.mono.sample_count);

    // Pause ACK gates publication, not native RX reads. Resume clears queued
    // PCM and re-warms the ADC before publishing fresh, discontinuous frames.
    reset(true);
    pause_at = WARMUP_FRAMES + 4;
    resume_at = pause_at + 21;
    for (int i = WARMUP_FRAMES; i < resume_at + 1 + WARMUP_FRAMES + 2; ++i)
        add_read(RAW_BYTES, ESP_OK, 0);
    run();
    assert(mute_calls == 2 && !mic.paused && !codec_muted);
    assert(mic.acknowledged_control == 2 && mic.control_result == ESP_OK);
    assert(queue_count == 2);
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, sequence_at_pause + 1,
                 (resume_at + 1 + WARMUP_FRAMES) * DIGITS_MIC_FRAME_SAMPLES + 1, true);
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, sequence_at_pause + 2,
                 (resume_at + 2 + WARMUP_FRAMES) * DIGITS_MIC_FRAME_SAMPLES + 1, false);
    unsigned paused_reports = 0;
    for (unsigned i = 0; i < reports_written; ++i) {
        if (report_history[i].paused) {
            ++paused_reports;
            assert(!report_history[i].levels.mono.sample_count);
        }
    }
    assert(paused_reports >= 2);
    if (!setjmp(finished)) report_worker(NULL);
    assert(next_report == reports_written && pause_logs == 1 && !error_logs);
    assert(level_reports == reports_written - paused_reports);

    // Control can interrupt a partial stereo word. Discard its remaining
    // bytes while paused; resume must preserve left/right byte alignment.
    reset(false);
    pause_at = WARMUP_FRAMES;
    resume_at = pause_at + 3;
    add_read(1, ESP_ERR_TIMEOUT, 0);
    add_read(3, ESP_OK, 0);
    for (int i = pause_at + 2; i < resume_at + 1 + WARMUP_FRAMES + 2; ++i)
        add_read(RAW_BYTES, ESP_OK, 0);
    run();
    assert(mute_calls == 2 && !mic.paused && queue_count == 2);
    size_t after_partial_control =
        (2 * WARMUP_FRAMES + 2) * DIGITS_MIC_FRAME_SAMPLES + 2;
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, 1, after_partial_control, true);
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, 2, after_partial_control + DIGITS_MIC_FRAME_SAMPLES, false);

    // Failed hardware mute is acknowledged as failure and leaves capture
    // active; callers therefore cannot mistake it for permission to play.
    reset(false);
    pause_at = WARMUP_FRAMES;
    mute_result = -1;
    add_read(RAW_BYTES, ESP_OK, 0);
    add_read(RAW_BYTES, ESP_OK, 0);
    add_read(RAW_BYTES, ESP_OK, 0);
    run();
    assert(mute_calls == 1 && mic.control_result == ESP_FAIL && !mic.paused);
    assert(queue_count == 3);
    for (unsigned sequence = 1; sequence <= 3; ++sequence) {
        assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
        assert_frame(&frame, sequence,
                     first_after_warmup + (sequence - 1) * DIGITS_MIC_FRAME_SAMPLES,
                     sequence == 2);
    }

    // A setter success with failed readback must also ACK failure. Publication
    // remains active but discontinuity exposes the uncertain hardware state.
    reset(false);
    pause_at = WARMUP_FRAMES;
    mute_read_error = -1;
    add_read(RAW_BYTES, ESP_OK, 0);
    add_read(RAW_BYTES, ESP_OK, 0);
    run();
    assert(mute_calls == 1 && mic.control_result == ESP_FAIL && !mic.paused);
    assert(codec_muted && queue_count == 2);
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, 1, first_after_warmup, false);
    assert(xQueueReceive(mic.frames, &frame, 0) == pdTRUE);
    assert_frame(&frame, 2, first_after_warmup + DIGITS_MIC_FRAME_SAMPLES, true);

    // Exercise the real API's bounded polling, including stale generation
    // acknowledgements, propagated codec failure and later resume after timeout.
    reset(false);
    rpc_mode = true;
    assert(digits_microphone_pause(true, 0) == ESP_ERR_INVALID_ARG);
    assert(digits_microphone_pause(true, 1001) == ESP_ERR_INVALID_ARG);
    assert(!semaphore_takes && !mic.requested_control);
    assert(digits_microphone_pause(true, 100) == ESP_OK);
    assert(rpc_delays == 3 && mic.requested_control == 1 && mic.requested_pause);
    assert(semaphore_takes == 1 && semaphore_gives == 1);
    rpc_delays = 0;
    mute_result = -1;
    assert(digits_microphone_pause(false, 100) == ESP_FAIL);
    assert(rpc_delays == 3 && mic.requested_control == 2 && !mic.requested_pause);
    rpc_timeout = true;
    assert(digits_microphone_pause(true, 30) == ESP_ERR_TIMEOUT);
    assert(mic.requested_control == 3 && mic.requested_pause);
    rpc_timeout = false;
    rpc_delays = mute_result = 0;
    assert(digits_microphone_pause(false, 100) == ESP_OK);
    assert(rpc_delays == 3 && mic.requested_control == 4 && !mic.requested_pause);
    semaphore_available = false;
    assert(digits_microphone_pause(true, 100) == ESP_ERR_TIMEOUT);
    assert(mic.requested_control == 4 && semaphore_gives == 4);
    mic.control_lock = NULL;
    assert(digits_microphone_pause(true, 100) == ESP_ERR_INVALID_STATE);
    puts("PASS: partial timeout/slot continuity, DMA-gap resync, bounded copied "
         "queue, paused RX drain/publication, fresh warmed resume, mute errors, "
         "suppressed paused reports and bounded generation-matched pause API");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="digits-microphone-capture-") as directory:
    directory = Path(directory)
    (directory / "esp_err.h").write_text(
        "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n"
        "#define ESP_ERR_TIMEOUT 0x107\n#define ESP_ERR_INVALID_SIZE 0x104\n"
        "#define ESP_FAIL -1\n#define ESP_ERR_INVALID_ARG 0x102\n"
        "#define ESP_ERR_INVALID_STATE 0x103\n"
    )
    test = directory / "microphone_capture_test.c"
    report_stub = r'''
static unsigned level_reports;
static void report_levels(const microphone_report_t *report, unsigned silent_windows[3])
{
    (void)silent_windows;
    assert(!report->paused);
    ++level_reports;
}
'''
    test.write_text(prefix + defines + state + report_stub + report_worker + worker + pause_api + suffix)
    binary = test.with_suffix("")
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-pedantic", "-I", str(directory), "-I", str(repo / "esp32/main"),
         str(test), str(repo / "esp32/main/microphone_pcm.c"), "-lm", "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
