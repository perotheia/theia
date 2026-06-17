// per_link — com's RemoteRef link to services/per's MANAGER node (the ops /
// tooling surface), so com can proxy per's schema-registry + snapshot ops over
// gRPC (PerView) for the GUI / rtdb. Mirrors sup_link: the nanopb per structs
// + the RemoteRef/TipcMux live ONLY in per_link.cc, so the libprotobuf gRPC
// edge (ComGrpcProxy_handlers.cc) never meets the nanopb per headers.
//
// PerManager is at TIPC 0x80010016/0 (moved off 0x80010008, which collided
// with com's own ComDaemon). Read/ops only — ListSchemas (the registry) +
// Snapshot (trigger a config backup). The config hot path (Get/Put/Watch)
// stays node↔per (schema-aware; not proxied).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace services_com {

// One (config_type, digest) registry entry, in primitives.
struct PerSchema {
    std::string config_type;
    std::string digest;
};

// A decoded PerReply (Snapshot etc.), in primitives.
struct PerOpReply {
    uint32_t    status = 0;     // 0 = OK
    std::string message;
    uint64_t    mod_rev = 0;
};

// One RAW config-store row (GetSnapshot), in primitives. `config` is the
// proto-wire bytes per stores; the gRPC caller decodes it against the schema.
struct PerStoreRow {
    std::string config_type;
    std::string digest;
    std::string config;        // serialized <Node>Config (binary proto bytes)
};

// Singleton link to PerManager. Opened by ComGrpcProxy::do_start, torn down by
// do_stop. Thread-safe.
class PerLink {
public:
    static PerLink& instance();

    // Connect the RemoteRef (TIPC 0x80010016/0) + start the reply pump.
    // Returns false if per isn't reachable. Idempotent.
    bool start(int connect_timeout_ms = 3000);
    void stop();
    bool connected() const;

    // ListSchemas(config_type="" → all). Fills out with the registry rows.
    // Returns false on transport error / timeout / not-connected.
    bool list_schemas(const std::string& config_type,
                      std::vector<PerSchema>& out, int timeout_ms = 5000);

    // Snapshot(label) — trigger an operational backup of per's config keyspace.
    bool snapshot(const std::string& label, PerOpReply& out,
                  int timeout_ms = 5000);

    // GetSnapshot(config_type="" → all). Fills out with the raw store rows
    // (config_type, digest, config bytes). Returns false on transport error /
    // timeout / not-connected.
    bool get_snapshot(const std::string& config_type,
                      std::vector<PerStoreRow>& out, int timeout_ms = 5000);

private:
    PerLink();
    ~PerLink();
    PerLink(const PerLink&)            = delete;
    PerLink& operator=(const PerLink&) = delete;

    struct Impl;
    Impl* impl_;   // pimpl so this header stays free of runtime/TIPC types
};

}  // namespace services_com
