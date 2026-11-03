#pragma once
#include "credentials.h"

// Blocking owner loop. Message dispatch is isolated here for a later phone FSM.
void digits_signaling_run(digits_credentials_t *credentials);
// Ends a call, closes the server connection cleanly, then restarts. Any task;
// returns at once, the restart follows after about half a second.
void digits_signaling_restart(void);
