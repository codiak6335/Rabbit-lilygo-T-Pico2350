#include "rabbit/core/pool_geometry.hpp"

#include <algorithm>
#include <cstdint>

namespace rabbit::core {

std::int32_t millimetres_per_unit(const DistanceUnit unit) {
    return unit == DistanceUnit::Yard ? 914 : 1000;
}

ValidationError validate_pool(const PoolProfile& profile) {
    if (profile.name.empty() || profile.length_mm <= 0 || profile.pixel_count == 0 ||
        profile.segment_count == 0 || profile.segment_count > kMaxPoolSegments) {
        return ValidationError::InvalidPool;
    }

    std::uint16_t previous_pixel = 0;
    std::int32_t previous_distance = 0;
    for (std::uint8_t index = 0; index < profile.segment_count; ++index) {
        const auto& segment = profile.segments[index];
        if (segment.first_pixel >= profile.pixel_count || segment.distance_mm < 0 ||
            segment.distance_mm > profile.length_mm ||
            (index > 0 && (segment.first_pixel <= previous_pixel || segment.distance_mm < previous_distance))) {
            return ValidationError::InvalidGeometry;
        }
        previous_pixel = segment.first_pixel;
        previous_distance = segment.distance_mm;
    }
    return ValidationError::None;
}

bool supports_distance(const PoolProfile& profile, const std::int32_t distance_mm) {
    return distance_mm > 0 && distance_mm <= static_cast<std::int32_t>(kMaxDistanceMillimetres) &&
        profile.length_mm > 0 && distance_mm % profile.length_mm == 0;
}

std::uint16_t pixel_for_distance(
    const PoolProfile& profile,
    const std::int32_t distance_mm,
    const Direction direction) {
    if (profile.pixel_count == 0 || profile.segment_count == 0) {
        return 0;
    }

    const auto clamped_distance = std::clamp(distance_mm, std::int32_t{0}, profile.length_mm);
    std::uint16_t first_pixel = profile.segments[0].first_pixel;
    std::int32_t first_distance = profile.segments[0].distance_mm;
    std::uint16_t last_pixel = static_cast<std::uint16_t>(profile.pixel_count - 1U);
    std::int32_t last_distance = profile.length_mm;

    for (std::uint8_t index = 1; index < profile.segment_count; ++index) {
        const auto& next = profile.segments[index];
        if (clamped_distance <= next.distance_mm) {
            last_pixel = next.first_pixel;
            last_distance = next.distance_mm;
            break;
        }
        first_pixel = next.first_pixel;
        first_distance = next.distance_mm;
    }

    const auto distance_span = std::max(std::int32_t{1}, last_distance - first_distance);
    const auto pixel_span = static_cast<std::int32_t>(last_pixel) - static_cast<std::int32_t>(first_pixel);
    const auto offset = (static_cast<std::int64_t>(clamped_distance - first_distance) * pixel_span) / distance_span;
    auto pixel = static_cast<std::int32_t>(first_pixel) + static_cast<std::int32_t>(offset);
    pixel = std::clamp(pixel, std::int32_t{0}, static_cast<std::int32_t>(profile.pixel_count) - 1);
    if (direction == Direction::FarToNear) {
        pixel = static_cast<std::int32_t>(profile.pixel_count) - 1 - pixel;
    }
    return static_cast<std::uint16_t>(pixel);
}

}  // namespace rabbit::core
