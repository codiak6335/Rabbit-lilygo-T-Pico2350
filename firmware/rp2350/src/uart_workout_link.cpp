#include "rabbit/rp2350/uart_workout_link.hpp"

#include "hardware/uart.h"

namespace rabbit::rp2350 {
namespace {
constexpr std::size_t kMaxRxBytesPerPoll = 64;
constexpr std::size_t kMaxTxBytesPerPoll = 64;
}

void UartWorkoutLink::poll(protocol::WorkoutService& service, const core::Microseconds now_us) {
    std::size_t received = 0;
    while (uart_is_readable(uart0) && received < kMaxRxBytesPerPoll) {
        protocol::Frame request{};
        protocol::Frame response{};
        protocol::DecodeError error{};
        const auto byte = static_cast<std::uint8_t>(uart_getc(uart0));
        ++received;
        ++stats_.received_bytes;
        if (decoder_.push(byte, request, error)) {
            ++stats_.received_frames;
            static_cast<void>(service.handle(request, now_us, response));
            queue(response);
        }
    }
    flush();
}

void UartWorkoutLink::queue(const protocol::Frame& frame) {
    protocol::EncodedFrame encoded{};
    if (!protocol::encode(frame, encoded)) return;
    pending_ = encoded;
    pending_offset_ = 0;
    pending_valid_ = true;
}

void UartWorkoutLink::flush() {
    if (!pending_valid_) return;
    std::size_t sent = 0;
    while (uart_is_writable(uart0) && pending_offset_ < pending_.size && sent < kMaxTxBytesPerPoll) {
        uart_putc_raw(uart0, pending_.bytes[pending_offset_]);
        ++pending_offset_;
        ++sent;
        ++stats_.transmitted_bytes;
    }
    if (pending_offset_ == pending_.size) {
        pending_valid_ = false;
        ++stats_.transmitted_frames;
    }
}

}  // namespace rabbit::rp2350
