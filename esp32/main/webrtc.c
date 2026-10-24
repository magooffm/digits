#include "webrtc.h"
#include "sdkconfig.h"

#if CONFIG_DIGITS_WEBRTC

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_peer_default.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "call_audio.h"
#include "incoming_ring.h"
#include "call_tones.h"
#include "ringtones.h"
#include "line_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define MESSAGE_QUEUE_LENGTH 8
#define NUMBER_CAPACITY 32
#define MAX_ICE_SERVERS 8
#define ICE_URL_CAPACITY 256
#define MAX_PENDING_CANDIDATES 32
#define WORKER_STACK_BYTES (20 * 1024)
#define WORKER_PRIORITY 6
#define RING_TIMEOUT_US (60LL * 1000000)
#define CONNECT_TIMEOUT_US (10LL * 1000000) // Pi connectTimeout, v1.99.0.
#define AUDIO_FRAME_US 20000
// The caller's offer, with its ringtone choice, normally follows the ring
// within a few hundred milliseconds; ring with the standard tone if not.
#define RINGTONE_HINT_WAIT_US 500000LL

static const char *TAG = "webrtc";

typedef enum {
    EVENT_RING, EVENT_SDP, EVENT_ANSWER, EVENT_ICE, EVENT_HANGUP,
    EVENT_BUSY, EVENT_ERROR, EVENT_ICE_SERVERS, EVENT_ICE_RESTART,
    COMMAND_CALL, COMMAND_ANSWER, COMMAND_HANGUP, COMMAND_STATUS, COMMAND_AUDIO,
} event_kind_t;

typedef struct {
    event_kind_t kind;
    uint32_t session;
    uint32_t epoch;
    char from[NUMBER_CAPACITY];
    bool conference;
    char *data;
    char *servers[MAX_ICE_SERVERS];
    unsigned server_count;
    unsigned skipped_servers;
    unsigned ringtone_hint; // COMMAND_CALL: 0 none, otherwise ringtone + 1.
} event_t;

typedef struct {
    uint32_t session;
    uint32_t epoch;
    bool online;
    bool paired;
    char number[NUMBER_CAPACITY];
    bool overflow;
    char overflow_from[NUMBER_CAPACITY];
} transport_t;

typedef enum { CALL_IDLE, CALL_CALLING, CALL_RINGING, CALL_CONNECTING, CALL_CONNECTED } call_phase_t;

static struct {
    QueueHandle_t queue;
    TaskHandle_t task;
    digits_webrtc_send_fn send;
    void *send_context;
    transport_t owner_transport;
    char *ice_urls[MAX_ICE_SERVERS];
    unsigned ice_count;
    bool ice_received;
    digits_call_audio_mode_t audio_mode;
    digits_call_audio_t *stopping_audio;
    uint32_t next_ring_id;
    uint32_t ended_calls;
    digits_call_end_t last_end;
    bool last_connected;
    call_audio_packet_t send_packet;
    struct {
        esp_peer_handle_t peer;
        call_phase_t phase;
        char remote[NUMBER_CAPACITY];
        bool caller;
        bool answer_requested;
        unsigned ringtone_hint;  // Outgoing: 0 none, otherwise ringtone + 1.
        bool ring_started;       // Incoming: ringer started for this call.
        int64_t ring_hint_deadline_us;
        bool remote_sdp;
        bool local_sdp;
        bool closing;
        const char *failure;
        char *offer;
        char *remote_sdp_text; // Conservatively retain peer input until close.
        esp_peer_cfg_t config;
        esp_peer_default_cfg_t defaults;
        esp_peer_ice_server_cfg_t servers[MAX_ICE_SERVERS];
        char *server_urls[MAX_ICE_SERVERS];
        unsigned server_count;
        char *candidates[MAX_PENDING_CANDIDATES];
        unsigned candidate_count;
        int64_t deadline_us;
        int64_t connected_at_us;
        int64_t next_audio_us;
        int64_t next_report_us;
        uint32_t tx_packets;
        uint32_t rx_packets;
        uint32_t rx_bytes;
        uint32_t skipped_audio;
        uint32_t blocked_audio;
        bool audio_info;
        digits_call_audio_t *audio;
        bool sent_audio;
        uint32_t last_audio_pts;
        uint32_t max_peer_loop_us;
        uint32_t slow_peer_loops;
    } call;
} rtc;

static portMUX_TYPE transport_mux = portMUX_INITIALIZER_UNLOCKED;
static transport_t latest_transport;
static portMUX_TYPE call_state_mux = portMUX_INITIALIZER_UNLOCKED;
static digits_webrtc_call_state_t published_call_state;

_Static_assert(CALL_CONNECTED == (call_phase_t)DIGITS_CALL_CONNECTED, "public phase order");

static const char *phase_name(call_phase_t phase)
{
    switch (phase) {
    case CALL_IDLE: return "idle";
    case CALL_CALLING: return "calling";
    case CALL_RINGING: return "ringing";
    case CALL_CONNECTING: return "connecting";
    case CALL_CONNECTED: return "connected";
    }
    return "unknown";
}

static const char *audio_mode_name(digits_call_audio_mode_t mode)
{
    return mode == DIGITS_CALL_AUDIO_TX ? "tx" : mode == DIGITS_CALL_AUDIO_RX ? "rx" : "full";
}

static void report_resources(const char *where);

static void report_call_audio(digits_call_audio_t *audio)
{
    if (!audio) return;
    digits_call_audio_stats_t s;
    digits_call_audio_get_stats(audio, &s);
    ESP_LOGI(TAG, "Call audio state=%u error=%s capture=%" PRIu32 " gaps=%" PRIu32 " discontinuities=%" PRIu32 " stale=%" PRIu32 " timeouts=%" PRIu32 " encoded=%" PRIu32 " encode_errors=%" PRIu32,
             (unsigned)s.state, esp_err_to_name(s.error), s.capture_frames, s.capture_gaps,
             s.capture_discontinuities, s.capture_stale, s.capture_timeouts, s.encoded_frames, s.encode_errors);
    ESP_LOGI(TAG, "Audio TX sent=%" PRIu32 " queue_drops=%" PRIu32 " pop_drops=%" PRIu32 " blocked=%" PRIu32 " errors=%" PRIu32 " max_encode_us=%" PRIu32 " max_capture_age_us=%" PRIu32 " max_send_age_us=%" PRIu32 " stack_min=%" PRIu32,
             s.sent_frames, s.tx_queue_drops, s.tx_pop_drops, s.send_blocked, s.send_errors,
             s.max_encode_us, s.max_capture_age_us, s.max_send_age_us, s.tx_stack_min_bytes);
    ESP_LOGI(TAG, "Audio RX received=%" PRIu32 " queue_drops=%" PRIu32 " queue_stale=%" PRIu32 " jitter_overruns=%" PRIu32 " late=%" PRIu32 " duplicate=%" PRIu32 " missing_pts=%" PRIu32 " rebases=%" PRIu32 " local_skips=%" PRIu32,
             s.received_frames, s.rx_queue_drops, s.rx_queue_stale, s.rx_jitter_overruns,
             s.rx_late, s.rx_duplicate, s.rx_missing,
             s.rx_discontinuities, s.rx_local_skips);
    ESP_LOGI(TAG, "Audio playback decoded=%" PRIu32 " decode_errors=%" PRIu32 " plc=%" PRIu32 " silence=%" PRIu32 " played=%" PRIu32 " write_errors=%" PRIu32 " producer_gaps=%" PRIu32 " max_decode_us=%" PRIu32 " max_receive_age_us=%" PRIu32 " max_write_us=%" PRIu32 " stack_min=%" PRIu32,
             s.decoded_frames, s.decode_errors, s.plc_frames, s.silence_frames, s.played_frames,
             s.playback_errors, s.playback_feed_gaps, s.max_decode_us, s.max_receive_age_us,
             s.max_write_us, s.rx_stack_min_bytes);
}

static void reap_audio(void)
{
    if (rtc.stopping_audio && digits_call_audio_is_stopped(rtc.stopping_audio)) {
        report_call_audio(rtc.stopping_audio);
        if (digits_call_audio_destroy(rtc.stopping_audio) == ESP_OK) {
            rtc.stopping_audio = NULL;
            ESP_LOGI(TAG, "Call audio workers released; microphone and speaker available");
            report_resources("Audio released");
        }
    }
    digits_settings_set_media_busy(rtc.call.phase != CALL_IDLE || rtc.stopping_audio ||
                                   !digits_incoming_ring_is_stopped());
}

static const char *peer_state_name(esp_peer_state_t state)
{
    switch (state) {
    case ESP_PEER_STATE_CLOSED: return "CLOSED";
    case ESP_PEER_STATE_DISCONNECTED: return "DISCONNECTED";
    case ESP_PEER_STATE_NEW_CONNECTION: return "NEW_CONNECTION";
    case ESP_PEER_STATE_CANDIDATE_GATHERING: return "CANDIDATE_GATHERING";
    case ESP_PEER_STATE_PAIRING: return "PAIRING";
    case ESP_PEER_STATE_PAIRED: return "PAIRED (ICE selected)";
    case ESP_PEER_STATE_CONNECTING: return "CONNECTING (DTLS)";
    case ESP_PEER_STATE_CONNECTED: return "CONNECTED (DTLS/SRTP)";
    case ESP_PEER_STATE_CONNECT_FAILED: return "CONNECT_FAILED";
    case ESP_PEER_STATE_REMOTE_AUDIO_TRACK_ADDED: return "REMOTE_AUDIO_TRACK_ADDED";
    case ESP_PEER_STATE_REMOTE_AUDIO_TRACK_REMOVED: return "REMOTE_AUDIO_TRACK_REMOVED";
    default: return "other";
    }
}

static void *audio_heap_alloc(size_t size)
{
    // No DMA uses these signaling copies. PSRAM is optional, internal fallback
    // is bounded by the message, queue and candidate limits below.
    return heap_caps_malloc_prefer(size, 2,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static char *copy_text(const char *text, size_t maximum)
{
    if (!text) return NULL;
    size_t length = strnlen(text, maximum + 1);
    if (length > maximum) return NULL;
    char *copy = audio_heap_alloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

static bool copy_number(char destination[NUMBER_CAPACITY], const char *text)
{
    if (!text || !text[0] || strnlen(text, NUMBER_CAPACITY) >= NUMBER_CAPACITY) return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    strcpy(destination, text);
    return true;
}

static const char *json_text(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static bool supported_stun_url(const char *url)
{
    if (!url || strncmp(url, "stun:", 5) != 0 || !url[5] ||
        strnlen(url, ICE_URL_CAPACITY) >= ICE_URL_CAPACITY) return false;
    // The first milestone deliberately uses IPv4 UDP. Do not silently convert
    // stuns/TURN, IPv6 literals or a transport=tcp request into a UDP fallback.
    const char *host = url + 5;
    const char *port = NULL;
    for (const char *p = host; *p; ++p) {
        if (*p == ':') {
            if (port) return false;
            port = p + 1;
        } else if ((*p < 'a' || *p > 'z') && (*p < 'A' || *p > 'Z') &&
                   (*p < '0' || *p > '9') && *p != '.' && *p != '-') return false;
    }
    if (port) {
        if (port == host + 1 || !port[0]) return false;
        unsigned value = 0;
        for (const char *p = port; *p; ++p) {
            if (*p < '0' || *p > '9') return false;
            value = value * 10 + (*p - '0');
            if (value > 65535) return false;
        }
        if (!value) return false;
    }
    return true;
}

static transport_t transport_snapshot(void)
{
    transport_t snapshot;
    portENTER_CRITICAL(&transport_mux);
    snapshot = latest_transport;
    portEXIT_CRITICAL(&transport_mux);
    return snapshot;
}

static bool current_transport(void)
{
    transport_t now = transport_snapshot();
    return now.online && now.paired &&
           now.epoch == rtc.owner_transport.epoch &&
           now.session == rtc.owner_transport.session;
}

static void queue_fault(uint32_t epoch, const char *from)
{
    portENTER_CRITICAL(&transport_mux);
    if (latest_transport.epoch == epoch) {
        latest_transport.overflow = true;
        if (from && strnlen(from, NUMBER_CAPACITY) < NUMBER_CAPACITY)
            strcpy(latest_transport.overflow_from, from);
    }
    portEXIT_CRITICAL(&transport_mux);
    if (rtc.task) xTaskNotifyGive(rtc.task);
}

static void free_event(event_t *event)
{
    if (!event) return;
    free(event->data);
    for (unsigned i = 0; i < event->server_count; ++i) free(event->servers[i]);
    free(event);
}

static esp_err_t enqueue(event_t *event)
{
    if (xQueueSend(rtc.queue, &event, 0) != pdTRUE) {
        queue_fault(event->epoch, event->from);
        free_event(event);
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(rtc.task);
    return ESP_OK;
}

static esp_err_t send_wire(const char *type, const char *to,
                           const char *field, const char *data,
                           const char *reason)
{
    if (!current_transport()) return ESP_ERR_INVALID_STATE;
    cJSON *message = cJSON_CreateObject();
    if (!message) return ESP_ERR_NO_MEM;
    bool ok = cJSON_AddStringToObject(message, "type", type) &&
              cJSON_AddNumberToObject(message, "voicemail_unheard_count", 0);
    if (to && to[0]) ok = ok && cJSON_AddStringToObject(message, "to", to);
    if (field) ok = ok && cJSON_AddStringToObject(message, field, data);
    if (reason) ok = ok && cJSON_AddStringToObject(message, "reason", reason);
    esp_err_t result = ok ? rtc.send(message, rtc.owner_transport.session, rtc.send_context)
                          : ESP_ERR_NO_MEM;
    cJSON_Delete(message);
    if (result != ESP_OK)
        ESP_LOGW(TAG, "Could not queue %s for session=%" PRIu32 ": %s",
                 type, rtc.owner_transport.session, esp_err_to_name(result));
    return result;
}

static void report_resources(const char *where)
{
    ESP_LOGI(TAG, "%s: phase=%s internal_free=%u internal_min=%u internal_largest=%u PSRAM_free=%u stack_free=%u bytes",
             where, phase_name(rtc.call.phase),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    wifi_ap_record_t access_point;
    wifi_ps_type_t power_save;
    if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK &&
        esp_wifi_get_ps(&power_save) == ESP_OK)
        ESP_LOGI(TAG, "Wi-Fi RSSI=%d dBm power_save=%u", access_point.rssi, (unsigned)power_save);
}

static digits_call_end_t classify_end(const char *reason)
{
    if (!reason) return DIGITS_CALL_END_LOCAL;
    if (!strcmp(reason, "local hangup")) return DIGITS_CALL_END_LOCAL;
    if (!strcmp(reason, "remote hangup")) return DIGITS_CALL_END_REMOTE_HANGUP;
    if (!strcmp(reason, "remote busy")) return DIGITS_CALL_END_BUSY;
    if (!strcmp(reason, "server error")) return DIGITS_CALL_END_UNREACHABLE;
    return DIGITS_CALL_END_FAILED;
}

static void end_call(bool notify, const char *reason)
{
    if (rtc.call.phase == CALL_IDLE && !rtc.call.peer) return;
    ++rtc.ended_calls;
    rtc.last_end = classify_end(reason);
    rtc.last_connected = rtc.call.phase == CALL_CONNECTED;
    digits_incoming_ring_stop();
    ESP_LOGI(TAG, "Ending one-to-one call peer=%s reason=%s tx=%" PRIu32 " rx=%" PRIu32,
             rtc.call.remote, reason ? reason : "local", rtc.call.tx_packets, rtc.call.rx_packets);
    if (notify && rtc.call.remote[0])
        send_wire("hangup", rtc.call.remote, NULL, NULL,
                  reason && strcmp(reason, "connect_timeout") == 0 ? reason : NULL);
    rtc.call.closing = true; // Close callbacks must never recursively close.
    if (rtc.call.audio) {
        digits_call_audio_request_stop(rtc.call.audio);
        rtc.stopping_audio = rtc.call.audio;
        rtc.call.audio = NULL;
    }
    if (rtc.call.peer) {
        int ret = esp_peer_close(rtc.call.peer);
        if (ret != ESP_PEER_ERR_NONE) ESP_LOGW(TAG, "esp_peer_close failed: %d", ret);
    }
    free(rtc.call.offer);
    free(rtc.call.remote_sdp_text);
    for (unsigned i = 0; i < rtc.call.server_count; ++i) free(rtc.call.server_urls[i]);
    for (unsigned i = 0; i < rtc.call.candidate_count; ++i) free(rtc.call.candidates[i]);
    memset(&rtc.call, 0, sizeof(rtc.call));
    report_resources("Peer released");
}

static void end_failed_call(void)
{
    // Remote transport closure can precede its signaling hangup. Once media
    // connected, retain the local diagnostic without failed-call treatment.
    end_call(true, rtc.call.phase == CALL_CONNECTED ? rtc.call.failure : "connect_timeout");
}

static int on_peer_state(esp_peer_state_t state, void *context)
{
    (void)context;
    ESP_LOGI(TAG, "Peer state=%s (%d)", peer_state_name(state), (int)state);
    if (rtc.call.closing || rtc.call.phase == CALL_IDLE) return 0;
    if (!current_transport()) {
        rtc.call.failure = "signaling session changed";
        return 0;
    }
    if (state == ESP_PEER_STATE_CONNECTED) {
        if (rtc.call.phase == CALL_CONNECTED) return 0;
        rtc.call.phase = CALL_CONNECTED;
        rtc.call.deadline_us = 0;
        rtc.call.connected_at_us = esp_timer_get_time();
        rtc.call.next_audio_us = rtc.call.connected_at_us;
        rtc.call.next_report_us = rtc.call.connected_at_us + 1000000;
        // Ringback may still hold the speaker; call audio waits for release.
        digits_call_tones_stop();
        ESP_LOGI(TAG, "ICE and DTLS/SRTP connected; received media is still awaiting verification");
    } else if (state == ESP_PEER_STATE_DISCONNECTED || state == ESP_PEER_STATE_CONNECT_FAILED ||
               state == ESP_PEER_STATE_CLOSED) {
        rtc.call.failure = "peer transport failed";
    }
    return 0;
}

// Session-level attribute before the first media section. Returns a new
// allocation, or NULL if it does not fit the SDP limit or memory runs out.
static char *sdp_add_ringtone(const char *sdp, unsigned ringtone)
{
    char line[48];
    int line_length = snprintf(line, sizeof(line), DIGITS_RINGTONE_SDP_ATTR "%u\r\n", ringtone);
    size_t length = strlen(sdp);
    if (line_length <= 0 || length + (size_t)line_length > DIGITS_WEBRTC_MAX_SDP) return NULL;
    const char *media = strstr(sdp, "\nm=");
    size_t at = media ? (size_t)(media - sdp) + 1 : length;
    char *out = audio_heap_alloc(length + (size_t)line_length + 1);
    if (!out) return NULL;
    memcpy(out, sdp, at);
    memcpy(out + at, line, (size_t)line_length);
    memcpy(out + at + line_length, sdp + at, length - at + 1);
    return out;
}

// Removes every ringtone attribute line in place so the peer library never
// sees it. Returns the first valid ringtone, or -1 if none was offered.
static int sdp_take_ringtone(char *sdp)
{
    static const char attribute[] = DIGITS_RINGTONE_SDP_ATTR;
    int ringtone = -1;
    char *line = sdp;
    while (line && *line) {
        char *end = strchr(line, '\n');
        char *next = end ? end + 1 : line + strlen(line);
        if (!strncmp(line, attribute, sizeof(attribute) - 1)) {
            char *value = line + sizeof(attribute) - 1;
            unsigned parsed = 0;
            bool digits = value < next && *value >= '0' && *value <= '9';
            while (value < next && *value >= '0' && *value <= '9' && parsed < 1000)
                parsed = parsed * 10 + (unsigned)(*value++ - '0');
            if (value < next && *value == '\r') ++value;
            bool clean = value == next || (*value == '\n' && value + 1 == next);
            if (ringtone < 0 && digits && clean && ringtones_valid(parsed)) ringtone = (int)parsed;
            memmove(line, next, strlen(next) + 1);
            continue; // The following line now starts here.
        }
        line = end ? next : NULL;
    }
    return ringtone;
}

static digits_ringtone_resolver_fn ringtone_resolver;

void digits_webrtc_set_ringtone_resolver(digits_ringtone_resolver_fn resolver)
{
    ringtone_resolver = resolver; // Boot-time, before calls.
}

// offered: the caller's choice from the SDP, or -1.
static void start_incoming_ring(int offered)
{
    if (rtc.call.phase != CALL_RINGING || rtc.call.ring_started || rtc.call.answer_requested) return;
    rtc.call.ring_started = true;
    unsigned ringtone = ringtone_resolver ? ringtone_resolver(rtc.call.remote, offered)
                        : offered >= 0 ? (unsigned)offered : RINGTONE_STANDARD;
    if (ringtone != (unsigned)offered)
        ESP_LOGI(TAG, "Ringtone %u chosen on this phone (caller offered %d)", ringtone, offered);
    if (++rtc.next_ring_id == 0) ++rtc.next_ring_id;
    digits_incoming_ring_start(rtc.next_ring_id, ringtone);
}

static int on_peer_message(esp_peer_msg_t *message, void *context)
{
    (void)context;
    if (rtc.call.closing || rtc.call.phase == CALL_IDLE) return 0;
    if (rtc.call.failure) return ESP_PEER_ERR_FAIL;
    if (!message || !message->data || !current_transport() || rtc.call.closing) {
        rtc.call.failure = "invalid local peer message or stale session";
        return ESP_PEER_ERR_FAIL;
    }
    size_t maximum = message->type == ESP_PEER_MSG_TYPE_SDP ? DIGITS_WEBRTC_MAX_SDP : DIGITS_WEBRTC_MAX_ICE;
    if (message->size <= 0 || (size_t)message->size > maximum + 1) {
        rtc.call.failure = "invalid local peer message size";
        return ESP_PEER_ERR_BAD_DATA;
    }
    size_t length = message->size;
    if (message->data[length - 1] == '\0') --length;
    if (!length || length > maximum || memchr(message->data, '\0', length)) {
        rtc.call.failure = "invalid local peer message text";
        return ESP_PEER_ERR_BAD_DATA;
    }
    char *text = audio_heap_alloc(length + 1);
    if (!text) {
        rtc.call.failure = "local peer message copy failed";
        return ESP_PEER_ERR_NO_MEM;
    }
    memcpy(text, message->data, length);
    text[length] = '\0';
    if (message->type == ESP_PEER_MSG_TYPE_SDP) {
        if (rtc.call.local_sdp) {
            rtc.call.failure = "invalid or repeated local SDP";
            free(text);
            return ESP_PEER_ERR_BAD_DATA;
        }
        const char *type = rtc.call.caller ? "sdp" : "answer";
        if (rtc.call.caller && rtc.call.ringtone_hint) {
            char *tagged = sdp_add_ringtone(text, rtc.call.ringtone_hint - 1);
            if (tagged) {
                free(text);
                text = tagged;
                length = strlen(text);
            } else {
                ESP_LOGW(TAG, "Ringtone choice not added to offer; callee uses its standard ring");
            }
        }
        if (send_wire(type, rtc.call.remote, "sdp", text, NULL) != ESP_OK) {
            rtc.call.failure = "local SDP transmit queue failed";
            free(text);
            return ESP_PEER_ERR_FAIL;
        }
        rtc.call.local_sdp = true;
        if (!rtc.call.caller) rtc.call.deadline_us = esp_timer_get_time() + CONNECT_TIMEOUT_US;
        ESP_LOGI(TAG, "Queued %s: %u SDP bytes (local candidates included); awaiting peer transport",
                 type, (unsigned)length);
        free(text);
        return 0;
    }
    if (message->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        // The pinned default implementation embeds local candidates in SDP.
        // Preserve the wire form if a future implementation also trickles.
        const char *candidate = text;
        if (!rtc.call.local_sdp || strncmp(candidate, "candidate:", 10) != 0 ||
            strnlen(candidate, DIGITS_WEBRTC_MAX_ICE + 1) > DIGITS_WEBRTC_MAX_ICE ||
            send_wire("ice", rtc.call.remote, "candidate", candidate, NULL) != ESP_OK) {
            rtc.call.failure = "invalid local trickle candidate or queue failure";
            free(text);
            return ESP_PEER_ERR_FAIL;
        }
    }
    free(text);
    return 0;
}

static int on_audio_info(esp_peer_audio_stream_info_t *info, void *context)
{
    (void)context;
    if (rtc.call.closing || rtc.call.phase == CALL_IDLE) return 0;
    if (!info || info->codec != ESP_PEER_AUDIO_CODEC_OPUS || info->sample_rate != 48000 ||
        (info->channel != 1 && info->channel != 2)) {
        rtc.call.failure = "unsupported negotiated audio codec/format";
        return ESP_PEER_ERR_NOT_SUPPORT;
    }
    rtc.call.audio_info = true;
    ESP_LOGI(TAG, "Negotiated audio: Opus, %" PRIu32 " Hz, channels=%u (encoded packets; local PCM remains mono)",
             info->sample_rate, (unsigned)info->channel);
    return 0;
}

static int on_audio_data(esp_peer_audio_frame_t *frame, void *context)
{
    (void)context;
    if (!frame || !frame->data || frame->size <= 0 || frame->size > 1500 ||
        rtc.call.phase != CALL_CONNECTED || !current_transport()) return 0;
    ++rtc.call.rx_packets;
    rtc.call.rx_bytes += frame->size;
    if (rtc.call.rx_packets == 1)
        ESP_LOGI(TAG, "First remote encoded audio packet received after SRTP processing (%d bytes)",
                 frame->size);
    // Encoded frames are owned by esp_peer and valid only during this callback.
    if (rtc.call.audio && (rtc.audio_mode & DIGITS_CALL_AUDIO_RX))
        digits_call_audio_receive(rtc.call.audio, frame->pts, frame->data, frame->size);
    return 0;
}

static esp_err_t create_peer(bool caller)
{
    // Keep all supplied configuration and strings stable through peer close;
    // a later server ICE refresh must not free a live peer's configuration.
    rtc.call.server_count = rtc.ice_count;
    for (unsigned i = 0; i < rtc.ice_count; ++i) {
        rtc.call.server_urls[i] = copy_text(rtc.ice_urls[i], ICE_URL_CAPACITY - 1);
        if (!rtc.call.server_urls[i]) return ESP_ERR_NO_MEM;
        rtc.call.servers[i].stun_url = rtc.call.server_urls[i];
    }
    rtc.call.defaults = (esp_peer_default_cfg_t){
        .agent_recv_timeout = 5,
        .keep_role = true,
        .max_candidates = 8,
        .rtp_cfg = {
            .audio_recv_jitter = {.cache_size = 8192, .cache_timeout = 100, .resend_delay = 20},
            .send_pool_size = 8192,
            .send_queue_num = 16,
            .max_resend_count = 3,
        },
    };
    rtc.call.config = (esp_peer_cfg_t){
        .server_lists = rtc.ice_count ? rtc.call.servers : NULL,
        .server_num = rtc.ice_count,
        .role = caller ? ESP_PEER_ROLE_CONTROLLING : ESP_PEER_ROLE_CONTROLLED,
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .audio_info = {.codec = ESP_PEER_AUDIO_CODEC_OPUS, .sample_rate = 48000, .channel = 1},
        .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .no_auto_reconnect = true,
        .enable_data_channel = false,
        .extra_cfg = &rtc.call.defaults,
        .extra_size = sizeof(rtc.call.defaults),
        .on_state = on_peer_state,
        .on_msg = on_peer_message,
        .on_audio_info = on_audio_info,
        .on_audio_data = on_audio_data,
    };
    if (!rtc.ice_count)
        ESP_LOGW(TAG, "No usable STUN configuration%s; host candidates only for this LAN test",
                 rtc.ice_received ? " in server response" : " received yet");
    report_resources("Before peer creation");
    int ret = esp_peer_open(&rtc.call.config, esp_peer_get_default_impl(), &rtc.call.peer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", ret);
        return ret == ESP_PEER_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    // Both roles must enter NEW_CONNECTION. Controlled waits for remote SDP.
    ret = esp_peer_new_connection(rtc.call.peer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_new_connection failed: %d", ret);
        return ESP_FAIL;
    }
    report_resources("Peer created");
    return ESP_OK;
}

static bool apply_remote_sdp(const char *sdp)
{
    if (rtc.call.remote_sdp_text) {
        rtc.call.failure = "remote SDP renegotiation is outside this milestone";
        return false;
    }
    rtc.call.remote_sdp_text = copy_text(sdp, DIGITS_WEBRTC_MAX_SDP);
    if (!rtc.call.remote_sdp_text) {
        rtc.call.failure = "remote SDP copy failed";
        return false;
    }
    esp_peer_msg_t message = {.type = ESP_PEER_MSG_TYPE_SDP,
                             .data = (uint8_t *)rtc.call.remote_sdp_text,
                             .size = strlen(rtc.call.remote_sdp_text)};
    int ret = esp_peer_send_msg(rtc.call.peer, &message);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "Remote SDP rejected: %d", ret);
        rtc.call.failure = "remote SDP rejected";
        return false;
    }
    rtc.call.remote_sdp = true;
    ESP_LOGI(TAG, "Remote SDP applied: %u bytes; this is not a connected peer", (unsigned)message.size);
    for (unsigned i = 0; i < rtc.call.candidate_count; ++i) {
        char *candidate = rtc.call.candidates[i];
        esp_peer_msg_t ice = {.type = ESP_PEER_MSG_TYPE_CANDIDATE, .data = (uint8_t *)candidate, .size = strlen(candidate)};
        ret = esp_peer_send_msg(rtc.call.peer, &ice);
        free(candidate);
        rtc.call.candidates[i] = NULL;
        if (ret != ESP_PEER_ERR_NONE) rtc.call.failure = "queued ICE candidate rejected";
    }
    ESP_LOGI(TAG, "Drained %u buffered remote ICE candidates", rtc.call.candidate_count);
    rtc.call.candidate_count = 0;
    return !rtc.call.failure;
}

static void answer_incoming(void)
{
    if (rtc.call.phase != CALL_RINGING) {
        ESP_LOGW(TAG, "No incoming call to answer");
        return;
    }
    bool first_request = !rtc.call.answer_requested;
    rtc.call.answer_requested = true;
    digits_incoming_ring_stop();
    if (!rtc.call.offer) {
        if (first_request) ESP_LOGI(TAG, "Answer requested; waiting for caller SDP");
        return;
    }
    if (!digits_incoming_ring_is_stopped()) {
        if (first_request) ESP_LOGI(TAG, "Answer requested; waiting for ringtone speaker release");
        return;
    }
    rtc.call.phase = CALL_CONNECTING;
    rtc.call.deadline_us = esp_timer_get_time() + CONNECT_TIMEOUT_US;
    if (create_peer(false) != ESP_OK || !apply_remote_sdp(rtc.call.offer)) {
        end_call(true, "connect_timeout");
        return;
    }
    free(rtc.call.offer);
    rtc.call.offer = NULL;
}

static void receive_ice(event_t *event)
{
    if (!event->data || !event->data[0]) {
        ESP_LOGI(TAG, "Remote ICE gathering complete (empty candidate marker)");
        return;
    }
    if (strncmp(event->data, "candidate:", 10) != 0) {
        ESP_LOGW(TAG, "Ignoring ICE with unsupported representation; expected bare candidate: attribute");
        return;
    }
    if (rtc.call.peer && rtc.call.remote_sdp) {
        esp_peer_msg_t message = {.type = ESP_PEER_MSG_TYPE_CANDIDATE,
                                 .data = (uint8_t *)event->data, .size = strlen(event->data)};
        if (esp_peer_send_msg(rtc.call.peer, &message) != ESP_PEER_ERR_NONE)
            rtc.call.failure = "remote ICE candidate rejected";
    } else if (rtc.call.candidate_count < MAX_PENDING_CANDIDATES) {
        rtc.call.candidates[rtc.call.candidate_count++] = event->data;
        event->data = NULL;
        ESP_LOGI(TAG, "Buffered remote ICE until remote SDP: %u/%u",
                 rtc.call.candidate_count, MAX_PENDING_CANDIDATES);
    } else {
        rtc.call.failure = "remote ICE buffer exhausted";
    }
}

static void clear_ice_servers(void)
{
    for (unsigned i = 0; i < rtc.ice_count; ++i) free(rtc.ice_urls[i]);
    memset(rtc.ice_urls, 0, sizeof(rtc.ice_urls));
    rtc.ice_count = 0;
    rtc.ice_received = false;
}

static void process_event(event_t *event)
{
    if (event->epoch != rtc.owner_transport.epoch || event->session != rtc.owner_transport.session) return;
    if (event->kind == COMMAND_STATUS) {
        digits_line_settings_t settings;
        digits_settings_stats_t settings_stats;
        digits_settings_get(&settings, &settings_stats);
        ESP_LOGI(TAG, "Status: signaling=%s paired=%s peer=%s codec_info=%s tx=%" PRIu32 " rx=%" PRIu32,
                 rtc.owner_transport.online ? "online" : "offline",
                 rtc.owner_transport.paired ? "yes" : "no", phase_name(rtc.call.phase),
                 rtc.call.audio_info ? "received" : "pending", rtc.call.tx_packets, rtc.call.rx_packets);
        report_resources("Status");
        ESP_LOGI(TAG, "Audio mode=%s cleanup_pending=%s peer_loop_max_us=%" PRIu32 " peer_loops_over_20ms=%" PRIu32,
                 audio_mode_name(rtc.audio_mode), rtc.stopping_audio ||
                 (rtc.call.phase != CALL_RINGING && !digits_incoming_ring_is_stopped()) ? "yes" : "no",
                 rtc.call.max_peer_loop_us, rtc.call.slow_peer_loops);
        ESP_LOGI(TAG, "Settings: effective_silent=%s voice_style=%s auto_update=%s voicemail=%s timeout=%us (voice/auto/voicemail cached only)",
                 settings.silent_mode ? "yes" : "no",
                 settings.voice_style == DIGITS_VOICE_MODERN ? "modern" : "copper",
                 settings.auto_update ? "yes" : "no", settings.voicemail_enabled ? "yes" : "no",
                 (unsigned)settings.voicemail_ring_timeout_seconds);
        ESP_LOGI(TAG, "Settings storage: loaded=%s dirty=%s revision=%" PRIu32 " writes=%" PRIu32 " errors=%" PRIu32 " load=%s last_write=%s; ringtone_worker_busy=%s",
                 settings_stats.loaded ? "yes" : "no", settings_stats.dirty ? "yes" : "no",
                 settings_stats.revision, settings_stats.writes, settings_stats.write_errors,
                 esp_err_to_name(settings_stats.load_error), esp_err_to_name(settings_stats.last_write_error),
                 digits_incoming_ring_is_stopped() ? "no" : "yes");
        ESP_LOGI(TAG, "Settings receive: accepted=%" PRIu32 " duplicates=%" PRIu32 " invalid=%" PRIu32 " stale=%" PRIu32 " persistence_deferred=%s",
                 settings_stats.accepted, settings_stats.duplicates, settings_stats.invalid,
                 settings_stats.stale, settings_stats.media_busy ? "yes" : "no");
        report_call_audio(rtc.call.audio ? rtc.call.audio : rtc.stopping_audio);
        if (rtc.call.peer) esp_peer_query(rtc.call.peer);
        return;
    }
    if (event->kind == COMMAND_AUDIO) {
        if (rtc.call.phase != CALL_IDLE || rtc.stopping_audio || !digits_incoming_ring_is_stopped()) {
            ESP_LOGW(TAG, "Audio mode change requires idle with completed cleanup");
            return;
        }
        rtc.audio_mode = !strcmp(event->from, "tx") ? DIGITS_CALL_AUDIO_TX :
                         !strcmp(event->from, "rx") ? DIGITS_CALL_AUDIO_RX : DIGITS_CALL_AUDIO_FULL;
        ESP_LOGI(TAG, "Next call audio mode=%s", audio_mode_name(rtc.audio_mode));
        return;
    }
    if (!current_transport()) return;
    if (event->kind == EVENT_ICE_SERVERS) {
        clear_ice_servers();
        rtc.ice_received = true;
        rtc.ice_count = event->server_count;
        for (unsigned i = 0; i < event->server_count; ++i) {
            rtc.ice_urls[i] = event->servers[i];
            event->servers[i] = NULL;
        }
        ESP_LOGI(TAG, "Server ICE configuration: %u STUN URLs cached for next peer, %u unsupported/over-limit URLs skipped",
                 rtc.ice_count, event->skipped_servers);
        if (event->skipped_servers)
            ESP_LOGW(TAG, "Initial interoperability scope uses IPv4 UDP host/STUN; TURN/TURNS and TCP relay are not enabled");
        return;
    }
    if (event->conference) {
        // Server ordinary hangup is line-wide. Reject this unsupported message
        // by ignoring it, preserving an already active ordinary peer.
        ESP_LOGW(TAG, "Ignoring conference signaling outside this milestone");
        return;
    }
    if (event->kind == COMMAND_CALL) {
        if (rtc.call.phase != CALL_IDLE || rtc.stopping_audio || !digits_incoming_ring_is_stopped() ||
            strcmp(event->from, rtc.owner_transport.number) == 0) {
            ESP_LOGW(TAG, "Call requires an idle device and a different destination");
            return;
        }
        strcpy(rtc.call.remote, event->from);
        rtc.call.ringtone_hint = event->ringtone_hint;
        digits_settings_set_media_busy(true);
        rtc.call.phase = CALL_CALLING;
        rtc.call.caller = true;
        rtc.call.deadline_us = esp_timer_get_time() + RING_TIMEOUT_US;
        // Server tracks the call before it will relay the following SDP.
        if (send_wire("call", rtc.call.remote, NULL, NULL, NULL) != ESP_OK || create_peer(true) != ESP_OK)
            end_call(true, "call setup failed");
        else ESP_LOGI(TAG, "Outgoing call to %s; local development ringing deadline=60s", rtc.call.remote);
        return;
    }
    if (event->kind == COMMAND_ANSWER) { answer_incoming(); return; }
    if (event->kind == COMMAND_HANGUP) { end_call(true, "local hangup"); return; }
    if (event->kind == EVENT_RING) {
        if (!event->from[0]) return;
        if (rtc.call.phase != CALL_IDLE || rtc.stopping_audio || !digits_incoming_ring_is_stopped()) {
            // Upstream ordinary hangup ends every call for this line; To does
            // not scope it. Never reject a foreign ring by hanging up this call.
            if (strcmp(event->from, rtc.call.remote) != 0)
                ESP_LOGW(TAG, "Ignoring another ring while the single peer is active");
            return;
        }
        strcpy(rtc.call.remote, event->from);
        digits_settings_set_media_busy(true);
        rtc.call.phase = CALL_RINGING;
        rtc.call.deadline_us = esp_timer_get_time() + RING_TIMEOUT_US;
        ESP_LOGI(TAG, "Incoming call from %s; use 'webrtc answer' or 'webrtc hangup'",
                 rtc.call.remote);
        // Ring once the offer reveals the caller's ringtone, or on timeout.
        rtc.call.ring_hint_deadline_us = esp_timer_get_time() + RINGTONE_HINT_WAIT_US;
#if CONFIG_DIGITS_WEBRTC_AUTO_ANSWER
        answer_incoming();
#endif
        return;
    }
    if (rtc.call.phase == CALL_IDLE) return;
    // Admin hangup and errors may omit from. All peer media requires it.
    if (event->from[0] && strcmp(event->from, rtc.call.remote) != 0) {
        ESP_LOGW(TAG, "Ignoring signaling from a different peer");
        return;
    }
    if (!event->from[0] && event->kind != EVENT_HANGUP && event->kind != EVENT_ERROR) return;
    switch (event->kind) {
    case EVENT_SDP:
        if (rtc.call.caller || rtc.call.phase != CALL_RINGING || !event->data || !event->data[0] || rtc.call.offer) {
            rtc.call.failure = "unexpected/repeated SDP offer";
            break;
        }
        rtc.call.offer = event->data;
        event->data = NULL;
        {
            int ringtone = sdp_take_ringtone(rtc.call.offer);
            ESP_LOGI(TAG, "Caller SDP retained until answer (%u bytes); caller ringtone=%d",
                     (unsigned)strlen(rtc.call.offer), ringtone);
            start_incoming_ring(ringtone);
        }
        if (rtc.call.answer_requested) answer_incoming();
        break;
    case EVENT_ANSWER:
        if (!rtc.call.caller || rtc.call.phase != CALL_CALLING || !rtc.call.local_sdp || !event->data || !event->data[0]) {
            rtc.call.failure = "unexpected/repeated SDP answer";
            break;
        }
        rtc.call.phase = CALL_CONNECTING;
        rtc.call.deadline_us = esp_timer_get_time() + CONNECT_TIMEOUT_US;
        apply_remote_sdp(event->data);
        break;
    case EVENT_ICE: receive_ice(event); break;
    case EVENT_HANGUP: end_call(false, "remote hangup"); break;
    case EVENT_BUSY: end_call(false, "remote busy"); break;
    case EVENT_ERROR:
        // The signaling dispatcher prints the exact server error independently.
        end_call(true, "server error");
        break;
    case EVENT_ICE_RESTART:
        ESP_LOGW(TAG, "ICE restart is outside this milestone; ending the affected call");
        end_call(true, "connect_timeout");
        break;
    default: break;
    }
}

static void service_audio(int64_t now)
{
    (void)now;
    if (rtc.call.phase != CALL_CONNECTED || !current_transport()) return;
    // A stopped ringer must acknowledge all outstanding speaker operations
    // before a codec worker can claim the output. Never wait in the peer task.
    if (!digits_incoming_ring_is_stopped()) return;
    if (!rtc.call.audio) {
        // Same rule for call-progress tones; only the start waits.
        if (!digits_call_tones_is_stopped()) return;
        esp_err_t err = digits_call_audio_start(rtc.audio_mode, &rtc.call.audio);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Call audio start failed: %s", esp_err_to_name(err));
            rtc.call.failure = "call audio start failed";
            return;
        }
    }
    digits_call_audio_stats_t stats;
    digits_call_audio_get_stats(rtc.call.audio, &stats);
    if (stats.state == DIGITS_CALL_AUDIO_FAILED) {
        ESP_LOGE(TAG, "Call audio worker failed: %s", esp_err_to_name(stats.error));
        rtc.call.failure = "call audio worker failed";
        return;
    }
    if (stats.state != DIGITS_CALL_AUDIO_READY || !(rtc.audio_mode & DIGITS_CALL_AUDIO_TX) ||
        !digits_call_audio_pop_encoded(rtc.call.audio, &rtc.send_packet)) return;
    call_audio_packet_t *packet = &rtc.send_packet;
    esp_peer_audio_frame_t frame = {.pts = packet->pts_ms, .data = packet->data, .size = packet->size};
    int ret = esp_peer_send_audio(rtc.call.peer, &frame);
    digits_call_audio_record_send(rtc.call.audio, packet, ret == ESP_PEER_ERR_NONE,
                                 ret == ESP_PEER_ERR_WOULD_BLOCK);
    if (ret == ESP_PEER_ERR_NONE) {
        if (rtc.call.sent_audio) {
            uint32_t step_ms = packet->pts_ms - rtc.call.last_audio_pts;
            if (step_ms > 20 && step_ms < 60000) rtc.call.skipped_audio += step_ms / 20 - 1;
        }
        rtc.call.last_audio_pts = packet->pts_ms;
        rtc.call.sent_audio = true;
        ++rtc.call.tx_packets;
    }
    else if (ret == ESP_PEER_ERR_WOULD_BLOCK) ++rtc.call.blocked_audio;
    else {
        ESP_LOGW(TAG, "Encoded audio send failed: %d", ret);
        rtc.call.failure = "encoded audio send failed";
    }
    // PTS follows capture slots, including skipped PCM; no catch-up or retry burst.
}

static void refresh_transport(void)
{
    transport_t snapshot = transport_snapshot();
    if (snapshot.epoch != rtc.owner_transport.epoch) {
        end_call(false, "signaling session changed");
        clear_ice_servers();
        rtc.owner_transport = snapshot;
        ESP_LOGI(TAG, "Signaling session=%" PRIu32 " %s paired=%s; old peer/messages invalidated",
                 snapshot.session, snapshot.online ? "online" : "offline", snapshot.paired ? "yes" : "no");
        if (snapshot.online && snapshot.paired)
            send_wire("request-ice-servers", NULL, NULL, NULL, NULL);
    }
    if (snapshot.overflow) {
        ESP_LOGE(TAG, "Bounded peer message queue/copy exhausted; aborting call instead of losing control messages");
        end_call(true, "signaling queue exhausted");
        // Do not hang up an unrelated line. Server hangup ignores To for an
        // ordinary call, so the active peer is the sole safe notification.
        event_t *event;
        for (unsigned i = 0; i < MESSAGE_QUEUE_LENGTH &&
             xQueuePeek(rtc.queue, &event, 0) == pdTRUE && event->epoch == snapshot.epoch; ++i) {
            if (xQueueReceive(rtc.queue, &event, 0) == pdTRUE) free_event(event);
        }
        portENTER_CRITICAL(&transport_mux);
        if (latest_transport.epoch == snapshot.epoch) {
            latest_transport.overflow = false;
            latest_transport.overflow_from[0] = '\0';
        }
        portEXIT_CRITICAL(&transport_mux);
    }
}

static void publish_call_state(void)
{
    digits_webrtc_call_state_t state = {
        .phase = (digits_call_phase_t)rtc.call.phase,
        .online = rtc.owner_transport.online,
        .paired = rtc.owner_transport.paired,
        .ended_calls = rtc.ended_calls,
        .last_end = rtc.last_end,
        .last_connected = rtc.last_connected,
    };
    strcpy(state.remote, rtc.call.remote);
    portENTER_CRITICAL(&call_state_mux);
    published_call_state = state;
    portEXIT_CRITICAL(&call_state_mux);
}

void digits_webrtc_call_state(digits_webrtc_call_state_t *state)
{
    portENTER_CRITICAL(&call_state_mux);
    *state = published_call_state;
    portEXIT_CRITICAL(&call_state_mux);
}

static void peer_task(void *context)
{
    (void)context;
    for (;;) {
        reap_audio();
        refresh_transport();
        event_t *event;
        for (unsigned i = 0; i < MESSAGE_QUEUE_LENGTH && xQueueReceive(rtc.queue, &event, 0) == pdTRUE; ++i) {
            process_event(event);
            free_event(event);
            refresh_transport();
            if (rtc.call.failure) end_failed_call();
        }
        if (current_transport() && rtc.call.phase == CALL_RINGING && rtc.call.answer_requested &&
            rtc.call.offer && digits_incoming_ring_is_stopped())
            answer_incoming();
        if (rtc.call.phase == CALL_RINGING && !rtc.call.ring_started &&
            esp_timer_get_time() >= rtc.call.ring_hint_deadline_us)
            start_incoming_ring(-1); // No offer yet.
        if (rtc.call.peer && current_transport()) {
            int64_t before = esp_timer_get_time();
            bool connected = rtc.call.phase == CALL_CONNECTED;
            int ret = esp_peer_main_loop(rtc.call.peer);
            if (connected && rtc.call.phase == CALL_CONNECTED) {
                uint32_t elapsed = (uint32_t)(esp_timer_get_time() - before);
                if (elapsed > rtc.call.max_peer_loop_us) rtc.call.max_peer_loop_us = elapsed;
                if (elapsed > AUDIO_FRAME_US) ++rtc.call.slow_peer_loops;
            }
            if (ret != ESP_PEER_ERR_NONE) rtc.call.failure = "peer main loop failed";
        }
        refresh_transport(); // A disconnect during DTLS must invalidate its result.
        int64_t now = esp_timer_get_time();
        if (rtc.call.failure) {
            ESP_LOGW(TAG, "Peer failure: %s", rtc.call.failure);
            end_failed_call();
        } else if (rtc.call.deadline_us && now >= rtc.call.deadline_us) {
            end_call(true, rtc.call.phase == CALL_CONNECTING ? "connect_timeout" : "ringing timeout");
        } else {
            service_audio(now);
            if (rtc.call.phase == CALL_CONNECTED && now >= rtc.call.next_report_us) {
                rtc.call.next_report_us = now + 5000000;
                ESP_LOGI(TAG, "Encoded media: tx=%" PRIu32 " rx=%" PRIu32 " rx_bytes=%" PRIu32 " tx_pts_skips=%" PRIu32 " send_blocked=%" PRIu32 " peer_loop_max_us=%" PRIu32 " loops_over_20ms=%" PRIu32,
                         rtc.call.tx_packets, rtc.call.rx_packets, rtc.call.rx_bytes,
                         rtc.call.skipped_audio, rtc.call.blocked_audio, rtc.call.max_peer_loop_us,
                         rtc.call.slow_peer_loops);
                report_call_audio(rtc.call.audio);
                report_resources("Connected");
            }
        }
        publish_call_state();
        // The short socket timeout already paces the owner. One tick permits
        // codec tasks to run without adding the old unconditional 10 ms delay.
        ulTaskNotifyTake(pdTRUE, rtc.call.peer || rtc.stopping_audio ||
                        !digits_incoming_ring_is_stopped() ? 1 : pdMS_TO_TICKS(10));
    }
}

esp_err_t digits_webrtc_init(digits_webrtc_send_fn send, void *context)
{
    if (!send) return ESP_ERR_INVALID_ARG;
    if (rtc.queue) return ESP_ERR_INVALID_STATE;
    rtc.send = send;
    rtc.send_context = context;
    rtc.audio_mode = DIGITS_CALL_AUDIO_FULL;
    rtc.queue = xQueueCreate(MESSAGE_QUEUE_LENGTH, sizeof(event_t *));
    if (!rtc.queue) return ESP_ERR_NO_MEM;
    // Keep the bounded network owner ahead of the priority-5 audio workers.
    // Its socket wait and one-tick yield leave the codec/capture tasks runnable.
    if (xTaskCreatePinnedToCore(peer_task, "digits_peer", WORKER_STACK_BYTES, NULL,
                                WORKER_PRIORITY, &rtc.task, 1) != pdPASS) {
        vQueueDelete(rtc.queue);
        rtc.queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Ready: esp_peer 1.5.7, live Opus call audio; worker stack=%u bytes, core=1",
             WORKER_STACK_BYTES);
    return ESP_OK;
}

void digits_webrtc_transport(uint32_t session, bool online, const char *number, bool paired)
{
    char normalized_number[NUMBER_CAPACITY] = {0};
    if (!copy_number(normalized_number, number)) paired = false;
    portENTER_CRITICAL(&transport_mux);
    if (session != latest_transport.session || online != latest_transport.online ||
        paired != latest_transport.paired || strcmp(normalized_number, latest_transport.number) != 0) {
        ++latest_transport.epoch;
        latest_transport.session = session;
        latest_transport.online = online;
        latest_transport.paired = paired;
        strcpy(latest_transport.number, normalized_number);
        latest_transport.overflow = false;
        latest_transport.overflow_from[0] = '\0';
    }
    portEXIT_CRITICAL(&transport_mux);
    if (rtc.task) xTaskNotifyGive(rtc.task);
}

bool digits_webrtc_receive(const cJSON *message, uint32_t session)
{
    const char *type = json_text(message, "type");
    if (!type) return false;
    event_kind_t kind;
    if (!strcmp(type, "ring")) kind = EVENT_RING;
    else if (!strcmp(type, "sdp")) kind = EVENT_SDP;
    else if (!strcmp(type, "answer")) kind = EVENT_ANSWER;
    else if (!strcmp(type, "ice")) kind = EVENT_ICE;
    else if (!strcmp(type, "hangup")) kind = EVENT_HANGUP;
    else if (!strcmp(type, "busy")) kind = EVENT_BUSY;
    else if (!strcmp(type, "error")) kind = EVENT_ERROR;
    else if (!strcmp(type, "ice-servers")) kind = EVENT_ICE_SERVERS;
    else if (!strcmp(type, "ice_restart")) kind = EVENT_ICE_RESTART;
    else return false;
    const char *conference = json_text(message, "conf_id");
    if (conference && conference[0]) {
        // Reject before copying SDP or consuming the bounded queue: oversized
        // unsupported conference traffic must not abort the ordinary call.
        ESP_LOGW(TAG, "Ignoring conference signaling outside this milestone");
        return true;
    }
    if (!rtc.queue) return true;
    transport_t snapshot = transport_snapshot();
    if (!snapshot.online || snapshot.session != session) return true;
    event_t *event = audio_heap_alloc(sizeof(*event));
    if (!event) { queue_fault(snapshot.epoch, json_text(message, "from")); return true; }
    memset(event, 0, sizeof(*event));
    event->kind = kind;
    event->session = session;
    event->epoch = snapshot.epoch;
    const char *from = json_text(message, "from");
    if (from && !copy_number(event->from, from)) {
        ESP_LOGW(TAG, "Ignoring %s with invalid source number", type);
        free_event(event);
        return true;
    }
    event->conference = conference && conference[0];
    const char *data = NULL;
    size_t maximum = 0;
    if (kind == EVENT_SDP || kind == EVENT_ANSWER || kind == EVENT_ICE_RESTART) {
        data = json_text(message, "sdp"); maximum = DIGITS_WEBRTC_MAX_SDP;
        if (!data || !data[0]) {
            ESP_LOGW(TAG, "Ignoring %s without an SDP string", type);
            free_event(event);
            return true;
        }
    } else if (kind == EVENT_ICE) {
        data = json_text(message, "candidate"); maximum = DIGITS_WEBRTC_MAX_ICE;
        if (!data) data = ""; // Go omitempty represents end-of-candidates this way.
    }
    if (data) {
        event->data = copy_text(data, maximum);
        if (!event->data) {
            queue_fault(snapshot.epoch, from);
            free_event(event);
            return true;
        }
    }
    if (kind == EVENT_ICE_SERVERS) {
        const cJSON *servers = cJSON_GetObjectItemCaseSensitive(message, "servers");
        const cJSON *server;
        cJSON_ArrayForEach(server, servers) {
            const cJSON *url;
            cJSON_ArrayForEach(url, cJSON_GetObjectItemCaseSensitive(server, "urls")) {
                if (!cJSON_IsString(url) || !supported_stun_url(url->valuestring) ||
                    event->server_count == MAX_ICE_SERVERS) {
                    ++event->skipped_servers;
                    continue;
                }
                char *copy = copy_text(url->valuestring, ICE_URL_CAPACITY - 1);
                if (!copy) {
                    queue_fault(snapshot.epoch, NULL);
                    free_event(event);
                    return true;
                }
                event->servers[event->server_count++] = copy;
            }
        }
    }
    enqueue(event);
    return true;
}

static esp_err_t queue_command(const char *verb, const char *number, unsigned ringtone_hint)
{
    if (!verb) return ESP_ERR_INVALID_ARG;
    if (!rtc.queue) return ESP_ERR_INVALID_STATE;
    event_kind_t kind;
    if (!strcmp(verb, "call")) kind = COMMAND_CALL;
    else if (!strcmp(verb, "answer")) kind = COMMAND_ANSWER;
    else if (!strcmp(verb, "hangup")) kind = COMMAND_HANGUP;
    else if (!strcmp(verb, "status")) kind = COMMAND_STATUS;
    else if (!strcmp(verb, "audio")) kind = COMMAND_AUDIO;
    else return ESP_ERR_INVALID_ARG;
    transport_t snapshot = transport_snapshot();
    if (kind != COMMAND_STATUS && kind != COMMAND_AUDIO &&
        (!snapshot.online || !snapshot.paired)) return ESP_ERR_INVALID_STATE;
    event_t *event = audio_heap_alloc(sizeof(*event));
    if (!event) return ESP_ERR_NO_MEM;
    memset(event, 0, sizeof(*event));
    event->kind = kind;
    event->session = snapshot.session;
    event->epoch = snapshot.epoch;
    if (kind == COMMAND_CALL && !copy_number(event->from, number)) {
        free_event(event);
        return ESP_ERR_INVALID_ARG;
    }
    event->ringtone_hint = kind == COMMAND_CALL ? ringtone_hint : 0;
    if (kind == COMMAND_AUDIO) {
        if (!number || (strcmp(number, "tx") && strcmp(number, "rx") && strcmp(number, "full"))) {
            free_event(event);
            return ESP_ERR_INVALID_ARG;
        }
        strcpy(event->from, number);
    }
    return enqueue(event);
}

esp_err_t digits_webrtc_command(const char *verb, const char *number)
{
    return queue_command(verb, number, 0);
}

esp_err_t digits_webrtc_call(const char *number, unsigned ringtone)
{
    if (!ringtones_valid(ringtone)) return ESP_ERR_INVALID_ARG;
    return queue_command("call", number, ringtone + 1);
}

#else

esp_err_t digits_webrtc_init(digits_webrtc_send_fn send, void *context)
{
    (void)send; (void)context;
    return ESP_OK;
}

void digits_webrtc_transport(uint32_t session, bool online, const char *number, bool paired)
{
    (void)session; (void)online; (void)number; (void)paired;
}

bool digits_webrtc_receive(const cJSON *message, uint32_t session)
{
    (void)message; (void)session;
    return false;
}

esp_err_t digits_webrtc_command(const char *verb, const char *number)
{
    (void)verb; (void)number;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t digits_webrtc_call(const char *number, unsigned ringtone)
{
    (void)number; (void)ringtone;
    return ESP_ERR_NOT_SUPPORTED;
}

void digits_webrtc_set_ringtone_resolver(digits_ringtone_resolver_fn resolver)
{
    (void)resolver;
}

void digits_webrtc_call_state(digits_webrtc_call_state_t *state)
{
    *state = (digits_webrtc_call_state_t){.phase = DIGITS_CALL_IDLE};
}

#endif
