#include "signaling.h"
#include "ring_test.h"
#include "webrtc.h"
#include "webrtc_console.h"
#include "wifi.h"
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#define RETRY BIT0
#define CREDENTIALS_CHANGED BIT1
#define STORAGE_FAILED BIT2
#define START_RETURNED BIT3
#define MAX_MESSAGE DIGITS_WEBRTC_MAX_SIGNALING_MESSAGE
#define TX_QUEUE_LENGTH 8

typedef struct {
    digits_credentials_t *credentials;
    EventGroupHandle_t events;
    esp_websocket_client_handle_t client;
    char rx[MAX_MESSAGE + 1];
    size_t used;
    bool text_active;
    int64_t connected_at;
    uint32_t session;
} signaling_t;

typedef struct {
    char *wire;
    uint32_t session;
} tx_message_t;

// Boot-lifetime mailbox: the peer task never retains the WebSocket owner's
// context or client handle. This also stays safe after a fatal NVS error.
static struct {
    QueueHandle_t queue;
    uint32_t session;
    bool registered;
} outbound;
static portMUX_TYPE outbound_lock = portMUX_INITIALIZER_UNLOCKED;

static void outbound_set(uint32_t session, bool registered)
{
    portENTER_CRITICAL(&outbound_lock);
    outbound.session = session;
    outbound.registered = registered;
    portEXIT_CRITICAL(&outbound_lock);
}

static esp_err_t enqueue_peer_message(const cJSON *json, uint32_t session, void *ctx)
{
    (void)ctx;
    char *wire = cJSON_PrintUnformatted(json);
    if (!wire) return ESP_ERR_NO_MEM;
    if (strlen(wire) > MAX_MESSAGE) {
        cJSON_free(wire);
        return ESP_ERR_INVALID_SIZE;
    }
    tx_message_t message = {.wire = wire, .session = session};
    portENTER_CRITICAL(&outbound_lock);
    bool ready = outbound.registered && outbound.session == session;
    // Zero wait, with no queue receiver waiting on this queue. The small
    // critical section makes session invalidation atomic with publication.
    bool queued = ready && xQueueSend(outbound.queue, &message, 0) == pdTRUE;
    portEXIT_CRITICAL(&outbound_lock);
    if (!queued) cJSON_free(wire);
    return queued ? ESP_OK : ready ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
}

static void discard_outbound(void)
{
    tx_message_t message;
    while (xQueueReceive(outbound.queue, &message, 0) == pdTRUE)
        cJSON_free(message.wire);
}

static void transport_down(signaling_t *s)
{
    outbound_set(s->session, false);
    digits_webrtc_transport(s->session, false, NULL, false);
}

static void drain_outbound(signaling_t *s)
{
    tx_message_t message;
    for (unsigned i = 0; i < TX_QUEUE_LENGTH &&
         xQueueReceive(outbound.queue, &message, 0) == pdTRUE; ++i) {
        portENTER_CRITICAL(&outbound_lock);
        bool ready = outbound.registered && outbound.session == message.session;
        portEXIT_CRITICAL(&outbound_lock);
        bool failed = false;
        if (ready && message.session == s->session &&
            !(xEventGroupGetBits(s->events) & (RETRY | CREDENTIALS_CHANGED | STORAGE_FAILED))) {
            size_t length = strlen(message.wire);
            int sent = esp_websocket_client_send_text(s->client, message.wire, length,
                                                      pdMS_TO_TICKS(1000));
            failed = sent != (int)length;
            if (failed) ESP_LOGW("signaling", "Peer signaling write failed; reconnecting");
        }
        cJSON_free(message.wire);
        if (failed) {
            transport_down(s);
            xEventGroupSetBits(s->events, RETRY);
            break;
        }
    }
}

static const char *string_field(cJSON *json, const char *name)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, name);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static bool copy_field(char *dest, size_t capacity, const char *value)
{
    if (!value || !value[0] || strlen(value) >= capacity) return false;
    strcpy(dest, value);
    return true;
}

static void dispatch_message(signaling_t *s)
{
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(s->rx, s->used + 1, &end, true);
    const char *type = string_field(json, "type");
    if (!type) {
        ESP_LOGW("signaling", "Invalid JSON signaling message");
        cJSON_Delete(json);
        return;
    }
    if (strcmp(type, "pairing_code") == 0) {
        const char *code = string_field(json, "pairing_code");
        cJSON *ttl = cJSON_GetObjectItemCaseSensitive(json, "pairing_code_ttl");
        if (code && cJSON_IsNumber(ttl))
            ESP_LOGI("signaling", "PAIRING CODE: %s (TTL: %d seconds)", code, ttl->valueint);
        else ESP_LOGW("signaling", "Invalid pairing_code fields");
    } else if (strcmp(type, "paired") == 0 || strcmp(type, "line_renumber") == 0) {
        bool paired = strcmp(type, "paired") == 0;
        digits_credentials_t next = *s->credentials;
        bool valid = copy_field(next.number, sizeof(next.number), string_field(json, "number"));
        if (paired) valid = valid && copy_field(next.device_token, sizeof(next.device_token),
                                               string_field(json, "device_token"));
        else valid = valid && next.device_token[0];
        if (!valid) {
            ESP_LOGE("signaling", "Invalid %s credentials; keeping existing state", type);
        } else if (strcmp(next.number, s->credentials->number) != 0 ||
                   strcmp(next.device_token, s->credentials->device_token) != 0) {
            esp_err_t err = credentials_save(&next);
            if (err == ESP_OK) {
                *s->credentials = next;
                ESP_LOGI("signaling", "%s persisted: number=%s; reconnecting", type, next.number);
                xEventGroupSetBits(s->events, CREDENTIALS_CHANGED);
            } else {
                ESP_LOGE("signaling", "%s persistence failed: %s; stopping automatic retries", type, esp_err_to_name(err));
                xEventGroupSetBits(s->events, STORAGE_FAILED);
            }
        }
    } else if (strcmp(type, "ring_test") == 0) {
        digits_ring_test_request();
    } else if (strcmp(type, "error") == 0) {
        const char *error = string_field(json, "error");
        ESP_LOGE("signaling", "Server error: %s", error ? error : "<missing error field>");
        digits_webrtc_receive(json, s->session);
    } else {
        if (!digits_webrtc_receive(json, s->session))
            ESP_LOGI("signaling", "Ignoring message type=%s", type);
    }
    cJSON_Delete(json);
}

static bool send_register(signaling_t *s)
{
    digits_credentials_t *c = s->credentials;
    cJSON *json = cJSON_CreateObject();
    bool ok = json && cJSON_AddStringToObject(json, "type", "register") &&
        cJSON_AddStringToObject(json, "number", c->device_token[0] ? c->number : "unpaired") &&
        cJSON_AddStringToObject(json, "hardware_id", c->hardware_id);
    if (ok && c->device_token[0]) ok = cJSON_AddStringToObject(json, "device_token", c->device_token) != NULL;
    char *wire = ok ? cJSON_PrintUnformatted(json) : NULL;
    int sent = wire ? esp_websocket_client_send_text(s->client, wire, strlen(wire), pdMS_TO_TICKS(3000)) : -1;
    bool registered = wire && sent == (int)strlen(wire);
    if (!registered) {
        ESP_LOGE("signaling", "Failed to send register; reconnecting");
        xEventGroupSetBits(s->events, RETRY);
    } else {
        ESP_LOGI("signaling", "register sent: number=%s hardware_id=%s token=%s (no register acknowledgement)",
                 c->device_token[0] ? c->number : "unpaired", c->hardware_id,
                 c->device_token[0] ? "present" : "absent");
    }
    cJSON_free(wire);
    cJSON_Delete(json);
    return registered;
}

static void websocket_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    signaling_t *s = arg;
    esp_websocket_event_data_t *event = data;
    if (id == WEBSOCKET_EVENT_BEGIN) {
        // v1.6.1 clears STOPPED_BIT after creating its worker task. Prevent a
        // fast failed connection from finishing before start() clears that bit,
        // which would otherwise make stop()/destroy() wait forever.
        xEventGroupWaitBits(s->events, START_RETURNED, pdFALSE, pdTRUE, portMAX_DELAY);
    } else if (id == WEBSOCKET_EVENT_CONNECTED) {
        s->connected_at = esp_timer_get_time();
        ESP_LOGI("signaling", "WebSocket connected");
        // Synchronous CONNECTED callback: first application frame, before reads.
        if (send_register(s)) {
            // Publish readiness only after the synchronous first frame. There
            // is no server register acknowledgement in the Digits protocol.
            // Pairing messages still arrive on the unpaired registration, but
            // peer call traffic requires a registration with a stored token.
            outbound_set(s->session, s->credentials->device_token[0] != 0);
            digits_webrtc_transport(s->session, true, s->credentials->number,
                                    s->credentials->device_token[0] != 0);
        }
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED ||
               id == WEBSOCKET_EVENT_FINISH) {
        transport_down(s);
        xEventGroupSetBits(s->events, RETRY);
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGW("signaling", "WebSocket error: type=%d HTTP=%d errno=%d",
                 event->error_handle.error_type, event->error_handle.esp_ws_handshake_status_code,
                 event->error_handle.esp_transport_sock_errno);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        // The pinned client answers PING automatically with the same payload.
        // Control frames may interrupt a fragmented text message; do not reset RX.
        if (event->op_code == 0x9) {
            ESP_LOGI("signaling", "Server PING received; transport replies with PONG");
            return;
        }
        if (event->op_code != 0x1 && event->op_code != 0x0) return;
        if (event->op_code == 0x1 && event->payload_offset == 0) {
            s->used = 0;
            s->text_active = true;
        }
        if (!s->text_active) return;
        if (event->data_len < 0 || (size_t)event->data_len > MAX_MESSAGE - s->used) {
            ESP_LOGW("signaling", "Oversized signaling message; reconnecting");
            s->text_active = false;
            xEventGroupSetBits(s->events, RETRY);
            return;
        }
        if (event->data_len) memcpy(s->rx + s->used, event->data_ptr, event->data_len);
        s->used += event->data_len;
        if (event->fin && event->payload_offset + event->data_len == event->payload_len) {
            s->rx[s->used] = 0;
            dispatch_message(s);
            s->text_active = false;
        }
    }
}

void digits_signaling_run(digits_credentials_t *credentials)
{
    const char *url = CONFIG_DIGITS_SERVER_URL;
    size_t length = strlen(url);
    ESP_ERROR_CHECK(length > 8 && strncmp(url, "ws://", 5) == 0 &&
                    strcmp(url + length - 3, "/ws") == 0 ? ESP_OK : ESP_ERR_INVALID_ARG);
    signaling_t *s = calloc(1, sizeof(*s));
    ESP_ERROR_CHECK(s ? ESP_OK : ESP_ERR_NO_MEM);
    s->credentials = credentials;
    s->events = xEventGroupCreate();
    ESP_ERROR_CHECK(s->events ? ESP_OK : ESP_ERR_NO_MEM);
    outbound.queue = xQueueCreate(TX_QUEUE_LENGTH, sizeof(tx_message_t));
    ESP_ERROR_CHECK(outbound.queue ? ESP_OK : ESP_ERR_NO_MEM);
    esp_err_t peer_err = digits_webrtc_init(enqueue_peer_message, NULL);
    if (peer_err != ESP_OK)
        ESP_LOGE("signaling", "WebRTC initialization failed: %s; signaling continues", esp_err_to_name(peer_err));
    else {
        peer_err = digits_webrtc_console_start();
        if (peer_err != ESP_OK)
            ESP_LOGE("signaling", "WebRTC development console unavailable: %s", esp_err_to_name(peer_err));
    }
    unsigned delay_seconds = 0;
    for (;;) {
        if (delay_seconds) {
            ESP_LOGI("signaling", "Retry in %u seconds", delay_seconds);
            vTaskDelay(pdMS_TO_TICKS(delay_seconds * 1000));
        }
        digits_wifi_wait_connected();
        s->connected_at = 0;
        ++s->session;
        transport_down(s);
        discard_outbound();
        s->used = 0;
        s->text_active = false;
        xEventGroupClearBits(s->events, RETRY | CREDENTIALS_CHANGED | STORAGE_FAILED | START_RETURNED);
        esp_websocket_client_config_t config = {
            .uri = url, .disable_auto_reconnect = true,
            .network_timeout_ms = 3000, .task_stack = 6144,
            .buffer_size = 1024, .ping_interval_sec = 30,
            .pingpong_timeout_sec = 15,
        };
        s->client = esp_websocket_client_init(&config);
        ESP_ERROR_CHECK(s->client ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(esp_websocket_register_events(s->client, WEBSOCKET_EVENT_ANY, websocket_event, s));
        esp_err_t err = esp_websocket_client_start(s->client);
        xEventGroupSetBits(s->events, START_RETURNED);
        EventBits_t bits = RETRY;
        if (err == ESP_OK) {
            do {
                bits = xEventGroupWaitBits(s->events,
                    RETRY | CREDENTIALS_CHANGED | STORAGE_FAILED, pdFALSE, pdFALSE,
                    pdMS_TO_TICKS(10)) & (RETRY | CREDENTIALS_CHANGED | STORAGE_FAILED);
                if (!bits) drain_outbound(s);
                bits |= xEventGroupGetBits(s->events) & (RETRY | CREDENTIALS_CHANGED | STORAGE_FAILED);
            } while (!bits);
        }
        else ESP_LOGW("signaling", "WebSocket start failed: %s", esp_err_to_name(err));
        // Stop/destroy are forbidden inside the WebSocket callback. Owner only.
        transport_down(s);
        if (err == ESP_OK) esp_websocket_client_stop(s->client);
        ESP_ERROR_CHECK(esp_websocket_client_destroy(s->client));
        discard_outbound();
        // Stop joins the callback, so collect any credential update racing closure.
        bits |= xEventGroupGetBits(s->events);
        if (bits & STORAGE_FAILED) {
            ESP_LOGE("signaling", "Credentials could not be committed; fix NVS and reboot. No deliberate reconnect.");
            vEventGroupDelete(s->events);
            free(s);
            return;
        }
        if (bits & CREDENTIALS_CHANGED) delay_seconds = 0;
        else {
            if (s->connected_at && esp_timer_get_time() - s->connected_at >= 45000000) delay_seconds = 0;
            else delay_seconds = delay_seconds == 0 ? 6 : delay_seconds < 30 ? delay_seconds * 2 : 60;
            ESP_LOGW("signaling", "WebSocket disconnected");
        }
    }
}
