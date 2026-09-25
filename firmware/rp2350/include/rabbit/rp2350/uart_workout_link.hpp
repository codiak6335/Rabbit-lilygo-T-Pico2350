#pragma once

#include <cstddef>

#include "rabbit/protocol/workout_service.hpp"

namespace rabbit::rp2350 {

struct UartLinkStats {
    std::uint32_t received_bytes{0};
    std::uint32_t received_frames{0};
    std::uint32_t transmitted_bytes{0};
    std::uint32_t transmitted_frames{0};
};

class UartWorkoutLink {
public:
    void poll(protocol::WorkoutService& service, core::Microseconds now_us);
    [[nodiscard]] const UartLinkStats& stats() const { return stats_; }

private:
    void queue(const protocol::Frame& frame);
    void flush();

    protocol::FrameDecoder decoder_{};
    protocol::EncodedFrame pending_{};
    std::size_t pending_offset_{0};
    bool pending_valid_{false};
    UartLinkStats stats_{};
};

}  // namespace rabbit::rp2350
