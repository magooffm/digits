from pathlib import Path
import argparse
import re
import tempfile
import subprocess

# Compile the actual worker bodies with a host model of IDF's DMA/tick waits.
# This regression covers deadline timing; it does not simulate physical I2C.
repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description='Ring Test DMA/tick deadline regression')
parser.add_argument('--source', type=Path, default=repo / 'esp32/main/ring_test.c')
parser.add_argument('--expect-bug', action='store_true', help='Verify a pre-fix source snapshot')
args = parser.parse_args()
source_path = args.source
expect_bug = args.expect_bug
source = source_path.read_text()
bodies = source[source.index('static bool silence_output(void)'):source.index('static void release_audio(void)')]
defines = '\n'.join(re.findall(r'^#define .+$', source, re.M))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include "ring_tone.h"
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
#define ESP_LOGI(tag, fmt, ...) log_info(fmt)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) log_error(fmt)
static const char *esp_err_to_name(int err) {return "fake";}
static struct {void *device; void *tx; void *requests;} audio;
static int64_t now_us, requested[3], starts[3], stops[3], playing_at[3];
static int64_t dma_next_us, dma_phase_us, rtos_phase_us, last_error_us;
static int request_count, next_request, starts_count, stops_count, playing_count;
static int writes, tone_writes, silence_writes, fail_at_tone_write;
static int errors, zero_tick_writes, logging_delay_us, dma_queue_ready;
static unsigned last_timeout_ms, last_timeout_ticks;
static jmp_buf done;
static void log_info(const char *format)
{
    if (strstr(format, "Playing")) {
        playing_at[playing_count++] = now_us;
        now_us += logging_delay_us;
        /* Continuous idle DMA leaves one completed buffer queued. Its next
         * completion has a fixed phase independent of the RTOS tick. */
        dma_queue_ready = 1;
        dma_next_us = ((now_us - dma_phase_us) / 10000 + 1) * 10000 + dma_phase_us;
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
    /* Native FreeRTOS relative delay expires on a tick boundary. */
    assert(ticks > 0);
    now_us = ((now_us - rtos_phase_us) / 10000 + ticks) * 10000 + rtos_phase_us;
}
static int xQueueReceive(void *q, int64_t *at, unsigned wait)
{
    if (next_request >= request_count) longjmp(done, 1);
    *at = requested[next_request++];
    if (now_us < *at) now_us = *at;
    return 1;
}
static int amplifier_set(bool enabled)
{
    if (enabled) starts[starts_count++] = now_us;
    else stops[stops_count++] = now_us;
    now_us += 1000; /* Expander read-modify-write and DAC setup consume budget. */
    return ESP_OK;
}
static int esp_codec_dev_set_out_mute(void *dev, bool muted) {return ESP_CODEC_DEV_OK;}
static int i2s_channel_write(void *tx, const void *data, size_t size,
                             size_t *written, unsigned timeout_ms)
{
    assert(timeout_ms > 0 && timeout_ms <= 50);
    assert(size == 640); /* Exactly one 10 ms stereo DMA buffer. */
    unsigned ticks = pdMS_TO_TICKS(timeout_ms);
    if (!ticks) zero_tick_writes++;
    *written = 0;
    writes++;
    const int16_t *pcm = data;
    bool tone = false;
    for (size_t i = 0; i < size / sizeof(int16_t); i++) if (pcm[i]) tone = true;
    /* xQueueReceive(timeout_ticks) cannot wait beyond its expiration tick. */
    int64_t wait_until = ticks ? ((now_us - rtos_phase_us) / 10000 + ticks) * 10000 + rtos_phase_us : now_us;
    if (tone && tone_writes + 1 == fail_at_tone_write) {
        fail_at_tone_write = 0;
        now_us = wait_until;
        return ESP_ERR_TIMEOUT;
    }
    if (dma_next_us <= now_us) {
        dma_queue_ready = 1;
        dma_next_us = ((now_us - dma_phase_us) / 10000 + 1) * 10000 + dma_phase_us;
    }
    if (!dma_queue_ready) {
        if (dma_next_us > wait_until) {
            now_us = wait_until;
            last_timeout_ms = timeout_ms;
            last_timeout_ticks = ticks;
            return ESP_ERR_TIMEOUT;
        }
        now_us = dma_next_us;
        dma_next_us += 10000;
    }
    dma_queue_ready = 0;
    if (tone) tone_writes++; else silence_writes++;
    *written = size;
    return ESP_OK;
}
'''
suffix = r'''
static void reset(int log_delay, int dma_phase)
{
    now_us = 10000; dma_phase_us = dma_phase; rtos_phase_us = 1000; logging_delay_us = log_delay;
    request_count = 1; next_request = starts_count = stops_count = playing_count = 0;
    writes = tone_writes = silence_writes = fail_at_tone_write = 0;
    errors = zero_tick_writes = 0; last_error_us = -1;
    last_timeout_ms = last_timeout_ticks = 0;
    dma_next_us = 0; dma_queue_ready = 0;
    memset(requested, 0, sizeof(requested));
}
static void run(void) {if (!setjmp(done)) ring_worker(NULL);}
int main(int argc, char **argv)
{
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
        assert(stops[0] >= 1010000 && stops[0] <= 1020000);
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
                assert(stops[0] >= 1010000 && stops[0] <= 1020000);
            }
        }
    }
    printf("DMA/tick phase sweep: %d/%d scenarios logged playback errors\n", failing_cases, cases);
    if (expect_bug) {
        assert(failing_cases > 0);
        puts("PASS: reproduces original final-block timeout with native 100 Hz timeout flooring");
        return 0;
    }
    reset(10000, 2000); fail_at_tone_write = 20; run();
    assert(errors == 1 && tone_writes == 19 && silence_writes == 3);
    assert(last_error_us < 900000 && stops[0] < 1000000);
    printf("Injected mid-playback DMA stall: logged=%d, gate_off=%lld us\n", errors, (long long)stops[0]);
    reset(10000, 1000); request_count = 3;
    requested[1] = 500000; requested[2] = 1100000; run();
    assert(starts_count == 2 && stops_count == 2 && silence_writes == 6);
    assert(errors == 0 && zero_tick_writes == 0);
    assert(starts[1] >= 1100000 && stops[1] >= 2100000 && stops[1] <= 2110000);
    puts("PASS: no final-block timeout, bounded stop, genuine stall retained, overlapping request coalesced, later request plays");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='digits-ring-deadline-') as directory:
    test = Path(directory) / 'ring_deadline_test.c'
    test.write_text(prefix + '\n' + defines + '\n' + bodies + suffix)
    binary = test.with_suffix('')
    subprocess.run(['cc', '-std=c11', '-I', str(repo / 'esp32/main'),
                    str(test), str(repo / 'esp32/main/ring_tone.c'),
                    '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)] + (['expect-bug'] if expect_bug else []), check=True)
