"""Run the real call-progress tone worker with finite FreeRTOS/speaker mocks.

Checks speaker ownership versus the stopped acknowledgement (call audio relies
on it), cadence continuity for repeated requests, tone switching, contention,
start/write failures and stop requests arriving mid-write, at 1000 Hz and a
100 Hz tick rate. Deterministic host test, not acoustic evidence.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()

headers = {
    "esp_err.h": """
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
const char *esp_err_to_name(esp_err_t error);
""",
    "driver/i2c_master.h": "#pragma once\ntypedef void *i2c_master_bus_handle_t;\n",
    "driver/i2s_std.h": "#pragma once\ntypedef void *i2s_chan_handle_t;\n",
    "esp_log.h": """
#pragma once
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
""",
    "freertos/FreeRTOS.h": """
#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
typedef unsigned TickType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ 1000
#endif
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdPASS 1
""",
    "freertos/task.h": """
#pragma once
typedef void *TaskHandle_t;
int xTaskCreate(void (*entry)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *);
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(int,TickType_t);
""",
}

harness = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "call_tones.c"

enum action { PLAY_NONE, PLAY_DIAL, PLAY_RINGBACK, PLAY_BUSY, OTHER_ACQUIRE, OTHER_RELEASE,
              PREVIEW_ODE, STOP_ALL };
struct event { int64_t at; enum action action; };
static struct event events[16];
static unsigned event_count, event_next, notifications, writes, starts, acquires, releases, task_creates;
static unsigned owner; // 0 free, 1 another audio user, 2 tone worker
static unsigned fail_write_at, fail_start_at;
static bool amplifier, rx_available = true, fail_task;
static int64_t now_us;
static enum action inject_on_write = PLAY_NONE;
static unsigned inject_at_write;
static int16_t last_block[BLOCK_FRAMES * 2];
static void (*task_entry)(void *);
static jmp_buf done;

static void control(enum action action)
{
    switch (action) {
    case PLAY_NONE: digits_call_tones_play(TONE_GEN_NONE); break;
    case PLAY_DIAL: digits_call_tones_play(TONE_GEN_DIAL); break;
    case PLAY_RINGBACK: digits_call_tones_play(TONE_GEN_RINGBACK); break;
    case PLAY_BUSY: digits_call_tones_play(TONE_GEN_BUSY); break;
    case OTHER_ACQUIRE: assert(owner == 0); owner = 1; break;
    case OTHER_RELEASE: assert(owner == 1); owner = 0; break;
    case PREVIEW_ODE: digits_call_tones_preview(2); break;
    case STOP_ALL: digits_call_tones_stop(); break;
    }
}

static void advance(int64_t target)
{
    while (event_next < event_count && events[event_next].at <= target) {
        now_us = events[event_next].at;
        control(events[event_next++].action);
    }
    now_us = target;
}

const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock"; }
i2s_chan_handle_t digits_audio_rx_channel(void) { return rx_available ? (void *)1 : NULL; }
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, TaskHandle_t *out)
{
    assert(!strcmp(name, "digits_tones") && stack == 6144 && !arg && priority == 4);
    ++task_creates;
    if (fail_task) return 0;
    task_entry = entry;
    *out = (void *)1;
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task == worker); ++notifications; }
unsigned ulTaskNotifyTake(int clear, TickType_t ticks)
{
    assert(clear == pdTRUE && ticks > 0);
    assert(owner != 2); // Never sleep while holding the speaker.
    if (notifications) { notifications = 0; return 1; }
    if (ticks == portMAX_DELAY && event_next == event_count) longjmp(done, 1);
    int64_t deadline = ticks == portMAX_DELAY ? INT64_MAX :
        now_us + (int64_t)ticks * 1000000 / configTICK_RATE_HZ;
    if (event_next < event_count && events[event_next].at < deadline) deadline = events[event_next].at;
    advance(deadline);
    unsigned result = notifications;
    notifications = 0;
    return result;
}
bool digits_audio_speaker_acquire(uint32_t timeout)
{
    assert(timeout == 0 && owner != 2);
    // The acknowledgement must already be withdrawn before ownership exists.
    assert(!digits_call_tones_is_stopped());
    ++acquires;
    if (owner) return false;
    owner = 2;
    return true;
}
void digits_audio_speaker_release(void) { assert(owner == 2 && !amplifier); ++releases; owner = 0; }
esp_err_t digits_audio_speaker_start(void)
{
    assert(owner == 2 && !digits_call_tones_is_stopped());
    ++starts;
    amplifier = true;
    return starts == fail_start_at ? ESP_FAIL : ESP_OK;
}
esp_err_t digits_audio_speaker_write(const int16_t *pcm, size_t bytes, size_t *written, uint32_t timeout)
{
    assert(owner == 2 && amplifier && !digits_call_tones_is_stopped());
    assert(bytes == sizeof(last_block) && timeout == 20 && ++writes < 5000);
    memcpy(last_block, pcm, bytes);
    if (inject_at_write && writes == inject_at_write) control(inject_on_write);
    advance(now_us + 10000);
    *written = bytes;
    return writes == fail_write_at ? ESP_FAIL : ESP_OK;
}
bool digits_audio_speaker_silence(void)
{
    assert(owner == 2);
    amplifier = false;
    advance(now_us + 30000);
    return true;
}

static void event(int64_t at, enum action action)
{
    assert(event_count < 16 && (!event_count || events[event_count - 1].at <= at));
    events[event_count++] = (struct event){at, action};
}
static void reset(void)
{
    memset(&state, 0, sizeof(state));
    state.preview = NO_PREVIEW;
    state.stopped = true;
    worker = NULL;
    event_count = event_next = notifications = writes = starts = acquires = releases = task_creates = 0;
    owner = fail_write_at = fail_start_at = inject_at_write = 0;
    amplifier = fail_task = false;
    rx_available = true;
    now_us = 0;
    assert(digits_call_tones_init() == ESP_OK);
    assert(digits_call_tones_init() == ESP_ERR_INVALID_STATE && task_creates == 1);
}
static void run(void)
{
    if (setjmp(done) == 0) task_entry(NULL);
    assert(owner != 2 && !amplifier && digits_call_tones_is_stopped());
}
static bool block_is(tone_gen_tone_t tone, uint64_t position)
{
    int16_t expected[BLOCK_FRAMES * 2];
    tone_gen_render(tone, position, expected, BLOCK_FRAMES);
    return !memcmp(expected, last_block, sizeof(expected));
}

int main(void)
{
    ring_tone_init();
    // No worker: requests must never block call audio.
    rx_available = false;
    assert(digits_call_tones_init() == ESP_ERR_INVALID_STATE);
    digits_call_tones_play(TONE_GEN_DIAL);
    assert(digits_call_tones_is_stopped());
    digits_call_tones_play(TONE_GEN_NONE);

    // Dial tone for 100 ms, then stop: one ownership, release, acknowledged.
    reset();
    control(PLAY_DIAL);
    assert(!digits_call_tones_is_stopped());
    event(100000, PLAY_NONE);
    run();
    assert(acquires == 1 && starts == 1 && releases == 1 && writes == 10);

    // Repeating the same request keeps the cadence position.
    reset();
    control(PLAY_DIAL);
    event(50000, PLAY_DIAL);
    event(65000, PLAY_NONE);
    run();
    assert(writes == 7 && block_is(TONE_GEN_DIAL, 6 * BLOCK_FRAMES));

    // Switching tones keeps the speaker and restarts the new cadence.
    reset();
    control(PLAY_RINGBACK);
    event(40000, PLAY_BUSY);
    event(55000, PLAY_NONE);
    run();
    assert(acquires == 1 && releases == 1 && block_is(TONE_GEN_BUSY, BLOCK_FRAMES));

    // Speaker busy elsewhere: stopped while waiting, plays after release.
    reset();
    owner = 1;
    control(PLAY_DIAL);
    event(30000, PLAY_DIAL); // No-op request while contended.
    event(60000, OTHER_RELEASE);
    event(200000, PLAY_NONE);
    if (setjmp(done) == 0) task_entry(NULL);
    assert(acquires >= 3 && starts == 1 && releases == 1);

    // Contended and cancelled before release: never touches the speaker.
    reset();
    owner = 1;
    control(PLAY_DIAL);
    event(50000, PLAY_NONE);
    run();
    assert(owner == 1 && starts == 0 && releases == 0);
    owner = 0;

    // Write failure: release and acknowledge although the tone is still
    // requested; a different tone may play again.
    reset();
    fail_write_at = 3;
    control(PLAY_DIAL);
    event(200000, PLAY_BUSY);
    event(250000, PLAY_NONE);
    run();
    assert(releases == 2 && starts == 2);

    // Speaker start failure behaves the same way.
    reset();
    fail_start_at = 1;
    control(PLAY_RINGBACK);
    event(100000, PLAY_NONE);
    run();
    assert(starts == 1 && writes == 0 && releases == 1);

    // Stop arriving during a write: that block finishes, then release.
    reset();
    inject_on_write = PLAY_NONE;
    inject_at_write = 4;
    control(PLAY_DIAL);
    run();
    assert(writes == 4 && releases == 1);

    // Stop and restart before the worker runs: plays the new request.
    reset();
    control(PLAY_DIAL);
    control(PLAY_NONE);
    control(PLAY_BUSY);
    event(30000, PLAY_NONE);
    run();
    assert(starts == 1 && block_is(TONE_GEN_BUSY, 2 * BLOCK_FRAMES));

    // Preview plays exactly one melody cycle, then releases by itself.
    reset();
    control(PREVIEW_ODE);
    assert(!digits_call_tones_is_stopped());
    run();
    {
        unsigned blocks = (ringtones_melody_frames(2) + BLOCK_FRAMES - 1) / BLOCK_FRAMES;
        assert(writes == blocks && releases == 1 && starts == 1);
    }

    // Repeating NONE (on-hook phone task) keeps a preview; dial tone replaces it.
    reset();
    control(PREVIEW_ODE);
    event(30000, PLAY_NONE);
    event(60000, PLAY_DIAL);
    event(85000, STOP_ALL);
    run();
    assert(acquires == 1 && releases == 1 && block_is(TONE_GEN_DIAL, 2 * BLOCK_FRAMES));

    // Stop cancels a preview mid-melody; standard-ring preview lasts 2 s.
    reset();
    control(PREVIEW_ODE);
    event(100000, STOP_ALL);
    run();
    assert(writes == 10 && releases == 1);
    reset();
    digits_call_tones_preview(0);
    run();
    assert(writes == 200 && releases == 1);
    digits_call_tones_preview(99); // Invalid: ignored.
    assert(digits_call_tones_is_stopped());

    printf("call-tones worker (%u Hz): ownership, cadence, contention and failures passed\n",
           (unsigned)configTICK_RATE_HZ);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="digits-call-tones-") as directory:
    root = Path(directory)
    for name, content in headers.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    source = root / "test.c"
    source.write_text(harness)
    for tick_rate in (1000, 100):
        binary = root / f"test-{tick_rate}hz"
        command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-DconfigTICK_RATE_HZ={tick_rate}",
                   "-I", str(root), "-I", str(repo / "esp32/main"),
                   str(source), str(repo / "esp32/main/tone_gen.c"),
                   str(repo / "esp32/main/ring_tone.c"), str(repo / "esp32/main/ringtones.c"), "-lm", "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
