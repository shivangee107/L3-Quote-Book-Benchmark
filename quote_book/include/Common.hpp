#pragma once

#include <cstdint>
#include <optional>
#include <string>

using Price = std::int32_t;
using Qty = std::int32_t;

constexpr Price kDefaultMinPriceTick = 0;
constexpr Price kDefaultMaxPriceTick = 100000;
constexpr Price kDefaultMidPriceTick = 50000;

// Keep the enum values explicit because they are sent over the wire.
enum class Side : std::uint8_t {
    Bid = 0,
    Ask = 1
};

struct Level {
    Price price{};
    Qty qty{};
};

inline std::string toString(Side side) {
    return side == Side::Bid ? "Bid" : "Ask";
}
