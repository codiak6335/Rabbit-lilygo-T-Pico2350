#pragma once

#include <array>
#include <cstdint>

#include "rabbit/rp2350/board_config.hpp"

namespace rabbit::rp2350 {

class Ws2812Dma {
public:
    bool initialise();
    void clear();
    void set_pixel(std::uint16_t index, std::uint8_t red, std::uint8_t green, std::uint8_t blue);
    void show();
    [[nodiscard]] bool enabled() const { return enabled_; }

private:
    std::array<std::uint32_t, kMaxLedPixels> pixels_{};
    int dma_channel_{-1};
    unsigned int program_offset_{0};
    unsigned int state_machine_{0};
    bool enabled_{false};
};

}  // namespace rabbit::rp2350
