#pragma once

#include "call_audio_jitter.h"
#include "esp_err.h"

typedef struct digits_call_audio digits_call_audio_t;
typedef enum {
    DIGITS_CALL_AUDIO_TX = 1,
    DIGITS_CALL_AUDIO_RX = 2,
    DIGITS_CALL_AUDIO_FULL = 3,
} digits_call_audio_mode_t;
typedef enum {
    DIGITS_CALL_AUDIO_STARTING,
    DIGITS_CALL_AUDIO_READY,
    DIGITS_CALL_AUDIO_FAILED,
    DIGITS_CALL_AUDIO_STOPPING,
    DIGITS_CALL_AUDIO_STOPPED,
} digits_call_audio_state_t;

typedef struct {
    digits_call_audio_state_t state;
    esp_err_t error;
    uint32_t capture_frames, capture_gaps, capture_discontinuities, capture_stale;
    uint32_t capture_timeouts;
    uint32_t encoded_frames, encode_errors, tx_queue_drops, tx_pop_drops;
    uint32_t sent_frames, send_errors, send_blocked;
    uint32_t max_encode_us, max_capture_age_us, max_send_age_us;
    uint32_t received_frames, rx_invalid, rx_queue_drops;
    uint32_t rx_late, rx_duplicate, rx_missing, rx_discontinuities, rx_local_skips;
    uint32_t rx_jitter_overruns, rx_queue_stale;
    uint32_t decoded_frames, decode_errors, plc_frames, silence_frames, played_frames;
    uint32_t playback_errors, playback_feed_gaps;
    uint32_t max_decode_us, max_receive_age_us, max_write_us;
    uint32_t tx_stack_min_bytes, rx_stack_min_bytes;
} digits_call_audio_stats_t;

// Owner is the peer worker. All I2S/codec work occurs in dedicated tasks.
// Queues and buffers are bounded and private to one call. Start does not wait
// for codecs or hardware; poll status and fail the call if startup fails.
// ESP32-S3 codec workers use internal 48 KiB TX / 24 KiB RX stacks. Their
// measured minima are reported after open, periodically and after cleanup.
esp_err_t digits_call_audio_start(digits_call_audio_mode_t mode, digits_call_audio_t **out);
void digits_call_audio_get_stats(digits_call_audio_t *audio, digits_call_audio_stats_t *stats);
bool digits_call_audio_pop_encoded(digits_call_audio_t *audio, call_audio_packet_t *packet);
void digits_call_audio_record_send(digits_call_audio_t *audio, const call_audio_packet_t *packet,
                                   bool success, bool blocked);
// Copy callback-owned packet memory; 0-wait queue send, never decode here.
bool digits_call_audio_receive(digits_call_audio_t *audio, uint32_t pts_ms,
                               const uint8_t *data, size_t size);
void digits_call_audio_request_stop(digits_call_audio_t *audio);
bool digits_call_audio_is_stopped(digits_call_audio_t *audio);
// Allowed only after is_stopped and after peer callbacks no longer reference it.
esp_err_t digits_call_audio_destroy(digits_call_audio_t *audio);
