#include "credentials.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

// Explicit byte-array layout; number and token are one power-loss-safe NVS value.
typedef struct {
    uint8_t version;
    char number[32];
    char token[65];
} pairing_record_t;

static bool valid_uuid(const char *s)
{
    if (strlen(s) != 36 || s[14] != '4' || !strchr("89ab", s[19])) return false;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!strchr("0123456789abcdef", s[i])) return false;
    }
    return true;
}

esp_err_t credentials_load(digits_credentials_t *c)
{
    memset(c, 0, sizeof(*c));
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("digits", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    size_t length = sizeof(c->hardware_id);
    err = nvs_get_str(nvs, "hardware_id", c->hardware_id, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t b[16];
        esp_fill_random(b, sizeof(b)); // Wi-Fi is started before this call.
        b[6] = (b[6] & 0x0f) | 0x40;
        b[8] = (b[8] & 0x3f) | 0x80;
        snprintf(c->hardware_id, sizeof(c->hardware_id),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        err = nvs_set_str(nvs, "hardware_id", c->hardware_id);
        if (err == ESP_OK) err = nvs_commit(nvs);
        if (err == ESP_OK) ESP_LOGI("credentials", "Generated persistent hardware_id=%s", c->hardware_id);
    }
    if (err == ESP_OK && !valid_uuid(c->hardware_id)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        pairing_record_t record = {0};
        length = sizeof(record);
        err = nvs_get_blob(nvs, "pairing", &record, &length);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        else if (err == ESP_OK) {
            if (length != sizeof(record) || record.version != 1 ||
                !memchr(record.number, 0, sizeof(record.number)) ||
                !memchr(record.token, 0, sizeof(record.token)) ||
                !record.number[0] || !record.token[0]) err = ESP_ERR_INVALID_STATE;
            else {
                strcpy(c->number, record.number);
                strcpy(c->device_token, record.token);
            }
        }
    }
    nvs_close(nvs);
    return err;
}

esp_err_t credentials_save(const digits_credentials_t *c)
{
    pairing_record_t record = {.version = 1};
    memcpy(record.number, c->number, sizeof(record.number));
    memcpy(record.token, c->device_token, sizeof(record.token));
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("digits", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, "pairing", &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
