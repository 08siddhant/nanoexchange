#pragma once
// Limit order book for one symbol with price-time priority.
//
// Data structure choices (good interview talking points):
//   * std::map<Price, Level>  -> price levels kept sorted, best price is begin().
//                                Bids sorted descending, asks ascending.
//   * std::list<Order> per level -> FIFO queue = time priority. List iterators
//                                stay valid on insert/erase, so we can cancel in O(1).
//   * unordered_map<OrderId, Loc> -> O(1) lookup of any resting order for cancel.
//
// The book is NOT thread-safe on purpose: each symbol is owned by exactly one
// engine thread, so no locks are needed on the hot path.

#include "engine/types.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nx {

struct LevelView {
    Price       price  = 0;
    Qty         qty    = 0;
    std::size_t orders = 0;
};

struct SubmitResult {
    std::vector<Trade> trades;
    Qty                filled = 0;
    Order              final;  // order state after matching (remaining etc.)
    OrderStatus        status = OrderStatus::Unfilled;
};

class OrderBook {
public:
    // Match an incoming order. Leftover quantity of a limit order rests in the
    // book; leftover quantity of a market order is discarded.
    SubmitResult submit(Order order);

    // Cancel a resting order. Only the owning client may cancel it.
    std::optional<Order> cancel(OrderId id, ClientId requester);

    // Cancel every resting order of a client. Returns the cancelled ids.
    std::vector<OrderId> cancelAll(ClientId client);

    // Insert an order straight into the book without matching. Used when a
    // snapshot is loaded during crash recovery.
    void restore(const Order& order);

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    std::vector<LevelView> depth(Side side, std::size_t levels) const;
    std::size_t orderCount() const { return index_.size(); }
    const Order* find(OrderId id) const;

    // Visit all resting orders in priority order (bids best->worst, then asks
    // best->worst, FIFO inside a level). Restoring them in this order keeps
    // time priority intact.
    void forEachOrder(const std::function<void(const Order&)>& fn) const;

    std::uint64_t tradeSeq() const { return tradeSeq_; }
    void setTradeSeq(std::uint64_t s) { tradeSeq_ = s; }

private:
    struct Level {
        std::list<Order> orders;
        Qty              total = 0;
    };
    using Bids = std::map<Price, Level, std::greater<>>;
    using Asks = std::map<Price, Level, std::less<>>;

    struct Loc {
        Side                       side;
        Price                      price;
        std::list<Order>::iterator it;
    };

    template <class Book>
    void matchAgainst(Order& in, Book& book, SubmitResult& result);

    template <class Book>
    void removeResting(Book& book, const Loc& loc);

    void rest(const Order& order);
    void forget(const Order& order);

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Loc>                            index_;
    std::unordered_map<ClientId, std::unordered_set<OrderId>>   byClient_;
    std::uint64_t                                               tradeSeq_ = 0;
};

}  // namespace nx
