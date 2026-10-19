#pragma once
// The Hub connects the engine threads to the WebSocket sessions.
// Engine threads call the EventSink methods; the Hub turns events into JSON
// and fans them out: private acks to one client, market data to everyone.

#include "engine/events.hpp"
#include "engine/exchange.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <nlohmann/json.hpp>

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nx {

class WsSession;
using SharedMsg = std::shared_ptr<const std::string>;

class Hub : public EventSink {
public:
    explicit Hub(boost::asio::io_context& ioc) : ioc_(ioc), statsTimer_(ioc), flushTimer_(ioc) {}

    void join(const std::shared_ptr<WsSession>& s);
    void leave(ClientId id);
    void sendTo(ClientId id, SharedMsg msg);
    void broadcast(SharedMsg msg);
    std::size_t clients() const;

    // Latest book of every symbol + recent trades, sent to a new client on connect.
    std::vector<SharedMsg> welcomeMessages() const;

    // Broadcast throughput / latency stats once per second, and flush batched
    // trades every 50 ms.
    void startStats(Exchange& ex);
    void stopStats() {
        statsTimer_.cancel();
        flushTimer_.cancel();
    }

    std::uint64_t messagesOut() const { return messagesOut_.load(std::memory_order_relaxed); }
    std::uint64_t slowConsumerDrops() const { return slowDrops_.load(std::memory_order_relaxed); }
    void countSlowDrop() { slowDrops_.fetch_add(1, std::memory_order_relaxed); }

    // EventSink (called from engine threads)
    void onOrderResult(const std::string& symbol, const SubmitResult& r, const std::string& clOrdId) override;
    void onCancelResult(const std::string& symbol, ClientId client, OrderId id, bool ok,
                        const std::string& clOrdId) override;
    void onCancelAll(const std::string& symbol, ClientId client, std::size_t count) override;
    void onTrades(const std::string& symbol, const std::vector<Trade>& trades) override;
    void onBook(const std::string& symbol, const BookSnapshot& snap) override;

private:
    void scheduleStats();
    void scheduleFlush();

    boost::asio::io_context&                              ioc_;
    boost::asio::steady_timer                             statsTimer_;
    boost::asio::steady_timer                             flushTimer_;
    // Trades are not sent one message per order: they are collected here and
    // flushed as one message per symbol every 50 ms (market-data conflation).
    // That turns N trades x M clients messages into (symbols x M) per tick.
    std::map<std::string, std::deque<nlohmann::json>>     pendingTrades_;
    Exchange*                                             exchange_ = nullptr;

    mutable std::mutex                                    m_;
    std::unordered_map<ClientId, std::weak_ptr<WsSession>> sessions_;
    std::map<std::string, SharedMsg>                      latestBook_;
    std::map<std::string, std::deque<nlohmann::json>>     recentTrades_;

    std::atomic<std::uint64_t>                            messagesOut_{0};
    std::atomic<std::uint64_t>                            slowDrops_{0};

    // previous totals for per-second rates
    std::uint64_t                                         prevOrders_ = 0, prevTrades_ = 0;
    std::array<std::uint64_t, LatencyHistogram::kBuckets> prevLatency_{};
};

}  // namespace nx
