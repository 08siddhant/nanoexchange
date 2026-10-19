#include "engine/order_book.hpp"

#include <gtest/gtest.h>

using namespace nx;

namespace {
Order limit(OrderId id, Side side, Price price, Qty qty, ClientId client = 1) {
    Order o;
    o.id = id; o.client = client; o.side = side; o.type = OrderType::Limit; o.price = price; o.qty = qty;
    return o;
}
Order market(OrderId id, Side side, Qty qty, ClientId client = 1) {
    Order o;
    o.id = id; o.client = client; o.side = side; o.type = OrderType::Market; o.qty = qty;
    return o;
}
}  // namespace

TEST(OrderBook, NonCrossingOrdersRest) {
    OrderBook b;
    auto r1 = b.submit(limit(1, Side::Buy, 100, 10));
    auto r2 = b.submit(limit(2, Side::Sell, 101, 5));
    EXPECT_EQ(r1.status, OrderStatus::Resting);
    EXPECT_EQ(r2.status, OrderStatus::Resting);
    EXPECT_TRUE(r1.trades.empty());
    EXPECT_EQ(b.bestBid(), 100);
    EXPECT_EQ(b.bestAsk(), 101);
    EXPECT_EQ(b.orderCount(), 2u);
}

TEST(OrderBook, FullFillAtRestingPrice) {
    OrderBook b;
    b.submit(limit(1, Side::Sell, 100, 10, 7));
    auto r = b.submit(limit(2, Side::Buy, 105, 10, 8));  // willing to pay more
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].price, 100);  // price improvement for the aggressor
    EXPECT_EQ(r.trades[0].qty, 10);
    EXPECT_EQ(r.trades[0].buyId, 2u);
    EXPECT_EQ(r.trades[0].sellId, 1u);
    EXPECT_EQ(r.trades[0].sellClient, 7u);
    EXPECT_EQ(r.status, OrderStatus::Filled);
    EXPECT_EQ(b.orderCount(), 0u);
}

TEST(OrderBook, PartialFillRestsRemainder) {
    OrderBook b;
    b.submit(limit(1, Side::Sell, 100, 4));
    auto r = b.submit(limit(2, Side::Buy, 100, 10));
    EXPECT_EQ(r.filled, 4);
    EXPECT_EQ(r.status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(r.final.remaining, 6);
    EXPECT_EQ(b.bestBid(), 100);
    EXPECT_FALSE(b.bestAsk().has_value());
    auto d = b.depth(Side::Buy, 5);
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].qty, 6);
}

TEST(OrderBook, TimePriorityWithinLevel) {
    OrderBook b;
    b.submit(limit(1, Side::Sell, 100, 5));  // first in queue
    b.submit(limit(2, Side::Sell, 100, 5));
    auto r = b.submit(limit(3, Side::Buy, 100, 7));
    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0].sellId, 1u);
    EXPECT_EQ(r.trades[0].qty, 5);
    EXPECT_EQ(r.trades[1].sellId, 2u);
    EXPECT_EQ(r.trades[1].qty, 2);
    ASSERT_NE(b.find(2), nullptr);
    EXPECT_EQ(b.find(2)->remaining, 3);
}

TEST(OrderBook, PricePriorityAcrossLevels) {
    OrderBook b;
    b.submit(limit(1, Side::Sell, 102, 5));
    b.submit(limit(2, Side::Sell, 100, 5));  // better price, arrived later
    b.submit(limit(3, Side::Sell, 101, 5));
    auto r = b.submit(limit(4, Side::Buy, 102, 12));
    ASSERT_EQ(r.trades.size(), 3u);
    EXPECT_EQ(r.trades[0].price, 100);
    EXPECT_EQ(r.trades[1].price, 101);
    EXPECT_EQ(r.trades[2].price, 102);
    EXPECT_EQ(r.trades[2].qty, 2);
}

TEST(OrderBook, LimitDoesNotTradeThroughItsPrice) {
    OrderBook b;
    b.submit(limit(1, Side::Sell, 100, 5));
    b.submit(limit(2, Side::Sell, 103, 5));
    auto r = b.submit(limit(3, Side::Buy, 101, 10));
    EXPECT_EQ(r.filled, 5);
    EXPECT_EQ(b.bestBid(), 101);
    EXPECT_EQ(b.bestAsk(), 103);
}

TEST(OrderBook, MarketOrderSweepsAndDropsRemainder) {
    OrderBook b;
    b.submit(limit(1, Side::Buy, 99, 3));
    b.submit(limit(2, Side::Buy, 98, 3));
    auto r = b.submit(market(3, Side::Sell, 10));
    EXPECT_EQ(r.filled, 6);
    EXPECT_EQ(r.status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(b.orderCount(), 0u);  // market leftovers never rest
}

TEST(OrderBook, MarketOrderOnEmptyBookIsUnfilled) {
    OrderBook b;
    auto r = b.submit(market(1, Side::Buy, 10));
    EXPECT_EQ(r.status, OrderStatus::Unfilled);
    EXPECT_EQ(b.orderCount(), 0u);
}

TEST(OrderBook, CancelRemovesOrderAndEmptyLevel) {
    OrderBook b;
    b.submit(limit(1, Side::Buy, 100, 5, 42));
    auto c = b.cancel(1, 42);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->remaining, 5);
    EXPECT_FALSE(b.bestBid().has_value());
    EXPECT_FALSE(b.cancel(1, 42).has_value());  // already gone
}

TEST(OrderBook, CannotCancelSomeoneElsesOrder) {
    OrderBook b;
    b.submit(limit(1, Side::Buy, 100, 5, 42));
    EXPECT_FALSE(b.cancel(1, 99).has_value());
    EXPECT_EQ(b.orderCount(), 1u);
}

TEST(OrderBook, CancelAllOnlyTouchesThatClient) {
    OrderBook b;
    b.submit(limit(1, Side::Buy, 100, 5, 1));
    b.submit(limit(2, Side::Sell, 105, 5, 1));
    b.submit(limit(3, Side::Buy, 99, 5, 2));
    auto ids = b.cancelAll(1);
    EXPECT_EQ(ids.size(), 2u);
    EXPECT_EQ(b.orderCount(), 1u);
    EXPECT_EQ(b.bestBid(), 99);
}

TEST(OrderBook, CancelMiddleOfQueueKeepsOthersInOrder) {
    OrderBook b;
    b.submit(limit(1, Side::Sell, 100, 1));
    b.submit(limit(2, Side::Sell, 100, 1));
    b.submit(limit(3, Side::Sell, 100, 1));
    ASSERT_TRUE(b.cancel(2, 1).has_value());
    auto r = b.submit(limit(4, Side::Buy, 100, 2));
    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0].sellId, 1u);
    EXPECT_EQ(r.trades[1].sellId, 3u);
}
