#pragma once

#include "order.h"

#include <cstdint>
#include <string>

namespace ob
{
    // event category
    enum class EventType
    {
        OrderAccepted,
        OrderRejected,

        Trade,

        OrderResting,
        OrderCompleted,

        MakerCompleted,

        OrderCancelled,
        CancelRejected
    };

    // event is emitted by the engine and can be logged and replayed
    struct Event
    {
        EventType type { EventType::OrderRejected };

        // primary id for order scoped events
        OrderId id { 0 };

        // seq for the primary id when relevant
        std::uint64_t seq { 0 };

        // context for add events and cancel events
        Side side { Side::Buy };
        PriceTicks price_ticks { 0 };
        Qty qty { 0 };

        // remaining qty for resting or completion summary events
        Qty remaining_qty { 0 };

        // trade fields
        OrderId maker_id { 0 };
        std::uint64_t maker_seq { 0 };
        OrderId taker_id { 0 };
        std::uint64_t taker_seq { 0 };
        PriceTicks trade_price_ticks { 0 };
        Qty trade_qty { 0 };

        // stable token for tests and logs
        const char* reason { "" };
    };
}
