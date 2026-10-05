#include <array>
#include <cstdio>
#include <cstring>

#include "deckscript_bridge.hpp"
#include "rabbit/protocol/plan_codec.hpp"

// Host fixture for the same embedded pool symbol used by the ESP build.
extern const unsigned char rabbit_pools_json_start[] asm("_binary_rabbit_pools_json_start") =
    R"json({"defaultPool":"Bellevue East","pools":{"Bellevue East":{"Length":"25 yards"}}})json";

int main() {
    std::array<char, rabbit::protocol::kMaxPlanBytes + 1U> input{};
    const auto count = std::fread(input.data(), 1, rabbit::protocol::kMaxPlanBytes, stdin);
    cJSON* const root = cJSON_ParseWithLength(input.data(), count);
    if (root == nullptr) return 1;
    rabbit::core::WorkoutPlan plan{};
    rabbit::core::PoolProfile pool{};
    const char* error = nullptr;
    const bool parsed = rabbit::esp32::parse_deckscript_plan(
        cJSON_GetObjectItemCaseSensitive(root, "plan"), plan, pool, error);
    cJSON_Delete(root);
    if (!parsed) {
        std::fprintf(stderr, "%s\n", error == nullptr ? "invalid plan" : error);
        return 2;
    }
    std::array<std::uint8_t, rabbit::protocol::kMaxPlanBytes> encoded{};
    std::uint16_t size = 0;
    if (!rabbit::protocol::encode_plan(plan, pool, encoded.data(),
                                       static_cast<std::uint16_t>(encoded.size()), size)) return 3;
    rabbit::core::WorkoutPlan decoded{};
    rabbit::core::PoolProfile decoded_pool{};
    if (!rabbit::protocol::decode_plan(encoded.data(), size, decoded, decoded_pool) ||
        decoded.entry_count != plan.entry_count || decoded_pool.length_mm != pool.length_mm ||
        std::strcmp(decoded.name.c_str(), plan.name.c_str()) != 0) return 4;
    std::printf("%s: %u steps, %u bytes\n", decoded.name.c_str(),
                static_cast<unsigned int>(decoded.entry_count), static_cast<unsigned int>(size));
    return 0;
}
