#pragma once
#include "engine/exchange.hpp"
#include "server/hub.hpp"
#include "server/server.hpp"

#include <string>

namespace nx {
// Render all metrics in the Prometheus text exposition format.
std::string renderPrometheus(Exchange& ex, Hub& hub, ServerContext& ctx);
}  // namespace nx
