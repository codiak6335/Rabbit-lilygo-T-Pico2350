#include "rabbit/core/workout_engine.hpp"

#include <algorithm>

namespace rabbit::core {
namespace {
constexpr Microseconds kBeepOnUs = 150'000;
constexpr Microseconds kBeepGapUs = 850'000;
constexpr Microseconds kTripleBeepDurationUs = (kBeepOnUs * 3) + (kBeepGapUs * 2);

std::uint16_t pacing_progress_permille(const SwimEntry& swim, const std::int32_t pool_length_mm,
                                       const Microseconds elapsed_us) {
    const auto duration = std::max<Microseconds>(1, swim.target_us);
    const auto elapsed = std::clamp(elapsed_us, Microseconds{0}, duration);
    if (swim.strategy == PacingStrategy::NegativeSplit) {
        const auto first_half_us = (duration + swim.split_delta_us) / 2;
        return static_cast<std::uint16_t>(elapsed <= first_half_us ?
            (elapsed * 500) / first_half_us :
            500 + ((elapsed - first_half_us) * 500) / (duration - first_half_us));
    }
    if (swim.strategy != PacingStrategy::Surge || pool_length_mm <= 0) {
        return static_cast<std::uint16_t>((elapsed * 1000) / duration);
    }
    const auto lengths = static_cast<std::uint32_t>(swim.distance_mm / pool_length_mm);
    if (lengths <= 1U) return static_cast<std::uint16_t>((elapsed * 1000) / duration);
    const auto low = static_cast<std::uint32_t>(1000U - swim.surge_permille);
    const auto high = static_cast<std::uint32_t>(1000U + swim.surge_permille);
    const auto total_weight = lengths * 1000U - (lengths % 2U == 0U ? 0U : swim.surge_permille);
    auto weighted_elapsed = static_cast<std::uint64_t>(elapsed) * total_weight /
        static_cast<std::uint64_t>(duration);
    std::uint32_t completed = static_cast<std::uint32_t>(std::min<std::uint64_t>(lengths / 2U, weighted_elapsed / 2000U)) * 2U;
    weighted_elapsed -= static_cast<std::uint64_t>(completed / 2U) * 2000U;
    if (completed < lengths && weighted_elapsed >= low) {
        weighted_elapsed -= low;
        ++completed;
    }
    if (completed < lengths && weighted_elapsed >= high) {
        weighted_elapsed -= high;
        ++completed;
    }
    const auto weight = completed % 2U == 0U ? low : high;
    const auto partial = completed < lengths ? static_cast<std::uint32_t>(weighted_elapsed * 1000U / weight) : 0U;
    return static_cast<std::uint16_t>((completed * 1000U + partial) / lengths);
}
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
    last_request_id_ = 0;
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
            if (plan_.continuous) {
                entry_index_ = 0;
                ++cycle_;
            } else {
                state_ = EngineState::Complete;
                ++sequence_;
                return true;
            }
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

bool WorkoutEngine::cancel(const std::uint32_t request_id) {
    if (state_ != EngineState::Prepared && state_ != EngineState::Stopped &&
        state_ != EngineState::Complete && state_ != EngineState::Idle) return false;
    last_request_id_ = request_id;
    pending_swim_ = false;
    state_ = EngineState::Idle;
    entry_index_ = 0;
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
    if (state_ != EngineState::Idle) {
        result.entry_count = plan_.entry_count;
        result.continuous = plan_.continuous;
        const auto* entry = current_entry();
        if (entry != nullptr && entry->kind == EntryKind::Swim) {
            result.distance_mm = static_cast<std::uint32_t>(entry->swim.distance_mm);
            result.target_ms = static_cast<std::uint32_t>(entry->swim.target_us / 1000);
        } else if (entry != nullptr && entry->kind == EntryKind::Rest) {
            result.target_ms = static_cast<std::uint32_t>(entry->rest.duration_us / 1000);
        }
    }
    result.cycle = cycle_;
    result.running = state_ == EngineState::PreStart || state_ == EngineState::Swimming || state_ == EngineState::Resting;
    result.audio_on = is_audio_window(now_us);
    result.sequence = sequence_;
    if (state_ == EngineState::Swimming) {
        const auto* entry = current_entry();
        const auto duration = entry == nullptr ? 1 : std::max<Microseconds>(1, entry->swim.target_us);
        const auto elapsed = std::clamp(now_us - swim_started_us_, Microseconds{0}, duration);
        const auto progress = entry == nullptr ? std::uint16_t{0} :
            pacing_progress_permille(entry->swim, pool_.length_mm, elapsed);
        result.cursor_visible = true;
        result.cursor_progress_permille = progress;
        if (entry != nullptr && pool_.length_mm > 0) {
            const auto distance = static_cast<std::int32_t>((static_cast<std::int64_t>(entry->swim.distance_mm) * progress) / 1000);
            auto lap = distance / pool_.length_mm;
            auto position = distance % pool_.length_mm;
            if (progress == 1000U) {
                lap = lap > 0 ? lap - 1 : 0;
                position = pool_.length_mm;
            }
            const auto direction = lap % 2 == 0 ? plan_.direction :
                (plan_.direction == Direction::NearToFar ? Direction::FarToNear : Direction::NearToFar);
            result.cursor_pixel = pixel_for_distance(pool_, position, direction);
        }
        result.seconds_until_next_us = std::max<Microseconds>(0, deadline_us_ - now_us);
    } else if (state_ == EngineState::PreStart || state_ == EngineState::Resting) {
        Microseconds next_start_us = deadline_us_;
        if (state_ == EngineState::Resting) {
            const auto* entry = current_entry();
            const auto next_index = static_cast<std::uint16_t>(entry_index_ + 1U);
            const bool next_is_swim = next_index < plan_.entry_count ?
                plan_.entries[next_index].kind == EntryKind::Swim :
                plan_.continuous && plan_.entries[0].kind == EntryKind::Swim;
            if (entry != nullptr && entry->kind == EntryKind::Swim && entry->swim.wait_for_interval &&
                plan_.audio_enabled && next_is_swim) {
                next_start_us += kTripleBeepDurationUs;
            }
        }
        result.seconds_until_next_us = std::max<Microseconds>(0, next_start_us - now_us);
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
    bool has_timed_entry = false;
    for (std::uint16_t index = 0; index < plan.entry_count; ++index) {
        const auto& entry = plan.entries[index];
        if (entry.label.size > kMaxLabelBytes) return ValidationError::InvalidEntry;
        if (entry.kind == EntryKind::Swim) {
            has_timed_entry = true;
            if (!supports_distance(pool, entry.swim.distance_mm)) return ValidationError::InvalidDistance;
            if (entry.swim.target_us <= 0 || entry.swim.target_us > kMaxDurationUs) return ValidationError::InvalidDuration;
            if (entry.swim.interval_us < entry.swim.target_us || entry.swim.interval_us > kMaxDurationUs) return ValidationError::InvalidInterval;
            if (entry.swim.strategy == PacingStrategy::NegativeSplit &&
                (entry.swim.split_delta_us <= 0 || entry.swim.split_delta_us >= entry.swim.target_us ||
                 entry.swim.distance_mm / pool.length_mm < 2 ||
                 (entry.swim.distance_mm / pool.length_mm) % 2 != 0)) {
                return ValidationError::InvalidStrategy;
            }
            if (entry.swim.strategy == PacingStrategy::Surge &&
                (entry.swim.surge_permille == 0 || entry.swim.surge_permille > 450)) {
                return ValidationError::InvalidStrategy;
            }
        } else if (entry.kind == EntryKind::Rest) {
            if (entry.rest.duration_us < 0 || entry.rest.duration_us > kMaxDurationUs) return ValidationError::InvalidDuration;
            if (entry.rest.duration_us > 0) has_timed_entry = true;
        } else if (entry.kind == EntryKind::Activity && entry.activity.name.empty()) {
            return ValidationError::InvalidEntry;
        }
    }
    if (plan.continuous && !has_timed_entry) return ValidationError::InvalidEntry;
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
        const auto next_index = static_cast<std::uint16_t>(entry_index_ + 1U);
        const bool next_is_swim = next_index < plan_.entry_count ?
            plan_.entries[next_index].kind == EntryKind::Swim :
            plan_.continuous && plan_.entries[0].kind == EntryKind::Swim;
        const Microseconds beep_lead_us = plan_.audio_enabled && next_is_swim ? kTripleBeepDurationUs : 0;
        deadline_us_ = std::max(now_us, swim_started_us_ + entry->swim.interval_us - beep_lead_us);
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
