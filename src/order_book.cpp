#include "order_book.h"

#include <algorithm>
#include <cassert>

namespace ob
{
    OrderBook::OrderBook() : bids_(MAX_TICKS), asks_(MAX_TICKS) {}

    OrderBook::~OrderBook()
    {
        for (Order* block : blocks_) {
            delete[] block;
        }
    }

    Order* OrderBook::allocate_order()
    {
        if (free_list_ != nullptr)
        {
            Order* o = free_list_;
            free_list_ = o->next;
            return o;
        }

        constexpr std::size_t BLOCK_SIZE = 4096;
        Order* block = new Order[BLOCK_SIZE];
        blocks_.push_back(block);

        for (std::size_t i = 0; i < BLOCK_SIZE - 1; ++i)
        {
            block[i].next = &block[i + 1];
        }
        block[BLOCK_SIZE - 1].next = nullptr;

        free_list_ = block->next;
        return block;
    }

    void OrderBook::free_order(Order* o)
    {
        o->next = free_list_;
        free_list_ = o;
    }

    // Determines if a resting maker order meets the taker's price limit requirement
    bool OrderBook::crosses(Side taker_side, PriceTicks taker_px, PriceTicks maker_px) const
    {
        if (taker_side == Side::Buy)
        {
            return maker_px <= taker_px;
        }
        return maker_px >= taker_px;
    }

    // Emits a MakerCompleted event when a resting order is fully exhausted by a trade
    void OrderBook::remove_filled_maker(std::vector<Event>& out_events, const Order& maker)
    {
        Event e {};
        e.type = EventType::MakerCompleted;
        e.id = maker.id;
        e.seq = maker.seq;
        e.side = maker.side;
        e.price_ticks = maker.price_ticks;
        e.qty = 0;
        e.remaining_qty = 0;
        e.reason = "filled";
        out_events.push_back(e);
    }

    // Aggregates count from all price levels to verify against the index
    std::size_t OrderBook::recompute_live_count() const
    {
        std::size_t total { 0 };
        for (std::size_t i = 0; i < bids_.size(); ++i)
        {
            total += bids_[i].size();
        }
        for (std::size_t i = 0; i < asks_.size(); ++i)
        {
            total += asks_[i].size();
        }
        return total;
    }

    // Validates system state consistency between containers and indices
    void OrderBook::assert_invariants() const
    {
        assert(index_.size() == recompute_live_count());

        for (std::size_t i = 0; i < bids_.size(); ++i)
        {
            if (bids_[i].empty()) continue;
            for (Order* o = bids_[i].head; o != nullptr; o = o->next)
            {
                assert(o->side == Side::Buy);
                assert(o->price_ticks == static_cast<PriceTicks>(i));
                assert(o->qty > 0);
                assert(o->seq != 0);

                const auto it = index_.find(o->id);
                assert(it != index_.end());

                assert(it->second.side == Side::Buy);
                assert(it->second.price_ticks == static_cast<PriceTicks>(i));
                assert(it->second.order_ptr->id == o->id);
            }
        }

        for (std::size_t i = 0; i < asks_.size(); ++i)
        {
            if (asks_[i].empty()) continue;
            for (Order* o = asks_[i].head; o != nullptr; o = o->next)
            {
                assert(o->side == Side::Sell);
                assert(o->price_ticks == static_cast<PriceTicks>(i));
                assert(o->qty > 0);
                assert(o->seq != 0);

                const auto it = index_.find(o->id);
                assert(it != index_.end());

                assert(it->second.side == Side::Sell);
                assert(it->second.price_ticks == static_cast<PriceTicks>(i));
                assert(it->second.order_ptr->id == o->id);
            }
        }
    }

    // Core matching logic for new orders
    void OrderBook::add_limit(OrderId id, Side side, PriceTicks price_ticks, Qty qty, std::vector<Event>& out_events) 
    {
        if (!is_valid_input(id, price_ticks, qty))
        {
            Event e {};
            e.type = EventType::OrderRejected;
            e.id = id;
            e.side = side;
            e.price_ticks = price_ticks;
            e.qty = qty;
            e.reason = "invalid";
            out_events.push_back(e);
            return;
        }

        if (index_.find(id) != index_.end())
        {
            Event e {};
            e.type = EventType::OrderRejected;
            e.id = id;
            e.side = side;
            e.price_ticks = price_ticks;
            e.qty = qty;
            e.reason = "duplicate_id";
            out_events.push_back(e);
            return;
        }

        // Taker sequence is assigned by the book upon arrival
        const std::uint64_t taker_seq = next_seq_;
        ++next_seq_;

        {
            Event e {};
            e.type = EventType::OrderAccepted;
            e.id = id;
            e.seq = taker_seq;
            e.side = side;
            e.price_ticks = price_ticks;
            e.qty = qty;
            e.reason = "accepted";
            out_events.push_back(e);
        }

        Qty remaining = qty;

        if (side == Side::Buy)
        {
            // Traverse asks to find matching liquidity
            while (remaining > 0 && best_ask_ <= price_ticks && best_ask_ < MAX_TICKS)
            {
                PriceLevel& level = asks_[best_ask_];
                if (level.empty())
                {
                    best_ask_++;
                    continue;
                }

                const PriceTicks maker_px = best_ask_;
                Order* it = level.head;

                while (remaining > 0 && it != nullptr)
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
                    out_events.push_back(trade);

                    remaining -= fill;
                    it->qty -= fill;

                    Order* next_it = it->next;

                    if (it->qty == 0)
                    {
                        const Order filled_maker = *it;
                        index_.erase(filled_maker.id);

                        // Remove node from intrusive list
                        if (it->prev) it->prev->next = it->next;
                        else level.head = it->next;
                        
                        if (it->next) it->next->prev = it->prev;
                        else level.tail = it->prev;
                        
                        level.count--;
                        active_ask_count_--;

                        free_order(it);
                        remove_filled_maker(out_events, filled_maker);
                    }
                    
                    it = next_it;
                }

                if (level.empty())
                {
                    if (active_ask_count_ == 0) 
                    {
                        best_ask_ = MAX_TICKS;
                    }
                    else 
                    {
                        while (best_ask_ < MAX_TICKS && asks_[best_ask_].empty())
                        {
                            best_ask_++;
                        }
                    }
                }
            }
        }
        else
        {
            // Traverse bids to find matching liquidity
            while (remaining > 0 && best_bid_ >= price_ticks && best_bid_ >= 0)
            {
                PriceLevel& level = bids_[best_bid_];
                if (level.empty())
                {
                    best_bid_--;
                    continue;
                }

                const PriceTicks maker_px = best_bid_;
                Order* it = level.head;

                while (remaining > 0 && it != nullptr)
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
                    out_events.push_back(trade);

                    remaining -= fill;
                    it->qty -= fill;

                    Order* next_it = it->next;

                    if (it->qty == 0)
                    {
                        const Order filled_maker = *it;
                        index_.erase(filled_maker.id);

                        if (it->prev) it->prev->next = it->next;
                        else level.head = it->next;
                        
                        if (it->next) it->next->prev = it->prev;
                        else level.tail = it->prev;
                        
                        level.count--;
                        active_bid_count_--;

                        free_order(it);
                        remove_filled_maker(out_events, filled_maker);
                    }
                    
                    it = next_it;
                }

                if (level.empty())
                {
                    if (active_bid_count_ == 0) 
                    {
                        best_bid_ = -1; 
                    }
                    else 
                    {
                        while (best_bid_ >= 0 && bids_[best_bid_].empty()) 
                        {
                            best_bid_--;
                        }
                    }
                }
            }
        }

        if (remaining > 0)
        {
            // Rest remainder in the book
            Order* o = allocate_order();
            o->id = id;
            o->side = side;
            o->price_ticks = price_ticks;
            o->qty = remaining;
            o->seq = taker_seq;
            o->next = nullptr;
            o->prev = nullptr;

            if (side == Side::Buy)
            {
                PriceLevel& level = bids_[price_ticks];

                // FIFO insertion at tail
                if (level.tail == nullptr)
                {
                    level.head = o;
                    level.tail = o;
                }
                else
                {
                    level.tail->next = o;
                    o->prev = level.tail;
                    level.tail = o;
                }
                level.count++;

                // Track highest bid
                if (price_ticks > best_bid_) best_bid_ = price_ticks;
                active_bid_count_++;

                const bool ok = index_.emplace(id, Locator { side, price_ticks, o }).second;
                assert(ok);

                Event e {};
                e.type = EventType::OrderResting;
                e.id = id;
                e.seq = taker_seq;
                e.side = side;
                e.price_ticks = price_ticks;
                e.qty = qty;
                e.remaining_qty = remaining;
                e.reason = "resting";
                out_events.push_back(e);
            }
            else
            {
                PriceLevel& level = asks_[price_ticks];

                // FIFO insertion at tail
                if (level.tail == nullptr)
                {
                    level.head = o;
                    level.tail = o;
                }
                else
                {
                    level.tail->next = o;
                    o->prev = level.tail;
                    level.tail = o;
                }
                level.count++;

                // Track lowest ask
                if (price_ticks < best_ask_) best_ask_ = price_ticks;\
                active_ask_count_++;

                const bool ok = index_.emplace(id, Locator { side, price_ticks, o }).second;
                assert(ok);

                Event e {};
                e.type = EventType::OrderResting;
                e.id = id;
                e.seq = taker_seq;
                e.side = side;
                e.price_ticks = price_ticks;
                e.qty = qty;
                e.remaining_qty = remaining;
                e.reason = "resting";
                out_events.push_back(e);
            }
        }
        else
        {
            Event e {};
            e.type = EventType::OrderCompleted;
            e.id = id;
            e.seq = taker_seq;
            e.side = side;
            e.price_ticks = price_ticks;
            e.qty = qty;
            e.remaining_qty = 0;
            e.reason = "filled";
            out_events.push_back(e);
        }
    }

    // Handles removal of resting orders from the index and linked list
    void OrderBook::cancel(OrderId id, std::vector<Event>& out_events)
    {
        if (id == 0)
        {
            Event e {};
            e.type = EventType::CancelRejected;
            e.id = id;
            e.reason = "invalid";
            out_events.push_back(e);
            return;
        }

        auto idx_it = index_.find(id);
        if (idx_it == index_.end())
        {
            Event e {};
            e.type = EventType::CancelRejected;
            e.id = id;
            e.reason = "not_found";
            out_events.push_back(e);
            return;
        }

        const Locator loc = idx_it->second;
        Order* it = loc.order_ptr;
        const Order snapshot = *it;

        if (loc.side == Side::Buy)
        {
            PriceLevel& level = bids_[loc.price_ticks];

            // Update head/tail if necessary
            if (it->prev) it->prev->next = it->next;
            else level.head = it->next;
            
            if (it->next) it->next->prev = it->prev;
            else level.tail = it->prev;
            
            level.count--;
            active_bid_count_--;

            // If we removed the best bid, go down to find the next one
            if (active_bid_count_ == 0)
            {
                best_bid_ = -1;
            }
            else if (level.empty() && loc.price_ticks == best_bid_)
            {
                while (best_bid_ >= 0 && bids_[best_bid_].empty())
                {
                    best_bid_--;
                }
            }
        }
        else
        {
            PriceLevel& level = asks_[loc.price_ticks];

            // update head/tail if necessary
            if (it->prev) it->prev->next = it->next;
            else level.head = it->next;
            
            if (it->next) it->next->prev = it->prev;
            else level.tail = it->prev;
            
            level.count--;
            active_ask_count_--;

            // If we removed the best ask, walk up to find the next one
            if (active_ask_count_ == 0)
            {
                best_ask_ = MAX_TICKS;
            }
            else if (level.empty() && loc.price_ticks == best_ask_)
            {
                while (best_ask_ < MAX_TICKS && asks_[best_ask_].empty())
                {
                    best_ask_++;
                }
            }
        }

        index_.erase(idx_it);
        free_order(it); 

        Event e {};
        e.type = EventType::OrderCancelled;
        e.id = snapshot.id;
        e.seq = snapshot.seq;
        e.side = snapshot.side;
        e.price_ticks = snapshot.price_ticks;
        e.qty = snapshot.qty;
        e.remaining_qty = 0;
        e.reason = "cancelled";
        out_events.push_back(e);
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
        if (best_bid_ == -1) return std::nullopt;
        return best_bid_;
    }

    std::optional<PriceTicks> OrderBook::best_ask_price() const
    {
        if (best_ask_ == MAX_TICKS) return std::nullopt;
        return best_ask_;
    }

    std::vector<OrderId> OrderBook::order_ids_at(Side side, PriceTicks price_ticks) const
    {
        std::vector<OrderId> out_ids;

        if (price_ticks < 0 || price_ticks >= MAX_TICKS) return out_ids;

        if (side == Side::Buy)
        {
            const PriceLevel& level = bids_[price_ticks];
            out_ids.reserve(level.size());
            for (Order* o = level.head; o != nullptr; o = o->next)
            {
                out_ids.push_back(o->id);
            }
            return out_ids;
        }

        const PriceLevel& level = asks_[price_ticks];
        out_ids.reserve(level.size());
        for (Order* o = level.head; o != nullptr; o = o->next)
        {
            out_ids.push_back(o->id);
        }
        return out_ids;
    }

    Qty OrderBook::total_qty_at(Side side, PriceTicks price_ticks) const
    {
        Qty total { 0 };

        if (price_ticks < 0 || price_ticks >= MAX_TICKS) return 0;

        if (side == Side::Buy)
        {
            for (Order* o = bids_[price_ticks].head; o != nullptr; o = o->next)
            {
                total += o->qty;
            }
            return total;
        }

        for (Order* o = asks_[price_ticks].head; o != nullptr; o = o->next)
        {
            total += o->qty;
        }
        return total;
    }
}