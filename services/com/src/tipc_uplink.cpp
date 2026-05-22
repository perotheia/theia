#include "com/tipc_uplink.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <linux/tipc.h>
#include <sys/socket.h>
#include <unistd.h>

namespace services_com {

namespace {

// Mirror of the supervisor's tipc_publisher tags.
constexpr uint16_t kTagControlReply = 0x0101;

void write_be16(char* dst, uint16_t v) {
    dst[0] = static_cast<char>((v >> 8) & 0xff);
    dst[1] = static_cast<char>(v & 0xff);
}

void log_err(const char* fn, const char* msg) {
    std::fprintf(stderr, "com/tipc_uplink: %s: %s\n", fn, msg);
}

}  // namespace

TipcUplink::TipcUplink(uint32_t t, uint32_t i)
    : type_(t), instance_(i) {}

TipcUplink::~TipcUplink() { stop(); }

bool TipcUplink::start() {
    fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd_ < 0) { log_err("socket", std::strerror(errno)); return false; }

    struct sockaddr_tipc addr{};
    addr.family                  = AF_TIPC;
    addr.addrtype                = TIPC_ADDR_NAME;
    addr.addr.name.name.type     = type_;
    addr.addr.name.name.instance = instance_;
    addr.scope                   = TIPC_NODE_SCOPE;
    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_err("connect", std::strerror(errno));
        ::close(fd_); fd_ = -1;
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void TipcUplink::stop() {
    if (!running_.exchange(false)) return;
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    // Wake any pending control-request waiters.
    std::lock_guard<std::mutex> lk(pending_mtx_);
    for (auto& kv : pending_) {
        std::lock_guard<std::mutex> p(kv.second->mtx);
        kv.second->have_reply = true;        // sentinel: socket gone
        kv.second->cv.notify_all();
    }
    pending_.clear();
}

std::shared_ptr<Subscriber> TipcUplink::subscribe() {
    auto s = std::make_shared<Subscriber>();
    std::lock_guard<std::mutex> lk(sub_mtx_);
    subs_.push_back(s);
    return s;
}

void TipcUplink::unsubscribe(const std::shared_ptr<Subscriber>& s) {
    if (!s) return;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->closed = true;
    }
    s->cv.notify_all();
    std::lock_guard<std::mutex> lk(sub_mtx_);
    subs_.erase(
        std::remove_if(subs_.begin(), subs_.end(),
                       [&](const std::weak_ptr<Subscriber>& w) {
                           auto sp = w.lock();
                           return !sp || sp.get() == s.get();
                       }),
        subs_.end());
}

bool TipcUplink::send_control_request(const std::string& serialized_request,
                                       uint64_t correlation_id,
                                       std::string& out_reply_payload,
                                       int timeout_ms) {
    if (fd_ < 0) return false;
    auto pending = std::make_shared<Pending>();
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_[correlation_id] = pending;
    }

    // Frame: u16_be(0x0100) + protobuf.
    std::string buf;
    buf.resize(2 + serialized_request.size());
    write_be16(&buf[0], 0x0100);
    std::memcpy(&buf[2], serialized_request.data(), serialized_request.size());
    ssize_t n = ::send(fd_, buf.data(), buf.size(), MSG_NOSIGNAL);
    if (n < 0) {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_.erase(correlation_id);
        return false;
    }

    // Wait for the reader to pick up our reply.
    std::unique_lock<std::mutex> lk(pending->mtx);
    bool ok = pending->cv.wait_for(
        lk, std::chrono::milliseconds(timeout_ms),
        [&] { return pending->have_reply; });
    if (ok) out_reply_payload = std::move(pending->reply_payload);

    {
        std::lock_guard<std::mutex> g(pending_mtx_);
        pending_.erase(correlation_id);
    }
    return ok && !out_reply_payload.empty();
}

void TipcUplink::run() {
    while (running_.load()) {
        char buf[65536];
        ssize_t r = ::recv(fd_, buf, sizeof(buf), 0);
        if (r <= 0) {
            if (running_.load()) {
                log_err("recv", r == 0 ? "peer closed" : std::strerror(errno));
            }
            break;
        }
        if (r < 2) continue;
        uint16_t tag = (static_cast<uint8_t>(buf[0]) << 8) |
                       static_cast<uint8_t>(buf[1]);
        std::string payload(buf + 2, static_cast<size_t>(r) - 2);

        if (tag == kTagControlReply) {
            // Pull correlation_id (field 4, varint) from the payload.
            // Cheap parser to avoid pulling protobuf for a single field.
            uint64_t corr = 0;
            // Walk fields; protobuf wire format: tag = (field<<3)|wire.
            size_t i = 0;
            while (i < payload.size()) {
                uint8_t fn_tag = static_cast<uint8_t>(payload[i++]);
                uint32_t fid = fn_tag >> 3;
                uint32_t wt  = fn_tag & 0x7;
                if (wt == 0) {           // varint
                    uint64_t v = 0; int sh = 0;
                    while (i < payload.size()) {
                        uint8_t b = static_cast<uint8_t>(payload[i++]);
                        v |= uint64_t(b & 0x7f) << sh;
                        if (!(b & 0x80)) break;
                        sh += 7;
                    }
                    if (fid == 4) { corr = v; break; }
                } else if (wt == 2) {    // length-delimited — skip
                    uint64_t len = 0; int sh = 0;
                    while (i < payload.size()) {
                        uint8_t b = static_cast<uint8_t>(payload[i++]);
                        len |= uint64_t(b & 0x7f) << sh;
                        if (!(b & 0x80)) break;
                        sh += 7;
                    }
                    i += static_cast<size_t>(len);
                } else { break; }
            }
            std::shared_ptr<Pending> p;
            {
                std::lock_guard<std::mutex> lk(pending_mtx_);
                auto it = pending_.find(corr);
                if (it != pending_.end()) p = it->second;
            }
            if (p) {
                std::lock_guard<std::mutex> lk(p->mtx);
                p->reply_payload = std::move(payload);
                p->have_reply    = true;
                p->cv.notify_all();
            }
            continue;
        }

        // Broadcast to every live subscriber.
        Frame f{tag, std::move(payload)};
        std::vector<std::shared_ptr<Subscriber>> live;
        {
            std::lock_guard<std::mutex> lk(sub_mtx_);
            for (auto it = subs_.begin(); it != subs_.end(); ) {
                auto sp = it->lock();
                if (sp) { live.push_back(sp); ++it; }
                else    { it = subs_.erase(it); }
            }
        }
        for (auto& s : live) {
            std::lock_guard<std::mutex> lk(s->mtx);
            if (s->closed) continue;
            s->queue.push_back(f);
            s->cv.notify_one();
        }
    }
    // On exit, mark every subscriber closed so streams wake & finish.
    std::lock_guard<std::mutex> lk(sub_mtx_);
    for (auto& w : subs_) {
        if (auto sp = w.lock()) {
            std::lock_guard<std::mutex> sl(sp->mtx);
            sp->closed = true;
            sp->cv.notify_all();
        }
    }
}

}  // namespace services_com
