#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace rabbit::core {

using Microseconds = std::int64_t;

constexpr std::size_t kMaxPlanEntries = 256;
constexpr std::size_t kMaxPoolSegments = 16;
constexpr std::size_t kMaxNameBytes = 64;
constexpr std::size_t kMaxLabelBytes = 80;
constexpr std::size_t kMaxDistanceMillimetres = 5'000'000;
constexpr Microseconds kMaxDurationUs = 86'400'000'000LL;

enum class Direction : std::uint8_t { NearToFar, FarToNear };
enum class DistanceUnit : std::uint8_t { Yard, Metre };
enum class EntryKind : std::uint8_t { Swim, Rest, Activity };
enum class PacingStrategy : std::uint8_t { Even, NegativeSplit, Surge };
enum class EngineState : std::uint8_t {
    Idle,
    Prepared,
    PreStart,
    Swimming,
    Resting,
    Stopped,
    Complete,
    Fault,
};

template <std::size_t Capacity>
struct FixedText {
    std::array<char, Capacity + 1> value{};
    std::uint8_t size{0};

    [[nodiscard]] bool assign(const char* source, std::size_t source_size) {
        if (source == nullptr || source_size > Capacity) {
            return false;
        }
        std::memcpy(value.data(), source, source_size);
        value[source_size] = '\0';
        size = static_cast<std::uint8_t>(source_size);
        return true;
    }

    [[nodiscard]] const char* c_str() const { return value.data(); }
    [[nodiscard]] bool empty() const { return size == 0; }
};

struct PoolSegment {
    std::uint16_t first_pixel{0};
    std::int32_t distance_mm{0};
};

struct PoolProfile {
    FixedText<kMaxLabelBytes> name{};
    DistanceUnit unit{DistanceUnit::Yard};
    std::int32_t length_mm{0};
    std::uint16_t pixel_count{0};
    bool last_pixel_inclusive{true};
    std::array<PoolSegment, kMaxPoolSegments> segments{};
    std::uint8_t segment_count{0};
    std::uint32_t revision{0};
};

struct SwimEntry {
    std::int32_t distance_mm{0};
    Microseconds target_us{0};
    Microseconds interval_us{0};
    bool wait_for_interval{false};
    PacingStrategy strategy{PacingStrategy::Even};
    Microseconds split_delta_us{0};
    std::uint16_t surge_permille{0};
};

struct RestEntry {
    Microseconds duration_us{0};
};

struct ActivityEntry {
    FixedText<kMaxLabelBytes> name{};
};

struct PlanEntry {
    EntryKind kind{EntryKind::Rest};
    FixedText<kMaxLabelBytes> label{};
    SwimEntry swim{};
    RestEntry rest{};
    ActivityEntry activity{};
};

struct WorkoutPlan {
    std::uint16_t version{2};
    FixedText<kMaxNameBytes> name{};
    FixedText<kMaxLabelBytes> pool_name{};
    std::uint32_t pool_revision{0};
    Direction direction{Direction::NearToFar};
    bool audio_enabled{true};
    bool continuous{false};
    bool deckscript{false};
    std::array<PlanEntry, kMaxPlanEntries> entries{};
    std::uint16_t entry_count{0};
};

inline void clear_workout_plan(WorkoutPlan& plan) {
    static_assert(std::is_trivially_copyable_v<WorkoutPlan>);
    std::memset(static_cast<void*>(&plan), 0, sizeof(plan));
    plan.version = 2;
}

struct EngineSnapshot {
    EngineState state{EngineState::Idle};
    std::uint16_t entry_index{0};
    std::uint16_t entry_count{0};
    std::uint32_t distance_mm{0};
    std::uint32_t target_ms{0};
    std::uint32_t cycle{0};
    bool running{false};
    bool continuous{false};
    bool audio_on{false};
    bool cursor_visible{false};
    std::uint16_t cursor_pixel{0};
    std::uint16_t cursor_progress_permille{0};
    Microseconds seconds_until_next_us{0};
    std::uint32_t sequence{0};
};

enum class ValidationError : std::uint8_t {
    None,
    UnsupportedVersion,
    MissingName,
    MissingPool,
    TooManyEntries,
    EmptyPlan,
    InvalidPool,
    InvalidEntry,
    InvalidDuration,
    InvalidDistance,
    InvalidInterval,
    InvalidStrategy,
    InvalidGeometry,
};

}  // namespace rabbit::core
