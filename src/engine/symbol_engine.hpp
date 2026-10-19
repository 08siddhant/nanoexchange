#pragma once
// One SymbolEngine = one symbol = one dedicated thread that owns the book.
//
// Why a thread per symbol?
//   * Orders for different symbols never interact, so they can run in parallel.
//   * Inside one symbol everything is sequential -> the order book needs no
//     locks and matching is deterministic (which is what makes WAL replay work).
//   * The only synchronisation point is the command queue.

#include "engine/blocking_queue.hpp"
#include "engine/events.hpp"
#include "engine/metrics.hpp"
#include "engine/order_book.hpp"
#include "engine/wal.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace nx {

struct EngineOptions {
    std::filesystem::path dataDir;              // empty = no persistence
    bool                  fsyncEachWrite = false;
    std::uint64_t         snapshotEvery = 50'000;  // commands between snapshots
    std::chrono::milliseconds publishInterval{100};  // book snapshot throttle
    std::size_t           queueCapacity = 100'000;
    std::size_t           depthLevels = 12;
};

struct RecoveryStats {
    bool          snapshotLoaded = false;
    std::size_t   snapshotOrders = 0;
    std::size_t   walRecordsReplayed = 0;
    std::size_t   corruptTail = 0;
    double        millis = 0;
};

class SymbolEngine {
public:
    SymbolEngine(std::string symbol, EngineOptions opts, EventSink& sink);
    ~SymbolEngine();

    // Rebuild state from snapshot + WAL. Call before start().
    RecoveryStats recover();
    void start();
    void stop();  // processes everything already queued, then joins

    // Thread-safe. Returns false if the queue is full (backpressure).
    bool post(Command cmd);

    const std::string& symbol() const { return symbol_; }
    EngineMetrics& metrics() { return metrics_; }
    std::size_t queueDepth() const { return queue_.size(); }

    // Direct access for tests / benchmarks (only safe when the thread is not running).
    OrderBook& bookUnsafe() { return book_; }
    void processUnsafe(const Command& cmd) { process(cmd); }

private:
    void run();
    void process(const Command& cmd);
    void applyRecord(const std::string& payload);
    void maybeSnapshot();
    void publishBook();

    std::string                symbol_;
    EngineOptions              opts_;
    EventSink&                 sink_;
    OrderBook                  book_;
    std::unique_ptr<Wal>       wal_;
    BlockingQueue<Command>     queue_;
    EngineMetrics              metrics_;
    std::thread                thread_;
    std::atomic<bool>          running_{false};

    OrderId                    nextId_ = 1;
    std::uint64_t              lsn_ = 0;
    std::uint64_t              sinceSnapshot_ = 0;
    std::optional<Price>       lastPrice_;
    std::uint64_t              volume_ = 0;
    bool                       dirty_ = true;
};

}  // namespace nx
