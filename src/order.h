#pragma once

#include <cstdint>

namespace ob
{
    // order id type
    using OrderId = std::uint64_t;

    // side of the book
    enum class Side
    {
        Buy,
        Sell
    };

    // integer tick pricing
    using PriceTicks = std::int64_t;

    // integer quantity
    using Qty = std::int64_t;

    // stored resting order state in the book
    struct Order
    {
        OrderId id {};
        Side side { Side::Buy };
        PriceTicks price_ticks { 0 };
        Qty qty { 0 }; // remaining qty
        std::uint64_t seq { 0 }; // assigned by book
        Order* next { nullptr };
        Order* prev { nullptr };
    };

    // validates caller supplied values for add limit
    inline bool is_valid_input(OrderId id, PriceTicks price_ticks, Qty qty)
    {
        if (id == 0)
        {
            return false;
        }
        if (price_ticks <= 0)
        {
            return false;
        }
        if (qty <= 0)
        {
            return false;
        }
        return true;
    }
}
