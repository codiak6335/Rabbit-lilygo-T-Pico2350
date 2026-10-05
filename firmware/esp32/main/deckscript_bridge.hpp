#pragma once

#include "cJSON.h"
#include "rabbit/core/types.hpp"

namespace rabbit::esp32 {

[[nodiscard]] bool parse_deckscript_plan(const cJSON* packed, core::WorkoutPlan& plan,
                                         core::PoolProfile& pool, const char*& error);

}  // namespace rabbit::esp32
