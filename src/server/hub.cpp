#include "server/hub.hpp"

#include "server/protocol.hpp"
#include "server/server.hpp"

#include <chrono>

namespace nx {

namespace {
constexpr std::size_t kRecentTrades = 60;

SharedMsg make(std::string s) { return std::make_shared<const std::string>(std::move(s)); }

std::int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

void Hub::join(const std::shared_ptr<WsSession>& s) {
    std::lock_guard lk(m_);
    sessions_[s->id()] = s;
}

void Hub::leave(ClientId id) {
    std::lock_guard lk(m_);
    sessions_.erase(id);
}

std::size_t Hub::clients() const {
    std::lock_guard lk(m_);
    return sessions_.size();
}

void Hub::sendTo(ClientId id, SharedMsg msg) {
    std::shared_ptr<WsSession> s;
    {
        std::lock_guard lk(m_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return;  // e.g. a bot, or client already gone
        s = it->second.lock();
    }
    if (s) {
        s->send(std::move(msg));
        messagesOut_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Hub::broadcast(SharedMsg msg) {
    // Copy the session list under the lock, send outside it: a slow client
    // can never block an engine thread.
    std::vector<std::shared_ptr<WsSession>> targets;
    {
        std::lock_guard lk(m_);
        targets.reserve(sessions_.size());
        for (auto& [_, w] : sessions_)
            if (auto s = w.lock()) targets.push_back(std::move(s));
    }
    for (auto& s : targets) s->send(msg);  // same shared buffer for everyone, no copies
    messagesOut_.fetch_add(targets.size(), std::memory_order_relaxed);
}

std::vector<SharedMsg> Hub::welcomeMessages() const {
    std::vector<SharedMsg> out;
    std::lock_guard lk(m_);
    for (const auto& [_, b] : latestBook_) out.push_back(b);
    for (const auto& [sym, list] : recentTrades_) out.push_back(make(proto::trades(sym, list, true)));
    return out;
}

void Hub::onOrderResult(const std::string& symbol, const SubmitResult& r, const std::string& clOrdId) {
    sendTo(r.final.client, make(proto::ack(symbol, r, clOrdId)));
}

void Hub::onCancelResult(const std::string& symbol, ClientId client, OrderId id, bool ok, const std::string& clOrdId) {
    sendTo(client, make(proto::cancelAck(symbol, id, ok, clOrdId)));
}

void Hub::onCancelAll(const std::string& symbol, ClientId client, std::size_t count) {
    sendTo(client, make(proto::cancelAllAck(symbol, count)));
}

void Hub::onTrades(const std::string& symbol, const std::vector<Trade>& trades) {
    const auto ts = nowMillis();
    std::deque<nlohmann::json> batch;
    for (const auto& t : trades) batch.push_back(proto::tradeObject(t, ts));
    std::lock_guard lk(m_);
    auto& recent = recentTrades_[symbol];
    auto& pending = pendingTrades_[symbol];
    for (const auto& t : batch) {
        recent.push_back(t);
        pending.push_back(t);
    }
    while (recent.size() > kRecentTrades) recent.pop_front();
}

void Hub::scheduleFlush() {
    flushTimer_.expires_after(std::chrono::milliseconds(50));
    flushTimer_.async_wait([this](boost::system::error_code ec) {
        if (ec) return;
        std::map<std::string, std::deque<nlohmann::json>> out;
        {
            std::lock_guard lk(m_);
            out.swap(pendingTrades_);
        }
        for (auto& [sym, list] : out)
            if (!list.empty()) broadcast(make(proto::trades(sym, list, false)));
        scheduleFlush();
    });
}

void Hub::onBook(const std::string& symbol, const BookSnapshot& snap) {
    auto msg = make(proto::book(symbol, snap));
    {
        std::lock_guard lk(m_);
        latestBook_[symbol] = msg;
    }
    broadcast(msg);
}

void Hub::startStats(Exchange& ex) {
    exchange_ = &ex;
    scheduleStats();
    scheduleFlush();
}

void Hub::scheduleStats() {
    statsTimer_.expires_after(std::chrono::seconds(1));
    statsTimer_.async_wait([this](boost::system::error_code ec) {
        if (ec || !exchange_) return;

        std::uint64_t orders = 0, trades = 0;
        std::int64_t bookOrders = 0;
        std::array<std::uint64_t, LatencyHistogram::kBuckets> lat{};
        for (const auto& sym : exchange_->symbols()) {
            auto& m = exchange_->engine(sym).metrics();
            orders += m.ordersIn.load() + m.cancels.load();
            trades += m.trades.load();
            bookOrders += m.bookOrders.load();
            auto snap = m.latency.snapshot();
            for (std::size_t i = 0; i < lat.size(); ++i) lat[i] += snap[i];
        }
        std::array<std::uint64_t, LatencyHistogram::kBuckets> delta{};
        for (std::size_t i = 0; i < lat.size(); ++i) delta[i] = lat[i] - prevLatency_[i];

        nlohmann::json j{{"type", "stats"},
                         {"ordersPerSec", orders - prevOrders_},
                         {"tradesPerSec", trades - prevTrades_},
                         {"p50us", LatencyHistogram::quantile(delta, 0.50)},
                         {"p99us", LatencyHistogram::quantile(delta, 0.99)},
                         {"clients", clients()},
                         {"bookOrders", bookOrders},
                         {"totalOrders", orders},
                         {"totalTrades", trades}};
        prevOrders_ = orders;
        prevTrades_ = trades;
        prevLatency_ = lat;
        broadcast(make(j.dump()));
        scheduleStats();
    });
}

}  // namespace nx
