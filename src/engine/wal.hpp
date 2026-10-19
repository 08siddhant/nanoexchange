#pragma once
// Write-ahead log (WAL) and snapshots for crash recovery.
//
// Rule: every command is appended to the WAL *before* it is applied to the
// order book. Because matching is deterministic, replaying the log on startup
// rebuilds exactly the same book.
//
// Record format (one line per command):
//     <lsn> <payload>|<fnv1a-32 checksum in hex>\n
// The checksum lets recovery detect a torn (half-written) last line after a
// crash and stop there instead of applying garbage.
//
// Durability:
//   * Each append is a single write(2) syscall, so data is in the OS page cache
//     immediately -> survives a *process* crash (kill -9, segfault).
//   * With fsync enabled, each append is also fdatasync'ed -> survives a
//     *machine* crash / power loss, at the cost of latency.
//
// Snapshots (compaction): the WAL would grow forever, so every N commands the
// engine writes the full book to <symbol>.snap (write tmp + fsync + atomic
// rename) and then truncates the WAL. The snapshot stores the last LSN it
// covers, so if we crash between the rename and the truncate, already-applied
// WAL records are skipped on replay instead of being applied twice.

#include "engine/order_book.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nx {

std::uint32_t fnv1a(std::string_view data);

struct WalRecord {
    std::uint64_t lsn = 0;
    std::string   payload;
};

class Wal {
public:
    Wal(std::filesystem::path path, bool fsyncEachWrite);
    ~Wal();
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    void append(std::uint64_t lsn, std::string_view payload);
    void truncate();  // called after a successful snapshot
    std::uint64_t bytes() const { return bytes_; }

    // Read all valid records. Stops at the first corrupt/torn record.
    static std::vector<WalRecord> readAll(const std::filesystem::path& path,
                                          std::size_t* corruptTail = nullptr);

private:
    std::filesystem::path path_;
    int                   fd_ = -1;
    bool                  fsync_;
    std::uint64_t         bytes_ = 0;
};

struct SnapshotHeader {
    std::uint64_t lastLsn  = 0;
    OrderId       nextId   = 1;
    std::uint64_t tradeSeq = 0;
};

// Atomically write the whole book to `path`.
void writeSnapshot(const std::filesystem::path& path, const SnapshotHeader& h, const OrderBook& book);

// Load a snapshot into `book`. Returns false if missing or invalid.
bool loadSnapshot(const std::filesystem::path& path, SnapshotHeader& h, OrderBook& book);

}  // namespace nx
