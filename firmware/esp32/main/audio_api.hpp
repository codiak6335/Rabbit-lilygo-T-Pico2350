#pragma once
#include "esp_http_server.h"
#include "rabbit/protocol/audio_service.hpp"
namespace rabbit::esp32 {
using AudioCall = bool (*)(protocol::MessageType, const std::uint8_t*, std::uint16_t, protocol::Frame&);
bool register_audio_api(httpd_handle_t server, AudioCall call);
}
