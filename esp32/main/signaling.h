#pragma once
#include "credentials.h"

// Blocking owner loop. Message dispatch is isolated here for a later phone FSM.
void digits_signaling_run(digits_credentials_t *credentials);
