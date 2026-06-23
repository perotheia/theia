// theia::runtime — process-group (pg) client. The node side of the TIPC
// name-sequence MULTICAST broadcast model.
//
// A group is the set of nodes interested in ONE wire message type T. Its IDENTITY
// is T's wire NAME (msg_type_name<T>() — a generated FC-header constant from the
// .art; NO free-form strings, no collision, no slippage). The SUPERVISOR is the
// namespace authority: pg_join<T>() is a CALL → the supervisor allocates the
// group's TIPC TYPE (0x8003 space, once per name) + a UNIQUE INSTANCE, and returns
// {group_type, instance}. The member BINDS {group_type, instance} on a recv
// socket; a broadcaster sends ONE datagram to the name-sequence {group_type,
// 0..~0, CLUSTER_SCOPE} → the kernel delivers a copy to EVERY member (NOT anycast
// — distinct instances of a shared type). pg_leave frees the instance.
//
// No PID identity (the instance is the group-domain id; the supervisor's watchdog
// frees a dead member's instance via the heartbeat). No per-member cast loop, no
// membership cache for delivery — the kernel fans out.
//
// RECEIVE path: the PG recv socket is a SOCK_RDM datagram (matching the
// broadcaster's nameseq multicast — SEQPACKET cannot receive multicast), so it
// canNOT live on the SEQPACKET TipcMux. Instead PgClient owns its own recv thread
// (the established runtime precedent — TraceStreamPump does the same) and
// dispatches each frame THROUGH THE NODE'S EXISTING demux: it looks up
// binding->entries[service_id] (populated by register_cast<T>) and calls the same
// decode→enqueue closure the mux would. One demux table, two ingress sockets.
//
// The CALL to the supervisor is HAND-FRAMED (request + reply parsed by a minimal
// proto3 reader) so the runtime does NOT depend on the supervisor proto (a cycle)
// — the same trick HeartbeatPublisher uses for its cast.
#pragma once

#include "RemoteCodec.hh"        // hash_msg_type_, msg_type_name<T>, RemoteCodec
#include "TheiaMsgHeader.hh"     // TheiaMsgHeader, kBusType*, kMsgGen*
#include "TipcMux.hh"            // NodeBinding, InboundEntry (the node's demux)

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <linux/tipc.h>
#include <unistd.h>
#include <poll.h>

namespace theia { namespace runtime {

class PgClient {
public:
    // The supervisor's control node (SupervisorControlIf — the CALL target).
    static constexpr uint32_t kSupTipcType     = 0x80020001u;
    static constexpr uint32_t kSupTipcInstance = 0u;

    // The CALL request service_ids (djb2 of the nanopb request type name, matching
    // the supervisor's register_call<PgJoinReq,...> / <PgLeaveReq,...> / PgWatch).
    static constexpr uint16_t kJoinSid  = hash_msg_type_("system_supervisor_PgJoinReq");
    static constexpr uint16_t kLeaveSid = hash_msg_type_("system_supervisor_PgLeaveReq");
    static constexpr uint16_t kWatchSid = hash_msg_type_("system_supervisor_PgWatchReq");
    // The supervisor casts PgMembership pushes to a watcher with this service_id.
    static constexpr uint16_t kMembershipSid =
        hash_msg_type_("system_supervisor_PgMembership");

    // Result of a join/resolve: the group's allocated TIPC type + (for join) this
    // member's instance. ok=false if the supervisor was unreachable.
    struct Group { bool ok{false}; uint32_t type{0}; uint32_t instance{0}; };

    // One group member's delivery address (where a producer casts records).
    struct Member { uint32_t type{0}; uint32_t instance{0}; };

    PgClient() : self_pid_(::getpid()), node_name_("pg") {}
    ~PgClient() { shutdown(); if (call_fd_ >= 0) ::close(call_fd_); }
    PgClient(const PgClient&) = delete;
    PgClient& operator=(const PgClient&) = delete;

    // Bind this client to its owning node's identity + demux table. `binding` is
    // the NodeBinding returned by TipcMux::bind_node — its entries[service_id]
    // map (filled by register_cast<T>) is how a received group frame reaches
    // handle_cast. `node_name` is the node's kNodeName (CALL field + watchdog key).
    void attach(const std::string& node_name, NodeBinding* binding) {
        node_name_ = node_name;
        binding_   = binding;
    }

    // RECEIVER: join the group for message T. CALL the supervisor → it allocates
    // {group_type, instance}; bind a SOCK_RDM recv socket at that address and
    // start (or extend) the recv thread that dispatches group frames through the
    // node's demux. group name = msg_type_name<T>() (the .art-derived wire name).
    template <typename T> Group join() {
        Group g = call_join_(msg_type_name<T>(), /*join=*/true);
        if (g.ok) bind_recv_(g.type, g.instance, msg_type_name<T>());
        return g;
    }
    // BROADCASTER: resolve the group's TIPC type (allocate it if new) WITHOUT
    // taking an instance — so a pure sender learns where to multicast.
    template <typename T> Group resolve() {
        return call_join_(msg_type_name<T>(), /*join=*/false);
    }
    template <typename T> void leave() {
        unbind_recv_(msg_type_name<T>());
    }

    // ---- OTP pg:monitor — the PRODUCER side -------------------------------
    // watch<T>(on_change): PgWatch the group (the supervisor returns the current
    // member list AND starts pushing PgMembership on every join/leave/reap).
    // Caches the member list; registers a handler on the node's binding so future
    // pushes refresh the cache + fire on_change (the {pg_join}/{pg_leave} hook —
    // OTP `pg:monitor`). The producer then broadcast<T>()s by looping the cache.
    // The watcher address = the node's OWN binding ({watcher_type, instance}); the
    // supervisor casts PgMembership there. Pass the node's bound addr.
    template <typename T>
    Group watch(uint32_t watcher_type, uint32_t watcher_instance,
                std::function<void()> on_change = {}) {
        const char* gname = msg_type_name<T>();
        uint16_t sid = RemoteCodec<T>::service_id;
        {
            std::lock_guard<std::mutex> lk(wmu_);
            watched_[sid].on_change = std::move(on_change);
            watched_[sid].group_name = gname;
        }
        // Receive the supervisor's PgMembership pushes. A GenServer watcher has a
        // binding (the push arrives on its mux → dispatch_frame_); a runnable
        // watcher (e.g. the LOG pump) has none, so bind a bare recv socket at the
        // watcher addr + run the recv thread (the push routes to apply_membership_
        // by kMembershipSid). Both end at apply_membership_.
        if (binding_) install_membership_handler_();
        else if (watcher_type) bind_recv_(watcher_type, watcher_instance,
                                          "__pg_watch__");
        Group g = call_watch_(gname, watcher_type, watcher_instance, /*watch=*/true);
        return g;   // members already cached by call_watch_'s reply parse
    }
    // The producer's current view of group T's members (the watch cache).
    template <typename T> std::vector<Member> members() {
        std::lock_guard<std::mutex> lk(wmu_);
        auto it = watched_.find(RemoteCodec<T>::service_id);
        return it == watched_.end() ? std::vector<Member>{} : it->second.members;
    }
    template <typename T> void unwatch(uint32_t wt, uint32_t wi) {
        call_watch_(msg_type_name<T>(), wt, wi, /*watch=*/false);
        std::lock_guard<std::mutex> lk(wmu_);
        watched_.erase(RemoteCodec<T>::service_id);
    }

    // PRODUCER broadcast — OTP `[Pid ! Msg || Pid <- pg:get_members(group)]`.
    // Loop the WATCHED member cache and cast `payload` to each member's delivery
    // address. The impl drives this (via the generated broadcast_*), so it can
    // inspect members<T>() first (empty → stop work, e.g. logcat) or branch on
    // its own state. service_id = T's id (the members' register_cast demux key).
    template <typename T>
    void broadcast_members(const uint8_t* payload, uint16_t len) {
        std::vector<Member> snap;
        {
            std::lock_guard<std::mutex> lk(wmu_);
            auto it = watched_.find(RemoteCodec<T>::service_id);
            if (it != watched_.end()) snap = it->second.members;
        }
        for (const auto& m : snap)
            send_unicast_(m.type, m.instance, RemoteCodec<T>::service_id, payload, len);
    }

    // RECEIVER (non-node consumer, e.g. com's trace_link): join group T and route
    // each received record's RAW proto bytes to `sink` — no NodeBinding/demux
    // needed. The sink runs on the recv thread; keep it cheap (copy + enqueue).
    // Used where the consumer is not a GenServer node (a gRPC bridge, a tool).
    template <typename T>
    Group join_raw(std::function<void(const uint8_t*, uint16_t)> sink) {
        return join_raw_named(msg_type_name<T>(), std::move(sink));
    }

    // Same, but the group is named by STRING — for a non-node consumer that
    // deliberately keeps the message's proto/RemoteCodec header off its TU (e.g.
    // com's edge links, which forward raw bytes to the gRPC side). `group_name`
    // MUST be the wire type name (== msg_type_name<T>() on the producer side).
    Group join_raw_named(const char* group_name,
                         std::function<void(const uint8_t*, uint16_t)> sink) {
        raw_sink_ = std::move(sink);
        Group g = call_join_(group_name, /*join=*/true);
        if (g.ok) bind_recv_(g.type, g.instance, group_name);
        return g;
    }

    // Broadcast `payload` (already proto-encoded T) to the WHOLE group: one
    // datagram to the name-sequence {group_type, 0..~0} → kernel multicast to all
    // members. service_id = T's service_id (the members' register_cast demux key).
    // Best-effort (lossy, like UDP). `group_type` is the value resolve<T>() or
    // join<T>() returned.
    template <typename T>
    void broadcast(uint32_t group_type, const uint8_t* payload, uint16_t len) {
        send_multicast_(group_type, RemoteCodec<T>::service_id, payload, len);
    }

    void shutdown() {
        running_ = false;
        if (recv_thread_.joinable()) recv_thread_.join();
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& b : bound_) if (b.fd >= 0) ::close(b.fd);
        bound_.clear();
    }

private:
    struct Bound {
        std::string group_name;
        uint32_t    type{0};
        uint32_t    instance{0};
        int         fd{-1};
    };

    // ---- the supervisor CALL (hand-framed; no supervisor-proto dep) --------
    Group call_join_(const char* group_name, bool join) {
        std::string req;
        pb_string(req, 1, node_name_.c_str(), node_name_.size());  // node_name
        pb_varint_field(req, 2, zigzag32(self_pid_));              // pid (sint32)
        pb_string(req, 3, group_name, std::strlen(group_name));    // group_name
        if (join) pb_varint_field(req, 4, 1);                      // join=true
        std::string reply;
        Group g;
        if (!call_(kJoinSid, req, reply)) return g;
        // PgJoinReply { uint32 status=1; uint32 group_type=2; uint32 instance=3 }
        uint64_t status = 0, type = 0, inst = 0;
        pb_read_uint(reply, 1, status);
        pb_read_uint(reply, 2, type);
        pb_read_uint(reply, 3, inst);
        g.ok       = (status == 0);
        g.type     = static_cast<uint32_t>(type);
        g.instance = static_cast<uint32_t>(inst);
        return g;
    }
    void call_leave_(const char* group_name, uint32_t group_type, uint32_t inst) {
        std::string req;
        pb_string(req, 1, node_name_.c_str(), node_name_.size());  // node_name
        pb_string(req, 2, group_name, std::strlen(group_name));    // group_name
        pb_varint_field(req, 3, group_type);                       // group_type
        pb_varint_field(req, 4, inst);                             // instance
        std::string reply;
        (void)call_(kLeaveSid, req, reply);                       // ignore reply
    }

    // PgWatch CALL → PgMembership reply. Parses the member list into the cache.
    Group call_watch_(const char* group_name, uint32_t watcher_type,
                      uint32_t watcher_instance, bool watch) {
        std::string req;
        pb_string(req, 1, node_name_.c_str(), node_name_.size());  // node_name
        pb_string(req, 2, group_name, std::strlen(group_name));    // group_name
        pb_varint_field(req, 3, watcher_type);                     // watcher_type
        pb_varint_field(req, 4, watcher_instance);                 // watcher_instance
        if (watch) pb_varint_field(req, 5, 1);                     // watch=true
        std::string reply;
        Group g;
        if (!call_(kWatchSid, req, reply)) return g;
        g.ok = true;
        uint64_t gtype = 0;
        pb_read_uint(reply, 3, gtype);                            // group_type=3
        g.type = static_cast<uint32_t>(gtype);
        apply_membership_(reply);                                 // fills the cache
        return g;
    }

    // One SEQPACKET connect to the supervisor + send a GEN_CALL + read the reply.
    bool call_(uint16_t service_id, const std::string& req, std::string& reply) {
        if (call_fd_ < 0 && !connect_()) return false;
        if (!send_call_(service_id, req)) {                       // peer gone?
            ::close(call_fd_); call_fd_ = -1;
            if (!connect_() || !send_call_(service_id, req)) return false;
        }
        return recv_reply_(reply);
    }
    bool connect_() {
        call_fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (call_fd_ < 0) return false;
        struct sockaddr_tipc a{};
        a.family                  = AF_TIPC;
        a.addrtype                = TIPC_ADDR_NAME;
        a.addr.name.name.type     = kSupTipcType;
        a.addr.name.name.instance = kSupTipcInstance;
        a.scope                   = TIPC_CLUSTER_SCOPE;
        for (int i = 0; i < 6; ++i) {   // retry stale bindings (probe-connect memory)
            if (::connect(call_fd_, reinterpret_cast<struct sockaddr*>(&a),
                          sizeof(a)) == 0) return true;
            ::close(call_fd_);
            call_fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
            if (call_fd_ < 0) return false;
        }
        ::close(call_fd_); call_fd_ = -1;
        return false;
    }
    bool send_call_(uint16_t service_id, const std::string& req) {
        TheiaMsgHeader hdr{};
        hdr.bus_type           = kBusTypeRpc;
        hdr.msg_type           = kMsgGenCall;
        hdr.proto_len          = static_cast<uint16_t>(req.size());
        hdr.rpc.service_id     = service_id;
        hdr.rpc.method_id      = 0;
        hdr.rpc.correlation_id = ++corr_;
        std::string frame(sizeof(hdr) + req.size(), '\0');
        std::memcpy(&frame[0], &hdr, sizeof(hdr));
        std::memcpy(&frame[sizeof(hdr)], req.data(), req.size());
        return ::send(call_fd_, frame.data(), frame.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(frame.size());
    }
    bool recv_reply_(std::string& out) {
        struct pollfd p{call_fd_, POLLIN, 0};
        if (::poll(&p, 1, 2000) <= 0) return false;             // 2s budget
        uint8_t buf[512];
        ssize_t n = ::recv(call_fd_, buf, sizeof(buf), 0);
        if (n <= static_cast<ssize_t>(sizeof(TheiaMsgHeader))) return false;
        TheiaMsgHeader hdr{};
        std::memcpy(&hdr, buf, sizeof(hdr));
        out.assign(reinterpret_cast<char*>(buf + sizeof(hdr)),
                   static_cast<size_t>(n) - sizeof(hdr));
        return true;
    }

    // ---- the receive side: bind {group_type, instance}, dispatch via demux --
    void bind_recv_(uint32_t type, uint32_t instance, const char* group_name) {
        int fd = ::socket(AF_TIPC, SOCK_RDM | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        struct sockaddr_tipc a{};
        a.family                  = AF_TIPC;
        a.addrtype                = TIPC_ADDR_NAME;
        a.addr.name.name.type     = type;
        a.addr.name.name.instance = instance;
        a.scope                   = TIPC_CLUSTER_SCOPE;
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) < 0) {
            ::close(fd);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            bound_.push_back({group_name, type, instance, fd});
        }
        start_recv_();
    }
    void unbind_recv_(const char* group_name) {
        Bound dead;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto it = bound_.begin(); it != bound_.end(); ++it) {
                if (it->group_name == group_name) {
                    dead = *it;
                    bound_.erase(it);
                    break;
                }
            }
        }
        if (dead.fd >= 0) {
            ::close(dead.fd);   // recv loop drops the fd on its next poll rebuild
            call_leave_(group_name, dead.type, dead.instance);
        }
    }
    void start_recv_() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;  // already up
        recv_thread_ = std::thread([this] { recv_loop_(); });
    }
    void recv_loop_() {
        while (running_) {
            std::vector<int> fds;
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& b : bound_) if (b.fd >= 0) fds.push_back(b.fd);
            }
            if (fds.empty()) { struct pollfd t{-1, 0, 0}; ::poll(&t, 0, 200); continue; }
            std::vector<struct pollfd> pfds;
            pfds.reserve(fds.size());
            for (int fd : fds) pfds.push_back({fd, POLLIN, 0});
            int r = ::poll(pfds.data(), pfds.size(), 200);
            if (r <= 0) continue;
            for (auto& pf : pfds) {
                if (!(pf.revents & POLLIN)) continue;
                uint8_t buf[8192];
                ssize_t n = ::recv(pf.fd, buf, sizeof(buf), 0);
                if (n <= static_cast<ssize_t>(sizeof(TheiaMsgHeader))) continue;
                dispatch_frame_(buf, static_cast<size_t>(n));
            }
        }
    }
    // Route a received group frame: to a raw sink (non-node consumer) if set,
    // else through the node's OWN demux table — the same entries[service_id]
    // .dispatch the mux would invoke for a SEQPACKET cast.
    void dispatch_frame_(const uint8_t* frame, size_t n) {
        TheiaMsgHeader hdr{};
        std::memcpy(&hdr, frame, sizeof(hdr));
        uint16_t len = hdr.proto_len;
        if (sizeof(hdr) + len > n) len = static_cast<uint16_t>(n - sizeof(hdr));
        // A PgMembership push (OTP pg:monitor) ALWAYS refreshes the watch cache,
        // whether this client has a node binding (a GenServer watcher) or a bare
        // recv socket (a runnable watcher, e.g. the LOG pump).
        if (hdr.rpc.service_id == kMembershipSid) {
            apply_membership_(std::string(
                reinterpret_cast<const char*>(frame + sizeof(hdr)), len));
            return;
        }
        if (raw_sink_) { raw_sink_(frame + sizeof(hdr), len); return; }
        if (!binding_) return;
        auto it = binding_->entries.find(hdr.rpc.service_id);
        if (it == binding_->entries.end()) return;   // not a type this node takes
        it->second.dispatch(frame + sizeof(hdr), len, /*reply_fd=*/-1,
                            hdr.rpc.correlation_id);
    }

    // ---- the name-sequence multicast send ---------------------------------
    void send_multicast_(uint32_t group_type, uint16_t service_id,
                         const uint8_t* payload, uint16_t len) {
        int fd = ::socket(AF_TIPC, SOCK_RDM, 0);   // datagram (RDM) for multicast
        if (fd < 0) return;
        struct sockaddr_tipc a{};
        a.family               = AF_TIPC;
        a.addrtype             = TIPC_ADDR_NAMESEQ;     // a RANGE → multicast
        a.scope                = TIPC_CLUSTER_SCOPE;
        a.addr.nameseq.type    = group_type;
        a.addr.nameseq.lower   = 0;
        a.addr.nameseq.upper   = ~0u;                   // all instances in the group
        TheiaMsgHeader hdr{};
        hdr.bus_type           = kBusTypeRpc;
        hdr.msg_type           = kMsgGenCast;
        hdr.proto_len          = len;
        hdr.rpc.service_id     = service_id;
        hdr.rpc.method_id      = 0;
        hdr.rpc.correlation_id = 0;
        std::string frame(sizeof(hdr) + len, '\0');
        std::memcpy(&frame[0], &hdr, sizeof(hdr));
        if (len) std::memcpy(&frame[sizeof(hdr)], payload, len);
        (void)::sendto(fd, frame.data(), frame.size(), MSG_NOSIGNAL | MSG_DONTWAIT,
                       reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
        ::close(fd);
    }

    // OTP per-member cast — RDM datagram to ONE member's {type,instance} (matches
    // the member's bound recv socket). Best-effort, like `Pid ! Msg`.
    void send_unicast_(uint32_t type, uint32_t instance, uint16_t service_id,
                       const uint8_t* payload, uint16_t len) {
        int fd = ::socket(AF_TIPC, SOCK_RDM, 0);
        if (fd < 0) return;
        struct sockaddr_tipc a{};
        a.family                  = AF_TIPC;
        a.addrtype                = TIPC_ADDR_NAME;
        a.scope                   = TIPC_CLUSTER_SCOPE;
        a.addr.name.name.type     = type;
        a.addr.name.name.instance = instance;
        TheiaMsgHeader hdr{};
        hdr.bus_type           = kBusTypeRpc;
        hdr.msg_type           = kMsgGenCast;
        hdr.proto_len          = len;
        hdr.rpc.service_id     = service_id;
        hdr.rpc.method_id      = 0;
        hdr.rpc.correlation_id = 0;
        std::string frame(sizeof(hdr) + len, '\0');
        std::memcpy(&frame[0], &hdr, sizeof(hdr));
        if (len) std::memcpy(&frame[sizeof(hdr)], payload, len);
        (void)::sendto(fd, frame.data(), frame.size(), MSG_NOSIGNAL | MSG_DONTWAIT,
                       reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
        ::close(fd);
    }

    // ---- OTP pg:monitor — watch cache + PgMembership push handling ---------
    // Register a handler on the node's binding for the supervisor's PgMembership
    // push (service_id = kMembershipSid). Idempotent. Routes a push into
    // apply_membership_ (refresh the cache + fire on_change). Needs a binding —
    // a node with a config_mux binding (the normal reporting FC case).
    void install_membership_handler_() {
        if (!binding_ || membership_handler_installed_) return;
        membership_handler_installed_ = true;
        InboundEntry e;
        e.kind = InboundEntry::Kind::Cast;
        e.dispatch = [this](const uint8_t* p, uint16_t len, int, uint32_t) {
            apply_membership_(std::string(reinterpret_cast<const char*>(p), len));
        };
        binding_->entries[kMembershipSid] = std::move(e);
    }
    // Parse a PgMembership { status=1, group_name=2, group_type=3,
    // repeated PgMember members=4 (each {tipc_type=1, tipc_instance=2}) } and
    // replace the cached member list for that group; fire its on_change.
    void apply_membership_(const std::string& b) {
        std::string gname;
        pb_read_string(b, 2, gname);
        std::vector<Member> mem;
        size_t i = 0;
        while (i < b.size()) {                     // scan top-level fields for #4
            uint64_t key = 0; if (!rd_varint(b, i, key)) break;
            uint32_t f = key >> 3, w = key & 7;
            if (w == 2) {                          // length-delimited
                uint64_t l = 0; if (!rd_varint(b, i, l)) break;
                if (f == 4) {                      // a PgMember sub-message
                    std::string sub = b.substr(i, l);
                    uint64_t t = 0, inst = 0;
                    pb_read_uint(sub, 1, t);
                    pb_read_uint(sub, 2, inst);
                    mem.push_back({static_cast<uint32_t>(t),
                                   static_cast<uint32_t>(inst)});
                }
                i += l;
            } else if (w == 0) { uint64_t t; if (!rd_varint(b, i, t)) break; }
            else break;
        }
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lk(wmu_);
            for (auto& [sid, w] : watched_) {
                if (w.group_name == gname) {
                    w.members = mem;
                    cb = w.on_change;
                    break;
                }
            }
        }
        if (cb) cb();
    }

    // ---- minimal proto3 helpers (encode + a uint-field reader) -------------
    static uint64_t zigzag32(int32_t v) {
        return static_cast<uint32_t>((v << 1) ^ (v >> 31));
    }
    static void pb_varint(std::string& o, uint64_t v) {
        while (v >= 0x80) { o.push_back(char((v & 0x7f) | 0x80)); v >>= 7; }
        o.push_back(static_cast<char>(v));
    }
    static void pb_tag(std::string& o, uint32_t f, uint32_t w) {
        pb_varint(o, (static_cast<uint64_t>(f) << 3) | w);
    }
    static void pb_varint_field(std::string& o, uint32_t f, uint64_t v) {
        pb_tag(o, f, 0); pb_varint(o, v);
    }
    static void pb_string(std::string& o, uint32_t f, const char* s, size_t n) {
        if (!s || !n) return;
        pb_tag(o, f, 2); pb_varint(o, n); o.append(s, n);
    }
    // Read a varint field `field` from proto3 bytes (the reply); leaves out=0 if absent.
    static void pb_read_uint(const std::string& b, uint32_t field, uint64_t& out) {
        size_t i = 0;
        while (i < b.size()) {
            uint64_t key = 0; if (!rd_varint(b, i, key)) return;
            uint32_t f = key >> 3, w = key & 7;
            if (f == field && w == 0) { rd_varint(b, i, out); return; }
            if (w == 0) { uint64_t t; if (!rd_varint(b, i, t)) return; }
            else if (w == 2) { uint64_t l; if (!rd_varint(b, i, l)) return; i += l; }
            else return;
        }
    }
    static bool rd_varint(const std::string& b, size_t& i, uint64_t& out) {
        out = 0; int sh = 0;
        while (i < b.size()) {
            uint8_t c = static_cast<uint8_t>(b[i++]);
            out |= static_cast<uint64_t>(c & 0x7f) << sh;
            if (!(c & 0x80)) return true;
            sh += 7; if (sh > 63) return false;
        }
        return false;
    }
    // Read a length-delimited string field `field` from proto3 bytes.
    static void pb_read_string(const std::string& b, uint32_t field,
                               std::string& out) {
        size_t i = 0;
        while (i < b.size()) {
            uint64_t key = 0; if (!rd_varint(b, i, key)) return;
            uint32_t f = key >> 3, w = key & 7;
            if (w == 2) {
                uint64_t l = 0; if (!rd_varint(b, i, l)) return;
                if (f == field) { out.assign(b, i, l); return; }
                i += l;
            } else if (w == 0) { uint64_t t; if (!rd_varint(b, i, t)) return; }
            else return;
        }
    }

    // One watched group's cache (OTP pg:monitor state).
    struct Watched {
        std::string           group_name;
        std::vector<Member>   members;
        std::function<void()> on_change;
    };

    pid_t              self_pid_;
    std::string        node_name_;
    NodeBinding*       binding_ = nullptr;   // the node's demux (not owned)
    std::function<void(const uint8_t*, uint16_t)> raw_sink_;  // non-node consumer
    int                call_fd_ = -1;
    uint32_t           corr_ = 0;

    std::mutex         mu_;
    std::vector<Bound> bound_;               // joined groups (recv sockets)
    std::atomic<bool>  running_{false};
    std::thread        recv_thread_;

    std::mutex                       wmu_;   // guards watched_
    std::map<uint16_t, Watched>      watched_;   // service_id(T) → watch cache
    bool                             membership_handler_installed_{false};
};

}}  // namespace theia::runtime
