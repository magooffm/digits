#include "setup.h"
#include "status.h"
#include "web_config.h"
#include "wifi.h"
#include "wifi_setup_form.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h" // Before dns_server.h, which uses its types.
#include "dns_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#define TAG "setup"
#define MAX_NETWORKS 20
#define FORM_BYTES 512
#define UPLOAD_CHUNK 4096
#define RESTART_DELAY_US 1500000
#define VALID_AFTER_US (5 * 60 * 1000000LL) // Or once the server answered.

extern const char page_start[] asm("_binary_setup_page_html_start");
extern const char page_end[] asm("_binary_setup_page_html_end");

static SemaphoreHandle_t lock;
static esp_netif_t *ap_netif;
static httpd_handle_t server;     // Always running: status page, plus setup while open.
static bool setup_open;
static dns_server_handle_t dns;
static char ap_name[32];
static char networks[MAX_NETWORKS][WIFI_SETUP_SSID_BYTES];
static unsigned network_count;
static int64_t forced_until;
static bool lan_allowed;

static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool server_connected;
    char code[8];
    char number[32];
} state;

static esp_timer_handle_t restart_timer, valid_timer;

// ---- State ----------------------------------------------------------------

static void mark_valid(void *arg)
{
    (void)arg;
    esp_ota_img_states_t image;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &image) == ESP_OK &&
        image == ESP_OTA_IMG_PENDING_VERIFY && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        ESP_LOGI(TAG, "New firmware confirmed; no rollback");
}

void digits_setup_report_server(bool connected)
{
    portENTER_CRITICAL(&state_mux);
    state.server_connected = connected;
    portEXIT_CRITICAL(&state_mux);
    if (connected && valid_timer) { // Confirm off the caller's task, now instead of later.
        esp_timer_stop(valid_timer);
        esp_timer_start_once(valid_timer, 1);
    }
}

void digits_setup_report_pairing_code(const char *code)
{
    portENTER_CRITICAL(&state_mux);
    snprintf(state.code, sizeof(state.code), "%s", code ? code : "");
    portEXIT_CRITICAL(&state_mux);
}

void digits_setup_report_number(const char *number)
{
    portENTER_CRITICAL(&state_mux);
    snprintf(state.number, sizeof(state.number), "%s", number ? number : "");
    if (state.number[0]) state.code[0] = '\0';
    portEXIT_CRITICAL(&state_mux);
}

bool digits_setup_server_connected(void)
{
    portENTER_CRITICAL(&state_mux);
    bool connected = state.server_connected;
    portEXIT_CRITICAL(&state_mux);
    return connected;
}

void digits_setup_number(char *out, size_t capacity)
{
    portENTER_CRITICAL(&state_mux);
    snprintf(out, capacity, "%s", state.number);
    portEXIT_CRITICAL(&state_mux);
}

bool digits_setup_paired(void)
{
    portENTER_CRITICAL(&state_mux);
    bool paired = state.number[0] != 0;
    portEXIT_CRITICAL(&state_mux);
    return paired;
}

bool digits_setup_forced(void) { return esp_timer_get_time() < forced_until; }

static const char *requested_reason;

void digits_setup_request(const char *reason)
{
    requested_reason = reason;
    forced_until = esp_timer_get_time() + CONFIG_DIGITS_SETUP_FORCED_MINUTES * 60 * 1000000LL;
}

const char *digits_setup_requested(void) { return digits_setup_forced() ? requested_reason : NULL; }
void digits_setup_allow_lan(void) { lan_allowed = true; }

static void restart(void *arg)
{
    (void)arg;
    esp_restart();
}

static void restart_soon(void)
{
    if (!restart_timer || esp_timer_start_once(restart_timer, RESTART_DELAY_US) != ESP_OK) esp_restart();
}

void digits_setup_init(void)
{
    lock = xSemaphoreCreateMutex();
    esp_timer_create_args_t restart_args = {.callback = restart, .name = "setup_restart"};
    esp_timer_create(&restart_args, &restart_timer);
    esp_ota_img_states_t image;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &image) == ESP_OK && image == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "Running new firmware from %s: confirmed once the server answers or after 5 minutes",
                 running->label);
        esp_timer_create_args_t valid_args = {.callback = mark_valid, .name = "setup_valid"};
        if (esp_timer_create(&valid_args, &valid_timer) == ESP_OK) esp_timer_start_once(valid_timer, VALID_AFTER_US);
    }
}

// ---- Page -----------------------------------------------------------------

static bool on_setup_network(httpd_req_t *req)
{
    struct sockaddr_storage address;
    socklen_t size = sizeof(address);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&address, &size)) return false;
    uint32_t ip;
    if (address.ss_family == AF_INET) {
        ip = ((struct sockaddr_in *)&address)->sin_addr.s_addr;
    } else if (address.ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&address;
        ip = v6->sin6_addr.un.u32_addr[3]; // IPv4-mapped.
    } else {
        return false;
    }
    esp_netif_ip_info_t info;
    return ap_netif && esp_netif_get_ip_info(ap_netif, &info) == ESP_OK && ip == info.ip.addr;
}

// Setup pages and actions: only while the setup network is open, and only
// on it, never on the home network. This also keeps every flash write in
// setup mode, whose server task has an internal-RAM stack.
static bool allowed(httpd_req_t *req)
{
    return setup_open && (lan_allowed || on_setup_network(req));
}

static esp_err_t refuse(httpd_req_t *req)
{
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_send(req, "Only on the phone's setup network", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_message(httpd_req_t *req, const char *title, const char *text)
{
    char escaped[160], page[640];
    wifi_setup_escape(text, escaped, sizeof(escaped));
    snprintf(page, sizeof(page),
             "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
             "<body style='font-family:system-ui;max-width:28em;margin:2em auto;padding:0 1em'>"
             "<h1>%s</h1><p>%s</p><p><a href=/>Back</a></p>", title, escaped);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static void scan_networks(void)
{
    wifi_scan_config_t config = {.show_hidden = false};
    uint16_t count = MAX_NETWORKS;
    wifi_ap_record_t *records = calloc(MAX_NETWORKS, sizeof(*records));
    if (!records || esp_wifi_scan_start(&config, true) != ESP_OK ||
        esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
        free(records);
        return;
    }
    network_count = 0;
    for (unsigned i = 0; i < count; ++i) { // Strongest first; one entry per name.
        const char *name = (const char *)records[i].ssid;
        bool seen = !name[0];
        for (unsigned j = 0; j < network_count && !seen; ++j) seen = !strcmp(networks[j], name);
        if (!seen) snprintf(networks[network_count++], WIFI_SETUP_SSID_BYTES, "%s", name);
    }
    free(records);
}

static esp_err_t root_get(httpd_req_t *req)
{
    if (!allowed(req)) return digits_status_page(req); // Home network: read-only status.
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page_start, page_end - page_start - 1); // Without EMBED_TXTFILES' NUL.
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!allowed(req)) return refuse(req);
    digits_wifi_config_t wifi;
    digits_wifi_config(&wifi);
    char code[8], number[32], page[WIFI_SETUP_URL_BYTES + 16] = "";
    portENTER_CRITICAL(&state_mux);
    bool server_connected = state.server_connected;
    memcpy(code, state.code, sizeof(code));
    memcpy(number, state.number, sizeof(number));
    portEXIT_CRITICAL(&state_mux);
    wifi_setup_pairing_page(wifi.url, page, sizeof(page));
    const esp_app_desc_t *app = esp_app_get_description();
    char firmware[80];
    snprintf(firmware, sizeof(firmware), "%s (%s %s)", app->version, app->date, app->time);
    cJSON *root = cJSON_CreateObject(), *w = cJSON_AddObjectToObject(root, "wifi"),
          *s = cJSON_AddObjectToObject(root, "server"), *list = cJSON_AddArrayToObject(root, "networks");
    cJSON_AddStringToObject(root, "firmware", firmware);
    cJSON_AddStringToObject(w, "ssid", wifi.ssid);
    cJSON_AddBoolToObject(w, "connected", digits_wifi_online());
    cJSON_AddNumberToObject(w, "rssi", digits_wifi_rssi());
    cJSON_AddStringToObject(s, "url", wifi.url);
    cJSON_AddBoolToObject(s, "connected", server_connected);
    cJSON_AddStringToObject(root, "number", number);
    cJSON_AddStringToObject(root, "code", code);
    cJSON_AddStringToObject(root, "pairing_page", page);
    for (unsigned i = 0; i < network_count; ++i) cJSON_AddItemToArray(list, cJSON_CreateString(networks[i]));
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text ? text : "{}", HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!allowed(req)) return refuse(req);
    scan_networks();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static int receive_all(httpd_req_t *req, char *body, size_t capacity)
{
    if (req->content_len >= capacity) return -1;
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return (int)got;
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!allowed(req)) return refuse(req);
    char body[FORM_BYTES], ssid[WIFI_SETUP_SSID_BYTES], password[WIFI_SETUP_PASSWORD_BYTES];
    char url[WIFI_SETUP_URL_BYTES];
    int got = receive_all(req, body, sizeof(body));
    bool ok = got >= 0 && wifi_setup_field(body, (size_t)got, "ssid", ssid, sizeof(ssid)) &&
              wifi_setup_field(body, (size_t)got, "password", password, sizeof(password)) &&
              wifi_setup_field(body, (size_t)got, "url", url, sizeof(url));
    if (ok && !password[0]) {
        // Empty keeps the saved password of the same network.
        digits_wifi_config_t current;
        digits_wifi_config(&current);
        if (!strcmp(current.ssid, ssid)) snprintf(password, sizeof(password), "%s", current.password);
    }
    if (!ok || !wifi_setup_valid(ssid, password) || !wifi_setup_url_valid(url))
        return send_message(req, "Not saved",
                            "Network name 1-32 characters, password empty or 8-63 characters, server like "
                            "ws://192.168.1.50:8080/ws.");
    esp_err_t err = digits_wifi_save(ssid, password, url);
    if (err != ESP_OK) return send_message(req, "Not saved", "The phone could not store the settings.");
    ESP_LOGI(TAG, "Setup page saved network %s and server %s; restarting", ssid, url);
    send_message(req, "Saved", "The phone restarts and joins the network. If it cannot connect, this setup "
                               "network comes back after two minutes.");
    restart_soon();
    return ESP_OK;
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (!allowed(req)) return refuse(req);
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    httpd_resp_set_type(req, "text/plain");
    if (!target || req->content_len < 1024 || req->content_len > target->size)
        return httpd_resp_send(req, "Not a firmware file of a usable size.", HTTPD_RESP_USE_STRLEN);
    char *chunk = malloc(UPLOAD_CHUNK);
    esp_ota_handle_t ota = 0;
    esp_err_t err = chunk ? esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota) : ESP_ERR_NO_MEM;
    size_t left = req->content_len;
    ESP_LOGI(TAG, "Receiving %u bytes of firmware into %s", (unsigned)left, target->label);
    while (err == ESP_OK && left) {
        int n = httpd_req_recv(req, chunk, left < UPLOAD_CHUNK ? left : UPLOAD_CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, chunk, (size_t)n);
        left -= (size_t)n;
    }
    free(chunk);
    if (err != ESP_OK) {
        if (ota) esp_ota_abort(ota);
        ESP_LOGE(TAG, "Firmware upload failed: %s", esp_err_to_name(err));
        return httpd_resp_send(req, "Upload failed; the phone keeps its current firmware.", HTTPD_RESP_USE_STRLEN);
    }
    err = esp_ota_end(ota); // Verifies the image.
    esp_app_desc_t incoming;
    if (err == ESP_OK) err = esp_ota_get_partition_description(target, &incoming);
    if (err == ESP_OK && strncmp(incoming.project_name, esp_app_get_description()->project_name,
                                 sizeof(incoming.project_name))) {
        ESP_LOGE(TAG, "Uploaded firmware is for %s", incoming.project_name);
        return httpd_resp_send(req, "That file is not Digits phone firmware; nothing changed.", HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK) err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Firmware rejected: %s", esp_err_to_name(err));
        return httpd_resp_send(req, "The file is not a valid firmware image; nothing changed.", HTTPD_RESP_USE_STRLEN);
    }
    ESP_LOGI(TAG, "Firmware %s written to %s; restarting", incoming.version, target->label);
    char reply[96];
    snprintf(reply, sizeof(reply), "Firmware %.32s installed. The phone restarts now.", incoming.version);
    httpd_resp_send(req, reply, HTTPD_RESP_USE_STRLEN);
    restart_soon();
    return ESP_OK;
}

static esp_err_t restart_post(httpd_req_t *req)
{
    if (!allowed(req)) return refuse(req);
    ESP_LOGI(TAG, "Restart from the setup page");
    send_message(req, "Restarting", "The phone restarts now.");
    restart_soon();
    return ESP_OK;
}

// Captive portal: phones probe fixed URLs; every unknown path leads here.
static esp_err_t redirect(httpd_req_t *req, httpd_err_code_t error)
{
    if (!allowed(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, "Redirect to the setup page", HTTPD_RESP_USE_STRLEN); // iOS needs a body.
}

// ---- Access point ---------------------------------------------------------

// Setup mode writes NVS and flash, so its task stack must be internal RAM;
// the status-only server keeps its stack in PSRAM. Caller holds lock.
static void server_start(bool setup_mode)
{
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.max_open_sockets = 4;
    http.lru_purge_enable = true;
    http.max_uri_handlers = 32;
    http.stack_size = 6144;
    http.task_priority = 2; // Below the call audio (5) and peer (6) tasks.
    http.task_caps = setup_mode ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (httpd_start(&server, &http) != ESP_OK) {
        server = NULL;
        ESP_LOGE(TAG, "Web server could not start");
        return;
    }
    static const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/update", .method = HTTP_POST, .handler = update_post},
        {.uri = "/restart", .method = HTTP_POST, .handler = restart_post},
    };
    for (unsigned i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i)
        httpd_register_uri_handler(server, &handlers[i]);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect);
    digits_status_register(server);
    digits_web_config_register(server);
}

static void server_stop(void)
{
    if (server) httpd_stop(server);
    server = NULL;
}

void digits_setup_start_web(void)
{
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR); // Portal probes and scans cause many 404s.
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!server) server_start(setup_open);
    xSemaphoreGive(lock);
}

void digits_setup_open(const char *reason, bool forced)
{
    if (forced) forced_until = esp_timer_get_time() + CONFIG_DIGITS_SETUP_FORCED_MINUTES * 60 * 1000000LL;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (setup_open) {
        xSemaphoreGive(lock);
        return;
    }
    if (!ap_netif) ap_netif = esp_netif_create_default_wifi_ap();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ap_name, sizeof(ap_name), "Digits-Setup-%02X%02X", mac[4], mac[5]);
    wifi_config_t config = {.ap = {.max_connection = 2, .channel = 1}};
    size_t name_length = strlen(ap_name), password_length = strlen(CONFIG_DIGITS_SETUP_PASSWORD);
    memcpy(config.ap.ssid, ap_name, name_length);
    config.ap.ssid_len = (uint8_t)name_length;
    bool protected = password_length >= 8 && password_length <= 63;
    config.ap.authmode = protected ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (protected) memcpy(config.ap.password, CONFIG_DIGITS_SETUP_PASSWORD, password_length);
    // The station stays on: it scans for the page and keeps retrying.
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &config);
    scan_networks();
    setup_open = true;
    server_stop();
    server_start(true);
    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    dns = start_dns_server(&dns_config);
    ESP_LOGI(TAG, "Setup network %s open (%s)%s; page at http://192.168.4.1", ap_name, reason,
             protected ? "" : ", WITHOUT a password: set CONFIG_DIGITS_SETUP_PASSWORD");
    xSemaphoreGive(lock);
}

void digits_setup_close(void)
{
    forced_until = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (setup_open) {
        if (dns) stop_dns_server(dns);
        dns = NULL;
        setup_open = false;
        server_stop();
        server_start(false);
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "Setup network %s closed", ap_name);
    }
    xSemaphoreGive(lock);
}

bool digits_setup_running(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool running = setup_open;
    xSemaphoreGive(lock);
    return running;
}
