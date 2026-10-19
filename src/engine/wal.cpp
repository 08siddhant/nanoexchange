#include "engine/wal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace nx {

std::uint32_t fnv1a(std::string_view data) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : data) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

namespace {

void writeAll(int fd, const char* data, std::size_t len) {
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("WAL write failed: ") + std::strerror(errno));
        }
        data += n;
        len -= static_cast<std::size_t>(n);
    }
}

std::string withChecksum(std::string_view body) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "|%08x", fnv1a(body));
    std::string line(body);
    line += buf;
    return line;
}

// Returns false if the line's checksum does not match.
bool verifyLine(const std::string& line, std::string& body) {
    auto bar = line.rfind('|');
    if (bar == std::string::npos || line.size() - bar != 9) return false;
    body = line.substr(0, bar);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", fnv1a(body));
    return line.compare(bar + 1, 8, buf) == 0;
}

}  // namespace

Wal::Wal(std::filesystem::path path, bool fsyncEachWrite) : path_(std::move(path)), fsync_(fsyncEachWrite) {
    if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path());
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ < 0) throw std::runtime_error("cannot open WAL " + path_.string() + ": " + std::strerror(errno));
    std::error_code ec;
    bytes_ = std::filesystem::file_size(path_, ec);
    if (ec) bytes_ = 0;
}

Wal::~Wal() {
    if (fd_ >= 0) {
        ::fdatasync(fd_);
        ::close(fd_);
    }
}

void Wal::append(std::uint64_t lsn, std::string_view payload) {
    std::string body = std::to_string(lsn);
    body += ' ';
    body += payload;
    std::string line = withChecksum(body);
    line += '\n';
    writeAll(fd_, line.data(), line.size());  // one syscall -> atomic enough for a process crash
    if (fsync_) ::fdatasync(fd_);
    bytes_ += line.size();
}

void Wal::truncate() {
    if (::ftruncate(fd_, 0) != 0)
        throw std::runtime_error(std::string("WAL truncate failed: ") + std::strerror(errno));
    ::fdatasync(fd_);
    bytes_ = 0;
}

std::vector<WalRecord> Wal::readAll(const std::filesystem::path& path, std::size_t* corruptTail) {
    std::vector<WalRecord> out;
    if (corruptTail) *corruptTail = 0;
    std::ifstream in(path);
    if (!in) return out;

    std::string line, body;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (!verifyLine(line, body)) {
            // Torn write from a crash: everything after this point is untrusted.
            if (corruptTail) {
                *corruptTail = 1;
                while (std::getline(in, line)) ++*corruptTail;
            }
            break;
        }
        auto sp = body.find(' ');
        if (sp == std::string::npos) break;
        WalRecord r;
        r.lsn     = std::stoull(body.substr(0, sp));
        r.payload = body.substr(sp + 1);
        out.push_back(std::move(r));
    }
    return out;
}

void writeSnapshot(const std::filesystem::path& path, const SnapshotHeader& h, const OrderBook& book) {
    std::ostringstream os;
    os << "SNAP v1 " << h.lastLsn << ' ' << h.nextId << ' ' << h.tradeSeq << '\n';
    std::size_t count = 0;
    book.forEachOrder([&](const Order& o) {
        os << "O " << o.id << ' ' << o.client << ' ' << (o.side == Side::Buy ? 'B' : 'S') << ' ' << o.price << ' '
           << o.qty << ' ' << o.remaining << '\n';
        ++count;
    });
    os << "END " << count << '\n';
    std::string data = os.str();
    data = withChecksum(data) + "\n";

    // Write to a temp file, fsync it, then atomically rename over the old one.
    // A crash at any point leaves either the old or the new snapshot, never half.
    auto tmp = path;
    tmp += ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) throw std::runtime_error("cannot write snapshot: " + std::string(std::strerror(errno)));
    writeAll(fd, data.data(), data.size());
    ::fsync(fd);
    ::close(fd);
    std::filesystem::rename(tmp, path);
}

bool loadSnapshot(const std::filesystem::path& path, SnapshotHeader& h, OrderBook& book) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!all.empty() && all.back() == '\n') all.pop_back();
    std::string body;
    if (!verifyLine(all, body)) return false;  // checksum covers the whole file

    std::istringstream is(body);
    std::string tag, ver;
    is >> tag >> ver >> h.lastLsn >> h.nextId >> h.tradeSeq;
    if (tag != "SNAP" || ver != "v1") return false;

    std::vector<Order> orders;
    while (is >> tag) {
        if (tag == "END") break;
        if (tag != "O") return false;
        Order o;
        char side;
        is >> o.id >> o.client >> side >> o.price >> o.qty >> o.remaining;
        o.side = side == 'B' ? Side::Buy : Side::Sell;
        o.type = OrderType::Limit;
        orders.push_back(o);
    }
    for (const auto& o : orders) book.restore(o);
    book.setTradeSeq(h.tradeSeq);
    return true;
}

}  // namespace nx
