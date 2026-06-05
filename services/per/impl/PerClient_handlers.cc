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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

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

// Resolve a subscriber node NAME to its TIPC address, from the staged
// netgraph.json (node name -> {type, instance}). Path comes from $THEIA_NETGRAPH
// (the supervisor/stage sets it; defaults to ./netgraph.json next to the
// binary). Loaded once and cached. This is the lightweight reuse of the cluster
// address book — per doesn't need the full supervisor Registry, just name->addr
// for the nodes it pushes config to. (A node-type name like "PerManager" or an
// app worker name resolves if it's in the netgraph.) Returns {ok}.
struct ResolvedAddr { bool ok; uint32_t type; uint32_t instance; };

const std::unordered_map<std::string, std::pair<uint32_t, uint32_t>>&
netgraph_addrs() {
    static const auto table = [] {
        std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> t;
        const char* path = std::getenv("THEIA_NETGRAPH");
        std::string p = path ? path : "netgraph.json";
        std::ifstream f(p);
        if (!f) return t;
        std::stringstream ss; ss << f.rdbuf();
        const std::string s = ss.str();
        // Minimal hand-parse of the {"nodes":[{"name":..,"tipc":{"type":..,
        // "instance":..}}]} shape — avoids pulling a JSON lib into per's impl.
        // Scan for "name":"X" ... "type":"0x..","instance":"N" triples.
        std::size_t pos = 0;
        while ((pos = s.find("\"name\"", pos)) != std::string::npos) {
            auto nq1 = s.find('"', s.find(':', pos) + 1);
            auto nq2 = s.find('"', nq1 + 1);
            if (nq1 == std::string::npos || nq2 == std::string::npos) break;
            std::string name = s.substr(nq1 + 1, nq2 - nq1 - 1);
            auto tpos = s.find("\"type\"", nq2);
            auto ipos = s.find("\"instance\"", nq2);
            if (tpos == std::string::npos || ipos == std::string::npos) {
                pos = nq2 + 1; continue;
            }
            auto rd = [&](std::size_t kpos) -> uint32_t {
                auto q1 = s.find('"', s.find(':', kpos) + 1);
                auto q2 = s.find('"', q1 + 1);
                return static_cast<uint32_t>(
                    std::strtoul(s.substr(q1 + 1, q2 - q1 - 1).c_str(), nullptr, 0));
            };
            t[name] = {rd(tpos), rd(ipos)};
            pos = nq2 + 1;
        }
        return t;
    }();
    return table;
}

ResolvedAddr resolve_subscriber(const std::string& name) {
    const auto& t = netgraph_addrs();
    auto it = t.find(name);
    if (it == t.end()) return {false, 0, 0};
    return {true, it->second.first, it->second.second};
}

// Cast the current stored config to one subscriber.
void push_config(PerClient& self, const Subscription& sub,
                 const StoredConfig& cur) {
    auto addr = resolve_subscriber(sub.subscriber_node);
    if (!addr.ok) {
        self.log().info(std::string("push: subscriber '") +
                        sub.subscriber_node + "' UNRESOLVED — not pushed");
        return;
    }
    char abuf[96];
    std::snprintf(abuf, sizeof(abuf),
                  "push: ConfigUpdated -> %s @ 0x%08x:%u (%zuB)",
                  sub.subscriber_node.c_str(), addr.type, addr.instance,
                  cur.config.size());
    self.log().info(abuf);
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
