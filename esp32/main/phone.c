#include "phone.h"
#include "sdkconfig.h"

#if CONFIG_DIGITS_PHONE
#include "phone_fsm.h"
#include "audio_board.h"
#include "incoming_ring.h"
#include "webrtc.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
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
#define TASK_STACK_BYTES 4096
#define TASK_PRIORITY 3

static const char *const target_names[PHONE_CALL_TARGETS] = {"A", "B"};
static const char *const nvs_keys[PHONE_CALL_TARGETS] = {"call_a", "call_b"};
static const char *const default_numbers[PHONE_CALL_TARGETS] = {
    CONFIG_DIGITS_PHONE_CALL_A_NUMBER, CONFIG_DIGITS_PHONE_CALL_B_NUMBER,
};
static const char *const phase_names[] = {"idle", "calling", "ringing", "connecting", "connected"};

// Shared between the phone task and the console task.
static portMUX_TYPE shared_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool started;
    char numbers[PHONE_CALL_TARGETS][NUMBER_CAPACITY];
    int sim_hook; // -1 none pending, 0 replace, 1 lift.
    uint32_t sim_presses[PHONE_CALL_TARGETS];
    bool off_hook;
    bool inputs_ok;
} shared = {.sim_hook = -1};

#if CONFIG_DIGITS_PHONE_INPUT_BOARD_KEYS
#define HOOK_IS_TOGGLE 1
#define INPUT_DESCRIPTION "board keys: KEY1 lifts/replaces handset, KEY2 calls A, KEY3 calls B"

static esp_err_t inputs_init(void) { return digits_audio_board_keys_init(); }

static esp_err_t inputs_read(bool *hook, bool buttons[PHONE_CALL_TARGETS])
{
    uint8_t keys = 0;
    esp_err_t err = digits_audio_board_keys_read(&keys);
    if (err != ESP_OK) return err;
    *hook = keys & 0x1;
    buttons[0] = keys & 0x2;
    buttons[1] = keys & 0x4;
    return ESP_OK;
}
#else
#define HOOK_IS_TOGGLE 0
#define INPUT_DESCRIPTION "GPIO: hook switch, call buttons A and B"

static esp_err_t inputs_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << CONFIG_DIGITS_PHONE_HOOK_GPIO) |
                        (1ULL << CONFIG_DIGITS_PHONE_CALL_A_GPIO) |
                        (1ULL << CONFIG_DIGITS_PHONE_CALL_B_GPIO),
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
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i) {
        size_t length = NUMBER_CAPACITY;
        if (err != ESP_OK || nvs_get_str(nvs, nvs_keys[i], numbers[i], &length) != ESP_OK ||
            !valid_number(numbers[i]))
            snprintf(numbers[i], NUMBER_CAPACITY, "%s", default_numbers[i]);
    }
    if (err == ESP_OK) nvs_close(nvs);
    else if (err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW(TAG, "Call button storage unavailable: %s; using build defaults", esp_err_to_name(err));
    portENTER_CRITICAL(&shared_mux);
    memcpy(shared.numbers, numbers, sizeof(numbers));
    portEXIT_CRITICAL(&shared_mux);
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

static void run_action(const phone_output_t *out, const digits_webrtc_call_state_t *call, bool off_hook)
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
            return;
        }
        ESP_LOGI(TAG, "Call button %s: calling %s", target_names[out->target], number);
        err = digits_webrtc_command("call", number);
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
                 !off_hook ? "lift the handset first" : "a call is already in progress");
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

        digits_webrtc_call_state_t call;
        digits_webrtc_call_state(&call);
        inputs.off_hook = off_hook;
        inputs.phase = (phone_phase_t)call.phase;
        phone_output_t out = phone_fsm_step(&fsm, &inputs);
        run_action(&out, &call, off_hook);
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
    char a[NUMBER_CAPACITY], b[NUMBER_CAPACITY];
    copy_number(0, a);
    copy_number(1, b);
    ESP_LOGI(TAG, "Ready (%s); call A=%s call B=%s", INPUT_DESCRIPTION,
             a[0] ? a : "unset", b[0] ? b : "unset");
    return ESP_OK;
}

static int parse_target(const char *text)
{
    if (!text || strlen(text) != 1) return -1;
    char c = (char)tolower((unsigned char)text[0]);
    return c == 'a' ? 0 : c == 'b' ? 1 : -1;
}

static void print_status(void)
{
    char numbers[PHONE_CALL_TARGETS][NUMBER_CAPACITY];
    portENTER_CRITICAL(&shared_mux);
    bool off_hook = shared.off_hook, inputs_ok = shared.inputs_ok, started = shared.started;
    memcpy(numbers, shared.numbers, sizeof(numbers));
    portEXIT_CRITICAL(&shared_mux);
    digits_webrtc_call_state_t call;
    digits_webrtc_call_state(&call);
    printf("Phone: %s, handset %s, inputs %s (%s)\n", started ? "running" : "stopped",
           off_hook ? "lifted" : "on hook", inputs_ok ? "ok" : "not read yet or failing",
           INPUT_DESCRIPTION);
    printf("Call: %s%s%s, signaling %s, %s\n", phase_names[call.phase],
           call.remote[0] ? " with " : "", call.remote,
           call.online ? "online" : "offline", call.paired ? "paired" : "unpaired");
    for (unsigned i = 0; i < PHONE_CALL_TARGETS; ++i)
        printf("Call button %s: %s\n", target_names[i], numbers[i][0] ? numbers[i] : "unset");
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
    printf("Usage: phone status | phone set a|b NUMBER | phone clear a|b | "
           "phone hook up|down | phone press a|b\n");
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
