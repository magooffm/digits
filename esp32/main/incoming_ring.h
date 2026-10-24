#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Boot-only: call after audio-board and Ring Test initialization (the latter
// prepares the shared, immutable ring_tone table before workers can render).
esp_err_t digits_incoming_ring_init(void);

// Nonblocking control. A nonzero ID identifies one incoming call; repeating
// the same active ID is idempotent, including its silent-until-hangup latch.
// A new ID starts a fresh cadence using the current policy. ringtone selects
// a ringtones.h entry: 0 is the standard 2 s on / 4 s off ring, a melody plays
// once per cycle followed by a 2 s pause. Unknown values use the standard ring.
void digits_incoming_ring_start(uint32_t call_id, unsigned ringtone);
void digits_incoming_ring_stop(void);
void digits_incoming_ring_set_silent(bool silent);
// Handset lifted: no tone while true. Unlike silent, this is not latched;
// clearing it while a call still rings restarts that call's cadence.
void digits_incoming_ring_set_suppressed(bool suppressed);

// After stop(), true acknowledges no speaker ownership or in-flight acquire,
// start or write from that call. A later start() can of course acquire again.
// Cleanup runs on the worker: hardware gate first, then the board's bounded
// silence flush. This is an ownership acknowledgment, not acoustic proof.
bool digits_incoming_ring_is_stopped(void);
