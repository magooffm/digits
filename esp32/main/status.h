#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

// Read-only status page on the home network: number, call, silence,
// answering machine, connection, memory, and the recent log. Caller numbers
// and the log need CONFIG_DIGITS_SETUP_PASSWORD (browser login prompt, any
// user name); without a configured password they are open.

// First thing at boot: starts keeping log lines for the page (PSRAM).
void digits_status_init(void);

// Registers /api/status, /logs and /api/log on the shared web server.
void digits_status_register(httpd_handle_t server);
// The status page itself; setup.c serves it at / on the home network.
esp_err_t digits_status_page(httpd_req_t *req);

// Shared with web_config.c. Authorized: the setup password was given, or
// none is configured. The challenge makes the browser ask for it.
bool digits_status_authorized(httpd_req_t *req);
esp_err_t digits_status_challenge(httpd_req_t *req);
// Quoted JSON string; control characters become spaces.
void digits_status_json_string(char *out, size_t capacity, const char *text);
