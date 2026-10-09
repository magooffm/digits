"""Compile real signaling.c with IDF cJSON and deterministic transport mocks.

This checks application ordering, ownership and bounded buffering. It neither
connects to a server nor simulates physical Wi-Fi, WebSocket ping replies or
ICE/DTLS/SRTP. The pinned WebSocket library owns actual control-frame replies.
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
parser.add_argument("--sanitize", action="store_true", help="Enable Address/UndefinedBehavior sanitizers")
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
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_TIMEOUT 0x107
#define BIT0 (1U << 0)
#define BIT1 (1U << 1)
#define BIT2 (1U << 2)
#define BIT3 (1U << 3)
#define pdTRUE 1
#define pdFALSE 0
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
typedef struct fake_event_group *EventGroupHandle_t;
typedef struct fake_queue *QueueHandle_t;
typedef struct fake_client *esp_websocket_client_handle_t;
typedef int portMUX_TYPE;
typedef const char *esp_event_base_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms) / 10U)
#define CONFIG_DIGITS_SERVER_URL "ws://test-server/ws"
#define ESP_ERROR_CHECK(result) assert((result) == ESP_OK)
void fake_log(const char *tag, const char *format, ...);
#define ESP_LOGI(tag, ...) fake_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) fake_log(tag, __VA_ARGS__)
const char *esp_err_to_name(esp_err_t error);
int64_t esp_timer_get_time(void);
void fake_enter(void);
void fake_exit(void);
#define portENTER_CRITICAL(lock) fake_enter()
#define portEXIT_CRITICAL(lock) fake_exit()
void vTaskDelay(TickType_t ticks);
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupGetBits(EventGroupHandle_t group);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                              int clear, int all, TickType_t ticks);
void vEventGroupDelete(EventGroupHandle_t group);
QueueHandle_t xQueueCreate(unsigned capacity, unsigned item_size);
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
void vQueueDelete(QueueHandle_t queue);
enum {
    WEBSOCKET_EVENT_ANY = -1, WEBSOCKET_EVENT_BEGIN = 1,
    WEBSOCKET_EVENT_CONNECTED, WEBSOCKET_EVENT_DISCONNECTED,
    WEBSOCKET_EVENT_CLOSED, WEBSOCKET_EVENT_FINISH,
    WEBSOCKET_EVENT_ERROR, WEBSOCKET_EVENT_DATA,
};
typedef struct {
    const char *uri;
    bool disable_auto_reconnect;
    int network_timeout_ms, task_stack, buffer_size;
    int ping_interval_sec, pingpong_timeout_sec;
} esp_websocket_client_config_t;
typedef struct {
    int op_code, data_len, payload_offset, payload_len;
    bool fin;
    char *data_ptr;
    struct {
        int error_type, esp_ws_handshake_status_code, esp_transport_sock_errno;
    } error_handle;
} esp_websocket_event_data_t;
typedef void (*fake_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config);
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client,
                                      int32_t events, fake_event_handler_t handler, void *context);
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client);
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client);
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client);
int esp_websocket_client_send_text(esp_websocket_client_handle_t client,
                                  const char *data, int length, TickType_t wait);
'''

harness = r'''
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "SIGNALING_SOURCE"

struct fake_event_group { EventBits_t bits; };
struct fake_queue {
    unsigned capacity, width, head, count;
    unsigned char *items;
};
struct fake_client {
    fake_event_handler_t handler;
    void *context;
    unsigned id, sends, polls;
    bool started, stopped, destroyed;
};
static struct fake_client clients[4];
static unsigned client_count, stops, destroys, waits, wait_begin, wifi_waits;
static unsigned callback_depth, critical_depth, atomic_publications;
static unsigned peer_up, peer_down, peer_received, ring_requests, saves, console_starts;
static unsigned json_live, tests;
static bool last_paired, inject_during_register, owner_running;
static bool register_fail, peer_write_fail, peer_init_fail;
static uint32_t last_session, received_session;
static unsigned delayed_ticks;
static int64_t now_us;
static char logs[65536], received[32768];
static size_t logs_used;
static esp_err_t save_result;
static digits_credentials_t stored;
static digits_webrtc_send_fn peer_send;
static void *peer_send_context;
enum owner_case { OWNER_NONE, OWNER_RECONNECT, OWNER_START_FAIL, OWNER_INIT_FAIL };
static enum owner_case owner_case;
static struct { unsigned client; char *wire; } sends[64];
static unsigned sent_count;

static void *json_alloc(size_t bytes)
{
    void *p = malloc(bytes);
    if (p) ++json_live;
    return p;
}
static void json_free(void *p)
{
    if (p) { assert(json_live); --json_live; free(p); }
}
void fake_log(const char *tag, const char *format, ...)
{
    (void)tag;
    va_list values;
    va_start(values, format);
    int count = vsnprintf(logs + logs_used, sizeof(logs) - logs_used, format, values);
    va_end(values);
    assert(count >= 0 && (size_t)count + 2 < sizeof(logs) - logs_used);
    logs_used += (size_t)count;
    logs[logs_used++] = '\n';
    logs[logs_used] = 0;
}
const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ESP_OK" : error == ESP_ERR_TIMEOUT ? "ESP_ERR_TIMEOUT" : "mock_error";
}
int64_t esp_timer_get_time(void) { return now_us; }
void fake_enter(void) { assert(!critical_depth); ++critical_depth; }
void fake_exit(void) { assert(critical_depth == 1); --critical_depth; }
void vTaskDelay(TickType_t ticks)
{
    assert(!callback_depth && ticks);
    delayed_ticks += ticks;
    now_us += (int64_t)ticks * 10000;
}
EventGroupHandle_t xEventGroupCreate(void)
{
    EventGroupHandle_t group = calloc(1, sizeof(*group));
    assert(group);
    return group;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    return group->bits |= bits;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    EventBits_t previous = group->bits;
    group->bits &= ~bits;
    return previous;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t group) { return group->bits; }
void vEventGroupDelete(EventGroupHandle_t group) { free(group); }
QueueHandle_t xQueueCreate(unsigned capacity, unsigned width)
{
    QueueHandle_t queue = calloc(1, sizeof(*queue));
    assert(queue);
    queue->capacity = capacity;
    queue->width = width;
    queue->items = calloc(capacity, width);
    assert(queue->items);
    return queue;
}
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    assert(queue && wait == 0);
    if (queue == outbound.queue) {
        // The real publication must remain inside its readiness/session guard.
        assert(critical_depth == 1);
        ++atomic_publications;
    }
    if (queue->count == queue->capacity) return pdFALSE;
    unsigned tail = (queue->head + queue->count++) % queue->capacity;
    memcpy(queue->items + tail * queue->width, item, queue->width);
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    assert(queue && wait == 0 && !critical_depth);
    if (!queue->count) return pdFALSE;
    memcpy(item, queue->items + queue->head * queue->width, queue->width);
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->count;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t queue)
{
    assert(queue && !queue->count);
    free(queue->items);
    free(queue);
}
static cJSON *message(const char *type)
{
    cJSON *json = cJSON_CreateObject();
    assert(json && cJSON_AddStringToObject(json, "type", type));
    return json;
}
static esp_err_t queue_message(const char *type, uint32_t session)
{
    cJSON *json = message(type);
    assert(cJSON_AddStringToObject(json, "to", "2002"));
    if (!strcmp(type, "sdp")) assert(cJSON_AddStringToObject(json, "sdp", "v=0\r\n"));
    esp_err_t error = enqueue_peer_message(json, session, NULL);
    cJSON_Delete(json); // Queue must retain its own wire copy.
    return error;
}
static void emit(struct fake_client *client, int32_t id, esp_websocket_event_data_t *event)
{
    assert(client->handler && !callback_depth);
    ++callback_depth;
    client->handler(client->context, "mock_ws", id, event);
    --callback_depth;
}
static void text_event(struct fake_client *client, const char *text)
{
    esp_websocket_event_data_t event = {
        .op_code = 1, .fin = true, .data_ptr = (char *)text,
        .data_len = (int)strlen(text), .payload_len = (int)strlen(text),
    };
    emit(client, WEBSOCKET_EVENT_DATA, &event);
}
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config)
{
    assert(!callback_depth && client_count < 4);
    assert(config->disable_auto_reconnect && config->buffer_size == 1024);
    assert(config->ping_interval_sec == 30 && config->pingpong_timeout_sec == 15);
    struct fake_client *client = &clients[client_count];
    client->id = ++client_count;
    return client;
}
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client,
                                      int32_t events, fake_event_handler_t handler, void *context)
{
    assert(events == WEBSOCKET_EVENT_ANY);
    client->handler = handler;
    client->context = context;
    return ESP_OK;
}
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client)
{
    assert(!callback_depth);
    if (owner_case == OWNER_START_FAIL && client->id == 1) return ESP_FAIL;
    client->started = true;
    emit(client, WEBSOCKET_EVENT_CONNECTED, NULL);
    return ESP_OK;
}
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client)
{
    assert(!callback_depth && client->started && !client->stopped);
    if (owner_running) assert(client->polls >= 2);
    client->stopped = true;
    ++stops;
    // Actual stop joins callbacks; a final event can happen during this call.
    emit(client, WEBSOCKET_EVENT_FINISH, NULL);
    return ESP_OK;
}
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client)
{
    assert(!callback_depth && !client->destroyed);
    assert(!client->started || client->stopped);
    client->destroyed = true;
    ++destroys;
    return ESP_OK;
}
int esp_websocket_client_send_text(esp_websocket_client_handle_t client,
                                  const char *data, int length, TickType_t wait)
{
    assert(client && !critical_depth && sent_count < 64);
    assert(length > 0 && (size_t)length == strlen(data));
    cJSON *json = cJSON_Parse(data);
    assert(json);
    const char *type = string_field(json, "type");
    assert(type);
    bool registration = !strcmp(type, "register");
    if (!client->sends) assert(registration); // Every new WS starts with register.
    if (registration) {
        assert(callback_depth == 1 && client->sends == 0 && wait == pdMS_TO_TICKS(3000));
        if (inject_during_register) {
            signaling_t *s = client->context;
            assert(queue_message("call", s->session) == ESP_ERR_INVALID_STATE);
        }
    } else {
        assert(!callback_depth && client->sends && wait == pdMS_TO_TICKS(1000));
    }
    cJSON_Delete(json);
    sends[sent_count].wire = malloc((size_t)length + 1);
    assert(sends[sent_count].wire);
    memcpy(sends[sent_count].wire, data, (size_t)length + 1);
    sends[sent_count++].client = client->id;
    ++client->sends;
    return (registration && register_fail) || (!registration && peer_write_fail) ? length - 1 : length;
}
esp_err_t credentials_save(const digits_credentials_t *credentials)
{
    assert(callback_depth == 1 && !clients[client_count - 1].stopped);
    ++saves;
    if (save_result == ESP_OK) stored = *credentials;
    return save_result;
}
void digits_ring_test_request(void) { assert(callback_depth == 1); ++ring_requests; }
void digits_wifi_wait_connected(void) { assert(!callback_depth); ++wifi_waits; }
esp_err_t digits_webrtc_console_start(void) { ++console_starts; return ESP_OK; }
esp_err_t digits_webrtc_init(digits_webrtc_send_fn send, void *context)
{
    peer_send = send;
    peer_send_context = context;
    return peer_init_fail ? ESP_FAIL : ESP_OK;
}
void digits_webrtc_transport(uint32_t session, bool online, const char *number, bool paired)
{
    last_session = session;
    if (!online) { ++peer_down; return; }
    ++peer_up;
    last_paired = paired;
    assert(number);
    // Deliberately emit setup immediately after readiness. First registration
    // must have completed already, and unpaired transports must reject traffic.
    if (owner_running && !peer_init_fail) {
        assert(queue_message("call", session - 1) == ESP_ERR_INVALID_STATE);
        assert(queue_message("call", session) == (paired ? ESP_OK : ESP_ERR_INVALID_STATE));
        if (paired) assert(queue_message("sdp", session) == ESP_OK);
    }
}
bool digits_webrtc_receive(const cJSON *json, uint32_t session)
{
    assert(callback_depth == 1);
    const char *type = string_field((cJSON *)json, "type");
    if (!type || !strcmp(type, "unknown")) return false;
    char *wire = cJSON_PrintUnformatted(json);
    assert(wire && strlen(wire) < sizeof(received));
    strcpy(received, wire);
    cJSON_free(wire);
    received_session = session;
    ++peer_received;
    return true;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                              int clear, int all, TickType_t ticks)
{
    ++waits;
    if (bits == START_RETURNED) {
        assert(callback_depth == 1 && all == pdTRUE && clear == pdFALSE && ticks == portMAX_DELAY);
        assert(group->bits & START_RETURNED);
        ++wait_begin;
        return group->bits;
    }
    assert(owner_running && !callback_depth && clear == pdFALSE && all == pdFALSE);
    assert(bits == (RETRY | CREDENTIALS_CHANGED | STORAGE_FAILED) && ticks == pdMS_TO_TICKS(10));
    struct fake_client *client = &clients[client_count - 1];
    assert(++client->polls <= 2); // Wrong START_RETURNED mask stops on the first poll.
    now_us += 10000;
    assert(group->bits & START_RETURNED);
    if (client->polls == 2) {
        if (owner_case == OWNER_RECONNECT && client->id == 1) {
            assert(client->sends == 3); // register, then queued call and SDP.
            assert(queue_message("sdp", last_session) == ESP_OK);
            emit(client, WEBSOCKET_EVENT_DISCONNECTED, NULL);
        } else {
            save_result = ESP_ERR_TIMEOUT;
            text_event(client, "{\"type\":\"paired\",\"number\":\"3003\",\"device_token\":\"new-test-token\"}");
            assert(group->bits & STORAGE_FAILED);
        }
    }
    // Native FreeRTOS returns ALL set bits, including unrelated persistent
    // START_RETURNED after a timeout. The owner must mask terminal bits.
    return group->bits;
}

static void reset(void)
{
    assert(!json_live && !outbound.queue && !critical_depth && !callback_depth);
    memset(clients, 0, sizeof(clients));
    client_count = stops = destroys = waits = wait_begin = wifi_waits = 0;
    atomic_publications = peer_up = peer_down = peer_received = ring_requests = saves = console_starts = 0;
    last_paired = inject_during_register = owner_running = register_fail = peer_write_fail = peer_init_fail = false;
    last_session = received_session = delayed_ticks = 0;
    now_us = 1000000;
    logs_used = sent_count = 0;
    logs[0] = received[0] = 0;
    save_result = ESP_OK;
    memset(&stored, 0, sizeof(stored));
    owner_case = OWNER_NONE;
    peer_send = NULL;
    peer_send_context = NULL;
}
static digits_credentials_t credentials(bool paired)
{
    digits_credentials_t c = {.hardware_id = "00112233-4455-4677-8899-aabbccddeeff"};
    if (paired) { strcpy(c.number, "1001"); strcpy(c.device_token, "original-test-token"); }
    return c;
}
static void setup(signaling_t *s, digits_credentials_t *c)
{
    reset();
    memset(s, 0, sizeof(*s));
    s->credentials = c;
    s->session = 9;
    s->events = xEventGroupCreate();
    outbound.queue = xQueueCreate(TX_QUEUE_LENGTH, sizeof(tx_message_t));
    const esp_websocket_client_config_t cfg = {
        .disable_auto_reconnect = true, .buffer_size = 1024,
        .ping_interval_sec = 30, .pingpong_timeout_sec = 15,
    };
    s->client = esp_websocket_client_init(&cfg);
    assert(esp_websocket_register_events(s->client, WEBSOCKET_EVENT_ANY, websocket_event, s) == ESP_OK);
    outbound_set(s->session, false);
}
static void connect(signaling_t *s) { emit(s->client, WEBSOCKET_EVENT_CONNECTED, NULL); }
static void cleanup(signaling_t *s)
{
    discard_outbound();
    vQueueDelete(outbound.queue);
    memset(&outbound, 0, sizeof(outbound));
    if (s) vEventGroupDelete(s->events);
    for (unsigned i = 0; i < sent_count; ++i) free(sends[i].wire);
    assert(!json_live && !critical_depth && !callback_depth);
    ++tests;
}
static void test_registration_and_pairing(void)
{
    signaling_t s;
    digits_credentials_t c = credentials(false);
    setup(&s, &c);
    inject_during_register = true;
    assert(queue_message("call", s.session) == ESP_ERR_INVALID_STATE);
    connect(&s);
    assert(sent_count == 1 && peer_up == 1 && !last_paired && !outbound.registered);
    cJSON *json = cJSON_Parse(sends[0].wire);
    assert(cJSON_GetArraySize(json) == 3);
    assert(!strcmp(string_field(json, "number"), "unpaired"));
    assert(!strcmp(string_field(json, "hardware_id"), c.hardware_id));
    assert(!cJSON_GetObjectItem(json, "device_token"));
    cJSON_Delete(json);
    assert(queue_message("call", s.session) == ESP_ERR_INVALID_STATE);
    text_event(s.client, "{\"type\":\"pairing_code\",\"pairing_code\":\"004201\",\"pairing_code_ttl\":300}");
    assert(strstr(logs, "PAIRING CODE: 004201 (TTL: 300 seconds)") && !peer_received);
    text_event(s.client, "{\"type\":\"paired\",\"number\":\"1001\",\"device_token\":\"original-test-token\"}");
    assert(saves == 1 && !strcmp(c.device_token, stored.device_token));
    assert(!strcmp(c.number, "1001") && (s.events->bits & CREDENTIALS_CHANGED));
    assert(!stops && !destroys); // Deliberate reconnect belongs to the owner.
    text_event(s.client, "{\"type\":\"paired\",\"number\":\"1001\",\"device_token\":\"original-test-token\"}");
    assert(saves == 1);
    cleanup(&s);

    c = credentials(true);
    setup(&s, &c);
    inject_during_register = true;
    connect(&s);
    json = cJSON_Parse(sends[0].wire);
    assert(cJSON_GetArraySize(json) == 4 && last_paired && outbound.registered);
    assert(!strcmp(string_field(json, "number"), "1001"));
    assert(!strcmp(string_field(json, "device_token"), "original-test-token"));
    cJSON_Delete(json);
    assert(!strstr(logs, "original-test-token"));
    assert(queue_message("call", s.session) == ESP_OK);
    drain_outbound(&s);
    assert(sent_count == 2 && strstr(sends[1].wire, "\"type\":\"call\""));
    cleanup(&s);
}
static void test_mailbox_bounds_and_generations(void)
{
    signaling_t s;
    digits_credentials_t c = credentials(true);
    setup(&s, &c);
    connect(&s);
    for (unsigned i = 0; i < TX_QUEUE_LENGTH; ++i)
        assert(queue_message("call", s.session) == ESP_OK);
    assert(outbound.queue->count == 8 && json_live == 8);
    assert(queue_message("call", s.session) == ESP_ERR_NO_MEM && json_live == 8);
    assert(queue_message("call", s.session - 1) == ESP_ERR_INVALID_STATE && json_live == 8);
    assert(atomic_publications == 9);
    drain_outbound(&s);
    assert(sent_count == 9 && !json_live && !outbound.queue->count);
    assert(queue_message("call", s.session) == ESP_OK);
    assert(queue_message("sdp", s.session) == ESP_OK);
    uint32_t old = s.session++;
    transport_down(&s);
    outbound_set(s.session, true);
    drain_outbound(&s); // Old queued work cannot reach the new connection.
    assert(sent_count == 9 && !json_live);
    assert(queue_message("call", old) == ESP_ERR_INVALID_STATE);
    assert(queue_message("call", s.session) == ESP_OK);
    drain_outbound(&s);
    assert(sent_count == 10);
    cleanup(&s);
}
static void test_wire_limit_and_terminal_gates(void)
{
    signaling_t s;
    digits_credentials_t c = credentials(true);
    setup(&s, &c);
    connect(&s);
    cJSON *json = message("sdp");
    char large[MAX_MESSAGE + 1];
    memset(large, 'x', MAX_MESSAGE);
    large[MAX_MESSAGE] = 0;
    assert(cJSON_AddStringToObject(json, "sdp", large));
    unsigned before = json_live;
    assert(enqueue_peer_message(json, s.session, NULL) == ESP_ERR_INVALID_SIZE);
    assert(json_live == before && !outbound.queue->count);
    cJSON_Delete(json);
    EventBits_t terminal[] = {RETRY, CREDENTIALS_CHANGED, STORAGE_FAILED};
    for (unsigned i = 0; i < 3; ++i) {
        assert(queue_message("call", s.session) == ESP_OK);
        s.events->bits = START_RETURNED | terminal[i];
        drain_outbound(&s);
        assert(sent_count == 1 && !json_live);
    }
    s.events->bits = START_RETURNED;
    assert(queue_message("call", s.session) == ESP_OK);
    transport_down(&s);
    drain_outbound(&s);
    assert(sent_count == 1 && !json_live);
    cleanup(&s);
}
static void test_write_failures(void)
{
    signaling_t s;
    digits_credentials_t c = credentials(true);
    setup(&s, &c);
    register_fail = true;
    connect(&s);
    assert(s.events->bits & RETRY);
    assert(!peer_up && !outbound.registered && !outbound.queue->count);
    assert(queue_message("call", s.session) == ESP_ERR_INVALID_STATE);
    assert(!stops && !destroys);
    cleanup(&s);

    setup(&s, &c);
    connect(&s);
    assert(queue_message("call", s.session) == ESP_OK);
    assert(queue_message("sdp", s.session) == ESP_OK);
    peer_write_fail = true;
    drain_outbound(&s);
    assert(s.events->bits & RETRY);
    assert(!outbound.registered && peer_down == 1 && outbound.queue->count == 1);
    assert(queue_message("call", s.session) == ESP_ERR_INVALID_STATE);
    assert(strstr(logs, "Peer signaling write failed") && !stops && !destroys);
    cleanup(&s);
}
static void test_fragmented_sdp_and_ping(void)
{
    signaling_t s;
    digits_credentials_t c = credentials(true);
    setup(&s, &c);
    connect(&s);
    char sdp[10001];
    strcpy(sdp, "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n");
    size_t prefix = strlen(sdp);
    memset(sdp + prefix, 'x', sizeof(sdp) - prefix - 1);
    sdp[sizeof(sdp) - 1] = 0;
    cJSON *json = message("sdp");
    assert(cJSON_AddStringToObject(json, "from", "2002"));
    assert(cJSON_AddStringToObject(json, "sdp", sdp));
    char *wire = cJSON_PrintUnformatted(json);
    assert(wire && strlen(wire) > 4096 && strlen(wire) < MAX_MESSAGE);
    cJSON_Delete(json);
    size_t length = strlen(wire), offset = 0;
    const size_t first_frame = 1600;
    while (offset < length) {
        bool first = offset < first_frame;
        size_t frame_start = first ? 0 : first_frame;
        size_t frame_length = first ? first_frame : length - first_frame;
        size_t count = frame_start + frame_length - offset;
        if (count > 700) count = 700;
        esp_websocket_event_data_t event = {
            .op_code = first ? 1 : 0, .fin = !first,
            .payload_offset = (int)(offset - frame_start), .payload_len = (int)frame_length,
            .data_ptr = wire + offset, .data_len = (int)count,
        };
        emit(s.client, WEBSOCKET_EVENT_DATA, &event);
        offset += count;
        if (offset == first_frame) {
            assert(!peer_received && s.used == first_frame && s.text_active);
            event = (esp_websocket_event_data_t){.op_code = 9, .fin = true, .data_ptr = "ping", .data_len = 4, .payload_len = 4};
            emit(s.client, WEBSOCKET_EVENT_DATA, &event);
            event.op_code = 10;
            emit(s.client, WEBSOCKET_EVENT_DATA, &event);
            assert(s.used == first_frame && s.text_active && !peer_received);
        }
        assert(peer_received == (offset == length ? 1U : 0U));
    }
    assert(received_session == s.session && !s.text_active && !(s.events->bits & RETRY));
    json = cJSON_Parse(received);
    assert(!strcmp(string_field(json, "from"), "2002"));
    assert(!strcmp(string_field(json, "sdp"), sdp));
    cJSON_Delete(json);
    cJSON_free(wire);
    assert(strstr(logs, "Server PING received") && sent_count == 1);
    cleanup(&s);
}
static void test_dispatch_preserves_existing_paths(void)
{
    signaling_t s;
    digits_credentials_t c = credentials(true);
    setup(&s, &c);
    connect(&s);
    text_event(s.client, "{\"type\":\"error\",\"error\":\"phone not connected: exact server text\"}");
    assert(peer_received == 1 && received_session == s.session);
    assert(strstr(logs, "Server error: phone not connected: exact server text"));
    text_event(s.client, "{\"type\":\"ring_test\",\"voicemail_unheard_count\":0}");
    assert(ring_requests == 1 && peer_received == 1);
    text_event(s.client, "{\"type\":\"unknown\"}");
    assert(peer_received == 1 && strstr(logs, "Ignoring message type=unknown"));
    text_event(s.client, "{\"type\":\"line_renumber\",\"number\":\"4004\"}");
    assert(saves == 1 && !strcmp(c.number, "4004"));
    assert(!strcmp(c.device_token, "original-test-token") && s.events->bits & CREDENTIALS_CHANGED);
    s.events->bits = 0;
    text_event(s.client, "{\"type\":\"paired\",\"number\":\"5005\"}");
    assert(saves == 1 && !s.events->bits && !strcmp(c.number, "4004"));
    save_result = ESP_ERR_TIMEOUT;
    text_event(s.client, "{\"type\":\"paired\",\"number\":\"5005\",\"device_token\":\"new-test-token\"}");
    assert(saves == 2 && s.events->bits & STORAGE_FAILED);
    assert(!strcmp(c.number, "4004") && !strcmp(c.device_token, "original-test-token"));
    assert(!stops && !destroys);
    cleanup(&s);
}
static void test_bad_messages_and_callback_ownership(void)
{
    signaling_t s;
    digits_credentials_t c = credentials(true);
    setup(&s, &c);
    connect(&s);
    s.events->bits = START_RETURNED;
    emit(s.client, WEBSOCKET_EVENT_BEGIN, NULL);
    assert(wait_begin == 1);
    text_event(s.client, "{broken JSON}");
    assert(!peer_received && strstr(logs, "Invalid JSON signaling message"));
    static char huge[MAX_MESSAGE + 1];
    memset(huge, 'x', sizeof(huge));
    esp_websocket_event_data_t event = {.op_code = 1, .data_ptr = huge, .data_len = sizeof(huge), .payload_len = sizeof(huge), .fin = true};
    emit(s.client, WEBSOCKET_EVENT_DATA, &event);
    assert(s.events->bits & RETRY && !s.text_active && !peer_received);
    unsigned down = peer_down;
    emit(s.client, WEBSOCKET_EVENT_DISCONNECTED, NULL);
    emit(s.client, WEBSOCKET_EVENT_CLOSED, NULL);
    emit(s.client, WEBSOCKET_EVENT_FINISH, NULL);
    assert(peer_down == down + 3 && !outbound.registered && !stops && !destroys);
    cleanup(&s);
}
static void test_owner(enum owner_case scenario)
{
    reset();
    owner_case = scenario;
    owner_running = inject_during_register = true;
    peer_init_fail = scenario == OWNER_INIT_FAIL;
    digits_credentials_t c = credentials(true);
    digits_signaling_run(&c); // Real reconnect/cleanup loop, exited by NVS failure.
    assert(saves == 1 && !strcmp(c.number, "1001") && !strcmp(c.device_token, "original-test-token"));
    assert(peer_send && peer_send_context == NULL && !outbound.registered);
    if (scenario == OWNER_RECONNECT) {
        assert(client_count == 2 && stops == 2 && destroys == 2 && delayed_ticks == 600);
        assert(sent_count == 6 && wifi_waits == 2);
        assert(sends[0].client == 1 && sends[3].client == 2);
        assert(strstr(sends[0].wire, "\"type\":\"register\"") && strstr(sends[3].wire, "\"type\":\"register\""));
    } else if (scenario == OWNER_START_FAIL) {
        assert(client_count == 2 && stops == 1 && destroys == 2 && delayed_ticks == 600);
        assert(sent_count == 3 && sends[0].client == 2);
    } else {
        assert(client_count == 1 && stops == 1 && destroys == 1 && !console_starts);
        assert(sent_count == 1);
    }
    assert(!outbound.queue->count && logs_used && strstr(logs, "No deliberate reconnect"));
    cleanup(NULL);
}
int main(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_alloc, .free_fn = json_free};
    cJSON_InitHooks(&hooks);
    test_registration_and_pairing();
    test_mailbox_bounds_and_generations();
    test_wire_limit_and_terminal_gates();
    test_write_failures();
    test_fragmented_sdp_and_ping();
    test_dispatch_preserves_existing_paths();
    test_bad_messages_and_callback_ownership();
    test_owner(OWNER_RECONNECT);
    test_owner(OWNER_START_FAIL);
    test_owner(OWNER_INIT_FAIL);
    printf("signaling.c: %u transport/owner scenarios passed; real cJSON, no network or physical hardware\n", tests);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="digits-signaling-webrtc-") as temporary:
    directory = Path(temporary)
    (directory / "fake_idf.h").write_text(fake_header)
    for name in ("esp_err.h", "esp_log.h", "esp_timer.h", "esp_websocket_client.h", "sdkconfig.h",
                 "freertos/FreeRTOS.h", "freertos/event_groups.h", "freertos/queue.h"):
        header = directory / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "fake_idf.h"\n')
    source = directory / "signaling_test.c"
    source.write_text(harness.replace("SIGNALING_SOURCE", str(repo / "esp32/main/signaling.c")))
    executable = directory / "signaling_test"
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        # Apple's sanitizer headers deprecate sprintf used by unmodified cJSON.
        "-Wno-unused-variable", "-Wno-deprecated-declarations",
        "-I", str(directory), "-I", str(cjson),
        str(source), str(cjson / "cJSON.c"), "-lm", "-o", str(executable),
    ]
    if args.sanitize:
        command += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
