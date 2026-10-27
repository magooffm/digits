#include "wifi.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static EventGroupHandle_t events;
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, BIT0);
        ESP_LOGW("wifi", "Disconnected; retrying Wi-Fi in 3 seconds");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI("wifi", "Station has IP address");
        xEventGroupSetBits(events, BIT0);
    }
}

static void clock_start(void);

static void wifi_retry(void *arg)
{
    bool clock_started = false;
    for (;;) {
        bool online = xEventGroupGetBits(events) & BIT0;
        if (!online) esp_wifi_connect();
        else if (!clock_started) {
            clock_started = true;
            clock_start();
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

static void clock_set(struct timeval *now)
{
    struct tm local;
    char text[32];
    localtime_r(&now->tv_sec, &local);
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    ESP_LOGI("wifi", "Clock set: %s", text);
}

// Message dates on the SD card, once the station first has an address.
static void clock_start(void)
{
    if (!CONFIG_DIGITS_SNTP_SERVER[0]) return;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_DIGITS_SNTP_SERVER);
    config.sync_cb = clock_set;
    if (esp_netif_sntp_init(&config) != ESP_OK) ESP_LOGW("wifi", "Clock sync unavailable");
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
    wifi_config_t config = {0};
    size_t ssid_len = strlen(CONFIG_DIGITS_WIFI_SSID);
    size_t password_len = strlen(CONFIG_DIGITS_WIFI_PASSWORD);
    ESP_ERROR_CHECK(ssid_len > 0 && ssid_len <= sizeof(config.sta.ssid) &&
                    password_len <= sizeof(config.sta.password) ? ESP_OK : ESP_ERR_INVALID_ARG);
    memcpy(config.sta.ssid, CONFIG_DIGITS_WIFI_SSID, ssid_len);
    memcpy(config.sta.password, CONFIG_DIGITS_WIFI_PASSWORD, password_len);
    // With several access points of one name, the default fast scan joins the
    // first one found, even at -90 dBm where DHCP stalls. Take the strongest.
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(xTaskCreate(wifi_retry, "wifi_retry", 3072, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void digits_wifi_wait_connected(void)
{
    xEventGroupWaitBits(events, BIT0, pdFALSE, pdTRUE, portMAX_DELAY);
}
