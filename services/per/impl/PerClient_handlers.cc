// User handler bodies for PerClient — the config gatekeeper's client API.
//
// Get / Put / Watch config. The backing store is in-memory FOR NOW (see
// PerClient_state.hh); swapping it for etcd-cpp-apiv3 touches only the store
// access + a watch thread, not this wire surface. Config values are opaque
// binary-proto bytes: per stores + forwards them, never decodes the node's
// config type.
//
// WatchConfig is a SUBSCRIPTION: it records (target, subscriber) and, on a
// Put that changes the target, casts platform.runtime.ConfigUpdated to each
// subscriber's framework config-service receiver (the third config-service
// push, sibling of Trace/LogLevelPush). The subscriber's GenServer base
// handle_cast(ConfigUpdated) applies it. The cast address is resolved from the
// subscriber node name at push time.

#include "lib/PerClient.hh"

#include "NodeRef.hh"                       // theia::runtime::cast(self, msg, TipcAddr)
#include "platform_runtime/runtime.pb.h"    // platform_runtime_ConfigUpdated

#include <cstdio>
#include <cstring>
#include <string>

namespace system_services_per {

namespace {

// Copy a std::string into a fixed nanopb char[] field (NUL-terminated, capped).
template <std::size_t N>
void set_str(char (&dst)[N], const std::string& s) {
    const std::size_t n = s.size() < N - 1 ? s.size() : N - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

// Copy bytes into a fixed nanopb PB_BYTES_ARRAY_T field (capped at its capacity).
template <typename BytesT>
void set_bytes(BytesT& dst, const std::string& s) {
    const std::size_t cap = sizeof(dst.bytes);
    const std::size_t n = s.size() < cap ? s.size() : cap;
    std::memcpy(dst.bytes, s.data(), n);
    dst.size = static_cast<pb_size_t>(n);
}

// Resolve a subscriber node NAME to its TIPC address. FOR NOW per derives it
// the same way the manifest does for app worker nodes: a node's address is
// declared in the netgraph/executor — but per (a plain FC) doesn't carry the
// cluster registry yet. So the first slice resolves a small set of known
// reporting nodes by convention; the real version reuses the supervisor's
// Registry (name -> {type,instance}) loaded from the manifest. Returns {ok}.
struct ResolvedAddr { bool ok; uint32_t type; uint32_t instance; };

ResolvedAddr resolve_subscriber(const std::string& /*name*/) {
    // TODO(per): load the manifest Registry and resolve name -> TipcAddr, like
    // the supervisor does (project-app-node-tipc-resolution). Until then a
    // subscriber must be reachable by an addr the test supplies out-of-band;
    // we return not-ok so Put/Watch DON'T attempt a cast to an unknown peer
    // (the store + subscription bookkeeping still works and is observable).
    return {false, 0, 0};
}

// Cast the current stored config to one subscriber.
void push_config(PerClient& self, const Subscription& sub,
                 const StoredConfig& cur) {
    auto addr = resolve_subscriber(sub.subscriber_node);
    if (!addr.ok) {
        self.log().debug(std::string("watch: subscriber '") +
                         sub.subscriber_node + "' unresolved — not pushed");
        return;
    }
    platform_runtime_ConfigUpdated m{};
    set_bytes(m.config, cur.config);
    set_str(m.digest, cur.digest);
    m.changed_count = 0;   // whole-snapshot delivery (no diff in the first slice)
    ::theia::runtime::cast(self, m,
                           ::theia::runtime::TipcAddr{addr.type, addr.instance},
                           /*dst_name=*/nullptr, /*connect_timeout_ms=*/250);
}

}  // namespace


void PerClient::init(PerClientState& /*s*/) {
}

void PerClient::handle_info(const char* /*info*/, PerClientState& /*s*/) {
}


ConfigSnapshot PerClient::handle_call(
        const GetConfigReq& req,
        PerClientState& s) {
    ConfigSnapshot rep{};
    auto it = s.store.find(req.target_node);
    if (it == s.store.end()) {
        // Missing key: empty snapshot (mod_rev 0 = "not present"). A real
        // NOT_FOUND status rides PerReply on the write path; Get returns the
        // snapshot shape, so emptiness signals absence.
        this->log().info(std::string("GetConfig ") + req.target_node +
                         " -> (absent)");
        return rep;
    }
    // TODO(per): if req.want_digest != stored digest, run the migration chain
    // to want_digest here (lazy migration on read), strip the version. The
    // first slice returns the stored value verbatim.
    const StoredConfig& cur = it->second;
    set_bytes(rep.config, cur.config);
    set_str(rep.digest, cur.digest);
    rep.mod_rev = cur.mod_rev;
    this->log().info(std::string("GetConfig ") + req.target_node + " -> " +
                     std::to_string(cur.config.size()) + "B digest=" +
                     cur.digest + " rev=" + std::to_string(cur.mod_rev));
    return rep;
}

PerReply PerClient::handle_call(
        const PutConfigReq& req,
        PerClientState& s) {
    PerReply rep{};
    // CAS guard: if expect_rev != 0 it must match the current rev.
    auto it = s.store.find(req.target_node);
    const uint64_t cur_rev = (it == s.store.end()) ? 0 : it->second.mod_rev;
    if (req.expect_rev != 0 && req.expect_rev != cur_rev) {
        rep.status = 1;   // CAS conflict
        set_str(rep.message, "rev mismatch (CAS)");
        rep.mod_rev = cur_rev;
        this->log().warn(std::string("PutConfig ") + req.target_node +
                         " CAS conflict: expected " +
                         std::to_string(req.expect_rev) + " have " +
                         std::to_string(cur_rev));
        return rep;
    }
    // Store the new value (binary-proto bytes verbatim; per never decodes it).
    StoredConfig& cur = s.store[req.target_node];
    cur.config.assign(reinterpret_cast<const char*>(req.config.bytes),
                      req.config.size);
    cur.digest = req.digest;
    cur.mod_rev = s.next_rev++;
    rep.status = 0;
    rep.mod_rev = cur.mod_rev;
    this->log().info(std::string("PutConfig ") + req.target_node + " <- " +
                     std::to_string(cur.config.size()) + "B digest=" +
                     cur.digest + " rev=" + std::to_string(cur.mod_rev));

    // Fan the change out to every subscriber watching this target.
    auto wit = s.watches.find(req.target_node);
    if (wit != s.watches.end()) {
        for (const auto& sub : wit->second) {
            push_config(*this, sub, cur);
        }
    }
    return rep;
}

PerReply PerClient::handle_call(
        const WatchConfigReq& req,
        PerClientState& s) {
    PerReply rep{};
    // Record the subscription (idempotent on (target, subscriber)).
    auto& subs = s.watches[req.target_node];
    bool exists = false;
    for (auto& sub : subs) {
        if (sub.subscriber_node == req.subscriber_node) {
            sub.want_digest = req.want_digest;   // refresh
            exists = true;
            break;
        }
    }
    if (!exists) {
        subs.push_back(Subscription{req.subscriber_node, req.want_digest});
    }
    rep.status = 0;
    set_str(rep.message, exists ? "watch refreshed" : "watch armed");
    this->log().info(std::string("WatchConfig ") + req.target_node +
                     " <- subscriber " + req.subscriber_node +
                     (exists ? " (refresh)" : " (new)"));

    // Deliver the current snapshot immediately so a fresh subscriber starts in
    // sync (etcd watch semantics: an initial value, then deltas).
    auto it = s.store.find(req.target_node);
    if (it != s.store.end()) {
        push_config(*this,
                    Subscription{req.subscriber_node, req.want_digest},
                    it->second);
    }
    return rep;
}


}  // namespace system_services_per
