#include "deckscript_bridge.hpp"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "rabbit/core/pool_geometry.hpp"

extern const unsigned char rabbit_pools_json_start[] asm("_binary_rabbit_pools_json_start");

namespace rabbit::esp32 {
namespace {

template <std::size_t Capacity>
bool assign_text(core::FixedText<Capacity>& target, const cJSON* value, const bool required) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr) return !required && cJSON_IsNull(value);
    const auto size = std::strlen(value->valuestring);
    return (size != 0U || !required) && target.assign(value->valuestring, size);
}

bool milliseconds(const cJSON* value, std::uint32_t& result, const bool allow_zero) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble)) return false;
    const auto scaled = value->valuedouble * 1000.0;
    if (scaled < (allow_zero ? 0.0 : 1.0) || scaled > 86'400'000.0) return false;
    result = static_cast<std::uint32_t>(std::llround(scaled));
    return allow_zero || result != 0U;
}

bool load_pool(const cJSON* name, core::PoolProfile& pool) {
    cJSON* const root = cJSON_Parse(reinterpret_cast<const char*>(rabbit_pools_json_start));
    if (root == nullptr) return false;
    const cJSON* pool_name = name;
    if (cJSON_IsNull(pool_name) || pool_name == nullptr) {
        pool_name = cJSON_GetObjectItemCaseSensitive(root, "defaultPool");
    }
    const cJSON* const profiles = cJSON_GetObjectItemCaseSensitive(root, "pools");
    const cJSON* const profile = cJSON_IsString(pool_name) && cJSON_IsObject(profiles) ?
        cJSON_GetObjectItemCaseSensitive(profiles, pool_name->valuestring) : nullptr;
    const cJSON* const length = cJSON_IsObject(profile) ?
        cJSON_GetObjectItemCaseSensitive(profile, "Length") : nullptr;
    bool valid = cJSON_IsObject(profile) && cJSON_IsString(length) &&
        assign_text(pool.name, pool_name, true);
    if (valid) {
        char unit[16]{};
        unsigned int count = 0;
        valid = std::sscanf(length->valuestring, "%u %15s", &count, unit) == 2 && count > 0U && count <= 1000U;
        if (valid && (std::strcmp(unit, "yard") == 0 || std::strcmp(unit, "yards") == 0)) {
            pool.unit = core::DistanceUnit::Yard;
            pool.length_mm = static_cast<std::int32_t>((static_cast<std::uint64_t>(count) * 9144U) / 10U);
        } else if (valid && (std::strcmp(unit, "meter") == 0 || std::strcmp(unit, "meters") == 0 ||
                             std::strcmp(unit, "metre") == 0 || std::strcmp(unit, "metres") == 0)) {
            pool.unit = core::DistanceUnit::Metre;
            pool.length_mm = static_cast<std::int32_t>(count * 1000U);
        } else valid = false;
    }
    if (valid) {
        // GPIO/LED geometry is still unverified; the controller validates lengths
        // without claiming the old board's physical LED mapping.
        pool.pixel_count = 1;
        pool.segment_count = 1;
        pool.segments[0] = {0, 0};
        pool.revision = 1;
    }
    cJSON_Delete(root);
    return valid;
}

bool parse_swim(const cJSON* source, core::PlanEntry& entry, const core::PoolProfile& pool,
                const char*& error) {
    if (cJSON_GetArraySize(source) < 15) {
        error = "Incomplete swim entry";
        return false;
    }
    const cJSON* const distance = cJSON_GetArrayItem(source, 1);
    const cJSON* const target = cJSON_GetArrayItem(source, 2);
    const cJSON* const interval = cJSON_GetArrayItem(source, 3);
    const cJSON* const label = cJSON_GetArrayItem(source, 4);
    const cJSON* const strategy = cJSON_GetArrayItem(source, 11);
    const cJSON* const split = cJSON_GetArrayItem(source, 12);
    const cJSON* const wait = cJSON_GetArrayItem(source, 13);
    const cJSON* const variation = cJSON_GetArrayItem(source, 14);
    if (!cJSON_IsNumber(distance) || !std::isfinite(distance->valuedouble) ||
        distance->valuedouble <= 0.0 || distance->valuedouble > 5000.0 ||
        !assign_text(entry.label, label, false) || !cJSON_IsString(strategy) ||
        !cJSON_IsBool(wait)) {
        error = "Invalid swim distance, label, strategy, or send-off";
        return false;
    }
    const auto distance_mm = static_cast<std::int32_t>(std::llround(
        distance->valuedouble * (pool.unit == core::DistanceUnit::Yard ? 914.4 : 1000.0)));
    if (!core::supports_distance(pool, distance_mm)) {
        error = "Swim distance must be whole pool lengths";
        return false;
    }
    std::uint32_t target_ms = 0;
    std::uint32_t interval_ms = 0;
    if (!milliseconds(target, target_ms, false) ||
        !(cJSON_IsNull(interval) ? (interval_ms = target_ms, true) : milliseconds(interval, interval_ms, false)) ||
        interval_ms < target_ms) {
        error = "Invalid Hold or On time";
        return false;
    }
    entry.kind = core::EntryKind::Swim;
    entry.swim.distance_mm = distance_mm;
    entry.swim.target_us = static_cast<core::Microseconds>(target_ms) * 1000;
    entry.swim.interval_us = static_cast<core::Microseconds>(interval_ms) * 1000;
    entry.swim.wait_for_interval = cJSON_IsTrue(wait);
    if (std::strcmp(strategy->valuestring, "even") == 0) {
        entry.swim.strategy = core::PacingStrategy::Even;
    } else if (std::strcmp(strategy->valuestring, "negativeSplit") == 0) {
        std::uint32_t split_ms = 0;
        const auto lengths = distance_mm / pool.length_mm;
        if (!milliseconds(split, split_ms, false) || split_ms >= target_ms ||
            lengths < 2 || lengths % 2 != 0) {
            error = "Negative split needs an even number of lengths and a valid margin";
            return false;
        }
        entry.swim.strategy = core::PacingStrategy::NegativeSplit;
        entry.swim.split_delta_us = static_cast<core::Microseconds>(split_ms) * 1000;
    } else if (std::strcmp(strategy->valuestring, "surge") == 0) {
        if (!cJSON_IsNumber(variation) || !std::isfinite(variation->valuedouble) ||
            variation->valuedouble < 0.001 || variation->valuedouble > 0.45) {
            error = "Invalid surge percentage";
            return false;
        }
        entry.swim.strategy = core::PacingStrategy::Surge;
        entry.swim.surge_permille = static_cast<std::uint16_t>(std::lround(variation->valuedouble * 1000.0));
    } else {
        error = "Unsupported pacing strategy";
        return false;
    }
    return true;
}

}  // namespace

bool parse_deckscript_plan(const cJSON* packed, core::WorkoutPlan& plan,
                           core::PoolProfile& pool, const char*& error) {
    error = "Invalid DeckScript plan";
    if (!cJSON_IsObject(packed)) return false;
    const cJSON* const version = cJSON_GetObjectItemCaseSensitive(packed, "version");
    const cJSON* const type = cJSON_GetObjectItemCaseSensitive(packed, "type");
    const cJSON* const name = cJSON_GetObjectItemCaseSensitive(packed, "name");
    const cJSON* const pool_name = cJSON_GetObjectItemCaseSensitive(packed, "pool");
    const cJSON* const direction = cJSON_GetObjectItemCaseSensitive(packed, "direction");
    const cJSON* const audio = cJSON_GetObjectItemCaseSensitive(packed, "audio");
    const cJSON* const continuous = cJSON_GetObjectItemCaseSensitive(packed, "continuous");
    const cJSON* const entries = cJSON_GetObjectItemCaseSensitive(packed, "entries");
    const int count = cJSON_GetArraySize(entries);
    if (!cJSON_IsNumber(version) || version->valuedouble != 2.0 ||
        !cJSON_IsString(type) || std::strcmp(type->valuestring, "compactExecutionPlan") != 0 ||
        !assign_text(plan.name, name, true) ||
        !cJSON_IsString(direction) || !cJSON_IsString(audio) || !cJSON_IsBool(continuous) ||
        !cJSON_IsArray(entries) || count < 1 || count > static_cast<int>(core::kMaxPlanEntries)) return false;
    if (!load_pool(pool_name, pool)) {
        error = "Unknown or invalid pool";
        return false;
    }
    if (std::strcmp(direction->valuestring, "Near") == 0) {
        plan.direction = core::Direction::NearToFar;
    } else if (std::strcmp(direction->valuestring, "Far") == 0) {
        plan.direction = core::Direction::FarToNear;
    } else return false;
    if (std::strcmp(audio->valuestring, "Yes") == 0) {
        plan.audio_enabled = true;
    } else if (std::strcmp(audio->valuestring, "No") == 0) {
        plan.audio_enabled = false;
    } else return false;
    plan.continuous = cJSON_IsTrue(continuous);
    plan.deckscript = true;
    plan.pool_name = pool.name;
    plan.pool_revision = pool.revision;
    plan.entry_count = static_cast<std::uint16_t>(count);
    for (int index = 0; index < count; ++index) {
        const cJSON* const source = cJSON_GetArrayItem(entries, index);
        const cJSON* const kind = cJSON_GetArrayItem(source, 0);
        auto& entry = plan.entries[index];
        if (!cJSON_IsArray(source) || !cJSON_IsString(kind)) return false;
        if (std::strcmp(kind->valuestring, "s") == 0) {
            if (!parse_swim(source, entry, pool, error)) return false;
        } else if (std::strcmp(kind->valuestring, "r") == 0) {
            std::uint32_t rest_ms = 0;
            if (cJSON_GetArraySize(source) < 3 || !milliseconds(cJSON_GetArrayItem(source, 1), rest_ms, true) ||
                !assign_text(entry.label, cJSON_GetArrayItem(source, 2), false)) return false;
            entry.kind = core::EntryKind::Rest;
            entry.rest.duration_us = static_cast<core::Microseconds>(rest_ms) * 1000;
        } else if (std::strcmp(kind->valuestring, "a") == 0) {
            if (cJSON_GetArraySize(source) < 3 ||
                !assign_text(entry.activity.name, cJSON_GetArrayItem(source, 1), true) ||
                !assign_text(entry.label, cJSON_GetArrayItem(source, 2), false)) return false;
            entry.kind = core::EntryKind::Activity;
        } else return false;
    }
    return true;
}

}  // namespace rabbit::esp32
