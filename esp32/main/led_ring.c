#include "led_ring.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "led_strip.h"

#define TAG "led_ring"
#define LEDS CONFIG_DIGITS_LED_RING_LEDS
#define FLASH_HALF_PERIOD_US 400000LL
#define HANGUP_US 2000000LL
#define SPIN_STEP_US 120000LL // Per LED: once around in 0.84 s with 7.

typedef struct {
    uint8_t r, g, b;
} color_t;

// Full scale; the configured brightness scales them.
static const color_t OFF = {0, 0, 0};
static const color_t YELLOW = {255, 170, 0};
static const color_t ORANGE = {255, 60, 0};
static const color_t GREEN = {60, 255, 60};
static const color_t RED = {255, 0, 0};
static const color_t BLUE = {0, 80, 255};

static led_strip_handle_t strip;
static digits_led_ring_mode_t mode_now = DIGITS_LED_RING_OFF;
static int64_t flash_since, red_until;
static volatile int upload = DIGITS_LED_RING_UPLOAD_DONE; // Set by the web server.
static color_t shown[LEDS];

static color_t scaled(color_t c, unsigned percent)
{
    return (color_t){c.r * percent / 100, c.g * percent / 100, c.b * percent / 100};
}

static void fill(color_t ring[LEDS], color_t c)
{
    for (unsigned i = 0; i < LEDS; ++i) ring[i] = c;
}

static esp_err_t write_ring(const color_t ring[LEDS])
{
    for (unsigned i = 0; i < LEDS; ++i) {
        color_t c = scaled(ring[i], CONFIG_DIGITS_LED_RING_BRIGHTNESS);
        esp_err_t err = led_strip_set_pixel(strip, i, c.r, c.g, c.b);
        if (err != ESP_OK) return err;
    }
    return led_strip_refresh(strip);
}

esp_err_t digits_led_ring_init(void)
{
    if (CONFIG_DIGITS_LED_RING_GPIO < 0) return ESP_OK;
    led_strip_config_t config = {
        .strip_gpio_num = CONFIG_DIGITS_LED_RING_GPIO,
        .max_leds = LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB, // As in Waveshare's demo.
    };
    led_strip_rmt_config_t rmt = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
    esp_err_t err = led_strip_new_rmt_device(&config, &rmt, &strip);
    if (err == ESP_OK) err = write_ring(shown);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED ring unavailable: %s", esp_err_to_name(err));
        if (strip) led_strip_del(strip);
        strip = NULL;
        return err;
    }
    ESP_LOGI(TAG, "LED ring ready: %d LEDs on GPIO%d", LEDS, CONFIG_DIGITS_LED_RING_GPIO);
    return ESP_OK;
}

void digits_led_ring_call_ended(int64_t now_us) { red_until = now_us + HANGUP_US; }

void digits_led_ring_upload(int percent) { upload = percent > 100 ? 100 : percent; }

void digits_led_ring_show(digits_led_ring_mode_t mode, int64_t now_us)
{
    if (!strip) return;
    if (mode != mode_now) {
        mode_now = mode;
        flash_since = now_us; // Each ringing call starts with the ring lit.
        if (mode == DIGITS_LED_RING_RINGING || mode == DIGITS_LED_RING_CALL) red_until = 0;
    }
    int percent = upload;
    if (percent == DIGITS_LED_RING_UPLOAD_FAILED) {
        upload = DIGITS_LED_RING_UPLOAD_DONE;
        red_until = now_us + HANGUP_US;
    }
    color_t ring[LEDS];
    fill(ring, OFF);
    if (percent >= 0) {
        // A bar of whole LEDs, the next one partly lit. Units: 1/100 LED.
        unsigned lit = (unsigned)percent * LEDS;
        for (unsigned i = 0; i < LEDS && i * 100 < lit; ++i)
            ring[i] = lit >= (i + 1) * 100 ? BLUE : scaled(BLUE, lit - i * 100);
    } else if (mode == DIGITS_LED_RING_STARTING && now_us >= red_until) {
        // A dot with a fading tail.
        unsigned head = (unsigned)(now_us / SPIN_STEP_US % LEDS);
        ring[head] = BLUE;
        ring[(head + LEDS - 1) % LEDS] = scaled(BLUE, 30);
        ring[(head + LEDS - 2) % LEDS] = scaled(BLUE, 8);
    } else {
        fill(ring, mode == DIGITS_LED_RING_CALL ? GREEN :
                   mode == DIGITS_LED_RING_RINGING ?
                       ((now_us - flash_since) / FLASH_HALF_PERIOD_US % 2 ? OFF : ORANGE) :
                   mode == DIGITS_LED_RING_ENDED || now_us < red_until ? RED :
                   mode == DIGITS_LED_RING_LIFTED ? YELLOW : OFF);
    }
    if (!memcmp(ring, shown, sizeof(ring))) return;
    if (write_ring(ring) == ESP_OK) memcpy(shown, ring, sizeof(ring));
}
