// User handler bodies for PerManager — the config gatekeeper's MANAGEMENT API.
//
// Ops/tooling only (separate port from the client hot path): the schema
// registry (RegisterSchema/ListSchemas), bulk migrate, and operational
// snapshot/restore. The schema registry is a PROCESS-GLOBAL singleton shared
// with PerClient (which reads it for migration-on-read) — see schema_registry.hpp.
//
// Snapshot/RestoreSnapshot + the real MigrateBulk rewrite need the etcd backend
// (wrap etcdctl / the etcd Maintenance gRPC); until that lands they return a
// NOT-IMPLEMENTED status rather than a fake success, so a caller is never
// misled. RegisterSchema/ListSchemas are fully functional now.

#include "lib/PerManager.hh"
#include "schema_registry.hpp"

#include <cstring>
#include <string>

namespace system_services_per {

namespace {

template <std::size_t N>
void set_str(char (&dst)[N], const std::string& s) {
    const std::size_t n = s.size() < N - 1 ? s.size() : N - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

// Status codes on PerReply: 0 ok, 2 invalid arg, 3 not implemented (needs etcd).
constexpr uint32_t kOk = 0, kInvalid = 2, kNotImpl = 3;

}  // namespace


void PerManager::init(PerManagerState& /*s*/) {
}

void PerManager::handle_info(const char* /*info*/, PerManagerState& /*s*/) {
}


PerReply PerManager::handle_call(
        const RegisterSchemaReq& req,
        PerManagerState& /*s*/) {
    PerReply rep{};
    const bool ok = SchemaRegistry::instance().register_schema(
        req.config_type, req.digest);
    rep.status = ok ? kOk : kInvalid;
    set_str(rep.message, ok ? "schema registered"
                            : "empty config_type or digest");
    this->log().info(std::string("RegisterSchema ") + req.config_type +
                     " digest=" + req.digest + (ok ? " -> ok" : " -> REJECTED"));
    return rep;
}

SchemaList PerManager::handle_call(
        const ListSchemasReq& req,
        PerManagerState& /*s*/) {
    SchemaList rep{};
    auto entries = SchemaRegistry::instance().list(req.config_type);
    const pb_size_t cap =
        static_cast<pb_size_t>(sizeof(rep.schemas) / sizeof(rep.schemas[0]));
    for (const auto& e : entries) {
        if (rep.schemas_count >= cap) break;   // fixed array full; truncate
        auto& s = rep.schemas[rep.schemas_count++];
        s = system_services_per_SchemaInfo{};   // nested element type (not aliased)
        set_str(s.config_type, e.config_type);
        set_str(s.digest, e.digest);
    }
    this->log().info(std::string("ListSchemas ") +
                     (req.config_type[0] ? req.config_type : "(all)") + " -> " +
                     std::to_string(rep.schemas_count) + " entries");
    return rep;
}

PerReply PerManager::handle_call(
        const MigrateBulkReq& req,
        PerManagerState& /*s*/) {
    PerReply rep{};
    // The lazy migration on read (PerClient.GetConfig) covers most reshapes; a
    // bulk rewrite of every stored value needs to walk the etcd keyspace +
    // apply the transform chain + a crash-safe resume marker — that's the etcd
    // backend. Not available against the in-memory store.
    rep.status = kNotImpl;
    set_str(rep.message, "MigrateBulk needs the etcd backend (not yet wired)");
    this->log().warn(std::string("MigrateBulk ") + req.config_type + " " +
                     req.from_digest + "->" + req.to_digest +
                     " -> NOT IMPLEMENTED (no etcd)");
    return rep;
}

PerReply PerManager::handle_call(
        const SnapshotReq& req,
        PerManagerState& /*s*/) {
    PerReply rep{};
    rep.status = kNotImpl;
    set_str(rep.message, "Snapshot needs the etcd backend (etcdctl/Maintenance)");
    this->log().warn(std::string("Snapshot '") + req.label +
                     "' -> NOT IMPLEMENTED (no etcd)");
    return rep;
}

PerReply PerManager::handle_call(
        const RestoreSnapshotReq& req,
        PerManagerState& /*s*/) {
    PerReply rep{};
    rep.status = kNotImpl;
    set_str(rep.message, "RestoreSnapshot needs the etcd backend");
    this->log().warn(std::string("RestoreSnapshot '") + req.label +
                     "' -> NOT IMPLEMENTED (no etcd)");
    return rep;
}


}  // namespace system_services_per
