#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "hardware/structs/scb.h"
#include "pico/stdlib.h"

#include "rabbit/rp2350/board_config.hpp"
#include "rabbit/rp2350/audio_output.hpp"
#include "rabbit/rp2350/uart_workout_link.hpp"
#include "rabbit/rp2350/ws2812_dma.hpp"

extern "C" void rabbit_hardfault_record(const std::uint32_t* frame) {
    watchdog_hw->scratch[0] |= 0x80000000U;
    watchdog_hw->scratch[1] = scb_hw->cfsr;
    const auto address = reinterpret_cast<std::uintptr_t>(frame);
    const bool valid_frame = address >= 0x20000000U && address <= 0x20081fe0U &&
        (scb_hw->cfsr & 0x3838U) == 0;
    watchdog_hw->scratch[2] = valid_frame ? frame[6] : 0;
    watchdog_hw->scratch[3] = valid_frame ? frame[5] : 0;
    while (true) __asm volatile("wfi");
}
extern "C" __attribute__((naked)) void isr_hardfault() {
    __asm volatile("tst lr, #4\n"
                   "ite eq\n"
                   "mrseq r0, msp\n"
                   "mrsne r0, psp\n"
                   "b rabbit_hardfault_record\n");
}

namespace {
std::uint32_t recovered_stage = 0, recovered_cfsr = 0, recovered_pc = 0, recovered_lr = 0;
constexpr rabbit::core::Microseconds kAudioTestDurationUs = 150'000;
constexpr rabbit::core::Microseconds kLedChaseIntervalUs = 75'000;

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
            (local_stop_pressed ? "pressed" : "released"), "external",
        static_cast<unsigned long>(uart_stats.received_bytes), static_cast<unsigned long>(uart_stats.received_frames),
        static_cast<unsigned long>(uart_stats.transmitted_bytes), static_cast<unsigned long>(uart_stats.transmitted_frames));
    std::printf("recovery stage=%08lx cfsr=%08lx pc=%08lx lr=%08lx\n",
        static_cast<unsigned long>(recovered_stage), static_cast<unsigned long>(recovered_cfsr),
        static_cast<unsigned long>(recovered_pc), static_cast<unsigned long>(recovered_lr));
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
    const bool recovered_hang = watchdog_enable_caused_reboot();
    const auto stalled_stage = watchdog_hw->scratch[0];
    if (recovered_hang) {
        recovered_stage = stalled_stage;
        recovered_cfsr = watchdog_hw->scratch[1];
        recovered_pc = watchdog_hw->scratch[2];
        recovered_lr = watchdog_hw->scratch[3];
    }
    watchdog_hw->scratch[1] = 0; watchdog_hw->scratch[2] = 0; watchdog_hw->scratch[3] = 0;
    stdio_init_all();
    watchdog_enable(4000, true);
    watchdog_hw->scratch[0] = 1;
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

    static rabbit::rp2350::Ws2812Dma leds{};
    leds.initialise();
    static rabbit::protocol::WorkoutService workout{transport_pool()};
    static rabbit::rp2350::UartWorkoutLink uart_link{};
    static rabbit::rp2350::AudioOutput audio{};
    audio.initialise();
    static rabbit::protocol::AudioService audio_service{audio};
    Diagnostics diagnostics{};
    bool local_stop_was_pressed = false;

    sleep_ms(500);
    std::puts("Rabbit RP2350 diagnostic firmware ready");
    if (recovered_hang) std::printf("watchdog: recovered stalled stage %lu\n", static_cast<unsigned long>(stalled_stage));
    std::printf("led_pin=%d audio_pin=%d local_stop_pin=%d uart0=115200 esp=%s\n",
                rabbit::rp2350::kLedDataPin, rabbit::rp2350::kAudioPin,
                rabbit::rp2350::kLocalStopButtonPin, "external");
    print_help();

    while (true) {
        watchdog_update();
        watchdog_hw->scratch[0] = 2;
        const auto now_us = static_cast<rabbit::core::Microseconds>(time_us_64());
        const bool local_stop_pressed = rabbit::rp2350::kLocalStopButtonPin >= 0 &&
            !gpio_get(static_cast<unsigned int>(rabbit::rp2350::kLocalStopButtonPin));
        if (local_stop_pressed) {
            static_cast<void>(workout.stop(0xFFFF'FFFFU));
        }
        if (local_stop_pressed && !local_stop_was_pressed) std::puts("local stop pressed");
        local_stop_was_pressed = local_stop_pressed;
        uart_link.poll(workout, audio_service, now_us);
        watchdog_hw->scratch[0] = 3;
        workout.advance(now_us);
        poll_console(diagnostics, leds, workout, uart_link, local_stop_pressed, now_us);
        const bool diagnostic_led_active = update_diagnostics(diagnostics, leds, now_us);
        const auto snapshot = workout.snapshot(now_us);
        watchdog_hw->scratch[0] = 4;
        audio.update(now_us, diagnostics.audio_test_active || snapshot.audio_on, snapshot.running);
        watchdog_hw->scratch[0] = 5;
        if (rabbit::rp2350::kAudioPin >= 0) {
            gpio_put(static_cast<unsigned int>(rabbit::rp2350::kAudioPin),
                     audio.buzzer_on());
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
