#pragma once

#include "esp_err.h"

// Start Ring Test worker after shared audio board initialization. Nonfatal.
esp_err_t digits_ring_test_init(void);

// Queue the existing Digits ring_test command; never block the WS callback.
void digits_ring_test_request(void);
