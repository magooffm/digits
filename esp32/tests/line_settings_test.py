"""Compile production settings and incoming ringer with real IDF cJSON and mocks.

Checks authoritative snapshot semantics, deterministic records, session gates,
deferred persistence, retries, concurrent newer snapshots and settings-driven
ring ownership across live updates, reconnect and outgoing calls. Does not establish
physical flash power-loss behavior or ESP32 task/audio scheduling performance.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--idf-path", type=Path, default=os.environ.get("IDF_PATH"))
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
idf_paths = [args.idf_path, Path("/private/tmp/digits-esp-idf")]
cjson = next((p / "components/json/cJSON" for p in idf_paths
              if p and (p / "components/json/cJSON/cJSON.c").is_file()), None)
if cjson is None:
    parser.error("Provide --idf-path or export IDF_PATH for the real IDF cJSON source")

fake_header = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
typedef int portMUX_TYPE;
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMUX_INITIALIZER_UNLOCKED 0
void fake_enter(void);
void fake_exit(void);
#define portENTER_CRITICAL(lock) ((void)(lock), fake_enter())
#define portEXIT_CRITICAL(lock) ((void)(lock), fake_exit())
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(int, TickType_t);
void vTaskDelay(TickType_t);
void fake_log(const char *, const char *, ...);
const char *esp_err_to_name(esp_err_t);
#define ESP_LOGW(tag, ...) fake_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) fake_log(tag, __VA_ARGS__)
typedef void *i2c_master_bus_handle_t;
typedef void *i2s_chan_handle_t;
int64_t esp_timer_get_time(void);
'''

harness = r'''
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "line_settings.c"
#include "incoming_ring.h"
#include "audio_board.h"
#include "ring_tone.h"

static bool in_callback, locked, fail_task, exists;
static unsigned nvs_calls, opens, closes, commits, sets, notifications, delays, logs;
static unsigned commit_failures, set_failures;
static esp_err_t open_error, read_error;
static uint8_t stored[32], staged[32];
static size_t stored_size, staged_size;
static const uint8_t pairing_sentinel[] = "paired-number-and-token-preserved";
static uint8_t pairing[sizeof(pairing_sentinel)];
static void (*task_entry)(void *);
static void (*commit_hook)(void);
static void (*delay_hook)(void);
static void (*unlock_hook)(void);
static jmp_buf worker_done;
static jmp_buf ring_done;
static void (*ring_task_entry)(void *);
static void (*speaker_write_hook)(void);
static bool running_ring;
static unsigned ring_notifications, speaker_owner, starts, writes, silences, releases;
static int64_t now_us;

void fake_enter(void) { assert(!locked); locked = true; }
void fake_exit(void)
{
    assert(locked); locked = false;
    if (unlock_hook) {
        void (*hook)(void) = unlock_hook; unlock_hook = NULL; hook();
    }
}
void fake_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; assert(!in_callback && !locked); ++logs; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock_error"; }
static void storage_access(void)
{
    assert(!in_callback && !locked);
    assert(!memcmp(pairing, pairing_sentinel, sizeof(pairing)));
    ++nvs_calls;
}
esp_err_t nvs_open(const char *space, int mode, nvs_handle_t *handle)
{
    storage_access();
    assert(!strcmp(space, "digits_settings")); // Never pairing namespace "digits".
    assert(mode == NVS_READONLY || mode == NVS_READWRITE);
    if (open_error) return open_error;
    if (!exists && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    *handle = 42; ++opens; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    storage_access(); assert(handle == 42 && !strcmp(key, "snapshot"));
    if (read_error) return read_error;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < stored_size) { *size = stored_size; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(value, stored, stored_size); *size = stored_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    storage_access(); assert(handle == 42 && !strcmp(key, "snapshot") && size == 12);
    ++sets;
    if (set_failures) { --set_failures; return ESP_FAIL; }
    memcpy(staged, value, size); staged_size = size; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    storage_access(); assert(handle == 42); ++commits;
    if (commit_hook) { void (*hook)(void) = commit_hook; commit_hook = NULL; hook(); }
    if (commit_failures) { --commit_failures; return ESP_FAIL; }
    memcpy(stored, staged, staged_size); stored_size = staged_size; exists = true;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { storage_access(); assert(handle == 42); ++closes; }
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, TaskHandle_t *out)
{
    assert(!in_callback && !locked && !arg);
    if (!strcmp(name, "digits_in_ring")) {
        assert(stack >= 8192 && priority == 4 && !ring_task_entry);
        ring_task_entry = entry; *out = (void *)2; return pdPASS;
    }
    assert(!strcmp(name, "digits_settings") && stack >= 4096 && priority == 2);
    if (fail_task) return 0;
    task_entry = entry; *out = (void *)1; return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t handle)
{
    assert(!locked);
    if (handle == (void *)2) { ++ring_notifications; return; }
    assert(handle == (void *)1); ++notifications;
}
unsigned ulTaskNotifyTake(int clear, TickType_t ticks)
{
    assert(clear && ticks == portMAX_DELAY && !locked && !in_callback);
    if (running_ring) {
        assert(speaker_owner != 2); // A worker may idle only after releasing its reservation.
        if (!ring_notifications) longjmp(ring_done, 1);
        unsigned result = ring_notifications; ring_notifications = 0; return result;
    }
    if (!notifications) longjmp(worker_done, 1);
    unsigned result = notifications; notifications = 0; return result;
}
void vTaskDelay(TickType_t ticks)
{
    assert(!locked && !in_callback && ticks == pdMS_TO_TICKS(1000));
    assert(++delays < 100); // A missed retry/dirty-clear cannot hang the harness.
    if (delay_hook) { void (*hook)(void) = delay_hook; delay_hook = NULL; hook(); }
}

static void run_worker(void) { assert(task_entry); if (!setjmp(worker_done)) task_entry(NULL); }
static void run_ring(void)
{
    assert(ring_task_entry && !running_ring);
    running_ring = true;
    if (!setjmp(ring_done)) ring_task_entry(NULL);
    running_ring = false;
}
int64_t esp_timer_get_time(void) { return now_us; }
i2s_chan_handle_t digits_audio_rx_channel(void) { return (void *)3; }
bool digits_audio_speaker_acquire(uint32_t timeout_ms)
{
    assert(running_ring && !in_callback && !locked && !timeout_ms);
    if (speaker_owner) return false;
    speaker_owner = 2; return true;
}
esp_err_t digits_audio_speaker_start(void)
{
    assert(running_ring && !in_callback && !locked && speaker_owner == 2);
    ++starts; return ESP_OK;
}
esp_err_t digits_audio_speaker_write(const int16_t *pcm, size_t bytes,
                                    size_t *written, uint32_t timeout_ms)
{
    assert(running_ring && !in_callback && !locked && speaker_owner == 2);
    assert(bytes == DIGITS_AUDIO_DMA_FRAMES * 2 * sizeof(*pcm) && timeout_ms == 20);
    bool audible = false;
    for (size_t i = 0; i < bytes / sizeof(*pcm); i += 2) {
        assert(pcm[i] == pcm[i + 1]);
        audible |= pcm[i] != 0;
    }
    assert(audible && ++writes < 100);
    *written = bytes; now_us += 10000;
    if (speaker_write_hook) {
        void (*hook)(void) = speaker_write_hook; speaker_write_hook = NULL; hook();
    }
    return ESP_OK;
}
bool digits_audio_speaker_silence(void)
{
    assert(running_ring && !in_callback && !locked && speaker_owner == 2);
    ++silences; return true;
}
void digits_audio_speaker_release(void)
{
    assert(running_ring && !in_callback && !locked && speaker_owner == 2);
    ++releases; speaker_owner = 0;
}
static void reboot(void)
{
    assert(!locked);
    memset(&settings, 0, sizeof(settings)); settings.current = defaults;
    task_entry = NULL; notifications = delays = 0;
    commit_hook = delay_hook = unlock_hook = NULL;
    assert(digits_settings_init() == ESP_OK);
    assert(opens == closes);
}
static bool receive(const char *json, uint32_t session)
{
    cJSON *message = cJSON_Parse(json); assert(message);
    unsigned previous_io = nvs_calls, previous_logs = logs;
    in_callback = true;
    bool result = digits_settings_receive(message, session);
    in_callback = false;
    assert(nvs_calls == previous_io && logs == previous_logs);
    cJSON_Delete(message); return result;
}
static digits_line_settings_t snapshot(void)
{ digits_line_settings_t s; digits_settings_get(&s, NULL); return s; }
static digits_settings_stats_t stats(void)
{ digits_settings_stats_t s; digits_settings_get(NULL, &s); return s; }
static void newer_snapshot(void)
{
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{\"voice_style\":\"modern\",\"silent_mode\":true}}", 9));
}
static void stop_media(void) { digits_settings_set_media_busy(false); }
static void invalidate_during_parse(void) { digits_settings_transport(10, false); }

// Match signaling's accepted-snapshot ordering while exercising both real
// modules. JSON allocation belongs to the WS parser, outside this callback.
static bool receive_and_apply(const char *json, uint32_t session)
{
    unsigned previous_io = nvs_calls, previous_logs = logs;
    unsigned previous_starts = starts, previous_writes = writes;
    unsigned previous_silences = silences, previous_releases = releases;
    bool accepted = receive(json, session);
    in_callback = true;
    if (accepted) digits_incoming_ring_set_silent(digits_settings_silent());
    in_callback = false;
    assert(nvs_calls == previous_io && logs == previous_logs);
    assert(starts == previous_starts && writes == previous_writes);
    assert(silences == previous_silences && releases == previous_releases);
    return accepted;
}
static const char *silent_on = "{\"type\":\"line_settings\",\"line_settings\":{\"silent_mode\":true}}";
static const char *silent_off = "{\"type\":\"line_settings\",\"line_settings\":{\"silent_mode\":false}}";
static void enable_silent_during_write(void)
{
    assert(receive_and_apply(silent_on, 80));
    assert(!digits_incoming_ring_is_stopped() && speaker_owner == 2);
}
static void answer_during_write(void)
{
    in_callback = true; digits_incoming_ring_stop(); in_callback = false;
    // Answer must wait for worker acknowledgement before taking the speaker.
    assert(!digits_incoming_ring_is_stopped() && speaker_owner == 2);
}
static void check_settings_ringing(void)
{
    reboot(); digits_settings_transport(80, true);
    assert(receive(silent_on, 80)); run_worker(); reboot();
    assert(stats().loaded && digits_settings_silent());
    ring_tone_init(); assert(digits_incoming_ring_init() == ESP_OK);
    digits_incoming_ring_set_silent(digits_settings_silent());
    digits_settings_transport(80, true);

    // Boot cache takes effect before the first ring; turning silence off does
    // not unexpectedly start sound for this already-silenced call.
    digits_incoming_ring_start(501, 0); run_ring();
    assert(digits_incoming_ring_is_stopped() && !writes && !starts);
    assert(receive_and_apply(silent_off, 80)); run_ring();
    assert(digits_incoming_ring_is_stopped() && !writes && !starts);

    // A subsequent call may ring. A live effective silent/Quiet Hours snapshot
    // stops it via its owning worker; the callback itself performs no I/O.
    digits_incoming_ring_stop(); digits_incoming_ring_start(502, 0);
    speaker_write_hook = enable_silent_during_write; run_ring();
    assert(writes == 1 && starts == 1 && silences == 1 && releases == 1);
    assert(digits_incoming_ring_is_stopped() && !speaker_owner && digits_settings_silent());
    assert(receive_and_apply(silent_off, 80)); run_ring();
    assert(writes == 1 && releases == 1); // Current-call latch survives un-silence.

    // Disconnect preserves cached effective silence. Stale/unregistered pushes
    // cannot change it; a fresh session may update the next call's behavior.
    assert(receive_and_apply(silent_on, 80));
    digits_incoming_ring_stop(); digits_incoming_ring_start(503, 0); run_ring();
    digits_settings_transport(80, false);
    assert(!receive_and_apply(silent_off, 80));
    digits_settings_transport(81, false);
    assert(!receive_and_apply(silent_off, 81));
    digits_settings_transport(81, true);
    assert(!receive_and_apply(silent_off, 80) && digits_settings_silent());
    assert(receive_and_apply(silent_off, 81)); run_ring();
    assert(!digits_settings_silent() && writes == 1);
    digits_incoming_ring_stop(); digits_incoming_ring_start(504, 0);
    speaker_write_hook = answer_during_write; run_ring();
    assert(writes == 2 && starts == 2 && silences == 2 && releases == 2);
    assert(digits_incoming_ring_is_stopped() && !speaker_owner);

    // Outgoing-call playback owns the same simulated reservation after the
    // ringer has stopped. Settings must neither release nor mute that owner.
    speaker_owner = 1;
    assert(receive_and_apply(silent_on, 81)); run_ring();
    assert(receive_and_apply(silent_off, 81)); run_ring();
    assert(speaker_owner == 1 && writes == 2 && starts == 2 && silences == 2 && releases == 2);
    assert(digits_incoming_ring_is_stopped()); speaker_owner = 0;
    assert(!memcmp(pairing, pairing_sentinel, sizeof(pairing)));
}

int main(void)
{
    memcpy(pairing, pairing_sentinel, sizeof(pairing));
    reboot();
    digits_line_settings_t s = snapshot();
    assert(s.voice_style == DIGITS_VOICE_COPPER && !s.silent_mode && !s.auto_update);
    assert(!s.voicemail_enabled && s.voicemail_ring_timeout_seconds == 20);
    assert(!stats().loaded && !stats().dirty && sets == 0);
    assert(digits_settings_init() == ESP_ERR_INVALID_STATE);
    assert(!receive("{\"type\":\"line_settings\",\"line_settings\":{\"silent_mode\":true}}", 1));
    digits_settings_transport(1, false); // Unpaired registration is never ready.
    assert(!receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 1));
    digits_settings_transport(1, true);
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 1));
    assert(stats().dirty); // First server snapshot repairs missing cache even if equal to defaults.
    assert(sets == 0); run_worker(); assert(sets == 1 && commits == 1 && !stats().dirty);
    const uint8_t expected_defaults[] = {'D','S','E','T',1,0,0,0,0,20,0,0};
    assert(stored_size == sizeof(expected_defaults) && !memcmp(stored, expected_defaults, stored_size));
    unsigned previous_sets = sets;
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 1));
    run_worker(); assert(sets == previous_sets && stats().duplicates == 1);

    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{\"voice_style\":\"modern\",\"silent_mode\":true,\"auto_update\":true,\"voicemail\":{\"enabled\":true,\"ring_timeout_seconds\":60},\"future\":{\"x\":1}}}", 1));
    s = snapshot();
    assert(s.voice_style == DIGITS_VOICE_MODERN && digits_settings_silent() && s.auto_update);
    assert(s.voicemail_enabled && s.voicemail_ring_timeout_seconds == 60);
    run_worker(); reboot(); s = snapshot();
    assert(stats().loaded && s.silent_mode && s.voice_style == DIGITS_VOICE_MODERN && s.auto_update);
    assert(s.voicemail_enabled && s.voicemail_ring_timeout_seconds == 60);
    digits_settings_transport(9, true);
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 9));
    s = snapshot();
    assert(!s.silent_mode && !s.auto_update && s.voice_style == DIGITS_VOICE_COPPER);
    assert(s.voicemail_enabled && s.voicemail_ring_timeout_seconds == 60); // Absent optional block retained.
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{\"voicemail\":null,\"voice_style\":\"unknown\"}}", 9));
    assert(snapshot().voicemail_enabled && snapshot().voicemail_ring_timeout_seconds == 60);
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{\"voicemail\":{}}}", 9));
    assert(!snapshot().voicemail_enabled && snapshot().voicemail_ring_timeout_seconds == 20);
    const char *timeouts[] = {"4", "61", "0", "-5", "5.5", "1e400", "null", "true", "\"30\""};
    for (unsigned i = 0; i < sizeof(timeouts)/sizeof(*timeouts); ++i) {
        char wire[200];
        snprintf(wire, sizeof(wire), "{\"type\":\"line_settings\",\"line_settings\":{\"voicemail\":{\"enabled\":true,\"ring_timeout_seconds\":%s}}}", timeouts[i]);
        assert(receive(wire, 9)); assert(snapshot().voicemail_ring_timeout_seconds == 20);
    }
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{\"silent_mode\":\"true\",\"auto_update\":1,\"voice_style\":null,\"voicemail\":{\"enabled\":false,\"ring_timeout_seconds\":5}}}", 9));
    s = snapshot(); assert(!s.silent_mode && !s.auto_update && !s.voicemail_enabled && s.voicemail_ring_timeout_seconds == 5);
    const char *invalid[] = {
        "{}", "null", "[]", "{\"type\":\"ring\",\"line_settings\":{}}",
        "{\"type\":\"line_settings\"}", "{\"type\":\"line_settings\",\"line_settings\":null}",
        "{\"type\":\"line_settings\",\"line_settings\":[]}",
        "{\"type\":\"line_settings\",\"line_settings\":{\"voicemail\":false}}",
        "{\"type\":\"line_settings\",\"line_settings\":{\"silent_mode\":true,\"silent_mode\":false}}",
        "{\"type\":\"line_settings\",\"line_settings\":{\"voicemail\":{\"enabled\":true,\"enabled\":false}}}",
    };
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
        uint32_t revision = stats().revision;
        assert(!receive(invalid[i], 9)); assert(stats().revision == revision);
    }
    cJSON *wide = cJSON_Parse("{\"type\":\"line_settings\",\"line_settings\":{}}");
    cJSON *body = cJSON_GetObjectItemCaseSensitive(wide, "line_settings");
    for (unsigned i = 0; i < 33; ++i) { char key[16]; snprintf(key, sizeof(key), "future%u", i); cJSON_AddBoolToObject(body, key, true); }
    in_callback = true; assert(!digits_settings_receive(wide, 9)); in_callback = false; cJSON_Delete(wide);

    run_worker();
    previous_sets = sets;
    digits_settings_transport(8, false); // Stale close cannot invalidate current session.
    newer_snapshot(); assert(digits_settings_silent());
    assert(!receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 8));
    assert(digits_settings_silent());
    digits_settings_transport(9, false);
    assert(!receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 9));
    digits_settings_transport(9, true);
    unlock_hook = invalidate_during_parse;
    assert(!receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 9));
    assert(digits_settings_silent());
    digits_settings_transport(10, true);
    assert(!receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 9));
    run_worker(); assert(sets == previous_sets + 1);

    // Failed commit retries without another snapshot, including duplicate snapshots.
    digits_settings_transport(11, true);
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 11));
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 11));
    commit_failures = 1; unsigned previous_commits = commits;
    run_worker(); assert(commits == previous_commits + 2 && stats().write_errors == 1 && !stats().dirty);
    assert(stats().last_write_error == ESP_OK && delays >= 1);

    // Failed set/open paths retain dirty and close only successfully opened handles.
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{\"silent_mode\":true}}", 11));
    set_failures = 1; assert(persist_pending() && stats().dirty);
    open_error = ESP_FAIL; assert(persist_pending() && stats().dirty); open_error = ESP_OK;
    assert(!persist_pending() && !stats().dirty && opens == closes);

    // A newer setting received during an old commit must remain dirty, on both outcomes.
    reboot(); digits_settings_transport(9, true);
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 9));
    commit_hook = newer_snapshot;
    assert(persist_pending());
    assert(stats().dirty && digits_settings_silent() && stored[6] == 0);
    assert(!persist_pending() && stored[5] == DIGITS_VOICE_MODERN && stored[6] == 1);
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 9));
    commit_failures = 1; commit_hook = newer_snapshot;
    assert(persist_pending() && stats().dirty);
    assert(!persist_pending() && !stats().dirty && stored[6] == 1);

    // Media uses latest RAM immediately; flash waits until ownership is released.
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 9));
    digits_settings_set_media_busy(true); previous_sets = sets;
    assert(persist_pending() && sets == previous_sets && !digits_settings_silent());
    delay_hook = stop_media; run_worker();
    assert(!stats().dirty && !stats().media_busy && sets == previous_sets + 1);

    // Unknown/truncated/corrupt cache never erases or rewrites any NVS at boot.
    for (unsigned kind = 0; kind < 7; ++kind) {
        memcpy(stored, expected_defaults, sizeof(expected_defaults)); stored_size = 12; exists = true;
        if (kind == 0) stored[4] = 99;
        if (kind == 1) stored_size = 3;
        if (kind == 2) stored_size = 13;
        if (kind == 3) stored[6] = 2;
        if (kind == 4) stored[9] = 61;
        if (kind == 5) stored[10] = 1;
        if (kind == 6) stored[0] = 0;
        previous_sets = sets; reboot();
        assert(!stats().loaded && !stats().dirty && !digits_settings_silent() && sets == previous_sets);
        digits_settings_transport(1, true);
        assert(receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 1));
        run_worker(); assert(stats().writes == 1 && sets == previous_sets + 1);
        assert(!memcmp(stored, expected_defaults, sizeof(expected_defaults)));
    }
    read_error = ESP_FAIL; reboot(); assert(stats().load_error == ESP_FAIL && !stats().loaded); read_error = ESP_OK;
    // Allocation failure disables persistence only. Preserve a valid silent
    // boot snapshot and still apply server silence/un-silence updates in RAM.
    memcpy(stored, expected_defaults, sizeof(expected_defaults)); stored[6] = 1;
    stored_size = 12; exists = true;
    memset(&settings, 0, sizeof(settings)); task_entry = NULL; notifications = 0; fail_task = true;
    assert(digits_settings_init() == ESP_ERR_NO_MEM);
    assert(settings.initialized && !settings.worker && !task_entry && stats().loaded);
    assert(digits_settings_silent() && stats().last_write_error == ESP_ERR_NO_MEM);
    fail_task = false;
    assert(digits_settings_init() == ESP_ERR_INVALID_STATE); // No runtime worker retry.
    unsigned previous_io = nvs_calls;
    digits_settings_transport(77, true);
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{\"voice_style\":\"modern\"}}", 77));
    assert(!digits_settings_silent() && snapshot().voice_style == DIGITS_VOICE_MODERN && stats().dirty);
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{\"silent_mode\":true}}", 77));
    assert(digits_settings_silent() && stats().dirty);
    digits_settings_set_media_busy(true); digits_settings_set_media_busy(false);
    assert(nvs_calls == previous_io && notifications == 0 && stored[6] == 1);
    assert(stats().last_write_error == ESP_ERR_NO_MEM && !stats().writes);
    assert(!receive("{\"type\":\"line_settings\",\"line_settings\":{}}", 76));
    reboot(); assert(digits_settings_silent() && stats().loaded && settings.worker);

    // Missing-cache plus worker failure also allows authenticated RAM silence.
    exists = false; memset(&settings, 0, sizeof(settings)); task_entry = NULL;
    notifications = 0; fail_task = true;
    assert(digits_settings_init() == ESP_ERR_NO_MEM && !stats().loaded);
    previous_io = nvs_calls;
    digits_settings_transport(1, true);
    assert(receive("{\"type\":\"line_settings\",\"line_settings\":{\"silent_mode\":true}}", 1));
    assert(digits_settings_silent() && stats().dirty && notifications == 0 && nvs_calls == previous_io);
    assert(stats().last_write_error == ESP_ERR_NO_MEM);
    fail_task = false;
    assert(!memcmp(pairing, pairing_sentinel, sizeof(pairing)) && opens == closes);
    digits_settings_get(NULL, NULL);
    check_settings_ringing();
    puts("Line settings: actual cJSON snapshots, NVS isolation, reboot, sessions, retry, concurrent revision, media deferral and parser-to-ringer lifecycle passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="digits-line-settings-") as temporary:
    build = Path(temporary)
    (build / "fake.h").write_text(fake_header)
    for name in ["esp_err.h", "esp_log.h", "esp_timer.h", "nvs.h", "freertos/FreeRTOS.h",
                 "freertos/task.h", "driver/i2c_master.h", "driver/i2s_std.h"]:
        path = build / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "fake.h"\n')
    (build / "test.c").write_text(harness)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
        # Apple marks sprintf in the unmodified IDF cJSON dependency deprecated.
        "-Wno-deprecated-declarations",
    ]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += ["-I", str(build), "-I", str(repo / "esp32/main"), "-I", str(cjson),
                str(build / "test.c"), str(repo / "esp32/main/incoming_ring.c"),
                str(repo / "esp32/main/ring_tone.c"), str(repo / "esp32/main/ringtones.c"), str(cjson / "cJSON.c"),
                "-lm", "-o", str(build / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(build / "test")], check=True)
