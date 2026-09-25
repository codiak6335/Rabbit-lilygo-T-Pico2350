#include <array>
#include <cstdio>
#include <cstdlib>

#include "rabbit/core/workout_engine.hpp"
#include "rabbit/protocol/framing.hpp"
#include "rabbit/protocol/workout_service.hpp"

namespace {

void check(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

rabbit::core::PoolProfile pool() {
    rabbit::core::PoolProfile result{};
    check(result.name.assign("Test Pool", 9), "pool name");
    result.unit = rabbit::core::DistanceUnit::Yard;
    result.length_mm = 22'860;
    result.pixel_count = 910;
    result.segment_count = 3;
    result.segments[0] = {174, 0};
    result.segments[1] = {517, 10'591};
    result.segments[2] = {883, 22'860};
    result.revision = 7;
    return result;
}

rabbit::core::WorkoutPlan plan() {
    rabbit::core::WorkoutPlan result{};
    check(result.name.assign("Pacing", 6), "plan name");
    check(result.pool_name.assign("Test Pool", 9), "plan pool");
    result.pool_revision = 7;
    result.audio_enabled = true;
    result.entry_count = 3;
    result.entries[0].kind = rabbit::core::EntryKind::Swim;
    result.entries[0].swim.distance_mm = 22'860;
    result.entries[0].swim.target_us = 20'000'000;
    result.entries[0].swim.interval_us = 30'000'000;
    result.entries[0].swim.wait_for_interval = true;
    result.entries[1].kind = rabbit::core::EntryKind::Rest;
    result.entries[1].rest.duration_us = 5'000'000;
    result.entries[2].kind = rabbit::core::EntryKind::Activity;
    check(result.entries[2].activity.name.assign("Drill", 5), "activity name");
    return result;
}

void test_geometry() {
    const auto test_pool = pool();
    check(rabbit::core::validate_pool(test_pool) == rabbit::core::ValidationError::None, "valid pool");
    check(rabbit::core::pixel_for_distance(test_pool, 0, rabbit::core::Direction::NearToFar) == 174, "start pixel");
    check(rabbit::core::pixel_for_distance(test_pool, 22'860, rabbit::core::Direction::NearToFar) == 883, "end pixel");
    check(rabbit::core::pixel_for_distance(test_pool, 0, rabbit::core::Direction::FarToNear) == 735, "reverse pixel");
    auto invalid = test_pool;
    invalid.segments[1].first_pixel = invalid.segments[0].first_pixel;
    check(rabbit::core::validate_pool(invalid) == rabbit::core::ValidationError::InvalidGeometry, "reject zero pixel span");
}

void test_engine() {
    rabbit::core::WorkoutEngine engine{};
    auto test_plan = plan();
    check(engine.prepare(test_plan, pool()) == rabbit::core::ValidationError::None, "prepare");
    check(engine.start(0, 1), "start");
    check(engine.snapshot(0).audio_on, "first beep begins without blocking");
    engine.advance(2'150'000);
    check(engine.state() == rabbit::core::EngineState::Swimming, "swim begins after scheduled beeps");
    auto middle = engine.snapshot(12'150'000);
    check(middle.cursor_visible && middle.cursor_progress_permille == 500, "cursor uses monotonic progress");
    engine.advance(22'150'000);
    check(engine.state() == rabbit::core::EngineState::Resting, "waits to interval deadline");
    engine.advance(32'150'000);
    check(engine.state() == rabbit::core::EngineState::Resting, "explicit rest begins after interval");
    check(engine.stop(2), "stop acknowledged immediately");
    check(engine.state() == rabbit::core::EngineState::Stopped, "stop never waits for a sleep");
    check(engine.start(33'000'000, 3), "continue advances to next entry");
    check(engine.state() == rabbit::core::EngineState::Complete, "activity is immediate and workout completes");

    test_plan.entries[0].swim.distance_mm = 1;
    check(engine.prepare(test_plan, pool()) == rabbit::core::ValidationError::InvalidDistance, "reject partial lengths");
}

void test_protocol() {
    rabbit::protocol::Frame source{};
    source.type = rabbit::protocol::MessageType::Start;
    source.session_id = 42;
    source.request_id = 19;
    source.payload[0] = 0;
    source.payload[1] = 7;
    source.payload_size = 2;
    rabbit::protocol::EncodedFrame encoded{};
    check(rabbit::protocol::encode(source, encoded), "encode frame");
    rabbit::protocol::FrameDecoder decoder{};
    rabbit::protocol::Frame decoded{};
    rabbit::protocol::DecodeError error{};
    bool complete = false;
    for (std::size_t index = 0; index < encoded.size; ++index) complete = decoder.push(encoded.bytes[index], decoded, error) || complete;
    check(complete && error == rabbit::protocol::DecodeError::None, "decode frame");
    check(decoded.request_id == 19 && decoded.payload_size == 2 && decoded.payload[1] == 7, "frame fields survive cobs");

    auto corrupted = encoded;
    corrupted.bytes[corrupted.size - 2] ^= 0x01U;
    bool corrupt_complete = false;
    for (std::size_t index = 0; index < corrupted.size; ++index) {
        corrupt_complete = decoder.push(corrupted.bytes[index], decoded, error) || corrupt_complete;
    }
    check(!corrupt_complete && error != rabbit::protocol::DecodeError::None, "reject corrupt frame");

    rabbit::protocol::PlanTransfer transfer{};
    constexpr std::array<std::uint8_t, 4> payload{{1, 2, 3, 4}};
    const auto checksum = rabbit::protocol::crc32(payload.data(), payload.size());
    check(transfer.begin(9, 4, checksum) == rabbit::protocol::TransferError::None, "begin transfer");
    check(transfer.append(9, 2, payload.data() + 2, 2) == rabbit::protocol::TransferError::Sequence, "reject out of order chunk");
    check(transfer.append(9, 0, payload.data(), 2) == rabbit::protocol::TransferError::None, "first chunk");
    check(transfer.append(9, 2, payload.data() + 2, 2) == rabbit::protocol::TransferError::None, "second chunk");
    const std::uint8_t* committed = nullptr;
    std::uint16_t committed_size = 0;
    check(transfer.commit(9, committed, committed_size) == rabbit::protocol::TransferError::None, "commit transfer");
    check(committed_size == payload.size() && committed[3] == 4, "transfer bytes");

    check(transfer.begin(10, 4, checksum + 1U) == rabbit::protocol::TransferError::None, "start bad checksum transfer");
    check(transfer.append(10, 0, payload.data(), 4) == rabbit::protocol::TransferError::None, "bad checksum payload");
    check(transfer.commit(10, committed, committed_size) == rabbit::protocol::TransferError::Crc, "reject bad transfer checksum");
}

void write_u32(std::uint8_t* bytes, const std::uint32_t value) {
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
}

void test_workout_service() {
    rabbit::protocol::WorkoutService service{pool()};
    rabbit::protocol::Frame request{};
    rabbit::protocol::Frame response{};
    request.type = rabbit::protocol::MessageType::Hello;
    request.session_id = 81;
    request.request_id = 1;
    check(service.handle(request, 0, response) && response.type == rabbit::protocol::MessageType::Ack, "hello acknowledged");

    request = {};
    request.type = rabbit::protocol::MessageType::PrepareBegin;
    request.session_id = 81;
    request.request_id = 2;
    request.payload_size = rabbit::protocol::kPreparePayloadSize;
    write_u32(request.payload.data() + 1, 22'860);
    write_u32(request.payload.data() + 5, 10'000);
    write_u32(request.payload.data() + 9, 10'000);
    check(service.handle(request, 0, response) && response.type == rabbit::protocol::MessageType::Ack, "prepare acknowledged");

    request = {};
    request.type = rabbit::protocol::MessageType::Start;
    request.session_id = 81;
    request.request_id = 3;
    check(service.handle(request, 10, response) && response.type == rabbit::protocol::MessageType::Ack, "start acknowledged");
    check(service.snapshot(10).state == rabbit::core::EngineState::Swimming, "service starts without audio delay");

    request.type = rabbit::protocol::MessageType::Stop;
    request.request_id = 4;
    check(service.handle(request, 20, response) && response.type == rabbit::protocol::MessageType::Ack, "stop acknowledged");
    check(service.snapshot(20).state == rabbit::core::EngineState::Stopped, "service stops immediately");

    request = {};
    request.type = rabbit::protocol::MessageType::Status;
    request.session_id = 81;
    request.request_id = 5;
    check(service.handle(request, 20, response) && response.type == rabbit::protocol::MessageType::Status,
          "status response");
    check(response.payload_size == 10 && response.payload[0] == static_cast<std::uint8_t>(rabbit::core::EngineState::Stopped),
          "status payload state");

    request.session_id = 82;
    check(service.handle(request, 20, response) && response.type == rabbit::protocol::MessageType::Reject,
          "reject stale session");
}

}  // namespace

int main() {
    test_geometry();
    test_engine();
    test_protocol();
    test_workout_service();
    std::puts("Rabbit native tests passed");
    return 0;
}
