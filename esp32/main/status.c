#include "status.h"
#include "line_settings.h"
#include "log_ring.h"
#include "sd_card.h"
#include "sd_ringtones.h"
#include "setup.h"
#include "silence.h"
#include "voicemail.h"
#include "webrtc.h"
#include "wifi.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#define LOG_BYTES (32 * 1024)
#define LOG_LINE_BYTES 256
#define LOG_REPLY_BYTES (8 * 1024) // Per poll; two block copies under the spinlock.

extern const char status_page_start[] asm("_binary_status_page_html_start");
extern const char status_page_end[] asm("_binary_status_page_html_end");
extern const char logs_page_start[] asm("_binary_logs_page_html_start");
extern const char logs_page_end[] asm("_binary_logs_page_html_end");

// ---- Log capture ----------------------------------------------------------

static log_ring_t ring;
static portMUX_TYPE ring_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t serial_vprintf;
static int formatting;          // One formatter at a time; others skip, never wait.
static char line[LOG_LINE_BYTES];

// Runs in whichever task logs. Never blocks: a line logged while another is
// being formatted still reaches the serial console, only not the page.
static int capture(const char *format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    // The microphone diagnostic's line every second would push everything
    // else out of the page's log; it stays on the serial console.
    bool keep = !strstr(format, "mono RMS=");
    if (keep && ring.data && !__atomic_exchange_n(&formatting, 1, __ATOMIC_ACQUIRE)) {
        int length = vsnprintf(line, sizeof(line), format, copy);
        if (length >= (int)sizeof(line)) {
            length = sizeof(line) - 1;
            line[length - 1] = '\n';
        }
        if (length > 0) {
            portENTER_CRITICAL(&ring_mux);
            log_ring_append(&ring, line, (size_t)length);
            portEXIT_CRITICAL(&ring_mux);
        }
        __atomic_store_n(&formatting, 0, __ATOMIC_RELEASE);
    }
    va_end(copy);
    return serial_vprintf(format, args);
}

void digits_status_init(void)
{
    char *storage = heap_caps_malloc(LOG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) return;
    log_ring_init(&ring, storage, LOG_BYTES);
    serial_vprintf = esp_log_set_vprintf(capture);
}

// ---- Access ---------------------------------------------------------------

bool digits_status_authorized(httpd_req_t *req)
{
    const char *password = CONFIG_DIGITS_SETUP_PASSWORD;
    size_t password_length = strlen(password);
    if (password_length < 8) return true; // No password configured: open.
    char header[200];
    unsigned char decoded[150];
    size_t decoded_length = 0;
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK ||
        strncmp(header, "Basic ", 6) ||
        mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_length, (const unsigned char *)header + 6,
                              strlen(header + 6)))
        return false;
    decoded[decoded_length] = '\0';
    const char *given = strchr((const char *)decoded, ':');
    if (!given || strlen(++given) != password_length) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < password_length; ++i) difference |= (unsigned char)(given[i] ^ password[i]);
    return !difference;
}

esp_err_t digits_status_challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Digits phone\", charset=\"UTF-8\"");
    return httpd_resp_send(req, "The setup password is needed (any user name).", HTTPD_RESP_USE_STRLEN);
}

// ---- Pages ----------------------------------------------------------------

// Each request costs a call some audio (network handling): pages ask again
// after this many milliseconds, much less often while a call is active.
static void set_poll_hint(httpd_req_t *req, unsigned idle_ms)
{
    digits_webrtc_call_state_t call;
    digits_webrtc_call_state(&call);
    static char idle[12];
    snprintf(idle, sizeof(idle), "%u", idle_ms);
    httpd_resp_set_hdr(req, "X-Poll-Ms", call.phase == DIGITS_CALL_IDLE ? idle : "15000");
}

static const char *phase_name(digits_call_phase_t phase)
{
    static const char *const names[] = {"idle", "calling", "ringing", "connecting", "connected"};
    return phase <= DIGITS_CALL_CONNECTED ? names[phase] : "unknown";
}

esp_err_t digits_status_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, status_page_start, status_page_end - status_page_start - 1);
}

// JSON string value with quotes; control characters become spaces.
void digits_status_json_string(char *out, size_t capacity, const char *text)
{
    size_t at = 0;
    if (capacity < 3) return;
    out[at++] = '"';
    for (; *text && at + 3 < capacity; ++text) {
        char c = *text;
        if (c == '"' || c == '\\') out[at++] = '\\';
        out[at++] = (unsigned char)c < 0x20 ? ' ' : c;
    }
    out[at++] = '"';
    out[at] = '\0';
}

#define STATUS_BYTES 1024

// Written into one PSRAM buffer: many small cJSON allocations would land in
// internal RAM, which calls need.
static esp_err_t status_get(httpd_req_t *req)
{
    bool allowed = digits_status_authorized(req);
    digits_wifi_config_t wifi;
    digits_wifi_config(&wifi);
    digits_webrtc_call_state_t call;
    digits_webrtc_call_state(&call);
    digits_silence_state_t silence = digits_silence_state();
    digits_line_settings_t settings;
    digits_settings_get(&settings, NULL);
    char number[32], address[16] = "", ssid[2 * WIFI_SETUP_SSID_BYTES + 3], url[2 * WIFI_SETUP_URL_BYTES + 3];
    digits_setup_number(number, sizeof(number));
    digits_wifi_address(address, sizeof(address));
    digits_status_json_string(ssid, sizeof(ssid), wifi.ssid);
    digits_status_json_string(url, sizeof(url), wifi.url);
    char *text = heap_caps_malloc(STATUS_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    int unheard = digits_voicemail_unheard();
    snprintf(text, STATUS_BYTES,
             "{\"firmware\":\"%s\",\"uptime_s\":%llu,\"number\":\"%s\",\"authorized\":%s,"
             "\"call\":{\"phase\":\"%s\",\"remote\":\"%s\"},"
             "\"silence\":{\"silent\":%s,\"server\":%s,\"overridden\":%s},\"voice_style\":\"%s\","
             "\"voicemail\":{\"ready\":%s,\"enabled\":%s,\"timeout_s\":%u,\"unheard\":%d},"
             "\"wifi\":{\"ssid\":%s,\"connected\":%s,\"rssi\":%d,\"ip\":\"%s\"},"
             "\"server\":{\"url\":%s,\"connected\":%s},"
             "\"sd\":\"%s, ringtones: %s\","
             "\"memory\":{\"internal_free\":%u,\"internal_min\":%u,\"internal_largest\":%u,\"psram_free\":%u}}",
             esp_app_get_description()->version, (unsigned long long)(esp_timer_get_time() / 1000000), number,
             allowed ? "true" : "false", phase_name(call.phase),
             allowed && call.phase != DIGITS_CALL_IDLE ? call.remote : "",
             silence.effective ? "true" : "false", silence.server ? "true" : "false",
             silence.overridden ? "true" : "false",
             settings.voice_style == DIGITS_VOICE_MODERN ? "modern" : "copper", digits_voicemail_ready() ? "true" : "false",
             settings.voicemail_enabled ? "true" : "false", (unsigned)settings.voicemail_ring_timeout_seconds,
             unheard > 0 ? unheard : 0, ssid, digits_wifi_online() ? "true" : "false", digits_wifi_rssi(), address,
             url, digits_setup_server_connected() ? "true" : "false",
             digits_sd_card_mounted() ? "mounted" : "none", digits_sd_ringtones_status(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    set_poll_hint(req, 3000);
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    free(text);
    return err;
}

static esp_err_t logs_get(httpd_req_t *req)
{
    if (!digits_status_authorized(req)) return digits_status_challenge(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, logs_page_start, logs_page_end - logs_page_start - 1);
}

static esp_err_t log_get(httpd_req_t *req)
{
    if (!digits_status_authorized(req)) return digits_status_challenge(req);
    char query[48], value[24];
    uint64_t since = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", value, sizeof(value)) == ESP_OK)
        since = strtoull(value, NULL, 10);
    char *text = heap_caps_malloc(LOG_REPLY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text || !ring.data) {
        free(text);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Log unavailable");
    }
    uint64_t next = 0;
    bool cut = false;
    portENTER_CRITICAL(&ring_mux);
    size_t length = log_ring_read(&ring, since, text, LOG_REPLY_BYTES, &next, &cut);
    portEXIT_CRITICAL(&ring_mux);
    if (cut) length = log_ring_whole_lines(text, length);
    char position[24];
    snprintf(position, sizeof(position), "%llu", (unsigned long long)next);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Log-Next", position);
    set_poll_hint(req, 2000);
    esp_err_t err = httpd_resp_send(req, text, (ssize_t)length);
    free(text);
    return err;
}

// The browser asks for the password here and then sends it with every
// request to this phone.
static esp_err_t login_get(httpd_req_t *req)
{
    if (!digits_status_authorized(req)) return digits_status_challenge(req);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

void digits_status_register(httpd_handle_t server)
{
    static const httpd_uri_t handlers[] = {
        {.uri = "/login", .method = HTTP_GET, .handler = login_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/logs", .method = HTTP_GET, .handler = logs_get},
        {.uri = "/api/log", .method = HTTP_GET, .handler = log_get},
    };
    for (unsigned i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i)
        httpd_register_uri_handler(server, &handlers[i]);
}
