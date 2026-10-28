#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// What the other side hears from the microphone, before Opus: 48 kHz to
// 16 kHz (decimate.h), Espressif's ESP-SR echo canceller with the speaker
// reference from the codec loopback (microphone.h), then the microphone
// gain: fixed, with a limiter against clipping (ESP-SR's WebRTC AGC in its
// fixed-gain mode; the adaptive mode rose too slowly for short phrases).
// 16 kHz is the canceller's only rate and wideband voice (to about 7 kHz).
// Decimation and cancellation run in a worker task on core 1, away from
// Wi-Fi and the encoder; that costs a fixed delay (logged by create).

#define TX_VOICE_RATE 16000
#define TX_VOICE_SAMPLES (TX_VOICE_RATE / 50) // 20 ms.

typedef struct tx_voice tx_voice_t;

// Per call, from the encoder task. NULL when memory is short.
tx_voice_t *tx_voice_create(void);
void tx_voice_destroy(tx_voice_t *voice);
// mic and reference: DIGITS_MIC_FRAME_SAMPLES at 48 kHz; out: TX_VOICE_SAMPLES
// processed earlier (silence while the worker falls behind).
void tx_voice_process(tx_voice_t *voice, const int16_t *mic, const int16_t *reference, int16_t *out);

// Boot, before calls: reads the saved microphone gain.
void tx_voice_init(void);
// Microphone gain in dB, 0 to TX_VOICE_GAIN_MAX_DB; applies from the next
// frame, also mid-call. Saving writes NVS: internal-RAM stack only.
#define TX_VOICE_GAIN_MAX_DB 30
int tx_voice_gain_db(void);
esp_err_t tx_voice_set_gain_db(int db);
// Echo cancellation can be switched off for comparison; on at boot, not saved.
bool tx_voice_echo_cancel(void);
void tx_voice_set_echo_cancel(bool on);

// Console: voice [status] | voice aec on|off | voice gain DB. 0 on success.
int tx_voice_console(int argc, char **argv);
