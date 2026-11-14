// gw_link — see gw_link.hpp. Raw bytes only: framing from TheiaMsgHeader, the service id
// derived from the request's wire type name (the same djb2 the generated codecs use).
#include "impl/gw_link.hpp"

#include "PgClient.hh"        // join_raw_named (pg feed by name), TheiaMsgHeader, hash_msg_type_

#include <sys/socket.h>
#include <linux/tipc.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <thread>

namespace services_com {

namespace {
uint64_t wall_ns() {
    timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}
std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out; std::string cur; std::istringstream is(s);
    while (std::getline(is, cur, d)) {
        while (!cur.empty() && (cur.front() == ' ')) cur.erase(cur.begin());
        while (!cur.empty() && (cur.back() == ' ')) cur.pop_back();
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}
}  // namespace

struct Feed {
    std::string name;
    ::theia::runtime::PgClient pg;           // one PgClient per feed: one raw sink each
    std::mutex mu;
    std::condition_variable cv;
    std::deque<GwFrameRec> ring;
    uint64_t next_seq = 1;
    size_t cap = 4096;
    std::atomic<bool> joined{false};
};

struct GwLink::Impl {
    std::map<std::string, std::pair<uint32_t, uint32_t>> calls;   // iface -> {tipc type, instance}
    std::map<std::string, std::unique_ptr<Feed>> feeds;
    std::atomic<bool> running{false};
    std::thread joiner;
};

GwLink::GwLink() : impl_(new Impl()) {}
GwLink::~GwLink() { stop(); delete impl_; }
GwLink& GwLink::instance() { static GwLink g; return g; }

bool GwLink::configure(const std::string& calls, const std::string& feeds, size_t ring) {
    std::map<std::string, std::pair<uint32_t, uint32_t>> c;
    for (const auto& e : split(calls, ',')) {
        const auto eq = e.find('='), co = e.rfind(':');
        if (eq == std::string::npos || co == std::string::npos || co < eq) {
            std::fprintf(stderr, "[gateway] bad gateway_calls entry '%s' (want iface=0xTYPE:INST)\n", e.c_str());
            return false;
        }
        c[e.substr(0, eq)] = {static_cast<uint32_t>(std::strtoul(e.substr(eq + 1, co - eq - 1).c_str(), nullptr, 0)),
                              static_cast<uint32_t>(std::strtoul(e.substr(co + 1).c_str(), nullptr, 0))};
    }
    impl_->calls = std::move(c);
    impl_->feeds.clear();
    for (const auto& f : split(feeds, ',')) {
        auto fd = std::make_unique<Feed>();
        fd->name = f; fd->cap = ring;
        impl_->feeds[f] = std::move(fd);
    }
    return true;
}

bool GwLink::start() {
    if (impl_->running.exchange(true)) return true;
    for (auto& [name, f] : impl_->feeds) f->pg.attach("com-gateway", /*binding=*/nullptr);
    // Join every feed, retrying with backoff while the supervisor is busy or down: the
    // gateway must never die of a slow join (the tap's lesson, 2026-11-13).
    impl_->joiner = std::thread([this] {
        int delay_ms = 500;
        while (impl_->running.load()) {
            bool all = true;
            for (auto& [name, fp] : impl_->feeds) {
                Feed* f = fp.get();
                if (f->joined.load()) continue;
                auto g = f->pg.join_raw_named(f->name.c_str(), [f](const uint8_t* p, uint16_t len) {
                    std::lock_guard<std::mutex> l(f->mu);
                    f->ring.push_back({f->next_seq++, wall_ns(), std::string(reinterpret_cast<const char*>(p), len)});
                    while (f->ring.size() > f->cap) f->ring.pop_front();
                    f->cv.notify_all();
                });
                if (g.ok) {
                    f->joined.store(true);
                    std::fprintf(stderr, "[gateway] joined feed %s (group 0x%08x)\n", f->name.c_str(), g.type);
                } else {
                    all = false;
                }
            }
            if (all) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms = std::min(delay_ms * 2, 5000);
        }
    });
    return true;
}

void GwLink::stop() {
    if (!impl_->running.exchange(false)) return;
    if (impl_->joiner.joinable()) impl_->joiner.join();
    for (auto& [name, f] : impl_->feeds) { f->pg.shutdown(); f->cv.notify_all(); }
}

bool GwLink::feed_allowed(const std::string& msg_type) const {
    return impl_->feeds.count(msg_type) > 0;
}

int GwLink::call(const std::string& iface, const std::string& req_type, const std::string& req,
                 uint32_t timeout_ms, std::string& reply, std::string& error) {
    auto it = impl_->calls.find(iface);
    if (it == impl_->calls.end()) { error = "interface not allowlisted: " + iface; return 3; }
    if (req.size() > ::theia::runtime::kMaxPgPayload) { error = "request too large"; return 3; }
    int fd = ::socket(AF_TIPC, SOCK_SEQPACKET, 0);
    if (fd < 0) { error = "no TIPC socket"; return 14; }
    sockaddr_tipc addr{};
    addr.family = AF_TIPC;
    addr.addrtype = TIPC_SERVICE_ADDR;
    addr.addr.name.name.type = it->second.first;
    addr.addr.name.name.instance = it->second.second;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        char b[96]; std::snprintf(b, sizeof b, "service 0x%08x:%u unreachable", it->second.first, it->second.second);
        error = b; return 14;
    }
    ::theia::runtime::TheiaMsgHeader hdr{};
    hdr.bus_type = ::theia::runtime::kBusTypeRpc;
    hdr.msg_type = ::theia::runtime::kMsgGenCall;
    hdr.proto_len = static_cast<uint16_t>(req.size());
    hdr.rpc.service_id = ::theia::runtime::hash_msg_type_(req_type.c_str());
    hdr.rpc.method_id = 0;
    static std::atomic<uint32_t> corr{1};
    hdr.rpc.correlation_id = corr++;
    std::string frame(sizeof(hdr) + req.size(), '\0');
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    std::memcpy(frame.data() + sizeof(hdr), req.data(), req.size());
    if (::send(fd, frame.data(), frame.size(), 0) < 0) { ::close(fd); error = "send failed"; return 14; }
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(timeout_ms ? timeout_ms : 2000)) <= 0) {
        ::close(fd); error = "no reply from " + iface + " (" + req_type + ") in time"; return 14;
    }
    std::string buf(66000, '\0');
    ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    ::close(fd);
    if (n < static_cast<ssize_t>(sizeof(hdr))) { error = "short reply"; return 14; }
    ::theia::runtime::TheiaMsgHeader rh{};
    std::memcpy(&rh, buf.data(), sizeof(rh));
    const size_t avail = static_cast<size_t>(n) - sizeof(rh);
    if (rh.proto_len > avail) { error = "truncated reply"; return 14; }
    reply.assign(buf.data() + sizeof(rh), rh.proto_len);
    return 0;
}

std::vector<GwFrameRec> GwLink::frames_since(const std::string& msg_type, uint64_t since,
                                             int wait_ms, size_t max) {
    std::vector<GwFrameRec> out;
    auto it = impl_->feeds.find(msg_type);
    if (it == impl_->feeds.end()) return out;
    Feed* f = it->second.get();
    std::unique_lock<std::mutex> l(f->mu);
    auto have = [&] { return !f->ring.empty() && f->ring.back().seq > since; };
    if (!have() && wait_ms > 0)
        f->cv.wait_for(l, std::chrono::milliseconds(wait_ms), have);
    for (const auto& r : f->ring) {
        if (r.seq <= since) continue;
        out.push_back(r);
        if (out.size() >= max) break;
    }
    return out;
}

}  // namespace services_com
