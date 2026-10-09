from pathlib import Path
import argparse
import re
import tempfile
import subprocess

# Compile the actual worker bodies with a host model of IDF's DMA/tick waits.
# Render cost and capture preemption below are deliberate test inputs, not
# measurements of ESP32 execution time. No physical I2C/audio is simulated.
repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description='Ring Test DMA/tick and producer-budget regression')
parser.add_argument('--source', type=Path, default=repo / 'esp32/main/ring_test.c')
parser.add_argument('--expect-bug', action='store_true', help='Verify a pre-deadline-fix source snapshot')
args = parser.parse_args()
source = args.source.read_text()
bodies = source[source.index('static bool silence_output(void)'):source.index('static void release_audio(void)')]
defines = '\n'.join(re.findall(r'^#define .+$', source, re.M))
board = (repo / 'esp32/main/audio_board.c').read_text()
silence = board[board.index('bool digits_audio_speaker_silence(void)'):]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include "ring_tone.h"
#include "audio_format.h"
typedef int esp_err_t;
typedef unsigned TickType_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 0x107
#define ESP_CODEC_DEV_OK 0
#define portMAX_DELAY 0xffffffffu
#define configTICK_RATE_HZ 100
#define portTICK_PERIOD_MS 10
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms) * configTICK_RATE_HZ / 1000)
#define ESP_LOGI(tag, ...) log_info(__VA_ARGS__)
#define ESP_LOGW(tag, ...) log_warn(__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) log_error(fmt)
static const char *esp_err_to_name(int err) {return "fake";}
static struct {void *device; void *tx; void *requests;} audio;
static struct {void *device; void *tx;} board = {(void *)1, (void *)1};
static bool digits_audio_speaker_silence(void);
static bool digits_audio_speaker_acquire(unsigned timeout) { assert(timeout == 0); return true; }
static void digits_audio_speaker_release(void) {}
static int64_t now_us, requested[3], starts[3], stops[3], playing_at[3];
static int64_t dma_next_us, dma_phase_us, rtos_phase_us, last_error_us;
static int request_count, next_request, starts_count, stops_count, playing_count;
static int writes, tone_writes, silence_writes, fail_at_tone_write;
static int errors, zero_tick_writes, logging_delay_us;
static unsigned last_timeout_ms, last_timeout_ticks;
static bool speaker_enabled, dma_active, dma_queue_ready, stream_started;
static bool dma_filled[2];
static int dma_completed_index, dma_queue_index, empty_blocks;
static unsigned render_cost_us, capture_pause_us, render_calls;
static unsigned reported_frames, reported_render_us, reported_feed_gap_us, reported_block_us;
static unsigned timing_reports, budget_warnings;
static jmp_buf done;

static void set_time(int64_t target)
{
    assert(target >= now_us);
    // Two native TX descriptors form a continuous ring. EOF clears the buffer
    // just consumed, then queues its pointer. If that one-slot queue was full,
    // IDF discards the old pointer; a later write still succeeds.
    while (dma_active && dma_next_us <= target) {
        now_us = dma_next_us;
        if (dma_filled[dma_completed_index]) {
            stream_started = true;
        } else if (speaker_enabled && stream_started &&
                   now_us < starts[starts_count - 1] + 980000) {
            // Exclude startup silence and the deliberate final 20 ms drain.
            ++empty_blocks;
        }
        dma_filled[dma_completed_index] = false; // DMA auto-clear.
        dma_queue_ready = true;
        dma_queue_index = dma_completed_index;
        dma_completed_index ^= 1;
        dma_next_us += 10000;
    }
    now_us = target;
}

static void log_info(const char *format, ...)
{
    if (strstr(format, "Playing")) {
        playing_at[playing_count++] = now_us;
        set_time(now_us + logging_delay_us);
        // Continuous idle DMA leaves one completed buffer queued. Its next
        // completion has a fixed phase independent of the RTOS tick.
        dma_queue_ready = true;
        dma_queue_index = 0;
        dma_completed_index = 1;
        dma_filled[0] = dma_filled[1] = false;
        stream_started = false;
        dma_next_us = ((now_us - dma_phase_us) / 10000 + 1) * 10000 + dma_phase_us;
        dma_active = true;
    } else if (strstr(format, "Stream timing:")) {
        // Timing diagnostics must run after the hardware amplifier is gated.
        assert(!speaker_enabled);
        va_list args;
        va_start(args, format);
        reported_frames = va_arg(args, unsigned);
        reported_render_us = va_arg(args, unsigned);
        reported_feed_gap_us = va_arg(args, unsigned);
        reported_block_us = va_arg(args, unsigned);
        va_end(args);
        ++timing_reports;
    }
}

static void log_warn(const char *format, ...)
{
    if (strstr(format, "PCM producer exceeded")) {
        assert(!speaker_enabled);
        ++budget_warnings;
    }
}

static void log_error(const char *format)
{
    if (strstr(format, "Playback write failed")) {
        errors++;
        last_error_us = now_us;
    }
}
static int64_t esp_timer_get_time(void) {return now_us;}
static void vTaskDelay(unsigned ticks)
{
    // Native FreeRTOS relative delay expires on a tick boundary.
    assert(ticks > 0);
    set_time(((now_us - rtos_phase_us) / 10000 + ticks) * 10000 + rtos_phase_us);
}
static int xQueueReceive(void *q, int64_t *at, unsigned wait)
{
    if (next_request >= request_count) longjmp(done, 1);
    *at = requested[next_request++];
    if (now_us < *at) set_time(*at);
    return 1;
}
static int amplifier_set(bool enabled)
{
    if (enabled) {
        starts[starts_count++] = now_us;
        stream_started = false;
    } else stops[stops_count++] = now_us;
    speaker_enabled = enabled;
    set_time(now_us + 1000); // Expander read-modify-write consumes budget.
    return ESP_OK;
}
static int esp_codec_dev_set_out_mute(void *dev, bool muted) {return ESP_CODEC_DEV_OK;}
static int i2s_channel_write(void *tx, const void *data, size_t size,
                             size_t *written, unsigned timeout_ms)
{
    assert(timeout_ms > 0 && timeout_ms <= 50);
    assert(size == DIGITS_AUDIO_DMA_FRAMES * 2 * sizeof(int16_t)); // 10 ms stereo.
    unsigned ticks = pdMS_TO_TICKS(timeout_ms);
    if (!ticks) zero_tick_writes++;
    *written = 0;
    writes++;
    const int16_t *pcm = data;
    bool tone = false;
    for (size_t i = 0; i < size / sizeof(int16_t); i++) if (pcm[i]) tone = true;
    // xQueueReceive(timeout_ticks) cannot wait beyond its expiration tick.
    int64_t wait_until = ticks ? ((now_us - rtos_phase_us) / 10000 + ticks) * 10000 + rtos_phase_us : now_us;
    if (tone && tone_writes + 1 == fail_at_tone_write) {
        fail_at_tone_write = 0;
        set_time(wait_until);
        return ESP_ERR_TIMEOUT;
    }
    if (!dma_queue_ready) {
        if (dma_next_us > wait_until) {
            set_time(wait_until);
            last_timeout_ms = timeout_ms;
            last_timeout_ticks = ticks;
            return ESP_ERR_TIMEOUT;
        }
        set_time(dma_next_us);
    }
    assert(dma_queue_ready);
    dma_queue_ready = false;
    dma_filled[dma_queue_index] = tone;
    if (tone) tone_writes++; else silence_writes++;
    *written = size;
    return ESP_OK;
}
static int digits_audio_speaker_start(void) {return amplifier_set(true);}
static int digits_audio_speaker_write(const int16_t *pcm, size_t size,
                                      size_t *written, unsigned timeout_ms)
{
    return i2s_channel_write(board.tx, pcm, size, written, timeout_ms);
}
static void modeled_ring_tone_render(int16_t *pcm, size_t first, size_t frames)
{
    // Always exercise the real renderer. Only its elapsed cost is modeled.
    ring_tone_render(pcm, first, frames);
    ++render_calls;
    set_time(now_us + render_cost_us);
    // Finite higher-priority ADC-style work every other nominal 10 ms block.
    if (capture_pause_us && render_calls % 2 == 0)
        set_time(now_us + capture_pause_us);
}
#define ring_tone_render modeled_ring_tone_render
'''
suffix = r'''
static void reset(int log_delay, int dma_phase)
{
    now_us = 10000; dma_phase_us = dma_phase; rtos_phase_us = 1000; logging_delay_us = log_delay;
    request_count = 1; next_request = starts_count = stops_count = playing_count = 0;
    writes = tone_writes = silence_writes = fail_at_tone_write = 0;
    errors = zero_tick_writes = 0; last_error_us = -1;
    last_timeout_ms = last_timeout_ticks = 0;
    dma_next_us = 0; dma_queue_ready = false; dma_active = false;
    dma_filled[0] = dma_filled[1] = false;
    speaker_enabled = stream_started = false;
    dma_completed_index = dma_queue_index = empty_blocks = 0;
    render_cost_us = capture_pause_us = render_calls = 0;
    reported_frames = reported_render_us = reported_feed_gap_us = reported_block_us = 0;
    timing_reports = budget_warnings = 0;
    memset(requested, 0, sizeof(requested));
}
static void run(void) {if (!setjmp(done)) ring_worker(NULL);}
static void assert_stopped_on_time(void)
{
    assert(starts_count == 1 && stops_count == 1 && silence_writes == 3);
    assert(stops[0] >= 1010000 && stops[0] <= 1020000);
    assert(!speaker_enabled);
}
int main(int argc, char **argv)
{
    // Firmware initializes the cached period before creating the worker.
    ring_tone_init();
    bool expect_bug = argc > 1;
    reset(10000, 2000); run();
    printf("Representative trace: playing=%lld us, errors=%d, error_offset=%lld us, "
           "zero_tick_writes=%d, gate_off=%lld us, tone_blocks=%d, failed_timeout=%u ms/%u ticks\n",
           (long long)playing_at[0], errors,
           (long long)(last_error_us < 0 ? -1 : last_error_us-playing_at[0]),
           zero_tick_writes, (long long)stops[0], tone_writes, last_timeout_ms, last_timeout_ticks);
    assert(starts_count == 1 && stops_count == 1 && silence_writes == 3);
    if (expect_bug) {
        assert(errors == 1);
        assert(last_error_us - playing_at[0] == 990000);
    } else {
        assert(errors == 0 && zero_tick_writes == 0);
        assert_stopped_on_time();
        assert(empty_blocks == 0);
    }
    int failing_cases = 0, cases = 0;
    int setup_delays[] = {0, 1000, 5000, 9000, 10000, 13000, 17000};
    for (size_t i = 0; i < sizeof(setup_delays)/sizeof(setup_delays[0]); ++i) {
        for (int phase = 0; phase < 10000; phase += 1000) {
            reset(setup_delays[i], phase); run(); cases++;
            assert(starts_count == 1 && stops_count == 1 && silence_writes == 3);
            if (errors) failing_cases++;
            if (!expect_bug) {
                assert(errors == 0 && zero_tick_writes == 0);
                assert_stopped_on_time();
                assert(empty_blocks == 0);
            }
        }
    }
    printf("DMA/tick phase sweep: %d/%d scenarios logged playback errors\n", failing_cases, cases);
    if (expect_bug) {
        assert(failing_cases > 0);
        puts("PASS: reproduces original final-block timeout with native 100 Hz timeout flooring");
        return 0;
    }

    // A slow producer can leave auto-cleared descriptors empty while every
    // write succeeds. 25 ms represents an intentionally over-budget workload;
    // it is not a measured cost of the previous ESP32 sine renderer.
    reset(0, 2000); render_cost_us = 25000; run();
    assert(errors == 0 && zero_tick_writes == 0);
    assert_stopped_on_time();
    assert(empty_blocks > 0 && tone_writes < 50);
    assert(timing_reports == 1 && reported_render_us == 25000 && reported_feed_gap_us == 25000);
    assert(reported_frames == (unsigned)tone_writes * DIGITS_AUDIO_DMA_FRAMES);
    assert(reported_block_us == 10000 && budget_warnings == 1);
    printf("Modeled 25 ms producer: %d empty DMA blocks despite %d successful writes; "
           "max_render=%u us max_feed_gap=%u us\n",
           empty_blocks, tone_writes, reported_render_us, reported_feed_gap_us);

    // Cached rendering has bounded integer work. Exercise a conservative
    // modeled 100 us render plus finite capture preemptions at several phases.
    int pauses[] = {0, 500, 1500, 2500};
    int bounded_cases = 0;
    for (size_t i = 0; i < sizeof(pauses)/sizeof(pauses[0]); ++i) {
        for (int phase = 0; phase < 10000; phase += 1000) {
            reset(10000, phase);
            render_cost_us = 100; capture_pause_us = pauses[i]; run();
            assert(errors == 0 && zero_tick_writes == 0 && empty_blocks == 0);
            assert_stopped_on_time();
            assert(tone_writes >= 95 && timing_reports == 1);
            assert(reported_frames == (unsigned)tone_writes * DIGITS_AUDIO_DMA_FRAMES);
            assert(reported_render_us == (unsigned)(100 + pauses[i]));
            assert(reported_feed_gap_us <= (unsigned)(100 + pauses[i]));
            assert(reported_block_us == 10000 && budget_warnings == 0);
            ++bounded_cases;
        }
    }
    printf("Bounded modeled 100 us renderer + capture phase sweep: %d/%d gap-free scenarios\n",
           bounded_cases, bounded_cases);

    reset(10000, 2000); render_cost_us = 100; fail_at_tone_write = 20; run();
    assert(errors == 1 && tone_writes == 19 && silence_writes == 3);
    assert(last_error_us < 900000 && stops[0] < 1000000 && !speaker_enabled);
    assert(timing_reports == 1 && reported_frames == 19U * DIGITS_AUDIO_DMA_FRAMES);
    printf("Injected mid-playback DMA stall: logged=%d, gate_off=%lld us\n",
           errors, (long long)stops[0]);

    reset(10000, 1000); render_cost_us = 100; capture_pause_us = 1500; request_count = 3;
    requested[1] = 500000; requested[2] = 1100000; run();
    assert(starts_count == 2 && stops_count == 2 && silence_writes == 6);
    assert(errors == 0 && zero_tick_writes == 0 && empty_blocks == 0);
    assert(starts[1] >= 1100000 && stops[1] >= 2100000 && stops[1] <= 2110000);
    assert(timing_reports == 2 && !speaker_enabled);
    puts("PASS: bounded stop, silent underfeed detected by model, post-silence timing, "
         "genuine stall retained, overlapping request coalesced, later request plays");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='digits-ring-deadline-') as directory:
    test = Path(directory) / 'ring_deadline_test.c'
    test.write_text(prefix + '\n' + defines + '\n' + silence + '\n' + bodies + suffix)
    binary = test.with_suffix('')
    subprocess.run(['cc', '-std=c11', '-I', str(repo / 'esp32/main'),
                    str(test), str(repo / 'esp32/main/ring_tone.c'),
                    '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)] + (['expect-bug'] if args.expect_bug else []), check=True)
