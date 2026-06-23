// theia::runtime — process-group (pg) client. The node side of the OTP-pg
// broadcast model: a node JOINs a group to receive its broadcasts, a broadcaster
// WATCHEs the groups it sends to be pushed the membership. The supervisor hosts
// the registry + reaps members via the watchdog (see platform/supervisor pg core).
//
// A "group" is keyed by the wire MESSAGE-TYPE service_id (the same djb2 the
// RemoteCodec computes + register_cast demuxes on). So:
//   - a RECEIVER port for message T  → PgClient::join(service_id(T), my_rx_addr)
//   - a SENDER   port for message T  → PgClient::watch(service_id(T), my_rx_addr)
//     and the broadcaster fans out by `cast`ing T to each PgClient::members(T).
//
// Outbound (join/leave/watch) is HAND-FRAMED + cast to the supervisor's
// NodeReportIf receiver — the SAME no-supervisor-proto-dep trick HeartbeatPublisher
// uses (the runtime must not depend on the supervisor package = a cycle). Inbound
// PgMembership is decoded by a minimal proto3 reader into a per-group member
// cache; the node registers on_membership() as an inline cast handler on its mux.
#pragma once

#include "RemoteCodec.hh"        // hash_msg_type_ — service_id derivation
#include "TheiaMsgHeader.hh"     // TheiaMsgHeader, kBusTypeRpc, kMsgGenCast

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <linux/tipc.h>
#include <unistd.h>

namespace theia { namespace runtime {

class PgClient {
public:
    struct Member { uint32_t tipc_type; uint32_t tipc_instance; };

    // The supervisor's control node (NodeReportIf receiver) — same address
    // HeartbeatPublisher casts to.
    static constexpr uint32_t kSupTipcType     = 0x80020001u;
    static constexpr uint32_t kSupTipcInstance = 0u;

    // service_ids of the three control messages (djb2 of the nanopb type name,
    // matching the supervisor's register_cast<PgJoin/PgLeave/PgWatch>).
    static constexpr uint16_t kJoinSid  = hash_msg_type_("system_supervisor_PgJoin");
    static constexpr uint16_t kLeaveSid = hash_msg_type_("system_supervisor_PgLeave");
    static constexpr uint16_t kWatchSid = hash_msg_type_("system_supervisor_PgWatch");
    // The inbound membership push the node must register_cast for.
    static constexpr const char* kMembershipTypeName =
        "system_supervisor_PgMembership";
    static constexpr uint16_t kMembershipSid =
        hash_msg_type_(kMembershipTypeName);

    PgClient(std::string node_name, uint32_t my_rx_type, uint32_t my_rx_instance)
        : node_name_(std::move(node_name)), self_pid_(::getpid()) {
        (void)my_rx_type; (void)my_rx_instance;   // address is now per-call
    }
    PgClient() : self_pid_(::getpid()) {}          // process-singleton ctor

    ~PgClient() { if (fd_ >= 0) ::close(fd_); }
    PgClient(const PgClient&) = delete;
    PgClient& operator=(const PgClient&) = delete;

    // Process-global instance — a process may host several nodes, but they share
    // ONE socket to the supervisor + ONE membership cache (membership is keyed by
    // group_id = message type, identical for every watcher in the process). Each
    // join/watch carries the specific node's RECEIVE address.
    static PgClient& instance() { static PgClient c; return c; }

    // RECEIVER: ask the supervisor to add (rx_type, rx_inst) to group_id; its
    // broadcasters will cast group_id's message to that address.
    void join(uint32_t group_id, uint32_t rx_type, uint32_t rx_inst) {
        send_(kJoinSid, group_id, rx_type, rx_inst, /*with_addr=*/true);
    }
    void leave(uint32_t group_id) {
        send_(kLeaveSid, group_id, 0, 0, /*with_addr=*/false);
    }
    // SENDER/broadcaster: ask to be pushed group_id's membership at (rx_type,
    // rx_inst) — the watcher's own receiver, where PgMembership arrives.
    void watch(uint32_t group_id, uint32_t rx_type, uint32_t rx_inst) {
        send_(kWatchSid, group_id, rx_type, rx_inst, /*with_addr=*/true);
    }

    // Feed an inbound PgMembership wire payload (the node's inline cast handler
    // calls this with the proto3 bytes). Updates the per-group member cache.
    void on_membership(const uint8_t* data, size_t len) {
        uint32_t group_id = 0;
        std::vector<Member> members;
        decode_membership_(data, len, group_id, members);
        std::lock_guard<std::mutex> lk(mu_);
        members_[group_id] = std::move(members);
    }

    // Snapshot of group_id's current members (for a broadcaster's fan-out loop).
    std::vector<Member> members(uint32_t group_id) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = members_.find(group_id);
        return it == members_.end() ? std::vector<Member>{} : it->second;
    }

private:
    bool ensure_socket_() {
        if (fd_ >= 0) return connected_ || connect_();
        fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        return connect_();
    }
    bool connect_() {
        struct sockaddr_tipc addr{};
        addr.family                  = AF_TIPC;
        addr.addrtype                = TIPC_ADDR_NAME;
        addr.addr.name.name.type     = kSupTipcType;
        addr.addr.name.name.instance = kSupTipcInstance;
        addr.scope                   = TIPC_CLUSTER_SCOPE;
        connected_ = (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                                sizeof(addr)) == 0);
        return connected_;
    }

    // Frame + cast one PgJoin/PgLeave/PgWatch. with_addr appends the member's
    // receive address (fields 4/5) — present on Join + Watch, absent on Leave.
    void send_(uint16_t service_id, uint32_t group_id,
               uint32_t rx_type, uint32_t rx_inst, bool with_addr) {
        if (!ensure_socket_()) return;
        std::string rec;
        rec.reserve(48);
        pb_string(rec, 1, node_name_.c_str(), node_name_.size());  // node_name
        pb_varint_field(rec, 2, zigzag32(self_pid_));              // pid (sint32)
        pb_varint_field(rec, 3, group_id);                        // group_id
        if (with_addr) {
            pb_varint_field(rec, 4, rx_type);                     // tipc_type
            pb_varint_field(rec, 5, rx_inst);                     // tipc_instance
        }
        TheiaMsgHeader hdr{};
        hdr.bus_type           = kBusTypeRpc;
        hdr.msg_type           = kMsgGenCast;
        hdr.proto_len          = static_cast<uint16_t>(rec.size());
        hdr.rpc.service_id     = service_id;
        hdr.rpc.method_id      = 0;
        hdr.rpc.correlation_id = 0;
        std::string frame(sizeof(hdr) + rec.size(), '\0');
        std::memcpy(&frame[0], &hdr, sizeof(hdr));
        std::memcpy(&frame[sizeof(hdr)], rec.data(), rec.size());
        if (::send(fd_, frame.data(), frame.size(),
                   MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
            ::close(fd_); fd_ = -1; connected_ = false;   // re-connect next call
        }
    }

    // Minimal proto3 reader for PgMembership { uint32 group_id=1;
    // repeated PgMember members=2 } where PgMember { uint32 tipc_type=1;
    // uint32 tipc_instance=2 }.
    static void decode_membership_(const uint8_t* p, size_t len,
                                   uint32_t& group_id,
                                   std::vector<Member>& members) {
        size_t i = 0;
        while (i < len) {
            uint64_t key = 0; if (!rd_varint(p, len, i, key)) return;
            uint32_t field = key >> 3, wire = key & 7;
            if (field == 1 && wire == 0) {            // group_id
                uint64_t v = 0; if (!rd_varint(p, len, i, v)) return;
                group_id = static_cast<uint32_t>(v);
            } else if (field == 2 && wire == 2) {     // members (length-delimited)
                uint64_t mlen = 0; if (!rd_varint(p, len, i, mlen)) return;
                if (i + mlen > len) return;
                Member m{0, 0};
                size_t end = i + mlen;
                while (i < end) {
                    uint64_t mk = 0; if (!rd_varint(p, end, i, mk)) return;
                    uint32_t mf = mk >> 3, mw = mk & 7;
                    uint64_t mv = 0;
                    if (mw == 0) { if (!rd_varint(p, end, i, mv)) return; }
                    else return;   // unexpected
                    if (mf == 1) m.tipc_type = static_cast<uint32_t>(mv);
                    else if (mf == 2) m.tipc_instance = static_cast<uint32_t>(mv);
                }
                members.push_back(m);
            } else {
                // skip unknown field
                if (wire == 0) { uint64_t v; if (!rd_varint(p, len, i, v)) return; }
                else if (wire == 2) { uint64_t l; if (!rd_varint(p, len, i, l)) return; i += l; }
                else return;
            }
        }
    }
    static bool rd_varint(const uint8_t* p, size_t len, size_t& i, uint64_t& out) {
        out = 0; int shift = 0;
        while (i < len) {
            uint8_t b = p[i++];
            out |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
            if (shift > 63) return false;
        }
        return false;
    }

    static uint64_t zigzag32(int32_t v) {
        return static_cast<uint32_t>((v << 1) ^ (v >> 31));
    }
    static void pb_varint(std::string& out, uint64_t v) {
        while (v >= 0x80) { out.push_back(char((v & 0x7f) | 0x80)); v >>= 7; }
        out.push_back(static_cast<char>(v));
    }
    static void pb_tag(std::string& out, uint32_t field, uint32_t wire) {
        pb_varint(out, (static_cast<uint64_t>(field) << 3) | wire);
    }
    static void pb_varint_field(std::string& out, uint32_t field, uint64_t v) {
        pb_tag(out, field, 0); pb_varint(out, v);
    }
    static void pb_string(std::string& out, uint32_t field,
                          const char* s, size_t n) {
        if (!s || !n) return;
        pb_tag(out, field, 2); pb_varint(out, n); out.append(s, n);
    }

    std::string node_name_ = "pg";
    pid_t       self_pid_;
    int         fd_ = -1;
    bool        connected_ = false;
    mutable std::mutex mu_;
    std::map<uint32_t, std::vector<Member>> members_;   // group_id → members
};

}}  // namespace theia::runtime
