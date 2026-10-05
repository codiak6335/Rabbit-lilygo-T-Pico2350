#include "rabbit/protocol/workout_service.hpp"
#include "rabbit/protocol/plan_codec.hpp"

#include <cstring>
#include <limits>

namespace rabbit::protocol {
namespace {

constexpr std::uint32_t kMillisecondsPerDay = 24U * 60U * 60U * 1000U;
static_assert(44U + 1U + core::kMaxNameBytes + 1U + core::kMaxLabelBytes <= kMaxPayloadBytes,
              "DeckScript status metadata must fit a UART frame");

std::uint16_t read_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
}

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
        if (active_session_id_ != request.session_id) transfer_.abort();
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
        error = request.payload_size == 10U ? begin_plan_transfer(request) : prepare(request);
        break;
    case MessageType::PrepareChunk:
        error = append_plan_chunk(request);
        break;
    case MessageType::PrepareCommit:
        error = commit_plan_transfer(request);
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
    case MessageType::Cancel:
        if (request.payload_size != 0U || !engine_.cancel(request.request_id)) error = ServiceError::Command;
        break;
    case MessageType::Heartbeat:
        if (request.payload_size != 0U) error = ServiceError::Payload;
        break;
    default:
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
    const auto& plan = engine_.plan();
    const bool deckscript = current.state != core::EngineState::Idle && plan.deckscript;
    response.payload[1] = static_cast<std::uint8_t>((current.running ? 0x01U : 0x00U) |
                                                     (current.continuous ? 0x02U : 0x00U) |
                                                     (deckscript ? 0x04U : 0x00U) |
                                                     (engine_.pool().unit == core::DistanceUnit::Metre ? 0x08U : 0x00U));
    write_u16(response.payload.data() + 2, current.entry_index);
    write_u16(response.payload.data() + 4, current.cursor_progress_permille);
    write_u32(response.payload.data() + 6, current.sequence);
    write_u32(response.payload.data() + 10, static_cast<std::uint32_t>(
        current.seconds_until_next_us <= 0 ? 0 : current.seconds_until_next_us / 1000));
    write_u32(response.payload.data() + 14, current.cycle);
    write_u16(response.payload.data() + 18, current.entry_count);
    write_u32(response.payload.data() + 20, current.distance_mm);
    write_u32(response.payload.data() + 24, current.target_ms);
    response.payload_size = 28;
    if (!deckscript) return;
    const auto* entry = current.entry_index < plan.entry_count ? &plan.entries[current.entry_index] : nullptr;
    response.payload[28] = entry == nullptr ? 0xffU : static_cast<std::uint8_t>(entry->kind);
    response.payload[29] = entry != nullptr && entry->kind == core::EntryKind::Swim ?
        static_cast<std::uint8_t>(entry->swim.strategy) : 0U;
    write_u32(response.payload.data() + 30, entry != nullptr && entry->kind == core::EntryKind::Swim ?
        static_cast<std::uint32_t>(entry->swim.interval_us / 1000) : 0U);
    write_u32(response.payload.data() + 34, entry != nullptr && entry->kind == core::EntryKind::Swim ?
        static_cast<std::uint32_t>(entry->swim.split_delta_us / 1000) : 0U);
    write_u16(response.payload.data() + 38, entry != nullptr && entry->kind == core::EntryKind::Swim ?
        entry->swim.surge_permille : 0U);
    std::uint16_t swim_index = 0;
    std::uint16_t swim_count = 0;
    for (std::uint16_t index = 0; index < plan.entry_count; ++index) {
        if (plan.entries[index].kind == core::EntryKind::Swim) {
            ++swim_count;
            if (index <= current.entry_index) ++swim_index;
        }
    }
    write_u16(response.payload.data() + 40, swim_index);
    write_u16(response.payload.data() + 42, swim_count);
    auto offset = std::size_t{44};
    response.payload[offset++] = plan.name.size;
    std::memcpy(response.payload.data() + offset, plan.name.value.data(), plan.name.size);
    offset += plan.name.size;
    const auto label_size = entry == nullptr ? std::uint8_t{0} : entry->label.size;
    response.payload[offset++] = label_size;
    if (entry != nullptr) std::memcpy(response.payload.data() + offset, entry->label.value.data(), label_size);
    offset += label_size;
    response.payload_size = static_cast<std::uint16_t>(offset);
}

ServiceError WorkoutService::begin_plan_transfer(const Frame& request) {
    if (engine_.state() == core::EngineState::PreStart || engine_.state() == core::EngineState::Swimming ||
        engine_.state() == core::EngineState::Resting) return ServiceError::Command;
    const auto id = read_u32(request.payload.data());
    const auto size = read_u16(request.payload.data() + 4);
    const auto checksum = read_u32(request.payload.data() + 6);
    if (id == 0U || size == 0U || size > kMaxPlanBytes) return ServiceError::Payload;
    transfer_.abort();
    return transfer_.begin(id, size, checksum) == TransferError::None ? ServiceError::None : ServiceError::Payload;
}

ServiceError WorkoutService::append_plan_chunk(const Frame& request) {
    if (request.payload_size < 7U) return ServiceError::Payload;
    const auto id = read_u32(request.payload.data());
    const auto offset = read_u16(request.payload.data() + 4);
    const auto result = transfer_.append(id, offset, request.payload.data() + 6,
                                         static_cast<std::uint16_t>(request.payload_size - 6U));
    return result == TransferError::None ? ServiceError::None : ServiceError::Payload;
}

ServiceError WorkoutService::commit_plan_transfer(const Frame& request) {
    if (request.payload_size != 4U) return ServiceError::Payload;
    const std::uint8_t* data = nullptr;
    std::uint16_t size = 0;
    if (transfer_.commit(read_u32(request.payload.data()), data, size) != TransferError::None) {
        return ServiceError::Payload;
    }
    static core::WorkoutPlan plan{};
    core::PoolProfile pool{};
    if (!decode_plan(data, size, plan, pool)) return ServiceError::Payload;
    return engine_.prepare(plan, pool) == core::ValidationError::None ? ServiceError::None : ServiceError::Validation;
}

ServiceError WorkoutService::prepare(const Frame& request) {
    if (request.payload_size != kPreparePayloadSize && request.payload_size != kPrepareSetPayloadSize) {
        return ServiceError::Payload;
    }
    const auto flags = request.payload[0];
    const auto distance_mm = read_u32(request.payload.data() + 1);
    const auto target_ms = read_u32(request.payload.data() + 5);
    const auto interval_ms = read_u32(request.payload.data() + 9);
    const std::uint16_t repetitions = request.payload_size == kPrepareSetPayloadSize ?
        read_u16(request.payload.data() + 13) : std::uint16_t{1};
    const auto final_target_ms = request.payload_size == kPrepareSetPayloadSize ?
        read_u32(request.payload.data() + 15) : target_ms;
    const std::uint16_t surge_permille = request.payload_size == kPrepareSetPayloadSize ?
        read_u16(request.payload.data() + 19) : std::uint16_t{0};
    if (distance_mm > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
        target_ms == 0U || target_ms > kMillisecondsPerDay || interval_ms > kMillisecondsPerDay ||
        repetitions == 0U || repetitions > core::kMaxPlanEntries || final_target_ms == 0U ||
        final_target_ms > target_ms || surge_permille > 450U) {
        return ServiceError::Payload;
    }

    // The RP2350 task stack is small; keep the bounded transfer plan in static RAM.
    static core::WorkoutPlan plan{};
    core::clear_workout_plan(plan);
    if (!plan.name.assign("UART", 4) || !plan.pool_name.assign(pool_.name.value.data(), pool_.name.size)) {
        return ServiceError::Validation;
    }
    plan.version = 2;
    plan.pool_revision = pool_.revision;
    plan.audio_enabled = (flags & kPrepareFlagAudio) != 0U;
    plan.continuous = (flags & kPrepareFlagContinuous) != 0U;
    plan.direction = (flags & kPrepareFlagFarToNear) != 0U ? core::Direction::FarToNear : core::Direction::NearToFar;
    plan.entry_count = repetitions;
    for (std::uint16_t index = 0; index < repetitions; ++index) {
        auto& swim = plan.entries[index];
        swim.kind = core::EntryKind::Swim;
        swim.swim.distance_mm = static_cast<std::int32_t>(distance_mm);
        const std::uint32_t adjusted_target = repetitions == 1U ? target_ms :
            target_ms - static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(target_ms - final_target_ms) * index) / (repetitions - 1U));
        swim.swim.target_us = static_cast<core::Microseconds>(adjusted_target) * 1000;
        swim.swim.interval_us = static_cast<core::Microseconds>(interval_ms == 0U ? target_ms : interval_ms) * 1000;
        swim.swim.wait_for_interval = (flags & kPrepareFlagWaitForInterval) != 0U;
        if (surge_permille != 0U) {
            swim.swim.strategy = core::PacingStrategy::Surge;
            swim.swim.surge_permille = surge_permille;
        }
    }
    const auto validation = engine_.prepare(plan, pool_);
    return validation == core::ValidationError::None ? ServiceError::None : ServiceError::Validation;
}

}  // namespace rabbit::protocol
