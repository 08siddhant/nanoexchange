// Crash-recovery tests: the book rebuilt from disk must be identical to the
// book that was running before the "crash".

#include "engine/symbol_engine.hpp"
#include "engine/wal.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <random>

using namespace nx;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("nx_test_" + std::to_string(std::random_device{}()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

Command newOrder(ClientId c, Side s, Price p, Qty q, OrderType t = OrderType::Limit) {
    Command cmd;
    cmd.kind = Command::Kind::New;
    cmd.client = c; cmd.side = s; cmd.price = p; cmd.qty = q; cmd.type = t;
    return cmd;
}

// Serialise everything observable about a book so two books can be compared.
std::string fingerprint(const OrderBook& b) {
    std::string out;
    b.forEachOrder([&](const Order& o) {
        out += std::to_string(o.id) + ":" + std::to_string(o.client) + ":" + (o.side == Side::Buy ? "B" : "S") + ":" +
               std::to_string(o.price) + ":" + std::to_string(o.remaining) + ";";
    });
    out += "|seq=" + std::to_string(b.tradeSeq());
    return out;
}

// Drive an engine with a deterministic pseudo-random workload.
void randomWorkload(SymbolEngine& e, int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> price(9900, 10100), qty(1, 50), client(1, 5), kind(0, 19);
    OrderId maxId = 0;
    for (int i = 0; i < n; ++i) {
        int k = kind(rng);
        if (k == 0) {
            Command c; c.kind = Command::Kind::CancelAll; c.client = static_cast<ClientId>(client(rng));
            e.processUnsafe(c);
        } else if (k <= 3 && maxId > 0) {
            Command c; c.kind = Command::Kind::Cancel; c.client = static_cast<ClientId>(client(rng));
            c.cancelId = std::uniform_int_distribution<OrderId>(1, maxId)(rng);
            e.processUnsafe(c);
        } else {
            auto t = k == 4 ? OrderType::Market : OrderType::Limit;
            e.processUnsafe(newOrder(static_cast<ClientId>(client(rng)), (rng() & 1) ? Side::Buy : Side::Sell,
                                     price(rng), qty(rng), t));
            ++maxId;
        }
    }
}

}  // namespace

TEST(Wal, ChecksumDetectsTornWrite) {
    TempDir d;
    auto p = d.path / "x.wal";
    {
        Wal w(p, false);
        w.append(1, "N 1 1 B L 100 5");
        w.append(2, "N 2 1 S L 101 5");
    }
    {
        std::ofstream f(p, std::ios::app);
        f << "3 N 3 1 B L 10";  // half-written line, no checksum
    }
    std::size_t corrupt = 0;
    auto recs = Wal::readAll(p, &corrupt);
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[1].lsn, 2u);
    EXPECT_EQ(corrupt, 1u);
}

TEST(Recovery, ReplayRebuildsIdenticalBook) {
    TempDir d;
    EventSink sink;
    EngineOptions opts;
    opts.dataDir = d.path;
    opts.snapshotEvery = 1'000'000;  // WAL only

    std::string before;
    {
        SymbolEngine e("TEST", opts, sink);
        e.recover();
        randomWorkload(e, 3000, 42);
        before = fingerprint(e.bookUnsafe());
    }  // "crash"

    SymbolEngine restored("TEST", opts, sink);
    auto st = restored.recover();
    EXPECT_FALSE(st.snapshotLoaded);
    EXPECT_EQ(st.walRecordsReplayed, 3000u);
    EXPECT_EQ(fingerprint(restored.bookUnsafe()), before);
}

TEST(Recovery, SnapshotPlusWalRebuildsIdenticalBook) {
    TempDir d;
    EventSink sink;
    EngineOptions opts;
    opts.dataDir = d.path;
    opts.snapshotEvery = 700;  // forces several snapshots + WAL truncations

    std::string before;
    {
        SymbolEngine e("TEST", opts, sink);
        e.recover();
        randomWorkload(e, 5000, 7);
        before = fingerprint(e.bookUnsafe());
    }

    SymbolEngine restored("TEST", opts, sink);
    auto st = restored.recover();
    EXPECT_TRUE(st.snapshotLoaded);
    EXPECT_LT(st.walRecordsReplayed, 700u);  // only the tail after the last snapshot
    EXPECT_EQ(fingerprint(restored.bookUnsafe()), before);
}

TEST(Recovery, CrashBetweenSnapshotAndTruncateDoesNotDoubleApply) {
    TempDir d;
    EventSink sink;
    EngineOptions opts;
    opts.dataDir = d.path;
    opts.snapshotEvery = 1'000'000;

    std::string before;
    {
        SymbolEngine e("TEST", opts, sink);
        e.recover();
        randomWorkload(e, 1000, 99);
        before = fingerprint(e.bookUnsafe());
        // Simulate: snapshot written, but process died before WAL truncate.
        writeSnapshot(d.path / "TEST.snap", {1000, 1'000'000, e.bookUnsafe().tradeSeq()}, e.bookUnsafe());
    }

    SymbolEngine restored("TEST", opts, sink);
    auto st = restored.recover();
    EXPECT_TRUE(st.snapshotLoaded);
    EXPECT_EQ(st.walRecordsReplayed, 0u);  // every record has lsn <= snapshot lsn
    EXPECT_EQ(fingerprint(restored.bookUnsafe()), before);
}

TEST(Recovery, OrderIdsContinueAfterRestart) {
    TempDir d;
    EventSink sink;
    EngineOptions opts;
    opts.dataDir = d.path;
    {
        SymbolEngine e("TEST", opts, sink);
        e.recover();
        e.processUnsafe(newOrder(1, Side::Buy, 100, 1));
        e.processUnsafe(newOrder(1, Side::Buy, 100, 1));
    }
    SymbolEngine e2("TEST", opts, sink);
    e2.recover();
    e2.processUnsafe(newOrder(1, Side::Buy, 100, 1));
    EXPECT_NE(e2.bookUnsafe().find(3), nullptr);  // ids 1,2 used before the crash
}
