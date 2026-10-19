#pragma once
// Lock-free counters and a latency histogram, exported in Prometheus format.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace nx {

class LatencyHistogram {
public:
    // Bucket upper bounds in microseconds.
    static constexpr std::array<double, 14> kBounds = {1,   2,    5,    10,   25,    50,    100,
                                                       250, 500,  1000, 2500, 5000,  10000, 50000};
    static constexpr std::size_t kBuckets = kBounds.size() + 1;  // +Inf

    void observe(double micros) {
        std::size_t i = 0;
        while (i < kBounds.size() && micros > kBounds[i]) ++i;
        buckets_[i].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        sumNanos_.fetch_add(static_cast<std::uint64_t>(micros * 1000.0), std::memory_order_relaxed);
    }

    std::array<std::uint64_t, kBuckets> snapshot() const {
        std::array<std::uint64_t, kBuckets> out{};
        for (std::size_t i = 0; i < kBuckets; ++i) out[i] = buckets_[i].load(std::memory_order_relaxed);
        return out;
    }
    std::uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    double sumSeconds() const { return sumNanos_.load(std::memory_order_relaxed) / 1e9; }

    // Approximate quantile from bucket counts (upper bound of the bucket).
    static double quantile(const std::array<std::uint64_t, kBuckets>& counts, double q) {
        std::uint64_t total = 0;
        for (auto c : counts) total += c;
        if (total == 0) return 0;
        const double target = q * static_cast<double>(total);
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            cum += counts[i];
            if (static_cast<double>(cum) >= target) return i < kBounds.size() ? kBounds[i] : kBounds.back() * 2;
        }
        return kBounds.back();
    }

private:
    std::array<std::atomic<std::uint64_t>, kBuckets> buckets_{};
    std::atomic<std::uint64_t>                       count_{0};
    std::atomic<std::uint64_t>                       sumNanos_{0};
};

struct EngineMetrics {
    std::atomic<std::uint64_t> ordersIn{0};
    std::atomic<std::uint64_t> cancels{0};
    std::atomic<std::uint64_t> trades{0};
    std::atomic<std::uint64_t> volume{0};
    std::atomic<std::uint64_t> rejectedBusy{0};
    std::atomic<std::int64_t>  bookOrders{0};
    std::atomic<std::uint64_t> walBytes{0};
    std::atomic<std::uint64_t> snapshots{0};
    std::atomic<std::uint64_t> recoveredRecords{0};
    LatencyHistogram           latency;  // time from network receive to processed
};

}  // namespace nx
