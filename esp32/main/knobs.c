#include "knobs.h"
#include "sdkconfig.h"

#include "audio_board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include <stdio.h>

#define TAG "knobs"
#define POLL_US 50000LL
#define LOW_MV 80   // At or below: volume 0 (muted). The stop reads 2-40 mV.
#define HIGH_MV 2900 // At or above: volume 100. The first fader peaked at
                     // 2969 mV of 3.3 V; dead bands make both ends reachable.
#define HYSTERESIS 3 // Volume steps; keeps a resting knob from flickering.
#define AVERAGE 4    // Readings averaged (200 ms).
#define SETTLED_US 1000000LL // Logs once the knob rests this long.

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static adc_channel_t channel;
static bool active;
static int64_t next_poll;
static int last_mv = -1;
static int samples[AVERAGE], sample_count, sample_next;
static int64_t changed_at; // 0: nothing to log.
static volatile bool trace = true; // Serial only; `knob off` stops it.
static int traced_mv = -1000;
static unsigned read_errors;

static int volume_from_mv(int mv)
{
    if (mv <= LOW_MV) return 0;
    if (mv >= HIGH_MV) return 100;
    return (mv - LOW_MV) * 100 / (HIGH_MV - LOW_MV);
}

static esp_err_t read_mv(int *mv)
{
    int raw;
    esp_err_t err = adc_oneshot_read(adc, channel, &raw);
    if (err == ESP_OK) err = adc_cali_raw_to_voltage(cali, raw, mv);
    return err;
}

static void apply(int mv, bool force, int64_t now_us)
{
    last_mv = mv;
    int volume = volume_from_mv(mv), current = digits_audio_speaker_volume();
    int change = volume > current ? volume - current : current - volume;
    if (volume != current && (force || change >= HYSTERESIS || volume == 0 || volume == 100)) {
        esp_err_t err = digits_audio_speaker_apply_volume(volume);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Volume knob: setting %d failed: %s", volume, esp_err_to_name(err));
            return;
        }
        changed_at = now_us ? now_us : 1;
    } else if (changed_at && now_us - changed_at >= SETTLED_US) {
        ESP_LOGI(TAG, "Volume knob: %d mV, volume %d", mv, current);
        changed_at = 0;
    }
}

esp_err_t digits_knobs_init(void)
{
    if (CONFIG_DIGITS_VOLUME_KNOB_GPIO < 0) return ESP_OK;
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(CONFIG_DIGITS_VOLUME_KNOB_GPIO, &unit, &channel);
    if (err == ESP_OK && unit != ADC_UNIT_1) err = ESP_ERR_NOT_SUPPORTED; // ADC2 serves Wi-Fi.
    if (err == ESP_OK) {
        adc_oneshot_unit_init_cfg_t unit_config = {.unit_id = ADC_UNIT_1};
        err = adc_oneshot_new_unit(&unit_config, &adc);
    }
    if (err == ESP_OK) {
        // 12 dB: about 0-3.1 V, enough for a fader on 3.3 V.
        adc_oneshot_chan_cfg_t config = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        err = adc_oneshot_config_channel(adc, channel, &config);
    }
    if (err == ESP_OK) {
        adc_cali_curve_fitting_config_t config = {
            .unit_id = ADC_UNIT_1, .chan = channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_cali_create_scheme_curve_fitting(&config, &cali);
    }
    int mv = 0;
    if (err == ESP_OK) err = read_mv(&mv);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Volume knob on GPIO%d unavailable: %s", CONFIG_DIGITS_VOLUME_KNOB_GPIO, esp_err_to_name(err));
        if (cali) adc_cali_delete_scheme_curve_fitting(cali);
        if (adc) adc_oneshot_del_unit(adc);
        cali = NULL;
        adc = NULL;
        return err;
    }
    active = true;
    ESP_LOGI(TAG, "Volume knob on GPIO%d: %d mV", CONFIG_DIGITS_VOLUME_KNOB_GPIO, mv);
    apply(mv, true, 0);
    changed_at = 0;
    return ESP_OK;
}

void digits_knobs_poll(int64_t now_us)
{
    if (!active || now_us < next_poll) return;
    next_poll = now_us + POLL_US;
    int mv;
    esp_err_t err = read_mv(&mv);
    if (err != ESP_OK) {
        ++read_errors;
        if (trace) printf("knob: read failed: %s (%u so far)\n", esp_err_to_name(err), read_errors);
        return;
    }
    samples[sample_next] = mv;
    sample_next = (sample_next + 1) % AVERAGE;
    if (sample_count < AVERAGE) ++sample_count;
    int sum = 0;
    for (int i = 0; i < sample_count; ++i) sum += samples[i];
    int before = digits_audio_speaker_volume();
    apply(sum / sample_count, false, now_us);
    int after = digits_audio_speaker_volume();
    // Every reading that moved 5 mV or more, or changed the volume.
    if (trace && (after != before || mv - traced_mv >= 5 || traced_mv - mv >= 5)) {
        printf("knob: %4d mV  avg %4d mV  volume %3d%s\n", mv, sum / sample_count, after,
               after != before ? "  (set)" : "");
        traced_mv = mv;
    }
}

bool digits_knobs_volume_active(void) { return active; }

int digits_knobs_volume_mv(void) { return last_mv; }

void digits_knobs_set_trace(bool on) { trace = on; }

bool digits_knobs_trace(void) { return trace; }

unsigned digits_knobs_read_errors(void) { return read_errors; }
