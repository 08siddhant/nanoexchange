#pragma once
// The exchange routes each command to the engine thread that owns its symbol.

#include "engine/symbol_engine.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nx {

class Exchange {
public:
    Exchange(const std::vector<std::string>& symbols, const EngineOptions& opts, EventSink& sink) {
        for (const auto& s : symbols) {
            engines_.emplace(s, std::make_unique<SymbolEngine>(s, opts, sink));
            order_.push_back(s);
        }
    }

    std::map<std::string, RecoveryStats> recover() {
        std::map<std::string, RecoveryStats> out;
        for (auto& [s, e] : engines_) out[s] = e->recover();
        return out;
    }
    void start() { for (auto& [_, e] : engines_) e->start(); }
    void stop()  { for (auto& [_, e] : engines_) e->stop(); }

    enum class PostResult { Ok, UnknownSymbol, Busy };
    PostResult post(const std::string& symbol, Command cmd) {
        auto it = engines_.find(symbol);
        if (it == engines_.end()) return PostResult::UnknownSymbol;
        return it->second->post(std::move(cmd)) ? PostResult::Ok : PostResult::Busy;
    }

    bool has(const std::string& symbol) const { return engines_.count(symbol) > 0; }
    const std::vector<std::string>& symbols() const { return order_; }
    SymbolEngine& engine(const std::string& symbol) { return *engines_.at(symbol); }

private:
    std::map<std::string, std::unique_ptr<SymbolEngine>> engines_;
    std::vector<std::string>                             order_;
};

}  // namespace nx
