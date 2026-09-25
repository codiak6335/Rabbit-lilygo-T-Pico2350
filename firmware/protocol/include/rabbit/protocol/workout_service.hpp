#pragma once

#include <cstdint>

#include "rabbit/core/workout_engine.hpp"
#include "rabbit/protocol/framing.hpp"

namespace rabbit::protocol {

constexpr std::uint16_t kPreparePayloadSize = 13;
constexpr std::uint8_t kPrepareFlagFarToNear = 0x01;
constexpr std::uint8_t kPrepareFlagAudio = 0x02;
constexpr std::uint8_t kPrepareFlagWaitForInterval = 0x04;

enum class ServiceError : std::uint8_t {
    None = 0,
    Session = 1,
    Payload = 2,
    Command = 3,
    Validation = 4,
    Unsupported = 5,
};

class WorkoutService {
public:
    explicit WorkoutService(core::PoolProfile pool);

    [[nodiscard]] bool handle(const Frame& request, core::Microseconds now_us, Frame& response);
    void advance(core::Microseconds now_us);
    [[nodiscard]] bool stop(std::uint32_t request_id);
    [[nodiscard]] core::EngineSnapshot snapshot(core::Microseconds now_us) const;
    [[nodiscard]] std::uint32_t active_session() const { return active_session_id_; }

private:
    void make_ack(const Frame& request, Frame& response) const;
    void make_reject(const Frame& request, ServiceError error, Frame& response) const;
    void make_status(const Frame& request, core::Microseconds now_us, Frame& response) const;
    [[nodiscard]] ServiceError prepare(const Frame& request);

    core::PoolProfile pool_{};
    core::WorkoutEngine engine_{};
    std::uint32_t active_session_id_{0};
    std::uint32_t last_request_id_{0};
    MessageType last_request_type_{MessageType::Hello};
};

}  // namespace rabbit::protocol
