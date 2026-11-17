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

// The audio in jack (L2/R2), direct: MIC at +6 dB, LINE at 0 dB. The
// handset microphone is on the tip, the left channel; calls add the
// microphone gain of tx_voice.h.
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
// map to -34..+6 dB, after the codec's +12 dB limiter boost) while enabled.
esp_err_t digits_handset_output(bool enabled, int volume);
esp_err_t digits_handset_write(const int16_t *stereo, size_t bytes, size_t *written, uint32_t timeout_ms);

// Microphone task, once per frame: always takes `frames` mono samples from
// the handset input (so it never fills up). When `use` is set they replace
// pcm and the echo reference becomes silence.
void digits_handset_capture(int16_t *pcm, int16_t *reference, size_t frames, bool use);

// Console tests, while no other sound plays. tone and echo play at the
// test volume.
esp_err_t digits_handset_set_test_db(int db); // -57..+6 dB.
int digits_handset_test_db(void);
esp_err_t digits_handset_tone(unsigned seconds);
esp_err_t digits_handset_mic(unsigned seconds);
esp_err_t digits_handset_echo(unsigned seconds);
// Diagnostic: WM8978 input registers R44, R45 (also R46) and R47 (also R48)
// written directly; r44 < 0 restores the selected input.
esp_err_t digits_handset_test_input(int r44, int r45, int r47);
// Records the stereo input at 48 kHz (`samples` frames, left/right) into a
// PSRAM buffer the caller frees with heap_caps_free; gain adds what a call
// adds (about 30 dB). Web: GET /api/handset-record?s=SECONDS returns a WAV
// with that gain; &raw=1 without, &r44=HEX&r45=HEX&r47=HEX try input registers.
esp_err_t digits_handset_capture_raw(unsigned seconds, bool gain, int16_t **out, size_t *samples);
// Records the input, then plays it through the speaker functions.
esp_err_t digits_handset_record(unsigned seconds);
void digits_handset_status(void);
