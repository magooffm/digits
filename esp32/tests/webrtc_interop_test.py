"""Exercise the real Digits peer worker with finite host peer/RTOS mocks.

This verifies adapter ordering, media queue ownership and cleanup. It does not
simulate ICE, DTLS, SRTP, packet loss, physical audio, or ESP32 scheduling use.
Requires an ESP-IDF checkout and the installed esp_peer public headers.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path, default=repo / "esp32/main/webrtc.c")
parser.add_argument("--idf-path", type=Path, default=os.environ.get("IDF_PATH"))
parser.add_argument(
    "--peer-include", type=Path,
    default=repo / "esp32/managed_components/espressif__esp_peer/include",
)
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
if args.idf_path is None:
    parser.error("Set IDF_PATH (source ESP-IDF export.sh) or pass --idf-path")
cjson_dir = args.idf_path / "components/json/cJSON"
if not (cjson_dir / "cJSON.c").is_file():
    parser.error(f"Missing ESP-IDF cJSON source: {cjson_dir}")
if not (args.peer_include / "esp_peer_default.h").is_file():
    parser.error("Install the project dependencies with idf.py build, or pass --peer-include")

headers = {
    "sdkconfig.h": "#define CONFIG_DIGITS_WEBRTC 1\n#define CONFIG_DIGITS_WEBRTC_AUTO_ANSWER 0\n",
    "esp_err.h": r'''
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
const char *esp_err_to_name(esp_err_t error);
''',
    "esp_heap_caps.h": r'''
#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
void *heap_caps_malloc_prefer(size_t size, size_t count, ...);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_minimum_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
''',
    "esp_log.h": r'''
#pragma once
void host_log(const char *tag, const char *format, ...);
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGE(...) host_log(__VA_ARGS__)
''',
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "esp_wifi.h": r'''
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct { int8_t rssi; } wifi_ap_record_t;
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
static inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *info) {
    info->rssi = -86;
    return ESP_OK;
}
static inline esp_err_t esp_wifi_get_ps(wifi_ps_type_t *mode) {
    *mode = WIFI_PS_MIN_MODEM;
    return ESP_OK;
}
''',
    "freertos/FreeRTOS.h": r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef unsigned TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define configTICK_RATE_HZ 1000
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
''',
    "freertos/queue.h": r'''
#pragma once
#include "FreeRTOS.h"
typedef struct host_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned length, unsigned item_size);
int xQueueSend(QueueHandle_t queue, const void *item, unsigned wait);
int xQueueReceive(QueueHandle_t queue, void *item, unsigned wait);
int xQueuePeek(QueueHandle_t queue, void *item, unsigned wait);
void vQueueDelete(QueueHandle_t queue);
''',
    "freertos/task.h": r'''
#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
                           unsigned stack, void *arg, unsigned priority,
                           TaskHandle_t *task, unsigned core);
unsigned xTaskNotifyGive(TaskHandle_t task);
unsigned ulTaskNotifyTake(bool clear, unsigned wait);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
''',
}

prefix = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_peer_default.h"
#include "webrtc.h"
#include "call_audio.h"
#include "line_settings.h"
#include "incoming_ring.h"
#include "call_tones.h"

static int64_t host_now;
static jmp_buf worker_boundary;
static bool in_worker;
static unsigned worker_iterations, requested_iterations;
static unsigned allocation_calls;
static int allocation_fail_after = -1;
static void (*task_entry)(void *);
static unsigned logs;
static char last_log[1024];
static bool queue_create_failure, task_create_failure;
static unsigned live_queues;

enum { TRACE_LIMIT = 4096, WIRE_LIMIT = 1024 };
static char trace[TRACE_LIMIT][40];
static unsigned trace_count;
static char *wire[WIRE_LIMIT];
static unsigned wire_count;
static uint32_t wire_session[WIRE_LIMIT];
static unsigned open_calls, new_calls, close_calls, loop_calls, candidate_calls, sdp_calls;
static unsigned audio_calls, query_calls;
static unsigned audio_pts[WIRE_LIMIT];
static int64_t audio_times[WIRE_LIMIT];
static unsigned last_worker_wait, largest_connected_wait;
static unsigned media_start_calls, media_stop_calls, media_destroy_calls;
static unsigned media_receive_calls, media_pop_calls, media_record_calls;
static esp_err_t media_start_result;
static digits_call_audio_state_t media_initial_state;
static bool media_stop_completes;
static bool ring_stopped = true, ring_stop_completes, settings_busy;
static unsigned ring_starts, ring_stops;
static uint32_t last_ring_id;
static digits_line_settings_t settings;
static int64_t main_loop_delay_us;
struct digits_call_audio {
    bool live;
    digits_call_audio_mode_t mode;
    digits_call_audio_stats_t stats;
    call_audio_packet_t encoded[16];
    unsigned encoded_count;
    call_audio_packet_t last_popped;
    call_audio_packet_t received;
    const digits_call_audio_unattended_t *hooks; // Set while unattended.
    unsigned attends;
};
static struct digits_call_audio media;
static esp_peer_cfg_t peer_config;
static bool peer_opened, peer_new, mock_remote_sdp, local_sdp_emitted;
static bool emit_local_on_new, connect_next_loop, disconnect_during_loop;
static bool suppress_local_sdp, disconnect_on_sdp_transmit;
static const char *reject_transmit_type;
static int open_result, new_result, loop_result, sdp_result, candidate_result, audio_result;
static esp_err_t transmit_result;
static uint32_t peer_session;
static char remote_sdp[16384];
static const char *borrowed_remote_sdp;
static char remote_candidates[64][1024];
static const char *local_sdp_text =
    "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
    "a=group:BUNDLE 0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "a=mid:0\r\na=rtpmap:111 opus/48000/2\r\na=sendrecv\r\n"
    "a=candidate:1 1 UDP 2130706431 192.0.2.10 12345 typ host\r\n";
static const char *candidate_text =
    "candidate:123456 1 udp 2122260223 192.0.2.42 49152 typ host";

static void add_trace(const char *what)
{
    assert(trace_count < TRACE_LIMIT);
    snprintf(trace[trace_count++], sizeof(trace[0]), "%s", what);
}

static unsigned trace_index(const char *what, unsigned start)
{
    for (unsigned i = start; i < trace_count; ++i)
        if (!strcmp(trace[i], what)) return i;
    assert(!"Missing trace event");
    return 0;
}

static int emit_local_sdp(void)
{
    assert(in_worker && peer_opened && peer_new);
    esp_peer_msg_t message = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)local_sdp_text,
        .size = (int)strlen(local_sdp_text),
    };
    local_sdp_emitted = true;
    add_trace("callback:local-sdp");
    return peer_config.on_msg(&message, peer_config.ctx);
}

static int emit_peer_state(esp_peer_state_t state)
{
    assert(in_worker && peer_opened);
    return peer_config.on_state(state, peer_config.ctx);
}
'''

runtime = r'''
void digits_settings_get(digits_line_settings_t *snapshot, digits_settings_stats_t *stats)
{
    if (snapshot) *snapshot = settings;
    if (stats) memset(stats, 0, sizeof(*stats));
}
void digits_settings_set_media_busy(bool busy) { settings_busy = busy; }
// Answering machine: storage and recording are tested in voicemail_*_test.c.
static bool vm_ready, vm_full, vm_begin_fails, vm_session;
static int vm_unheard = -1;
static unsigned vm_begins, vm_ends;
static char vm_caller[32];
static void vm_fill(int16_t *mono, size_t frames) { (void)mono; (void)frames; }
static void vm_record(const uint8_t *packet, size_t size) { (void)packet; (void)size; }
static const digits_call_audio_unattended_t vm_hooks = {.fill = vm_fill, .record = vm_record};
bool digits_voicemail_ready(void) { return vm_ready; }
int digits_voicemail_unheard(void) { return vm_unheard; }
const digits_call_audio_unattended_t *digits_voicemail_begin(const char *caller)
{
    assert(in_worker && caller && !vm_session);
    ++vm_begins;
    if (vm_begin_fails) return NULL;
    snprintf(vm_caller, sizeof(vm_caller), "%s", caller);
    vm_session = true;
    return &vm_hooks;
}
void digits_voicemail_end(void)
{
    assert(in_worker);
    if (vm_session) ++vm_ends;
    vm_session = false;
}
bool digits_voicemail_full(void) { return vm_session && vm_full; }
static unsigned last_ringtone = 99;
void digits_incoming_ring_start(uint32_t id, unsigned ringtone)
{
    assert(in_worker && id && id != last_ring_id);
    last_ring_id = id;
    last_ringtone = ringtone;
    ++ring_starts;
    ring_stopped = false;
}
void digits_incoming_ring_stop(void)
{
    assert(in_worker);
    ++ring_stops;
    if (ring_stop_completes) ring_stopped = true;
}
bool digits_incoming_ring_is_stopped(void) { return ring_stopped; }
// Call-progress tones: the peer worker only ever stops them (at CONNECTED)
// and must not start call audio until the player has released the speaker.
static bool tones_stopped = true, tones_stop_completes = true;
static unsigned tone_stop_requests;
void digits_call_tones_stop(void)
{
    assert(in_worker);
    ++tone_stop_requests;
    if (tones_stop_completes) tones_stopped = true;
}
bool digits_call_tones_is_stopped(void) { return tones_stopped; }

struct host_queue {
    unsigned length, size, head, count;
    unsigned char *items;
};

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{
    if (queue_create_failure) return NULL;
    struct host_queue *queue = calloc(1, sizeof(*queue));
    assert(queue);
    queue->length = length;
    queue->size = item_size;
    queue->items = calloc(length, item_size);
    assert(queue->items);
    ++live_queues;
    return queue;
}

int xQueueSend(QueueHandle_t queue, const void *item, unsigned wait)
{
    assert(queue && wait == 0);
    if (queue->count == queue->length) return pdFALSE;
    unsigned tail = (queue->head + queue->count) % queue->length;
    memcpy(queue->items + tail * queue->size, item, queue->size);
    ++queue->count;
    return pdTRUE;
}

int xQueueReceive(QueueHandle_t queue, void *item, unsigned wait)
{
    assert(queue && wait == 0 && in_worker);
    if (!queue->count) return pdFALSE;
    memcpy(item, queue->items + queue->head * queue->size, queue->size);
    queue->head = (queue->head + 1) % queue->length;
    --queue->count;
    return pdTRUE;
}

int xQueuePeek(QueueHandle_t queue, void *item, unsigned wait)
{
    assert(queue && wait == 0 && in_worker);
    if (!queue->count) return pdFALSE;
    memcpy(item, queue->items + queue->head * queue->size, queue->size);
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t queue)
{
    assert(queue && !queue->count);
    assert(live_queues);
    --live_queues;
    free(queue->items);
    free(queue);
}

int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name,
                           unsigned stack, void *arg, unsigned priority,
                           TaskHandle_t *task, unsigned core)
{
    assert(entry && name && stack >= 10240 && arg == NULL);
    assert(priority == 6 && core == 1); // Network progress precedes priority-5 codec/capture work.
    if (task_create_failure) return pdFALSE;
    task_entry = entry;
    *task = (void *)1;
    return pdPASS;
}

unsigned xTaskNotifyGive(TaskHandle_t task)
{
    assert(task == (void *)1);
    return 1;
}

unsigned ulTaskNotifyTake(bool clear, unsigned wait)
{
    assert(in_worker && clear);
    last_worker_wait = wait;
    if (rtc.call.phase == CALL_CONNECTED) {
        assert(wait <= 1); // The peer owner must not add another 10 ms delay.
        if (wait > largest_connected_wait) largest_connected_wait = wait;
    }
    if (++worker_iterations == requested_iterations) longjmp(worker_boundary, 1);
    return 0;
}

unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    assert(task == NULL);
    return 4096; // Synthetic; never reported as ESP32 memory measurements.
}

void *heap_caps_malloc_prefer(size_t size, size_t count, ...)
{
    assert(count == 2 && size);
    if (allocation_fail_after >= 0 && allocation_calls++ >= (unsigned)allocation_fail_after)
        return NULL;
    return malloc(size);
}

size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 100000; }
size_t heap_caps_get_minimum_free_size(unsigned caps) { (void)caps; return 80000; }
size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 60000; }
int64_t esp_timer_get_time(void) { return host_now; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "host error"; }

void host_log(const char *tag, const char *format, ...)
{
    assert(!strcmp(tag, "webrtc"));
    va_list args;
    va_start(args, format);
    vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args);
    ++logs;
}

static esp_err_t host_transmit(const cJSON *message, uint32_t session, void *context)
{
    assert(in_worker && context == (void *)0x42);
    assert(session == peer_session);
    const char *type = json_text(message, "type");
    if (transmit_result || (reject_transmit_type && !strcmp(type, reject_transmit_type)))
        return transmit_result ? transmit_result : ESP_ERR_NO_MEM;
    assert(wire_count < WIRE_LIMIT);
    wire[wire_count] = cJSON_PrintUnformatted(message);
    assert(wire[wire_count]);
    wire_session[wire_count++] = session;
    char event[40];
    snprintf(event, sizeof(event), "wire:%s", type);
    add_trace(event);
    if (disconnect_on_sdp_transmit && (!strcmp(type, "sdp") || !strcmp(type, "answer"))) {
        disconnect_on_sdp_transmit = false;
        digits_webrtc_transport(peer_session, false, "1001", true);
    }
    return ESP_OK;
}

const esp_peer_ops_t *esp_peer_get_default_impl(void)
{
    static esp_peer_ops_t ops;
    assert(in_worker);
    return &ops;
}

int esp_peer_open(esp_peer_cfg_t *config, const esp_peer_ops_t *ops,
                  esp_peer_handle_t *handle)
{
    assert(in_worker && config && ops && handle && !peer_opened);
    assert(config->audio_info.codec == ESP_PEER_AUDIO_CODEC_OPUS);
    assert(config->audio_info.sample_rate == 48000 && config->audio_info.channel == 1);
    assert(config->audio_dir == ESP_PEER_MEDIA_DIR_SEND_RECV);
    assert(config->video_dir == ESP_PEER_MEDIA_DIR_NONE && !config->enable_data_channel);
    assert(config->no_auto_reconnect && config->extra_cfg);
    const esp_peer_default_cfg_t *defaults = config->extra_cfg;
    assert(defaults->agent_recv_timeout == 5); // Nonzero: zero selects the long default.
    assert(defaults->rtp_cfg.send_pool_size > 0 && defaults->rtp_cfg.send_pool_size <= 16384);
    assert(defaults->rtp_cfg.audio_recv_jitter.cache_size > 0 &&
           defaults->rtp_cfg.audio_recv_jitter.cache_size <= 16384);
    for (unsigned i = 0; i < config->server_num; ++i)
        assert(config->server_lists[i].stun_url &&
               !strncmp(config->server_lists[i].stun_url, "stun:", 5));
    ++open_calls;
    add_trace("peer:open");
    if (open_result) return open_result;
    peer_config = *config;
    peer_opened = true;
    *handle = (void *)0x1234;
    peer_new = mock_remote_sdp = local_sdp_emitted = false;
    return ESP_PEER_ERR_NONE;
}

int esp_peer_new_connection(esp_peer_handle_t peer)
{
    assert(in_worker && peer == (void *)0x1234 && peer_opened && !peer_new);
    ++new_calls;
    add_trace("peer:new");
    if (new_result) return new_result;
    peer_new = true;
    emit_peer_state(ESP_PEER_STATE_NEW_CONNECTION);
    if (emit_local_on_new) return emit_local_sdp();
    return ESP_PEER_ERR_NONE;
}

int esp_peer_send_msg(esp_peer_handle_t peer, esp_peer_msg_t *message)
{
    assert(in_worker && peer == (void *)0x1234 && peer_opened && peer_new);
    assert(message && message->data && message->size >= 0);
    assert((size_t)message->size == strlen((const char *)message->data));
    if (message->type == ESP_PEER_MSG_TYPE_SDP) {
        assert((size_t)message->size < sizeof(remote_sdp));
        memcpy(remote_sdp, message->data, (size_t)message->size + 1);
        borrowed_remote_sdp = (const char *)message->data;
        ++sdp_calls;
        add_trace("peer:remote-sdp");
        if (sdp_result) return sdp_result;
        mock_remote_sdp = true;
    } else {
        assert(message->type == ESP_PEER_MSG_TYPE_CANDIDATE);
        assert(mock_remote_sdp && candidate_calls < 64);
        assert(!strncmp((const char *)message->data, "candidate:", 10));
        snprintf(remote_candidates[candidate_calls++], sizeof(remote_candidates[0]), "%s", message->data);
        add_trace("peer:remote-ice");
        if (candidate_result) return candidate_result;
    }
    return ESP_PEER_ERR_NONE;
}

int esp_peer_main_loop(esp_peer_handle_t peer)
{
    assert(in_worker && peer == (void *)0x1234 && peer_opened && peer_new);
    if (peer_config.role == ESP_PEER_ROLE_CONTROLLED) assert(mock_remote_sdp);
    ++loop_calls;
    host_now += main_loop_delay_us;
    add_trace("peer:loop");
    if (disconnect_during_loop) {
        digits_webrtc_transport(peer_session, false, "1001", true);
        disconnect_during_loop = false;
    }
    if (!local_sdp_emitted && !suppress_local_sdp) emit_local_sdp();
    if (connect_next_loop) {
        connect_next_loop = false;
        assert(mock_remote_sdp);
        emit_peer_state(ESP_PEER_STATE_PAIRED);
        emit_peer_state(ESP_PEER_STATE_CONNECTING);
        emit_peer_state(ESP_PEER_STATE_CONNECTED);
    }
    return loop_result;
}

int esp_peer_send_audio(esp_peer_handle_t peer, esp_peer_audio_frame_t *frame)
{
    assert(in_worker && peer == (void *)0x1234 && peer_opened);
    assert(rtc.call.phase == CALL_CONNECTED && mock_remote_sdp);
    assert(media.live && (media.mode & DIGITS_CALL_AUDIO_TX));
    assert(frame && frame->size == media.last_popped.size && frame->size == 5);
    assert(!memcmp(frame->data, media.last_popped.data, (size_t)frame->size));
    assert(frame->pts == media.last_popped.pts_ms);
    assert(audio_calls < WIRE_LIMIT);
    audio_pts[audio_calls] = frame->pts;
    audio_times[audio_calls++] = host_now;
    add_trace("peer:audio");
    return audio_result;
}

int esp_peer_query(esp_peer_handle_t peer)
{
    assert(in_worker && peer == (void *)0x1234 && peer_opened);
    ++query_calls;
    return ESP_PEER_ERR_NONE;
}

int esp_peer_close(esp_peer_handle_t peer)
{
    assert(in_worker && peer == (void *)0x1234 && peer_opened);
    ++close_calls;
    add_trace("peer:close");
    emit_peer_state(ESP_PEER_STATE_CLOSED);
    peer_opened = false;
    peer_new = false;
    return ESP_PEER_ERR_NONE;
}

esp_err_t digits_call_audio_start(digits_call_audio_mode_t mode, digits_call_audio_t **out);
esp_err_t digits_call_audio_start_unattended(digits_call_audio_mode_t mode,
                                             const digits_call_audio_unattended_t *hooks,
                                             digits_call_audio_t **out)
{
    assert(hooks == &vm_hooks && vm_session); // The machine was ready first.
    esp_err_t err = digits_call_audio_start(mode, out);
    if (err == ESP_OK) media.hooks = hooks;   // Unattended from the first frame.
    return err;
}

void digits_call_audio_attend(digits_call_audio_t *audio)
{
    assert(in_worker && audio == &media && media.live);
    media.hooks = NULL;
    ++media.attends;
}

esp_err_t digits_call_audio_start(digits_call_audio_mode_t mode, digits_call_audio_t **out)
{
    assert(in_worker && out && !media.live && ring_stopped && tones_stopped);
    assert(mode >= DIGITS_CALL_AUDIO_TX && mode <= DIGITS_CALL_AUDIO_FULL);
    ++media_start_calls;
    add_trace("media:start");
    *out = NULL;
    if (media_start_result) return media_start_result;
    memset(&media, 0, sizeof(media));
    media.live = true;
    media.mode = mode;
    media.stats.state = media_initial_state;
    *out = &media;
    return ESP_OK;
}

void digits_call_audio_get_stats(digits_call_audio_t *audio, digits_call_audio_stats_t *stats)
{
    assert(in_worker && audio == &media && media.live && stats);
    *stats = media.stats;
}

bool digits_call_audio_pop_encoded(digits_call_audio_t *audio, call_audio_packet_t *packet)
{
    assert(in_worker && audio == &media && media.live && packet);
    ++media_pop_calls;
    if (!media.encoded_count) return false;
    // Model the engine's published-frame contract, not the ADC/codec internals:
    // one fresh latest encoded frame is available after a producer backlog.
    *packet = media.encoded[media.encoded_count - 1];
    media.stats.tx_pop_drops += media.encoded_count - 1;
    media.encoded_count = 0;
    if (host_now - packet->captured_at_us > 60000) {
        ++media.stats.tx_pop_drops;
        return false;
    }
    media.last_popped = *packet;
    return true;
}

void digits_call_audio_record_send(digits_call_audio_t *audio, const call_audio_packet_t *packet,
                                   bool success, bool blocked)
{
    assert(in_worker && audio == &media && media.live && packet);
    assert(!(success && blocked));
    ++media_record_calls;
    if (success) ++media.stats.sent_frames;
    else if (blocked) ++media.stats.send_blocked;
    else ++media.stats.send_errors;
}

bool digits_call_audio_receive(digits_call_audio_t *audio, uint32_t pts_ms,
                               const uint8_t *data, size_t size)
{
    assert(in_worker && audio == &media && media.live && data && size);
    assert(rtc.call.phase == CALL_CONNECTED && size <= sizeof(media.received.data));
    ++media_receive_calls;
    media.received.pts_ms = pts_ms;
    media.received.size = (uint16_t)size;
    memcpy(media.received.data, data, size); // The real engine owns this copy boundary.
    return true;
}

void digits_call_audio_request_stop(digits_call_audio_t *audio)
{
    assert(in_worker && audio == &media && media.live);
    ++media_stop_calls;
    add_trace("media:stop");
    media.stats.state = media_stop_completes ? DIGITS_CALL_AUDIO_STOPPED : DIGITS_CALL_AUDIO_STOPPING;
}

bool digits_call_audio_is_stopped(digits_call_audio_t *audio)
{
    assert(in_worker && audio == &media && media.live);
    return media.stats.state == DIGITS_CALL_AUDIO_STOPPED;
}

esp_err_t digits_call_audio_destroy(digits_call_audio_t *audio)
{
    assert(in_worker && audio == &media && media.live && media.stats.state == DIGITS_CALL_AUDIO_STOPPED);
    assert(!peer_opened); // No peer callback may retain an audio-engine handle.
    ++media_destroy_calls;
    add_trace("media:destroy");
    media.live = false;
    return ESP_OK;
}

static void queue_microphone_packet(uint32_t pts_ms, int64_t captured_at_us)
{
    assert(!in_worker && media.live && (media.mode & DIGITS_CALL_AUDIO_TX));
    assert(media.encoded_count < sizeof(media.encoded) / sizeof(media.encoded[0]));
    call_audio_packet_t *packet = &media.encoded[media.encoded_count++];
    memset(packet, 0, sizeof(*packet));
    packet->pts_ms = pts_ms;
    packet->captured_at_us = captured_at_us;
    packet->size = 5;
    packet->data[0] = 0x71;
    packet->data[1] = 0x82;
    packet->data[2] = (uint8_t)pts_ms;
    packet->data[3] = (uint8_t)(pts_ms >> 8);
    packet->data[4] = 0xa9; // Distinct encoded media; never the old canned silence.
}

static void pump(unsigned iterations)
{
    assert(iterations && !in_worker && task_entry);
    worker_iterations = 0;
    requested_iterations = iterations;
    in_worker = true;
    if (!setjmp(worker_boundary)) task_entry(NULL);
    in_worker = false;
    assert(worker_iterations == iterations);
}

static void clear_wire(void)
{
    for (unsigned i = 0; i < wire_count; ++i) free(wire[i]);
    memset(wire, 0, sizeof(wire));
    wire_count = trace_count = 0;
}

static cJSON *wire_message(unsigned i, const char *type)
{
    assert(i < wire_count && wire_session[i] == peer_session);
    cJSON *message = cJSON_Parse(wire[i]);
    assert(message && !strcmp(json_text(message, "type"), type));
    return message;
}

static unsigned wires_of_type(const char *type)
{
    unsigned count = 0;
    for (unsigned i = 0; i < wire_count; ++i) {
        cJSON *message = cJSON_Parse(wire[i]);
        assert(message);
        count += !strcmp(json_text(message, "type"), type);
        cJSON_Delete(message);
    }
    return count;
}

static void receive(const char *type, const char *from, const char *field,
                    const char *data, const char *conference)
{
    cJSON *message = cJSON_CreateObject();
    assert(message && cJSON_AddStringToObject(message, "type", type));
    if (from) assert(cJSON_AddStringToObject(message, "from", from));
    if (field) assert(cJSON_AddStringToObject(message, field, data));
    if (conference) assert(cJSON_AddStringToObject(message, "conf_id", conference));
    unsigned before = open_calls + close_calls + audio_calls + sdp_calls + loop_calls;
    assert(digits_webrtc_receive(message, peer_session));
    // Delete the JSON immediately: the callback must retain its own copy.
    cJSON_Delete(message);
    assert(before == open_calls + close_calls + audio_calls + sdp_calls + loop_calls);
}

static void command(const char *verb, const char *number)
{
    unsigned before = open_calls + close_calls + audio_calls + sdp_calls + loop_calls;
    assert(digits_webrtc_command(verb, number) == ESP_OK);
    assert(before == open_calls + close_calls + audio_calls + sdp_calls + loop_calls);
}

static void setup(void)
{
    assert(!peer_opened);
    assert(!media.live);
    assert(!live_queues);
    clear_wire();
    memset(&rtc, 0, sizeof(rtc));
    memset(&latest_transport, 0, sizeof(latest_transport));
    memset(&peer_config, 0, sizeof(peer_config));
    open_calls = new_calls = close_calls = loop_calls = candidate_calls = sdp_calls = 0;
    audio_calls = query_calls = logs = 0;
    media_start_calls = media_stop_calls = media_destroy_calls = 0;
    media_receive_calls = media_pop_calls = media_record_calls = 0;
    media_start_result = ESP_OK;
    media_initial_state = DIGITS_CALL_AUDIO_READY;
    media_stop_completes = true;
    ring_stopped = ring_stop_completes = true;
    tones_stopped = tones_stop_completes = true;
    tone_stop_requests = 0;
    settings_busy = false;
    ring_starts = ring_stops = last_ring_id = 0;
    memset(&settings, 0, sizeof(settings));
    vm_ready = vm_full = vm_begin_fails = vm_session = false;
    vm_unheard = -1;
    vm_begins = vm_ends = 0;
    vm_caller[0] = '\0';
    main_loop_delay_us = 0;
    last_worker_wait = largest_connected_wait = 0;
    memset(&media, 0, sizeof(media));
    open_result = new_result = loop_result = sdp_result = candidate_result = audio_result = 0;
    transmit_result = ESP_OK;
    allocation_fail_after = -1;
    allocation_calls = 0;
    queue_create_failure = task_create_failure = false;
    emit_local_on_new = connect_next_loop = disconnect_during_loop = false;
    suppress_local_sdp = disconnect_on_sdp_transmit = false;
    reject_transmit_type = borrowed_remote_sdp = NULL;
    peer_new = mock_remote_sdp = local_sdp_emitted = false;
    host_now = 1000000;
    peer_session = 7;
    assert(digits_webrtc_init(NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(digits_webrtc_init(host_transmit, (void *)0x42) == ESP_OK);
    assert(digits_webrtc_init(host_transmit, (void *)0x42) == ESP_ERR_INVALID_STATE);
    digits_webrtc_transport(peer_session, true, "1001", true);
    pump(1);
    assert(wire_count == 1);
    cJSON_Delete(wire_message(0, "request-ice-servers"));
    clear_wire();
}

static void finish(void)
{
    transmit_result = ESP_OK;
    if (rtc.call.phase != CALL_IDLE || rtc.call.peer) {
        command("hangup", NULL);
        pump(1);
    }
    digits_webrtc_transport(peer_session, false, "1001", true);
    pump(1);
    assert(!peer_opened && rtc.call.phase == CALL_IDLE && rtc.call.peer == NULL);
    assert(!media.live && !rtc.stopping_audio);
    assert(ring_stopped && !settings_busy && !vm_session);
    assert(!rtc.call.offer && !rtc.call.candidate_count);
    assert(!rtc.call.remote_sdp_text && !rtc.call.server_count);
    assert(rtc.ice_count == 0 && rtc.queue->count == 0);
    vQueueDelete(rtc.queue);
    rtc.queue = NULL;
    assert(!live_queues);
    clear_wire();
}

static void start_outgoing(void)
{
    command("call", "1002");
    pump(1);
    assert(rtc.call.phase == CALL_CALLING && rtc.call.caller && peer_opened);
    assert(peer_config.role == ESP_PEER_ROLE_CONTROLLING && new_calls == 1);
    assert(wire_count == 2 && rtc.call.local_sdp && !rtc.call.remote_sdp);
    assert(trace_index("wire:call", 0) < trace_index("peer:open", 0));
    assert(trace_index("wire:call", 0) < trace_index("wire:sdp", 0));
    cJSON *call = wire_message(0, "call");
    assert(!strcmp(json_text(call, "to"), "1002"));
    cJSON_Delete(call);
    cJSON *offer = wire_message(1, "sdp");
    assert(!strcmp(json_text(offer, "sdp"), local_sdp_text));
    cJSON_Delete(offer);
    assert(audio_calls == 0 && close_calls == 0);
    assert(media_start_calls == 0); // Signaling and SDP alone cannot open physical media.
}

static void apply_answer(void)
{
    unsigned before_audio = audio_calls, before_start = media_start_calls;
    receive("answer", "1002", "sdp", local_sdp_text, NULL);
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTING && rtc.call.remote_sdp);
    assert(!strcmp(remote_sdp, local_sdp_text));
    assert(audio_calls == before_audio); // An SDP answer is not ICE/DTLS/SRTP success.
    assert(media_start_calls == before_start);
}

static void connect_peer(void)
{
    connect_next_loop = true;
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTED && rtc.call.deadline_us == 0);
    assert(media_start_calls == 1 && media.live && rtc.call.audio == &media);
    assert(audio_calls == 0); // START is asynchronous; no manufactured silence is sent.
    if (media.mode & DIGITS_CALL_AUDIO_TX) {
        queue_microphone_packet(0, host_now);
        pump(1);
    }
    if (!(media.mode & DIGITS_CALL_AUDIO_TX)) return;
    assert(audio_calls == 1 && audio_pts[0] == 0);
}

static unsigned cases;
'''

tests = r'''
static void test_initialization_failures(void)
{
    for (unsigned mode = 0; mode < 2; ++mode) {
        memset(&rtc, 0, sizeof(rtc));
        assert(!live_queues);
        queue_create_failure = mode == 0;
        task_create_failure = mode == 1;
        assert(digits_webrtc_init(host_transmit, (void *)0x42) == ESP_ERR_NO_MEM);
        assert(!rtc.queue && !live_queues && !peer_opened);
        assert(digits_webrtc_command("status", NULL) == ESP_ERR_INVALID_STATE);
        queue_create_failure = task_create_failure = false;
        // Failed initialization must permit a fresh successful attempt.
        assert(digits_webrtc_init(host_transmit, (void *)0x42) == ESP_OK);
        assert(rtc.queue && live_queues == 1);
        vQueueDelete(rtc.queue);
        rtc.queue = NULL;
        assert(!live_queues);
        ++cases;
    }
}

static void test_outgoing_and_media(void)
{
    setup();
    start_outgoing();
    receive("ice", "1002", "candidate", candidate_text, NULL);
    pump(1);
    assert(rtc.call.candidate_count == 1 && candidate_calls == 0);
    apply_answer();
    assert(rtc.call.candidate_count == 0 && candidate_calls == 1);
    assert(!strcmp(remote_candidates[0], candidate_text));
    connect_peer();
    pump(3);
    assert(audio_calls == 1);
    host_now += 19999;
    pump(1);
    assert(audio_calls == 1);
    ++host_now;
    queue_microphone_packet(20, host_now);
    pump(1);
    assert(audio_calls == 2 && audio_pts[1] == 20);
    host_now += 100000;
    queue_microphone_packet(40, host_now - 80000);
    queue_microphone_packet(60, host_now - 60000);
    queue_microphone_packet(80, host_now - 40000);
    queue_microphone_packet(100, host_now - 20000);
    queue_microphone_packet(120, host_now);
    pump(1);
    assert(audio_calls == 3 && audio_pts[2] == 120 && rtc.call.skipped_audio == 4);
    pump(5);
    assert(audio_calls == 3); // Never catch up by flooding late RTP frames.
    audio_result = ESP_PEER_ERR_WOULD_BLOCK;
    host_now += 20000;
    queue_microphone_packet(140, host_now);
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTED && rtc.call.blocked_audio == 1);
    assert(rtc.call.tx_packets == 3);
    assert(media.stats.sent_frames == 3 && media.stats.send_blocked == 1);
    assert(media_record_calls == 4 && media.stats.tx_pop_drops == 4);
    unsigned before_rx = rtc.call.rx_packets;
    unsigned char payload[] = {0xf8, 0xff, 0xfe};
    esp_peer_audio_frame_t frame = {.data = payload, .size = 3, .pts = 20};
    in_worker = true;
    assert(on_audio_data(&frame, NULL) == 0);
    in_worker = false;
    memset(payload, 0, sizeof(payload));
    assert(rtc.call.rx_packets == before_rx + 1 && rtc.call.rx_bytes == 3);
    assert(media_receive_calls == 1 && media.received.pts_ms == 20 && media.received.size == 3);
    static const unsigned char expected_received[] = {0xf8, 0xff, 0xfe};
    assert(!memcmp(media.received.data, expected_received, sizeof(expected_received)));
    command("status", NULL);
    pump(1);
    assert(query_calls == 1);
    unsigned wire_before = wire_count;
    command("hangup", NULL);
    pump(1);
    assert(!peer_opened && close_calls == 1 && wire_count == wire_before + 1);
    cJSON_Delete(wire_message(wire_before, "hangup"));
    unsigned before = audio_calls;
    host_now += 1000000;
    pump(1);
    assert(audio_calls == before);
    finish();
    ++cases;
}

static void test_incoming(bool answer_before_offer)
{
    setup();
    receive("ring", "1002", NULL, NULL, NULL);
    pump(1);
    assert(rtc.call.phase == CALL_RINGING && !peer_opened && wire_count == 0);
    receive("ice", "1002", "candidate", candidate_text, NULL);
    pump(1);
    assert(rtc.call.candidate_count == 1 && candidate_calls == 0);
    if (answer_before_offer) {
        command("answer", NULL);
        pump(1);
        assert(rtc.call.answer_requested && !peer_opened);
    }
    receive("sdp", "1002", "sdp", local_sdp_text, NULL);
    pump(1);
    if (!answer_before_offer) {
        assert(rtc.call.offer && !strcmp(rtc.call.offer, local_sdp_text));
        assert(!peer_opened && !wire_count);
        command("answer", NULL);
        pump(1);
    }
    assert(rtc.call.phase == CALL_CONNECTING && !rtc.call.caller);
    assert(peer_config.role == ESP_PEER_ROLE_CONTROLLED && new_calls == 1);
    assert(sdp_calls == 1 && candidate_calls == 1 && wire_count == 1);
    assert(trace_index("peer:new", 0) < trace_index("peer:remote-sdp", 0));
    assert(trace_index("peer:remote-sdp", 0) < trace_index("peer:loop", 0));
    assert(!strcmp(remote_sdp, local_sdp_text) && !strcmp(remote_candidates[0], candidate_text));
    cJSON *answer = wire_message(0, "answer");
    assert(!strcmp(json_text(answer, "to"), "1002"));
    assert(!strcmp(json_text(answer, "sdp"), local_sdp_text));
    cJSON_Delete(answer);
    assert(!rtc.call.offer && !rtc.call.candidate_count && audio_calls == 0);
    connect_peer();
    finish();
    ++cases;
}

static void test_wrong_peer_and_late_messages(void)
{
    setup();
    start_outgoing();
    unsigned before = wire_count;
    receive("answer", "1003", "sdp", local_sdp_text, NULL);
    receive("ice", "1003", "candidate", candidate_text, NULL);
    receive("ring", "1003", NULL, NULL, NULL);
    pump(1);
    assert(sdp_calls == 0 && candidate_calls == 0 && wire_count == before && peer_opened);
    assert(rtc.call.phase == CALL_CALLING); // Foreign ring must not hang up an existing server call.
    receive("hangup", "1002", NULL, NULL, NULL);
    pump(1);
    assert(!peer_opened && rtc.call.phase == CALL_IDLE && wire_count == before);
    receive("answer", "1002", "sdp", local_sdp_text, NULL);
    receive("ice", "1002", "candidate", candidate_text, NULL);
    pump(1);
    assert(open_calls == 1 && sdp_calls == 0 && candidate_calls == 0);
    command("call", "1002"); // A later call must work after teardown.
    pump(1);
    assert(peer_opened && open_calls == 2 && rtc.call.phase == CALL_CALLING);
    finish();
    ++cases;
}

static void test_transport_generation(void)
{
    setup();
    command("call", "1002");
    digits_webrtc_transport(peer_session, false, "1001", true);
    pump(1);
    assert(open_calls == 0 && rtc.call.phase == CALL_IDLE && !wire_count);
    ++peer_session;
    digits_webrtc_transport(peer_session, true, "1001", true);
    pump(1);
    assert(wire_count == 1);
    clear_wire();
    start_outgoing();
    receive("answer", "1002", "sdp", local_sdp_text, NULL);
    digits_webrtc_transport(peer_session, false, "1001", true);
    pump(1);
    assert(close_calls == 1 && sdp_calls == 0 && rtc.call.phase == CALL_IDLE);
    assert(wires_of_type("hangup") == 0); // No sends through a dead signaling session.
    finish();
    ++cases;
}

static void test_disconnect_during_dtls(void)
{
    setup();
    start_outgoing();
    apply_answer();
    disconnect_during_loop = true;
    connect_next_loop = true;
    pump(1);
    assert(!peer_opened && rtc.call.phase == CALL_IDLE && close_calls == 1);
    assert(audio_calls == 0); // Stale CONNECTED callback cannot send media.
    finish();
    ++cases;
}

static void test_teardown_events(void)
{
    static const char *types[] = {"busy", "hangup", "error", "ice_restart"};
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        setup();
        start_outgoing();
        receive(types[i], !strcmp(types[i], "error") ? NULL : "1002",
                !strcmp(types[i], "ice_restart") ? "sdp" : NULL,
                local_sdp_text, NULL);
        pump(1);
        assert(!peer_opened && rtc.call.phase == CALL_IDLE && close_calls == 1);
        assert(wires_of_type("hangup") == ((!strcmp(types[i], "error") || !strcmp(types[i], "ice_restart")) ? 1U : 0U));
        command("call", "1002");
        pump(1);
        assert(peer_opened && open_calls == 2);
        finish();
        ++cases;
    }
}

static void test_timeouts(void)
{
    for (unsigned answered = 0; answered < 2; ++answered) {
        setup();
        start_outgoing();
        if (answered) apply_answer();
        int64_t deadline = rtc.call.deadline_us;
        assert(deadline == host_now + (answered ? 10000000 : 60000000));
        host_now = deadline - 1;
        pump(1);
        assert(peer_opened);
        host_now = deadline;
        pump(1);
        assert(!peer_opened && close_calls == 1);
        cJSON *hangup = wire_message(wire_count - 1, "hangup");
        if (answered) assert(!strcmp(json_text(hangup, "reason"), "connect_timeout"));
        else assert(json_text(hangup, "reason") == NULL);
        cJSON_Delete(hangup);
        finish();
        ++cases;
    }
}

static void test_failure_hangup_reasons(void)
{
    for (unsigned connected = 0; connected < 2; ++connected) {
        for (unsigned event_failure = 0; event_failure < 2; ++event_failure) {
            setup();
            start_outgoing();
            apply_answer();
            if (connected) connect_peer();
            if (event_failure) {
                // Cover cleanup while draining a late candidate, as well as
                // cleanup after the peer loop observes remote transport close.
                candidate_result = ESP_PEER_ERR_BAD_DATA;
                receive("ice", "1002", "candidate", candidate_text, NULL);
            } else {
                in_worker = true;
                assert(emit_peer_state(ESP_PEER_STATE_DISCONNECTED) == 0);
                in_worker = false;
            }
            pump(1);
            assert(!peer_opened && rtc.call.phase == CALL_IDLE && close_calls == 1);
            assert(wires_of_type("hangup") == 1);
            cJSON *hangup = wire_message(wire_count - 1, "hangup");
            if (connected) assert(json_text(hangup, "reason") == NULL);
            else assert(!strcmp(json_text(hangup, "reason"), "connect_timeout"));
            cJSON_Delete(hangup);
            // In the observed call, DTLS closure preceded signaling hangup.
            // That later message must neither close twice nor echo a hangup.
            unsigned sent = wire_count;
            receive("hangup", "1002", NULL, NULL, NULL);
            pump(1);
            assert(close_calls == 1 && wire_count == sent);
            finish();
            ++cases;
        }
    }
}

static void test_candidate_limits_and_ownership(void)
{
    setup();
    receive("ring", "1002", NULL, NULL, NULL);
    pump(1);
    receive("ice", "1002", "candidate", "a=candidate:1 1 udp 1 192.0.2.1 1 typ host", NULL);
    receive("ice", "1002", NULL, NULL, NULL); // omitted/empty end marker.
    pump(1);
    assert(!rtc.call.candidate_count && !rtc.call.failure);
    for (unsigned i = 0; i < MAX_PENDING_CANDIDATES; ++i) {
        receive("ice", "1002", "candidate", candidate_text, NULL);
        pump(1);
    }
    assert(rtc.call.candidate_count == MAX_PENDING_CANDIDATES && !peer_opened);
    receive("ice", "1002", "candidate", candidate_text, NULL);
    pump(1);
    assert(rtc.call.phase == CALL_IDLE && rtc.call.candidate_count == 0);
    assert(wires_of_type("hangup") == 1);
    finish();
    ++cases;
}

static void test_overflow_and_copy_failure(void)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        setup();
        start_outgoing();
        if (mode == 0) {
            for (unsigned i = 0; i <= MESSAGE_QUEUE_LENGTH; ++i)
                receive("ice", "1002", "candidate", candidate_text, NULL);
        } else if (mode == 1) {
            char too_large[DIGITS_WEBRTC_MAX_SDP + 2];
            memset(too_large, 'x', sizeof(too_large) - 1);
            too_large[sizeof(too_large) - 1] = '\0';
            receive("answer", "1002", "sdp", too_large, NULL);
        } else {
            allocation_calls = 0;
            allocation_fail_after = 1; // event allocation succeeds; owned payload fails.
            receive("answer", "1002", "sdp", local_sdp_text, NULL);
            allocation_fail_after = -1;
        }
        assert(transport_snapshot().overflow);
        pump(1);
        assert(!peer_opened && rtc.call.phase == CALL_IDLE && !rtc.queue->count);
        assert(!transport_snapshot().overflow && !rtc.call.candidate_count);
        command("call", "1002");
        pump(1);
        assert(peer_opened && open_calls == 2);
        finish();
        ++cases;
    }
}

static void test_library_and_transmit_failures(void)
{
    for (unsigned mode = 0; mode < 6; ++mode) {
        setup();
        if (mode == 0) open_result = ESP_PEER_ERR_NO_MEM;
        if (mode == 1) new_result = ESP_PEER_ERR_FAIL;
        if (mode == 2) transmit_result = ESP_ERR_NO_MEM;
        if (mode == 3) loop_result = ESP_PEER_ERR_FAIL;
        command("call", "1002");
        pump(1);
        if (mode == 4) {
            sdp_result = ESP_PEER_ERR_BAD_DATA;
            receive("answer", "1002", "sdp", local_sdp_text, NULL);
            pump(1);
        }
        if (mode == 5) {
            apply_answer();
            audio_result = ESP_PEER_ERR_FAIL;
            connect_next_loop = true;
            pump(1);
            queue_microphone_packet(0, host_now);
            pump(2);
        }
        assert(!peer_opened && rtc.call.phase == CALL_IDLE);
        assert(rtc.call.peer == NULL && !rtc.call.offer && !rtc.call.candidate_count);
        open_result = new_result = loop_result = sdp_result = audio_result = 0;
        transmit_result = ESP_OK;
        command("call", "1002");
        pump(1);
        assert(peer_opened && rtc.call.phase == CALL_CALLING);
        finish();
        ++cases;
    }
}

static void test_ice_servers_and_unpaired_controls(void)
{
    setup();
    cJSON *message = cJSON_Parse("{\"type\":\"ice-servers\",\"servers\":[{\"urls\":[\"stun:stun.example.test:19302\",\"turn:turn.example.test:3478?transport=udp\",\"turns:turn.example.test:5349\"],\"username\":\"private\",\"credential\":\"private\"}]}");
    assert(message && digits_webrtc_receive(message, peer_session));
    cJSON_Delete(message);
    pump(1);
    assert(rtc.ice_received && rtc.ice_count == 1);
    assert(!strcmp(rtc.ice_urls[0], "stun:stun.example.test:19302"));
    start_outgoing();
    assert(peer_config.server_num == 1);
    finish();
    ++cases;

    setup();
    assert(digits_webrtc_command(NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(digits_webrtc_command("invalid", NULL) == ESP_ERR_INVALID_ARG);
    assert(digits_webrtc_command("call", "not-a-number") == ESP_ERR_INVALID_ARG);
    command("call", "1001");
    pump(1);
    assert(!open_calls && !wire_count);
    digits_webrtc_transport(peer_session, true, "unpaired", false);
    pump(1);
    assert(digits_webrtc_command("call", "1002") == ESP_ERR_INVALID_STATE);
    assert(digits_webrtc_command("answer", NULL) == ESP_ERR_INVALID_STATE);
    command("status", NULL);
    pump(1);
    assert(!open_calls && !wire_count);
    finish();
    ++cases;
}

static void test_group_and_local_trickle(void)
{
    setup();
    receive("ring", "1002", NULL, NULL, "test-group");
    pump(1);
    assert(rtc.call.phase == CALL_IDLE && !peer_opened && !wire_count);
    start_outgoing();
    in_worker = true;
    esp_peer_msg_t candidate = {.type = ESP_PEER_MSG_TYPE_CANDIDATE,
        .data = (uint8_t *)candidate_text, .size = (int)strlen(candidate_text)};
    assert(on_peer_message(&candidate, NULL) == 0);
    in_worker = false;
    cJSON *ice = wire_message(wire_count - 1, "ice");
    assert(!strcmp(json_text(ice, "candidate"), candidate_text));
    cJSON_Delete(ice);
    unsigned before = wire_count;
    receive("answer", "1002", "sdp", local_sdp_text, "test-group");
    pump(1);
    assert(rtc.call.phase == CALL_CALLING && peer_opened && sdp_calls == 0);
    assert(wire_count == before); // Unsupported group signaling must not cancel this call.
    char oversized_group_sdp[DIGITS_WEBRTC_MAX_SDP + 2];
    memset(oversized_group_sdp, 'x', sizeof(oversized_group_sdp) - 1);
    oversized_group_sdp[sizeof(oversized_group_sdp) - 1] = '\0';
    // Unsupported groups are filtered before allocation/copy/queue limits.
    // Their oversized or repeated messages cannot abort an ordinary call.
    unsigned queued = rtc.queue->count;
    for (unsigned i = 0; i < MESSAGE_QUEUE_LENGTH + 1; ++i)
        receive("sdp", "1002", "sdp", oversized_group_sdp, "test-group");
    assert(rtc.queue->count == queued && !transport_snapshot().overflow);
    pump(1);
    assert(rtc.call.phase == CALL_CALLING && peer_opened && sdp_calls == 0 && wire_count == before);
    apply_answer();
    connect_peer();
    finish();
    ++cases;
}

static void test_borrowed_peer_configuration(void)
{
    setup();
    cJSON *servers = cJSON_Parse("{\"type\":\"ice-servers\",\"servers\":[{\"urls\":[\"stun:first.example.test:19302\"]}]}");
    assert(servers && digits_webrtc_receive(servers, peer_session));
    cJSON_Delete(servers);
    pump(1);
    start_outgoing();
    assert(peer_config.server_num == 1 && peer_config.server_lists);
    const char *peer_url = peer_config.server_lists[0].stun_url;
    assert(!strcmp(peer_url, "stun:first.example.test:19302"));
    const esp_peer_default_cfg_t *borrowed_defaults = peer_config.extra_cfg;
    apply_answer();
    const char *peer_sdp = borrowed_remote_sdp;
    assert(peer_sdp && !strcmp(peer_sdp, local_sdp_text));
    servers = cJSON_Parse("{\"type\":\"ice-servers\",\"servers\":[{\"urls\":[\"stun:second.example.test:19302\"]}]}");
    assert(servers && digits_webrtc_receive(servers, peer_session));
    cJSON_Delete(servers);
    pump(1);
    assert(!strcmp(rtc.ice_urls[0], "stun:second.example.test:19302"));
    // Live peer input survives freeing/replacing the cached next-call servers.
    assert(!strcmp(peer_url, "stun:first.example.test:19302"));
    assert(borrowed_defaults->rtp_cfg.send_pool_size == 8192);
    assert(!strcmp(peer_sdp, local_sdp_text));
    finish();
    ++cases;
}

static void test_local_callback_size_and_text(void)
{
    for (unsigned mode = 0; mode < 5; ++mode) {
        setup();
        suppress_local_sdp = true;
        command("call", "1002");
        pump(1);
        assert(peer_opened && !rtc.call.local_sdp && wire_count == 1);
        const size_t text_size = strlen(local_sdp_text);
        unsigned char *text = malloc(text_size + 1);
        assert(text);
        memcpy(text, local_sdp_text, text_size);
        text[text_size] = '\0';
        esp_peer_msg_t message = {.type = ESP_PEER_MSG_TYPE_SDP, .data = text,
                                 .size = (int)text_size};
        if (mode == 1) ++message.size; // Size includes the terminator.
        if (mode == 2) message.size = 0;
        if (mode == 3) text[10] = '\0'; // Embedded NUL is not a complete SDP.
        if (mode == 4) message.size = DIGITS_WEBRTC_MAX_SDP + 2;
        in_worker = true;
        int result = on_peer_message(&message, NULL);
        in_worker = false;
        memset(text, 0xa5, text_size + 1);
        free(text);
        if (mode < 2) {
            assert(result == ESP_PEER_ERR_NONE && rtc.call.local_sdp && wire_count == 2);
            cJSON *offer = wire_message(1, "sdp");
            assert(!strcmp(json_text(offer, "sdp"), local_sdp_text));
            cJSON_Delete(offer);
            size_t candidate_size = strlen(candidate_text);
            unsigned char *raw_candidate = malloc(candidate_size); // No NUL allocation.
            assert(raw_candidate);
            memcpy(raw_candidate, candidate_text, candidate_size);
            esp_peer_msg_t candidate = {.type = ESP_PEER_MSG_TYPE_CANDIDATE,
                .data = raw_candidate, .size = (int)candidate_size};
            in_worker = true;
            assert(on_peer_message(&candidate, NULL) == ESP_PEER_ERR_NONE);
            in_worker = false;
            memset(raw_candidate, 0xa5, candidate_size);
            free(raw_candidate);
            cJSON *ice = wire_message(wire_count - 1, "ice");
            assert(!strcmp(json_text(ice, "candidate"), candidate_text));
            cJSON_Delete(ice);
        } else {
            assert(result == ESP_PEER_ERR_BAD_DATA && wire_count == 1 && rtc.call.failure);
            pump(1);
            assert(!peer_opened && rtc.call.phase == CALL_IDLE);
        }
        finish();
        ++cases;
    }
}

static void test_codec_callbacks_and_duplicate_connected(void)
{
    for (unsigned mode = 0; mode < 5; ++mode) {
        setup();
        start_outgoing();
        esp_peer_audio_stream_info_t info = {
            .codec = ESP_PEER_AUDIO_CODEC_OPUS, .sample_rate = 48000,
            .channel = (uint8_t)(mode == 1 ? 2 : 1),
        };
        if (mode == 2) info.codec = ESP_PEER_AUDIO_CODEC_G711U;
        if (mode == 3) info.sample_rate = 16000;
        if (mode == 4) info.channel = 3;
        in_worker = true;
        int result = on_audio_info(&info, NULL);
        unsigned char packet[] = {0xf8, 0xff, 0xfe};
        esp_peer_audio_frame_t frame = {.data = packet, .size = 3};
        assert(on_audio_data(&frame, NULL) == 0);
        in_worker = false;
        assert(!rtc.call.rx_packets); // Receive counts require actual CONNECTED.
        if (mode < 2) {
            assert(result == ESP_PEER_ERR_NONE && rtc.call.audio_info);
            apply_answer();
            connect_peer();
            int64_t connected = rtc.call.connected_at_us;
            int64_t next = rtc.call.next_audio_us;
            host_now += 5000;
            in_worker = true;
            emit_peer_state(ESP_PEER_STATE_CONNECTED);
            in_worker = false;
            assert(rtc.call.connected_at_us == connected && rtc.call.next_audio_us == next);
            pump(1);
            assert(audio_calls == 1);
        } else {
            assert(result == ESP_PEER_ERR_NOT_SUPPORT && rtc.call.failure);
            pump(1);
            assert(!peer_opened && rtc.call.phase == CALL_IDLE);
        }
        finish();
        ++cases;
    }
}

static void test_stale_callbacks_after_close(void)
{
    setup();
    start_outgoing();
    command("hangup", NULL);
    pump(1);
    assert(!peer_opened && rtc.call.phase == CALL_IDLE);
    unsigned before = wire_count;
    in_worker = true;
    assert(on_peer_state(ESP_PEER_STATE_CONNECTED, NULL) == 0);
    esp_peer_msg_t message = {.type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)local_sdp_text, .size = (int)strlen(local_sdp_text)};
    assert(on_peer_message(&message, NULL) == 0);
    esp_peer_audio_stream_info_t info = {.codec = ESP_PEER_AUDIO_CODEC_G711A};
    assert(on_audio_info(&info, NULL) == 0);
    in_worker = false;
    pump(1);
    assert(rtc.call.phase == CALL_IDLE && !rtc.call.failure && wire_count == before && !audio_calls);
    finish();
    ++cases;
}

static void test_disconnect_or_transmit_failure_in_local_sdp(void)
{
    for (unsigned mode = 0; mode < 2; ++mode) {
        setup();
        if (mode == 0) disconnect_on_sdp_transmit = true;
        else reject_transmit_type = "sdp";
        command("call", "1002");
        pump(1);
        assert(!peer_opened && rtc.call.phase == CALL_IDLE && close_calls == 1 && !audio_calls);
        if (mode == 0) assert(!wires_of_type("hangup"));
        else assert(wires_of_type("hangup") == 1);
        reject_transmit_type = NULL;
        finish();
        ++cases;
    }
}

static void test_audio_startup_and_published_frames(void)
{
    setup();
    start_outgoing();
    apply_answer();
    media_initial_state = DIGITS_CALL_AUDIO_STARTING;
    connect_next_loop = true;
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTED && media_start_calls == 1 && audio_calls == 0);
    queue_microphone_packet(1000, host_now);
    pump(5);
    assert(audio_calls == 0 && media_pop_calls == 0); // No sends during hardware/codec startup.
    media.stats.state = DIGITS_CALL_AUDIO_READY;
    pump(1);
    assert(audio_calls == 1 && audio_pts[0] == 1000); // Preserve producer time, including nonzero origin.
    host_now += 100000;
    queue_microphone_packet(1020, host_now - 61000);
    pump(1);
    assert(audio_calls == 1 && media.stats.tx_pop_drops == 1); // Engine withheld stale media.
    queue_microphone_packet(1100, host_now);
    pump(8);
    assert(audio_calls == 2 && audio_pts[1] == 1100 && rtc.call.skipped_audio == 4);
    assert(media_record_calls == 2); // Empty/stale publication never becomes synthetic RTP.
    finish();
    ++cases;
}

static void test_audio_start_and_worker_failures(void)
{
    for (unsigned failure = 0; failure < 2; ++failure) {
        setup();
        start_outgoing();
        apply_answer();
        if (failure == 0) media_start_result = ESP_ERR_NO_MEM;
        else media_initial_state = DIGITS_CALL_AUDIO_STARTING;
        connect_next_loop = true;
        pump(1);
        assert(media_start_calls == 1 && audio_calls == 0);
        if (failure == 1) {
            assert(media.live && rtc.call.phase == CALL_CONNECTED);
            media.stats.state = DIGITS_CALL_AUDIO_FAILED;
            media.stats.error = ESP_FAIL;
        }
        pump(3);
        assert(rtc.call.phase == CALL_IDLE && !peer_opened && !media.live);
        assert(close_calls == 1 && wires_of_type("hangup") == 1);
        cJSON *hangup = wire_message(wire_count - 1, "hangup");
        assert(json_text(hangup, "reason") == NULL); // Physical media failure follows a connected transport.
        cJSON_Delete(hangup);
        media_start_result = ESP_OK;
        media_initial_state = DIGITS_CALL_AUDIO_READY;
        command("call", "1002");
        pump(1);
        assert(open_calls == 2 && peer_opened && rtc.call.phase == CALL_CALLING);
        finish();
        ++cases;
    }
}

static void test_audio_modes_and_offline_control(void)
{
    static const char *names[] = {"tx", "rx", "full"};
    static const digits_call_audio_mode_t modes[] = {
        DIGITS_CALL_AUDIO_TX, DIGITS_CALL_AUDIO_RX, DIGITS_CALL_AUDIO_FULL,
    };
    for (unsigned i = 0; i < 3; ++i) {
        setup();
        assert(rtc.audio_mode == DIGITS_CALL_AUDIO_FULL);
        assert(digits_webrtc_command("audio", NULL) == ESP_ERR_INVALID_ARG);
        assert(digits_webrtc_command("audio", "invalid") == ESP_ERR_INVALID_ARG);
        command("audio", names[i]);
        assert(rtc.audio_mode == DIGITS_CALL_AUDIO_FULL); // Command dispatch is queued.
        pump(1);
        assert(rtc.audio_mode == modes[i] && media_start_calls == 0);
        start_outgoing();
        command("audio", names[(i + 1) % 3]);
        pump(1);
        assert(rtc.audio_mode == modes[i]); // A live call's codec/ownership mode is immutable.
        apply_answer();
        connect_peer();
        assert(media.mode == modes[i]);
        uint8_t encoded[] = {0xf8, 0xff, 0xfe};
        esp_peer_audio_frame_t packet = {.pts = 9180, .data = encoded, .size = 3};
        in_worker = true;
        assert(on_audio_data(&packet, NULL) == ESP_PEER_ERR_NONE);
        in_worker = false;
        assert(media_receive_calls == ((modes[i] & DIGITS_CALL_AUDIO_RX) ? 1U : 0U));
        assert(audio_calls == ((modes[i] & DIGITS_CALL_AUDIO_TX) ? 1U : 0U));
        command("audio", names[(i + 1) % 3]);
        pump(1);
        assert(rtc.audio_mode == modes[i] && media.mode == modes[i]);
        digits_webrtc_transport(peer_session, false, "1001", true);
        pump(2);
        assert(!peer_opened && !media.live && !rtc.stopping_audio);
        command("audio", names[(i + 1) % 3]);
        pump(1);
        assert(rtc.audio_mode == modes[(i + 1) % 3]); // Development mode selection works offline.
        finish();
        ++cases;
    }
}

static void test_async_audio_stop_and_repeated_calls(void)
{
    setup();
    start_outgoing();
    apply_answer();
    connect_peer();
    media_stop_completes = false;
    command("hangup", NULL);
    pump(1);
    assert(!peer_opened && rtc.call.phase == CALL_IDLE && rtc.stopping_audio == &media);
    assert(media.live && media.stats.state == DIGITS_CALL_AUDIO_STOPPING);
    assert(media_stop_calls == 1 && media_destroy_calls == 0);
    assert(trace_index("media:stop", 0) < trace_index("peer:close", 0));
    command("call", "1002");
    command("audio", "tx");
    pump(2);
    assert(open_calls == 1 && rtc.call.phase == CALL_IDLE && !peer_opened);
    assert(rtc.audio_mode == DIGITS_CALL_AUDIO_FULL && media_destroy_calls == 0);
    receive("ring", "1002", NULL, NULL, NULL);
    pump(1);
    assert(open_calls == 1 && rtc.call.phase == CALL_IDLE && media_start_calls == 1);
    uint8_t encoded[] = {0xf8, 0xff, 0xfe};
    esp_peer_audio_frame_t packet = {.pts = 20, .data = encoded, .size = 3};
    in_worker = true;
    assert(on_audio_data(&packet, NULL) == ESP_PEER_ERR_NONE);
    in_worker = false;
    assert(media_receive_calls == 0); // No stale callback may write into the retiring engine.
    media.stats.state = DIGITS_CALL_AUDIO_STOPPED;
    pump(1);
    assert(!media.live && !rtc.stopping_audio && media_destroy_calls == 1);
    assert(trace_index("peer:close", 0) < trace_index("media:destroy", 0));
    command("call", "1002");
    pump(1);
    assert(open_calls == 2 && peer_opened && rtc.call.phase == CALL_CALLING);
    assert(trace_index("media:destroy", 0) < trace_index("peer:open", trace_index("peer:open", 0) + 1));
    apply_answer();
    connect_next_loop = true;
    pump(1);
    assert(media_start_calls == 2 && media.live);
    queue_microphone_packet(0, host_now);
    pump(1);
    assert(audio_calls == 2 && audio_pts[1] == 0); // New call owns a new timing origin.
    media_stop_completes = true;
    finish();
    assert(media_stop_calls == 2 && media_destroy_calls == 2);
    ++cases;
}

static void test_peer_loop_timing_diagnostics(void)
{
    setup();
    start_outgoing();
    apply_answer();
    main_loop_delay_us = 50000;
    connect_next_loop = true;
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTED && rtc.call.max_peer_loop_us == 0);
    assert(rtc.call.slow_peer_loops == 0); // DTLS/transition duration is excluded from connected scheduling.
    main_loop_delay_us = 6000;
    pump(1);
    assert(rtc.call.max_peer_loop_us == 6000 && rtc.call.slow_peer_loops == 0);
    main_loop_delay_us = 20001;
    pump(1);
    assert(rtc.call.max_peer_loop_us == 20001 && rtc.call.slow_peer_loops == 1);
    assert(largest_connected_wait == 1 && last_worker_wait == 1);
    main_loop_delay_us = 1000;
    pump(3);
    assert(rtc.call.max_peer_loop_us == 20001 && rtc.call.slow_peer_loops == 1);
    assert(audio_calls == 0); // Delays do not turn an empty encoder queue into invented media.
    main_loop_delay_us = 0;
    finish();
    ++cases;
}

static void test_ringtone_answer_ownership(void)
{
    setup();
    receive("ring", "1002", NULL, NULL, NULL);
    pump(1);
    assert(ring_starts == 0 && settings_busy); // Waits for the caller's ringtone.
    receive("sdp", "1002", "sdp", local_sdp_text, NULL);
    pump(1);
    assert(ring_starts == 1 && last_ringtone == 0 && !ring_stopped); // No choice: standard.
    ring_stop_completes = false;
    command("answer", NULL);
    pump(2);
    assert(rtc.call.phase == CALL_RINGING && rtc.call.answer_requested);
    assert(!peer_opened && !media_start_calls && ring_stops == 1);
    // The worker stays responsive while the speaker owner's bounded write ends.
    command("status", NULL);
    pump(1);
    assert(!peer_opened);
    ring_stopped = true;
    ring_stop_completes = true;
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTING && peer_opened);
    connect_peer();
    assert(settings_busy && ring_starts == 1);
    // Publishing settings while established never changes media or call state.
    settings.silent_mode = true;
    settings.voice_style = DIGITS_VOICE_MODERN;
    settings.auto_update = settings.voicemail_enabled = true;
    command("status", NULL);
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTED && media_start_calls == 1 && !media_stop_calls);
    settings.silent_mode = false;
    pump(1);
    assert(ring_starts == 1 && media.live);
    media_stop_completes = false;
    command("hangup", NULL);
    pump(1);
    assert(rtc.call.phase == CALL_IDLE && settings_busy && rtc.stopping_audio);
    media.stats.state = DIGITS_CALL_AUDIO_STOPPED;
    pump(1);
    assert(!settings_busy && !rtc.stopping_audio);
    finish();
    ++cases;
}

// Rings with voicemail on (5 s) until the machine answers and connects.
static void answer_by_machine(void)
{
    settings.voicemail_enabled = true;
    settings.voicemail_ring_timeout_seconds = 5;
    vm_ready = true;
    receive("ring", "1002", NULL, NULL, NULL);
    pump(1);
    assert(rtc.call.phase == CALL_RINGING && rtc.call.voicemail_at_us == host_now + 5000000);
    receive("sdp", "1002", "sdp", local_sdp_text, NULL);
    pump(1);
    assert(ring_starts == 1);
    host_now += 4999999;
    pump(1);
    assert(rtc.call.phase == CALL_RINGING && !rtc.call.answer_requested && !peer_opened);
    host_now += 1;
    pump(1);
    assert(rtc.call.voicemail && rtc.call.phase == CALL_CONNECTING && peer_opened && ring_stops >= 1);
    assert(wires_of_type("answer") == 1 && !vm_begins); // An ordinary answer; no recording yet.
    connect_next_loop = true;
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTED && vm_begins == 1 && !strcmp(vm_caller, "1002"));
    assert(media.live && media.hooks == &vm_hooks && media.mode == DIGITS_CALL_AUDIO_FULL);
}

static void test_answering_machine(void)
{
    // Picks up after the server's timeout; lifting the handset takes over.
    setup();
    answer_by_machine();
    queue_microphone_packet(0, host_now); // The greeting still goes out as call audio.
    pump(1);
    assert(audio_calls == 1);
    command("answer", NULL);
    pump(1);
    assert(!rtc.call.voicemail && !media.hooks && media.attends == 1 && vm_ends == 1);
    assert(rtc.call.phase == CALL_CONNECTED && media_start_calls == 1 && wires_of_type("hangup") == 0);
    command("answer", NULL); // A second lift report changes nothing.
    pump(1);
    assert(media.attends == 1 && vm_ends == 1);
    finish();
    assert(vm_ends == 1);
    ++cases;

    // The caller hangs up during the message: it is kept.
    setup();
    answer_by_machine();
    receive("hangup", "1002", NULL, NULL, NULL);
    pump(1);
    assert(rtc.call.phase == CALL_IDLE && vm_ends == 1 && !rtc.call.voicemail);
    finish();
    ++cases;

    // The message reached its limit: the machine hangs up.
    setup();
    answer_by_machine();
    vm_full = true;
    pump(1);
    assert(rtc.call.phase == CALL_IDLE && vm_ends == 1 && wires_of_type("hangup") == 1);
    assert(rtc.last_end == DIGITS_CALL_END_FAILED);
    finish();
    ++cases;

    // No recording possible at connect: hang up rather than open the room microphone.
    setup();
    vm_begin_fails = true;
    settings.voicemail_enabled = true;
    settings.voicemail_ring_timeout_seconds = 5;
    vm_ready = true;
    receive("ring", "1002", NULL, NULL, NULL);
    receive("sdp", "1002", "sdp", local_sdp_text, NULL);
    pump(1);
    host_now += 5000000;
    pump(1);
    connect_next_loop = true;
    pump(2); // Connected, refused; the next iteration ends the call.
    assert(vm_begins == 1 && !media_start_calls && rtc.call.phase == CALL_IDLE);
    assert(wires_of_type("hangup") == 1);
    finish();
    ++cases;
}

static void test_answering_machine_not_armed(void)
{
    // Answered in person before the timeout: the machine never starts.
    setup();
    settings.voicemail_enabled = true;
    settings.voicemail_ring_timeout_seconds = 5;
    vm_ready = true;
    receive("ring", "1002", NULL, NULL, NULL);
    receive("sdp", "1002", "sdp", local_sdp_text, NULL);
    pump(1);
    host_now += 4000000;
    command("answer", NULL);
    pump(1);
    host_now += 2000000;
    connect_next_loop = true;
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTED && !rtc.call.voicemail && !vm_begins && !media.hooks);
    finish();
    ++cases;

    // Voicemail off, or no card: keeps ringing until the caller gives up.
    for (unsigned variant = 0; variant < 3; ++variant) {
        setup();
        settings.voicemail_enabled = variant != 0;
        settings.voicemail_ring_timeout_seconds = 5;
        vm_ready = variant != 1;
        if (variant == 2) {
            command("audio", "rx"); // Lab mode cannot send a greeting.
            pump(1);
        }
        receive("ring", "1002", NULL, NULL, NULL);
        receive("sdp", "1002", "sdp", local_sdp_text, NULL);
        pump(1);
        assert(!rtc.call.voicemail_at_us);
        host_now += 30000000;
        pump(1);
        assert(rtc.call.phase == CALL_RINGING && !peer_opened && !vm_begins);
        finish();
        ++cases;
    }
}

static int reported_count(unsigned index)
{
    cJSON *message = wire_message(index, "voicemail_state");
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(message, "voicemail_unheard_count");
    assert(cJSON_IsNumber(count) && !cJSON_GetObjectItemCaseSensitive(message, "to"));
    int value = count->valueint;
    cJSON_Delete(message);
    return value;
}

static void test_voicemail_count_reports(void)
{
    setup();
    pump(2);
    assert(!wire_count); // No store: nothing to report.
    vm_unheard = 2;
    pump(1);
    assert(wire_count == 1 && reported_count(0) == 2);
    pump(3);
    assert(wire_count == 1); // Unchanged: not repeated.
    vm_unheard = 0;
    pump(1);
    assert(wire_count == 2 && reported_count(1) == 0); // An explicit zero clears the badge.
    // A new session repeats it: the server dropped the count on disconnect.
    digits_webrtc_transport(peer_session, false, "1001", true);
    pump(1);
    ++peer_session;
    digits_webrtc_transport(peer_session, true, "1001", true);
    clear_wire();
    pump(1);
    assert(wires_of_type("voicemail_state") == 1);
    // A failed send is retried a second later.
    clear_wire();
    vm_unheard = 4;
    transmit_result = ESP_ERR_NO_MEM;
    pump(1);
    transmit_result = ESP_OK;
    pump(1);
    assert(!wire_count);
    host_now += 1000000;
    pump(1);
    assert(wire_count == 1 && reported_count(0) == 4);
    finish();
    ++cases;
}

static void test_tones_release_before_call_audio(void)
{
    setup();
    start_outgoing();
    apply_answer();
    // Ringback still owns the speaker when media connects.
    tones_stopped = false;
    tones_stop_completes = false;
    connect_next_loop = true;
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTED && tone_stop_requests == 1);
    assert(media_start_calls == 0 && !media.live);
    pump(3); // The worker keeps looping without blocking on the speaker.
    assert(media_start_calls == 0 && rtc.call.phase == CALL_CONNECTED);
    tones_stopped = true;
    pump(1);
    assert(media_start_calls == 1 && media.live);
    // Once media runs, tone state no longer gates sending.
    tones_stopped = false;
    queue_microphone_packet(0, host_now);
    pump(1);
    assert(audio_calls == 1);
    tones_stopped = true;
    finish();
    ++cases;
}

static void end_reason_case(const char *how, digits_call_end_t want, bool connect)
{
    setup();
    digits_webrtc_call_state_t before, after;
    digits_webrtc_call_state(&before);
    start_outgoing();
    if (connect) {
        apply_answer();
        connect_peer();
    }
    if (!strcmp(how, "local")) command("hangup", NULL);
    else if (!strcmp(how, "timeout")) host_now += 61LL * 1000000;
    else receive(how, "1002", NULL, NULL, NULL);
    pump(2);
    digits_webrtc_call_state(&after);
    assert(rtc.call.phase == CALL_IDLE && after.phase == DIGITS_CALL_IDLE);
    assert(after.ended_calls == before.ended_calls + 1);
    assert(after.last_end == want && after.last_connected == connect);
    finish();
    ++cases;
}

static void test_call_end_reasons(void)
{
    end_reason_case("busy", DIGITS_CALL_END_BUSY, false);
    end_reason_case("hangup", DIGITS_CALL_END_REMOTE_HANGUP, false);
    end_reason_case("hangup", DIGITS_CALL_END_REMOTE_HANGUP, true);
    end_reason_case("error", DIGITS_CALL_END_UNREACHABLE, false);
    end_reason_case("local", DIGITS_CALL_END_LOCAL, true);
    end_reason_case("timeout", DIGITS_CALL_END_FAILED, false);
}

static char resolver_caller[32];
static int resolver_offered = -2;
static unsigned test_resolver(const char *caller, int offered)
{
    assert(in_worker && caller);
    snprintf(resolver_caller, sizeof(resolver_caller), "%s", caller);
    resolver_offered = offered;
    return 3;
}

static void offer_with(char *out, size_t size, const char *attribute_line)
{
    const char *media = strstr(local_sdp_text, "m=audio");
    assert(media);
    snprintf(out, size, "%.*s%s%s", (int)(media - local_sdp_text), local_sdp_text, attribute_line, media);
}

static void test_caller_ringtone(void)
{
    char offer[1024];
    // Incoming choice: used for the ringer, removed before the peer library.
    setup();
    receive("ring", "1002", NULL, NULL, NULL);
    pump(1);
    assert(ring_starts == 0);
    offer_with(offer, sizeof(offer), "a=x-digits-ringtone:3\r\n");
    receive("sdp", "1002", "sdp", offer, NULL);
    pump(1);
    assert(ring_starts == 1 && last_ringtone == 3);
    assert(!strstr(rtc.call.offer, "x-digits-ringtone") && !strcmp(rtc.call.offer, local_sdp_text));
    command("answer", NULL);
    pump(2);
    assert(sdp_calls == 1 && !strcmp(remote_sdp, local_sdp_text));
    finish();
    ++cases;

    // Unknown or malformed values fall back to the standard ring.
    const char *bad[] = {"a=x-digits-ringtone:99\r\n", "a=x-digits-ringtone:\r\n",
                         "a=x-digits-ringtone:2x\r\n"};
    for (unsigned i = 0; i < 3; ++i) {
        setup();
        receive("ring", "1002", NULL, NULL, NULL);
        offer_with(offer, sizeof(offer), bad[i]);
        receive("sdp", "1002", "sdp", offer, NULL);
        pump(2);
        assert(ring_starts == 1 && last_ringtone == 0);
        assert(!strstr(rtc.call.offer, "x-digits-ringtone"));
        finish();
        ++cases;
    }

    // Answering before the offer and before the wait never starts the ringer.
    setup();
    receive("ring", "1002", NULL, NULL, NULL);
    command("answer", NULL);
    pump(1);
    host_now += 600000;
    pump(1);
    assert(ring_starts == 0);
    offer_with(offer, sizeof(offer), "a=x-digits-ringtone:1\r\n");
    receive("sdp", "1002", "sdp", offer, NULL);
    pump(2);
    assert(ring_starts == 0 && peer_opened);
    finish();
    ++cases;

    // Outgoing choice travels as a session-level attribute before m=.
    setup();
    assert(digits_webrtc_call("1002", 4) == ESP_OK);
    assert(digits_webrtc_call("1002", 99) == ESP_ERR_INVALID_ARG);
    pump(1);
    assert(rtc.call.phase == CALL_CALLING && wire_count == 2);
    cJSON *sent = wire_message(1, "sdp");
    const char *text = json_text(sent, "sdp");
    const char *attribute = strstr(text, "a=x-digits-ringtone:4\r\n");
    assert(attribute && attribute < strstr(text, "m=audio"));
    offer_with(offer, sizeof(offer), "a=x-digits-ringtone:4\r\n");
    assert(!strcmp(text, offer));
    cJSON_Delete(sent);
    finish();
    ++cases;

    // The phone layer's resolver decides the final ringtone per caller.
    setup();
    digits_webrtc_set_ringtone_resolver(test_resolver);
    receive("ring", "1002", NULL, NULL, NULL);
    offer_with(offer, sizeof(offer), "a=x-digits-ringtone:1\r\n");
    receive("sdp", "1002", "sdp", offer, NULL);
    pump(2);
    assert(ring_starts == 1 && last_ringtone == 3);
    assert(!strcmp(resolver_caller, "1002") && resolver_offered == 1);
    finish();
    ++cases;
    setup(); // No offer: the resolver is told -1.
    receive("ring", "1002", NULL, NULL, NULL);
    pump(1);
    host_now += 500000;
    pump(1);
    assert(ring_starts == 1 && resolver_offered == -1 && last_ringtone == 3);
    digits_webrtc_set_ringtone_resolver(NULL);
    finish();
    ++cases;

    // A plain console call adds nothing.
    setup();
    start_outgoing(); // Asserts the offer equals the library's SDP exactly.
    finish();
    ++cases;
}

static void test_ringtone_terminal_cleanup(void)
{
    for (unsigned terminal = 0; terminal < 3; ++terminal) {
        setup();
        receive("ring", "1002", NULL, NULL, NULL);
        pump(1);
        assert(ring_starts == 0);
        host_now += 500000; // No offer: the standard ring starts after the wait.
        pump(1);
        assert(ring_starts == 1 && last_ringtone == 0);
        ring_stop_completes = false;
        if (terminal == 0) command("hangup", NULL);
        else if (terminal == 1) host_now = rtc.call.deadline_us + 1;
        else digits_webrtc_transport(peer_session, false, "1001", true);
        pump(1);
        assert(rtc.call.phase == CALL_IDLE && !ring_stopped && settings_busy);
        if (terminal == 2) {
            ++peer_session;
            digits_webrtc_transport(peer_session, true, "1001", true);
            pump(1);
        }
        command("call", "1002");
        receive("ring", "1002", NULL, NULL, NULL);
        pump(1);
        assert(rtc.call.phase == CALL_IDLE && !peer_opened && ring_starts == 1);
        ring_stopped = true;
        ring_stop_completes = true;
        pump(1);
        assert(!settings_busy);
        receive("ring", "1002", NULL, NULL, NULL);
        pump(1);
        host_now += 500000;
        pump(1);
        assert(rtc.call.phase == CALL_RINGING && ring_starts == 2 && last_ring_id == 2);
        finish();
        ++cases;
    }
}

int main(void)
{
    test_initialization_failures();
    test_outgoing_and_media();
    test_incoming(false);
    test_incoming(true);
    test_wrong_peer_and_late_messages();
    test_transport_generation();
    test_disconnect_during_dtls();
    test_teardown_events();
    test_timeouts();
    test_failure_hangup_reasons();
    test_candidate_limits_and_ownership();
    test_overflow_and_copy_failure();
    test_library_and_transmit_failures();
    test_ice_servers_and_unpaired_controls();
    test_group_and_local_trickle();
    test_borrowed_peer_configuration();
    test_local_callback_size_and_text();
    test_codec_callbacks_and_duplicate_connected();
    test_stale_callbacks_after_close();
    test_disconnect_or_transmit_failure_in_local_sdp();
    test_audio_startup_and_published_frames();
    test_audio_start_and_worker_failures();
    test_audio_modes_and_offline_control();
    test_async_audio_stop_and_repeated_calls();
    test_peer_loop_timing_diagnostics();
    test_ringtone_answer_ownership();
    test_ringtone_terminal_cleanup();
    test_caller_ringtone();
    test_tones_release_before_call_audio();
    test_call_end_reasons();
    test_answering_machine();
    test_answering_machine_not_armed();
    test_voicemail_count_reports();
    printf("WebRTC adapter: %u real-worker lifecycle cases passed (mock peer; physical interoperability unverified)\n", cases);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="digits-webrtc-host-") as directory:
    temporary = Path(directory)
    for name, content in headers.items():
        target = temporary / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
    harness = temporary / "test.c"
    include = f'\n#include "{args.source.resolve()}"\n'
    harness.write_text(prefix + include + runtime + tests)
    binary = temporary / "webrtc-test"
    command = [
        "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
        "-I", str(temporary), "-I", str(repo / "esp32/main"),
        "-I", str(args.peer_include), "-I", str(cjson_dir),
        str(harness), str(cjson_dir / "cJSON.c"), str(repo / "esp32/main/ringtones.c"),
        "-lm", "-o", str(binary),
    ]
    if args.sanitize:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
