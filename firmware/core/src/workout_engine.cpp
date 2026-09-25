#include "rabbit/core/workout_engine.hpp"

#include <algorithm>

namespace rabbit::core {
namespace {
constexpr Microseconds kBeepOnUs = 150'000;
constexpr Microseconds kBeepGapUs = 850'000;
constexpr Microseconds kTripleBeepDurationUs = (kBeepOnUs * 3) + (kBeepGapUs * 2);
}

ValidationError WorkoutEngine::prepare(const WorkoutPlan& plan, const PoolProfile& pool) {
    if (state_ == EngineState::PreStart || state_ == EngineState::Swimming || state_ == EngineState::Resting) {
        return ValidationError::InvalidEntry;
    }
    const auto result = validate_plan(plan, pool);
    if (result != ValidationError::None) {
        return result;
    }
    plan_ = plan;
    pool_ = pool;
    entry_index_ = 0;
    cycle_ = 1;
    sequence_++;
    pending_swim_ = false;
    state_ = EngineState::Prepared;
    return ValidationError::None;
}

bool WorkoutEngine::start(const Microseconds now_us, const std::uint32_t request_id) {
    if (request_id == last_request_id_) {
        return state_ == EngineState::PreStart || state_ == EngineState::Swimming || state_ == EngineState::Resting;
    }
    if (state_ != EngineState::Prepared && state_ != EngineState::Stopped) {
        return false;
    }
    last_request_id_ = request_id;
    if (state_ == EngineState::Stopped) {
        ++entry_index_;
        if (entry_index_ >= plan_.entry_count) {
            state_ = EngineState::Complete;
            ++sequence_;
            return true;
        }
    }
    enter_entry(now_us);
    ++sequence_;
    return true;
}

bool WorkoutEngine::stop(const std::uint32_t request_id) {
    if (request_id == last_request_id_) {
        return state_ == EngineState::Stopped;
    }
    if (state_ != EngineState::PreStart && state_ != EngineState::Swimming && state_ != EngineState::Resting) {
        return false;
    }
    last_request_id_ = request_id;
    pending_swim_ = false;
    state_ = EngineState::Stopped;
    ++sequence_;
    return true;
}

void WorkoutEngine::advance(const Microseconds now_us) {
    if (state_ == EngineState::PreStart && pending_swim_ && now_us >= deadline_us_) {
        begin_swim(now_us);
        return;
    }
    if ((state_ == EngineState::Swimming || state_ == EngineState::Resting) && now_us >= deadline_us_) {
        complete_entry(now_us);
    }
}

EngineSnapshot WorkoutEngine::snapshot(const Microseconds now_us) const {
    EngineSnapshot result{};
    result.state = state_;
    result.entry_index = entry_index_;
    result.cycle = cycle_;
    result.running = state_ == EngineState::PreStart || state_ == EngineState::Swimming || state_ == EngineState::Resting;
    result.audio_on = is_audio_window(now_us);
    result.sequence = sequence_;
    if (state_ == EngineState::Swimming) {
        const auto* entry = current_entry();
        const auto duration = entry == nullptr ? 1 : std::max<Microseconds>(1, entry->swim.target_us);
        const auto elapsed = std::clamp(now_us - swim_started_us_, Microseconds{0}, duration);
        const auto progress = static_cast<std::uint16_t>((elapsed * 1000) / duration);
        result.cursor_visible = true;
        result.cursor_progress_permille = progress;
        const auto distance = static_cast<std::int32_t>((static_cast<std::int64_t>(entry->swim.distance_mm) * progress) / 1000);
        result.cursor_pixel = pixel_for_distance(pool_, distance, plan_.direction);
        result.seconds_until_next_us = std::max<Microseconds>(0, deadline_us_ - now_us);
    } else if (state_ == EngineState::PreStart || state_ == EngineState::Resting) {
        result.seconds_until_next_us = std::max<Microseconds>(0, deadline_us_ - now_us);
    }
    return result;
}

ValidationError WorkoutEngine::validate_plan(const WorkoutPlan& plan, const PoolProfile& pool) const {
    if (plan.version != 2) return ValidationError::UnsupportedVersion;
    if (plan.name.empty()) return ValidationError::MissingName;
    if (plan.pool_name.empty()) return ValidationError::MissingPool;
    if (plan.entry_count == 0) return ValidationError::EmptyPlan;
    if (plan.entry_count > kMaxPlanEntries) return ValidationError::TooManyEntries;
    if (validate_pool(pool) != ValidationError::None || plan.pool_revision != pool.revision) return ValidationError::InvalidPool;
    for (std::uint16_t index = 0; index < plan.entry_count; ++index) {
        const auto& entry = plan.entries[index];
        if (entry.label.size > kMaxLabelBytes) return ValidationError::InvalidEntry;
        if (entry.kind == EntryKind::Swim) {
            if (!supports_distance(pool, entry.swim.distance_mm)) return ValidationError::InvalidDistance;
            if (entry.swim.target_us <= 0 || entry.swim.target_us > kMaxDurationUs) return ValidationError::InvalidDuration;
            if (entry.swim.interval_us < entry.swim.target_us || entry.swim.interval_us > kMaxDurationUs) return ValidationError::InvalidInterval;
            if (entry.swim.strategy == PacingStrategy::NegativeSplit &&
                (entry.swim.split_delta_us <= 0 || entry.swim.split_delta_us >= entry.swim.target_us)) {
                return ValidationError::InvalidStrategy;
            }
            if (entry.swim.strategy == PacingStrategy::Surge &&
                (entry.swim.surge_permille == 0 || entry.swim.surge_permille > 450)) {
                return ValidationError::InvalidStrategy;
            }
        } else if (entry.kind == EntryKind::Rest) {
            if (entry.rest.duration_us < 0 || entry.rest.duration_us > kMaxDurationUs) return ValidationError::InvalidDuration;
        } else if (entry.kind == EntryKind::Activity && entry.activity.name.empty()) {
            return ValidationError::InvalidEntry;
        }
    }
    return ValidationError::None;
}

void WorkoutEngine::enter_entry(const Microseconds now_us) {
    while (entry_index_ < plan_.entry_count) {
        const auto& entry = plan_.entries[entry_index_];
        entry_started_us_ = now_us;
        if (entry.kind == EntryKind::Activity) {
            ++entry_index_;
            continue;
        }
        if (entry.kind == EntryKind::Rest) {
            deadline_us_ = now_us + entry.rest.duration_us;
            state_ = EngineState::Resting;
            return;
        }
        pending_swim_ = true;
        announcement_started_us_ = now_us;
        deadline_us_ = now_us + (plan_.audio_enabled ? kTripleBeepDurationUs : 0);
        state_ = EngineState::PreStart;
        return;
    }
    if (plan_.continuous) {
        entry_index_ = 0;
        ++cycle_;
        enter_entry(now_us);
        return;
    }
    state_ = EngineState::Complete;
    ++sequence_;
}

void WorkoutEngine::begin_swim(const Microseconds now_us) {
    const auto* entry = current_entry();
    if (entry == nullptr) {
        state_ = EngineState::Fault;
        ++sequence_;
        return;
    }
    pending_swim_ = false;
    swim_started_us_ = now_us;
    deadline_us_ = now_us + entry->swim.target_us;
    state_ = EngineState::Swimming;
    ++sequence_;
}

void WorkoutEngine::complete_entry(const Microseconds now_us) {
    const auto* entry = current_entry();
    if (entry == nullptr) {
        state_ = EngineState::Fault;
        ++sequence_;
        return;
    }
    if (state_ == EngineState::Swimming && entry->swim.wait_for_interval &&
        entry->swim.interval_us > entry->swim.target_us) {
        deadline_us_ = swim_started_us_ + entry->swim.interval_us;
        state_ = EngineState::Resting;
        ++sequence_;
        return;
    }
    ++entry_index_;
    enter_entry(now_us);
    ++sequence_;
}

bool WorkoutEngine::is_audio_window(const Microseconds now_us) const {
    if (state_ != EngineState::PreStart || !plan_.audio_enabled) return false;
    const auto elapsed = now_us - announcement_started_us_;
    return (elapsed >= 0 && elapsed < kBeepOnUs) ||
        (elapsed >= kBeepOnUs + kBeepGapUs && elapsed < (2 * kBeepOnUs) + kBeepGapUs) ||
        (elapsed >= (2 * kBeepOnUs) + (2 * kBeepGapUs) && elapsed < kTripleBeepDurationUs);
}

const PlanEntry* WorkoutEngine::current_entry() const {
    return entry_index_ < plan_.entry_count ? &plan_.entries[entry_index_] : nullptr;
}

}  // namespace rabbit::core
