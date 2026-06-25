#include "engine.h"

#include <gtest/gtest.h>

TEST(Engine, AssignsMonotonicSeq)
{
    // verify sequencing is owned by the book not the caller
    ob::Engine eng;

    std::vector<ob::Event> e1;
    std::vector<ob::Event> e2;

    eng.apply(ob::Command::add_limit(1, ob::Side::Buy, 10'000, 100), e1);
    eng.apply(ob::Command::add_limit(2, ob::Side::Buy, 10'000, 100), e2);

    ASSERT_EQ(e1.size(), 1u);
    ASSERT_EQ(e2.size(), 1u);

    EXPECT_EQ(e1[0].type, ob::EventType::OrderAccepted);
    EXPECT_EQ(e2[0].type, ob::EventType::OrderAccepted);

    EXPECT_EQ(e1[0].seq, 1u);
    EXPECT_EQ(e2[0].seq, 2u);
}

TEST(Engine, BestPricesReflectBookState)
{
    // best bid is highest price, best ask is lowest price
    ob::Engine eng;
    std::vector<ob::Event> events;

    EXPECT_FALSE(eng.book().best_bid_price().has_value());
    EXPECT_FALSE(eng.book().best_ask_price().has_value());

    eng.apply(ob::Command::add_limit(1, ob::Side::Buy, 10'000, 100), events);
    eng.apply(ob::Command::add_limit(2, ob::Side::Buy, 10'050, 100), events);
    eng.apply(ob::Command::add_limit(3, ob::Side::Sell, 10'200, 100), events);
    eng.apply(ob::Command::add_limit(4, ob::Side::Sell, 10'150, 100), events);

    ASSERT_TRUE(eng.book().best_bid_price().has_value());
    ASSERT_TRUE(eng.book().best_ask_price().has_value());

    EXPECT_EQ(*eng.book().best_bid_price(), 10'050);
    EXPECT_EQ(*eng.book().best_ask_price(), 10'150);
}

TEST(Engine, PreservesFifoWithinPriceLevel)
{
    // orders at the same price should remain fifo
    ob::Engine eng;

    std::vector<ob::Event> events;

    eng.apply(ob::Command::add_limit(10, ob::Side::Buy, 10'000, 1), events);
    eng.apply(ob::Command::add_limit(11, ob::Side::Buy, 10'000, 1), events);
    eng.apply(ob::Command::add_limit(12, ob::Side::Buy, 10'000, 1), events);

    const auto ids = eng.book().order_ids_at(ob::Side::Buy, 10'000);
    ASSERT_EQ(ids.size(), 3u);

    EXPECT_EQ(ids[0], 10u);
    EXPECT_EQ(ids[1], 11u);
    EXPECT_EQ(ids[2], 12u);
}

TEST(Engine, CancelRemovesExactOrderAndCleansLevel)
{
    // cancel should remove only that order and clean empty levels
    ob::Engine eng;

    std::vector<ob::Event> events;

    eng.apply(ob::Command::add_limit(20, ob::Side::Buy, 10'000, 1), events);
    eng.apply(ob::Command::add_limit(21, ob::Side::Buy, 10'000, 1), events);
    eng.apply(ob::Command::add_limit(22, ob::Side::Buy, 10'000, 1), events);

    eng.apply(ob::Command::cancel(21), events);

    const auto ids = eng.book().order_ids_at(ob::Side::Buy, 10'000);
    ASSERT_EQ(ids.size(), 2u);

    EXPECT_EQ(ids[0], 20u);
    EXPECT_EQ(ids[1], 22u);

    eng.apply(ob::Command::cancel(20));
    eng.apply(ob::Command::cancel(22));

    // now that level should be gone
    const auto ids2 = eng.book().order_ids_at(ob::Side::Buy, 10'000);
    EXPECT_TRUE(ids2.empty());

    // No bids left so best bid should be empty now
    EXPECT_FALSE(eng.book().best_bid_price().has_value());
}

TEST(Engine, RejectsDuplicateId)
{
    // duplicate ids should be rejected consistently
    ob::Engine eng;

    std::vector<ob::Event> e1;
    std::vector<ob::Event> e2;

    eng.apply(ob::Command::add_limit(1, ob::Side::Buy, 10'000, 100), e1);
    eng.apply(ob::Command::add_limit(1, ob::Side::Sell, 10'001, 50), e2);

    ASSERT_EQ(e1.size(), 1u);
    ASSERT_EQ(e2.size(), 1u);

    EXPECT_EQ(e1[0].type, ob::EventType::OrderAccepted);
    EXPECT_EQ(e2[0].type, ob::EventType::OrderRejected);
    EXPECT_EQ(e2[0].reason, "duplicate_id");
}

TEST(Engine, CancelReturnsOriginalSeq)
{
    // cancel should report the original seq from acceptance
    ob::Engine eng;

    std::vector<ob::Event> add;
    std::vector<ob::Event> c1;
    std::vector<ob::Event> c2;

    eng.apply(ob::Command::add_limit(7, ob::Side::Buy, 10'000, 100), add);
    ASSERT_EQ(add.size(), 1u);
    EXPECT_EQ(add[0].type, ob::EventType::OrderAccepted);

    const std::uint64_t seq = add[0].seq;

    eng.apply(ob::Command::cancel(7), c1);
    ASSERT_EQ(c1.size(), 1u);
    EXPECT_EQ(c1[0].type, ob::EventType::OrderCancelled);
    EXPECT_EQ(c1[0].seq, seq);

    eng.apply(ob::Command::cancel(7), c2);
    ASSERT_EQ(c2.size(), 1u);
    EXPECT_EQ(c2[0].type, ob::EventType::CancelRejected);
    EXPECT_EQ(c2[0].reason, "not_found");
}

TEST(Engine, RejectsInvalidInputs)
{
    // invalid inputs should reject with a stable reason
    ob::Engine eng;

    std::vector<ob::Event> e1;
    std::vector<ob::Event> e2;
    std::vector<ob::Event> e3;

    eng.apply(ob::Command::add_limit(0, ob::Side::Buy, 10'000, 100), e1);
    eng.apply(ob::Command::add_limit(1, ob::Side::Buy, 0, 100), e2);
    eng.apply(ob::Command::add_limit(1, ob::Side::Buy, 10'000, 0), e3)

    ASSERT_EQ(e1.size(), 1u);
    ASSERT_EQ(e2.size(), 1u);
    ASSERT_EQ(e3.size(), 1u);

    EXPECT_EQ(e1[0].type, ob::EventType::OrderRejected);
    EXPECT_EQ(e2[0].type, ob::EventType::OrderRejected);
    EXPECT_EQ(e3[0].type, ob::EventType::OrderRejected);

    EXPECT_EQ(e1[0].reason, "invalid");
    EXPECT_EQ(e2[0].reason, "invalid");
    EXPECT_EQ(e3[0].reason, "invalid");
}
