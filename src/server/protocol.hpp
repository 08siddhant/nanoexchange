#pragma once
// JSON wire protocol between browser clients and the exchange.
//
// Client -> server
//   {"op":"new","symbol":"AAPL","side":"buy","type":"limit","price":230.15,"qty":10,"clOrdId":"c1"}
//   {"op":"cancel","symbol":"AAPL","id":42}
//   {"op":"cancel_all","symbol":"AAPL"}
//   {"op":"ping"}
//
// Server -> client
//   hello, book, trades, ack, cancel_ack, cancel_all_ack, stats, error, pong

#include "engine/events.hpp"

#include <nlohmann/json.hpp>

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace nx::proto {

enum class Op { New, Cancel, CancelAll, Ping, Invalid };

struct Parsed {
    Op          op = Op::Invalid;
    std::string symbol;
    Command     cmd;
    std::string error;
};

Parsed parseClientMessage(std::string_view text, ClientId client);

double toDecimal(Price ticks);

std::string hello(ClientId client, const std::vector<std::string>& symbols);
std::string book(const std::string& symbol, const BookSnapshot& s);
nlohmann::json tradeObject(const Trade& t, std::int64_t tsMillis);
std::string trades(const std::string& symbol, const std::deque<nlohmann::json>& list, bool initial);
std::string ack(const std::string& symbol, const SubmitResult& r, const std::string& clOrdId);
std::string cancelAck(const std::string& symbol, OrderId id, bool ok, const std::string& clOrdId);
std::string cancelAllAck(const std::string& symbol, std::size_t count);
std::string error(const std::string& message, const std::string& clOrdId = "");

}  // namespace nx::proto
