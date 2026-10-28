#pragma once

#include "esp_http_server.h"

// Settings on the status page: call buttons, ringtone positions and SD
// card songs, answering-machine messages and greeting. Reading needs the
// setup password (when one is set); changing anything needs a configured
// password. NVS writes run on an internal-RAM worker task.
void digits_web_config_register(httpd_handle_t server);
