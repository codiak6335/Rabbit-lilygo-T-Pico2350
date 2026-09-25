#pragma once

#include <cstdint>

namespace rabbit::rp2350 {

// These onboard assignments are taken from LilyGO's T-Pico2 factory example.
// External LED and audio pins are deliberately build-time options until the
// physical Rabbit installation is measured and recorded.
constexpr std::uint8_t kI2cSdaPin = 0;
constexpr std::uint8_t kI2cSclPin = 1;
constexpr std::uint8_t kExpanderAddress = 0x24;
constexpr std::uint8_t kExpanderOutputPort0Register = 0x02;
constexpr std::uint8_t kExpanderConfigPort0Register = 0x06;
constexpr std::uint8_t kEspEnableMask = 1U << 3U;
constexpr std::uint8_t kEspUartTxPin = 28;
constexpr std::uint8_t kEspUartRxPin = 29;
constexpr std::uint8_t kEspUartRtsPin = 27;
constexpr std::uint8_t kEspUartCtsPin = 26;
constexpr int kLocalStopButtonPin = RABBIT_LOCAL_STOP_PIN;
constexpr int kLedDataPin = RABBIT_LED_PIN;
constexpr int kAudioPin = RABBIT_AUDIO_PIN;
constexpr std::uint16_t kMaxLedPixels = 910;

}  // namespace rabbit::rp2350
