#include "server/protocol.hpp"

#include <cmath>

namespace nx::proto {

using nlohmann::json;

namespace {
constexpr double kMaxPrice = 1'000'000.0;
constexpr Qty    kMaxQty   = 100'000;
constexpr std::size_t kMaxClOrdId = 32;

json levels(const std::vector<LevelView>& lv) {
    json arr = json::array();
    for (const auto& l : lv) arr.push_back({toDecimal(l.price), l.qty, l.orders});
    return arr;
}
}  // namespace

double toDecimal(Price ticks) { return static_cast<double>(ticks) * kTickSize; }

Parsed parseClientMessage(std::string_view text, ClientId client) {
    Parsed out;
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        out.error = "invalid JSON";
        return out;
    }
    const std::string op = j.value("op", "");
    out.cmd.client = client;
    out.cmd.received = std::chrono::steady_clock::now();

    if (j.contains("clOrdId") && j["clOrdId"].is_string()) {
        out.cmd.clOrdId = j["clOrdId"].get<std::string>();
        if (out.cmd.clOrdId.size() > kMaxClOrdId) out.cmd.clOrdId.resize(kMaxClOrdId);
    }

    if (op == "ping") {
        out.op = Op::Ping;
        return out;
    }

    if (!j.contains("symbol") || !j["symbol"].is_string()) {
        out.error = "missing symbol";
        return out;
    }
    out.symbol = j["symbol"].get<std::string>();

    if (op == "new") {
        const std::string side = j.value("side", "");
        const std::string type = j.value("type", "limit");
        if (side != "buy" && side != "sell") { out.error = "side must be buy or sell"; return out; }
        if (type != "limit" && type != "market") { out.error = "type must be limit or market"; return out; }
        if (!j.contains("qty") || !j["qty"].is_number()) { out.error = "qty required"; return out; }

        const double qty = j["qty"].get<double>();
        if (!(qty >= 1 && qty <= kMaxQty) || qty != std::floor(qty)) {
            out.error = "qty must be a whole number between 1 and 100000";
            return out;
        }
        out.cmd.kind = Command::Kind::New;
        out.cmd.side = side == "buy" ? Side::Buy : Side::Sell;
        out.cmd.type = type == "limit" ? OrderType::Limit : OrderType::Market;
        out.cmd.qty = static_cast<Qty>(qty);

        if (out.cmd.type == OrderType::Limit) {
            if (!j.contains("price") || !j["price"].is_number()) { out.error = "price required"; return out; }
            const double p = j["price"].get<double>();
            if (!(p > 0 && p <= kMaxPrice)) { out.error = "price out of range"; return out; }
            out.cmd.price = static_cast<Price>(std::llround(p / kTickSize));
            if (out.cmd.price <= 0) { out.error = "price below tick size"; return out; }
        }
        out.op = Op::New;
        return out;
    }
    if (op == "cancel") {
        if (!j.contains("id") || !j["id"].is_number_unsigned()) { out.error = "id required"; return out; }
        out.cmd.kind = Command::Kind::Cancel;
        out.cmd.cancelId = j["id"].get<OrderId>();
        out.op = Op::Cancel;
        return out;
    }
    if (op == "cancel_all") {
        out.cmd.kind = Command::Kind::CancelAll;
        out.op = Op::CancelAll;
        return out;
    }
    out.error = "unknown op";
    return out;
}

std::string hello(ClientId client, const std::vector<std::string>& symbols) {
    return json{{"type", "hello"}, {"clientId", client}, {"symbols", symbols}, {"tick", kTickSize}}.dump();
}

std::string book(const std::string& symbol, const BookSnapshot& s) {
    json j{{"type", "book"},
           {"symbol", symbol},
           {"bids", levels(s.bids)},
           {"asks", levels(s.asks)},
           {"volume", s.volume},
           {"orders", s.orders}};
    j["last"] = s.lastPrice ? json(toDecimal(*s.lastPrice)) : json(nullptr);
    return j.dump();
}

json tradeObject(const Trade& t, std::int64_t tsMillis) {
    // Client ids are deliberately NOT published (anonymous market).
    return json{{"seq", t.seq},          {"price", toDecimal(t.price)}, {"qty", t.qty},
                {"side", toString(t.aggressor)}, {"buyId", t.buyId},  {"sellId", t.sellId},
                {"ts", tsMillis}};
}

std::string trades(const std::string& symbol, const std::deque<json>& list, bool initial) {
    json arr = json::array();
    for (const auto& t : list) arr.push_back(t);
    return json{{"type", "trades"}, {"symbol", symbol}, {"initial", initial}, {"trades", arr}}.dump();
}

std::string ack(const std::string& symbol, const SubmitResult& r, const std::string& clOrdId) {
    const Order& o = r.final;
    return json{{"type", "ack"},
                {"symbol", symbol},
                {"id", o.id},
                {"clOrdId", clOrdId},
                {"side", toString(o.side)},
                {"orderType", toString(o.type)},
                {"price", toDecimal(o.price)},
                {"qty", o.qty},
                {"filled", r.filled},
                {"remaining", r.status == OrderStatus::Resting || r.status == OrderStatus::PartiallyFilled
                                  ? (o.type == OrderType::Limit ? o.remaining : 0)
                                  : 0},
                {"status", toString(r.status)},
                {"fills", r.trades.size()}}
        .dump();
}

std::string cancelAck(const std::string& symbol, OrderId id, bool ok, const std::string& clOrdId) {
    return json{{"type", "cancel_ack"}, {"symbol", symbol}, {"id", id}, {"ok", ok}, {"clOrdId", clOrdId}}.dump();
}

std::string cancelAllAck(const std::string& symbol, std::size_t count) {
    return json{{"type", "cancel_all_ack"}, {"symbol", symbol}, {"count", count}}.dump();
}

std::string error(const std::string& message, const std::string& clOrdId) {
    return json{{"type", "error"}, {"message", message}, {"clOrdId", clOrdId}}.dump();
}

}  // namespace nx::proto
