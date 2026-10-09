#pragma once

#include "esp_err.h"

// Optional diagnostic selected in menuconfig: one two-second test per boot,
// or repeated KEY1 hold-to-record/release-to-play. Disabled builds do nothing.
// Runs outside signaling; sole consumer of microphone PCM while recording.
esp_err_t digits_record_playback_test_start(void);
