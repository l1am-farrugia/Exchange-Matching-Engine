#pragma once

#include "event.h"
#include "order.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ob
{
    // order book stores resting orders grouped by side and price
    class OrderBook
    {
    public:

        OrderBook();
        ~OrderBook();

        OrderBook(const OrderBook&) = delete;
        OrderBook& operator=(const OrderBook&) = delete;

        // applies an add limit and emits events for accept trades and final state
        void add_limit(OrderId id, Side side, PriceTicks price_ticks, Qty qty, std::vector<Event>& out_events);

        // applies a cancel and emits cancelled or rejected
        void cancel(OrderId id, std::vector<Event>& out_events);

        // resets the book state to reuse memory
        void reset(); 

        // number of live resting orders
        std::size_t live_order_count() const;

        // quick membership check
        bool has_order(OrderId id) const;

        // best bid and ask prices if present
        std::optional<PriceTicks> best_bid_price() const;
        std::optional<PriceTicks> best_ask_price() const;

        // ids at a specific level in fifo order
        std::vector<OrderId> order_ids_at(Side side, PriceTicks price_ticks) const;

        // total qty at a level
        Qty total_qty_at(Side side, PriceTicks price_ticks) const;

    private:

        // Memory pool state
        std::vector<Order*> blocks_;
        Order* free_list_{nullptr};

        Order* allocate_order();
        void free_order(Order* o);

        // a price level holds fifo orders
        struct IntrusivePriceLevel
        {
            Order* head { nullptr };
            Order* tail { nullptr };
            std::size_t count { 0 };

            bool empty() const { return head == nullptr; }
            std::size_t size() const { return count; }
        };

        using PriceLevel = IntrusivePriceLevel;

        // locator points to an exact stored order
        struct Locator
        {
            Side side { Side::Buy };
            PriceTicks price_ticks { 0 };
            Order* order_ptr { nullptr };
        };

        // assigns the next seq value
        std::uint64_t next_seq_ { 1 };

        static constexpr std::int64_t MAX_TICKS = 1000000;

        // O(1) level lookups from direct mapping 
        std::vector<PriceLevel> bids_;
        std::vector<PriceLevel> asks_;
        
        // track best prices and jump directly to the top of the book
        PriceTicks best_bid_ { -1 };
        PriceTicks best_ask_ { MAX_TICKS };

        std::size_t active_bid_count_ { 0 };
        std::size_t active_ask_count_ { 0 };

        // id index for fast cancel and direct access
        std::unordered_map<OrderId, Locator> index_;

        // helper for matching cross condition
        bool crosses(Side taker_side, PriceTicks taker_px, PriceTicks maker_px) const;

        // maker completion event helper
        void remove_filled_maker(std::vector<Event>& events, const Order& maker);

        // invariants and sanity checks
        std::size_t recompute_live_count() const;
        void assert_invariants() const;

        // 1000000 ticks / 64 = 15625 words
        static constexpr std::size_t MASK_SIZE = (MAX_TICKS + 63) / 64;
        // 15625 words / 64 = 245 summary words
        static constexpr std::size_t SUMMARY_MASK_SIZE = (MASK_SIZE + 63) / 64;

        std::uint64_t bid_mask_[MASK_SIZE];
        std::uint64_t bid_summary_mask_[SUMMARY_MASK_SIZE];
        
        std::uint64_t ask_mask_[MASK_SIZE];
        std::uint64_t ask_summary_mask_[SUMMARY_MASK_SIZE];

        // flips bit on when a level gets its first order
        inline void set_ask_bit(std::int64_t price) 
        {
            std::size_t word_idx = price / 64;
            ask_mask_[word_idx] |= (1ULL << (price % 64));
            ask_summary_mask_[word_idx / 64] |= (1ULL << (word_idx % 64));
        }

        // flips bit off when a level becomes empty
        inline void clear_ask_bit(std::int64_t price) 
        {
            std::size_t word_idx = price / 64;
            ask_mask_[word_idx] &= ~(1ULL << (price % 64));
            if (ask_mask_[word_idx] == 0) 
            {
                ask_summary_mask_[word_idx / 64] &= ~(1ULL << (word_idx % 64));
            }
        }

        // flibs on for bids
        inline void set_bid_bit(std::int64_t price) 
        {
            std::size_t word_idx = price / 64;
            bid_mask_[word_idx] |= (1ULL << (price % 64));
            bid_summary_mask_[word_idx / 64] |= (1ULL << (word_idx % 64));
        }

        // flips off for bids
        inline void clear_bid_bit(std::int64_t price) 
        {
            std::size_t word_idx = price / 64;
            bid_mask_[word_idx] &= ~(1ULL << (price % 64));

            if (bid_mask_[word_idx] == 0) 
            {
                bid_summary_mask_[word_idx / 64] &= ~(1ULL << (word_idx % 64));
            }
        }

        // search for next populated ask pric
        inline std::int64_t get_next_ask(std::int64_t start_price) const;
        
        // searches the highest first for the next populated bid price
        inline std::int64_t get_next_bid(std::int64_t start_price) const;

    };
}