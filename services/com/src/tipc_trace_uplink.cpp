#include "com/tipc_trace_uplink.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <linux/tipc.h>
#include <sys/socket.h>
#include <unistd.h>

namespace services_com {

namespace {

// Outbound: TraceConfigRequest carried to TraceCollector via this tag.
// TraceCollector forwards it via the supervisor's NodeTraceCtl push
// (#361); from the gRPC bridge's perspective the call is fire-and-
// forget.
constexpr uint16_t kTagTraceConfig = 0x0301;

void log_err(const char* fn, const char* msg) {
    std::fprintf(stderr, "com/tipc_trace_uplink: %s: %s\n", fn, msg);
}

void write_be16(char* dst, uint16_t v) {
    dst[0] = static_cast<char>((v >> 8) & 0xff);
    dst[1] = static_cast<char>(v & 0xff);
}

}  // namespace

TipcTraceUplink::TipcTraceUplink(uint32_t t, uint32_t i)
    : type_(t), instance_(i) {}

TipcTraceUplink::~TipcTraceUplink() { stop(); }

bool TipcTraceUplink::start() {
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

void TipcTraceUplink::stop() {
    if (!running_.exchange(false)) return;
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

std::shared_ptr<TraceSubscriber> TipcTraceUplink::subscribe() {
    auto s = std::make_shared<TraceSubscriber>();
    std::lock_guard<std::mutex> lk(sub_mtx_);
    subs_.push_back(s);
    return s;
}

void TipcTraceUplink::unsubscribe(const std::shared_ptr<TraceSubscriber>& s) {
    if (!s) return;
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->closed = true;
    }
    s->cv.notify_all();
    std::lock_guard<std::mutex> lk(sub_mtx_);
    subs_.erase(
        std::remove_if(subs_.begin(), subs_.end(),
                       [&](const std::weak_ptr<TraceSubscriber>& w) {
                           auto sp = w.lock();
                           return !sp || sp.get() == s.get();
                       }),
        subs_.end());
}

bool TipcTraceUplink::send_config_request(const std::string& serialized) {
    if (fd_ < 0 || !running_.load()) {
        log_err("send_config_request", "uplink not running");
        return false;
    }
    // [u16 tag BE][payload]
    std::string frame(2 + serialized.size(), '\0');
    write_be16(frame.data(), kTagTraceConfig);
    std::memcpy(frame.data() + 2, serialized.data(), serialized.size());
    ssize_t n = ::send(fd_, frame.data(), frame.size(), 0);
    if (n < 0) { log_err("send", std::strerror(errno)); return false; }
    return static_cast<size_t>(n) == frame.size();
}

void TipcTraceUplink::run() {
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

        // For TraceStream we treat ALL inbound frames as records to
        // fan out — tag discrimination is reserved for future wire-
        // shape extensions (e.g. heartbeat from the collector).
        (void)tag;

        TraceFrame f{tag, std::move(payload)};
        std::vector<std::shared_ptr<TraceSubscriber>> live;
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
    // On exit, wake every subscriber so streams finish.
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
