#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#define DIGITS_WEBRTC_MAX_SIGNALING_MESSAGE 16384
#define DIGITS_WEBRTC_MAX_SDP 12288
#define DIGITS_WEBRTC_MAX_ICE 512

// The callback copies/queues JSON for the specified WebSocket session. Success
// means queue acceptance, not delivery or a connected peer. It must not block.
typedef esp_err_t (*digits_webrtc_send_fn)(const cJSON *message,
                                         uint32_t session, void *context);

esp_err_t digits_webrtc_init(digits_webrtc_send_fn send, void *context);

// Nonblocking latest-state snapshot, independent of the bounded message queue.
// Every disconnect invalidates the old session's peer and queued work.
void digits_webrtc_transport(uint32_t session, bool online,
                             const char *number, bool paired);

// Copies recognized one-to-one signaling into a bounded queue. No peer or
// WebSocket calls run here. Returns false for messages handled elsewhere.
bool digits_webrtc_receive(const cJSON *message, uint32_t session);

// Development commands: call NUMBER, answer, hangup, status, audio tx|rx|full.
// Audio mode changes are allowed only while idle with completed cleanup.
// Queue acceptance
// is reported immediately; the owner task logs the actual result.
esp_err_t digits_webrtc_command(const char *verb, const char *number);

// One-to-one call phase, in the worker's own order.
typedef enum {
    DIGITS_CALL_IDLE,
    DIGITS_CALL_CALLING,
    DIGITS_CALL_RINGING,
    DIGITS_CALL_CONNECTING,
    DIGITS_CALL_CONNECTED,
} digits_call_phase_t;

typedef struct {
    digits_call_phase_t phase;
    bool online;
    bool paired;
    char remote[32];
} digits_webrtc_call_state_t;

// Nonblocking copy of the state the worker published after its last loop.
// It can lag a just-queued command by one worker iteration.
void digits_webrtc_call_state(digits_webrtc_call_state_t *state);
