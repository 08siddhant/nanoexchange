#pragma once
// Commands flow into the engine; events flow out through an EventSink.
// The engine knows nothing about WebSockets or JSON - the server implements
// EventSink, and the unit tests use a recording sink.

#include "engine/order_book.hpp"
#include "engine/types.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace nx {

struct Command {
    enum class Kind { New, Cancel, CancelAll, Stop };
    Kind        kind = Kind::New;
    ClientId    client = 0;
    Side        side = Side::Buy;
    OrderType   type = OrderType::Limit;
    Price       price = 0;
    Qty         qty = 0;
    OrderId     cancelId = 0;
    std::string clOrdId;  // client's own reference, echoed back in acks
    std::chrono::steady_clock::time_point received = std::chrono::steady_clock::now();
};

struct BookSnapshot {
    std::vector<LevelView> bids;
    std::vector<LevelView> asks;
    std::optional<Price>   lastPrice;
    std::uint64_t          volume = 0;
    std::size_t            orders = 0;
};

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void onOrderResult(const std::string& /*symbol*/, const SubmitResult& /*r*/, const std::string& /*clOrdId*/) {}
    virtual void onCancelResult(const std::string& /*symbol*/, ClientId /*client*/, OrderId /*id*/, bool /*ok*/,
                                const std::string& /*clOrdId*/) {}
    virtual void onCancelAll(const std::string& /*symbol*/, ClientId /*client*/, std::size_t /*count*/) {}
    virtual void onTrades(const std::string& /*symbol*/, const std::vector<Trade>& /*trades*/) {}
    virtual void onBook(const std::string& /*symbol*/, const BookSnapshot& /*snap*/) {}
};

}  // namespace nx
