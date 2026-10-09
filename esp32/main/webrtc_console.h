#pragma once

#include "esp_err.h"

// Development-only USB Serial/JTAG console. Commands enqueue peer operations;
// this task never owns a peer, codec, microphone frame or signaling transport.
// When disabled in menuconfig, this is a no-op returning ESP_OK.
esp_err_t digits_webrtc_console_start(void);
