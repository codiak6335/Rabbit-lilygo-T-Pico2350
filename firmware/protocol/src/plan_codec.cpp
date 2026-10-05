#include "rabbit/protocol/plan_codec.hpp"

#include <cstring>
#include <limits>

namespace rabbit::protocol {
namespace {

constexpr std::uint8_t kMagic[] = {'R', 'B', 'P', '2'};
constexpr std::uint8_t kFarFlag = 0x01U;
constexpr std::uint8_t kAudioFlag = 0x02U;
constexpr std::uint8_t kContinuousFlag = 0x04U;
constexpr std::uint8_t kDeckScriptFlag = 0x08U;

struct Writer {
    std::uint8_t* data;
    std::uint16_t capacity;
    std::uint16_t offset{0};

    bool bytes(const void* source, const std::uint16_t count) {
        if (source == nullptr || count > capacity - offset) return false;
        std::memcpy(data + offset, source, count);
        offset = static_cast<std::uint16_t>(offset + count);
        return true;
    }
    bool u8(const std::uint8_t value) { return bytes(&value, 1); }
    bool u16(const std::uint16_t value) {
        const std::uint8_t encoded[] = {static_cast<std::uint8_t>(value >> 8U), static_cast<std::uint8_t>(value)};
        return bytes(encoded, sizeof(encoded));
    }
    bool u32(const std::uint32_t value) {
        const std::uint8_t encoded[] = {static_cast<std::uint8_t>(value >> 24U), static_cast<std::uint8_t>(value >> 16U),
                                        static_cast<std::uint8_t>(value >> 8U), static_cast<std::uint8_t>(value)};
        return bytes(encoded, sizeof(encoded));
    }
    template <std::size_t Capacity>
    bool text(const core::FixedText<Capacity>& value) {
        return u8(value.size) && bytes(value.value.data(), value.size);
    }
};

struct Reader {
    const std::uint8_t* data;
    std::uint16_t size;
    std::uint16_t offset{0};

    bool bytes(void* target, const std::uint16_t count) {
        if (target == nullptr || count > size - offset) return false;
        std::memcpy(target, data + offset, count);
        offset = static_cast<std::uint16_t>(offset + count);
        return true;
    }
    bool u8(std::uint8_t& value) { return bytes(&value, 1); }
    bool u16(std::uint16_t& value) {
        std::uint8_t encoded[2]{};
        if (!bytes(encoded, sizeof(encoded))) return false;
        value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(encoded[0]) << 8U) | encoded[1]);
        return true;
    }
    bool u32(std::uint32_t& value) {
        std::uint8_t encoded[4]{};
        if (!bytes(encoded, sizeof(encoded))) return false;
        value = (static_cast<std::uint32_t>(encoded[0]) << 24U) | (static_cast<std::uint32_t>(encoded[1]) << 16U) |
                (static_cast<std::uint32_t>(encoded[2]) << 8U) | encoded[3];
        return true;
    }
    template <std::size_t Capacity>
    bool text(core::FixedText<Capacity>& value) {
        std::uint8_t count = 0;
        if (!u8(count) || count > Capacity || count > size - offset) return false;
        const bool assigned = value.assign(reinterpret_cast<const char*>(data + offset), count);
        offset = static_cast<std::uint16_t>(offset + count);
        return assigned;
    }
};

bool duration_ms(const core::Microseconds value, std::uint32_t& milliseconds) {
    if (value < 0 || value > static_cast<core::Microseconds>(std::numeric_limits<std::uint32_t>::max()) * 1000 ||
        value % 1000 != 0) return false;
    milliseconds = static_cast<std::uint32_t>(value / 1000);
    return true;
}

}  // namespace

bool encode_plan(const core::WorkoutPlan& plan, const core::PoolProfile& pool,
                 std::uint8_t* output, const std::uint16_t capacity, std::uint16_t& size) {
    size = 0;
    if (output == nullptr || capacity > kMaxPlanBytes || plan.entry_count == 0 ||
        plan.entry_count > core::kMaxPlanEntries || pool.length_mm <= 0 || plan.name.empty() ||
        plan.pool_name.empty()) return false;
    Writer writer{output, capacity};
    const std::uint8_t flags = static_cast<std::uint8_t>(
        (plan.direction == core::Direction::FarToNear ? kFarFlag : 0U) |
        (plan.audio_enabled ? kAudioFlag : 0U) |
        (plan.continuous ? kContinuousFlag : 0U) | kDeckScriptFlag);
    if (!writer.bytes(kMagic, sizeof(kMagic)) || !writer.u8(flags) ||
        !writer.u8(static_cast<std::uint8_t>(pool.unit)) ||
        !writer.u32(static_cast<std::uint32_t>(pool.length_mm)) ||
        !writer.text(plan.name) || !writer.text(plan.pool_name) || !writer.u16(plan.entry_count)) return false;
    for (std::uint16_t index = 0; index < plan.entry_count; ++index) {
        const auto& entry = plan.entries[index];
        if (!writer.u8(static_cast<std::uint8_t>(entry.kind)) || !writer.text(entry.label)) return false;
        if (entry.kind == core::EntryKind::Swim) {
            std::uint32_t target_ms = 0;
            std::uint32_t interval_ms = 0;
            std::uint32_t split_ms = 0;
            if (entry.swim.distance_mm <= 0 || !duration_ms(entry.swim.target_us, target_ms) ||
                !duration_ms(entry.swim.interval_us, interval_ms) ||
                !duration_ms(entry.swim.split_delta_us, split_ms) ||
                !writer.u32(static_cast<std::uint32_t>(entry.swim.distance_mm)) ||
                !writer.u32(target_ms) || !writer.u32(interval_ms) ||
                !writer.u8(entry.swim.wait_for_interval ? 1U : 0U) ||
                !writer.u8(static_cast<std::uint8_t>(entry.swim.strategy)) ||
                !writer.u32(split_ms) || !writer.u16(entry.swim.surge_permille)) return false;
        } else if (entry.kind == core::EntryKind::Rest) {
            std::uint32_t rest_ms = 0;
            if (!duration_ms(entry.rest.duration_us, rest_ms) || !writer.u32(rest_ms)) return false;
        } else if (entry.kind == core::EntryKind::Activity) {
            if (!writer.text(entry.activity.name)) return false;
        } else return false;
    }
    size = writer.offset;
    return true;
}

bool decode_plan(const std::uint8_t* data, const std::uint16_t size,
                 core::WorkoutPlan& plan, core::PoolProfile& pool) {
    if (data == nullptr || size == 0 || size > kMaxPlanBytes) return false;
    Reader reader{data, size};
    std::uint8_t magic[sizeof(kMagic)]{};
    std::uint8_t flags = 0;
    std::uint8_t unit = 0;
    std::uint32_t length_mm = 0;
    core::clear_workout_plan(plan);
    pool = {};
    if (!reader.bytes(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 ||
        !reader.u8(flags) || (flags & ~std::uint8_t{0x0fU}) != 0U || (flags & kDeckScriptFlag) == 0U ||
        !reader.u8(unit) || unit > static_cast<std::uint8_t>(core::DistanceUnit::Metre) ||
        !reader.u32(length_mm) || length_mm == 0U || length_mm > core::kMaxDistanceMillimetres ||
        !reader.text(plan.name) || !reader.text(plan.pool_name) ||
        !reader.u16(plan.entry_count) || plan.entry_count == 0U ||
        plan.entry_count > core::kMaxPlanEntries) return false;
    plan.direction = (flags & kFarFlag) != 0U ? core::Direction::FarToNear : core::Direction::NearToFar;
    plan.audio_enabled = (flags & kAudioFlag) != 0U;
    plan.continuous = (flags & kContinuousFlag) != 0U;
    plan.deckscript = true;
    plan.pool_revision = 1;
    pool.name = plan.pool_name;
    pool.unit = static_cast<core::DistanceUnit>(unit);
    pool.length_mm = static_cast<std::int32_t>(length_mm);
    pool.pixel_count = 1;
    pool.segment_count = 1;
    pool.segments[0] = {0, 0};
    pool.revision = 1;
    for (std::uint16_t index = 0; index < plan.entry_count; ++index) {
        auto& entry = plan.entries[index];
        std::uint8_t kind = 0;
        if (!reader.u8(kind) || kind > static_cast<std::uint8_t>(core::EntryKind::Activity) ||
            !reader.text(entry.label)) return false;
        entry.kind = static_cast<core::EntryKind>(kind);
        if (entry.kind == core::EntryKind::Swim) {
            std::uint32_t distance_mm = 0;
            std::uint32_t target_ms = 0;
            std::uint32_t interval_ms = 0;
            std::uint8_t wait = 0;
            std::uint8_t strategy = 0;
            std::uint32_t split_ms = 0;
            if (!reader.u32(distance_mm) || distance_mm > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
                !reader.u32(target_ms) || !reader.u32(interval_ms) || !reader.u8(wait) || wait > 1U ||
                !reader.u8(strategy) || strategy > static_cast<std::uint8_t>(core::PacingStrategy::Surge) ||
                !reader.u32(split_ms) || !reader.u16(entry.swim.surge_permille)) return false;
            entry.swim.distance_mm = static_cast<std::int32_t>(distance_mm);
            entry.swim.target_us = static_cast<core::Microseconds>(target_ms) * 1000;
            entry.swim.interval_us = static_cast<core::Microseconds>(interval_ms) * 1000;
            entry.swim.wait_for_interval = wait != 0U;
            entry.swim.strategy = static_cast<core::PacingStrategy>(strategy);
            entry.swim.split_delta_us = static_cast<core::Microseconds>(split_ms) * 1000;
        } else if (entry.kind == core::EntryKind::Rest) {
            std::uint32_t rest_ms = 0;
            if (!reader.u32(rest_ms)) return false;
            entry.rest.duration_us = static_cast<core::Microseconds>(rest_ms) * 1000;
        } else if (!reader.text(entry.activity.name)) return false;
    }
    if (reader.offset != size) return false;
    return true;
}

}  // namespace rabbit::protocol
