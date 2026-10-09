from pathlib import Path
import subprocess
import tempfile

# Compile the real component with a deterministic clock/I2C/RTOS shim. Tests
# exercise its debounce filter and publication/error handling, not hardware.
repo = Path(__file__).resolve().parents[2]
program = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "record_button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int critical_depth, error_logs, ready_logs, task_creations;
static int task_create_result = pdPASS;
static esp_err_t init_result = ESP_OK;
static TaskFunction_t created_worker;
static int64_t clock_us;
static jmp_buf worker_done;
static bool worker_running;
static unsigned read_index, read_count;
typedef struct {bool pressed; esp_err_t error;} input_t;
static input_t reads[256];

static void enter_critical(void *mux)
{
    assert(mux && critical_depth == 0);
    ++critical_depth;
}
static void exit_critical(void *mux)
{
    assert(mux && critical_depth == 1);
    --critical_depth;
}
static void fake_log(bool error)
{
    assert(!critical_depth);
    if (error) {
        digits_record_button_state_t state;
        digits_record_button_snapshot(&state);
        assert(!state.valid && state.error != ESP_OK);
        ++error_logs;
    } else ++ready_logs;
}
#define portENTER_CRITICAL(mux) enter_critical(mux)
#define portEXIT_CRITICAL(mux) exit_critical(mux)
#define ESP_LOGI(tag, ...) fake_log(false)
#define ESP_LOGE(tag, ...) fake_log(true)

int64_t esp_timer_get_time(void) {return clock_us;}
esp_err_t digits_audio_record_button_init(void) {return init_result;}
esp_err_t digits_audio_record_button_read(bool *pressed)
{
    assert(worker_running && pressed && !critical_depth);
    if (read_index == read_count) longjmp(worker_done, 1);
    *pressed = reads[read_index].pressed;
    return reads[read_index++].error;
}
BaseType_t xTaskCreate(TaskFunction_t worker, const char *name,
                       uint32_t stack, void *arg, UBaseType_t priority,
                       TaskHandle_t *handle)
{
    assert(worker && !strcmp(name, "digits_record_key"));
    assert(stack >= 2048 && arg == NULL && priority == 3 && handle == NULL);
    ++task_creations;
    created_worker = worker;
    return task_create_result;
}
void vTaskDelay(TickType_t ticks)
{
    assert(ticks == 1 && !critical_depth);
    clock_us += 10000;
}

#include "record_button.c"

static record_button_filter_t fresh_filter(void)
{
    const record_button_filter_t initial = {.state = {.error = ESP_ERR_INVALID_STATE}};
    return initial;
}
static void sample(record_button_filter_t *f, bool pressed, int64_t at_us)
{
    filter_sample(f, pressed, ESP_OK, at_us);
}
static void establish_released(record_button_filter_t *f, int64_t at_us)
{
    sample(f, false, at_us);
    assert(!f->state.valid);
    sample(f, false, at_us + 30000);
    assert(f->state.valid && !f->state.pressed && !f->state.press_sequence);
    assert(f->state.released_at_us == at_us && f->state.error == ESP_OK);
}
static void test_startup_and_boot_hold(void)
{
    record_button_filter_t f = fresh_filter();
    sample(&f, true, 100000);
    sample(&f, true, 129999);
    assert(!f.state.valid && !f.state.press_sequence);
    sample(&f, true, 130000);
    assert(f.state.valid && f.state.pressed && f.state.press_sequence == 1);
    assert(f.state.pressed_at_us == 100000 && f.state.released_at_us == 0);
    sample(&f, true, 900000);
    assert(f.state.press_sequence == 1 && f.state.pressed_at_us == 100000);
    sample(&f, false, 910000);
    sample(&f, false, 939999);
    assert(f.state.pressed);
    sample(&f, false, 940000);
    assert(f.state.valid && !f.state.pressed && f.state.press_sequence == 1);
    assert(f.state.released_at_us == 910000);
}
static void test_bounce_and_raw_edge_timestamps(void)
{
    record_button_filter_t f = fresh_filter();
    establish_released(&f, 100000);
    sample(&f, true, 200000);
    sample(&f, false, 210000);
    sample(&f, true, 220000);
    sample(&f, false, 240000);
    assert(f.state.valid && !f.state.pressed && !f.state.press_sequence);
    sample(&f, true, 250000);
    sample(&f, true, 279999);
    assert(!f.state.pressed);
    sample(&f, true, 280000);
    assert(f.state.pressed && f.state.press_sequence == 1);
    assert(f.state.pressed_at_us == 250000);
    sample(&f, false, 300000);
    sample(&f, true, 310000);
    sample(&f, false, 320000);
    sample(&f, false, 350000);
    assert(!f.state.pressed && f.state.released_at_us == 320000);
    assert(f.state.press_sequence == 1);
    sample(&f, true, 360000);
    sample(&f, true, 390000);
    assert(f.state.press_sequence == 2 && f.state.pressed_at_us == 360000);
}
static void test_read_error_and_recovery(void)
{
    record_button_filter_t f = fresh_filter();
    establish_released(&f, 100000);
    sample(&f, true, 200000);
    sample(&f, true, 230000);
    filter_sample(&f, false, ESP_ERR_TIMEOUT, 240000);
    assert(!f.state.valid && f.state.pressed && f.state.press_sequence == 1);
    assert(f.state.error == ESP_ERR_TIMEOUT && f.state.pressed_at_us == 200000);
    sample(&f, true, 250000);
    sample(&f, true, 279999);
    assert(!f.state.valid);
    sample(&f, true, 280000);
    assert(f.state.valid && f.state.pressed && f.state.press_sequence == 1);
    assert(f.state.pressed_at_us == 200000 && f.state.error == ESP_OK);
    filter_sample(&f, true, ESP_FAIL, 290000);
    sample(&f, false, 300000);
    filter_sample(&f, false, ESP_ERR_TIMEOUT, 320000);
    sample(&f, false, 330000);
    sample(&f, false, 359999);
    assert(!f.state.valid);
    sample(&f, false, 360000);
    assert(f.state.valid && !f.state.pressed && f.state.released_at_us == 330000);
    assert(f.state.press_sequence == 1);
    // A different confirmed state after recovery is an actual new edge.
    filter_sample(&f, false, ESP_FAIL, 370000);
    sample(&f, true, 380000);
    sample(&f, true, 410000);
    assert(f.state.valid && f.state.pressed && f.state.press_sequence == 2);
    assert(f.state.pressed_at_us == 380000);
    // An error during an unconfirmed press must restart the full debounce.
    f = fresh_filter();
    sample(&f, true, 100000);
    filter_sample(&f, true, ESP_ERR_TIMEOUT, 120000);
    sample(&f, true, 125000);
    sample(&f, true, 154999);
    assert(!f.state.valid && !f.state.press_sequence);
    sample(&f, true, 155000);
    assert(f.state.valid && f.state.press_sequence == 1 && f.state.pressed_at_us == 125000);
}
static void test_snapshot_and_start_failures(void)
{
    digits_record_button_state_t state;
    digits_record_button_snapshot(NULL);
    digits_record_button_snapshot(&state);
    assert(!state.valid && state.error == ESP_ERR_INVALID_STATE);
    init_result = ESP_FAIL;
    assert(digits_record_button_start() == ESP_FAIL && !started && !task_creations);
    digits_record_button_snapshot(&state);
    assert(!state.valid && state.error == ESP_FAIL);
    init_result = ESP_OK;
    task_create_result = pdFAIL;
    assert(digits_record_button_start() == ESP_ERR_NO_MEM && !started);
    digits_record_button_snapshot(&state);
    assert(!state.valid && state.error == ESP_ERR_NO_MEM);
    task_create_result = pdPASS;
    assert(digits_record_button_start() == ESP_OK && started && ready_logs == 1);
    assert(digits_record_button_start() == ESP_ERR_INVALID_STATE && task_creations == 2);
    assert(created_worker == button_worker && !critical_depth);
}
static void run_inputs(void)
{
    read_index = 0;
    clock_us = 100000;
    worker_running = true;
    if (!setjmp(worker_done)) created_worker(NULL);
    worker_running = false;
    assert(read_index == read_count && !critical_depth);
}
static void test_real_worker_read_failures_and_rate_limit(void)
{
    // A valid key is immediately invalidated on read failure; after recovery
    // three 10ms intervals are required before it can become valid again.
    read_count = 5;
    for (unsigned i = 0; i < read_count; ++i) reads[i] = (input_t){false, ESP_OK};
    reads[4].error = ESP_ERR_TIMEOUT;
    run_inputs();
    digits_record_button_state_t state;
    digits_record_button_snapshot(&state);
    assert(!state.valid && state.error == ESP_ERR_TIMEOUT && !state.press_sequence);
    assert(state.released_at_us == 100000 && error_logs == 1);
    // Continuous errors log once per second, not once per 10ms I2C poll.
    error_logs = 0;
    read_count = 205;
    for (unsigned i = 0; i < read_count; ++i) reads[i] = (input_t){true, ESP_FAIL};
    run_inputs();
    digits_record_button_snapshot(&state);
    assert(!state.valid && state.error == ESP_FAIL && error_logs == 3);
    // The actual worker establishes a hold at startup, then recovers from an
    // error without creating a second press for the same sustained hold.
    error_logs = 0;
    read_count = 10;
    for (unsigned i = 0; i < read_count; ++i) reads[i] = (input_t){true, ESP_OK};
    reads[4].error = ESP_ERR_TIMEOUT;
    run_inputs();
    digits_record_button_snapshot(&state);
    assert(state.valid && state.pressed && state.press_sequence == 1);
    assert(state.error == ESP_OK && state.pressed_at_us == 100000 && error_logs == 1);
}
int main(void)
{
    test_startup_and_boot_hold();
    test_bounce_and_raw_edge_timestamps();
    test_read_error_and_recovery();
    test_snapshot_and_start_failures();
    test_real_worker_read_failures_and_rate_limit();
    puts("PASS: real K1 filter/worker, stable 30ms edges, bounce rejection, raw "
         "timestamps, held boot, immediate I2C invalidation, recovery counters, "
         "startup failures, safe snapshots and one-second error log limit");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="digits-record-button-") as directory:
    root = Path(directory)
    (root / "freertos").mkdir()
    (root / "driver").mkdir()
    (root / "esp_err.h").write_text('''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
const char *esp_err_to_name(esp_err_t error);
''')
    (root / "esp_log.h").write_text("#pragma once\n")
    (root / "esp_timer.h").write_text("#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
    (root / "driver/i2c_master.h").write_text("#pragma once\ntypedef void *i2c_master_bus_handle_t;\n")
    (root / "driver/i2s_std.h").write_text("#pragma once\ntypedef void *i2s_chan_handle_t;\n")
    (root / "freertos/FreeRTOS.h").write_text('''#pragma once
#include <stdint.h>
typedef unsigned TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms) * 100 / 1000)
#define pdPASS 1
#define pdFAIL 0
''')
    (root / "freertos/task.h").write_text('''#pragma once
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(TaskFunction_t worker, const char *name, uint32_t stack,
                      void *arg, UBaseType_t priority, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
''')
    (root / "test.c").write_text(program)
    executable = root / "record_button_test"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(root), "-I", str(repo / "esp32/main"),
        str(root / "test.c"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
