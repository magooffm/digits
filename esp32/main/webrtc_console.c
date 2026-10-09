#include "webrtc_console.h"
#include "sdkconfig.h"

#ifdef CONFIG_DIGITS_WEBRTC_CONSOLE
#include "webrtc.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"

static esp_console_repl_t *console;

static int webrtc_command(int argc, char **argv)
{
    bool call = argc >= 2 && strcmp(argv[1], "call") == 0;
    bool action = argc >= 2 &&
        (strcmp(argv[1], "answer") == 0 || strcmp(argv[1], "hangup") == 0 ||
         strcmp(argv[1], "status") == 0);
    if ((call && argc != 3) || (action && argc != 2) || (!call && !action)) {
        printf("Usage: webrtc call NUMBER | webrtc answer | webrtc hangup | webrtc status\n");
        return 1;
    }
    // The worker validates the number and call state. Never wait for ICE/DTLS
    // or write to the WebSocket from this low-priority console callback.
    esp_err_t err = digits_webrtc_command(argv[1], call ? argv[2] : NULL);
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
        .help = "Development WebRTC commands: call NUMBER, answer, hangup, status. Sends Opus silence; received media is counted.",
        .hint = "call NUMBER | answer | hangup | status",
        .func = webrtc_command,
    };
    esp_err_t err = esp_console_cmd_register(&command);
    if (err != ESP_OK) return err;

    esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    config.prompt = "digits>";
    config.task_stack_size = 4096;
    config.task_priority = 3;
    config.max_cmdline_length = 96;
    config.max_history_len = 8;
    config.history_save_path = NULL; // Bounded RAM history; never write NVS.
    esp_console_dev_usb_serial_jtag_config_t device = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&device, &config, &console);
    if (err != ESP_OK) {
        // Construction can deinitialize the command registry on failure.
        (void)esp_console_cmd_deregister("webrtc");
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
