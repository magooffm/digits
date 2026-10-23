#pragma once

#include "esp_err.h"

// Hook switch and call buttons. Start after audio-board and incoming-ring
// initialization; WebRTC may start later (commands fail until it is online).
// A no-op returning ESP_OK when CONFIG_DIGITS_PHONE is disabled.
esp_err_t digits_phone_start(void);

// Console handler for: phone status | set a|b NUMBER | clear a|b |
// hook up|down | press a|b. Returns 0 on success, 1 on usage error.
int digits_phone_console(int argc, char **argv);
