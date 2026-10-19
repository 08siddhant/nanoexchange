#include "engine/order_book.hpp"

#include <algorithm>

namespace nx {

namespace {
// Does an incoming limit order cross the given resting price?
bool crosses(const Order& in, Price restingPrice) {
    if (in.type == OrderType::Market) return true;
    return in.side == Side::Buy ? in.price >= restingPrice : in.price <= restingPrice;
}
}  // namespace

template <class Book>
void OrderBook::matchAgainst(Order& in, Book& book, SubmitResult& result) {
    while (in.remaining > 0 && !book.empty()) {
        auto levelIt = book.begin();  // best opposite price
        if (!crosses(in, levelIt->first)) break;

        Level& level = levelIt->second;
        while (in.remaining > 0 && !level.orders.empty()) {
            Order& resting = level.orders.front();  // oldest order = time priority
            const Qty fill = std::min(in.remaining, resting.remaining);

            Trade t;
            t.seq       = ++tradeSeq_;
            t.price     = levelIt->first;  // trade at resting order's price
            t.qty       = fill;
            t.aggressor = in.side;
            if (in.side == Side::Buy) {
                t.buyId = in.id;       t.buyClient = in.client;
                t.sellId = resting.id; t.sellClient = resting.client;
            } else {
                t.sellId = in.id;     t.sellClient = in.client;
                t.buyId = resting.id; t.buyClient = resting.client;
            }
            result.trades.push_back(t);

            in.remaining      -= fill;
            resting.remaining -= fill;
            level.total       -= fill;
            result.filled     += fill;

            if (resting.remaining == 0) {
                forget(resting);
                level.orders.pop_front();
            }
        }
        if (level.orders.empty()) book.erase(levelIt);
    }
}

SubmitResult OrderBook::submit(Order order) {
    SubmitResult result;
    order.remaining = order.qty;

    if (order.side == Side::Buy) matchAgainst(order, asks_, result);
    else                         matchAgainst(order, bids_, result);

    if (order.remaining > 0 && order.type == OrderType::Limit) {
        rest(order);
        result.status = result.filled > 0 ? OrderStatus::PartiallyFilled : OrderStatus::Resting;
    } else if (order.remaining == 0) {
        result.status = OrderStatus::Filled;
    } else {
        // Market order with leftover: the rest is dropped (no liquidity).
        result.status = result.filled > 0 ? OrderStatus::PartiallyFilled : OrderStatus::Unfilled;
    }
    result.final = order;
    return result;
}

void OrderBook::rest(const Order& order) {
    std::list<Order>::iterator it;
    if (order.side == Side::Buy) {
        Level& lvl = bids_[order.price];
        it = lvl.orders.insert(lvl.orders.end(), order);
        lvl.total += order.remaining;
    } else {
        Level& lvl = asks_[order.price];
        it = lvl.orders.insert(lvl.orders.end(), order);
        lvl.total += order.remaining;
    }
    index_[order.id] = Loc{order.side, order.price, it};
    byClient_[order.client].insert(order.id);
}

void OrderBook::restore(const Order& order) {
    if (order.remaining <= 0 || index_.count(order.id)) return;
    rest(order);
}

void OrderBook::forget(const Order& order) {
    index_.erase(order.id);
    auto it = byClient_.find(order.client);
    if (it != byClient_.end()) {
        it->second.erase(order.id);
        if (it->second.empty()) byClient_.erase(it);
    }
}

template <class Book>
void OrderBook::removeResting(Book& book, const Loc& loc) {
    auto levelIt = book.find(loc.price);
    Level& level = levelIt->second;
    level.total -= loc.it->remaining;
    level.orders.erase(loc.it);
    if (level.orders.empty()) book.erase(levelIt);
}

std::optional<Order> OrderBook::cancel(OrderId id, ClientId requester) {
    auto found = index_.find(id);
    if (found == index_.end()) return std::nullopt;
    const Loc loc = found->second;
    if (loc.it->client != requester) return std::nullopt;  // not your order

    Order copy = *loc.it;
    forget(copy);
    if (loc.side == Side::Buy) removeResting(bids_, loc);
    else                       removeResting(asks_, loc);
    return copy;
}

std::vector<OrderId> OrderBook::cancelAll(ClientId client) {
    std::vector<OrderId> ids;
    auto it = byClient_.find(client);
    if (it == byClient_.end()) return ids;
    ids.assign(it->second.begin(), it->second.end());
    std::sort(ids.begin(), ids.end());  // deterministic order
    for (OrderId id : ids) cancel(id, client);
    return ids;
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::vector<LevelView> OrderBook::depth(Side side, std::size_t levels) const {
    std::vector<LevelView> out;
    auto collect = [&](const auto& book) {
        for (const auto& [price, lvl] : book) {
            if (out.size() >= levels) break;
            out.push_back({price, lvl.total, lvl.orders.size()});
        }
    };
    if (side == Side::Buy) collect(bids_);
    else                   collect(asks_);
    return out;
}

const Order* OrderBook::find(OrderId id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &*it->second.it;
}

void OrderBook::forEachOrder(const std::function<void(const Order&)>& fn) const {
    for (const auto& [_, lvl] : bids_)
        for (const auto& o : lvl.orders) fn(o);
    for (const auto& [_, lvl] : asks_)
        for (const auto& o : lvl.orders) fn(o);
}

}  // namespace nx
