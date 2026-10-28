#pragma once

#include <stdbool.h>
#include <stddef.h>

// Setup network "Digits-Setup-XXXX" with its page: Wi-Fi and server,
// pairing code, firmware upload and restart. WPA2 with the built-in
// CONFIG_DIGITS_SETUP_PASSWORD guards it; the page answers only on the setup
// network, never on the home network. wifi.c opens and closes it.

// Boot, before Wi-Fi starts.
void digits_setup_init(void);
// After Wi-Fi started: the web server with the status page (status.h);
// opening the setup network adds the setup page.
void digits_setup_start_web(void);

// The Wi-Fi supervisor opens and closes the network. A request (button,
// the server's recovery command, console) keeps it open for
// CONFIG_DIGITS_SETUP_FORCED_MINUTES even while everything works; it only
// sets a flag, so it is safe in callbacks. The supervisor opens it within a
// second and returns the reason from digits_setup_requested().
void digits_setup_request(const char *reason);
const char *digits_setup_requested(void); // NULL when there is none.
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
void digits_setup_number(char *out, size_t capacity); // "" when unpaired.

// Console only: also serve the page on the home network until restart.
void digits_setup_allow_lan(void);
