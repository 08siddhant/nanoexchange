#include "server/bots.hpp"

#include <cmath>

namespace nx {

BotFleet::BotFleet(Exchange& ex, std::map<std::string, double> basePrices, std::chrono::milliseconds tick)
    : ex_(ex), base_(std::move(basePrices)), tick_(tick) {
    for (const auto& s : ex_.symbols()) {
        if (!base_.count(s)) base_[s] = 100.0;
        fair_[s] = base_[s];
    }
}

void BotFleet::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void BotFleet::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void BotFleet::run() {
    std::normal_distribution<double> step(0.0, 1.0);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    while (running_) {
        for (const auto& s : ex_.symbols()) {
            double& fair = fair_[s];
            const double base = base_[s];
            // Random walk with gentle mean reversion so prices stay sensible forever.
            fair += fair * 0.0006 * step(rng_) + 0.002 * (base - fair);
            fair = std::max(fair, base * 0.2);
            if (coin(rng_) < 0.7) quote(s, fair);
            if (coin(rng_) < 0.6) take(s);
        }
        std::this_thread::sleep_for(tick_);
    }
}

void BotFleet::quote(const std::string& symbol, double fair) {
    Command cancel;
    cancel.kind = Command::Kind::CancelAll;
    cancel.client = kMarketMaker;
    ex_.post(symbol, cancel);

    std::uniform_int_distribution<int> size(5, 120);
    const Price mid = static_cast<Price>(std::llround(fair / kTickSize));
    const Price spread = std::max<Price>(2, static_cast<Price>(std::llround(fair * 0.0004 / kTickSize)));
    for (int level = 0; level < 12; ++level) {
        const Price gap = spread / 2 + level * std::max<Price>(1, spread / 2);
        for (Side side : {Side::Buy, Side::Sell}) {
            Command c;
            c.kind = Command::Kind::New;
            c.client = kMarketMaker;
            c.side = side;
            c.type = OrderType::Limit;
            c.price = side == Side::Buy ? mid - gap : mid + gap;
            c.qty = size(rng_) * (level + 1) / 2 + 1;  // deeper levels are bigger
            if (c.price > 0) ex_.post(symbol, c);
        }
    }
}

void BotFleet::take(const std::string& symbol) {
    std::uniform_int_distribution<int> size(1, 60);
    Command c;
    c.kind = Command::Kind::New;
    c.client = kTaker;
    c.side = (rng_() & 1) ? Side::Buy : Side::Sell;
    c.type = OrderType::Market;
    c.qty = size(rng_);
    ex_.post(symbol, c);
}

}  // namespace nx
