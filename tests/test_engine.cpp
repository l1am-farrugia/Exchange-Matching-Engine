#include "engine.h"

#include "event_io.h"

#include <gtest/gtest.h>

static std::vector<std::string> to_lines(const std::vector<ob::Event>& es)
{
    std::vector<std::string> out;
    out.reserve(es.size());

    // convert events to the stable log format
    for (const auto& e : es)
    {
        out.push_back(ob::event_to_line(e));
    }
    return out;
}

TEST(Matching, SimpleCrossTradeAtMakerPrice)
{
    // maker ask then taker buy should trade at ask price
    ob::Engine eng;

    std::vector<ob::Event> ev1;
    std::vector<ob::Event> ev2;

    eng.apply(ob::Command::add_limit(1, ob::Side::Sell, 100, 10), ev1);
    eng.apply(ob::Command::add_limit(2, ob::Side::Buy, 150, 4), ev2);

    bool saw_trade = false;
    for (const auto& e : ev2)
    {
        if (e.type == ob::EventType::Trade)
        {
            saw_trade = true;
            EXPECT_EQ(e.maker_id, 1u);
            EXPECT_EQ(e.taker_id, 2u);
            EXPECT_EQ(e.trade_price_ticks, 100);
            EXPECT_EQ(e.trade_qty, 4);
        }
    }
    EXPECT_TRUE(saw_trade);

    // maker remaining is 6 at ask 100
    EXPECT_EQ(eng.book().total_qty_at(ob::Side::Sell, 100), 6);
    EXPECT_EQ(*eng.book().best_ask_price(), 100);
}

TEST(Matching, FifoWithinLevel)
{
    // two makers at same price should fill in arrival order
    ob::Engine eng;

    std::vector<ob::Event> events;

    eng.apply(ob::Command::add_limit(10, ob::Side::Sell, 100, 5), events);
    eng.apply(ob::Command::add_limit(11, ob::Side::Sell, 100, 5), events);

    eng.apply(ob::Command::add_limit(20, ob::Side::Buy, 100, 6), events);

    EXPECT_FALSE(eng.book().has_order(10));
    EXPECT_TRUE(eng.book().has_order(11));
    EXPECT_EQ(eng.book().total_qty_at(ob::Side::Sell, 100), 4);

    const auto ids = eng.book().order_ids_at(ob::Side::Sell, 100);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 11u);
}

TEST(Matching, MultiLevelSweep)
{
    // taker sweeps multiple ask levels from best ask upward
    ob::Engine eng;

    std::vector<ob::Event> events;

    eng.apply(ob::Command::add_limit(1, ob::Side::Sell, 100, 3), events);
    eng.apply(ob::Command::add_limit(2, ob::Side::Sell, 105, 4), events);
    eng.apply(ob::Command::add_limit(3, ob::Side::Sell, 110, 5), events);

    eng.apply(ob::Command::add_limit(9, ob::Side::Buy, 110, 10), events);
    
    EXPECT_FALSE(eng.book().has_order(1));
    EXPECT_TRUE(eng.book().has_order(3));

    EXPECT_EQ(eng.book().total_qty_at(ob::Side::Sell, 110), 2);
    EXPECT_EQ(*eng.book().best_ask_price(), 110);
}

TEST(Matching, RestingWhenNotCrossing)
{
    // order that does not cross should rest
    ob::Engine eng;
    
    std::vector<ob::Event> events;

    eng.apply(ob::Command::add_limit(1, ob::Side::Buy, 100, 5), events);
    eng.apply(ob::Command::add_limit(2, ob::Side::Sell, 200, 5), events);

    EXPECT_EQ(*eng.book().best_bid_price(), 100);
    EXPECT_EQ(*eng.book().best_ask_price(), 200);

    EXPECT_EQ(eng.book().total_qty_at(ob::Side::Buy, 100), 5);
    EXPECT_EQ(eng.book().total_qty_at(ob::Side::Sell, 200), 5);
}

TEST(Matching, PartialFillThenTakerRestsRemainder)
{
    // taker consumes makers then rests leftover at its limit
    ob::Engine eng;
    
    std::vector<ob::Event> events;

    eng.apply(ob::Command::add_limit(1, ob::Side::Sell, 100, 5), events);
    eng.apply(ob::Command::add_limit(2, ob::Side::Sell, 105, 4), events);

    eng.apply(ob::Command::add_limit(9, ob::Side::Buy, 110, 12), events);

    // should remove both makers and rest 3 at bid 110
    EXPECT_FALSE(eng.book().has_order(1));
    EXPECT_FALSE(eng.book().has_order(2));

    EXPECT_TRUE(eng.book().has_order(9));
    EXPECT_EQ(*eng.book().best_bid_price(), 110);
    EXPECT_EQ(eng.book().total_qty_at(ob::Side::Buy, 110), 3);
}

TEST(Matching, TakerFullyFilledDoesNotRest)
{
    // if taker gets fully filled it should not become a resting order
    ob::Engine eng;

    std::vector<ob::Event> events;

    eng.apply(ob::Command::add_limit(1, ob::Side::Sell, 100, 5), events);
    eng.apply(ob::Command::add_limit(2, ob::Side::Buy, 100, 5), events);

    EXPECT_FALSE(eng.book().has_order(1));
    EXPECT_FALSE(eng.book().has_order(2));

    EXPECT_FALSE(eng.book().best_ask_price().has_value());
}

TEST(Cancel, RemovesExactOrder)
{
    // cancel should remove only the specified order
    ob::Engine eng;

    std::vector<ob::Event> events;

    eng.apply(ob::Command::add_limit(1, ob::Side::Buy, 100, 5), events);
    eng.apply(ob::Command::add_limit(2, ob::Side::Buy, 100, 7), events);
    eng.apply(ob::Command::cancel(1), events);

    EXPECT_FALSE(eng.book().has_order(1));
    EXPECT_TRUE(eng.book().has_order(2));

    const auto ids = eng.book().order_ids_at(ob::Side::Buy, 100);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 2u);

    EXPECT_EQ(eng.book().total_qty_at(ob::Side::Buy, 100), 7);
}

TEST(Cancel, MiddleOfFifoKeepsOrder)
{
    // cancel in the middle should keep fifo order for remaining items
    ob::Engine eng;

    std::vector<ob::Event> events;

    eng.apply(ob::Command::add_limit(10, ob::Side::Sell, 100, 1), events);
    eng.apply(ob::Command::add_limit(11, ob::Side::Sell, 100, 1), events);
    eng.apply(ob::Command::add_limit(12, ob::Side::Sell, 100, 1), events);

    eng.apply(ob::Command::cancel(11), events);

    const auto ids = eng.book().order_ids_at(ob::Side::Sell, 100);
    ASSERT_EQ(ids.size(), 2u);

    EXPECT_EQ(ids[0], 10u);
    EXPECT_EQ(ids[1], 12u);
}

TEST(Cancel, CancelFilledMakerRejectsNotFound)
{
    // once a maker is filled it should not be cancellable
    ob::Engine eng;
    
    std::vector<ob::Event> events;

    eng.apply(ob::Command::add_limit(1, ob::Side::Sell, 100, 3), events);
    eng.apply(ob::Command::add_limit(2, ob::Side::Buy, 100, 3), events);

    EXPECT_FALSE(eng.book().has_order(1));

    std::vector<ob::Event> cancel_events;
    eng.apply(ob::Command::cancel(1), cancel_events);
    ASSERT_EQ(cancel_events.size(), 1u);

    EXPECT_EQ(cancel_events[0].type, ob::EventType::CancelRejected);
    EXPECT_EQ(cancel_events[0].reason, "not_found");
}

TEST(Determinism, SameCommandsSameEvents)
{
    // same script applied twice should produce identical event lines
    std::vector<ob::Command> cmds;
    
    cmds.push_back(ob::Command::add_limit(1, ob::Side::Sell, 100, 5));
    cmds.push_back(ob::Command::add_limit(2, ob::Side::Sell, 100, 5));
    cmds.push_back(ob::Command::add_limit(3, ob::Side::Buy, 100, 6));
    cmds.push_back(ob::Command::cancel(2));
    cmds.push_back(ob::Command::add_limit(4, ob::Side::Buy, 100, 10));

    ob::Engine a;
    ob::Engine b;

    std::vector<ob::Event> ea;
    std::vector<ob::Event> eb;
    a.apply_all(cmds, ea);
    b.apply_all(cmds, eb);

    EXPECT_EQ(to_lines(ea), to_lines(eb));
}

TEST(EventIO, RoundTripTradeEvent)
{
    // event line encoding should parse back for core fields
    ob::Event e {};
    e.type = ob::EventType::Trade;
    e.maker_id = 7;
    e.maker_seq = 3;
    e.taker_id = 9;
    e.taker_seq = 8;
    e.trade_price_ticks = 123;
    e.trade_qty = 4;
    e.reason = "trade";

    const auto line = ob::event_to_line(e);
    const auto parsed = ob::line_to_event(line);

    ASSERT_TRUE(parsed.has_value());

    EXPECT_EQ(parsed->type, ob::EventType::Trade);
    EXPECT_EQ(parsed->maker_id, 7u);
    EXPECT_EQ(parsed->taker_id, 9u);
    EXPECT_EQ(parsed->trade_price_ticks, 123);
    EXPECT_EQ(parsed->trade_qty, 4);
    EXPECT_EQ(parsed->reason, "trade");
}
