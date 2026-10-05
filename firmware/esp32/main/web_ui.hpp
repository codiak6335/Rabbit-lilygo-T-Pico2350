#pragma once

#include "esp_http_server.h"

namespace rabbit::esp32 {

// Registers the checked-in Coach On Deck UI and bounded local configuration API.
bool register_web_ui(httpd_handle_t server);

}  // namespace rabbit::esp32
