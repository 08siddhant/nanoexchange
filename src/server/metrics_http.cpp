#include "server/metrics_http.hpp"

#include <sstream>

namespace nx {

std::string renderPrometheus(Exchange& ex, Hub& hub, ServerContext& ctx) {
    std::ostringstream os;
    auto perSymbol = [&](const char* name, const char* type, const char* help, auto getter) {
        os << "# HELP " << name << ' ' << help << "\n# TYPE " << name << ' ' << type << '\n';
        for (const auto& s : ex.symbols()) os << name << "{symbol=\"" << s << "\"} " << getter(ex.engine(s)) << '\n';
    };

    perSymbol("nx_orders_total", "counter", "New orders processed by the engine",
              [](SymbolEngine& e) { return e.metrics().ordersIn.load(); });
    perSymbol("nx_cancels_total", "counter", "Orders cancelled",
              [](SymbolEngine& e) { return e.metrics().cancels.load(); });
    perSymbol("nx_trades_total", "counter", "Trades executed",
              [](SymbolEngine& e) { return e.metrics().trades.load(); });
    perSymbol("nx_traded_volume_total", "counter", "Total traded quantity",
              [](SymbolEngine& e) { return e.metrics().volume.load(); });
    perSymbol("nx_rejected_busy_total", "counter", "Commands rejected because the engine queue was full",
              [](SymbolEngine& e) { return e.metrics().rejectedBusy.load(); });
    perSymbol("nx_book_orders", "gauge", "Resting orders in the book",
              [](SymbolEngine& e) { return e.metrics().bookOrders.load(); });
    perSymbol("nx_queue_depth", "gauge", "Commands waiting in the engine queue",
              [](SymbolEngine& e) { return e.queueDepth(); });
    perSymbol("nx_wal_bytes", "gauge", "Current write-ahead log size in bytes",
              [](SymbolEngine& e) { return e.metrics().walBytes.load(); });
    perSymbol("nx_snapshots_total", "counter", "Snapshots written (WAL compactions)",
              [](SymbolEngine& e) { return e.metrics().snapshots.load(); });
    perSymbol("nx_recovered_records", "gauge", "WAL records replayed at last startup",
              [](SymbolEngine& e) { return e.metrics().recoveredRecords.load(); });

    // Latency histogram, in seconds as Prometheus convention expects.
    os << "# HELP nx_engine_latency_seconds Time from WebSocket receive to order processed\n"
          "# TYPE nx_engine_latency_seconds histogram\n";
    for (const auto& s : ex.symbols()) {
        auto& h = ex.engine(s).metrics().latency;
        auto counts = h.snapshot();
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < LatencyHistogram::kBounds.size(); ++i) {
            cum += counts[i];
            os << "nx_engine_latency_seconds_bucket{symbol=\"" << s << "\",le=\""
               << LatencyHistogram::kBounds[i] / 1e6 << "\"} " << cum << '\n';
        }
        cum += counts.back();
        os << "nx_engine_latency_seconds_bucket{symbol=\"" << s << "\",le=\"+Inf\"} " << cum << '\n';
        os << "nx_engine_latency_seconds_sum{symbol=\"" << s << "\"} " << h.sumSeconds() << '\n';
        os << "nx_engine_latency_seconds_count{symbol=\"" << s << "\"} " << cum << '\n';
    }

    os << "# HELP nx_ws_clients Connected WebSocket clients\n# TYPE nx_ws_clients gauge\n"
       << "nx_ws_clients " << hub.clients() << '\n';
    os << "# HELP nx_ws_connections_total WebSocket connections accepted\n# TYPE nx_ws_connections_total counter\n"
       << "nx_ws_connections_total " << ctx.connectionsTotal.load() << '\n';
    os << "# HELP nx_ws_messages_in_total Messages received from clients\n# TYPE nx_ws_messages_in_total counter\n"
       << "nx_ws_messages_in_total " << ctx.messagesIn.load() << '\n';
    os << "# HELP nx_ws_messages_out_total Messages sent to clients\n# TYPE nx_ws_messages_out_total counter\n"
       << "nx_ws_messages_out_total " << hub.messagesOut() << '\n';
    os << "# HELP nx_ws_rate_limited_total Messages dropped by the per-client rate limiter\n"
          "# TYPE nx_ws_rate_limited_total counter\n"
       << "nx_ws_rate_limited_total " << ctx.rateLimited.load() << '\n';
    os << "# HELP nx_ws_slow_consumer_drops_total Clients disconnected for not reading fast enough\n"
          "# TYPE nx_ws_slow_consumer_drops_total counter\n"
       << "nx_ws_slow_consumer_drops_total " << hub.slowConsumerDrops() << '\n';
    return os.str();
}

}  // namespace nx
