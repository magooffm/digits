"""Exercise the real Digits peer worker with finite host peer/RTOS mocks.

This verifies adapter ordering, ownership and cleanup. It does not simulate
ICE, DTLS, SRTP, packet loss, physical audio, or ESP32 scheduling/memory use.
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
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms) / 10)
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
    assert(priority == 3 && core == 1); // Ring Test/recording workers retain priority 4.
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
    assert(in_worker && clear && wait > 0);
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
    static const unsigned char expected[] = {0xf8, 0xff, 0xfe};
    assert(in_worker && peer == (void *)0x1234 && peer_opened);
    assert(rtc.call.phase == CALL_CONNECTED && mock_remote_sdp);
    assert(frame && frame->size == (int)sizeof(expected));
    assert(!memcmp(frame->data, expected, sizeof(expected)));
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
    assert(!live_queues);
    clear_wire();
    memset(&rtc, 0, sizeof(rtc));
    memset(&latest_transport, 0, sizeof(latest_transport));
    memset(&peer_config, 0, sizeof(peer_config));
    open_calls = new_calls = close_calls = loop_calls = candidate_calls = sdp_calls = 0;
    audio_calls = query_calls = logs = 0;
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
}

static void apply_answer(void)
{
    receive("answer", "1002", "sdp", local_sdp_text, NULL);
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTING && rtc.call.remote_sdp);
    assert(!strcmp(remote_sdp, local_sdp_text));
    assert(audio_calls == 0); // An SDP answer is not ICE/DTLS/SRTP success.
}

static void connect_peer(void)
{
    connect_next_loop = true;
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTED && rtc.call.deadline_us == 0);
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
    pump(1);
    assert(audio_calls == 2 && audio_pts[1] == 20);
    host_now += 100000;
    pump(1);
    assert(audio_calls == 3 && audio_pts[2] == 120 && rtc.call.skipped_audio == 4);
    pump(5);
    assert(audio_calls == 3); // Never catch up by flooding late RTP frames.
    audio_result = ESP_PEER_ERR_WOULD_BLOCK;
    host_now += 20000;
    pump(1);
    assert(rtc.call.phase == CALL_CONNECTED && rtc.call.blocked_audio == 1);
    assert(rtc.call.tx_packets == 3);
    unsigned before_rx = rtc.call.rx_packets;
    unsigned char payload[] = {0xf8, 0xff, 0xfe};
    esp_peer_audio_frame_t frame = {.data = payload, .size = 3, .pts = 20};
    in_worker = true;
    assert(on_audio_data(&frame, NULL) == 0);
    in_worker = false;
    memset(payload, 0, sizeof(payload));
    assert(rtc.call.rx_packets == before_rx + 1 && rtc.call.rx_bytes == 3);
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
        str(harness), str(cjson_dir / "cJSON.c"), "-lm", "-o", str(binary),
    ]
    if args.sanitize:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
