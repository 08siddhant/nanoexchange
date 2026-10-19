// Multi-threaded test: many producer threads post orders to the engine thread
// concurrently. Run under ThreadSanitizer in CI to catch data races.

#include "engine/exchange.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>

using namespace nx;

namespace {
class CountingSink : public EventSink {
public:
    std::atomic<int>      results{0};
    std::atomic<long long> tradedQty{0};
    std::mutex            m;
    std::map<std::string, int> perSymbol;

    void onOrderResult(const std::string& sym, const SubmitResult&, const std::string&) override {
        results++;
        std::lock_guard lk(m);
        perSymbol[sym]++;
    }
    void onTrades(const std::string&, const std::vector<Trade>& ts) override {
        for (auto& t : ts) tradedQty += t.qty;
    }
};
}  // namespace

TEST(EngineThreads, ConcurrentProducersAcrossSymbols) {
    CountingSink sink;
    EngineOptions opts;  // in-memory
    Exchange ex({"AAA", "BBB", "CCC"}, opts, sink);
    ex.start();

    constexpr int kThreads = 6, kPerThread = 2000;
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&, t] {
            const std::string sym = ex.symbols()[t % 3];
            for (int i = 0; i < kPerThread; ++i) {
                Command c;
                c.client = static_cast<ClientId>(t + 1);
                c.side = (i % 2) ? Side::Buy : Side::Sell;
                c.price = 1000 + (i % 7) - 3;
                c.qty = 1 + i % 5;
                while (ex.post(sym, c) == Exchange::PostResult::Busy) std::this_thread::yield();
            }
        });
    }
    for (auto& p : producers) p.join();
    ex.stop();  // drains the queues

    EXPECT_EQ(sink.results.load(), kThreads * kPerThread);
    EXPECT_EQ(sink.perSymbol["AAA"], 2 * kPerThread);
    EXPECT_GT(sink.tradedQty.load(), 0);
}

TEST(EngineThreads, UnknownSymbolRejected) {
    EventSink sink;
    Exchange ex({"AAA"}, EngineOptions{}, sink);
    EXPECT_EQ(ex.post("ZZZ", Command{}), Exchange::PostResult::UnknownSymbol);
}
