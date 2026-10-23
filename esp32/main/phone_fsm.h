#pragma once

// Pure phone logic: input debouncing and the hook/button state machine.
// No ESP-IDF dependencies, so tests/phone_fsm_test.c runs it on the host.

#include <stdbool.h>
#include <stdint.h>

#define PHONE_CALL_TARGETS 2
#define PHONE_DEBOUNCE_US 30000LL

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

typedef struct {
    bool off_hook;
    phone_phase_t phase;
    uint32_t presses[PHONE_CALL_TARGETS]; // Monotonic press counters.
} phone_inputs_t;

typedef enum {
    PHONE_ACTION_NONE,
    PHONE_ACTION_CALL,   // Dial target.
    PHONE_ACTION_ANSWER,
    PHONE_ACTION_HANGUP,
    PHONE_ACTION_IGNORED_PRESS, // Target pressed while a call is impossible.
} phone_action_t;

typedef struct {
    phone_action_t action;
    unsigned target;
} phone_output_t;

typedef struct {
    bool started;
    bool off_hook;
    bool answered; // ANSWER issued for the current incoming call.
    uint32_t presses[PHONE_CALL_TARGETS];
} phone_fsm_t;

// One step per poll. Emits at most one action; hook changes win over presses
// observed in the same step, and those presses are consumed, never replayed.
phone_output_t phone_fsm_step(phone_fsm_t *fsm, const phone_inputs_t *inputs);
