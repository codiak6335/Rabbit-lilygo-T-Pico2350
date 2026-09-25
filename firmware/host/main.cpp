#include <cstdio>

#include "rabbit/core/workout_engine.hpp"

int main() {
    rabbit::core::PoolProfile pool{};
    if (!pool.name.assign("Desktop Pool", 12)) return 1;
    pool.length_mm = 22'860;
    pool.pixel_count = 910;
    pool.segment_count = 2;
    pool.segments[0] = {174, 0};
    pool.segments[1] = {883, 22'860};
    pool.revision = 1;

    rabbit::core::WorkoutPlan plan{};
    if (!plan.name.assign("Host demo", 9) || !plan.pool_name.assign("Desktop Pool", 12)) return 1;
    plan.pool_revision = 1;
    plan.audio_enabled = false;
    plan.entry_count = 1;
    plan.entries[0].kind = rabbit::core::EntryKind::Swim;
    plan.entries[0].swim.distance_mm = 22'860;
    plan.entries[0].swim.target_us = 30'000'000;
    plan.entries[0].swim.interval_us = 30'000'000;

    rabbit::core::WorkoutEngine engine{};
    if (engine.prepare(plan, pool) != rabbit::core::ValidationError::None || !engine.start(0, 1)) {
        std::fputs("Unable to prepare host demo\n", stderr);
        return 1;
    }
    engine.advance(0);
    const auto state = engine.snapshot(15'000'000);
    std::printf("state=%u pixel=%u progress=%u\n", static_cast<unsigned>(state.state), state.cursor_pixel,
                state.cursor_progress_permille);
    return 0;
}
