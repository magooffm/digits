// Host test for the pure phone logic. esp32/tests/run_host_tests.sh builds
// and runs it, normally and under ASan/UBSan.
#include "phone_fsm.h"
#include <assert.h>
#include <stdio.h>

static phone_fsm_t fsm;
static phone_inputs_t in;

static phone_output_t step(void) { return phone_fsm_step(&fsm, &in); }

static void reset(bool off_hook, phone_phase_t phase)
{
    fsm = (phone_fsm_t){0};
    in = (phone_inputs_t){.off_hook = off_hook, .phase = phase};
    phone_output_t out = step();
    assert(out.action == PHONE_ACTION_NONE);
}

static void test_debounce(void)
{
    phone_debounce_t d = {0};
    assert(phone_debounce_update(&d, true, 0)); // Held at boot: level, no press.
    assert(d.presses == 0);
    assert(phone_debounce_update(&d, false, 1000));
    assert(!phone_debounce_update(&d, false, 1000 + PHONE_DEBOUNCE_US));
    // Bounce shorter than the window is ignored.
    phone_debounce_update(&d, true, 100000);
    phone_debounce_update(&d, false, 110000);
    assert(!phone_debounce_update(&d, false, 200000));
    assert(d.presses == 0);
    // Stable press counts once, release does not count.
    phone_debounce_update(&d, true, 300000);
    assert(!phone_debounce_update(&d, true, 300000 + PHONE_DEBOUNCE_US - 1));
    assert(phone_debounce_update(&d, true, 300000 + PHONE_DEBOUNCE_US));
    assert(phone_debounce_update(&d, true, 400000));
    assert(d.presses == 1);
    phone_debounce_update(&d, false, 500000);
    assert(!phone_debounce_update(&d, false, 600000));
    assert(d.presses == 1);
}

static void test_outgoing_call(void)
{
    reset(false, PHONE_PHASE_IDLE);
    in.presses[0] = 1; // Pressed on-hook: ignored.
    phone_output_t out = step();
    assert(out.action == PHONE_ACTION_IGNORED_PRESS && out.target == 0);
    in.off_hook = true;
    assert(step().action == PHONE_ACTION_NONE); // Lift alone does nothing.
    in.presses[1] = 1;
    out = step();
    assert(out.action == PHONE_ACTION_CALL && out.target == 1);
    in.phase = PHONE_PHASE_CALLING;
    in.presses[0] = 2; // Second button during a call: ignored.
    assert(step().action == PHONE_ACTION_IGNORED_PRESS);
    in.phase = PHONE_PHASE_CONNECTED;
    assert(step().action == PHONE_ACTION_NONE);
    in.off_hook = false;
    assert(step().action == PHONE_ACTION_HANGUP);
    in.phase = PHONE_PHASE_IDLE;
    assert(step().action == PHONE_ACTION_NONE);
}

static void test_hangup_while_calling(void)
{
    reset(true, PHONE_PHASE_CALLING);
    in.off_hook = false;
    assert(step().action == PHONE_ACTION_HANGUP);
}

static void test_remote_hangup_then_redial(void)
{
    reset(true, PHONE_PHASE_CONNECTED);
    in.phase = PHONE_PHASE_IDLE; // Remote hung up, handset still lifted.
    assert(step().action == PHONE_ACTION_NONE);
    in.presses[0] = 1;
    assert(step().action == PHONE_ACTION_CALL);
    in.off_hook = false; // Replaced before the phase updated: nothing to end.
    assert(step().action == PHONE_ACTION_NONE);
}

static void test_incoming_answer(void)
{
    reset(false, PHONE_PHASE_IDLE);
    in.phase = PHONE_PHASE_RINGING;
    phone_output_t out = step();
    assert(out.action == PHONE_ACTION_NONE);
    in.off_hook = true;
    out = step();
    assert(out.action == PHONE_ACTION_ANSWER);
    // Offer may still be pending: phase stays RINGING, ringer stays off.
    out = step();
    assert(out.action == PHONE_ACTION_NONE);
    in.phase = PHONE_PHASE_CONNECTING;
    assert(step().action == PHONE_ACTION_NONE);
    in.phase = PHONE_PHASE_CONNECTED;
    in.off_hook = false;
    assert(step().action == PHONE_ACTION_HANGUP);
}

static void test_answered_before_offer_then_replaced(void)
{
    reset(false, PHONE_PHASE_RINGING);
    in.off_hook = true;
    assert(step().action == PHONE_ACTION_ANSWER);
    in.off_hook = false; // Still RINGING locally, but already answered.
    phone_output_t out = step();
    assert(out.action == PHONE_ACTION_HANGUP);
}

static void test_ring_while_off_hook(void)
{
    reset(true, PHONE_PHASE_IDLE);
    in.phase = PHONE_PHASE_RINGING;
    phone_output_t out = step();
    assert(out.action == PHONE_ACTION_NONE); // Silent, not answered.
    in.presses[0] = 1;
    assert(step().action == PHONE_ACTION_IGNORED_PRESS);
    in.off_hook = false; // Replaced: no hangup; the ringer is unsuppressed.
    out = step();
    assert(out.action == PHONE_ACTION_NONE);
    in.off_hook = true;
    out = step();
    assert(out.action == PHONE_ACTION_ANSWER);
}

static void test_caller_gives_up(void)
{
    reset(false, PHONE_PHASE_RINGING);
    in.phase = PHONE_PHASE_IDLE;
    assert(step().action == PHONE_ACTION_NONE);
    in.off_hook = true; // Lift after the caller gave up: nothing to answer.
    assert(step().action == PHONE_ACTION_NONE);
}

static void test_failed_answer_resets(void)
{
    reset(false, PHONE_PHASE_RINGING);
    in.off_hook = true;
    assert(step().action == PHONE_ACTION_ANSWER);
    in.phase = PHONE_PHASE_IDLE; // Connect timeout.
    assert(step().action == PHONE_ACTION_NONE);
    in.off_hook = false;
    assert(step().action == PHONE_ACTION_NONE);
    in.phase = PHONE_PHASE_RINGING; // A new call is answered normally.
    assert(step().action == PHONE_ACTION_NONE);
    in.off_hook = true;
    assert(step().action == PHONE_ACTION_ANSWER);
}

static void test_hook_beats_press(void)
{
    reset(false, PHONE_PHASE_RINGING);
    in.off_hook = true;
    in.presses[0] = 1;
    assert(step().action == PHONE_ACTION_ANSWER);
    assert(step().action == PHONE_ACTION_NONE); // Press consumed, not replayed.
}

int main(void)
{
    test_debounce();
    test_outgoing_call();
    test_hangup_while_calling();
    test_remote_hangup_then_redial();
    test_incoming_answer();
    test_answered_before_offer_then_replaced();
    test_ring_while_off_hook();
    test_caller_gives_up();
    test_failed_answer_resets();
    test_hook_beats_press();
    puts("phone_fsm_test: all cases passed");
    return 0;
}
