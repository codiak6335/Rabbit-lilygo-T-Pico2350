#include "lcd_status.hpp"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace rabbit::esp32 {
namespace {
// Waveshare ESP32-C6-LCD-1.9: ST7789V2, 320 x 170 in landscape.
constexpr int kWidth = 320;
constexpr int kHeight = 170;
constexpr int kStripeHeight = 10;
constexpr int kBacklightPin = 15;
constexpr std::uint16_t kBackground = 0x080F;
constexpr std::uint16_t kWhite = 0xFFFF;
constexpr std::uint16_t kCyan = 0x07FF;

constexpr std::uint8_t kLetters[26][5] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x51, 0x49, 0x45, 0x43},
};
constexpr std::uint8_t kDigits[10][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
};
constexpr std::uint8_t kDot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
constexpr std::uint8_t kColon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
constexpr std::uint8_t kDash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
constexpr std::uint8_t kBlank[5] = {};

esp_lcd_panel_handle_t panel = nullptr;
SemaphoreHandle_t transfer_done = nullptr;
SemaphoreHandle_t display_mutex = nullptr;
std::uint8_t* stripe = nullptr;
bool ready = false;
char current_ip[16] = "--";

bool check(const esp_err_t result, const char* step) {
    if (result == ESP_OK) return true;
    std::printf("Rabbit LCD %s failed: %s\n", step, esp_err_to_name(result));
    return false;
}

bool on_transfer_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    BaseType_t higher_priority_woken = pdFALSE;
    xSemaphoreGiveFromISR(transfer_done, &higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

const std::uint8_t* glyph(const char character) {
    if (character >= 'A' && character <= 'Z') return kLetters[character - 'A'];
    if (character >= '0' && character <= '9') return kDigits[character - '0'];
    if (character == '.') return kDot;
    if (character == ':') return kColon;
    if (character == '-') return kDash;
    return kBlank;
}

void pixel(const int x, const int y, const int stripe_y, const std::uint16_t color) {
    if (x < 0 || x >= kWidth || y < stripe_y || y >= stripe_y + kStripeHeight || y >= kHeight) return;
    const auto offset = static_cast<std::size_t>((y - stripe_y) * kWidth + x) * 2U;
    stripe[offset] = static_cast<std::uint8_t>(color >> 8U);
    stripe[offset + 1U] = static_cast<std::uint8_t>(color);
}

void draw_text(const char* value, const int y, const int scale, const std::uint16_t color, const int stripe_y) {
    if (value == nullptr) return;
    const auto length = std::strlen(value);
    const int x_start = (kWidth - static_cast<int>(length) * 6 * scale) / 2;
    for (std::size_t index = 0; index < length; ++index) {
        const auto* columns = glyph(value[index]);
        for (int column = 0; column < 5; ++column) {
            for (int row = 0; row < 7; ++row) {
                if ((columns[column] & (1U << row)) == 0U) continue;
                for (int dx = 0; dx < scale; ++dx) {
                    for (int dy = 0; dy < scale; ++dy) {
                        pixel(x_start + (static_cast<int>(index) * 6 + column) * scale + dx,
                              y + row * scale + dy, stripe_y, color);
                    }
                }
            }
        }
    }
}

void clear_stripe() {
    for (int index = 0; index < kWidth * kStripeHeight; ++index) {
        stripe[index * 2] = static_cast<std::uint8_t>(kBackground >> 8U);
        stripe[index * 2 + 1] = static_cast<std::uint8_t>(kBackground);
    }
}

bool flush_stripe(const int y) {
    if (!check(esp_lcd_panel_draw_bitmap(panel, 0, y, kWidth, y + kStripeHeight, stripe), "draw") ||
        xSemaphoreTake(transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        std::puts("Rabbit LCD transfer timed out");
        return false;
    }
    return true;
}
}  // namespace

bool initialise_lcd() {
    gpio_config_t backlight{};
    backlight.pin_bit_mask = 1ULL << kBacklightPin;
    backlight.mode = GPIO_MODE_OUTPUT;
    // Waveshare's GPIO15 backlight is active-low: high is off, low is on.
    if (!check(gpio_config(&backlight), "backlight pin") ||
        !check(gpio_set_level(static_cast<gpio_num_t>(kBacklightPin), 1), "backlight off")) return false;

    spi_bus_config_t bus{};
    bus.mosi_io_num = 4;
    bus.miso_io_num = -1;
    bus.sclk_io_num = 5;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kWidth * kStripeHeight * 2;
    if (!check(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), "SPI bus")) return false;

    transfer_done = xSemaphoreCreateBinary();
    display_mutex = xSemaphoreCreateMutex();
    stripe = static_cast<std::uint8_t*>(heap_caps_malloc(kWidth * kStripeHeight * 2, MALLOC_CAP_DMA));
    if (transfer_done == nullptr || display_mutex == nullptr || stripe == nullptr) {
        std::puts("Rabbit LCD buffer allocation failed");
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = 7;
    io_config.dc_gpio_num = 6;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 20 * 1000 * 1000;
    io_config.trans_queue_depth = 2;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.on_color_trans_done = on_transfer_done;
    esp_lcd_panel_io_handle_t io = nullptr;
    if (!check(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST),
                                        &io_config, &io), "SPI panel I/O")) return false;

    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = 14;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
    if (!check(esp_lcd_new_panel_st7789(io, &panel_config, &panel), "ST7789 panel") ||
        !check(esp_lcd_panel_reset(panel), "reset") ||
        !check(esp_lcd_panel_init(panel), "init")) return false;

    // Match Waveshare's 320x170 landscape address window and scan direction.
    constexpr std::uint8_t direction = 0x70;
    if (!check(esp_lcd_panel_io_tx_param(io, 0x36, &direction, 1), "orientation") ||
        !check(esp_lcd_panel_set_gap(panel, 0, 35), "display offset") ||
        !check(esp_lcd_panel_invert_color(panel, true), "color inversion") ||
        !check(esp_lcd_panel_disp_on_off(panel, true), "display on")) return false;

    ready = true;
    std::puts("Rabbit LCD ready");
    return true;
}

void show_lcd_status(const char* status, const char* ip_address) {
    if (!ready) return;
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    std::snprintf(current_ip, sizeof(current_ip), "%s", ip_address == nullptr ? "--" : ip_address);
    char ip_line[32]{};
    std::snprintf(ip_line, sizeof(ip_line), "IP: %s", current_ip);
    for (int y = 0; y < kHeight; y += kStripeHeight) {
        clear_stripe();
        draw_text("RABBIT", 17, 4, kCyan, y);
        draw_text(status, 76, 3, kWhite, y);
        draw_text(ip_line, 128, 2, kCyan, y);
        if (!flush_stripe(y)) break;
    }
    check(gpio_set_level(static_cast<gpio_num_t>(kBacklightPin), 0), "backlight on");
    xSemaphoreGive(display_mutex);
}

void show_lcd_workout(const LcdWorkoutStatus& status) {
    if (!ready) return;
    static constexpr const char* states[] = {
        "IDLE", "READY", "STARTING", "SWIMMING", "RESTING", "STOPPED", "COMPLETE", "FAULT", "LINK DOWN"
    };
    const char* phase = status.state < sizeof(states) / sizeof(states[0]) ? states[status.state] : "UNKNOWN";
    char distance_line[32]{};
    char rep_line[40]{};
    char time_line[32]{};
    char progress_line[32]{};
    char ip_line[32]{};
    if (status.entry_count != 0) {
        if (status.distance_yards != 0U) {
            std::snprintf(distance_line, sizeof(distance_line), "%" PRIu32 " %s",
                          status.distance_yards, status.metres ? "M" : "YD");
        }
        if (status.deckscript && status.entry_kind != 0U) {
            const auto step = status.entry_index < status.entry_count ? status.entry_index + 1U : status.entry_count;
            std::snprintf(rep_line, sizeof(rep_line), "STEP %u OF %u",
                          static_cast<unsigned int>(step), static_cast<unsigned int>(status.entry_count));
        } else if (status.deckscript && status.continuous) {
            std::snprintf(rep_line, sizeof(rep_line), "SWIM %u LOOP %" PRIu32,
                          static_cast<unsigned int>(status.swim_index), status.cycle);
        } else if (status.deckscript) {
            std::snprintf(rep_line, sizeof(rep_line), "REP %u OF %u",
                          static_cast<unsigned int>(status.swim_index), static_cast<unsigned int>(status.swim_count));
        } else if (status.continuous) {
            std::snprintf(rep_line, sizeof(rep_line), "REP %" PRIu32 " CONTINUOUS", status.cycle);
        } else {
            const auto rep = status.entry_index < status.entry_count ? status.entry_index + 1U : status.entry_count;
            std::snprintf(rep_line, sizeof(rep_line), "REP %u OF %u",
                          static_cast<unsigned int>(rep), static_cast<unsigned int>(status.entry_count));
        }
        const auto seconds = (status.remaining_ms + 999U) / 1000U;
        std::snprintf(time_line, sizeof(time_line), "TIME %" PRIu32 ":%02" PRIu32,
                      seconds / 60U, seconds % 60U);
        if (status.state == 3U) {
            std::snprintf(progress_line, sizeof(progress_line), "TRACK %u PCT",
                          static_cast<unsigned int>(status.progress_permille / 10U));
        }
    }
    xSemaphoreTake(display_mutex, portMAX_DELAY);
    std::snprintf(ip_line, sizeof(ip_line), "IP:%s", current_ip);
    for (int y = 0; y < kHeight; y += kStripeHeight) {
        clear_stripe();
        draw_text("RABBIT", 6, 2, kCyan, y);
        draw_text(phase, 27, 3, kWhite, y);
        draw_text(distance_line, 57, 2, kCyan, y);
        draw_text(rep_line, 84, 2, kWhite, y);
        draw_text(time_line, 112, 2, kWhite, y);
        draw_text(progress_line, 138, 1, kCyan, y);
        draw_text(ip_line, 153, 1, kCyan, y);
        if (!flush_stripe(y)) break;
    }
    check(gpio_set_level(static_cast<gpio_num_t>(kBacklightPin), 0), "backlight on");
    xSemaphoreGive(display_mutex);
}

}  // namespace rabbit::esp32
