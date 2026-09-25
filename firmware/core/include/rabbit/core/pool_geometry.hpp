#pragma once

#include "rabbit/core/types.hpp"

namespace rabbit::core {

[[nodiscard]] ValidationError validate_pool(const PoolProfile& profile);
[[nodiscard]] bool supports_distance(const PoolProfile& profile, std::int32_t distance_mm);
[[nodiscard]] std::uint16_t pixel_for_distance(
    const PoolProfile& profile,
    std::int32_t distance_mm,
    Direction direction);
[[nodiscard]] std::int32_t millimetres_per_unit(DistanceUnit unit);

}  // namespace rabbit::core
