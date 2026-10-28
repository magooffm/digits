#include "webrtc_console.h"
#include "sdkconfig.h"

#ifdef CONFIG_DIGITS_WEBRTC_CONSOLE
#include "webrtc.h"
#include "phone.h"
#include "tx_voice.h"
#include "voicemail.h"
#include "wifi.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"

static esp_console_repl_t *console;

static int webrtc_command(int argc, char **argv)
{
    bool call = argc >= 2 && strcmp(argv[1], "call") == 0;
    bool audio = argc >= 2 && strcmp(argv[1], "audio") == 0;
    bool action = argc >= 2 &&
        (strcmp(argv[1], "answer") == 0 || strcmp(argv[1], "hangup") == 0 ||
         strcmp(argv[1], "status") == 0);
    if (((call || audio) && argc != 3) || (action && argc != 2) || (!call && !action && !audio)) {
        printf("Usage: webrtc call NUMBER | webrtc answer | webrtc hangup | webrtc status | webrtc audio tx|rx|full\n");
        return 1;
    }
    // The worker validates the number and call state. Never wait for ICE/DTLS
    // or write to the WebSocket from this low-priority console callback.
    esp_err_t err = digits_webrtc_command(argv[1], call || audio ? argv[2] : NULL);
    if (err != ESP_OK) {
        printf("WebRTC command rejected: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("WebRTC %s queued; connection details follow in the logs\n", argv[1]);
    return 0;
}
#endif

esp_err_t digits_webrtc_console_start(void)
{
#ifdef CONFIG_DIGITS_WEBRTC_CONSOLE
    if (console) return ESP_ERR_INVALID_STATE;
    // Register before creating the REPL, following IDF's basic console example.
    // The REPL helper initializes esp_console and installs its help command.
    const esp_console_cmd_t command = {
        .command = "webrtc",
        .help = "WebRTC call NUMBER, answer, hangup, status; audio tx|rx|full selects the next call's audio.",
        .hint = "call NUMBER | answer | hangup | status | audio tx|rx|full",
        .func = webrtc_command,
    };
    esp_err_t err = esp_console_cmd_register(&command);
    if (err != ESP_OK) return err;
#ifdef CONFIG_DIGITS_PHONE
    const esp_console_cmd_t phone = {
        .command = "phone",
        .help = "Phone controls: status, set/clear a|b|c, ringtones, ringtone a|b|c N (how they "
                "ring), preview N, positions, position P N, selector a|b|c P (how they ring here); "
                "silence on|off|toggle (like the silence button); "
                "hook up|down and press a|b|c simulate inputs.",
        .hint = "status | set | clear | ringtones | ringtone | preview | positions | position | selector | silence | hook | press",
        .func = digits_phone_console,
    };
    err = esp_console_cmd_register(&phone);
    if (err != ESP_OK) return err;
#endif
    const esp_console_cmd_t voicemail = {
        .command = "vm",
        .help = "Answering machine: status, list, play [N] (all new messages, or message N), "
                "greeting (hear it), stop, heard N, delete N, delete heard.",
        .hint = "status | list | play [N] | greeting | stop | heard N | delete N|heard",
        .func = digits_voicemail_console,
    };
    err = esp_console_cmd_register(&voicemail);
    if (err != ESP_OK) return err;
    const esp_console_cmd_t voice = {
        .command = "voice",
        .help = "Outgoing voice: status; aec on|off switches echo cancellation until restart (for "
                "comparison, also mid-call); gain DB saves the microphone gain (0-30 dB).",
        .hint = "status | aec on|off | gain DB",
        .func = tx_voice_console,
    };
    err = esp_console_cmd_register(&voice);
    if (err != ESP_OK) return err;
    const esp_console_cmd_t wifi = {
        .command = "wifi",
        .help = "Wi-Fi and server: status, set SSID [PASSWORD] and server URL (save and restart; quote "
                "names with spaces), forget (back to the build defaults), setup [lan] (open the setup "
                "network now; lan also serves the page on the home network), close.",
        .hint = "status | set SSID [PASSWORD] | server URL | forget | setup [lan] | close",
        .func = digits_wifi_console,
    };
    err = esp_console_cmd_register(&wifi);
    if (err != ESP_OK) return err;

    esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    config.prompt = "digits>";
    config.task_stack_size = 6144; // vm list reads files on the SD card.
    config.task_priority = 3;
    config.max_cmdline_length = 96;
    config.max_history_len = 8;
    config.history_save_path = NULL; // Bounded RAM history; never write NVS.
    esp_console_dev_usb_serial_jtag_config_t device = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&device, &config, &console);
    if (err != ESP_OK) {
        // Construction can deinitialize the command registry on failure.
        (void)esp_console_cmd_deregister("webrtc");
#ifdef CONFIG_DIGITS_PHONE
        (void)esp_console_cmd_deregister("phone");
#endif
        (void)esp_console_cmd_deregister("vm");
        (void)esp_console_cmd_deregister("voice");
        (void)esp_console_cmd_deregister("wifi");
        return err;
    }
    err = esp_console_start_repl(console);
    if (err == ESP_OK)
        ESP_LOGI("webrtc_console", "USB console ready: webrtc call NUMBER / answer / hangup / status");
    return err;
#else
    return ESP_OK;
#endif
}
