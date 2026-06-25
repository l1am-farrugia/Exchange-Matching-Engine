#include "order_book.h"

#include <algorithm>
#include <cassert>

namespace ob
{
    bool OrderBook::crosses(Side taker_side, PriceTicks taker_px, PriceTicks maker_px) const
    {
        // buy crosses when maker ask price is <= taker limit
        if (taker_side == Side::Buy)
        {
            return maker_px <= taker_px;
        }

        // sell crosses when maker bid price is >= taker limit
        return maker_px >= taker_px;
    }

    void OrderBook::remove_filled_maker(std::vector<Event>& events, const Order& maker)
    {
        // maker completion helps replay diffs and tests a lot
        Event e {};
        e.type = EventType::MakerCompleted;
        e.id = maker.id;
        e.seq = maker.seq;
        e.side = maker.side;
        e.price_ticks = maker.price_ticks;
        e.qty = 0;
        e.remaining_qty = 0;
        e.reason = "filled";
        events.push_back(e);
    }

    std::size_t OrderBook::recompute_live_count() const
    {
        // recompute live count from containers not from index
        std::size_t total { 0 };

        for (const auto& kv : bids_)
        {
            total += kv.second.size();
        }

        for (const auto& kv : asks_)
        {
            total += kv.second.size();
        }

        return total;
    }

    void OrderBook::assert_invariants() const
    {
        // core size invariant
        assert(index_.size() == recompute_live_count());

        // validate all bid levels and index entries for them
        for (const auto& kv : bids_)
        {
            assert(!kv.second.empty());

            for (const auto& o : kv.second)
            {
                assert(o.side == Side::Buy);
                assert(o.price_ticks == kv.first);
                assert(o.qty > 0);
                assert(o.seq != 0);

                const auto it = index_.find(o.id);
                assert(it != index_.end());

                // locator must point back to this exact stored node
                assert(it->second.side == Side::Buy);
                assert(it->second.price_ticks == kv.first);
                assert(it->second.it->id == o.id);
            }
        }

        // validate all ask levels and index entries for them
        for (const auto& kv : asks_)
        {
            assert(!kv.second.empty());

            for (const auto& o : kv.second)
            {
                assert(o.side == Side::Sell);
                assert(o.price_ticks == kv.first);
                assert(o.qty > 0);
                assert(o.seq != 0);

                const auto it = index_.find(o.id);
                assert(it != index_.end());

                assert(it->second.side == Side::Sell);
                assert(it->second.price_ticks == kv.first);
                assert(it->second.it->id == o.id);
            }
        }
    }
    // applies an add limit
    void OrderBook::add_limit(OrderId id, Side side, PriceTicks price_ticks, Qty qty, std::vector<Event>& out_events) 
    {
        // validate input from caller
        if (!is_valid_input(id, price_ticks, qty))
        {
            Event e {};
            e.type = EventType::OrderRejected;
            e.id = id;
            e.side = side;
            e.price_ticks = price_ticks;
            e.qty = qty;
            e.reason = "invalid";
            events.push_back(e);
            return events;
        }

        // reject duplicate live ids
        if (index_.find(id) != index_.end())
        {
            Event e {};
            e.type = EventType::OrderRejected;
            e.id = id;
            e.side = side;
            e.price_ticks = price_ticks;
            e.qty = qty;
            e.reason = "duplicate_id";
            events.push_back(e);
            return events;
        }

        // assign taker seq deterministically
        const std::uint64_t taker_seq = next_seq_;
        ++next_seq_;

        // acceptance is always first
        {
            Event e {};
            e.type = EventType::OrderAccepted;
            e.id = id;
            e.seq = taker_seq;
            e.side = side;
            e.price_ticks = price_ticks;
            e.qty = qty;
            e.reason = "accepted";
            events.push_back(e);
        }

        Qty remaining = qty;

        if (side == Side::Buy)
        {
            // match against asks while best ask crosses
            while (remaining > 0 && !asks_.empty() && crosses(side, price_ticks, asks_.begin()->first))
            {
                auto lvl_it = asks_.begin();
                const PriceTicks maker_px = lvl_it->first;
                PriceLevel& level = lvl_it->second;

                // walk fifo orders at this level
                auto it = level.begin();
                while (remaining > 0 && it != level.end())
                {
                    const Qty fill = std::min(remaining, it->qty);

                    // trade executes at maker price
                    Event trade {};
                    trade.type = EventType::Trade;
                    trade.maker_id = it->id;
                    trade.maker_seq = it->seq;
                    trade.taker_id = id;
                    trade.taker_seq = taker_seq;
                    trade.trade_price_ticks = maker_px;
                    trade.trade_qty = fill;
                    trade.reason = "trade";
                    events.push_back(trade);

                    remaining -= fill;
                    it->qty -= fill;

                    if (it->qty == 0)
                    {
                        // fully filled maker gets removed from book and index
                        const Order filled_maker = *it;

                        index_.erase(filled_maker.id);
                        it = level.erase(it);

                        remove_filled_maker(events, filled_maker);
                    }
                    else
                    {
                        ++it;
                    }
                }

                if (level.empty())
                {
                    asks_.erase(lvl_it);
                }
            }
        }
        else
        {
            // match against bids while best bid crosses
            while (remaining > 0 && !bids_.empty() && crosses(side, price_ticks, bids_.begin()->first))
            {
                auto lvl_it = bids_.begin();
                const PriceTicks maker_px = lvl_it->first;
                PriceLevel& level = lvl_it->second;

                auto it = level.begin();
                while (remaining > 0 && it != level.end())
                {
                    const Qty fill = std::min(remaining, it->qty);

                    Event trade {};
                    trade.type = EventType::Trade;
                    trade.maker_id = it->id;
                    trade.maker_seq = it->seq;
                    trade.taker_id = id;
                    trade.taker_seq = taker_seq;
                    trade.trade_price_ticks = maker_px;
                    trade.trade_qty = fill;
                    trade.reason = "trade";
                    events.push_back(trade);

                    remaining -= fill;
                    it->qty -= fill;

                    if (it->qty == 0)
                    {
                        const Order filled_maker = *it;

                        index_.erase(filled_maker.id);
                        it = level.erase(it);

                        remove_filled_maker(events, filled_maker);
                    }
                    else
                    {
                        ++it;
                    }
                }

                if (level.empty())
                {
                    bids_.erase(lvl_it);
                }
            }
        }

        if (remaining > 0)
        {
            // taker rests remaining qty at its own limit price
            Order o {};
            o.id = id;
            o.side = side;
            o.price_ticks = price_ticks;
            o.qty = remaining;
            o.seq = taker_seq;

            if (side == Side::Buy)
            {
                auto [lvl_it, created] = bids_.try_emplace(price_ticks, PriceLevel {});
                PriceLevel& level = lvl_it->second;

                // append to keep fifo for this level
                level.push_back(o);
                auto iter = std::prev(level.end());

                const bool ok = index_.emplace(id, Locator { side, price_ticks, iter }).second;
                assert(ok); // this should always be true

                Event e {};
                e.type = EventType::OrderResting;
                e.id = id;
                e.seq = taker_seq;
                e.side = side;
                e.price_ticks = price_ticks;
                e.qty = qty;
                e.remaining_qty = remaining;
                e.reason = "resting";
                events.push_back(e);
            }
            else
            {
                auto [lvl_it, created] = asks_.try_emplace(price_ticks, PriceLevel {});
                PriceLevel& level = lvl_it->second;

                level.push_back(o);
                auto iter = std::prev(level.end());

                const bool ok = index_.emplace(id, Locator { side, price_ticks, iter }).second;
                assert(ok); // inserton should not fail

                Event e {};
                e.type = EventType::OrderResting;
                e.id = id;
                e.seq = taker_seq;
                e.side = side;
                e.price_ticks = price_ticks;
                e.qty = qty;
                e.remaining_qty = remaining;
                e.reason = "resting";
                events.push_back(e);
            }
        }
        else
        {
            // taker fully filled immediately
            Event e {};
            e.type = EventType::OrderCompleted;
            e.id = id;
            e.seq = taker_seq;
            e.side = side;
            e.price_ticks = price_ticks;
            e.qty = qty;
            e.remaining_qty = 0;
            e.reason = "filled";
            events.push_back(e);
        }

        assert_invariants();
        return events;
    }

    std::vector<Event> OrderBook::cancel(OrderId id)
    {
        std::vector<Event> events;

        // id zero is invalid input
        if (id == 0)
        {
            Event e {};
            e.type = EventType::CancelRejected;
            e.id = id;
            e.reason = "invalid";
            events.push_back(e);
            return events;
        }

        auto idx_it = index_.find(id);
        if (idx_it == index_.end())
        {
            Event e {};
            e.type = EventType::CancelRejected;
            e.id = id;
            e.reason = "not_found";
            events.push_back(e);
            return events;
        }

        const Locator loc = idx_it->second;

        // capture state before erase
        const Order snapshot = *loc.it;

        if (loc.side == Side::Buy)
        {
            auto lvl_it = bids_.find(loc.price_ticks);
            assert(lvl_it != bids_.end());

            PriceLevel& level = lvl_it->second;
            level.erase(loc.it);

            if (level.empty())
            {
                bids_.erase(lvl_it);
            }
        }
        else
        {
            auto lvl_it = asks_.find(loc.price_ticks);
            assert(lvl_it != asks_.end());

            PriceLevel& level = lvl_it->second;
            level.erase(loc.it);

            if (level.empty())
            {
                asks_.erase(lvl_it);
            }
        }

        index_.erase(idx_it);

        // cancellation event reports remaining qty that was removed
        Event e {};
        e.type = EventType::OrderCancelled;
        e.id = snapshot.id;
        e.seq = snapshot.seq;
        e.side = snapshot.side;
        e.price_ticks = snapshot.price_ticks;
        e.qty = snapshot.qty;
        e.remaining_qty = 0;
        e.reason = "cancelled";
        events.push_back(e);

        assert_invariants();
        return events;
    }

    std::size_t OrderBook::live_order_count() const
    {
        return index_.size();
    }

    bool OrderBook::has_order(OrderId id) const
    {
        return index_.find(id) != index_.end();
    }

    std::optional<PriceTicks> OrderBook::best_bid_price() const
    {
        // best bid is first key in bids map
        if (bids_.empty())
        {
            return std::nullopt;
        }
        return bids_.begin()->first;
    }

    std::optional<PriceTicks> OrderBook::best_ask_price() const
    {
        // best ask is first key in asks map
        if (asks_.empty())
        {
            return std::nullopt;
        }
        return asks_.begin()->first;
    }

    std::vector<OrderId> OrderBook::order_ids_at(Side side, PriceTicks price_ticks) const
    {
        // returns ids in fifo order at the exact level
        std::vector<OrderId> out_ids;

        if (side == Side::Buy)
        {
            auto it = bids_.find(price_ticks);
            if (it == bids_.end())
            {
                return out_ids;
            }

            const PriceLevel& level = it->second;
            out_ids.reserve(level.size());

            for (const auto& o : level)
            {
                out_ids.push_back(o.id);
            }
            return out_ids;
        }

        auto it = asks_.find(price_ticks);
        if (it == asks_.end())
        {
            return out_ids;
        }

        const PriceLevel& level = it->second;
        out_ids.reserve(level.size());

        for (const auto& o : level)
        {
            out_ids.push_back(o.id);
        }
        return out_ids;
    }

    Qty OrderBook::total_qty_at(Side side, PriceTicks price_ticks) const
    {
        // sums remaining qty at a level
        Qty total { 0 };

        if (side == Side::Buy)
        {
            auto it = bids_.find(price_ticks);
            if (it == bids_.end())
            {
                return 0;
            }

            for (const auto& o : it->second)
            {
                total += o.qty;
            }
            return total;
        }

        auto it = asks_.find(price_ticks);
        if (it == asks_.end())
        {
            return 0;
        }

        for (const auto& o : it->second)
        {
            total += o.qty;
        }
        return total;
    }
}
