#include "phone.h"
#include "sdkconfig.h"

#if CONFIG_DIGITS_PHONE
#include "phone_fsm.h"
#include "audio_board.h"
#include "incoming_ring.h"
#include "call_tones.h"
#include "ringtones.h"
#include "ring_select.h"
#include "sd_ringtones.h"
#include "silence.h"
#include "setup.h"
#include "ring_test.h"
#include "voicemail.h"
#include "webrtc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG "phone"
#define POLL_MS 10
#define NUMBER_CAPACITY 32
#define NVS_NAMESPACE "digits_phone"
#define ERROR_LOG_INTERVAL_US 1000000LL
#define SETUP_HOLD_US 8000000LL // Hold a call button this long, on hook, to open the setup network.
#define TASK_STACK_BYTES 4096
#define TASK_PRIORITY 3

static const char *const target_names[PHONE_CALL_TARGETS] = {"A", "B", "C"};
static const char *const nvs_keys[PHONE_CALL_TARGETS] = {"call_a", "call_b", "call_c"};
static const char *const ringtone_keys[PHONE_CALL_TARGETS] = {"ring_a", "ring_b", "ring_c"};
static const char *const selector_keys[PHONE_CALL_TARGETS] = {"sel_a", "sel_b", "sel_c"};
static const char *const default_numbers[PHONE_CALL_TARGETS] = {
    CONFIG_DIGITS_PHONE_CALL_A_NUMBER, CONFIG_DIGITS_PHONE_CALL_B_NUMBER,
    CONFIG_DIGITS_PHONE_CALL_C_NUMBER,
};
static const int selector_gpios[PHONE_CALL_TARGETS] = {
    CONFIG_DIGITS_PHONE_SELECTOR_A_GPIO, CONFIG_DIGITS_PHONE_SELECTOR_B_GPIO,
    CONFIG_DIGITS_PHONE_SELECTOR_C_GPIO,
};
#define MAP_KEY "posmap"
#define SELECTOR_POSITIONS CONFIG_DIGITS_PHONE_SELECTOR_POSITIONS
#define SELECTOR_POLL_DIVIDER 5 // Read analog selectors every 50 ms.
#define SELECTOR_SAMPLES 4
#define ADC_FULL_SCALE 4095
static const char *const phase_names[] = {"idle", "calling", "ringing", "connecting", "connected"};

// Shared between the phone task and the console task.
static portMUX_TYPE shared_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool started;
    char numbers[PHONE_CALL_TARGETS][NUMBER_CAPACITY];
    uint8_t ringtones[PHONE_CALL_TARGETS]; // How the callee's phone rings.
    uint8_t selector[PHONE_CALL_TARGETS];  // Local selector position, 0-based.
    bool selector_hardware[PHONE_CALL_TARGETS];
    ring_select_map_t map;                 // Position -> ringtone on this phone.
    int preview_request; // Ringtone to preview, or -1.
    int sim_hook; // -1 none pending, 0 replace, 1 lift.
    uint32_t sim_presses[PHONE_CALL_TARGETS];
    bool off_hook;
    bool inputs_ok;
} shared = {.sim_hook = -1, .preview_request = -1};

#if CONFIG_DIGITS_PHONE_INPUT_BOARD_KEYS
#define HOOK_IS_TOGGLE 1
#define INPUT_DESCRIPTION "board keys: KEY1 lifts/replaces handset, KEY2 calls A, KEY3 calls B (no C)"

static esp_err_t inputs_init(void) { return digits_audio_board_keys_init(); }

static esp_err_t inputs_read(bool *hook, bool buttons[PHONE_CALL_TARGETS])
{
    uint8_t keys = 0;
    esp_err_t err = digits_audio_board_keys_read(&keys);
    if (err != ESP_OK) return err;
    *hook = keys & 0x1;
    buttons[0] = keys & 0x2;
    buttons[1] = keys & 0x4;
    buttons[2] = false; // No third key on the board.
    return ESP_OK;
}
#else
#define HOOK_IS_TOGGLE 0
#define INPUT_DESCRIPTION "GPIO: hook switch, call buttons A, B and C"

static esp_err_t inputs_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << CONFIG_DIGITS_PHONE_HOOK_GPIO) |
                        (1ULL << CONFIG_DIGITS_PHONE_CALL_A_GPIO) |
                        (1ULL << CONFIG_DIGITS_PHONE_CALL_B_GPIO) |
                        (1ULL << CONFIG_DIGITS_PHONE_CALL_C_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

static esp_err_t inputs_read(bool *hook, bool buttons[PHONE_CALL_TARGETS])
{
#if CONFIG_DIGITS_PHONE_HOOK_OFF_HOOK_LOW
    *hook = gpio_get_level(CONFIG_DIGITS_PHONE_HOOK_GPIO) == 0;
#else
    *hook = gpio_get_level(CONFIG_DIGITS_PHONE_HOOK_GPIO) != 0;
#endif
    buttons[0] = gpio_get_level(CONFIG_DIGITS_PHONE_CALL_A_GPIO) == 0;
    buttons[1] = gpio_get_level(CONFIG_DIGITS_PHONE_CALL_B_GPIO) == 0;
    buttons[2] = gpio_get_level(CONFIG_DIGITS_PHONE_CALL_C_GPIO) == 0;
    return ESP_OK;
}
#endif

static bool valid_number(const char *number)
{
    size_t length = strlen(number);
    if (!length || length >= NUMBER_CAPACITY) return false;
    for (size_t i = 0; i < length; ++i)
        if (!isdigit((unsigned char)number[i])) return false;
    return true;
}

static void load_numbers(void)
{
    char numbers[PHONE_CALL_TARGETS][NUMBER_CAPACITY] = {{0}};
    uint8_t ringtones[PHONE_CALL_TARGETS] = {0};
    uint8_t selector[PHONE_CALL_TARGETS];
    // Default map from what this phone has; a stored one replaces it only if
    // it was made for the same number of positions.
    unsigned available[RINGTONE_FIRST_CLIP + RINGTONE_MAX_CLIPS];
    unsigned count = ringtones_list(available, sizeof(available) / sizeof(available[0]));
    ring_select_map_t map;
    ring_select_default_map(&map, SELECTOR_POSITIONS, available, count);
    for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) selector[i] = 0;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) {
        size_t length = NUMBER_CAPACITY;
        if (err != ESP_OK || nvs_get_str(nvs, nvs_keys[i], numbers[i], &length) != ESP_OK ||
            !valid_number(numbers[i]))
            snprintf(numbers[i], NUMBER_CAPACITY, "%s", default_numbers[i]);
        uint8_t ringtone = RINGTONE_STANDARD;
        if (err == ESP_OK && nvs_get_u8(nvs, ringtone_keys[i], &ringtone) == ESP_OK &&
            ringtones_sendable(ringtone))
            ringtones[i] = ringtone;
        uint8_t position;
        if (err == ESP_OK && nvs_get_u8(nvs, selector_keys[i], &position) == ESP_OK &&
            position < map.positions)
            selector[i] = position;
    }
    if (err == ESP_OK) {
        ring_select_map_t stored;
        size_t length = sizeof(stored);
        if (nvs_get_blob(nvs, MAP_KEY, &stored, &length) == ESP_OK && length == sizeof(stored) &&
            stored.positions == SELECTOR_POSITIONS) {
            ring_select_sanitize(&stored);
            map = stored;
        }
    }
    if (err == ESP_OK) nvs_close(nvs);
    else if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW(TAG, "Call button storage unavailable: %s; using build defaults", esp_err_to_name(err));
    portENTER_CRITICAL(&shared_mux);
    memcpy(shared.numbers, numbers, sizeof(numbers));
    memcpy(shared.ringtones, ringtones, sizeof(ringtones));
    memcpy(shared.selector, selector, sizeof(selector));
    shared.map = map;
    portEXIT_CRITICAL(&shared_mux);
}

static esp_err_t store_u8(const char *key, uint8_t value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, key, value);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t store_map(const ring_select_map_t *map)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, MAP_KEY, map, sizeof(*map));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

// Called by the WebRTC worker when an incoming call starts ringing.
static unsigned resolve_ringtone(const char *caller, int offered)
{
    int position = -1;
    ring_select_map_t map;
    portENTER_CRITICAL(&shared_mux);
    for (unsigned i = 0; i < PHONE_CALL_TARGETS && caller; ++i)
        if (shared.numbers[i][0] && !strcmp(shared.numbers[i], caller)) {
            position = shared.selector[i];
            break;
        }
    map = shared.map;
    portEXIT_CRITICAL(&shared_mux);
    return ring_select_resolve(&map, position, offered, ringtones_valid);
}

// Analog selectors: ADC1 only (ADC2 is unavailable while Wi-Fi runs).
static adc_oneshot_unit_handle_t adc_unit;
static adc_channel_t adc_channels[PHONE_CALL_TARGETS];

static void selectors_init(void)
{
    for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) {
        if (selector_gpios[i] < 0) continue;
        adc_unit_t unit;
        if (adc_oneshot_io_to_channel(selector_gpios[i], &unit, &adc_channels[i]) != ESP_OK || unit != ADC_UNIT_1) {
            ESP_LOGE(TAG, "Selector %s: GPIO%d is not an ADC1 pin; using the console position",
                     target_names[i], selector_gpios[i]);
            continue;
        }
        if (!adc_unit) {
            adc_oneshot_unit_init_cfg_t unit_config = {.unit_id = ADC_UNIT_1};
            if (adc_oneshot_new_unit(&unit_config, &adc_unit) != ESP_OK) {
                ESP_LOGE(TAG, "ADC unavailable; selectors use console positions");
                return;
            }
        }
        adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
        if (adc_oneshot_config_channel(adc_unit, adc_channels[i], &channel) != ESP_OK) continue;
        portENTER_CRITICAL(&shared_mux);
        shared.selector_hardware[i] = true;
        portEXIT_CRITICAL(&shared_mux);
        ESP_LOGI(TAG, "Selector %s: GPIO%d, %u positions", target_names[i], selector_gpios[i],
                 (unsigned)SELECTOR_POSITIONS);
    }
}

static void selectors_poll(int previous[PHONE_CALL_TARGETS])
{
    for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) {
        portENTER_CRITICAL(&shared_mux);
        bool hardware = shared.selector_hardware[i];
        unsigned positions = shared.map.positions;
        portEXIT_CRITICAL(&shared_mux);
        if (!hardware) continue;
        int sum = 0, value = 0, samples = 0;
        for (unsigned s = 0; s < SELECTOR_SAMPLES; ++s)
            if (adc_oneshot_read(adc_unit, adc_channels[i], &value) == ESP_OK) {
                sum += value;
                ++samples;
            }
        if (!samples) continue;
        unsigned position = ring_select_position((unsigned)(sum / samples), ADC_FULL_SCALE, positions, previous[i]);
        if ((int)position != previous[i]) {
            ESP_LOGI(TAG, "Selector %s: position %u", target_names[i], position + 1);
            previous[i] = (int)position;
            portENTER_CRITICAL(&shared_mux);
            shared.selector[i] = (uint8_t)position;
            portEXIT_CRITICAL(&shared_mux);
        }
    }
}

static esp_err_t store_ringtone(unsigned target, uint8_t ringtone)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, ringtone_keys[target], ringtone);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t store_number(unsigned target, const char *number)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = number ? nvs_set_str(nvs, nvs_keys[target], number) : nvs_erase_key(nvs, nvs_keys[target]);
    if (err == ESP_ERR_NVS_NOT_FOUND && !number) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void copy_number(unsigned target, char out[NUMBER_CAPACITY])
{
    portENTER_CRITICAL(&shared_mux);
    memcpy(out, shared.numbers[target], NUMBER_CAPACITY);
    portEXIT_CRITICAL(&shared_mux);
}

_Static_assert(PHONE_END_FAILED == (phone_end_t)DIGITS_CALL_END_FAILED, "end reason order");

static const char *ignored_reason(const phone_fsm_t *fsm, bool off_hook)
{
    if (!off_hook) return "lift the handset first";
    switch (fsm->line) {
    case PHONE_LINE_BUSY:
    case PHONE_LINE_REORDER:
    case PHONE_LINE_LOCKOUT:
        return "replace the handset first";
    default:
        return "a call is already in progress";
    }
}

static void run_action(phone_fsm_t *fsm, const phone_output_t *out,
                       const digits_webrtc_call_state_t *call, bool off_hook, int64_t now)
{
    esp_err_t err = ESP_OK;
    switch (out->action) {
    case PHONE_ACTION_NONE:
        return;
    case PHONE_ACTION_CALL: {
        char number[NUMBER_CAPACITY];
        copy_number(out->target, number);
        if (!number[0]) {
            ESP_LOGW(TAG, "Call button %s has no number; use 'phone set %c NUMBER'",
                     target_names[out->target], (char)tolower((unsigned char)target_names[out->target][0]));
            phone_fsm_call_rejected(fsm, now);
            return;
        }
        portENTER_CRITICAL(&shared_mux);
        unsigned ringtone = shared.ringtones[out->target];
        portEXIT_CRITICAL(&shared_mux);
        ESP_LOGI(TAG, "Call button %s: calling %s with ringtone %u (%s)", target_names[out->target],
                 number, ringtone, ringtones_name(ringtone));
        err = digits_webrtc_call(number, ringtone);
        if (err != ESP_OK) phone_fsm_call_rejected(fsm, now);
        break;
    }
    case PHONE_ACTION_ANSWER:
        ESP_LOGI(TAG, "Answering call from %s", call->remote[0] ? call->remote : "unknown");
        err = digits_webrtc_command("answer", NULL);
        break;
    case PHONE_ACTION_HANGUP:
        ESP_LOGI(TAG, "Handset replaced: ending %s call", phase_names[call->phase]);
        err = digits_webrtc_command("hangup", NULL);
        break;
    case PHONE_ACTION_IGNORED_PRESS:
        ESP_LOGI(TAG, "Call button %s ignored: %s", target_names[out->target],
                 ignored_reason(fsm, off_hook));
        return;
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "Phone command rejected: %s (signaling %s, %s)", esp_err_to_name(err),
                 call->online ? "online" : "offline", call->paired ? "paired" : "unpaired");
}

static void phone_task(void *arg)
{
    (void)arg;
    phone_debounce_t hook_input = {0}, buttons[PHONE_CALL_TARGETS] = {{0}};
    phone_fsm_t fsm = {0};
    bool off_hook = false, inputs_ok = false;
#if HOOK_IS_TOGGLE
    uint32_t hook_presses = 0;
#else
    bool hook_known = false;
#endif
    int64_t next_error_log = 0;
    bool previewing = false;
    int selector_previous[PHONE_CALL_TARGETS] = {-1, -1, -1};
    unsigned polls = 0;
    int64_t held_since[PHONE_CALL_TARGETS] = {0};
    bool hold_used[PHONE_CALL_TARGETS] = {false};
    TickType_t poll_ticks = pdMS_TO_TICKS(POLL_MS) ? pdMS_TO_TICKS(POLL_MS) : 1;
    for (;;) {
        int64_t now = esp_timer_get_time();
        bool hook_raw = false, raw[PHONE_CALL_TARGETS] = {false};
        esp_err_t err = inputs_read(&hook_raw, raw);
        bool previous_hook = off_hook;
        const char *source = NULL;
        if (err == ESP_OK) {
            inputs_ok = true;
            bool hook_level = phone_debounce_update(&hook_input, hook_raw, now);
#if HOOK_IS_TOGGLE
            // Momentary key: each confirmed press flips the simulated handset.
            (void)hook_level;
            if (hook_input.presses != hook_presses) {
                hook_presses = hook_input.presses;
                off_hook = !off_hook;
                source = "KEY1";
            }
#else
            // Level switch: follow it, including its position at boot.
            if (!hook_known || hook_level != off_hook) {
                if (hook_known) source = "hook switch";
                off_hook = hook_level;
                hook_known = true;
            }
#endif
            for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) {
                uint32_t before = buttons[i].presses;
                phone_debounce_update(&buttons[i], raw[i], now);
                if (buttons[i].presses != before)
                    ESP_LOGI(TAG, "Call button %s pressed", target_names[i]);
            }
        } else {
            inputs_ok = false;
            if (now >= next_error_log) {
                ESP_LOGE(TAG, "Input read failed: %s; keeping last state", esp_err_to_name(err));
                next_error_log = now + ERROR_LOG_INTERVAL_US;
            }
        }

        phone_inputs_t inputs = {0};
        portENTER_CRITICAL(&shared_mux);
        if (shared.sim_hook >= 0) {
            off_hook = shared.sim_hook == 1;
            shared.sim_hook = -1;
            source = "console";
        }
        for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i)
            inputs.presses[i] = buttons[i].presses + shared.sim_presses[i];
        shared.off_hook = off_hook;
        shared.inputs_ok = inputs_ok;
        portEXIT_CRITICAL(&shared_mux);

        if (off_hook != previous_hook && source)
            ESP_LOGI(TAG, "Handset %s (%s)", off_hook ? "lifted" : "replaced", source);
        if (off_hook != previous_hook) digits_incoming_ring_set_suppressed(off_hook);
        if (off_hook && !previous_hook) digits_voicemail_stop(); // Dial tone needs the speaker.
        digits_silence_poll();

        digits_webrtc_call_state_t call;
        digits_webrtc_call_state(&call);
        inputs.now_us = now;
        inputs.off_hook = off_hook;
        inputs.phase = (phone_phase_t)call.phase;
        inputs.ended_calls = call.ended_calls;
        inputs.last_end = (phone_end_t)call.last_end;
        inputs.last_connected = call.last_connected;
        phone_output_t out = phone_fsm_step(&fsm, &inputs);
        run_action(&fsm, &out, &call, off_hook, now);
        // A rejected call switches tone in the same poll, not the next one.
        if (out.action == PHONE_ACTION_CALL && fsm.line != PHONE_LINE_CALL)
            out.tone = TONE_GEN_NONE;
        // Previews run only on-hook with an idle line; lifting the handset
        // replaces them with dial tone, an incoming call silences them.
        portENTER_CRITICAL(&shared_mux);
        int preview = shared.preview_request;
        shared.preview_request = -1;
        portEXIT_CRITICAL(&shared_mux);
        if (preview >= 0) {
            if (!off_hook && call.phase == DIGITS_CALL_IDLE) {
                digits_call_tones_preview((unsigned)preview);
                previewing = true;
            } else {
                ESP_LOGW(TAG, "Ringtone preview needs the handset down and no call");
            }
        }
        if (previewing && (off_hook || call.phase != DIGITS_CALL_IDLE)) {
            previewing = false;
            if (!off_hook) digits_call_tones_stop();
        }
        digits_call_tones_play(out.tone); // Idempotent; keeps cadence.
        // Long press on hook with an idle line: open the setup network.
        for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) {
            bool holding = err == ESP_OK && buttons[i].stable && !off_hook && call.phase == DIGITS_CALL_IDLE;
            if (!holding) {
                held_since[i] = 0;
                hold_used[i] = false;
            } else if (!held_since[i]) {
                held_since[i] = now;
            } else if (!hold_used[i] && now - held_since[i] >= SETUP_HOLD_US) {
                hold_used[i] = true;
                ESP_LOGI(TAG, "Call button %s held: opening the setup network", target_names[i]);
                digits_setup_open("call button held", true);
                digits_ring_test_request(); // Short ring as confirmation.
            }
        }
        if (++polls % SELECTOR_POLL_DIVIDER == 0) selectors_poll(selector_previous);
        vTaskDelay(poll_ticks);
    }
}

esp_err_t digits_phone_start(void)
{
    portENTER_CRITICAL(&shared_mux);
    bool started = shared.started;
    shared.started = true;
    portEXIT_CRITICAL(&shared_mux);
    if (started) return ESP_ERR_INVALID_STATE;
    load_numbers();
    selectors_init();
    digits_webrtc_set_ringtone_resolver(resolve_ringtone);
    esp_err_t err = inputs_init();
    if (err == ESP_OK &&
        xTaskCreate(phone_task, "digits_phone", TASK_STACK_BYTES, NULL, TASK_PRIORITY, NULL) != pdPASS)
        err = ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        portENTER_CRITICAL(&shared_mux);
        shared.started = false;
        portEXIT_CRITICAL(&shared_mux);
        return err;
    }
    char a[NUMBER_CAPACITY], b[NUMBER_CAPACITY], c[NUMBER_CAPACITY];
    copy_number(0, a);
    copy_number(1, b);
    copy_number(2, c);
    ESP_LOGI(TAG, "Ready (%s); call A=%s B=%s C=%s", INPUT_DESCRIPTION,
             a[0] ? a : "unset", b[0] ? b : "unset", c[0] ? c : "unset");
    return ESP_OK;
}

static int parse_target(const char *text)
{
    if (!text || strlen(text) != 1) return -1;
    char c = (char)tolower((unsigned char)text[0]);
    return c == 'a' ? 0 : c == 'b' ? 1 : c == 'c' ? 2 : -1;
}

static void print_status(void)
{
    char numbers[PHONE_CALL_TARGETS][NUMBER_CAPACITY];
    uint8_t ringtones[PHONE_CALL_TARGETS], selector[PHONE_CALL_TARGETS];
    bool hardware[PHONE_CALL_TARGETS];
    portENTER_CRITICAL(&shared_mux);
    bool off_hook = shared.off_hook, inputs_ok = shared.inputs_ok, started = shared.started;
    memcpy(numbers, shared.numbers, sizeof(numbers));
    memcpy(ringtones, shared.ringtones, sizeof(ringtones));
    memcpy(selector, shared.selector, sizeof(selector));
    memcpy(hardware, shared.selector_hardware, sizeof(hardware));
    ring_select_map_t map = shared.map;
    portEXIT_CRITICAL(&shared_mux);
    digits_webrtc_call_state_t call;
    digits_webrtc_call_state(&call);
    printf("Phone: %s, handset %s, inputs %s (%s)\n", started ? "running" : "stopped",
           off_hook ? "lifted" : "on hook", inputs_ok ? "ok" : "not read yet or failing",
           INPUT_DESCRIPTION);
    printf("Call: %s%s%s, signaling %s, %s\n", phase_names[call.phase],
           call.remote[0] ? " with " : "", call.remote,
           call.online ? "online" : "offline", call.paired ? "paired" : "unpaired");
    digits_silence_state_t silence = digits_silence_state();
    printf("Incoming calls: %s (server says %s%s)\n", silence.effective ? "silent" : "ring",
           silence.server ? "silent" : "ring", silence.overridden ? ", overridden here" : "");
    if (digits_voicemail_unheard() >= 0)
        printf("Answering machine: %d new message(s)\n", digits_voicemail_unheard());
    for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) {
        printf("Call button %s: %s, rings their phone with %u (%s)\n", target_names[i],
               numbers[i][0] ? numbers[i] : "unset", ringtones[i], ringtones_name(ringtones[i]));
        unsigned position = selector[i];
        unsigned local = map.ringtone[position];
        if (ringtones_valid(RINGTONE_FIRST_LOCAL + position)) local = RINGTONE_FIRST_LOCAL + position;
        printf("  their calls ring here: selector %u/%u (%s) = %u (%s%s)\n", position + 1,
               map.positions, hardware[i] ? "switch" : "console", local,
               local >= RINGTONE_FIRST_LOCAL ? "SD card: " : "",
               ringtones_name(local) ? ringtones_name(local) : "missing, falls back");
    }
}

static void print_positions(void)
{
    portENTER_CRITICAL(&shared_mux);
    ring_select_map_t map = shared.map;
    portEXIT_CRITICAL(&shared_mux);
    for (unsigned p = 0; p < map.positions; ++p) {
        uint8_t ringtone = map.ringtone[p];
        printf("%2u  %u %s", p + 1, ringtone,
               ringtones_name(ringtone) ? ringtones_name(ringtone) : "(missing: falls back)");
        if (ringtones_valid(RINGTONE_FIRST_LOCAL + p))
            printf("  -> plays SD card %s instead", ringtones_name(RINGTONE_FIRST_LOCAL + p));
        printf("\n");
    }
}

static bool parse_position(const char *text, unsigned positions, unsigned *position)
{
    char *end = NULL;
    unsigned long value = text ? strtoul(text, &end, 10) : 0;
    if (!text || !text[0] || *end || value < 1 || value > positions) return false;
    *position = (unsigned)value - 1;
    return true;
}

// local: also accept SD card ringtones, which only this phone can play.
static bool parse_ringtone(const char *text, bool local, unsigned *ringtone)
{
    char *end = NULL;
    unsigned long value = text ? strtoul(text, &end, 10) : 0;
    if (!text || !text[0] || *end || value > UINT8_MAX) return false;
    if (local ? !ringtones_valid((unsigned)value) : !ringtones_sendable((unsigned)value)) return false;
    *ringtone = (unsigned)value;
    return true;
}

int digits_phone_console(int argc, char **argv)
{
    const char *verb = argc >= 2 ? argv[1] : "";
    int target = argc >= 3 ? parse_target(argv[2]) : -1;
    if (!strcmp(verb, "status") && argc == 2) {
        print_status();
        return 0;
    }
    if (!strcmp(verb, "set") && argc == 4 && target >= 0) {
        if (!valid_number(argv[3])) {
            printf("Number must be 1-%d digits\n", NUMBER_CAPACITY - 1);
            return 1;
        }
        esp_err_t err = store_number((unsigned)target, argv[3]);
        if (err != ESP_OK) {
            printf("Saving failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        portENTER_CRITICAL(&shared_mux);
        snprintf(shared.numbers[target], NUMBER_CAPACITY, "%s", argv[3]);
        portEXIT_CRITICAL(&shared_mux);
        printf("Call button %s now calls %s (saved)\n", target_names[target], argv[3]);
        return 0;
    }
    if (!strcmp(verb, "clear") && argc == 3 && target >= 0) {
        esp_err_t err = store_number((unsigned)target, NULL);
        if (err != ESP_OK) {
            printf("Clearing failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        portENTER_CRITICAL(&shared_mux);
        snprintf(shared.numbers[target], NUMBER_CAPACITY, "%s", default_numbers[target]);
        portEXIT_CRITICAL(&shared_mux);
        printf("Call button %s reset to build default: %s\n", target_names[target],
               default_numbers[target][0] ? default_numbers[target] : "unset");
        return 0;
    }
    if (!strcmp(verb, "hook") && argc == 3 && (!strcmp(argv[2], "up") || !strcmp(argv[2], "down"))) {
        portENTER_CRITICAL(&shared_mux);
        shared.sim_hook = !strcmp(argv[2], "up");
        portEXIT_CRITICAL(&shared_mux);
        printf("Simulated handset %s\n", !strcmp(argv[2], "up") ? "lift" : "replace");
        return 0;
    }
    if (!strcmp(verb, "press") && argc == 3 && target >= 0) {
        portENTER_CRITICAL(&shared_mux);
        ++shared.sim_presses[target];
        portEXIT_CRITICAL(&shared_mux);
        printf("Simulated call button %s press\n", target_names[target]);
        return 0;
    }
    if (!strcmp(verb, "ringtones") && argc == 2) {
        unsigned ids[RINGTONE_FIRST_CLIP + RINGTONE_MAX_CLIPS];
        unsigned count = ringtones_list(ids, sizeof(ids) / sizeof(ids[0]));
        for (unsigned i = 0; i < count; ++i)
            printf("%2u  %s%s\n", ids[i], ringtones_name(ids[i]),
                   ids[i] >= RINGTONE_FIRST_CLIP ? "  (clip)" : "");
        printf("SD card: %s\n", digits_sd_ringtones_status());
        for (unsigned id = RINGTONE_FIRST_LOCAL; id < RINGTONE_FIRST_LOCAL + RINGTONE_LOCAL_SLOTS; ++id)
            if (ringtones_valid(id))
                printf("%3u  %s  (SD card, position %u, %.1f s)\n", id, ringtones_name(id),
                       id - RINGTONE_FIRST_LOCAL + 1,
                       (double)ringtones_melody_frames(id) / RINGTONE_SAMPLE_RATE);
        return 0;
    }
    unsigned ringtone;
    if (!strcmp(verb, "ringtone") && argc == 4 && target >= 0) {
        if (!parse_ringtone(argv[3], false, &ringtone)) {
            printf("Unknown ringtone; see 'phone ringtones'\n");
            return 1;
        }
        esp_err_t err = store_ringtone((unsigned)target, (uint8_t)ringtone);
        if (err != ESP_OK) {
            printf("Saving failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        portENTER_CRITICAL(&shared_mux);
        shared.ringtones[target] = (uint8_t)ringtone;
        portEXIT_CRITICAL(&shared_mux);
        printf("Calls from button %s now ring with %u (%s) (saved)\n", target_names[target],
               ringtone, ringtones_name(ringtone));
        return 0;
    }
    if (!strcmp(verb, "silence") && argc == 3) {
        if (!strcmp(argv[2], "on")) digits_silence_set(true);
        else if (!strcmp(argv[2], "off")) digits_silence_set(false);
        else if (!strcmp(argv[2], "toggle")) digits_silence_toggle();
        else {
            printf("Usage: phone silence on|off|toggle\n");
            return 1;
        }
        digits_silence_state_t silence = digits_silence_state();
        printf("Incoming calls now %s%s\n", silence.effective ? "silent" : "ring",
               silence.overridden ? " until the server setting changes" : "");
        return 0;
    }
    if (!strcmp(verb, "positions") && argc == 2) {
        print_positions();
        return 0;
    }
    portENTER_CRITICAL(&shared_mux);
    ring_select_map_t map = shared.map;
    portEXIT_CRITICAL(&shared_mux);
    unsigned position;
    if (!strcmp(verb, "position") && argc == 4) {
        if (!parse_position(argv[2], map.positions, &position)) {
            printf("Position must be 1-%u\n", map.positions);
            return 1;
        }
        if (!parse_ringtone(argv[3], false, &ringtone)) {
            printf("Unknown ringtone; see 'phone ringtones'\n");
            return 1;
        }
        map.ringtone[position] = (uint8_t)ringtone;
        esp_err_t err = store_map(&map);
        if (err != ESP_OK) {
            printf("Saving failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        portENTER_CRITICAL(&shared_mux);
        shared.map = map;
        portEXIT_CRITICAL(&shared_mux);
        printf("Selector position %u now rings with %u (%s) (saved)\n", position + 1, ringtone,
               ringtones_name(ringtone));
        return 0;
    }
    if (!strcmp(verb, "selector") && argc == 4 && target >= 0) {
        if (!parse_position(argv[3], map.positions, &position)) {
            printf("Position must be 1-%u\n", map.positions);
            return 1;
        }
        portENTER_CRITICAL(&shared_mux);
        bool hardware = shared.selector_hardware[target];
        portEXIT_CRITICAL(&shared_mux);
        if (hardware) {
            printf("Selector %s is a physical switch; turn it instead\n", target_names[target]);
            return 1;
        }
        esp_err_t err = store_u8(selector_keys[target], (uint8_t)position);
        if (err != ESP_OK) {
            printf("Saving failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        portENTER_CRITICAL(&shared_mux);
        shared.selector[target] = (uint8_t)position;
        portEXIT_CRITICAL(&shared_mux);
        printf("Selector %s set to position %u (saved)\n", target_names[target], position + 1);
        return 0;
    }
    if (!strcmp(verb, "preview") && argc == 3 && parse_ringtone(argv[2], true, &ringtone)) {
        portENTER_CRITICAL(&shared_mux);
        shared.preview_request = (int)ringtone;
        portEXIT_CRITICAL(&shared_mux);
        printf("Previewing %u (%s)\n", ringtone, ringtones_name(ringtone));
        return 0;
    }
    printf("Usage: phone status | phone set a|b|c NUMBER | phone clear a|b|c | "
           "phone ringtones | phone ringtone a|b|c N | phone preview N | "
           "phone positions | phone position P N | phone selector a|b|c P | "
           "phone silence on|off|toggle | phone hook up|down | phone press a|b|c\n");
    return 1;
}

#else

esp_err_t digits_phone_start(void) { return ESP_OK; }

int digits_phone_console(int argc, char **argv)
{
    (void)argc; (void)argv;
    return 1;
}

#endif
