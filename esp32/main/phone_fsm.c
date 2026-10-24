#include "phone_fsm.h"

bool phone_debounce_update(phone_debounce_t *input, bool active, int64_t now_us)
{
    if (!input->known) {
        input->known = true;
        input->stable = input->candidate = active;
        input->candidate_at_us = now_us;
        return active;
    }
    if (active != input->candidate) {
        input->candidate = active;
        input->candidate_at_us = now_us;
    }
    if (input->candidate != input->stable && now_us - input->candidate_at_us >= PHONE_DEBOUNCE_US) {
        input->stable = input->candidate;
        if (input->stable) ++input->presses;
    }
    return input->stable;
}

static void enter(phone_fsm_t *fsm, phone_line_t line, int64_t now_us)
{
    fsm->line = line;
    fsm->line_since_us = now_us;
}

static void enter_call(phone_fsm_t *fsm, const phone_inputs_t *in, bool outgoing)
{
    enter(fsm, PHONE_LINE_CALL, in->now_us);
    fsm->outgoing = outgoing;
    fsm->call_seen = in->phase != PHONE_PHASE_IDLE;
    fsm->ended_seen = in->ended_calls;
}

void phone_fsm_call_rejected(phone_fsm_t *fsm, int64_t now_us)
{
    if (fsm->line == PHONE_LINE_CALL && !fsm->call_seen) enter(fsm, PHONE_LINE_REORDER, now_us);
}

static tone_gen_tone_t line_tone(const phone_fsm_t *fsm, const phone_inputs_t *in)
{
    switch (fsm->line) {
    case PHONE_LINE_DIAL:
        return TONE_GEN_DIAL;
    case PHONE_LINE_CALL:
        // Like the Pi, ringback starts with the call request itself.
        return fsm->outgoing && (in->phase == PHONE_PHASE_CALLING ||
                                 (in->phase == PHONE_PHASE_IDLE && !fsm->call_seen))
                   ? TONE_GEN_RINGBACK : TONE_GEN_NONE;
    case PHONE_LINE_BUSY:
        return TONE_GEN_BUSY;
    case PHONE_LINE_REORDER:
        return in->now_us - fsm->line_since_us >= PHONE_TREATMENT_DELAY_US ? TONE_GEN_REORDER
                                                                            : TONE_GEN_NONE;
    default:
        return TONE_GEN_NONE;
    }
}

// Behavior follows the Pi controller (pi/digitsd/internal/phone/controller.go):
// lifting during RINGING answers and replacing the handset ends any call. A
// ring arriving while off-hook is not answered by that lift; the ringer is
// suppressed while off-hook and sounds once the handset is replaced. Busy
// answers and a far end leaving while ringing out play busy; any other end
// plays reorder after a short silence. Buttons work only from dial tone.
phone_output_t phone_fsm_step(phone_fsm_t *fsm, const phone_inputs_t *in)
{
    phone_output_t out = {.action = PHONE_ACTION_NONE};
    if (!fsm->started) {
        fsm->started = true;
        fsm->off_hook = in->off_hook;
        for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) fsm->presses[i] = in->presses[i];
        fsm->ended_seen = in->ended_calls;
        if (!in->off_hook) enter(fsm, PHONE_LINE_IDLE, in->now_us);
        else if (in->phase != PHONE_PHASE_IDLE) enter_call(fsm, in, in->phase == PHONE_PHASE_CALLING);
        else enter(fsm, PHONE_LINE_DIAL, in->now_us);
    }
    if (in->phase == PHONE_PHASE_IDLE) fsm->answered = false;

    bool lifted = in->off_hook && !fsm->off_hook;
    bool replaced = !in->off_hook && fsm->off_hook;
    fsm->off_hook = in->off_hook;

    if (replaced) {
        bool unanswered_ring = in->phase == PHONE_PHASE_RINGING && !fsm->answered;
        // answered stays set until IDLE: the call this phone took is ending.
        if (in->phase != PHONE_PHASE_IDLE && !unanswered_ring) out.action = PHONE_ACTION_HANGUP;
        enter(fsm, PHONE_LINE_IDLE, in->now_us);
    } else if (lifted) {
        if (in->phase == PHONE_PHASE_RINGING) {
            out.action = PHONE_ACTION_ANSWER;
            fsm->answered = true;
            enter_call(fsm, in, false);
        } else if (in->phase != PHONE_PHASE_IDLE) {
            enter_call(fsm, in, in->phase == PHONE_PHASE_CALLING); // Console-started call.
        } else {
            enter(fsm, PHONE_LINE_DIAL, in->now_us);
        }
    }

    if (fsm->line == PHONE_LINE_CALL) {
        if (in->ended_calls != fsm->ended_seen) {
            fsm->ended_seen = in->ended_calls;
            bool busy = in->last_end == PHONE_END_BUSY ||
                        (in->last_end == PHONE_END_REMOTE_HANGUP && !in->last_connected);
            enter(fsm, busy ? PHONE_LINE_BUSY : PHONE_LINE_REORDER, in->now_us);
        } else if (in->phase != PHONE_PHASE_IDLE) {
            fsm->call_seen = true;
        } else if (!fsm->call_seen && in->now_us - fsm->line_since_us >= PHONE_CALL_START_TIMEOUT_US) {
            enter(fsm, PHONE_LINE_REORDER, in->now_us);
        }
    } else {
        fsm->ended_seen = in->ended_calls;
    }
    if (fsm->line == PHONE_LINE_DIAL && in->now_us - fsm->line_since_us >= PHONE_DIAL_TONE_TIMEOUT_US)
        enter(fsm, PHONE_LINE_REORDER, in->now_us);
    if (fsm->line == PHONE_LINE_REORDER &&
        in->now_us - fsm->line_since_us >= PHONE_TREATMENT_DELAY_US + PHONE_REORDER_DURATION_US)
        enter(fsm, PHONE_LINE_LOCKOUT, in->now_us);

    for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) {
        if (in->presses[i] == fsm->presses[i]) continue;
        fsm->presses[i] = in->presses[i];
        if (out.action != PHONE_ACTION_NONE) continue;
        out.target = i;
        if (in->off_hook && fsm->line == PHONE_LINE_DIAL && in->phase == PHONE_PHASE_IDLE) {
            out.action = PHONE_ACTION_CALL;
            enter_call(fsm, in, true);
        } else {
            out.action = PHONE_ACTION_IGNORED_PRESS;
        }
    }
    out.tone = line_tone(fsm, in);
    return out;
}
