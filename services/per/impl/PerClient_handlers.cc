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
#include "ParamsConfig.hh"                  // get_config() — static param push_connect_ms
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

// Cast one config snapshot to one subscriber. Runs ON THE NODE THREAD (only
// ever invoked from the deferred mailbox lambda in schedule_push, never inline
// from a handler) — see schedule_push for why.
void push_config_now(PerClient& self, const std::string& subscriber,
                     const StoreValue& snap) {
    auto addr = resolve_subscriber(subscriber);
    if (!addr.ok) {
        self.log().info(std::string("push: subscriber '") + subscriber +
                        "' UNRESOLVED — not pushed");
        return;
    }
    char abuf[96];
    std::snprintf(abuf, sizeof(abuf),
                  "push: ConfigUpdated -> %s @ 0x%08x:%u (%zuB)",
                  subscriber.c_str(), addr.type, addr.instance,
                  snap.config.size());
    self.log().info(abuf);
    platform_runtime_ConfigUpdated m{};
    set_bytes(m.config, snap.config);
    set_str(m.digest, snap.digest);
    m.changed_count = 0;   // whole-snapshot delivery (no diff in the first slice)
    // Bounded connect: a subscriber that's DOWN must fail fast, never stall the
    // node thread. The budget is a STATIC PARAM (push_connect_ms) read from the
    // per-FC config JSON at boot — defaults to 250ms (the supervisor's child
    // trace/log push budget) when no config is staged.
    const uint32_t budget =
        ::theia::runtime::get_config().node(PerClient::kNodeName)
            .u32("push_connect_ms", 250);
    ::theia::runtime::cast(self, m,
                           ::theia::runtime::TipcAddr{addr.type, addr.instance},
                           /*dst_name=*/nullptr,
                           /*connect_timeout_ms=*/static_cast<int>(budget));
}

// Schedule a fan-out of `snap` to `subscribers` to run LATER on the node
// thread, OFF the current handler's reply path.
//
// Why deferred (the re-entrancy / blocking hazard): casting from inside a
// handle_call runs a SYNCHRONOUS TIPC connect (bounded at 250ms) per
// subscriber. Doing that inline would (a) make the CALLER wait for every cast
// before it gets its reply, and (b) for an N-subscriber fan-out, block the
// node thread for up to N*250ms — wedging every other config op behind it. So
// we enqueue() a copy of the work onto this node's own mailbox: the handler
// returns its reply immediately, and the casts run one-by-one on the node
// thread afterwards, each still bounded. Copies (not refs) are captured because
// the lambda outlives the handler frame and `s.store` may change before it
// runs.
void schedule_push(PerClient& self,
                   std::vector<std::string> subscribers,
                   StoreValue snap) {
    if (subscribers.empty()) return;
    self.enqueue([&self, subs = std::move(subscribers),
                  snap = std::move(snap)](::theia::runtime::GenServerBase*) {
        for (const auto& sub : subs) {
            push_config_now(self, sub, snap);
        }
    });
}

}  // namespace


void PerClient::init(PerClientState& s) {
    // Read static params at boot (deployment knobs from the per-FC config JSON).
    auto cfg = ::theia::runtime::get_config().node(kNodeName);
    const std::string endpoint = cfg.str("etcd_endpoint", "127.0.0.1:2379");
    this->log().info(std::string("params: push_connect_ms=") +
                     std::to_string(cfg.u32("push_connect_ms", 250)) +
                     " etcd_endpoint=" + endpoint);

    // Choose the store: etcd-backed when reachable, else the in-memory
    // fallback. THEIA_PER_BACKEND=mem forces in-memory (tests / no etcd).
    const char* backend = std::getenv("THEIA_PER_BACKEND");
    if (backend && std::string(backend) == "mem") {
        s.store = make_memory_store();
        this->log().info("store: in-memory (forced by THEIA_PER_BACKEND=mem)");
    } else {
        s.store = make_etcd_store(endpoint);
        if (s.store) {
            this->log().info("store: etcd @ " + endpoint);
            // Watch the whole config prefix. On an etcd change, per casts
            // ConfigUpdated to every subscriber of that target. The watch cb
            // runs on the etcd watcher thread — defer the cast onto the node
            // thread (the re-entrancy/blocking rule), capturing copies.
            PerClient* self = this;
            s.store->watch_prefix([self, &s](const WatchEvent& ev) {
                std::vector<std::string> subs;
                auto wit = s.watches.find(ev.target_node);
                if (wit != s.watches.end())
                    for (const auto& sub : wit->second)
                        subs.push_back(sub.subscriber_node);
                if (subs.empty()) return;
                StoreValue snap = ev.cur;
                self->enqueue([self, subs = std::move(subs),
                               snap = std::move(snap)]
                              (::theia::runtime::GenServerBase*) {
                    for (const auto& sub : subs) push_config_now(*self, sub, snap);
                });
            });
        } else {
            this->log().warn("store: etcd unreachable @ " + endpoint +
                             " — falling back to in-memory");
            s.store = make_memory_store();
        }
    }
}

void PerClient::handle_info(const char* /*info*/, PerClientState& /*s*/) {
}


ConfigSnapshot PerClient::handle_call(
        const GetConfigReq& req,
        PerClientState& s) {
    ConfigSnapshot rep{};
    StoreValue cur = s.store->get(req.target_node);
    if (!cur.found) {
        // Missing key: empty snapshot (mod_rev 0 = "not present"). Get returns
        // the snapshot shape, so emptiness signals absence.
        this->log().info(std::string("GetConfig ") + req.target_node +
                         " -> (absent)");
        return rep;
    }
    // TODO(per): if req.want_digest != cur.digest, run the migration chain to
    // want_digest here (lazy migration on read, consulting SchemaRegistry). The
    // first slice returns the stored value verbatim.
    set_bytes(rep.config, cur.config);
    set_str(rep.digest, cur.digest);
    rep.mod_rev = static_cast<uint64_t>(cur.mod_rev);
    this->log().info(std::string("GetConfig ") + req.target_node + " -> " +
                     std::to_string(cur.config.size()) + "B digest=" +
                     cur.digest + " rev=" + std::to_string(cur.mod_rev));
    return rep;
}

PerReply PerClient::handle_call(
        const PutConfigReq& req,
        PerClientState& s) {
    PerReply rep{};
    const std::string config(reinterpret_cast<const char*>(req.config.bytes),
                             req.config.size);
    const int64_t new_rev = s.store->put(
        req.target_node, config, req.digest,
        static_cast<int64_t>(req.expect_rev));
    if (new_rev == 0) {
        // CAS conflict (expect_rev didn't match the store's current rev).
        rep.status = 1;
        set_str(rep.message, "rev mismatch (CAS)");
        StoreValue v = s.store->get(req.target_node);
        rep.mod_rev = static_cast<uint64_t>(v.mod_rev);
        this->log().warn(std::string("PutConfig ") + req.target_node +
                         " CAS conflict (expected " +
                         std::to_string(req.expect_rev) + ")");
        return rep;
    }
    rep.status = 0;
    rep.mod_rev = static_cast<uint64_t>(new_rev);
    this->log().info(std::string("PutConfig ") + req.target_node + " <- " +
                     std::to_string(config.size()) + "B digest=" +
                     std::string(req.digest) + " rev=" + std::to_string(new_rev));

    // Fan-out: with the ETCD store the change comes back via the watch callback
    // (registered in init), so we DON'T fan out here — that would double-push.
    // With the IN-MEMORY store there is no watch source, so push directly here.
    // Both paths defer off the reply (schedule_push enqueues onto the node).
    if (!s.store->is_watched()) {
        auto wit = s.watches.find(req.target_node);
        if (wit != s.watches.end()) {
            std::vector<std::string> subs;
            subs.reserve(wit->second.size());
            for (const auto& sub : wit->second)
                subs.push_back(sub.subscriber_node);
            StoreValue snap;
            snap.config = config; snap.digest = req.digest; snap.mod_rev = new_rev;
            schedule_push(*this, std::move(subs), std::move(snap));
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
    // sync (etcd watch semantics: an initial value, then deltas) — DEFERRED off
    // the reply path. This is the per-subscriber initial sync, NOT covered by
    // the etcd watch (which only fires on future changes), so it runs for both
    // store backends.
    StoreValue cur = s.store->get(req.target_node);
    if (cur.found) {
        schedule_push(*this, {req.subscriber_node}, std::move(cur));
    }
    return rep;
}


}  // namespace system_services_per
