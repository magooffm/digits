#include "line_settings.h"
#include <stddef.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define SETTINGS_NAMESPACE "digits_settings"
#define SETTINGS_KEY "snapshot"
#define SETTINGS_RECORD_SIZE 12U
#define SETTINGS_WORKER_STACK 4096U
#define SETTINGS_RETRY_MS 1000U
#define SETTINGS_OBJECT_MEMBERS 32U
#define SETTINGS_DEFAULT_TIMEOUT 20U

static const digits_line_settings_t defaults = {
    .voice_style = DIGITS_VOICE_COPPER,
    .voicemail_ring_timeout_seconds = SETTINGS_DEFAULT_TIMEOUT,
};

static portMUX_TYPE settings_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    digits_line_settings_t current;
    digits_settings_stats_t stats;
    TaskHandle_t worker;
    uint32_t session, transport_revision;
    bool initialized, have_session, ready, have_authoritative;
} settings = {
    .current = {
        .voice_style = DIGITS_VOICE_COPPER,
        .voicemail_ring_timeout_seconds = SETTINGS_DEFAULT_TIMEOUT,
    },
};

// Explicit bytes, no padding, pointers, ABI-dependent enums or byte order. NVS
// supplies the atomic blob/CRC; reserved bytes are zero for deterministic writes.
static void encode_record(const digits_line_settings_t *s, uint8_t record[SETTINGS_RECORD_SIZE])
{
    const uint8_t bytes[SETTINGS_RECORD_SIZE] = {
        'D', 'S', 'E', 'T', 1, (uint8_t)s->voice_style,
        s->silent_mode, s->auto_update, s->voicemail_enabled,
        s->voicemail_ring_timeout_seconds, 0, 0,
    };
    memcpy(record, bytes, sizeof(bytes));
}

static bool decode_record(const uint8_t record[SETTINGS_RECORD_SIZE], digits_line_settings_t *s)
{
    if (memcmp(record, "DSET", 4) || record[4] != 1 || record[5] > DIGITS_VOICE_MODERN ||
        record[6] > 1 || record[7] > 1 || record[8] > 1 ||
        record[9] < 5 || record[9] > 60 || record[10] || record[11]) return false;
    *s = (digits_line_settings_t){
        .voice_style = (digits_voice_style_t)record[5],
        .silent_mode = record[6] != 0,
        .auto_update = record[7] != 0,
        .voicemail_enabled = record[8] != 0,
        .voicemail_ring_timeout_seconds = record[9],
    };
    return true;
}

static bool equal_settings(const digits_line_settings_t *a, const digits_line_settings_t *b)
{
    return a->voice_style == b->voice_style && a->silent_mode == b->silent_mode &&
        a->auto_update == b->auto_update && a->voicemail_enabled == b->voicemail_enabled &&
        a->voicemail_ring_timeout_seconds == b->voicemail_ring_timeout_seconds;
}

static esp_err_t load_record(digits_line_settings_t *out)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs);
    if (error != ESP_OK) return error;
    uint8_t record[SETTINGS_RECORD_SIZE];
    size_t size = sizeof(record);
    error = nvs_get_blob(nvs, SETTINGS_KEY, record, &size);
    nvs_close(nvs);
    if (error != ESP_OK) return error;
    return size == sizeof(record) && decode_record(record, out) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

// Only the persistence worker calls this. A newer RAM generation remains dirty
// even if an older write succeeds; a failed write always remains retryable.
static bool persist_pending(void)
{
    digits_line_settings_t snapshot;
    uint32_t revision;
    portENTER_CRITICAL(&settings_lock);
    bool dirty = settings.stats.dirty;
    bool media_busy = settings.stats.media_busy;
    snapshot = settings.current;
    revision = settings.stats.revision;
    portEXIT_CRITICAL(&settings_lock);
    if (!dirty) return false;
    if (media_busy) return true;

    uint8_t record[SETTINGS_RECORD_SIZE];
    encode_record(&snapshot, record);
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error == ESP_OK) {
        error = nvs_set_blob(nvs, SETTINGS_KEY, record, sizeof(record));
        if (error == ESP_OK) error = nvs_commit(nvs);
        nvs_close(nvs);
    }
    portENTER_CRITICAL(&settings_lock);
    settings.stats.last_write_error = error;
    if (error == ESP_OK) {
        ++settings.stats.writes;
        if (revision == settings.stats.revision) settings.stats.dirty = false;
    } else {
        ++settings.stats.write_errors;
    }
    dirty = settings.stats.dirty;
    portEXIT_CRITICAL(&settings_lock);
    if (error != ESP_OK)
        ESP_LOGW("settings", "Settings cache write failed: %s; retry pending", esp_err_to_name(error));
    return dirty;
}

static void persistence_worker(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Retry independently of further snapshots, including duplicates. Newer
        // settings arriving during a commit are picked up on the next attempt.
        while (persist_pending()) vTaskDelay(pdMS_TO_TICKS(SETTINGS_RETRY_MS));
    }
}

esp_err_t digits_settings_init(void)
{
    if (settings.initialized) return ESP_ERR_INVALID_STATE;
    digits_line_settings_t loaded = defaults;
    esp_err_t error = load_record(&loaded);
    settings.current = error == ESP_OK ? loaded : defaults;
    settings.stats = (digits_settings_stats_t){.loaded = error == ESP_OK, .load_error = error};
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW("settings", "Settings cache unavailable: %s; using defaults", esp_err_to_name(error));
    bool worker_started = xTaskCreate(persistence_worker, "digits_settings", SETTINGS_WORKER_STACK,
                                     NULL, 2, &settings.worker) == pdPASS;
    // Runtime settings must remain usable even when persistence cannot start:
    // a server silence push is still authoritative for incoming-call audio.
    settings.initialized = true;
    if (!worker_started) {
        settings.worker = NULL;
        settings.stats.last_write_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void digits_settings_set_media_busy(bool busy)
{
    portENTER_CRITICAL(&settings_lock);
    bool was_busy = settings.stats.media_busy;
    settings.stats.media_busy = busy;
    TaskHandle_t worker = settings.worker;
    bool notify = settings.initialized && was_busy && !busy && settings.stats.dirty;
    portEXIT_CRITICAL(&settings_lock);
    if (notify && worker) xTaskNotifyGive(worker);
}

void digits_settings_transport(uint32_t session, bool ready)
{
    portENTER_CRITICAL(&settings_lock);
    if (settings.initialized && (!settings.have_session || session == settings.session ||
                                 (int32_t)(session - settings.session) > 0)) {
        if (!settings.have_session || settings.session != session || settings.ready != ready)
            ++settings.transport_revision;
        settings.have_session = true;
        settings.session = session;
        settings.ready = ready;
    }
    portEXIT_CRITICAL(&settings_lock);
}

// Bound traversal even when a message contains many unknown fields. Duplicate
// known keys are rejected instead of allowing ambiguous settings snapshots.
static bool member(const cJSON *object, const char *key, const cJSON **value)
{
    if (!cJSON_IsObject(object)) return false;
    *value = NULL;
    unsigned count = 0;
    for (const cJSON *item = object->child; item; item = item->next) {
        if (++count > SETTINGS_OBJECT_MEMBERS) return false;
        if (item->string && !strcmp(item->string, key)) {
            if (*value) return false;
            *value = item;
        }
    }
    return true;
}

bool digits_settings_receive(const cJSON *message, uint32_t session)
{
    portENTER_CRITICAL(&settings_lock);
    bool ready = settings.initialized && settings.have_session && settings.ready && settings.session == session;
    uint32_t transport_revision = settings.transport_revision;
    if (!ready) ++settings.stats.stale;
    portEXIT_CRITICAL(&settings_lock);
    if (!ready) return false;

    const cJSON *type, *body, *voice, *silent, *auto_update, *voicemail;
    digits_line_settings_t next = defaults;
    if (!member(message, "type", &type) || !cJSON_IsString(type) || strcmp(type->valuestring, "line_settings") ||
        !member(message, "line_settings", &body) || !cJSON_IsObject(body) ||
        !member(body, "voice_style", &voice) || !member(body, "silent_mode", &silent) ||
        !member(body, "auto_update", &auto_update) || !member(body, "voicemail", &voicemail)) goto invalid;
    next.voice_style = cJSON_IsString(voice) && !strcmp(voice->valuestring, "modern") ?
        DIGITS_VOICE_MODERN : DIGITS_VOICE_COPPER;
    next.silent_mode = cJSON_IsTrue(silent);
    next.auto_update = cJSON_IsTrue(auto_update);
    bool replace_voicemail = voicemail && !cJSON_IsNull(voicemail);
    if (replace_voicemail) {
        const cJSON *enabled, *timeout;
        if (!member(voicemail, "enabled", &enabled) ||
            !member(voicemail, "ring_timeout_seconds", &timeout)) goto invalid;
        next.voicemail_enabled = cJSON_IsTrue(enabled);
        if (cJSON_IsNumber(timeout) && timeout->valuedouble >= 5 && timeout->valuedouble <= 60 &&
            timeout->valuedouble == (int)timeout->valuedouble)
            next.voicemail_ring_timeout_seconds = (uint8_t)timeout->valuedouble;
    }

    portENTER_CRITICAL(&settings_lock);
    if (!settings.ready || settings.session != session || settings.transport_revision != transport_revision) {
        ++settings.stats.stale;
        portEXIT_CRITICAL(&settings_lock);
        return false;
    }
    if (!replace_voicemail) {
        next.voicemail_enabled = settings.current.voicemail_enabled;
        next.voicemail_ring_timeout_seconds = settings.current.voicemail_ring_timeout_seconds;
    }
    ++settings.stats.accepted;
    bool changed = !equal_settings(&next, &settings.current);
    // Do not rewrite unknown/corrupt records at boot. The first authoritative
    // snapshot can repair one, even when its values match fallback defaults.
    bool repair = !settings.stats.loaded && !settings.have_authoritative;
    settings.have_authoritative = true;
    if (changed || repair) {
        settings.current = next;
        ++settings.stats.revision;
        settings.stats.dirty = true;
    } else {
        ++settings.stats.duplicates;
    }
    TaskHandle_t worker = settings.worker;
    portEXIT_CRITICAL(&settings_lock);
    if ((changed || repair) && worker) xTaskNotifyGive(worker);
    return true;
invalid:
    portENTER_CRITICAL(&settings_lock);
    ++settings.stats.invalid;
    portEXIT_CRITICAL(&settings_lock);
    return false;
}

void digits_settings_get(digits_line_settings_t *snapshot, digits_settings_stats_t *stats)
{
    portENTER_CRITICAL(&settings_lock);
    if (snapshot) *snapshot = settings.current;
    if (stats) *stats = settings.stats;
    portEXIT_CRITICAL(&settings_lock);
}

bool digits_settings_silent(void)
{
    portENTER_CRITICAL(&settings_lock);
    bool silent = settings.current.silent_mode;
    portEXIT_CRITICAL(&settings_lock);
    return silent;
}
