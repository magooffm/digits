#pragma once

#include <stdbool.h>

// Whether incoming calls ring audibly: the server's silent flag combined
// with a local choice (silence_policy.h). The local choice is kept in NVS
// across restarts, so a reboot does not undo a pressed silence button.

// Boot: after digits_settings_init and incoming-ring init.
void digits_silence_init(void);

// Each accepted server settings snapshot. Nonblocking, no flash: safe in
// the WebSocket callback. A changed value drops the local choice.
void digits_silence_server(bool server);

// Local choice, from the console or a button task. Saves it.
void digits_silence_set(bool silent);
void digits_silence_toggle(void);

// From a task that may write flash: saves a choice the server dropped.
void digits_silence_poll(void);

typedef struct {
    bool effective, server, overridden;
} digits_silence_state_t;
digits_silence_state_t digits_silence_state(void);
