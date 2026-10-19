#pragma once
// Simulated traders so the live demo always has a moving market.
//   * Market maker (client 1): every tick it cancels its old quotes and posts a
//     fresh ladder of bids and asks around a "fair price" that does a random walk.
//   * Taker (client 2): randomly sends market orders that hit those quotes,
//     producing trades.
// Bots go through exactly the same command path as real clients.

#include "engine/exchange.hpp"

#include <atomic>
#include <map>
#include <random>
#include <string>
#include <thread>

namespace nx {

class BotFleet {
public:
    static constexpr ClientId kMarketMaker = 1;
    static constexpr ClientId kTaker = 2;

    BotFleet(Exchange& ex, std::map<std::string, double> basePrices, std::chrono::milliseconds tick);
    ~BotFleet() { stop(); }
    void start();
    void stop();

private:
    void run();
    void quote(const std::string& symbol, double fair);
    void take(const std::string& symbol);

    Exchange&                     ex_;
    std::map<std::string, double> base_;
    std::map<std::string, double> fair_;
    std::chrono::milliseconds     tick_;
    std::mt19937                  rng_{std::random_device{}()};
    std::thread                   thread_;
    std::atomic<bool>             running_{false};
};

}  // namespace nx
