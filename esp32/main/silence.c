#include "silence.h"
#include "incoming_ring.h"
#include "line_settings.h"
#include "silence_policy.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#define TAG "silence"
#define NVS_NAMESPACE "digits_phone"
#define NVS_KEY "silence"
enum { STORED_NONE, STORED_SILENT, STORED_RINGING };

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static silence_policy_t policy;
static bool dirty;

static void save(void)
{
    portENTER_CRITICAL(&mux);
    uint8_t value = !policy.overridden ? STORED_NONE : policy.local ? STORED_SILENT : STORED_RINGING;
    dirty = false;
    portEXIT_CRITICAL(&mux);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, NVS_KEY, value);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "Saving the silence choice failed: %s", esp_err_to_name(err));
}

void digits_silence_init(void)
{
    uint8_t stored = STORED_NONE;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_KEY, &stored);
        nvs_close(nvs);
    }
    // The cached server value is the one the choice was made against.
    bool server = digits_settings_silent();
    portENTER_CRITICAL(&mux);
    policy.server = server;
    if (stored == STORED_SILENT || stored == STORED_RINGING)
        silence_policy_local(&policy, stored == STORED_SILENT);
    bool effective = silence_policy_effective(&policy);
    bool overridden = policy.overridden;
    portEXIT_CRITICAL(&mux);
    digits_incoming_ring_set_silent(effective);
    if (overridden) ESP_LOGI(TAG, "Phone set %s here, overriding the server", effective ? "silent" : "to ring");
}

void digits_silence_server(bool server)
{
    portENTER_CRITICAL(&mux);
    bool was = policy.overridden;
    silence_policy_server(&policy, server);
    if (was && !policy.overridden) dirty = true;
    bool effective = silence_policy_effective(&policy);
    portEXIT_CRITICAL(&mux);
    digits_incoming_ring_set_silent(effective);
}

static void apply_local(bool silent)
{
    portENTER_CRITICAL(&mux);
    silence_policy_local(&policy, silent);
    bool overridden = policy.overridden;
    portEXIT_CRITICAL(&mux);
    digits_incoming_ring_set_silent(silent);
    save();
    ESP_LOGI(TAG, "Incoming calls %s%s", silent ? "silent" : "ring",
             overridden ? " (local choice until the server setting changes)" : " (matches the server)");
}

void digits_silence_set(bool silent) { apply_local(silent); }

void digits_silence_toggle(void)
{
    portENTER_CRITICAL(&mux);
    bool effective = silence_policy_effective(&policy);
    portEXIT_CRITICAL(&mux);
    apply_local(!effective);
}

void digits_silence_poll(void)
{
    portENTER_CRITICAL(&mux);
    bool pending = dirty;
    portEXIT_CRITICAL(&mux);
    if (pending) save();
}

digits_silence_state_t digits_silence_state(void)
{
    portENTER_CRITICAL(&mux);
    digits_silence_state_t state = {
        .effective = silence_policy_effective(&policy),
        .server = policy.server,
        .overridden = policy.overridden,
    };
    portEXIT_CRITICAL(&mux);
    return state;
}
