#include "rabbit/rp2350/ws2812_dma.hpp"

#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "ws2812.pio.h"

namespace rabbit::rp2350 {

bool Ws2812Dma::initialise() {
    if (kLedDataPin < 0) return false;
    PIO pio = pio0;
    const int selected_sm = pio_claim_unused_sm(pio, false);
    const int selected_dma = dma_claim_unused_channel(false);
    if (selected_sm < 0 || selected_dma < 0) return false;
    state_machine_ = static_cast<unsigned int>(selected_sm);
    dma_channel_ = selected_dma;
    program_offset_ = pio_add_program(pio, &rabbit_ws2812_program);
    const auto config = rabbit_ws2812_program_get_default_config(program_offset_);
    pio_sm_config mutable_config = config;
    sm_config_set_sideset_pins(&mutable_config, static_cast<unsigned int>(kLedDataPin));
    sm_config_set_out_shift(&mutable_config, false, true, 24);
    sm_config_set_fifo_join(&mutable_config, PIO_FIFO_JOIN_TX);
    const float divider = static_cast<float>(clock_get_hz(clk_sys)) / 12'800'000.0F;
    sm_config_set_clkdiv(&mutable_config, divider);
    pio_gpio_init(pio, static_cast<unsigned int>(kLedDataPin));
    pio_sm_set_consecutive_pindirs(pio, state_machine_, static_cast<unsigned int>(kLedDataPin), 1, true);
    pio_sm_init(pio, state_machine_, program_offset_, &mutable_config);
    pio_sm_set_enabled(pio, state_machine_, true);
    enabled_ = true;
    clear();
    show();
    return true;
}

void Ws2812Dma::clear() {
    pixels_.fill(0);
}

void Ws2812Dma::set_pixel(
    const std::uint16_t index,
    const std::uint8_t red,
    const std::uint8_t green,
    const std::uint8_t blue) {
    if (index >= pixels_.size()) return;
    pixels_[index] = (static_cast<std::uint32_t>(green) << 24U) |
        (static_cast<std::uint32_t>(red) << 16U) | (static_cast<std::uint32_t>(blue) << 8U);
}

void Ws2812Dma::show() {
    if (!enabled_) return;
    PIO pio = pio0;
    dma_channel_wait_for_finish_blocking(dma_channel_);
    auto config = dma_channel_get_default_config(dma_channel_);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_dreq(&config, pio_get_dreq(pio, state_machine_, true));
    dma_channel_configure(
        dma_channel_, &config, &pio->txf[state_machine_], pixels_.data(), pixels_.size(), true);
}

}  // namespace rabbit::rp2350
