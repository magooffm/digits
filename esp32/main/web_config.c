#include "web_config.h"
#include "call_tones.h"
#include "phone.h"
#include "ringtones.h"
#include "sd_card.h"
#include "sd_ringtones.h"
#include "sd_ringtone_util.h"
#include "status.h"
#include "voicemail.h"
#include "webrtc.h"
#include "wifi_setup_form.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TAG "web_config"
#define PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CHUNK 4096
#define FORM_BYTES 256
#define CONFIG_JSON_BYTES 12288
#define MAX_RINGTONE_BYTES (20 * 1024 * 1024)
#define MAX_GREETING_BYTES (5 * 1024 * 1024)
#define RINGTONE_UPLOAD DIGITS_SD_RINGTONE_FOLDER "/upload.tmp"
#define GREETING_UPLOAD DIGITS_SD_MOUNT_POINT "/voicemail/greeting.tmp"

// ---- Settings writes on an internal-RAM stack ----------------------------
// The web server's stack is in PSRAM, and writing NVS turns the flash cache
// off, which such a task must not do. Writes run here instead; the server
// handles one request at a time, so one job slot is enough.

typedef esp_err_t (*job_fn)(const void *arg);
static struct {
    QueueHandle_t start;
    SemaphoreHandle_t done;
    job_fn fn;
    const void *arg;
    esp_err_t result;
} job;

static void worker(void *unused)
{
    (void)unused;
    for (;;) {
        bool go;
        xQueueReceive(job.start, &go, portMAX_DELAY);
        job.result = job.fn(job.arg);
        xSemaphoreGive(job.done);
    }
}

static esp_err_t run_internal(job_fn fn, const void *arg)
{
    if (!job.start) return ESP_ERR_INVALID_STATE;
    job.fn = fn;
    job.arg = arg;
    bool go = true;
    xQueueSend(job.start, &go, portMAX_DELAY);
    return xSemaphoreTake(job.done, pdMS_TO_TICKS(10000)) == pdTRUE ? job.result : ESP_ERR_TIMEOUT;
}

// ---- Common ---------------------------------------------------------------

static bool password_set(void) { return strlen(CONFIG_DIGITS_SETUP_PASSWORD) >= 8; }

// Reading private data: the password when one is set.
static bool may_read(httpd_req_t *req) { return digits_status_authorized(req); }

static esp_err_t reply(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

// Changing anything needs a configured password: without one, everybody on
// the home network could otherwise change who the buttons call.
static bool may_write(httpd_req_t *req, esp_err_t *result)
{
    if (!password_set()) {
        *result = reply(req, "403 Forbidden", "Set a setup password (menuconfig) to change settings here.");
        return false;
    }
    if (!digits_status_authorized(req)) {
        *result = digits_status_challenge(req);
        return false;
    }
    return true;
}

static bool line_idle(void)
{
    digits_webrtc_call_state_t call;
    digits_webrtc_call_state(&call);
    digits_phone_config_t phone;
    digits_phone_config(&phone);
    return call.phase == DIGITS_CALL_IDLE && !phone.off_hook;
}

static esp_err_t done(httpd_req_t *req, esp_err_t err, const char *what)
{
    if (err == ESP_OK) return reply(req, "200 OK", what);
    char text[96];
    snprintf(text, sizeof(text), "Not done: %s", err == ESP_ERR_INVALID_ARG ? "invalid value" :
             err == ESP_ERR_INVALID_STATE ? "the line is busy" : err == ESP_ERR_NOT_SUPPORTED ? "not possible here" :
             err == ESP_ERR_INVALID_RESPONSE ? "not a playable MP3 file" : esp_err_to_name(err));
    return reply(req, "409 Conflict", text);
}

static int read_form(httpd_req_t *req, char *body, size_t capacity)
{
    if (req->content_len >= capacity) return -1;
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    body[got] = '\0';
    return (int)got;
}

static bool form_number(const char *body, size_t size, const char *name, unsigned long *out)
{
    char text[16], *end;
    if (!wifi_setup_field(body, size, name, text, sizeof(text)) || !text[0]) return false;
    *out = strtoul(text, &end, 10);
    return !*end;
}

static bool query_number(httpd_req_t *req, const char *name, unsigned long *out)
{
    char query[64], text[16], *end;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, name, text, sizeof(text)) != ESP_OK || !text[0])
        return false;
    *out = strtoul(text, &end, 10);
    return !*end;
}

static esp_err_t send_file(httpd_req_t *req, const char *path, const char *type)
{
    FILE *file = fopen(path, "rb");
    char *chunk = file ? heap_caps_malloc(CHUNK, PSRAM) : NULL;
    if (!chunk) {
        if (file) fclose(file);
        return reply(req, "404 Not Found", "No such file");
    }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = ESP_OK;
    size_t n;
    while (err == ESP_OK && (n = fread(chunk, 1, CHUNK, file)) > 0)
        err = httpd_resp_send_chunk(req, chunk, (ssize_t)n);
    fclose(file);
    free(chunk);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

// Request body to a temporary file on the card.
static esp_err_t receive_file(httpd_req_t *req, const char *path, size_t limit)
{
    if (!req->content_len || req->content_len > limit) return ESP_ERR_INVALID_SIZE;
    FILE *file = fopen(path, "wb");
    char *chunk = file ? heap_caps_malloc(CHUNK, PSRAM) : NULL;
    esp_err_t err = chunk ? ESP_OK : ESP_FAIL;
    for (size_t left = req->content_len; err == ESP_OK && left;) {
        int n = httpd_req_recv(req, chunk, left < CHUNK ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || fwrite(chunk, 1, (size_t)n, file) != (size_t)n) err = ESP_FAIL;
        else left -= (size_t)n;
    }
    free(chunk);
    if (file && fclose(file)) err = ESP_FAIL;
    if (err != ESP_OK) unlink(path);
    return err;
}

// ---- Configuration --------------------------------------------------------

static size_t append(char *out, size_t at, size_t capacity, const char *format, ...)
    __attribute__((format(printf, 4, 5)));
static size_t append(char *out, size_t at, size_t capacity, const char *format, ...)
{
    if (at >= capacity) return at;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out + at, capacity - at, format, args);
    va_end(args);
    return n < 0 ? at : at + (size_t)n;
}

static esp_err_t config_get(httpd_req_t *req)
{
    if (!may_read(req)) return digits_status_challenge(req);
    char *out = heap_caps_malloc(CONFIG_JSON_BYTES, PSRAM);
    if (!out) return reply(req, "500 Internal Server Error", "No memory");
    digits_phone_config_t phone;
    digits_phone_config(&phone);
    char name[2 * RINGTONE_NAME_BYTES + 3];
    size_t at = append(out, 0, CONFIG_JSON_BYTES, "{\"writable\":%s,\"idle\":%s,\"buttons\":[",
                       password_set() ? "true" : "false", line_idle() ? "true" : "false");
    for (unsigned i = 0; i < phone.buttons; ++i) {
        const digits_phone_button_t *b = &phone.button[i];
        at = append(out, at, CONFIG_JSON_BYTES,
                    "%s{\"name\":\"%c\",\"number\":\"%s\",\"ringtone\":%u,\"selector\":%u,\"knob\":%s,\"rings_here\":%u}",
                    i ? "," : "", 'A' + i, b->number, b->ringtone, b->selector + 1,
                    b->selector_hardware ? "true" : "false", b->rings_here);
    }
    at = append(out, at, CONFIG_JSON_BYTES, "],\"positions\":[");
    for (unsigned p = 0; p < phone.map.positions; ++p) {
        digits_status_json_string(name, sizeof(name), phone.file[p]);
        at = append(out, at, CONFIG_JSON_BYTES, "%s{\"position\":%u,\"builtin\":%u,\"file\":%s,\"plays\":%u}",
                    p ? "," : "", p + 1, phone.map.ringtone[p], name, phone.effective[p]);
    }
    at = append(out, at, CONFIG_JSON_BYTES, "],\"ringtones\":[");
    unsigned ids[RINGTONE_FIRST_CLIP + RINGTONE_MAX_CLIPS];
    unsigned count = ringtones_list(ids, sizeof(ids) / sizeof(ids[0]));
    for (unsigned i = 0; i < count; ++i) {
        digits_status_json_string(name, sizeof(name), ringtones_name(ids[i]));
        at = append(out, at, CONFIG_JSON_BYTES, "%s{\"id\":%u,\"name\":%s}", i ? "," : "", ids[i], name);
    }
    at = append(out, at, CONFIG_JSON_BYTES, "],\"library\":[");
    bool first = true;
    for (unsigned id = RINGTONE_FIRST_LOCAL; id < RINGTONE_FIRST_LOCAL + RINGTONE_LOCAL_SLOTS; ++id) {
        if (!ringtones_valid(id)) continue;
        digits_status_json_string(name, sizeof(name), ringtones_name(id));
        at = append(out, at, CONFIG_JSON_BYTES, "%s{\"id\":%u,\"name\":%s,\"seconds\":%u}", first ? "" : ",", id,
                    name, (unsigned)(ringtones_melody_frames(id) / RINGTONE_SAMPLE_RATE));
        first = false;
    }
    at = append(out, at, CONFIG_JSON_BYTES, "],\"card\":\"%s\",\"greeting\":{\"custom\":%s,\"seconds\":%u}}",
                digits_sd_ringtones_status(), digits_sd_greeting_frames() ? "true" : "false",
                (unsigned)(digits_sd_greeting_frames() / RINGTONE_SAMPLE_RATE));
    esp_err_t err = at < CONFIG_JSON_BYTES ? (httpd_resp_set_type(req, "application/json"),
                                              httpd_resp_send(req, out, (ssize_t)at))
                                           : reply(req, "500 Internal Server Error", "Too much to list");
    free(out);
    return err;
}

typedef struct {
    unsigned long button, ringtone, selector;
    char number[32];
    bool has_number, has_ringtone, has_selector;
} button_change_t;

static esp_err_t apply_button(const void *arg)
{
    const button_change_t *c = arg;
    esp_err_t err = ESP_OK;
    if (c->has_number) err = digits_phone_set_number((unsigned)c->button, c->number);
    if (err == ESP_OK && c->has_ringtone) err = digits_phone_set_ringtone((unsigned)c->button, (unsigned)c->ringtone);
    if (err == ESP_OK && c->has_selector && c->selector)
        err = digits_phone_set_selector((unsigned)c->button, (unsigned)c->selector - 1);
    return err;
}

static esp_err_t button_post(httpd_req_t *req)
{
    esp_err_t result;
    if (!may_write(req, &result)) return result;
    char body[FORM_BYTES];
    int size = read_form(req, body, sizeof(body));
    button_change_t change = {0};
    if (size < 0 || !form_number(body, (size_t)size, "button", &change.button))
        return done(req, ESP_ERR_INVALID_ARG, "");
    change.has_number = wifi_setup_field(body, (size_t)size, "number", change.number, sizeof(change.number));
    change.has_ringtone = form_number(body, (size_t)size, "ringtone", &change.ringtone);
    change.has_selector = form_number(body, (size_t)size, "selector", &change.selector);
    return done(req, run_internal(apply_button, &change), "Saved");
}

typedef struct { unsigned long position, ringtone; } position_change_t;

static esp_err_t apply_position(const void *arg)
{
    const position_change_t *c = arg;
    return c->position ? digits_phone_set_position((unsigned)c->position - 1, (unsigned)c->ringtone)
                       : ESP_ERR_INVALID_ARG;
}

static esp_err_t position_post(httpd_req_t *req)
{
    esp_err_t result;
    if (!may_write(req, &result)) return result;
    char body[FORM_BYTES];
    int size = read_form(req, body, sizeof(body));
    position_change_t change;
    if (size < 0 || !form_number(body, (size_t)size, "position", &change.position) ||
        !form_number(body, (size_t)size, "ringtone", &change.ringtone))
        return done(req, ESP_ERR_INVALID_ARG, "");
    return done(req, run_internal(apply_position, &change), "Saved");
}

// Previews and playback use the speaker: harmless, but still the owner's.
static esp_err_t preview_post(httpd_req_t *req)
{
    esp_err_t result;
    if (!may_write(req, &result)) return result;
    char body[FORM_BYTES];
    int size = read_form(req, body, sizeof(body));
    unsigned long id;
    if (size < 0 || !form_number(body, (size_t)size, "id", &id)) return done(req, ESP_ERR_INVALID_ARG, "");
    return done(req, digits_phone_preview((unsigned)id), "Playing on the phone");
}

static esp_err_t stop_post(httpd_req_t *req)
{
    esp_err_t result;
    if (!may_write(req, &result)) return result;
    digits_voicemail_stop();
    digits_call_tones_stop();
    return reply(req, "200 OK", "Stopped");
}

// ---- Ringtone library ----------------------------------------------------

static esp_err_t ringtone_get(httpd_req_t *req)
{
    if (!may_read(req)) return digits_status_challenge(req);
    unsigned long id;
    char path[128];
    if (!query_number(req, "id", &id) || !digits_sd_ringtone_file((unsigned)id, path, sizeof(path)))
        return reply(req, "404 Not Found", "No such song");
    return send_file(req, path, "audio/mpeg");
}

// Body: the MP3; query: name=<file name>. Same name replaces that song.
static esp_err_t ringtone_post(httpd_req_t *req)
{
    esp_err_t result;
    if (!may_write(req, &result)) return result;
    char query[160], name[RINGTONE_NAME_BYTES], path[sizeof(DIGITS_SD_RINGTONE_FOLDER) + RINGTONE_NAME_BYTES + 1];
    size_t query_length = httpd_req_get_url_query_len(req);
    if (query_length >= sizeof(query) || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        !wifi_setup_field(query, query_length, "name", name, sizeof(name)) ||
        !sd_ringtone_name_valid(name, sizeof(name)))
        return reply(req, "400 Bad Request", "The file name must end in .mp3, up to 51 characters, without / \\ : * ? \" < > |");
    if (!digits_sd_card_mounted()) return reply(req, "409 Conflict", "No SD card");
    if (!line_idle()) return done(req, ESP_ERR_INVALID_STATE, "");
    mkdir(DIGITS_SD_RINGTONE_FOLDER, 0700);
    esp_err_t err = receive_file(req, RINGTONE_UPLOAD, MAX_RINGTONE_BYTES);
    if (err == ESP_ERR_INVALID_SIZE) return reply(req, "413 Payload Too Large", "Up to 20 MB");
    if (err != ESP_OK) return reply(req, "500 Internal Server Error", "Writing to the SD card failed");
    snprintf(path, sizeof(path), "%s/%s", DIGITS_SD_RINGTONE_FOLDER, name);
    int existing = digits_sd_ringtone_find(name);
    char old[sizeof(path)];
    if (existing >= 0 && digits_sd_ringtone_file((unsigned)existing, old, sizeof(old))) unlink(old); // Any case.
    unlink(path);
    if (rename(RINGTONE_UPLOAD, path)) {
        unlink(RINGTONE_UPLOAD);
        return reply(req, "500 Internal Server Error", "Writing to the SD card failed");
    }
    err = digits_sd_ringtone_added(name, NULL);
    if (err == ESP_ERR_INVALID_RESPONSE) unlink(path);
    if (err == ESP_ERR_NO_MEM) {
        unlink(path);
        return reply(req, "409 Conflict", "The library is full (32 songs): delete one first");
    }
    ESP_LOGI(TAG, "Song %s uploaded: %s", name, esp_err_to_name(err));
    return done(req, err, "Uploaded");
}

static esp_err_t forget_song(const void *name) { return digits_phone_forget_song(name); }

static esp_err_t ringtone_delete_post(httpd_req_t *req)
{
    esp_err_t result;
    if (!may_write(req, &result)) return result;
    char body[FORM_BYTES], name[RINGTONE_NAME_BYTES];
    int size = read_form(req, body, sizeof(body));
    unsigned long id;
    if (size < 0 || !form_number(body, (size_t)size, "id", &id) || !ringtones_name((unsigned)id) ||
        id < RINGTONE_FIRST_LOCAL)
        return done(req, ESP_ERR_INVALID_ARG, "");
    if (!line_idle()) return done(req, ESP_ERR_INVALID_STATE, "");
    snprintf(name, sizeof(name), "%s", ringtones_name((unsigned)id));
    esp_err_t err = digits_sd_ringtone_delete((unsigned)id);
    if (err == ESP_OK) err = run_internal(forget_song, name); // Positions using it fall back.
    return done(req, err, "Deleted");
}

// ---- Messages -------------------------------------------------------------

static esp_err_t messages_get(httpd_req_t *req)
{
    if (!may_read(req)) return digits_status_challenge(req);
    digits_voicemail_message_t probe;
    unsigned total = digits_voicemail_list(&probe, 0);
    digits_voicemail_message_t *list = total ? heap_caps_calloc(total, sizeof(*list), PSRAM) : NULL;
    size_t capacity = 96 + (size_t)total * 128;
    char *out = heap_caps_malloc(capacity, PSRAM);
    if ((total && !list) || !out) {
        free(list);
        free(out);
        return reply(req, "500 Internal Server Error", "No memory");
    }
    unsigned count = total ? digits_voicemail_list(list, total) : 0;
    if (count > total) count = total;
    size_t at = append(out, 0, capacity, "{\"ready\":%s,\"messages\":[", digits_voicemail_ready() ? "true" : "false");
    for (unsigned i = 0; i < count; ++i)
        at = append(out, at, capacity, "%s{\"n\":%u,\"caller\":\"%s\",\"heard\":%s,\"time\":%lld,\"seconds\":%u.%u}",
                    i ? "," : "", (unsigned)list[i].number, list[i].caller, list[i].heard ? "true" : "false",
                    (long long)list[i].recorded, (unsigned)list[i].tenths / 10, (unsigned)list[i].tenths % 10);
    at = append(out, at, capacity, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, out, (ssize_t)(at < capacity ? at : capacity - 1));
    free(list);
    free(out);
    return err;
}

static esp_err_t message_get(httpd_req_t *req)
{
    if (!may_read(req)) return digits_status_challenge(req);
    unsigned long n;
    char path[128];
    if (!query_number(req, "n", &n) || !digits_voicemail_file((uint32_t)n, path, sizeof(path)))
        return reply(req, "404 Not Found", "No such message");
    return send_file(req, path, "audio/ogg");
}

static esp_err_t message_post(httpd_req_t *req)
{
    esp_err_t result;
    if (!may_write(req, &result)) return result;
    char body[FORM_BYTES], action[12];
    int size = read_form(req, body, sizeof(body));
    unsigned long n;
    if (size < 0 || !form_number(body, (size_t)size, "n", &n) ||
        !wifi_setup_field(body, (size_t)size, "action", action, sizeof(action)))
        return done(req, ESP_ERR_INVALID_ARG, "");
    if (!strcmp(action, "heard")) return done(req, digits_voicemail_mark_heard((uint32_t)n), "Marked heard");
    if (!strcmp(action, "delete")) return done(req, digits_voicemail_delete((uint32_t)n), "Deleted");
    if (!strcmp(action, "play")) return done(req, digits_voicemail_play((uint32_t)n), "Playing on the phone");
    return done(req, ESP_ERR_INVALID_ARG, "");
}

// ---- Greeting -------------------------------------------------------------

static esp_err_t greeting_get(httpd_req_t *req)
{
    if (!may_read(req)) return digits_status_challenge(req);
    return send_file(req, DIGITS_SD_GREETING_PATH, "audio/mpeg");
}

static esp_err_t greeting_post(httpd_req_t *req)
{
    esp_err_t result;
    if (!may_write(req, &result)) return result;
    if (!digits_sd_card_mounted()) return reply(req, "409 Conflict", "No SD card");
    if (!line_idle()) return done(req, ESP_ERR_INVALID_STATE, "");
    mkdir(DIGITS_SD_MOUNT_POINT "/voicemail", 0700);
    esp_err_t err = receive_file(req, GREETING_UPLOAD, MAX_GREETING_BYTES);
    if (err == ESP_ERR_INVALID_SIZE) return reply(req, "413 Payload Too Large", "Up to 5 MB");
    if (err != ESP_OK) return reply(req, "500 Internal Server Error", "Writing to the SD card failed");
    unlink(DIGITS_SD_GREETING_PATH);
    if (rename(GREETING_UPLOAD, DIGITS_SD_GREETING_PATH)) {
        unlink(GREETING_UPLOAD);
        return reply(req, "500 Internal Server Error", "Writing to the SD card failed");
    }
    err = digits_sd_greeting_reload();
    if (err == ESP_ERR_INVALID_RESPONSE) {
        unlink(DIGITS_SD_GREETING_PATH);
        digits_sd_greeting_reload();
    }
    return done(req, err, "Greeting uploaded");
}

static esp_err_t greeting_action_post(httpd_req_t *req)
{
    esp_err_t result;
    if (!may_write(req, &result)) return result;
    char body[FORM_BYTES], action[12];
    int size = read_form(req, body, sizeof(body));
    if (size < 0 || !wifi_setup_field(body, (size_t)size, "action", action, sizeof(action)))
        return done(req, ESP_ERR_INVALID_ARG, "");
    if (!strcmp(action, "play")) return done(req, digits_voicemail_play_greeting(), "Playing on the phone");
    if (!strcmp(action, "delete")) {
        if (!line_idle()) return done(req, ESP_ERR_INVALID_STATE, "");
        unlink(DIGITS_SD_GREETING_PATH);
        return done(req, digits_sd_greeting_reload(), "The built-in greeting is back");
    }
    return done(req, ESP_ERR_INVALID_ARG, "");
}

// ---- Registration ---------------------------------------------------------

void digits_web_config_register(httpd_handle_t server)
{
    if (!job.start) {
        job.start = xQueueCreate(1, sizeof(bool));
        job.done = xSemaphoreCreateBinary();
        if (!job.start || !job.done ||
            xTaskCreate(worker, "digits_web_nvs", 4096, NULL, 3, NULL) != pdPASS)
            ESP_LOGE(TAG, "Settings worker unavailable: changes from the web page will fail");
    }
    static const httpd_uri_t handlers[] = {
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/button", .method = HTTP_POST, .handler = button_post},
        {.uri = "/api/position", .method = HTTP_POST, .handler = position_post},
        {.uri = "/api/preview", .method = HTTP_POST, .handler = preview_post},
        {.uri = "/api/stop", .method = HTTP_POST, .handler = stop_post},
        {.uri = "/api/ringtone", .method = HTTP_GET, .handler = ringtone_get},
        {.uri = "/api/ringtone", .method = HTTP_POST, .handler = ringtone_post},
        {.uri = "/api/ringtone-delete", .method = HTTP_POST, .handler = ringtone_delete_post},
        {.uri = "/api/messages", .method = HTTP_GET, .handler = messages_get},
        {.uri = "/api/message", .method = HTTP_GET, .handler = message_get},
        {.uri = "/api/message", .method = HTTP_POST, .handler = message_post},
        {.uri = "/api/greeting", .method = HTTP_GET, .handler = greeting_get},
        {.uri = "/api/greeting", .method = HTTP_POST, .handler = greeting_post},
        {.uri = "/api/greeting-action", .method = HTTP_POST, .handler = greeting_action_post},
    };
    for (unsigned i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i)
        httpd_register_uri_handler(server, &handlers[i]);
}
