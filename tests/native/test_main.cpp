#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rabbit/core/workout_engine.hpp"
#include "rabbit/protocol/framing.hpp"
#include "rabbit/protocol/plan_codec.hpp"
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

    test_plan.entries[0].swim.distance_mm = 22'860;
    test_plan.entry_count = 1;
    test_plan.audio_enabled = false;
    test_plan.continuous = true;
    check(engine.prepare(test_plan, pool()) == rabbit::core::ValidationError::None, "prepare continuous set");
    check(engine.start(0, 10), "start continuous set");
    engine.advance(0);
    engine.advance(20'000'000);
    engine.advance(30'000'000);
    check(engine.snapshot(30'000'000).cycle == 2, "continuous set loops");
    check(engine.stop(11), "stop continuous set");
    check(engine.start(21'000'000, 12), "continue continuous set");
    check(engine.snapshot(21'000'000).cycle == 3, "continue wraps continuous set");
    check(engine.stop(13), "stop before cancel");
    check(engine.cancel(14) && engine.state() == rabbit::core::EngineState::Idle, "cancel clears set");
    check(engine.prepare(test_plan, pool()) == rabbit::core::ValidationError::None, "prepare after cancel");
    check(engine.start(31'000'000, 14), "new plan accepts a reused request id");
    check(engine.stop(15), "stop before cadence test");

    test_plan.entry_count = 2;
    test_plan.continuous = false;
    test_plan.audio_enabled = true;
    test_plan.entries[1] = test_plan.entries[0];
    check(engine.prepare(test_plan, pool()) == rabbit::core::ValidationError::None, "prepare two-rep cadence test");
    check(engine.start(0, 16), "start two-rep cadence test");
    engine.advance(2'150'000);
    engine.advance(22'150'000);
    check(engine.state() == rabbit::core::EngineState::Resting, "rest until next send-off lead");
    check(engine.snapshot(22'150'000).seconds_until_next_us == 10'000'000,
          "countdown targets the next swim start");
    engine.advance(29'999'000);
    check(engine.state() == rabbit::core::EngineState::Resting, "does not beep early");
    engine.advance(30'000'000);
    check(engine.state() == rabbit::core::EngineState::PreStart, "next beeps start before send-off");
    engine.advance(32'150'000);
    check(engine.state() == rabbit::core::EngineState::Swimming, "next swim starts on 30-second send-off");

    check(engine.stop(17), "stop before pacing strategy tests");
    test_plan = plan();
    test_plan.entry_count = 1;
    test_plan.audio_enabled = false;
    test_plan.entries[0].swim.distance_mm = 91'440;
    test_plan.entries[0].swim.target_us = 80'000'000;
    test_plan.entries[0].swim.interval_us = 80'000'000;
    test_plan.entries[0].swim.wait_for_interval = false;
    test_plan.entries[0].swim.strategy = rabbit::core::PacingStrategy::Surge;
    test_plan.entries[0].swim.surge_permille = 200;
    check(engine.prepare(test_plan, pool()) == rabbit::core::ValidationError::None, "prepare surge pacing");
    check(engine.start(0, 18), "start surge pacing");
    engine.advance(0);
    check(engine.snapshot(16'000'000).cursor_progress_permille == 250,
          "surge reaches the first length early");
    check(engine.snapshot(40'000'000).cursor_progress_permille == 500,
          "surge preserves the total target");
    rabbit::core::WorkoutEngine odd_surge{};
    auto odd_plan = test_plan;
    odd_plan.entries[0].swim.distance_mm = 68'580;
    odd_plan.entries[0].swim.target_us = 60'000'000;
    odd_plan.entries[0].swim.interval_us = 60'000'000;
    check(odd_surge.prepare(odd_plan, pool()) == rabbit::core::ValidationError::None,
          "prepare three-length surge");
    check(odd_surge.start(0, 1), "start three-length surge");
    odd_surge.advance(0);
    check(odd_surge.snapshot(30'000'000).cursor_progress_permille == 500 &&
          odd_surge.snapshot(60'000'000).cursor_progress_permille == 1000,
          "odd-length surge normalizes its weighted lengths");
    auto untimed_loop = plan();
    untimed_loop.entry_count = 1;
    untimed_loop.entries[0] = untimed_loop.entries[2];
    untimed_loop.continuous = true;
    rabbit::core::WorkoutEngine guard_engine{};
    check(guard_engine.prepare(untimed_loop, pool()) == rabbit::core::ValidationError::InvalidEntry,
          "reject a continuous activity-only plan that cannot make timed progress");
    check(engine.stop(19), "stop before negative-split pacing");
    test_plan.entries[0].swim.strategy = rabbit::core::PacingStrategy::NegativeSplit;
    test_plan.entries[0].swim.surge_permille = 0;
    test_plan.entries[0].swim.split_delta_us = 8'000'000;
    check(engine.prepare(test_plan, pool()) == rabbit::core::ValidationError::None, "prepare negative-split pacing");
    check(engine.start(0, 20), "start negative-split pacing");
    engine.advance(0);
    check(engine.snapshot(44'000'000).cursor_progress_permille == 500,
          "negative split reaches halfway after the slower first half");
    check(engine.stop(21), "stop before odd-length validation");
    test_plan.entries[0].swim.distance_mm = 68'580;
    check(engine.prepare(test_plan, pool()) == rabbit::core::ValidationError::InvalidStrategy,
          "negative split rejects an odd number of lengths");
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

void write_u16(std::uint8_t* bytes, const std::uint16_t value) {
    bytes[0] = static_cast<std::uint8_t>(value >> 8U);
    bytes[1] = static_cast<std::uint8_t>(value);
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
    check(response.payload_size == 28 && response.payload[0] == static_cast<std::uint8_t>(rabbit::core::EngineState::Stopped),
          "status payload state");
    check(response.payload[18] == 0 && response.payload[19] == 1 &&
          response.payload[20] == 0 && response.payload[21] == 0 &&
          response.payload[22] == 89 && response.payload[23] == 76 &&
          response.payload[24] == 0 && response.payload[25] == 0 &&
          response.payload[26] == 39 && response.payload[27] == 16,
          "status reports set count, distance, and target from RP2350");

    request = {};
    request.type = rabbit::protocol::MessageType::PrepareBegin;
    request.session_id = 81;
    request.request_id = 6;
    request.payload_size = rabbit::protocol::kPrepareSetPayloadSize;
    write_u32(request.payload.data() + 1, 22'860);
    write_u32(request.payload.data() + 5, 10'000);
    write_u32(request.payload.data() + 9, 12'000);
    write_u16(request.payload.data() + 13, 3);
    write_u32(request.payload.data() + 15, 8'000);
    check(service.handle(request, 20, response) && response.type == rabbit::protocol::MessageType::Ack,
          "repeatable set acknowledged");
    request = {};
    request.type = rabbit::protocol::MessageType::Status;
    request.session_id = 81;
    request.request_id = 7;
    check(service.handle(request, 20, response) && response.type == rabbit::protocol::MessageType::Status &&
          response.payload[18] == 0 && response.payload[19] == 3 &&
          response.payload[24] == 0 && response.payload[25] == 0 &&
          response.payload[26] == 39 && response.payload[27] == 16,
          "status retains the prepared set without ESP-side metadata");
    request = {};
    request.type = rabbit::protocol::MessageType::Cancel;
    request.session_id = 81;
    request.request_id = 8;
    check(service.handle(request, 21, response) && response.type == rabbit::protocol::MessageType::Ack,
          "prepared set canceled");

    request.session_id = 82;
    check(service.handle(request, 20, response) && response.type == rabbit::protocol::MessageType::Reject,
          "reject stale session");

    request = {};
    request.type = rabbit::protocol::MessageType::PrepareBegin;
    request.session_id = 81;
    request.request_id = 9;
    request.payload_size = rabbit::protocol::kPrepareSetPayloadSize;
    write_u32(request.payload.data() + 1, 91'440);
    write_u32(request.payload.data() + 5, 80'000);
    write_u32(request.payload.data() + 9, 90'000);
    write_u16(request.payload.data() + 13, 1);
    write_u32(request.payload.data() + 15, 80'000);
    write_u16(request.payload.data() + 19, 80);
    check(service.handle(request, 0, response) && response.type == rabbit::protocol::MessageType::Ack,
          "legacy quick set accepts surge percentage");
    request = {};
    request.type = rabbit::protocol::MessageType::Start;
    request.session_id = 81;
    request.request_id = 10;
    check(service.handle(request, 0, response) && response.type == rabbit::protocol::MessageType::Ack,
          "start legacy surge quick set");
    request = {};
    request.type = rabbit::protocol::MessageType::Status;
    request.session_id = 81;
    request.request_id = 11;
    check(service.handle(request, 16'000'000, response) && response.type == rabbit::protocol::MessageType::Status &&
          static_cast<unsigned int>((static_cast<std::uint16_t>(response.payload[4]) << 8U) |
                                    response.payload[5]) > 200U,
          "legacy surge changes progress instead of silently using even pace");

    static rabbit::core::WorkoutPlan deck{};
    deck = {};
    check(deck.name.assign("Surge Deck", 10) && deck.pool_name.assign("Test Pool", 9),
          "DeckScript name and pool");
    deck.audio_enabled = false;
    deck.deckscript = true;
    deck.entry_count = 3;
    deck.entries[0].kind = rabbit::core::EntryKind::Swim;
    check(deck.entries[0].label.assign("Fast first length", 17), "surge label");
    deck.entries[0].swim.distance_mm = 91'440;
    deck.entries[0].swim.target_us = 80'000'000;
    deck.entries[0].swim.interval_us = 90'000'000;
    deck.entries[0].swim.wait_for_interval = true;
    deck.entries[0].swim.strategy = rabbit::core::PacingStrategy::Surge;
    deck.entries[0].swim.surge_permille = 200;
    deck.entries[1].kind = rabbit::core::EntryKind::Rest;
    check(deck.entries[1].label.assign("Recovery", 8), "rest label");
    deck.entries[1].rest.duration_us = 5'000'000;
    deck.entries[2].kind = rabbit::core::EntryKind::Swim;
    deck.entries[2].swim.distance_mm = 45'720;
    deck.entries[2].swim.target_us = 40'000'000;
    deck.entries[2].swim.interval_us = 50'000'000;
    deck.entries[2].swim.strategy = rabbit::core::PacingStrategy::NegativeSplit;
    deck.entries[2].swim.split_delta_us = 4'000'000;
    static std::array<std::uint8_t, rabbit::protocol::kMaxPlanBytes> encoded{};
    std::uint16_t encoded_size = 0;
    check(rabbit::protocol::encode_plan(deck, pool(), encoded.data(),
                                        static_cast<std::uint16_t>(encoded.size()), encoded_size),
          "encode compiled DeckScript plan");
    static rabbit::core::WorkoutPlan decoded{};
    rabbit::core::PoolProfile decoded_pool{};
    check(rabbit::protocol::decode_plan(encoded.data(), encoded_size, decoded, decoded_pool) &&
          decoded.entry_count == 3 && decoded.entries[0].swim.surge_permille == 200 &&
          decoded.entries[2].swim.split_delta_us == 4'000'000 &&
          decoded_pool.length_mm == 22'860, "DeckScript wire round trip");
    for (std::uint16_t truncated = 0; truncated < encoded_size; ++truncated) {
        check(!rabbit::protocol::decode_plan(encoded.data(), truncated, decoded, decoded_pool),
              "reject every truncated DeckScript plan prefix");
    }

    rabbit::protocol::WorkoutService deck_service{pool()};
    request = {};
    request.type = rabbit::protocol::MessageType::Hello;
    request.session_id = 99;
    request.request_id = 1;
    check(deck_service.handle(request, 0, response) && response.type == rabbit::protocol::MessageType::Ack,
          "DeckScript session handshake");
    request = {};
    request.type = rabbit::protocol::MessageType::PrepareBegin;
    request.session_id = 99;
    request.request_id = 2;
    request.payload_size = 10;
    write_u32(request.payload.data(), 777);
    write_u16(request.payload.data() + 4, encoded_size);
    write_u32(request.payload.data() + 6, rabbit::protocol::crc32(encoded.data(), encoded_size));
    check(deck_service.handle(request, 0, response) && response.type == rabbit::protocol::MessageType::Ack,
          "begin checksummed DeckScript transfer");
    std::uint32_t next_request = 3;
    for (std::uint16_t offset = 0; offset < encoded_size;) {
        const auto count = static_cast<std::uint16_t>(std::min<std::size_t>(120U, encoded_size - offset));
        request = {};
        request.type = rabbit::protocol::MessageType::PrepareChunk;
        request.session_id = 99;
        request.request_id = next_request++;
        request.payload_size = static_cast<std::uint16_t>(count + 6U);
        write_u32(request.payload.data(), 777);
        write_u16(request.payload.data() + 4, offset);
        std::memcpy(request.payload.data() + 6, encoded.data() + offset, count);
        check(deck_service.handle(request, 0, response) && response.type == rabbit::protocol::MessageType::Ack,
              "append DeckScript transfer chunk");
        offset = static_cast<std::uint16_t>(offset + count);
    }
    request = {};
    request.type = rabbit::protocol::MessageType::PrepareCommit;
    request.session_id = 99;
    request.request_id = next_request++;
    request.payload_size = 4;
    write_u32(request.payload.data(), 777);
    check(deck_service.handle(request, 0, response) && response.type == rabbit::protocol::MessageType::Ack,
          "commit and validate DeckScript plan");
    request = {};
    request.type = rabbit::protocol::MessageType::Status;
    request.session_id = 99;
    request.request_id = next_request++;
    check(deck_service.handle(request, 0, response) && response.type == rabbit::protocol::MessageType::Status &&
          (response.payload[1] & 0x04U) != 0U && response.payload[28] == 0U && response.payload[29] == 2U &&
          response.payload[42] == 0U && response.payload[43] == 2U,
          "status exposes DeckScript step and surge strategy");
    check(response.payload_size == 73U && response.payload[44] == 10U &&
          std::memcmp(response.payload.data() + 45, "Surge Deck", 10) == 0 &&
          response.payload[55] == 17U &&
          std::memcmp(response.payload.data() + 56, "Fast first length", 17) == 0,
          "status retains DeckScript name and current step label after ESP reset");
    request = {};
    request.type = rabbit::protocol::MessageType::Start;
    request.session_id = 99;
    request.request_id = next_request++;
    check(deck_service.handle(request, 0, response) && response.type == rabbit::protocol::MessageType::Ack,
          "start transferred DeckScript plan");
    request = {};
    request.type = rabbit::protocol::MessageType::PrepareBegin;
    request.session_id = 99;
    request.request_id = next_request++;
    request.payload_size = 10;
    write_u32(request.payload.data(), 778);
    write_u16(request.payload.data() + 4, encoded_size);
    write_u32(request.payload.data() + 6, rabbit::protocol::crc32(encoded.data(), encoded_size));
    check(deck_service.handle(request, 1'000'000, response) && response.type == rabbit::protocol::MessageType::Reject,
          "reject a new plan while the current workout is running");
    request = {};
    request.type = rabbit::protocol::MessageType::Status;
    request.session_id = 99;
    request.request_id = next_request++;
    check(deck_service.handle(request, 16'000'000, response) && response.type == rabbit::protocol::MessageType::Status &&
          response.payload[4] == 0U && response.payload[5] == 250U,
          "transferred surge changes live cursor progress");
    deck_service.advance(80'000'000);
    deck_service.advance(90'000'000);
    request.request_id = next_request++;
    check(deck_service.handle(request, 90'000'000, response) &&
          response.payload[0] == static_cast<std::uint8_t>(rabbit::core::EngineState::Resting) &&
          response.payload[28] == static_cast<std::uint8_t>(rabbit::core::EntryKind::Rest) &&
          response.payload[24] == 0U && response.payload[25] == 0U &&
          response.payload[26] == 19U && response.payload[27] == 136U,
          "DeckScript rest step follows the surge swim");
    deck_service.advance(95'000'000);
    deck_service.advance(95'000'000);
    request.request_id = next_request++;
    check(deck_service.handle(request, 117'000'000, response) &&
          response.payload[28] == static_cast<std::uint8_t>(rabbit::core::EntryKind::Swim) &&
          response.payload[29] == static_cast<std::uint8_t>(rabbit::core::PacingStrategy::NegativeSplit) &&
          response.payload[4] == 1U && response.payload[5] == 244U,
          "DeckScript negative split reaches halfway at its slower first-half deadline");
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
