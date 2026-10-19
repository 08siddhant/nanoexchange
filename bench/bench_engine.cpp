// Benchmark: matching throughput and latency.
//   ./nx_bench [orders]
// Prints throughput and latency percentiles for:
//   1. pure in-memory matching (single thread, no queue)
//   2. matching + write-ahead log (no fsync)
//   3. end-to-end through the queue + engine thread (what a client sees, minus network)

#include "engine/exchange.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <thread>
#include <vector>

using namespace nx;
using Clock = std::chrono::steady_clock;

namespace {

std::vector<Command> makeWorkload(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> offset(0.0, 8.0);
    std::uniform_int_distribution<int> qty(1, 100), roll(0, 99), client(1, 50);
    std::vector<Command> cmds;
    cmds.reserve(n);
    OrderId issued = 0;
    for (std::size_t i = 0; i < n; ++i) {
        Command c;
        c.client = static_cast<ClientId>(client(rng));
        int r = roll(rng);
        if (r < 20 && issued > 100) {  // 20% cancels of recent orders
            c.kind = Command::Kind::Cancel;
            c.cancelId = issued - static_cast<OrderId>(rng() % 100);
        } else {
            c.kind = Command::Kind::New;
            c.side = (rng() & 1) ? Side::Buy : Side::Sell;
            c.type = r < 25 ? OrderType::Market : OrderType::Limit;
            // Buys slightly below mid, sells slightly above -> realistic mix of resting and crossing.
            double off = offset(rng) + (c.side == Side::Buy ? -3 : 3);
            c.price = 10000 + static_cast<Price>(off);
            c.qty = qty(rng);
            ++issued;
        }
        cmds.push_back(std::move(c));
    }
    return cmds;
}

void report(const char* name, std::vector<double>& ns, double totalSec) {
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double p) { return ns[static_cast<std::size_t>(p * (ns.size() - 1))]; };
    std::printf("%-34s %10.0f ops/s   p50 %7.0f ns   p99 %8.0f ns   p99.9 %8.0f ns\n", name,
                ns.size() / totalSec, pct(0.50), pct(0.99), pct(0.999));
}

// Cancels need the owning client; route them as that client would.
void fixCancelOwners(std::vector<Command>& cmds) {
    std::vector<ClientId> owner(cmds.size() + 2, 0);
    OrderId next = 1;
    for (auto& c : cmds) {
        if (c.kind == Command::Kind::New) owner[next++] = c.client;
        else if (c.cancelId < owner.size()) c.client = owner[c.cancelId];
    }
}

void benchInline(const char* name, std::vector<Command> cmds, const EngineOptions& opts) {
    EventSink sink;
    SymbolEngine e("BENCH", opts, sink);
    e.recover();
    std::vector<double> lat;
    lat.reserve(cmds.size());
    auto t0 = Clock::now();
    for (auto& c : cmds) {
        auto s = Clock::now();
        c.received = s;
        e.processUnsafe(c);
        lat.push_back(std::chrono::duration<double, std::nano>(Clock::now() - s).count());
    }
    double sec = std::chrono::duration<double>(Clock::now() - t0).count();
    report(name, lat, sec);
    std::printf("%-34s resting orders at end: %zu, trades: %llu\n", "", e.bookUnsafe().orderCount(),
                static_cast<unsigned long long>(e.metrics().trades.load()));
}

// Throughput when the producer pushes as fast as it can (queue + thread hop).
void benchThreadedSaturated(std::vector<Command> cmds) {
    EventSink sink;
    EngineOptions opts;
    opts.queueCapacity = cmds.size() + 10;
    Exchange ex({"BENCH"}, opts, sink);
    ex.start();
    auto t0 = Clock::now();
    for (auto& c : cmds) {
        c.received = Clock::now();
        ex.post("BENCH", c);
    }
    ex.stop();
    double sec = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("%-34s %10.0f ops/s   (producer thread -> queue -> engine thread)\n",
                "engine thread, saturated", cmds.size() / sec);
}

// Latency at a realistic, steady rate (queue stays near empty).
void benchThreadedPaced(std::vector<Command> cmds, double ratePerSec) {
    EventSink sink;
    Exchange ex({"BENCH"}, EngineOptions{}, sink);
    ex.start();
    const auto gap = std::chrono::nanoseconds(static_cast<long long>(1e9 / ratePerSec));
    auto next = Clock::now();
    for (auto& c : cmds) {
        while (Clock::now() < next) {}  // spin until the next send slot
        next += gap;
        c.received = Clock::now();
        ex.post("BENCH", c);
    }
    ex.stop();
    auto snap = ex.engine("BENCH").metrics().latency.snapshot();
    char name[64];
    std::snprintf(name, sizeof(name), "engine thread @ %.0fk orders/s", ratePerSec / 1000);
    std::printf("%-34s queue-to-processed latency: p50 <= %.0f us, p99 <= %.0f us (histogram buckets)\n", name,
                LatencyHistogram::quantile(snap, 0.5), LatencyHistogram::quantile(snap, 0.99));
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n = argc > 1 ? std::stoul(argv[1]) : 1'000'000;
    auto cmds = makeWorkload(n, 12345);
    fixCancelOwners(cmds);
    std::printf("workload: %zu commands (~55%% limit, ~25%% market, ~20%% cancel), %u hw threads\n\n", n,
                std::thread::hardware_concurrency());

    benchInline("in-memory matching", cmds, EngineOptions{});

    auto dir = std::filesystem::temp_directory_path() / "nx_bench_wal";
    std::filesystem::remove_all(dir);
    EngineOptions walOpts;
    walOpts.dataDir = dir;
    walOpts.snapshotEvery = 200'000;
    benchInline("matching + WAL (no fsync)", cmds, walOpts);
    std::filesystem::remove_all(dir);

    std::vector<Command> few(cmds.begin(), cmds.begin() + std::min<std::size_t>(n, 2000));
    walOpts.fsyncEachWrite = true;
    benchInline("matching + WAL + fsync each write", few, walOpts);
    std::filesystem::remove_all(dir);

    benchThreadedSaturated(cmds);
    std::vector<Command> paced(cmds.begin(), cmds.begin() + std::min<std::size_t>(n, 200'000));
    benchThreadedPaced(paced, 100'000);
    benchThreadedPaced(std::vector<Command>(paced.begin(), paced.begin() + 100000), 20'000);
    return 0;
}
