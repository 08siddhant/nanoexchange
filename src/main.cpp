// nanoexchange - a real-time stock exchange simulator.
//
// Configuration (environment variables):
//   PORT            HTTP/WebSocket port               (default 8080)
//   SYMBOLS         comma separated symbols           (default AAPL,TSLA,INFY)
//   DATA_DIR        WAL + snapshot directory          (default ./data, "" = in-memory)
//   WAL_FSYNC       1 = fdatasync every WAL write     (default 0)
//   SNAPSHOT_EVERY  commands between snapshots        (default 50000)
//   BOTS            1 = run market-maker/taker bots   (default 1)
//   BOT_TICK_MS     bot loop interval                 (default 250)
//   THREADS         network I/O threads               (default 2)
//   STATIC_DIR      web UI directory                  (default ./web)

#include "engine/exchange.hpp"
#include "server/bots.hpp"
#include "server/hub.hpp"
#include "server/metrics_http.hpp"
#include "server/server.hpp"

#include <boost/asio/signal_set.hpp>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>

namespace {

std::string env(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : def;
}
long envInt(const char* name, long def) {
    const char* v = std::getenv(name);
    return v && *v ? std::strtol(v, nullptr, 10) : def;
}
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep))
        if (!item.empty()) out.push_back(item);
    return out;
}

}  // namespace

int main() {
    using namespace nx;
    try {
        const auto port = static_cast<unsigned short>(envInt("PORT", 8080));
        const auto symbols = split(env("SYMBOLS", "AAPL,TSLA,INFY"), ',');
        const int threads = std::max<long>(1, envInt("THREADS", 2));

        EngineOptions opts;
        opts.dataDir = env("DATA_DIR", "./data");
        opts.fsyncEachWrite = envInt("WAL_FSYNC", 0) == 1;
        opts.snapshotEvery = static_cast<std::uint64_t>(std::max<long>(100, envInt("SNAPSHOT_EVERY", 50'000)));

        net::io_context ioc{threads};
        Hub hub(ioc);
        Exchange exchange(symbols, opts, hub);

        // ---- crash recovery ------------------------------------------------
        std::cout << "nanoexchange starting | symbols=" << symbols.size() << " data_dir="
                  << (opts.dataDir.empty() ? "<in-memory>" : opts.dataDir.string())
                  << " fsync=" << (opts.fsyncEachWrite ? "on" : "off") << std::endl;
        for (const auto& [sym, st] : exchange.recover()) {
            std::cout << "  [recovery] " << sym << ": snapshot=" << (st.snapshotLoaded ? "yes" : "no")
                      << " (" << st.snapshotOrders << " orders), wal_replayed=" << st.walRecordsReplayed
                      << (st.corruptTail ? ", dropped torn tail" : "") << ", took " << st.millis << " ms"
                      << std::endl;
        }
        exchange.start();

        ServerContext ctx{exchange, hub, loadStaticFiles(env("STATIC_DIR", "./web")), {}};
        ctx.renderMetrics = [&] { return renderPrometheus(exchange, hub, ctx); };

        auto listener = std::make_shared<Listener>(ioc, tcp::endpoint{net::ip::address_v4::any(), port}, ctx);
        listener->run();
        hub.startStats(exchange);

        std::unique_ptr<BotFleet> bots;
        if (envInt("BOTS", 1) == 1) {
            bots = std::make_unique<BotFleet>(
                exchange, std::map<std::string, double>{{"AAPL", 230.0}, {"TSLA", 250.0}, {"INFY", 1550.0}},
                std::chrono::milliseconds(std::max<long>(20, envInt("BOT_TICK_MS", 250))));
            bots->start();
        }

        // ---- graceful shutdown on SIGINT / SIGTERM (Docker & Kubernetes send SIGTERM)
        net::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](beast::error_code, int sig) {
            std::cout << "signal " << sig << " received, shutting down" << std::endl;
            listener->stop();
            hub.stopStats();
            ioc.stop();
        });

        std::cout << "listening on http://0.0.0.0:" << port << "  (ws: /ws, metrics: /metrics)" << std::endl;
        std::vector<std::thread> pool;
        for (int i = 1; i < threads; ++i) pool.emplace_back([&] { ioc.run(); });
        ioc.run();
        for (auto& t : pool) t.join();

        if (bots) bots->stop();
        exchange.stop();  // drain queues, flush WAL
        std::cout << "bye" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
