#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// WM8978 handset codec (I2C 0x1A) on I2S1: MCK GPIO3, SCK GPIO4, FSA GPIO5,
// data on GPIO6 and GPIO7, 48 kHz, 16-bit samples in 32-bit slots. Which
// data pin carries the codec's ADC output is found at boot by listening on
// each. The channels then run for good; a reader task drains the input.
//
// Routing lives in audio_board.c: while the handset route is set, the
// speaker functions play on the earpiece and the microphone task takes the
// handset microphone (digits_handset_capture).

typedef enum { HANDSET_INPUT_MIC, HANDSET_INPUT_LINE } handset_input_t;

// Boot, after the audio board. Fails without the module; the phone then
// keeps the board speaker and microphones.
esp_err_t digits_handset_init(void);
bool digits_handset_ready(void);
// GPIO carrying the codec's ADC data, -1 if not found.
int digits_handset_adc_gpio(void);
esp_err_t digits_handset_set_input(handset_input_t input);
handset_input_t digits_handset_input(void);

// For audio_board.c. The earpiece plays at `volume` 0-100 (0 mutes, 1-100
// map to -50..0 dB) while enabled.
esp_err_t digits_handset_output(bool enabled, int volume);
esp_err_t digits_handset_write(const int16_t *stereo, size_t bytes, size_t *written, uint32_t timeout_ms);

// Microphone task, once per frame: always takes `frames` mono samples from
// the handset input (so it never fills up). When `use` is set they replace
// pcm and the echo reference becomes silence.
void digits_handset_capture(int16_t *pcm, int16_t *reference, size_t frames, bool use);

// Console tests, with the handset down and no other sound playing. tone and
// echo play at the test volume.
esp_err_t digits_handset_set_test_db(int db); // -57..+6 dB.
int digits_handset_test_db(void);
esp_err_t digits_handset_tone(unsigned seconds);
esp_err_t digits_handset_mic(unsigned seconds);
esp_err_t digits_handset_echo(unsigned seconds);
void digits_handset_status(void);
