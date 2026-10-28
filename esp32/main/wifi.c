#include "wifi.h"
#include "setup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG "wifi"
#define NVS_NAMESPACE "digits_wifi"
#define ONLINE BIT0     // Has an address.
#define ASSOCIATED BIT1 // Joined the access point; may still wait for DHCP.
#define RETRY_MS 3000
#define RETRY_DURING_SETUP_MS 30000 // Each attempt scans and briefly takes the setup network off-channel.
#define RESTART_DELAY_US 1500000

static EventGroupHandle_t events;
static digits_wifi_config_t current; // Fixed for this boot.

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (xEventGroupGetBits(events) & ONLINE) ESP_LOGW(TAG, "Disconnected; retrying");
        xEventGroupClearBits(events, ONLINE | ASSOCIATED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(events, ASSOCIATED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Station has IP address");
        xEventGroupSetBits(events, ONLINE);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "A device joined the setup network");
    }
}

// ---- Saved configuration --------------------------------------------------

static void load_config(digits_wifi_config_t *c)
{
    memset(c, 0, sizeof(*c));
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t ssid_size = sizeof(c->ssid), password_size = sizeof(c->password), url_size = sizeof(c->url);
        c->saved = nvs_get_str(nvs, "ssid", c->ssid, &ssid_size) == ESP_OK &&
                   nvs_get_str(nvs, "password", c->password, &password_size) == ESP_OK &&
                   wifi_setup_valid(c->ssid, c->password);
        if (nvs_get_str(nvs, "url", c->url, &url_size) != ESP_OK || !wifi_setup_url_valid(c->url)) c->url[0] = '\0';
        nvs_close(nvs);
    }
    if (!c->saved) {
        snprintf(c->ssid, sizeof(c->ssid), "%s", CONFIG_DIGITS_WIFI_SSID);
        snprintf(c->password, sizeof(c->password), "%s", CONFIG_DIGITS_WIFI_PASSWORD);
        if (!wifi_setup_valid(c->ssid, c->password)) c->ssid[0] = c->password[0] = '\0';
    }
    if (!c->url[0] && wifi_setup_url_valid(CONFIG_DIGITS_SERVER_URL))
        snprintf(c->url, sizeof(c->url), "%s", CONFIG_DIGITS_SERVER_URL);
}

void digits_wifi_config(digits_wifi_config_t *out) { *out = current; }
const char *digits_wifi_server_url(void) { return current.url; }

esp_err_t digits_wifi_save(const char *ssid, const char *password, const char *url)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    if (ssid) {
        err = nvs_set_str(nvs, "ssid", ssid);
        if (err == ESP_OK) err = nvs_set_str(nvs, "password", password);
    }
    if (err == ESP_OK && url) err = nvs_set_str(nvs, "url", url);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t forget(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

bool digits_wifi_online(void) { return events && (xEventGroupGetBits(events) & ONLINE); }

int digits_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return digits_wifi_online() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

bool digits_wifi_address(char *out, size_t capacity)
{
    esp_netif_ip_info_t info;
    esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!digits_wifi_online() || !station || esp_netif_get_ip_info(station, &info) != ESP_OK || !info.ip.addr)
        return false;
    snprintf(out, capacity, IPSTR, IP2STR(&info.ip));
    return true;
}

// ---- Clock ----------------------------------------------------------------

static void clock_set(struct timeval *now)
{
    struct tm local;
    char text[32];
    localtime_r(&now->tv_sec, &local);
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    ESP_LOGI(TAG, "Clock set: %s", text);
}

// Message dates on the SD card, once the station first has an address.
static void clock_start(void)
{
    if (!CONFIG_DIGITS_SNTP_SERVER[0]) return;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_DIGITS_SNTP_SERVER);
    config.sync_cb = clock_set;
    if (esp_netif_sntp_init(&config) != ESP_OK) ESP_LOGW(TAG, "Clock sync unavailable");
}

// ---- Supervision ----------------------------------------------------------

static void supervisor(void *arg)
{
    (void)arg;
    bool clock_started = false;
    int64_t unhealthy_since = esp_timer_get_time(), last_attempt = INT64_MIN / 2; // First attempt at once.
    bool configured = current.ssid[0] && current.url[0];
    for (;;) {
        int64_t now = esp_timer_get_time();
        EventBits_t bits = xEventGroupGetBits(events);
        bool online = bits & ONLINE;
        if (online && !clock_started) {
            clock_started = true;
            clock_start();
        }
        // Healthy: Wi-Fi and the server both answer.
        if (online && digits_setup_server_connected()) unhealthy_since = now;
        bool stuck = now - unhealthy_since >= CONFIG_DIGITS_WIFI_SETUP_AFTER_S * 1000000LL;
        const char *reason = !configured ? "no network or server configured"
                           : !digits_setup_paired() ? "not paired yet"
                           : stuck ? (online ? "server unreachable" : "network unreachable") : NULL;
        bool running = digits_setup_running();
        if (reason && !running) digits_setup_open(reason, false);
        else if (!reason && running && !digits_setup_forced()) digits_setup_close();
        if (current.ssid[0] && !online) {
            int64_t interval = (running ? RETRY_DURING_SETUP_MS : RETRY_MS) * 1000LL;
            if (!(bits & ASSOCIATED) && now - last_attempt >= interval) {
                last_attempt = now;
                esp_wifi_connect();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void digits_wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    setenv("TZ", CONFIG_DIGITS_TIMEZONE, 1);
    tzset();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_ERR_NO_MEM);
    events = xEventGroupCreate();
    ESP_ERROR_CHECK(events ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    load_config(&current);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (current.ssid[0]) {
        wifi_config_t config = {0};
        memcpy(config.sta.ssid, current.ssid, strlen(current.ssid));
        memcpy(config.sta.password, current.password, strlen(current.password));
        // With several access points of one name, the default fast scan joins the
        // first one found, even at -90 dBm where DHCP stalls. Take the strongest.
        config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Network %s (%s), server %s", current.ssid[0] ? current.ssid : "none",
             current.saved ? "saved" : "build default", current.url[0] ? current.url : "none");
    ESP_ERROR_CHECK(xTaskCreate(supervisor, "wifi_supervisor", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK
                                                                                        : ESP_ERR_NO_MEM);
}

void digits_wifi_wait_connected(void)
{
    xEventGroupWaitBits(events, ONLINE, pdFALSE, pdTRUE, portMAX_DELAY);
}

// ---- Console --------------------------------------------------------------

static void restart(void *arg)
{
    (void)arg;
    esp_restart();
}

static void restart_soon(void)
{
    static esp_timer_handle_t timer;
    esp_timer_create_args_t args = {.callback = restart, .name = "wifi_restart"};
    if (!timer && esp_timer_create(&args, &timer) != ESP_OK) esp_restart();
    esp_timer_start_once(timer, RESTART_DELAY_US);
}

int digits_wifi_console(int argc, char **argv)
{
    const char *verb = argc >= 2 ? argv[1] : "status";
    esp_err_t err;
    if (!strcmp(verb, "status") && argc <= 2) {
        printf("Network: %s (%s), %s\n", current.ssid[0] ? current.ssid : "none",
               current.saved ? "saved" : "build default",
               digits_wifi_online() ? "connected" : "not connected");
        printf("Server: %s, %s\n", current.url[0] ? current.url : "none",
               digits_setup_server_connected() ? "connected" : "not connected");
        printf("Setup network: %s%s\n", digits_setup_running() ? "open" : "closed",
               digits_setup_forced() ? " (opened on request)" : "");
        return 0;
    }
    if (!strcmp(verb, "set") && (argc == 3 || argc == 4)) {
        const char *password = argc == 4 ? argv[3] : "";
        if (!wifi_setup_valid(argv[2], password)) {
            printf("Network name 1-32 characters; password empty or 8-63 characters\n");
            return 1;
        }
        err = digits_wifi_save(argv[2], password, NULL);
    } else if (!strcmp(verb, "server") && argc == 3) {
        if (!wifi_setup_url_valid(argv[2])) {
            printf("Server like ws://192.168.1.50:8080/ws\n");
            return 1;
        }
        err = digits_wifi_save(NULL, NULL, argv[2]);
    } else if (!strcmp(verb, "forget") && argc == 2) {
        err = forget();
    } else if (!strcmp(verb, "setup") && (argc == 2 || (argc == 3 && !strcmp(argv[2], "lan")))) {
        if (argc == 3) digits_setup_allow_lan();
        digits_setup_open("console", true);
        printf("Setup network open%s\n", argc == 3 ? "; the page also answers on the home network until restart" : "");
        return 0;
    } else if (!strcmp(verb, "close") && argc == 2) {
        digits_setup_close();
        return 0;
    } else {
        printf("Usage: wifi [status] | wifi set SSID [PASSWORD] | wifi server URL | wifi forget | "
               "wifi setup [lan] | wifi close\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("Saving failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Saved; restarting\n");
    restart_soon();
    return 0;
}
