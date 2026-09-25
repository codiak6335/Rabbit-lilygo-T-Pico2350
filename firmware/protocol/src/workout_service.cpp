#include "rabbit/protocol/workout_service.hpp"

#include <limits>

namespace rabbit::protocol {
namespace {

constexpr std::uint32_t kMillisecondsPerDay = 24U * 60U * 60U * 1000U;

std::uint32_t read_u32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
        (static_cast<std::uint32_t>(bytes[1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[2]) << 8U) |
        static_cast<std::uint32_t>(bytes[3]);
}

void write_u16(std::uint8_t* bytes, const std::uint16_t value) {
    bytes[0] = static_cast<std::uint8_t>(value >> 8U);
    bytes[1] = static_cast<std::uint8_t>(value);
}

void write_u32(std::uint8_t* bytes, const std::uint32_t value) {
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
}

}  // namespace

WorkoutService::WorkoutService(core::PoolProfile pool) : pool_(pool) {}

bool WorkoutService::handle(const Frame& request, const core::Microseconds now_us, Frame& response) {
    if (request.type == MessageType::Hello) {
        if (request.session_id == 0U) {
            make_reject(request, ServiceError::Session, response);
            return true;
        }
        active_session_id_ = request.session_id;
        last_request_id_ = request.request_id;
        last_request_type_ = request.type;
        make_ack(request, response);
        return true;
    }

    if (active_session_id_ == 0U || request.session_id != active_session_id_) {
        make_reject(request, ServiceError::Session, response);
        return true;
    }

    if (request.type == MessageType::Status) {
        make_status(request, now_us, response);
        return true;
    }

    if (request.request_id == last_request_id_ && request.type == last_request_type_) {
        make_ack(request, response);
        return true;
    }

    ServiceError error = ServiceError::None;
    switch (request.type) {
    case MessageType::PrepareBegin:
        error = prepare(request);
        break;
    case MessageType::Start:
        if (request.payload_size != 0U || !engine_.start(now_us, request.request_id)) {
            error = ServiceError::Command;
        } else {
            engine_.advance(now_us);
        }
        break;
    case MessageType::Stop:
        if (request.payload_size != 0U || !engine_.stop(request.request_id)) error = ServiceError::Command;
        break;
    case MessageType::Heartbeat:
        if (request.payload_size != 0U) error = ServiceError::Payload;
        break;
    case MessageType::PrepareChunk:
    case MessageType::PrepareCommit:
    case MessageType::Ack:
    case MessageType::Reject:
        error = ServiceError::Unsupported;
        break;
    case MessageType::Hello:
    case MessageType::Status:
        break;
    }

    if (error != ServiceError::None) {
        make_reject(request, error, response);
        return true;
    }

    last_request_id_ = request.request_id;
    last_request_type_ = request.type;
    make_ack(request, response);
    return true;
}

void WorkoutService::advance(const core::Microseconds now_us) {
    engine_.advance(now_us);
}

bool WorkoutService::stop(const std::uint32_t request_id) {
    return engine_.stop(request_id);
}

core::EngineSnapshot WorkoutService::snapshot(const core::Microseconds now_us) const {
    return engine_.snapshot(now_us);
}

void WorkoutService::make_ack(const Frame& request, Frame& response) const {
    response = {};
    response.type = MessageType::Ack;
    response.session_id = active_session_id_;
    response.request_id = request.request_id;
    response.payload[0] = static_cast<std::uint8_t>(request.type);
    response.payload[1] = static_cast<std::uint8_t>(engine_.state());
    write_u32(response.payload.data() + 2, engine_.snapshot(0).sequence);
    response.payload_size = 6;
}

void WorkoutService::make_reject(const Frame& request, const ServiceError error, Frame& response) const {
    response = {};
    response.type = MessageType::Reject;
    response.session_id = active_session_id_ == 0U ? request.session_id : active_session_id_;
    response.request_id = request.request_id;
    response.payload[0] = static_cast<std::uint8_t>(request.type);
    response.payload[1] = static_cast<std::uint8_t>(error);
    response.payload_size = 2;
}

void WorkoutService::make_status(const Frame& request, const core::Microseconds now_us, Frame& response) const {
    const auto current = engine_.snapshot(now_us);
    response = {};
    response.type = MessageType::Status;
    response.session_id = active_session_id_;
    response.request_id = request.request_id;
    response.payload[0] = static_cast<std::uint8_t>(current.state);
    response.payload[1] = current.running ? 0x01U : 0x00U;
    write_u16(response.payload.data() + 2, current.entry_index);
    write_u16(response.payload.data() + 4, current.cursor_progress_permille);
    write_u32(response.payload.data() + 6, current.sequence);
    response.payload_size = 10;
}

ServiceError WorkoutService::prepare(const Frame& request) {
    if (request.payload_size != kPreparePayloadSize) return ServiceError::Payload;
    const auto flags = request.payload[0];
    const auto distance_mm = read_u32(request.payload.data() + 1);
    const auto target_ms = read_u32(request.payload.data() + 5);
    const auto interval_ms = read_u32(request.payload.data() + 9);
    if (distance_mm > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        target_ms == 0U || target_ms > kMillisecondsPerDay || interval_ms > kMillisecondsPerDay) {
        return ServiceError::Payload;
    }

    core::WorkoutPlan plan{};
    if (!plan.name.assign("UART", 4) || !plan.pool_name.assign(pool_.name.value.data(), pool_.name.size)) {
        return ServiceError::Validation;
    }
    plan.version = 2;
    plan.pool_revision = pool_.revision;
    plan.audio_enabled = (flags & kPrepareFlagAudio) != 0U;
    plan.direction = (flags & kPrepareFlagFarToNear) != 0U ? core::Direction::FarToNear : core::Direction::NearToFar;
    plan.entry_count = 1;
    auto& swim = plan.entries[0];
    swim.kind = core::EntryKind::Swim;
    swim.swim.distance_mm = static_cast<std::int32_t>(distance_mm);
    swim.swim.target_us = static_cast<core::Microseconds>(target_ms) * 1000;
    swim.swim.interval_us = static_cast<core::Microseconds>(interval_ms == 0U ? target_ms : interval_ms) * 1000;
    swim.swim.wait_for_interval = (flags & kPrepareFlagWaitForInterval) != 0U;
    const auto validation = engine_.prepare(plan, pool_);
    return validation == core::ValidationError::None ? ServiceError::None : ServiceError::Validation;
}

}  // namespace rabbit::protocol
