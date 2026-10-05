#pragma once

#include <cstdint>

#include "rabbit/core/types.hpp"
#include "rabbit/protocol/framing.hpp"

namespace rabbit::protocol {

// Bounded, versioned binary form of a compiled DeckScript execution plan.
[[nodiscard]] bool encode_plan(const core::WorkoutPlan& plan, const core::PoolProfile& pool,
                               std::uint8_t* output, std::uint16_t capacity, std::uint16_t& size);
[[nodiscard]] bool decode_plan(const std::uint8_t* data, std::uint16_t size,
                               core::WorkoutPlan& plan, core::PoolProfile& pool);

}  // namespace rabbit::protocol
