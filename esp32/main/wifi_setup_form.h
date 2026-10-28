#pragma once

// Pure helpers for the Wi-Fi setup page: form decoding, credential checks and
// HTML escaping. No ESP-IDF: tests/wifi_setup_form_test.c runs on the host.

#include <stdbool.h>
#include <stddef.h>

#define WIFI_SETUP_SSID_BYTES 33     // 32 bytes plus terminator.
#define WIFI_SETUP_PASSWORD_BYTES 65 // 64 bytes plus terminator.

// Finds name=value in an application/x-www-form-urlencoded body and decodes
// it ('+' and %XX). False if absent, malformed, containing NUL or not
// fitting out (capacity includes the terminator).
bool wifi_setup_field(const char *body, size_t size, const char *name, char *out, size_t capacity);

// SSID: 1-32 bytes. Password: empty (open network), 8-63 printable ASCII
// characters, or exactly 64 hex digits (WPA2 raw key).
bool wifi_setup_valid(const char *ssid, const char *password);

#define WIFI_SETUP_URL_BYTES 128

// Digits server address: ws://HOST[:PORT]/ws, printable, no spaces.
bool wifi_setup_url_valid(const char *url);

// The server's web page for pairing, from its WebSocket address:
// ws://10.0.0.5:8090/ws -> http://10.0.0.5:8090/phones. False if invalid.
bool wifi_setup_pairing_page(const char *url, char *out, size_t capacity);

// Escapes & < > " ' for HTML text and attribute values. Returns false if
// out is too small; out is then empty.
bool wifi_setup_escape(const char *in, char *out, size_t capacity);
