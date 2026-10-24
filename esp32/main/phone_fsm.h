#pragma once

// Pure phone logic: input debouncing, the hook/button state machine and the
// call-progress tone it implies. No ESP-IDF dependencies, so
// tests/phone_fsm_test.c runs it on the host.

#include <stdbool.h>
#include <stdint.h>
#include "tone_gen.h"

#define PHONE_CALL_TARGETS 3
#define PHONE_DEBOUNCE_US 30000LL
// Timings follow the Pi controller and Pico firmware (GR-506 style).
#define PHONE_DIAL_TONE_TIMEOUT_US (15LL * 1000000)  // Unused dial tone.
#define PHONE_TREATMENT_DELAY_US (1LL * 1000000)     // Silence before reorder.
#define PHONE_REORDER_DURATION_US (45LL * 1000000)   // Then silent lockout.
#define PHONE_CALL_START_TIMEOUT_US (3LL * 1000000)  // Dialed call never started.

// Debounced input. presses counts confirmed inactive -> active edges. The
// first sample is taken as the stable level, so a key held at boot is not a
// press and a hook switch starts in its real position.
typedef struct {
    bool known;
    bool stable;
    bool candidate;
    int64_t candidate_at_us;
    uint32_t presses;
} phone_debounce_t;

// Returns the stable level after this sample.
bool phone_debounce_update(phone_debounce_t *input, bool active, int64_t now_us);

// Mirrors the WebRTC worker's one-to-one call phase.
typedef enum {
    PHONE_PHASE_IDLE,
    PHONE_PHASE_CALLING,
    PHONE_PHASE_RINGING,
    PHONE_PHASE_CONNECTING,
    PHONE_PHASE_CONNECTED,
} phone_phase_t;

// Mirrors digits_call_end_t.
typedef enum {
    PHONE_END_LOCAL,
    PHONE_END_REMOTE_HANGUP,
    PHONE_END_BUSY,
    PHONE_END_UNREACHABLE,
    PHONE_END_FAILED,
} phone_end_t;

typedef struct {
    int64_t now_us;
    bool off_hook;
    phone_phase_t phase;
    uint32_t presses[PHONE_CALL_TARGETS]; // Monotonic press counters.
    uint32_t ended_calls;                 // Monotonic, from the worker.
    phone_end_t last_end;
    bool last_connected;
} phone_inputs_t;

typedef enum {
    PHONE_ACTION_NONE,
    PHONE_ACTION_CALL,   // Dial target.
    PHONE_ACTION_ANSWER,
    PHONE_ACTION_HANGUP,
    PHONE_ACTION_IGNORED_PRESS, // Target pressed while a call is impossible.
} phone_action_t;

// What the handset is doing, which decides the tone.
typedef enum {
    PHONE_LINE_IDLE,    // On hook.
    PHONE_LINE_DIAL,    // Lifted, waiting for a call button: dial tone.
    PHONE_LINE_CALL,    // Calling, answering or talking: ringback while calling.
    PHONE_LINE_BUSY,    // Busy tone until the handset is replaced.
    PHONE_LINE_REORDER, // Short silence, then reorder.
    PHONE_LINE_LOCKOUT, // Silent until the handset is replaced.
} phone_line_t;

typedef struct {
    phone_action_t action;
    unsigned target;
    tone_gen_tone_t tone; // Tone to play now.
} phone_output_t;

typedef struct {
    bool started;
    bool off_hook;
    bool answered;  // ANSWER issued for the current incoming call.
    bool outgoing;  // LINE_CALL was started by a call button.
    bool call_seen; // The worker has shown this call as active.
    phone_line_t line;
    int64_t line_since_us;
    uint32_t ended_seen;
    uint32_t presses[PHONE_CALL_TARGETS];
} phone_fsm_t;

// One step per poll. Emits at most one action; hook changes win over presses
// observed in the same step, and those presses are consumed, never replayed.
phone_output_t phone_fsm_step(phone_fsm_t *fsm, const phone_inputs_t *inputs);

// The CALL action could not be issued (no number, worker rejected it):
// play the failure treatment instead of ringback.
void phone_fsm_call_rejected(phone_fsm_t *fsm, int64_t now_us);
