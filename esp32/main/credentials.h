#pragma once
#include "esp_err.h"

typedef struct {
    char hardware_id[37];
    char number[32];
    char device_token[65];
} digits_credentials_t;

esp_err_t credentials_load(digits_credentials_t *credentials);
esp_err_t credentials_save(const digits_credentials_t *credentials);
