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

// Behavior follows the Pi controller (pi/digitsd/internal/phone/controller.go):
// lifting during RINGING answers and replacing the handset ends any call. A
// ring arriving while off-hook is not answered by that lift; the ringer is
// suppressed while off-hook and sounds once the handset is replaced.
phone_output_t phone_fsm_step(phone_fsm_t *fsm, const phone_inputs_t *in)
{
    phone_output_t out = {.action = PHONE_ACTION_NONE};
    if (!fsm->started) {
        fsm->started = true;
        fsm->off_hook = in->off_hook;
        for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) fsm->presses[i] = in->presses[i];
    }
    if (in->phase == PHONE_PHASE_IDLE) fsm->answered = false;

    bool lifted = in->off_hook && !fsm->off_hook;
    bool replaced = !in->off_hook && fsm->off_hook;
    fsm->off_hook = in->off_hook;

    if (replaced) {
        bool unanswered_ring = in->phase == PHONE_PHASE_RINGING && !fsm->answered;
        // answered stays set until IDLE: the call this phone took is ending.
        if (in->phase != PHONE_PHASE_IDLE && !unanswered_ring) out.action = PHONE_ACTION_HANGUP;
    } else if (lifted && in->phase == PHONE_PHASE_RINGING) {
        out.action = PHONE_ACTION_ANSWER;
        fsm->answered = true;
    }

    for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) {
        if (in->presses[i] == fsm->presses[i]) continue;
        fsm->presses[i] = in->presses[i];
        if (out.action != PHONE_ACTION_NONE) continue;
        out.action = in->off_hook && in->phase == PHONE_PHASE_IDLE ? PHONE_ACTION_CALL
                                                                   : PHONE_ACTION_IGNORED_PRESS;
        out.target = i;
    }
    return out;
}
