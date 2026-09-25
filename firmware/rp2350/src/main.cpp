#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#include "rabbit/rp2350/board_config.hpp"
#include "rabbit/rp2350/uart_workout_link.hpp"
#include "rabbit/rp2350/ws2812_dma.hpp"

namespace {
constexpr rabbit::core::Microseconds kAudioTestDurationUs = 150'000;
constexpr rabbit::core::Microseconds kLedChaseIntervalUs = 75'000;
constexpr std::uint32_t kI2cTimeoutUs = 20'000;

struct Diagnostics {
    char command[24]{};
    std::uint8_t command_size{0};
    bool audio_test_active{false};
    bool led_test_active{false};
    std::uint16_t led_test_pixel{0};
    rabbit::core::Microseconds audio_test_deadline_us{0};
    rabbit::core::Microseconds led_test_deadline_us{0};
};

rabbit::core::PoolProfile transport_pool() {
    rabbit::core::PoolProfile pool{};
    static_cast<void>(pool.name.assign("Controller Pool", 15));
    pool.unit = rabbit::core::DistanceUnit::Yard;
    pool.length_mm = 22'860;
    pool.pixel_count = 1;
    pool.segment_count = 1;
    pool.segments[0] = {0, 0};
    pool.revision = 1;
    return pool;
}

void configure_uart() {
    uart_init(uart0, 115200);
    gpio_set_function(rabbit::rp2350::kEspUartTxPin, GPIO_FUNC_UART);
    gpio_set_function(rabbit::rp2350::kEspUartRxPin, GPIO_FUNC_UART);
    uart_set_hw_flow(uart0, false, false);
}

bool read_expander_register(const std::uint8_t register_address, std::uint8_t& value) {
    if (i2c_write_timeout_us(i2c0, rabbit::rp2350::kExpanderAddress, &register_address, 1, true,
                             kI2cTimeoutUs) != 1) {
        return false;
    }
    return i2c_read_timeout_us(i2c0, rabbit::rp2350::kExpanderAddress, &value, 1, false,
                               kI2cTimeoutUs) == 1;
}

bool write_expander_register(const std::uint8_t register_address, const std::uint8_t value) {
    const std::uint8_t bytes[]{register_address, value};
    return i2c_write_timeout_us(i2c0, rabbit::rp2350::kExpanderAddress, bytes, sizeof(bytes), false,
                                kI2cTimeoutUs) == static_cast<int>(sizeof(bytes));
}

bool release_esp32() {
    i2c_init(i2c0, 100'000);
    gpio_set_function(rabbit::rp2350::kI2cSdaPin, GPIO_FUNC_I2C);
    gpio_set_function(rabbit::rp2350::kI2cSclPin, GPIO_FUNC_I2C);
    gpio_pull_up(rabbit::rp2350::kI2cSdaPin);
    gpio_pull_up(rabbit::rp2350::kI2cSclPin);

    std::uint8_t configuration{};
    std::uint8_t outputs{};
    if (!read_expander_register(rabbit::rp2350::kExpanderConfigPort0Register, configuration) ||
        !write_expander_register(rabbit::rp2350::kExpanderConfigPort0Register,
                                 static_cast<std::uint8_t>(configuration & ~rabbit::rp2350::kEspEnableMask)) ||
        !read_expander_register(rabbit::rp2350::kExpanderOutputPort0Register, outputs) ||
        !write_expander_register(rabbit::rp2350::kExpanderOutputPort0Register,
                                 static_cast<std::uint8_t>(outputs & ~rabbit::rp2350::kEspEnableMask))) {
        return false;
    }
    sleep_ms(100);
    return write_expander_register(rabbit::rp2350::kExpanderOutputPort0Register,
                                   static_cast<std::uint8_t>(outputs | rabbit::rp2350::kEspEnableMask));
}

const char* esp_enable_state() {
    std::uint8_t configuration{};
    std::uint8_t outputs{};
    if (!read_expander_register(rabbit::rp2350::kExpanderConfigPort0Register, configuration) ||
        !read_expander_register(rabbit::rp2350::kExpanderOutputPort0Register, outputs)) {
        return "unavailable";
    }
    if ((configuration & rabbit::rp2350::kEspEnableMask) != 0U) return "input";
    return (outputs & rabbit::rp2350::kEspEnableMask) != 0U ? "released" : "held-low";
}

void print_help() {
    std::puts("commands: help status led audio off");
}

void print_status(
    const rabbit::rp2350::Ws2812Dma& leds,
    const rabbit::protocol::WorkoutService& workout,
    const rabbit::rp2350::UartWorkoutLink& uart_link,
    const bool local_stop_pressed) {
    const auto snapshot = workout.snapshot(0);
    const auto& uart_stats = uart_link.stats();
    std::printf(
        "status state=%u led=%s led_pin=%d audio_pin=%d stop=%s esp=%s uart_rx=%lu uart_req=%lu uart_tx=%lu uart_rsp=%lu\n",
        static_cast<unsigned int>(snapshot.state), leds.enabled() ? "ready" : "disabled",
        rabbit::rp2350::kLedDataPin, rabbit::rp2350::kAudioPin,
        rabbit::rp2350::kLocalStopButtonPin < 0 ? "unconfigured" :
            (local_stop_pressed ? "pressed" : "released"), esp_enable_state(),
        static_cast<unsigned long>(uart_stats.received_bytes), static_cast<unsigned long>(uart_stats.received_frames),
        static_cast<unsigned long>(uart_stats.transmitted_bytes), static_cast<unsigned long>(uart_stats.transmitted_frames));
}

void clear_leds(rabbit::rp2350::Ws2812Dma& leds) {
    if (!leds.enabled()) return;
    leds.clear();
    leds.show();
}

void handle_command(
    Diagnostics& diagnostics,
    rabbit::rp2350::Ws2812Dma& leds,
    const rabbit::protocol::WorkoutService& workout,
    const rabbit::rp2350::UartWorkoutLink& uart_link,
    const bool local_stop_pressed,
    const rabbit::core::Microseconds now_us) {
    diagnostics.command[diagnostics.command_size] = '\0';
    if (std::strcmp(diagnostics.command, "help") == 0) {
        print_help();
    } else if (std::strcmp(diagnostics.command, "status") == 0) {
        print_status(leds, workout, uart_link, local_stop_pressed);
    } else if (std::strcmp(diagnostics.command, "led") == 0) {
        if (!leds.enabled()) {
            std::puts("led test unavailable: configure a verified RABBIT_LED_PIN");
        } else {
            diagnostics.led_test_active = true;
            diagnostics.led_test_pixel = 0;
            diagnostics.led_test_deadline_us = now_us;
            std::puts("led chase started; send off to stop");
        }
    } else if (std::strcmp(diagnostics.command, "audio") == 0) {
        if (rabbit::rp2350::kAudioPin < 0) {
            std::puts("audio test unavailable: configure a verified RABBIT_AUDIO_PIN");
        } else {
            diagnostics.audio_test_active = true;
            diagnostics.audio_test_deadline_us = now_us + kAudioTestDurationUs;
            std::puts("audio chirp started");
        }
    } else if (std::strcmp(diagnostics.command, "off") == 0) {
        diagnostics.led_test_active = false;
        diagnostics.audio_test_active = false;
        clear_leds(leds);
        std::puts("diagnostics stopped");
    } else if (diagnostics.command_size != 0U) {
        std::puts("unknown command");
        print_help();
    }
    diagnostics.command_size = 0;
}

void poll_console(
    Diagnostics& diagnostics,
    rabbit::rp2350::Ws2812Dma& leds,
    const rabbit::protocol::WorkoutService& workout,
    const rabbit::rp2350::UartWorkoutLink& uart_link,
    const bool local_stop_pressed,
    const rabbit::core::Microseconds now_us) {
    int next_character = getchar_timeout_us(0);
    while (next_character != PICO_ERROR_TIMEOUT) {
        const char character = static_cast<char>(next_character);
        if (character == '\r' || character == '\n') {
            if (diagnostics.command_size != 0U) {
                handle_command(diagnostics, leds, workout, uart_link, local_stop_pressed, now_us);
            }
        } else if (diagnostics.command_size < sizeof(diagnostics.command) - 1U) {
            diagnostics.command[diagnostics.command_size] = character;
            ++diagnostics.command_size;
        }
        next_character = getchar_timeout_us(0);
    }
}

bool update_diagnostics(
    Diagnostics& diagnostics,
    rabbit::rp2350::Ws2812Dma& leds,
    const rabbit::core::Microseconds now_us) {
    if (diagnostics.audio_test_active && now_us >= diagnostics.audio_test_deadline_us) {
        diagnostics.audio_test_active = false;
        std::puts("audio chirp complete");
    }
    if (!diagnostics.led_test_active || now_us < diagnostics.led_test_deadline_us) {
        return diagnostics.led_test_active;
    }
    leds.clear();
    leds.set_pixel(diagnostics.led_test_pixel, 32, 32, 32);
    leds.show();
    ++diagnostics.led_test_pixel;
    if (diagnostics.led_test_pixel >= rabbit::rp2350::kMaxLedPixels) diagnostics.led_test_pixel = 0;
    diagnostics.led_test_deadline_us = now_us + kLedChaseIntervalUs;
    return true;
}

}  // namespace

int main() {
    stdio_init_all();
    const bool esp_released = release_esp32();
    configure_uart();
    if (rabbit::rp2350::kLocalStopButtonPin >= 0) {
        gpio_init(static_cast<unsigned int>(rabbit::rp2350::kLocalStopButtonPin));
        gpio_set_dir(static_cast<unsigned int>(rabbit::rp2350::kLocalStopButtonPin), GPIO_IN);
        gpio_pull_up(static_cast<unsigned int>(rabbit::rp2350::kLocalStopButtonPin));
    }
    if (rabbit::rp2350::kAudioPin >= 0) {
        gpio_init(static_cast<unsigned int>(rabbit::rp2350::kAudioPin));
        gpio_set_dir(static_cast<unsigned int>(rabbit::rp2350::kAudioPin), GPIO_OUT);
    }

    rabbit::rp2350::Ws2812Dma leds{};
    leds.initialise();
    rabbit::protocol::WorkoutService workout{transport_pool()};
    rabbit::rp2350::UartWorkoutLink uart_link{};
    Diagnostics diagnostics{};
    bool local_stop_was_pressed = false;

    sleep_ms(500);
    std::puts("Rabbit RP2350 diagnostic firmware ready");
    std::printf("led_pin=%d audio_pin=%d local_stop_pin=%d uart0=115200 esp=%s\n",
                rabbit::rp2350::kLedDataPin, rabbit::rp2350::kAudioPin,
                rabbit::rp2350::kLocalStopButtonPin, esp_released ? "released" : "unavailable");
    print_help();

    while (true) {
        const auto now_us = static_cast<rabbit::core::Microseconds>(time_us_64());
        const bool local_stop_pressed = rabbit::rp2350::kLocalStopButtonPin >= 0 &&
            !gpio_get(static_cast<unsigned int>(rabbit::rp2350::kLocalStopButtonPin));
        if (local_stop_pressed) {
            static_cast<void>(workout.stop(0xFFFF'FFFFU));
        }
        if (local_stop_pressed && !local_stop_was_pressed) std::puts("local stop pressed");
        local_stop_was_pressed = local_stop_pressed;
        uart_link.poll(workout, now_us);
        workout.advance(now_us);
        poll_console(diagnostics, leds, workout, uart_link, local_stop_pressed, now_us);
        const bool diagnostic_led_active = update_diagnostics(diagnostics, leds, now_us);
        const auto snapshot = workout.snapshot(now_us);
        if (rabbit::rp2350::kAudioPin >= 0) {
            gpio_put(static_cast<unsigned int>(rabbit::rp2350::kAudioPin),
                     diagnostics.audio_test_active || snapshot.audio_on);
        }
        if (!diagnostic_led_active && snapshot.cursor_visible && leds.enabled()) {
            leds.clear();
            leds.set_pixel(snapshot.cursor_pixel, 255, 255, 255);
            leds.show();
        }
        if (!diagnostic_led_active && !snapshot.running && leds.enabled()) {
            leds.clear();
            leds.show();
        }
        sleep_ms(1);
    }
}
