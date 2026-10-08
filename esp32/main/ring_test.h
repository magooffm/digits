#pragma once

#include "esp_err.h"

// Initialize speaker output once, before signaling starts. Failure is nonfatal.
esp_err_t digits_ring_test_init(void);

// Queue the existing Digits ring_test command; never block the WS callback.
void digits_ring_test_request(void);
