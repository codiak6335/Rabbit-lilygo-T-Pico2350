#pragma once

#include <cstdint>

namespace rabbit::esp32 {

bool initialise_lcd();
void show_lcd_status(const char* status, const char* ip_address);

struct LcdWorkoutStatus {
    std::uint8_t state{0};
    std::uint16_t entry_index{0};
    std::uint16_t entry_count{0};
    std::uint32_t distance_yards{0};
    std::uint32_t remaining_ms{0};
    std::uint16_t progress_permille{0};
    std::uint32_t cycle{0};
    bool continuous{false};
    bool deckscript{false};
    bool metres{false};
    std::uint8_t entry_kind{0xffU};
    std::uint16_t swim_index{0};
    std::uint16_t swim_count{0};
};

void show_lcd_workout(const LcdWorkoutStatus& status);

}  // namespace rabbit::esp32
