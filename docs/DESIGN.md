# Design decisions

Each section explains one choice, the alternative, and the trade-off. These are the questions an interviewer is most likely to ask.

## 1. Order book data structures

| Need | Structure | Cost |
|---|---|---|
| Best bid/ask, prices kept sorted | `std::map<Price, Level>` (bids `greater<>`, asks `less<>`) | best price O(1) via `begin()`, new level O(log P) |
| Time priority inside a price | `std::list<Order>` per level (FIFO) | push back / pop front O(1) |
| Cancel any order quickly | `unordered_map<OrderId, {side, price, list::iterator}>` | O(1) lookup and erase |
| Cancel-all for one client | `unordered_map<ClientId, unordered_set<OrderId>>` | O(orders of that client) |

**Why `std::list`?** Its iterators stay valid when other elements are inserted or erased, so the index can store an iterator and cancel in O(1). A `std::deque` or `std::vector` would invalidate those iterators.

**Why integer prices?** `230.15` becomes `23015` ticks. Floating-point equality is unreliable (`0.1 + 0.2 != 0.3`), and prices have to be compared exactly to decide matches and levels.

**Possible improvements:** a flat array indexed by price tick for O(1) level access (used by HFT engines when the price range is bounded), and an intrusive list or object pool to avoid one heap allocation per order.

## 2. Matching rules

- **Price-time priority:** the best price matches first; within a price, the oldest order matches first.
- **Trade price = resting order's price.** An aggressive buyer at 105 against a resting ask at 100 trades at 100, giving price improvement to the aggressor.
- **Limit orders** never trade worse than their limit. Any leftover rests in the book.
- **Market orders** sweep levels until filled or the book is empty. Any leftover is dropped, never rested.
- **Cancel** is only allowed for the owner of the order (checked in the engine, tested end to end).

## 3. Threading: one engine thread per symbol

```
network threads (N) ──► bounded MPSC queue per symbol ──► 1 engine thread per symbol ──► Hub ──► sessions
```

- Orders for different symbols never interact, so symbols are **independent shards** that run in parallel.
- Inside a symbol, everything is **single-threaded**, so the order book needs no mutex, there are no data races, and results are **deterministic**. Determinism is what makes WAL replay correct.
- The only shared structure is the queue (mutex + condition variable). It is **bounded**: if it fills up, the client gets `exchange busy` (backpressure) instead of the server running out of memory.
- Measured cost of the thread hop: 1.58M orders/s inline vs 770k/s through the queue. Tail latency at moderate load comes from condition-variable wake-ups. A busy-spinning consumer with a lock-free SPSC ring buffer and core pinning would cut it further, at the cost of burning a CPU core per symbol.

## 4. Crash recovery: write-ahead log + snapshots

**Rule: log first, then apply.** If the process dies after logging, replay re-applies the command. If it dies before logging, the client never got an ack, so nothing was promised.

- **Record format:** `<lsn> <command>|<fnv1a checksum>`. Only inputs are logged (new, cancel, cancel-all), not outputs. Because the engine is deterministic, re-running the inputs regenerates the same trades and book.
- **Torn writes:** a crash can leave a half-written last line. The checksum detects it, and recovery stops there and discards the tail (unit-tested).
- **Durability levels:**
  - Default: one `write(2)` per record. Data is in the OS page cache immediately, so it survives a *process* crash (`kill -9`, segfault). Cost: ~1 µs.
  - `WAL_FSYNC=1`: `fdatasync` per record, which survives *power loss*, but drops throughput to ~5.7k orders/s on this disk. Real systems use **group commit** (fsync a batch of orders together) to get both.
- **Snapshots / compaction:** every 50,000 commands the full book is written to `.snap` (write a temp file, `fsync`, then atomic `rename`), and the WAL is truncated. Recovery = load snapshot + replay WAL records with `lsn > snapshot.lastLsn`.
- **Crash between snapshot and truncate:** the old WAL records are still there, but they are skipped because their LSN is covered by the snapshot. Recovery is **idempotent** (unit-tested).

## 5. WebSocket server (Boost.Beast)

- One `io_context`, run by `THREADS` threads. Each connection gets its **own strand**, so a session's handlers never run concurrently (no locks inside a session), while different sessions run in parallel.
- HTTP and WebSocket share one port. `GET /ws` with an `Upgrade` header becomes a WebSocket; `/metrics`, `/health` and the UI are served as plain HTTP.
- **Write queue per session:** Beast allows only one outstanding `async_write`, so messages go into an outbox and are written one after another. Broadcasts share one `shared_ptr<const string>`, so 100 clients means 100 pointers, not 100 copies.
- **Slow consumers:** if a client's outbox exceeds 2,048 messages, it is disconnected. One slow phone can't make the server buffer forever.
- **Rate limiting:** a token bucket of 50 messages/s per connection (burst 100) protects the public demo.
- **`TCP_NODELAY`:** without it, Nagle's algorithm held small ack packets for up to ~40 ms. The load test caught it (p95 48 ms → 5 ms after the fix).
- **Market-data conflation:** trades are batched per symbol every 50 ms, and book snapshots are throttled to 10/s. Acks are never batched, since they're private and latency-sensitive.

## 6. Scaling: why `replicas: 1`

The order book is **stateful** and the WAL must have **exactly one writer**. Two replicas behind a load balancer would each hold a different book, and two writers on one log would corrupt it. That's why the Kubernetes deployment uses `replicas: 1`, `strategy: Recreate`, and a PersistentVolumeClaim.

How it would scale:

1. **Shard by symbol:** AAPL on pod A, TSLA on pod B, with a gateway that routes orders by symbol. This is how real exchanges partition.
2. **High availability:** replicate the WAL to a hot standby (primary/backup) and fail over on crash. The standby replays the same log, so it holds the same book.
3. **Market data:** publish trades to Kafka or Redis pub/sub so many gateway pods can fan out to millions of clients, while the engine only talks to a few subscribers.

## 7. DevOps choices

- **Multi-stage Docker build:** the build stage has a compiler and runs the unit tests (a failing test fails the image build). The runtime stage contains only the binary and the web UI, runs as a non-root user, and has a `HEALTHCHECK`.
- **CI matrix:** release, ASan + UBSan, and TSan. TSan runs the multi-producer engine test, so a data race fails the build.
- **Docker e2e in CI:** CI starts the container, runs the Python WebSocket test, then `docker kill -9`s it, restarts it, and asserts that the WAL was replayed. A k6 smoke test runs next, and only then is the image pushed to GHCR.
- **Graceful shutdown:** `SIGTERM` (sent by Docker and Kubernetes) stops accepting connections, lets engine threads drain their queues, and flushes the WAL.
- **Metrics:** Prometheus histogram buckets (latency in seconds, cumulative `le` buckets) mean `histogram_quantile()` works in Grafana.

## 8. Known limitations (worth mentioning honestly)

- No authentication. Client ids are random per connection, so after a reconnect you can't cancel the previous session's orders.
- No self-trade prevention, no stop orders, no IOC/FOK (straightforward to add in `OrderBook::submit`).
- The last trade price and volume are not stored in snapshots, so they reset after a snapshot-based restart until the next trade.
- One `std::list` node allocation per resting order. A memory pool would remove allocator jitter.
