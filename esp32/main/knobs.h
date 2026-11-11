#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// The volume fader on an ESP32 ADC1 pin (CONFIG_DIGITS_VOLUME_KNOB_GPIO,
// default GPIO9, header pin 12) sets the speaker volume.

// Boot-only, after the audio board. Applies the fader's position at once.
esp_err_t digits_knobs_init(void);
// Call often from one task (the phone task's poll); reads every 50 ms.
void digits_knobs_poll(int64_t now_us);
// The knob sets the speaker volume; the status page then only shows it.
bool digits_knobs_volume_active(void);
// Last reading in millivolts, -1 before the first one.
int digits_knobs_volume_mv(void);
// Serial trace (off at boot): each reading that moved 5 mV or set the volume,
// and read errors. Printed with printf, so it stays out of the web log.
void digits_knobs_set_trace(bool on);
bool digits_knobs_trace(void);
unsigned digits_knobs_read_errors(void);
