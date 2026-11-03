#include "led_ring.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include "esp_log.h"
#include "led_strip.h"

#define TAG "led_ring"
#define FLASH_HALF_PERIOD_US 400000LL
#define HANGUP_US 2000000LL

typedef struct {
    uint8_t r, g, b;
} color_t;

// Full scale; the configured brightness scales them.
static const color_t OFF = {0, 0, 0};
static const color_t YELLOW = {255, 170, 0};
static const color_t ORANGE = {255, 60, 0};
static const color_t GREEN = {60, 255, 60};
static const color_t RED = {255, 0, 0};

static led_strip_handle_t strip;
static digits_led_ring_mode_t mode_now = DIGITS_LED_RING_OFF;
static int64_t flash_since, red_until;
static color_t shown;

static bool same(color_t a, color_t b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

static esp_err_t write_ring(color_t c)
{
    if (same(c, OFF)) return led_strip_clear(strip);
    unsigned level = CONFIG_DIGITS_LED_RING_BRIGHTNESS;
    for (unsigned i = 0; i < CONFIG_DIGITS_LED_RING_LEDS; ++i) {
        esp_err_t err = led_strip_set_pixel(strip, i, c.r * level / 100, c.g * level / 100, c.b * level / 100);
        if (err != ESP_OK) return err;
    }
    return led_strip_refresh(strip);
}

esp_err_t digits_led_ring_init(void)
{
    if (CONFIG_DIGITS_LED_RING_GPIO < 0) return ESP_OK;
    led_strip_config_t config = {
        .strip_gpio_num = CONFIG_DIGITS_LED_RING_GPIO,
        .max_leds = CONFIG_DIGITS_LED_RING_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB, // As in Waveshare's demo.
    };
    led_strip_rmt_config_t rmt = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
    esp_err_t err = led_strip_new_rmt_device(&config, &rmt, &strip);
    if (err == ESP_OK) err = write_ring(OFF);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED ring unavailable: %s", esp_err_to_name(err));
        if (strip) led_strip_del(strip);
        strip = NULL;
        return err;
    }
    ESP_LOGI(TAG, "LED ring ready: %d LEDs on GPIO%d", CONFIG_DIGITS_LED_RING_LEDS, CONFIG_DIGITS_LED_RING_GPIO);
    return ESP_OK;
}

void digits_led_ring_call_ended(int64_t now_us) { red_until = now_us + HANGUP_US; }

void digits_led_ring_show(digits_led_ring_mode_t mode, int64_t now_us)
{
    if (!strip) return;
    if (mode != mode_now) {
        mode_now = mode;
        flash_since = now_us; // Each ringing call starts with the ring lit.
        if (mode == DIGITS_LED_RING_RINGING || mode == DIGITS_LED_RING_CALL) red_until = 0;
    }
    color_t c = mode == DIGITS_LED_RING_CALL ? GREEN :
                mode == DIGITS_LED_RING_RINGING ? ((now_us - flash_since) / FLASH_HALF_PERIOD_US % 2 ? OFF : ORANGE) :
                mode == DIGITS_LED_RING_ENDED || now_us < red_until ? RED :
                mode == DIGITS_LED_RING_LIFTED ? YELLOW : OFF;
    if (same(c, shown)) return;
    if (write_ring(c) == ESP_OK) shown = c;
}
