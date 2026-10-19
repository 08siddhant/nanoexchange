#include "engine/symbol_engine.hpp"

#include <iostream>
#include <sstream>

namespace nx {

namespace {
std::filesystem::path walPath(const EngineOptions& o, const std::string& s) { return o.dataDir / (s + ".wal"); }
std::filesystem::path snapPath(const EngineOptions& o, const std::string& s) { return o.dataDir / (s + ".snap"); }

std::string encodeNew(const Order& o) {
    std::ostringstream os;
    os << "N " << o.id << ' ' << o.client << ' ' << (o.side == Side::Buy ? 'B' : 'S') << ' '
       << (o.type == OrderType::Limit ? 'L' : 'M') << ' ' << o.price << ' ' << o.qty;
    return os.str();
}
}  // namespace

SymbolEngine::SymbolEngine(std::string symbol, EngineOptions opts, EventSink& sink)
    : symbol_(std::move(symbol)), opts_(std::move(opts)), sink_(sink), queue_(opts_.queueCapacity) {}

SymbolEngine::~SymbolEngine() { stop(); }

// Re-apply one WAL record. Must mirror process() exactly, minus side effects.
void SymbolEngine::applyRecord(const std::string& payload) {
    std::istringstream is(payload);
    char kind;
    is >> kind;
    if (kind == 'N') {
        Order o;
        char side, type;
        is >> o.id >> o.client >> side >> type >> o.price >> o.qty;
        o.side = side == 'B' ? Side::Buy : Side::Sell;
        o.type = type == 'L' ? OrderType::Limit : OrderType::Market;
        auto r = book_.submit(o);
        for (const auto& t : r.trades) {
            lastPrice_ = t.price;
            volume_ += static_cast<std::uint64_t>(t.qty);
        }
        if (o.id >= nextId_) nextId_ = o.id + 1;
    } else if (kind == 'C') {
        OrderId id;
        ClientId c;
        is >> id >> c;
        book_.cancel(id, c);
    } else if (kind == 'A') {
        ClientId c;
        is >> c;
        book_.cancelAll(c);
    }
}

RecoveryStats SymbolEngine::recover() {
    RecoveryStats st;
    if (opts_.dataDir.empty()) return st;
    auto t0 = std::chrono::steady_clock::now();
    std::filesystem::create_directories(opts_.dataDir);

    SnapshotHeader h;
    if (loadSnapshot(snapPath(opts_, symbol_), h, book_)) {
        st.snapshotLoaded = true;
        st.snapshotOrders = book_.orderCount();
        lsn_ = h.lastLsn;
        nextId_ = h.nextId;
    }

    // Replay only records newer than the snapshot (idempotent recovery).
    auto records = Wal::readAll(walPath(opts_, symbol_), &st.corruptTail);
    for (const auto& r : records) {
        if (r.lsn <= lsn_) continue;
        applyRecord(r.payload);
        lsn_ = r.lsn;
        ++st.walRecordsReplayed;
    }

    wal_ = std::make_unique<Wal>(walPath(opts_, symbol_), opts_.fsyncEachWrite);
    if (st.corruptTail > 0) {
        // Drop the torn tail: snapshot the good state and start a clean log.
        writeSnapshot(snapPath(opts_, symbol_), {lsn_, nextId_, book_.tradeSeq()}, book_);
        wal_->truncate();
    }
    metrics_.walBytes = wal_->bytes();
    metrics_.bookOrders = static_cast<std::int64_t>(book_.orderCount());
    metrics_.recoveredRecords = st.walRecordsReplayed;
    st.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return st;
}

void SymbolEngine::start() {
    if (running_.exchange(true)) return;
    if (!wal_ && !opts_.dataDir.empty()) recover();
    thread_ = std::thread([this] { run(); });
}

void SymbolEngine::stop() {
    if (!running_.exchange(false)) return;
    Command stopCmd;
    stopCmd.kind = Command::Kind::Stop;
    queue_.push(std::move(stopCmd));
    if (thread_.joinable()) thread_.join();
}

bool SymbolEngine::post(Command cmd) {
    if (!queue_.tryPush(std::move(cmd))) {
        metrics_.rejectedBusy.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void SymbolEngine::run() {
    auto lastPublish = std::chrono::steady_clock::now();
    publishBook();
    for (;;) {
        auto cmd = queue_.popFor(std::chrono::milliseconds(20));
        if (cmd) {
            if (cmd->kind == Command::Kind::Stop) break;
            process(*cmd);
        }
        auto now = std::chrono::steady_clock::now();
        if (dirty_ && now - lastPublish >= opts_.publishInterval) {
            publishBook();
            lastPublish = now;
        }
    }
    publishBook();
}

void SymbolEngine::process(const Command& cmd) {
    switch (cmd.kind) {
        case Command::Kind::New: {
            Order o;
            o.id     = nextId_++;
            o.client = cmd.client;
            o.side   = cmd.side;
            o.type   = cmd.type;
            o.price  = cmd.type == OrderType::Limit ? cmd.price : 0;
            o.qty    = cmd.qty;
            if (wal_) wal_->append(++lsn_, encodeNew(o));  // 1) log first ...

            SubmitResult r = book_.submit(o);              // 2) ... then apply
            metrics_.ordersIn.fetch_add(1, std::memory_order_relaxed);
            for (const auto& t : r.trades) {
                lastPrice_ = t.price;
                volume_ += static_cast<std::uint64_t>(t.qty);
                metrics_.volume.fetch_add(static_cast<std::uint64_t>(t.qty), std::memory_order_relaxed);
            }
            metrics_.trades.fetch_add(r.trades.size(), std::memory_order_relaxed);

            sink_.onOrderResult(symbol_, r, cmd.clOrdId);
            if (!r.trades.empty()) sink_.onTrades(symbol_, r.trades);
            break;
        }
        case Command::Kind::Cancel: {
            if (wal_) wal_->append(++lsn_, "C " + std::to_string(cmd.cancelId) + " " + std::to_string(cmd.client));
            bool ok = book_.cancel(cmd.cancelId, cmd.client).has_value();
            metrics_.cancels.fetch_add(1, std::memory_order_relaxed);
            sink_.onCancelResult(symbol_, cmd.client, cmd.cancelId, ok, cmd.clOrdId);
            break;
        }
        case Command::Kind::CancelAll: {
            if (wal_) wal_->append(++lsn_, "A " + std::to_string(cmd.client));
            auto ids = book_.cancelAll(cmd.client);
            metrics_.cancels.fetch_add(ids.size(), std::memory_order_relaxed);
            sink_.onCancelAll(symbol_, cmd.client, ids.size());
            break;
        }
        case Command::Kind::Stop: return;
    }

    dirty_ = true;
    metrics_.bookOrders.store(static_cast<std::int64_t>(book_.orderCount()), std::memory_order_relaxed);
    const double us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - cmd.received).count();
    metrics_.latency.observe(us);
    maybeSnapshot();
}

void SymbolEngine::maybeSnapshot() {
    if (!wal_) return;
    metrics_.walBytes.store(wal_->bytes(), std::memory_order_relaxed);
    if (++sinceSnapshot_ < opts_.snapshotEvery) return;
    sinceSnapshot_ = 0;
    writeSnapshot(snapPath(opts_, symbol_), {lsn_, nextId_, book_.tradeSeq()}, book_);
    wal_->truncate();  // safe: snapshot already covers every record up to lsn_
    metrics_.snapshots.fetch_add(1, std::memory_order_relaxed);
    metrics_.walBytes.store(0, std::memory_order_relaxed);
}

void SymbolEngine::publishBook() {
    BookSnapshot s;
    s.bids      = book_.depth(Side::Buy, opts_.depthLevels);
    s.asks      = book_.depth(Side::Sell, opts_.depthLevels);
    s.lastPrice = lastPrice_;
    s.volume    = volume_;
    s.orders    = book_.orderCount();
    sink_.onBook(symbol_, s);
    dirty_ = false;
}

}  // namespace nx
