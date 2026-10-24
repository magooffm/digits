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

// Like digits_webrtc_command("call", number), and asks the callee to ring
// with ringtones.h entry `ringtone`. The choice travels as a session-level
// SDP attribute (DIGITS_RINGTONE_SDP_ATTR) in the offer; the unchanged server
// relays it, and phones that do not know it ignore it.
#define DIGITS_RINGTONE_SDP_ATTR "a=x-digits-ringtone:"
esp_err_t digits_webrtc_call(const char *number, unsigned ringtone);

// Picks the ringtone for an incoming call from `caller`; offered is the
// caller's choice, or -1 for none. Runs on the peer worker when ringing
// starts: it must be quick and nonblocking. Without a resolver the caller's
// choice is used, or the standard ring.
typedef unsigned (*digits_ringtone_resolver_fn)(const char *caller, int offered);
void digits_webrtc_set_ringtone_resolver(digits_ringtone_resolver_fn resolver);

// One-to-one call phase, in the worker's own order.
typedef enum {
    DIGITS_CALL_IDLE,
    DIGITS_CALL_CALLING,
    DIGITS_CALL_RINGING,
    DIGITS_CALL_CONNECTING,
    DIGITS_CALL_CONNECTED,
} digits_call_phase_t;

// Why the most recent call ended.
typedef enum {
    DIGITS_CALL_END_LOCAL,         // This device hung up.
    DIGITS_CALL_END_REMOTE_HANGUP, // Peer sent hangup.
    DIGITS_CALL_END_BUSY,          // Peer or server reported busy.
    DIGITS_CALL_END_UNREACHABLE,   // Server error, e.g. "phone not connected".
    DIGITS_CALL_END_FAILED,        // Timeouts, transport, setup or signaling loss.
} digits_call_end_t;

typedef struct {
    digits_call_phase_t phase;
    bool online;
    bool paired;
    char remote[32];
    uint32_t ended_calls;      // Increments once per ended call.
    digits_call_end_t last_end;
    bool last_connected;       // The ended call had reached CONNECTED.
} digits_webrtc_call_state_t;

// Nonblocking copy of the state the worker published after its last loop.
// It can lag a just-queued command by one worker iteration.
void digits_webrtc_call_state(digits_webrtc_call_state_t *state);
