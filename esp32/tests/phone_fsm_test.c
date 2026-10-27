// Host test for the pure phone logic. esp32/tests/run_host_tests.sh builds
// and runs it, normally and under ASan/UBSan.
#include "phone_fsm.h"
#include <assert.h>
#include <stdio.h>

static phone_fsm_t fsm;
static phone_inputs_t in;

static phone_output_t step(void) { return phone_fsm_step(&fsm, &in); }

static void end_call(phone_end_t why, bool connected)
{
    ++in.ended_calls;
    in.last_end = why;
    in.last_connected = connected;
}

static void at(int64_t seconds_x10) { in.now_us = seconds_x10 * 100000LL; } // Tenths of seconds.

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
    end_call(PHONE_END_REMOTE_HANGUP, true);
    assert(step().action == PHONE_ACTION_NONE);
    in.presses[0] = 1; // Like the Pi: hang up first, no redial from reorder.
    assert(step().action == PHONE_ACTION_IGNORED_PRESS);
    in.off_hook = false;
    assert(step().action == PHONE_ACTION_NONE); // Nothing left to end.
    in.off_hook = true;
    assert(step().tone == TONE_GEN_DIAL);
    in.presses[0] = 2;
    assert(step().action == PHONE_ACTION_CALL);
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

static void test_tones_outgoing_and_remote_hangup(void)
{
    reset(false, PHONE_PHASE_IDLE);
    assert(step().tone == TONE_GEN_NONE);
    in.off_hook = true;
    assert(step().tone == TONE_GEN_DIAL);
    in.presses[0] = 1;
    phone_output_t out = step();
    assert(out.action == PHONE_ACTION_CALL && out.tone == TONE_GEN_RINGBACK); // Before the worker moves.
    in.phase = PHONE_PHASE_CALLING;
    assert(step().tone == TONE_GEN_RINGBACK);
    in.phase = PHONE_PHASE_CONNECTING;
    assert(step().tone == TONE_GEN_NONE); // Answered: speaker free for call audio.
    in.phase = PHONE_PHASE_CONNECTED;
    assert(step().tone == TONE_GEN_NONE);
    at(100);
    in.phase = PHONE_PHASE_IDLE;
    end_call(PHONE_END_REMOTE_HANGUP, true);
    assert(step().tone == TONE_GEN_NONE); // Treatment delay.
    at(109);
    assert(step().tone == TONE_GEN_NONE);
    at(110);
    assert(step().tone == TONE_GEN_REORDER);
    at(100 + 459);
    assert(step().tone == TONE_GEN_REORDER);
    at(100 + 460);
    assert(step().tone == TONE_GEN_NONE && fsm.line == PHONE_LINE_LOCKOUT);
    in.off_hook = false;
    assert(step().tone == TONE_GEN_NONE && fsm.line == PHONE_LINE_IDLE);
}

static void test_tones_busy(void)
{
    reset(true, PHONE_PHASE_IDLE);
    in.presses[1] = 1;
    assert(step().action == PHONE_ACTION_CALL);
    in.phase = PHONE_PHASE_CALLING;
    assert(step().tone == TONE_GEN_RINGBACK);
    in.phase = PHONE_PHASE_IDLE;
    end_call(PHONE_END_BUSY, false);
    assert(step().tone == TONE_GEN_BUSY); // No delay for busy.
    at(600);
    assert(step().tone == TONE_GEN_BUSY); // Until the handset is replaced.
    in.presses[1] = 2;
    assert(step().action == PHONE_ACTION_IGNORED_PRESS);
    in.off_hook = false;
    assert(step().tone == TONE_GEN_NONE);
}

static void test_tones_far_end_gone_while_ringing_out(void)
{
    reset(true, PHONE_PHASE_IDLE);
    in.presses[0] = 1;
    step();
    in.phase = PHONE_PHASE_CALLING;
    step();
    in.phase = PHONE_PHASE_IDLE;
    end_call(PHONE_END_REMOTE_HANGUP, false);
    assert(step().tone == TONE_GEN_BUSY);
}

static void test_tones_unreachable_before_first_poll(void)
{
    reset(true, PHONE_PHASE_IDLE);
    in.presses[0] = 1;
    step();
    // Server error ended the call before the phone ever saw CALLING.
    end_call(PHONE_END_UNREACHABLE, false);
    assert(step().tone == TONE_GEN_NONE && fsm.line == PHONE_LINE_REORDER);
    at(10);
    assert(step().tone == TONE_GEN_REORDER);
}

static void test_tones_call_never_started(void)
{
    reset(true, PHONE_PHASE_IDLE);
    in.presses[0] = 1;
    step();
    at(29);
    assert(step().tone == TONE_GEN_RINGBACK);
    at(30); // Worker never took the call.
    assert(step().tone == TONE_GEN_NONE && fsm.line == PHONE_LINE_REORDER);
    at(40);
    assert(step().tone == TONE_GEN_REORDER);
}

static void test_tones_call_rejected(void)
{
    reset(true, PHONE_PHASE_IDLE);
    in.presses[0] = 1;
    assert(step().action == PHONE_ACTION_CALL);
    phone_fsm_call_rejected(&fsm, in.now_us);
    assert(step().tone == TONE_GEN_NONE && fsm.line == PHONE_LINE_REORDER);
    at(10);
    assert(step().tone == TONE_GEN_REORDER);
    phone_fsm_call_rejected(&fsm, in.now_us); // Ignored outside a starting call.
    assert(fsm.line == PHONE_LINE_REORDER && step().tone == TONE_GEN_REORDER);
}

static void test_tones_dial_timeout(void)
{
    reset(false, PHONE_PHASE_IDLE);
    at(10);
    in.off_hook = true;
    assert(step().tone == TONE_GEN_DIAL);
    at(10 + 149);
    assert(step().tone == TONE_GEN_DIAL);
    at(10 + 150);
    assert(step().tone == TONE_GEN_NONE);
    at(10 + 160);
    assert(step().tone == TONE_GEN_REORDER);
    in.presses[0] = 1;
    assert(step().action == PHONE_ACTION_IGNORED_PRESS);
}

static void test_tones_incoming(void)
{
    reset(false, PHONE_PHASE_RINGING);
    assert(step().tone == TONE_GEN_NONE); // The ringer, not a tone, while on-hook.
    in.off_hook = true;
    phone_output_t out = step();
    assert(out.action == PHONE_ACTION_ANSWER && out.tone == TONE_GEN_NONE);
    in.phase = PHONE_PHASE_CONNECTING;
    assert(step().tone == TONE_GEN_NONE);
    in.phase = PHONE_PHASE_CONNECTED;
    assert(step().tone == TONE_GEN_NONE);
    in.phase = PHONE_PHASE_IDLE;
    end_call(PHONE_END_REMOTE_HANGUP, true);
    step();
    at(10);
    assert(step().tone == TONE_GEN_REORDER);
}

static void test_tones_foreign_ring_keeps_dial_tone(void)
{
    reset(true, PHONE_PHASE_IDLE);
    in.phase = PHONE_PHASE_RINGING;
    assert(step().tone == TONE_GEN_DIAL);
    in.phase = PHONE_PHASE_IDLE; // Caller gave up.
    end_call(PHONE_END_REMOTE_HANGUP, false);
    assert(step().tone == TONE_GEN_DIAL && fsm.line == PHONE_LINE_DIAL);
    in.presses[0] = 1;
    assert(step().action == PHONE_ACTION_CALL);
}

static void test_tones_lift_during_console_call(void)
{
    reset(false, PHONE_PHASE_IDLE);
    in.phase = PHONE_PHASE_CALLING; // Started with 'webrtc call' while on-hook.
    assert(step().tone == TONE_GEN_NONE);
    in.off_hook = true;
    assert(step().tone == TONE_GEN_RINGBACK && fsm.line == PHONE_LINE_CALL);
}

static void test_lift_takes_over_answering_machine(void)
{
    reset(false, PHONE_PHASE_RINGING);
    in.phase = PHONE_PHASE_CONNECTING; // The machine answered while on hook.
    assert(step().action == PHONE_ACTION_NONE);
    in.phase = PHONE_PHASE_CONNECTED;
    assert(step().action == PHONE_ACTION_NONE && fsm.line == PHONE_LINE_IDLE);
    in.off_hook = true;
    phone_output_t out = step();
    assert(out.action == PHONE_ACTION_ANSWER && fsm.answered && fsm.line == PHONE_LINE_CALL);
    assert(out.tone == TONE_GEN_NONE);
    in.off_hook = false;
    assert(step().action == PHONE_ACTION_HANGUP);
    // Lifting while a console call still rings out only plays ringback.
    reset(false, PHONE_PHASE_CALLING);
    in.off_hook = true;
    assert(step().action == PHONE_ACTION_NONE);
}

static void test_boot_off_hook(void)
{
    reset(true, PHONE_PHASE_IDLE);
    assert(step().tone == TONE_GEN_DIAL);
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
    test_tones_outgoing_and_remote_hangup();
    test_tones_busy();
    test_tones_far_end_gone_while_ringing_out();
    test_tones_unreachable_before_first_poll();
    test_tones_call_never_started();
    test_tones_call_rejected();
    test_tones_dial_timeout();
    test_tones_incoming();
    test_tones_foreign_ring_keeps_dial_tone();
    test_tones_lift_during_console_call();
    test_lift_takes_over_answering_machine();
    test_boot_off_hook();
    puts("phone_fsm_test: all cases passed");
    return 0;
}
