#pragma once

#include <stdbool.h>

// Setup network "Digits-Setup-XXXX" with its page: Wi-Fi and server,
// pairing code, firmware upload and restart. WPA2 with the built-in
// CONFIG_DIGITS_SETUP_PASSWORD guards it; the page answers only on the setup
// network, never on the home network. wifi.c opens and closes it.

// Boot, before Wi-Fi starts.
void digits_setup_init(void);

// forced: the button, the server's recovery command or the console; stays
// open for CONFIG_DIGITS_SETUP_FORCED_MINUTES even while everything works.
void digits_setup_open(const char *reason, bool forced);
void digits_setup_close(void);
bool digits_setup_running(void);
bool digits_setup_forced(void);

// State reports from signaling (nonblocking, no flash).
void digits_setup_report_server(bool connected);
void digits_setup_report_pairing_code(const char *code);
void digits_setup_report_number(const char *number); // NULL or "": unpaired.
bool digits_setup_server_connected(void);
bool digits_setup_paired(void);

// Console only: also serve the page on the home network until restart.
void digits_setup_allow_lan(void);
