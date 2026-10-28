#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "wifi_setup_form.h"

// Station Wi-Fi and the saved network/server (NVS namespace digits_wifi;
// the menuconfig values are only a fallback). It also decides when the
// setup network (setup.h) is open: without a configured network or server,
// while unpaired, or after CONFIG_DIGITS_WIFI_SETUP_AFTER_S seconds without
// reaching the Wi-Fi or the server. It closes again once all of that works,
// unless it was opened on purpose.

typedef struct {
    char ssid[WIFI_SETUP_SSID_BYTES];
    char password[WIFI_SETUP_PASSWORD_BYTES];
    char url[WIFI_SETUP_URL_BYTES];
    bool saved; // From NVS, not the build fallback.
} digits_wifi_config_t;

void digits_wifi_start(void);
void digits_wifi_wait_connected(void);
bool digits_wifi_online(void);
int digits_wifi_rssi(void); // 0 when not connected.

void digits_wifi_config(digits_wifi_config_t *out);
// Server address for this boot; "" when none is configured.
const char *digits_wifi_server_url(void);
// Saves for the next boot; the caller restarts.
esp_err_t digits_wifi_save(const char *ssid, const char *password, const char *url);

// Console: wifi status | set SSID [PASSWORD] | server URL | forget | setup [lan] | close.
int digits_wifi_console(int argc, char **argv);
