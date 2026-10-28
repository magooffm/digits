// Host tests for wifi_setup_form.c.
#include "wifi_setup_form.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool field(const char *body, const char *name, char *out, size_t capacity)
{
    return wifi_setup_field(body, strlen(body), name, out, capacity);
}

int main(void)
{
    char out[WIFI_SETUP_PASSWORD_BYTES];
    assert(field("ssid=My+Home%21&password=p%26ss%3Dword", "ssid", out, sizeof(out)) && !strcmp(out, "My Home!"));
    assert(field("ssid=My+Home%21&password=p%26ss%3Dword", "password", out, sizeof(out)) && !strcmp(out, "p&ss=word"));
    assert(field("password=&ssid=x", "password", out, sizeof(out)) && !strcmp(out, ""));
    assert(field("xssid=a&ssid=b", "ssid", out, sizeof(out)) && !strcmp(out, "b")); // Whole names only.
    assert(!field("ssid=a", "password", out, sizeof(out)));
    assert(!field("ssid=%4", "ssid", out, sizeof(out)));          // Truncated escape.
    assert(!field("ssid=%4&x=1", "ssid", out, sizeof(out)));      // Escape cut by the next field.
    assert(!field("ssid=%zz", "ssid", out, sizeof(out)));
    assert(!field("ssid=a%00b", "ssid", out, sizeof(out)));       // Embedded NUL.
    assert(!field("ssid=abcd", "ssid", out, 4));                  // "abcd" needs 5 bytes.
    assert(field("ssid=abc", "ssid", out, 4) && !strcmp(out, "abc"));
    // A body without a terminator: only size bytes are read.
    const char raw[] = {'s', 's', 'i', 'd', '=', 'a', 'b', 'X'};
    assert(wifi_setup_field(raw, 7, "ssid", out, sizeof(out)) && !strcmp(out, "ab"));

    assert(wifi_setup_valid("Home", ""));
    assert(wifi_setup_valid("Home", "12345678"));
    assert(!wifi_setup_valid("Home", "1234567"));
    assert(wifi_setup_valid("Home", "123456789012345678901234567890123456789012345678901234567890123"));
    assert(!wifi_setup_valid("Home", "123456789012345678901234567890123456789012345678901234567890123x"));
    assert(wifi_setup_valid("Home", "0123456789abcdefABCDEF0123456789abcdef0123456789abcdef0123456789"));
    assert(!wifi_setup_valid("Home", "pass\tword"));
    assert(!wifi_setup_valid("", "12345678"));
    assert(wifi_setup_valid("12345678901234567890123456789012", ""));
    assert(!wifi_setup_valid("123456789012345678901234567890123", ""));

    assert(wifi_setup_url_valid("ws://192.168.1.46:8090/ws"));
    assert(wifi_setup_url_valid("ws://digits.local/ws"));
    assert(!wifi_setup_url_valid("wss://digits.local/ws"));
    assert(!wifi_setup_url_valid("ws://digits.local/"));
    assert(!wifi_setup_url_valid("ws:///ws"));
    assert(!wifi_setup_url_valid("ws://my host/ws"));
    assert(!wifi_setup_url_valid(""));
    char page[64];
    assert(wifi_setup_pairing_page("ws://192.168.1.46:8090/ws", page, sizeof(page)) &&
           !strcmp(page, "http://192.168.1.46:8090/phones"));
    assert(wifi_setup_pairing_page("ws://digits.local/api/ws", page, sizeof(page)) &&
           !strcmp(page, "http://digits.local/phones"));
    assert(!wifi_setup_pairing_page("ws://192.168.1.46:8090/ws", page, 20));
    assert(!wifi_setup_pairing_page("http://x/ws", page, sizeof(page)));

    char html[32];
    assert(wifi_setup_escape("a<b>&\"c'", html, sizeof(html)) && !strcmp(html, "a&lt;b&gt;&amp;&quot;c&#39;"));
    assert(!wifi_setup_escape("<<<<<<<<", html, 10) && !html[0]);
    assert(wifi_setup_escape("", html, 1) && !html[0]);
    puts("wifi_setup_form tests passed");
    return 0;
}
