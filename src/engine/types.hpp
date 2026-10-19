#pragma once
// Core value types shared by the whole engine.
// Prices are stored as integer ticks (1 tick = 0.01) so matching never
// suffers from floating-point rounding.

#include <cstdint>
#include <string>

namespace nx {

using OrderId  = std::uint64_t;
using ClientId = std::uint64_t;
using Price    = std::int64_t;  // in ticks
using Qty      = std::int64_t;

inline constexpr double kTickSize = 0.01;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

inline const char* toString(Side s) { return s == Side::Buy ? "buy" : "sell"; }
inline const char* toString(OrderType t) { return t == OrderType::Limit ? "limit" : "market"; }

struct Order {
    OrderId   id        = 0;
    ClientId  client    = 0;
    Side      side      = Side::Buy;
    OrderType type      = OrderType::Limit;
    Price     price     = 0;  // ignored for market orders
    Qty       qty       = 0;  // original quantity
    Qty       remaining = 0;  // still open
};

struct Trade {
    std::uint64_t seq        = 0;  // per-symbol trade sequence number
    OrderId       buyId      = 0;
    OrderId       sellId     = 0;
    ClientId      buyClient  = 0;
    ClientId      sellClient = 0;
    Price         price      = 0;  // always the resting order's price
    Qty           qty        = 0;
    Side          aggressor  = Side::Buy;  // side of the incoming order
};

// Result status of a new order after the matching step.
enum class OrderStatus : std::uint8_t { Filled, PartiallyFilled, Resting, Unfilled };

inline const char* toString(OrderStatus s) {
    switch (s) {
        case OrderStatus::Filled: return "filled";
        case OrderStatus::PartiallyFilled: return "partial";
        case OrderStatus::Resting: return "resting";
        case OrderStatus::Unfilled: return "unfilled";
    }
    return "unknown";
}

}  // namespace nx
