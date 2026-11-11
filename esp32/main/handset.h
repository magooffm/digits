#pragma once

#include <stdbool.h>
#include "esp_err.h"

// WM8978 handset codec (I2C 0x1A) on I2S1: MCK GPIO3, SCK GPIO4, FSA GPIO5,
// data on GPIO6 and GPIO7. Which data pin carries the codec's ADC output is
// found by listening on each. Test stage: nothing runs at boot and the I2S
// channels exist only during a test, from the console task.

typedef enum { HANDSET_INPUT_MIC, HANDSET_INPUT_LINE } handset_input_t;

// Resets and configures the codec, then finds the data direction (once).
esp_err_t digits_handset_init(void);
bool digits_handset_ready(void);
// GPIO carrying the codec's ADC data, -1 if not found.
int digits_handset_adc_gpio(void);
esp_err_t digits_handset_set_input(handset_input_t input);
handset_input_t digits_handset_input(void);
// Headphone output -57..+6 dB in 1 dB steps.
esp_err_t digits_handset_set_output_db(int db);
int digits_handset_output_db(void);

// Blocking tests; each prints its own progress.
esp_err_t digits_handset_tone(unsigned seconds);
esp_err_t digits_handset_mic(unsigned seconds);
esp_err_t digits_handset_echo(unsigned seconds);
