#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "audio_format.h"
#include "esp_err.h"

#define DIGITS_MIC_FRAME_SAMPLES (DIGITS_AUDIO_SAMPLE_RATE / 50) // 20 ms, 960 samples.
#define DIGITS_MIC_FRAME_DISCONTINUITY (1U << 0)

typedef struct {
    uint32_t sequence;
    uint32_t flags;
    int64_t captured_at_us; // Read completion time, not a calibrated ADC timestamp.
    int16_t pcm[DIGITS_MIC_FRAME_SAMPLES]; // Signed 16-bit, 48 kHz, mono.
    // What the speaker played at the same instants (the codec's loopback),
    // for echo cancellation.
    int16_t reference[DIGITS_MIC_FRAME_SAMPLES];
} digits_microphone_frame_t;

// Boot-time initialization after digits_audio_board_init. ADC failure leaves
// the speaker/signaling running. A dedicated task continuously drains RX.
esp_err_t digits_microphone_init(void);

// Reserve the single PCM consumer for a call or a complete recording/playback
// diagnostic (including ADC pause/resume). Never acquire from a WS callback.
// The acquiring task must release it; a busy microphone rejects another user.
bool digits_microphone_acquire(uint32_t timeout_ms);
void digits_microphone_release(void);

// Copy the next frame from a four-frame queue (in PSRAM; frames are 3.9 KB,
// keep them off small stacks). Reservation holder only; 0 = poll.
// timeout_ms is bounded to 1000. Oldest frames are dropped when no consumer
// keeps up. Sequence gaps/flags identify discontinuities for the encoder.
esp_err_t digits_microphone_read(digits_microphone_frame_t *frame, uint32_t timeout_ms);

// Changes only diagnostics, never transport clocks, capture or signaling.
// Initial state comes from menuconfig; safe to toggle from application tasks.
void digits_microphone_set_diagnostic(bool enabled);

// Checked ADC mute + stop PCM publication, while shared native RX keeps
// draining to preserve TX clocks. Resume clears stale PCM and adds warmup.
// Bounded acknowledgement from the capture task; no calls from WS callbacks.
esp_err_t digits_microphone_pause(bool paused, uint32_t timeout_ms);
