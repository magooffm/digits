#include "wifi_setup_form.h"
#include <stdio.h>
#include <string.h>

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool wifi_setup_field(const char *body, size_t size, const char *name, char *out, size_t capacity)
{
    size_t name_length = strlen(name);
    for (size_t at = 0; at < size;) {
        size_t end = at;
        while (end < size && body[end] != '&') ++end;
        if (end - at > name_length && !memcmp(body + at, name, name_length) && body[at + name_length] == '=') {
            size_t length = 0;
            for (size_t i = at + name_length + 1; i < end; ++i) {
                char c = body[i];
                if (c == '+') c = ' ';
                else if (c == '%') {
                    if (i + 2 >= end) return false; // Both digits inside this field.
                    int high = hex(body[i + 1]), low = hex(body[i + 2]);
                    if (high < 0 || low < 0) return false;
                    c = (char)(high << 4 | low);
                    i += 2;
                }
                if (!c || length + 1 >= capacity) return false;
                out[length++] = c;
            }
            out[length] = '\0';
            return true;
        }
        at = end + 1;
    }
    return false;
}

bool wifi_setup_valid(const char *ssid, const char *password)
{
    size_t ssid_length = strlen(ssid), length = strlen(password);
    if (!ssid_length || ssid_length > 32) return false;
    if (!length) return true;
    if (length == 64) {
        for (size_t i = 0; i < length; ++i)
            if (hex(password[i]) < 0) return false;
        return true;
    }
    if (length < 8 || length > 63) return false;
    for (size_t i = 0; i < length; ++i)
        if (password[i] < 0x20 || password[i] > 0x7e) return false;
    return true;
}

bool wifi_setup_escape(const char *in, char *out, size_t capacity)
{
    size_t length = 0;
    for (; *in; ++in) {
        const char *entity = *in == '&' ? "&amp;" : *in == '<' ? "&lt;" : *in == '>' ? "&gt;"
                           : *in == '"' ? "&quot;" : *in == '\'' ? "&#39;" : NULL;
        size_t n = entity ? strlen(entity) : 1;
        if (length + n >= capacity) {
            if (capacity) out[0] = '\0';
            return false;
        }
        if (entity) memcpy(out + length, entity, n);
        else out[length] = *in;
        length += n;
    }
    out[length] = '\0';
    return true;
}

bool wifi_setup_url_valid(const char *url)
{
    size_t length = strlen(url);
    if (length >= WIFI_SETUP_URL_BYTES || length <= 8 || strncmp(url, "ws://", 5) ||
        strcmp(url + length - 3, "/ws"))
        return false;
    for (size_t i = 0; i < length; ++i)
        if (url[i] <= 0x20 || url[i] > 0x7e) return false;
    return url[5] != '/'; // A host is required.
}

bool wifi_setup_pairing_page(const char *url, char *out, size_t capacity)
{
    if (!wifi_setup_url_valid(url)) return false;
    const char *host = url + 5, *end = strchr(host, '/');
    int n = snprintf(out, capacity, "http://%.*s/phones", (int)(end - host), host);
    return n > 0 && (size_t)n < capacity;
}
