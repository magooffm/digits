"""Run the real incoming-ring worker with finite FreeRTOS/audio ownership mocks.

Checks command races, cadence, mute latching, error cleanup and bounded writes
at the project's 1000 Hz tick rate and a 100 Hz regression configuration.
This is a deterministic host test, not acoustic or ESP32 scheduling evidence.
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
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
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
#define ring_tone_render host_render
#include "incoming_ring.c"
#undef ring_tone_render
void ring_tone_render(int16_t *,size_t,size_t);

enum action { STOP, START, SILENT_ON, SILENT_OFF, OTHER_ACQUIRE, OTHER_RELEASE, LIFT, REPLACE };
enum stage { NONE, ACQUIRE, START_OUTPUT, RENDER, WRITE, SILENCE };
struct event { int64_t at; enum action action; unsigned id; };
static struct event events[32];
static unsigned event_count,event_next,notifications,waits,writes,starts,releases,acquires;
static unsigned fail_write,partial_write,fail_start,task_creates;
static unsigned owner; // 0 free, 1 another audio user, 2 incoming-ring worker
static bool amplifier,rx_available,fail_task,acquiring,fail_silence;
static int64_t now_us,start_times[8],release_times[8],acquire_times[512];
static enum stage injection_stage;
static enum action injection_action;
static unsigned injection_id;
static bool start_during_cleanup;
static void (*task_entry)(void *);
static jmp_buf done;

static void control(enum action action,unsigned id)
{
    int64_t before = now_us;
    switch (action) {
    case STOP:
        digits_incoming_ring_stop();
        if (owner == 2 || acquiring) assert(!digits_incoming_ring_is_stopped());
        break;
    case START: digits_incoming_ring_start(id); break;
    case SILENT_ON: digits_incoming_ring_set_silent(true); break;
    case SILENT_OFF: digits_incoming_ring_set_silent(false); break;
    case OTHER_ACQUIRE: assert(owner == 0); owner = 1; break;
    case OTHER_RELEASE: assert(owner == 1); owner = 0; break;
    case LIFT: digits_incoming_ring_set_suppressed(true); break;
    case REPLACE: digits_incoming_ring_set_suppressed(false); break;
    }
    assert(now_us == before); // Control paths perform no audio I/O or wait.
}

static void inject(enum stage stage)
{
    if (injection_stage != stage) return;
    injection_stage = NONE;
    control(injection_action,injection_id);
    // Clearing silent immediately must not defeat the per-call mute latch.
    if (injection_action == SILENT_ON) control(SILENT_OFF,0);
}

static void advance(int64_t target)
{
    assert(target >= now_us);
    while (event_next < event_count && events[event_next].at <= target) {
        struct event event = events[event_next++];
        assert(event.at >= now_us);
        now_us = event.at;
        control(event.action,event.id);
    }
    now_us = target;
}

int64_t esp_timer_get_time(void) { return now_us; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock error"; }
i2s_chan_handle_t digits_audio_rx_channel(void) { return rx_available ? (void *)1 : NULL; }
int xTaskCreate(void (*entry)(void *),const char *name,unsigned stack,void *arg,
                unsigned priority,TaskHandle_t *out)
{
    assert(strcmp(name,"digits_in_ring") == 0 && stack == 8192 && !arg && priority == 4);
    ++task_creates;
    if (fail_task) return 0;
    task_entry = entry;
    *out = (void *)1;
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task == worker); ++notifications; }
unsigned ulTaskNotifyTake(int clear,TickType_t ticks)
{
    assert(clear == pdTRUE && ticks > 0 && ++waits < 2000);
    assert(owner != 2); // Worker sleeps only after relinquishing speaker.
    if (notifications) { notifications = 0; return 1; }
    if (ticks == portMAX_DELAY && event_next == event_count) longjmp(done,1);
    int64_t deadline = ticks == portMAX_DELAY ? INT64_MAX :
        now_us + (int64_t)ticks * 1000000 / configTICK_RATE_HZ;
    if (event_next < event_count && events[event_next].at < deadline)
        deadline = events[event_next].at;
    advance(deadline);
    unsigned result = notifications;
    notifications = 0;
    return result;
}
bool digits_audio_speaker_acquire(uint32_t timeout)
{
    assert(timeout == 0 && owner != 2 && acquires < 512);
    acquire_times[acquires++] = now_us;
    assert(!digits_incoming_ring_is_stopped());
    acquiring = true;
    inject(ACQUIRE);
    acquiring = false;
    if (owner) return false;
    owner = 2;
    return true;
}
void digits_audio_speaker_release(void)
{
    assert(owner == 2 && !amplifier && releases < 8);
    release_times[releases++] = now_us;
    owner = 0;
}
esp_err_t digits_audio_speaker_start(void)
{
    assert(owner == 2 && !digits_incoming_ring_is_stopped() && starts < 8);
    start_times[starts++] = now_us;
    amplifier = true;
    inject(START_OUTPUT);
    return starts == fail_start ? ESP_FAIL : ESP_OK;
}
esp_err_t digits_audio_speaker_write(const int16_t *pcm,size_t bytes,
                                    size_t *written,uint32_t timeout)
{
    assert(owner == 2 && amplifier && !digits_incoming_ring_is_stopped());
    assert(bytes == 480 * 2 * sizeof(int16_t) && timeout == 20 && ++writes < 1000);
    bool nonzero = false;
    for (unsigned i = 0; i < 960; i += 2) {
        assert(pcm[i] == pcm[i + 1] && pcm[i] <= 4096 && pcm[i] >= -4096);
        nonzero |= pcm[i] != 0;
    }
    assert(nonzero); // Both seconds contain real waveform data.
    inject(WRITE);
    advance(now_us + 10000);
    *written = writes == partial_write ? bytes / 2 : bytes;
    return writes == fail_write ? ESP_FAIL : ESP_OK;
}
bool digits_audio_speaker_silence(void)
{
    assert(owner == 2 && !digits_incoming_ring_is_stopped());
    amplifier = false;
    inject(SILENCE);
    if (start_during_cleanup) {
        start_during_cleanup = false;
        control(START,2);
        assert(!digits_incoming_ring_is_stopped());
    }
    advance(now_us + 30000); // Hardware gate precedes bounded DMA flush.
    return !fail_silence;
}
void host_render(int16_t *pcm,size_t first,size_t frames)
{
    assert(first < RING_TONE_FRAMES && frames == 480);
    ring_tone_render(pcm,first,frames);
    inject(RENDER);
}

static void event(int64_t at,enum action action,unsigned id)
{
    assert(event_count < 32 && (!event_count || events[event_count - 1].at <= at));
    events[event_count++] = (struct event){at,action,id};
}
static void reset(void)
{
    state = (ring_state_t){.stopped=true};
    worker = NULL;
    event_count=event_next=notifications=waits=writes=starts=releases=acquires=0;
    fail_write=partial_write=fail_start=task_creates=owner=0;
    amplifier=fail_task=acquiring=start_during_cleanup=fail_silence=false;
    rx_available=true;
    now_us=0;
    injection_stage=NONE;
    assert(digits_incoming_ring_init() == ESP_OK);
    assert(digits_incoming_ring_init() == ESP_ERR_INVALID_STATE && task_creates == 1);
}
static void run(void)
{
    if (setjmp(done) == 0) task_entry(NULL);
    assert(owner != 2 && !amplifier && digits_incoming_ring_is_stopped());
    // Once stopped and acknowledged, waking the worker cannot perform stale I/O.
    unsigned before_writes=writes,before_acquires=acquires;
    control(STOP,0);
    if (setjmp(done) == 0) task_entry(NULL);
    assert(writes == before_writes && acquires == before_acquires);
}
int main(void)
{
    ring_tone_init();
    rx_available=false;
    assert(digits_incoming_ring_init() == ESP_ERR_INVALID_STATE);
    control(START,1); // No task: preserve indication without blocking cleanup.
    assert(digits_incoming_ring_is_stopped());
    control(STOP,0);
    rx_available=true; fail_task=true;
    assert(digits_incoming_ring_init() == ESP_ERR_NO_MEM && !worker);

    reset(); control(START,0); run();
    assert(starts == 0 && acquires == 0);
    reset(); control(START,1); control(STOP,0); run();
    assert(starts == 0 && acquires == 0);

    reset(); control(START,1);
    event(1000000,START,1); // Retransmitted ring must not extend the on phase.
    event(2100000,OTHER_ACQUIRE,0); event(4000000,OTHER_RELEASE,0); event(7000000,STOP,0);
    run();
    assert(starts == 2 && releases == 2 && writes == 300);
    assert(start_times[0] == 0 && start_times[1] == 6000000);
    assert(release_times[0] == 2030000 && release_times[1] == 7030000);

    reset(); control(SILENT_ON,0); control(START,1);
    event(100000,SILENT_OFF,0); event(200000,START,1);
    event(300000,START,2); event(400000,STOP,0); run();
    assert(starts == 1 && start_times[0] == 300000 && writes == 10);

    reset(); control(START,1);
    event(30000,SILENT_ON,0); event(40000,SILENT_OFF,0); event(50000,START,1);
    event(100000,STOP,0); event(120000,START,2); event(150000,STOP,0); run();
    assert(starts == 2 && writes == 6 && start_times[1] == 120000);

    reset(); owner=1; control(START,1);
    event(60000,OTHER_RELEASE,0); event(100000,STOP,0); run();
    assert(starts == 1 && releases == 1 && start_times[0] == 60000);
    // Initial wake may retry once immediately, then contention yields 20 ms.
    assert(acquires <= 5 && acquire_times[acquires - 1] == 60000);

    reset(); owner=1; control(START,1); event(30000,STOP,0); run();
    assert(owner == 1 && starts == 0 && releases == 0 && acquires <= 3);

    for (unsigned which=0;which<3;++which) {
        reset(); control(START,1);
        if (which == 0) fail_start=1;
        if (which == 1) fail_write=3;
        if (which == 2) partial_write=3;
        event(200000,START,1); // Same call must not spin/retry a playback fault.
        event(300000,START,2); event(350000,STOP,0); run();
        assert(starts == 2 && releases == 2 && start_times[1] == 300000);
        assert(writes == (which == 0 ? 5 : 8));
    }
    for (enum stage stage=ACQUIRE;stage<=WRITE;++stage) {
        reset(); control(START,1);
        injection_stage=stage; injection_action=STOP; run();
        assert(writes == (stage == WRITE ? 1 : 0) && releases == 1);
    }
    reset(); control(START,1); injection_stage=RENDER; injection_action=SILENT_ON;
    event(100000,START,1); event(200000,STOP,0); run();
    assert(writes == 0 && starts == 1 && releases == 1);

    reset(); control(START,1); injection_stage=RENDER; injection_action=START; injection_id=2;
    event(100000,STOP,0); run();
    assert(starts == 2 && releases == 2 && writes == 7 && start_times[1] == 30000);

    reset(); control(START,1); injection_stage=WRITE; injection_action=STOP;
    start_during_cleanup=true; event(100000,STOP,0); run();
    assert(starts == 2 && releases == 2 && writes == 7 && start_times[1] == 40000);

    // Handset lifted mid-tone: release at once; replaced while the same call
    // still rings: restart its cadence immediately, not after the pause.
    reset(); control(START,1); event(500000,LIFT,0); event(3500000,REPLACE,0);
    event(3600000,STOP,0); run();
    assert(starts == 2 && releases == 2 && start_times[1] == 3500000);
    assert(release_times[0] == 530000 && release_times[1] == 3630000);

    // Ring arriving while off-hook stays silent, and a later ring after the
    // handset is replaced is audible again.
    reset(); control(LIFT,0); control(START,1); event(200000,STOP,0);
    event(300000,REPLACE,0); event(400000,START,2); event(500000,STOP,0); run();
    assert(starts == 1 && start_times[0] == 400000 && releases == 1);

    // Replacing the handset with no call active never starts the speaker.
    reset(); control(LIFT,0); control(REPLACE,0); run();
    assert(starts == 0 && acquires == 0);

    // Lift during each worker stage still releases without a stale write.
    for (enum stage stage=ACQUIRE;stage<=WRITE;++stage) {
        reset(); control(START,1);
        injection_stage=stage; injection_action=LIFT; event(100000,STOP,0); run();
        assert(writes == (stage == WRITE ? 1 : 0) && releases == 1);
        control(REPLACE,0);
    }

    reset(); control(START,1); fail_silence=true; event(20000,STOP,0); run();
    assert(writes == 2 && releases == 1); // Report cleanup fault without leaking reservation.
    printf("incoming-ring worker (%u Hz): cadence, ownership, mute latches, handset suppression, races and failures passed\n",
           (unsigned)configTICK_RATE_HZ);
}
'''

with tempfile.TemporaryDirectory(prefix="digits-incoming-ring-") as directory:
    root = Path(directory)
    for name, content in headers.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    source = root / "test.c"
    source.write_text(harness)
    for tick_rate in (1000, 100):
        binary = root / f"test-{tick_rate}hz"
        command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-variable", f"-DconfigTICK_RATE_HZ={tick_rate}",
                   "-I", str(root), "-I", str(repo / "esp32/main"),
                   str(source), str(repo / "esp32/main/ring_tone.c"), "-lm", "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
