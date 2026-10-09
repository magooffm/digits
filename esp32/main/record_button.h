#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool valid; // False at startup and immediately after any I2C read failure.
    bool pressed; // Confirmed, active-low K1 state; meaningful only when valid.
    uint32_t press_sequence; // Increments for each confirmed released -> pressed edge.
    int64_t pressed_at_us; // First raw sample of the most recently confirmed press.
    int64_t released_at_us; // First raw sample of the most recently confirmed release.
    esp_err_t error; // Last read/init error until a stable state is established.
} digits_record_button_state_t;

// Boot-time initialization after digits_audio_board_init. Poll K1 / TCA9555
// P11 in its own task; both edges require 30 ms of stable samples. A key held
// at boot is reported pressed; consumers must require release before arming.
esp_err_t digits_record_button_start(void);

// Copy the latest state under a short critical section; never performs I2C.
// An error preserves the last confirmed values/counter while marking invalid.
void digits_record_button_snapshot(digits_record_button_state_t *state);
