#pragma once

#include "esp_err.h"

// Development-only KEY1 hold-to-record/release-to-play worker.
// Ten seconds maximum; no continuous loopback or Digits commands.
esp_err_t digits_button_record_playback_start(void);
