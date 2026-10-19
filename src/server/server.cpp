#include "server/server.hpp"

#include "server/protocol.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

namespace nx {

namespace {

ClientId newClientId() {
    // Random 48-bit id (fits in a JS number). Bots use small fixed ids.
    thread_local std::mt19937_64 rng{std::random_device{}()};
    return (rng() & ((1ULL << 48) - 1)) | (1ULL << 40);
}

std::string contentTypeFor(const std::filesystem::path& p) {
    const auto ext = p.extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".json") return "application/json";
    return "application/octet-stream";
}

}  // namespace

std::map<std::string, StaticFile> loadStaticFiles(const std::string& dir) {
    std::map<std::string, StaticFile> files;
    namespace fs = std::filesystem;
    if (!fs::exists(dir)) {
        std::cerr << "[warn] static dir not found: " << dir << "\n";
        return files;
    }
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string url = "/" + fs::relative(entry.path(), dir).generic_string();
        files[url] = {std::move(body), contentTypeFor(entry.path())};
    }
    return files;
}

// ============================ WebSocket session ============================

WsSession::WsSession(tcp::socket&& socket, ServerContext& ctx, ClientId id)
    : ws_(std::move(socket)), ctx_(ctx), id_(id) {}

WsSession::~WsSession() { ctx_.hub.leave(id_); }

void WsSession::run(http::request<http::string_body> req) {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));  // idle pings
    ws_.read_message_max(8 * 1024);
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) { res.set(http::field::server, "nanoexchange"); }));
    ws_.async_accept(req, beast::bind_front_handler(&WsSession::onAccept, shared_from_this()));
}

void WsSession::onAccept(beast::error_code ec) {
    if (ec) return;
    ctx_.connectionsTotal.fetch_add(1, std::memory_order_relaxed);
    ctx_.hub.join(shared_from_this());
    send(std::make_shared<const std::string>(proto::hello(id_, ctx_.exchange.symbols())));
    for (auto& m : ctx_.hub.welcomeMessages()) send(m);
    doRead();
}

void WsSession::doRead() {
    ws_.async_read(buffer_, beast::bind_front_handler(&WsSession::onRead, shared_from_this()));
}

void WsSession::onRead(beast::error_code ec, std::size_t) {
    if (ec) {  // closed, timed out or error
        closing_ = true;
        ctx_.hub.leave(id_);
        return;
    }
    std::string text = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());
    handleMessage(text);
    doRead();
}

void WsSession::handleMessage(const std::string& text) {
    ctx_.messagesIn.fetch_add(1, std::memory_order_relaxed);
    auto reply = [this](std::string s) { send(std::make_shared<const std::string>(std::move(s))); };

    if (!limiter_.allow()) {
        ctx_.rateLimited.fetch_add(1, std::memory_order_relaxed);
        reply(proto::error("rate limit exceeded (50 msg/s)"));
        return;
    }

    auto p = proto::parseClientMessage(text, id_);
    if (!p.error.empty()) {
        reply(proto::error(p.error, p.cmd.clOrdId));
        return;
    }
    if (p.op == proto::Op::Ping) {
        reply(R"({"type":"pong"})");
        return;
    }
    const std::string clOrdId = p.cmd.clOrdId;
    switch (ctx_.exchange.post(p.symbol, std::move(p.cmd))) {
        case Exchange::PostResult::Ok: break;
        case Exchange::PostResult::UnknownSymbol: reply(proto::error("unknown symbol", clOrdId)); break;
        case Exchange::PostResult::Busy: reply(proto::error("exchange busy, retry", clOrdId)); break;
    }
}

void WsSession::send(SharedMsg msg) {
    // Hop onto this connection's strand; the outbox is only touched there.
    net::post(ws_.get_executor(), [self = shared_from_this(), msg = std::move(msg)]() mutable {
        if (self->closing_) return;
        if (self->outbox_.size() >= kMaxQueuedMessages) {
            // Client can't keep up: disconnect it instead of buffering forever.
            self->closing_ = true;
            self->outbox_.clear();
            self->ctx_.hub.countSlowDrop();
            beast::get_lowest_layer(self->ws_).close();
            return;
        }
        self->outbox_.push_back(std::move(msg));
        if (self->outbox_.size() == 1) self->doWrite();  // otherwise a write is already in flight
    });
}

void WsSession::doWrite() {
    ws_.text(true);
    ws_.async_write(net::buffer(*outbox_.front()), beast::bind_front_handler(&WsSession::onWrite, shared_from_this()));
}

void WsSession::onWrite(beast::error_code ec, std::size_t) {
    if (ec) {
        closing_ = true;
        outbox_.clear();
        return;
    }
    outbox_.pop_front();
    if (!outbox_.empty()) doWrite();
}

// =============================== HTTP session ==============================

void HttpSession::run() {
    net::dispatch(stream_.get_executor(), beast::bind_front_handler(&HttpSession::doRead, shared_from_this()));
}

void HttpSession::doRead() {
    req_ = {};
    stream_.expires_after(std::chrono::seconds(30));
    http::async_read(stream_, buffer_, req_, beast::bind_front_handler(&HttpSession::onRead, shared_from_this()));
}

void HttpSession::onRead(beast::error_code ec, std::size_t) {
    if (ec == http::error::end_of_stream) {
        beast::error_code ignored;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
        return;
    }
    if (ec) return;

    std::string target(req_.target());
    if (auto q = target.find('?'); q != std::string::npos) target.resize(q);

    if (websocket::is_upgrade(req_)) {
        if (target != "/ws") {
            beast::error_code ignored;
            stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
            return;
        }
        stream_.expires_never();
        std::make_shared<WsSession>(stream_.release_socket(), ctx_, newClientId())->run(std::move(req_));
        return;
    }

    auto respond = [&](http::status status, std::string contentType, std::string body, bool cache = false) {
        auto res = std::make_shared<http::response<http::string_body>>(status, req_.version());
        res->set(http::field::server, "nanoexchange");
        res->set(http::field::content_type, contentType);
        res->set(http::field::cache_control, cache ? "public, max-age=300" : "no-store");
        res->keep_alive(req_.keep_alive());
        if (req_.method() != http::verb::head) res->body() = std::move(body);
        res->prepare_payload();
        if (req_.method() == http::verb::head) res->content_length(res->body().size());
        res_ = res;
        http::async_write(stream_, *res,
                          beast::bind_front_handler(&HttpSession::onWrite, shared_from_this(), res->keep_alive()));
    };

    if (req_.method() != http::verb::get && req_.method() != http::verb::head) {
        respond(http::status::method_not_allowed, "text/plain", "method not allowed\n");
        return;
    }
    if (target == "/health" || target == "/healthz") {
        nlohmann::json j{{"status", "ok"}, {"symbols", ctx_.exchange.symbols()}, {"clients", ctx_.hub.clients()}};
        respond(http::status::ok, "application/json", j.dump());
        return;
    }
    if (target == "/metrics") {
        respond(http::status::ok, "text/plain; version=0.0.4", ctx_.renderMetrics());
        return;
    }
    if (target == "/") target = "/index.html";
    if (target.find("..") == std::string::npos) {
        if (auto it = ctx_.files.find(target); it != ctx_.files.end()) {
            respond(http::status::ok, it->second.contentType, it->second.body, target != "/index.html");
            return;
        }
    }
    respond(http::status::not_found, "text/plain", "not found\n");
}

void HttpSession::onWrite(bool keepAlive, beast::error_code ec, std::size_t) {
    if (ec) return;
    if (!keepAlive) {
        beast::error_code ignored;
        stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
        return;
    }
    res_.reset();
    doRead();
}

// ================================= Listener ================================

Listener::Listener(net::io_context& ioc, tcp::endpoint ep, ServerContext& ctx)
    : ioc_(ioc), acceptor_(net::make_strand(ioc)), ctx_(ctx) {
    acceptor_.open(ep.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen(net::socket_base::max_listen_connections);
}

void Listener::stop() {
    beast::error_code ignored;
    acceptor_.close(ignored);
}

void Listener::doAccept() {
    acceptor_.async_accept(net::make_strand(ioc_), beast::bind_front_handler(&Listener::onAccept, shared_from_this()));
}

void Listener::onAccept(beast::error_code ec, tcp::socket socket) {
    if (!ec) {
        // Disable Nagle's algorithm: small messages (acks) go out immediately
        // instead of waiting up to ~40 ms to be coalesced with delayed ACKs.
        beast::error_code ignored;
        socket.set_option(tcp::no_delay(true), ignored);
        std::make_shared<HttpSession>(std::move(socket), ctx_)->run();
    }
    if (acceptor_.is_open()) doAccept();
}

}  // namespace nx
