#pragma once

#include "rabbit/core/pool_geometry.hpp"

namespace rabbit::core {

class WorkoutEngine {
public:
    [[nodiscard]] ValidationError prepare(const WorkoutPlan& plan, const PoolProfile& pool);
    [[nodiscard]] bool start(Microseconds now_us, std::uint32_t request_id);
    [[nodiscard]] bool stop(std::uint32_t request_id);
    [[nodiscard]] bool cancel(std::uint32_t request_id);
    void advance(Microseconds now_us);
    [[nodiscard]] EngineSnapshot snapshot(Microseconds now_us) const;
    [[nodiscard]] EngineState state() const { return state_; }
    [[nodiscard]] const WorkoutPlan& plan() const { return plan_; }
    [[nodiscard]] const PoolProfile& pool() const { return pool_; }
    [[nodiscard]] std::uint32_t last_request_id() const { return last_request_id_; }

private:
    [[nodiscard]] ValidationError validate_plan(const WorkoutPlan& plan, const PoolProfile& pool) const;
    void enter_entry(Microseconds now_us);
    void begin_swim(Microseconds now_us);
    void complete_entry(Microseconds now_us);
    [[nodiscard]] bool is_audio_window(Microseconds now_us) const;
    [[nodiscard]] const PlanEntry* current_entry() const;

    WorkoutPlan plan_{};
    PoolProfile pool_{};
    EngineState state_{EngineState::Idle};
    std::uint16_t entry_index_{0};
    std::uint32_t cycle_{0};
    std::uint32_t sequence_{0};
    std::uint32_t last_request_id_{0};
    Microseconds entry_started_us_{0};
    Microseconds deadline_us_{0};
    Microseconds swim_started_us_{0};
    Microseconds announcement_started_us_{0};
    bool pending_swim_{false};
};

}  // namespace rabbit::core
