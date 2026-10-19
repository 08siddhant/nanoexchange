#pragma once
// Boost.Beast HTTP + WebSocket server on a single port.
//   GET /          -> web UI (static files)
//   GET /metrics   -> Prometheus metrics
//   GET /health    -> liveness/readiness probe
//   GET /ws        -> WebSocket upgrade (trading + live market data)
//
// Concurrency model: an io_context run by N threads. Every connection gets its
// own strand, so handlers of one connection never run concurrently and need
// no locks, while different connections run in parallel.

#include "engine/exchange.hpp"
#include "server/hub.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace nx {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

struct StaticFile {
    std::string body;
    std::string contentType;
};

struct ServerContext {
    Exchange&                          exchange;
    Hub&                               hub;
    std::map<std::string, StaticFile>  files;   // url path -> file
    std::function<std::string()>       renderMetrics;
    std::atomic<std::uint64_t>         messagesIn{0};
    std::atomic<std::uint64_t>         rateLimited{0};
    std::atomic<std::uint64_t>         connectionsTotal{0};
};

std::map<std::string, StaticFile> loadStaticFiles(const std::string& dir);

// Simple token bucket: protects the public demo from clients spamming orders.
class RateLimiter {
public:
    RateLimiter(double perSecond, double burst) : rate_(perSecond), burst_(burst), tokens_(burst) {}
    bool allow() {
        auto now = std::chrono::steady_clock::now();
        tokens_ = std::min(burst_, tokens_ + std::chrono::duration<double>(now - last_).count() * rate_);
        last_ = now;
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }

private:
    double rate_, burst_, tokens_;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    static constexpr std::size_t kMaxQueuedMessages = 2048;  // slow-consumer limit

    WsSession(tcp::socket&& socket, ServerContext& ctx, ClientId id);
    ~WsSession();

    void run(http::request<http::string_body> req);
    void send(SharedMsg msg);  // thread-safe, called from engine threads too
    ClientId id() const { return id_; }

private:
    void onAccept(beast::error_code ec);
    void doRead();
    void onRead(beast::error_code ec, std::size_t bytes);
    void handleMessage(const std::string& text);
    void doWrite();
    void onWrite(beast::error_code ec, std::size_t bytes);

    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer                   buffer_;
    std::deque<SharedMsg>                outbox_;
    ServerContext&                       ctx_;
    ClientId                             id_;
    RateLimiter                          limiter_{50, 100};
    bool                                 closing_ = false;
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket&& socket, ServerContext& ctx) : stream_(std::move(socket)), ctx_(ctx) {}
    void run();

private:
    void doRead();
    void onRead(beast::error_code ec, std::size_t bytes);
    void onWrite(bool keepAlive, beast::error_code ec, std::size_t bytes);

    beast::tcp_stream                 stream_;
    beast::flat_buffer                buffer_;
    http::request<http::string_body>  req_;
    std::shared_ptr<void>             res_;
    ServerContext&                    ctx_;
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(net::io_context& ioc, tcp::endpoint ep, ServerContext& ctx);
    void run() { doAccept(); }
    void stop();

private:
    void doAccept();
    void onAccept(beast::error_code ec, tcp::socket socket);

    net::io_context& ioc_;
    tcp::acceptor    acceptor_;
    ServerContext&   ctx_;
};

}  // namespace nx
