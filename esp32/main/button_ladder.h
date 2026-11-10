#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "button_ladder_map.h"

// Ring A, ring B and speaker buttons on one ADS1115 input (I2C 0x48,
// CONFIG_DIGITS_BUTTON_LADDER_CHANNEL); see button_ladder_map.h. Without
// the chip the ladder stays off.

esp_err_t digits_button_ladder_init(void);
bool digits_button_ladder_active(void);
// BUTTON_LADDER_* bits pressed now (undebounced); one 2-byte I2C read.
esp_err_t digits_button_ladder_read(unsigned *buttons, int *mv);
