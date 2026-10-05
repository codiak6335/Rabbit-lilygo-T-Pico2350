#pragma once

#include <cstdint>

namespace rabbit::rp2350 {

// Waveshare RP2350B Plus W physical pins 1 and 2 connect to the ESP32-C6.
// External LED and audio pins remain build-time options until verified.
constexpr std::uint8_t kEspUartTxPin = 0;
constexpr std::uint8_t kEspUartRxPin = 1;
constexpr int kLocalStopButtonPin = RABBIT_LOCAL_STOP_PIN;
constexpr int kLedDataPin = RABBIT_LED_PIN;
constexpr int kAudioPin = RABBIT_AUDIO_PIN;
constexpr std::uint16_t kMaxLedPixels = 910;

}  // namespace rabbit::rp2350
