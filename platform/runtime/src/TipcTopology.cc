// TipcTopology — see TipcTopology.hh. Connects to the TIPC topology server and
// turns PUBLISHED/WITHDRAWN events into a live presence set + a change callback.

#include "TipcTopology.hh"

#include "Logger.hh"   // theia::runtime::process_logger()

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <linux/tipc.h>
#include <sys/socket.h>
#include <unistd.h>

namespace theia {
namespace runtime {

TipcTopology::~TipcTopology() { stop(); }

bool TipcTopology::connect_() {
    int fd = ::socket(AF_TIPC, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        process_logger().warn(std::string("[TipcTopology] socket(AF_TIPC): ") +
                              std::strerror(errno));
        return false;
    }
    // Connect to the kernel topology server, service {TIPC_TOP_SRV,TIPC_TOP_SRV}.
    struct sockaddr_tipc topsrv{};
    topsrv.family                  = AF_TIPC;
    topsrv.addrtype                = TIPC_SERVICE_ADDR;
    topsrv.addr.name.name.type     = TIPC_TOP_SRV;
    topsrv.addr.name.name.instance = TIPC_TOP_SRV;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&topsrv), sizeof(topsrv)) < 0) {
        process_logger().warn(std::string("[TipcTopology] connect(TIPC_TOP_SRV): ") +
                              std::strerror(errno));
        ::close(fd);
        return false;
    }
    fd_ = fd;
    return true;
}

bool TipcTopology::send_subscr_(const Range& r) {
    struct tipc_subscr sub{};
    sub.seq.type  = r.type;
    sub.seq.lower = r.lower;
    sub.seq.upper = r.upper;
    sub.timeout   = TIPC_WAIT_FOREVER;
    // TIPC_SUB_PORTS: an event for EACH matching (type,instance) bind/unbind —
    // we want per-instance granularity (which machines have shwa), not just
    // first-up/last-down (that's TIPC_SUB_SERVICE).
    sub.filter    = TIPC_SUB_PORTS;
    if (::send(fd_, &sub, sizeof(sub), 0) != static_cast<ssize_t>(sizeof(sub))) {
        process_logger().warn(std::string("[TipcTopology] send(subscr): ") +
                              std::strerror(errno));
        return false;
    }
    return true;
}

bool TipcTopology::subscribe(uint32_t type, uint32_t lower, uint32_t upper) {
    Range r{type, lower, upper};
    {
        std::lock_guard<std::mutex> lk(mu_);
        ranges_.push_back(r);
    }
    // If already running, send it live; else it's flushed at start().
    if (running_.load() && fd_ >= 0) return send_subscr_(r);
    return true;
}

bool TipcTopology::start(Callback on_change) {
    if (running_.exchange(true)) return true;   // already started
    if (!connect_()) { running_.store(false); return false; }
    cb_ = std::move(on_change);
    // Flush queued subscriptions.
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& r : ranges_) send_subscr_(r);
    }
    thread_ = std::thread([this] { run_(); });
    return true;
}

void TipcTopology::run_() {
    process_logger().info("[TipcTopology] presence discovery up "
                          "(TIPC_TOP_SRV subscribed)");
    while (running_.load()) {
        struct tipc_event ev{};
        ssize_t n = ::recv(fd_, &ev, sizeof(ev), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            if (running_.load())
                process_logger().warn(std::string("[TipcTopology] recv ended: ") +
                                      (n == 0 ? "EOF" : std::strerror(errno)));
            break;
        }
        if (n != static_cast<ssize_t>(sizeof(ev))) continue;  // short/garbled

        // TIPC_SUB_PORTS gives one event per matching instance, so
        // found_lower == found_upper is the single instance that changed.
        const bool present = (ev.event == TIPC_PUBLISHED);
        if (ev.event != TIPC_PUBLISHED && ev.event != TIPC_WITHDRAWN) continue;
        const uint32_t type = ev.s.seq.type;
        const uint32_t inst = ev.found_lower;

        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const uint64_t k = key_(type, inst);
            if (present) changed = present_.insert(k).second;
            else         changed = (present_.erase(k) > 0);
        }
        if (changed && cb_) {
            cb_(TopologyEvent{type, inst, present});
        }
    }
    process_logger().info("[TipcTopology] presence discovery down");
}

void TipcTopology::stop() {
    if (!running_.exchange(false)) return;
    // Closing the fd unblocks the recv in run_().
    if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(mu_);
    present_.clear();
}

bool TipcTopology::present(uint32_t type, uint32_t instance) const {
    std::lock_guard<std::mutex> lk(mu_);
    return present_.count(key_(type, instance)) > 0;
}

std::vector<uint32_t> TipcTopology::instances_of(uint32_t type) const {
    std::vector<uint32_t> out;
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t lo = key_(type, 0);
    const uint64_t hi = key_(type, 0xFFFFFFFFu);
    for (auto it = present_.lower_bound(lo); it != present_.end() && *it <= hi; ++it) {
        out.push_back(static_cast<uint32_t>(*it & 0xFFFFFFFFu));
    }
    return out;
}

}  // namespace runtime
}  // namespace theia
