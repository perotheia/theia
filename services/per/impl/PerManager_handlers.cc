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
#include "impl/etcd_store.hpp"          // shared_store() + Store::scan/put
#include "impl/migration_plugin.hpp"   // load_migration_plugin (dlopen)
#include "impl/migration_registry.hpp" // MigrationRegistry::migrate (the chain)
#include "impl/snapshot_ops.hpp"       // write/restore_config_snapshot
#include "ParamsConfig.hh"             // get_config() — snapshot_dir param
#include "schema_registry.hpp"

#include <sys/stat.h>
#include <sys/types.h>

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

// Snapshot file path for a label: <snapshot_dir>/<label>.persnap. snapshot_dir
// is the PerManager static param. mkdir -p the dir on save.
std::string snapshot_dir() {
    return ::theia::runtime::get_config().node(PerManager::kNodeName)
        .str("snapshot_dir", "/tmp/theia/per-snapshots");
}
std::string snapshot_path(const std::string& label) {
    return snapshot_dir() + "/" + label + ".persnap";
}
// mkdir -p each path component (POSIX, no <filesystem> dep).
void mkdir_p(const std::string& dir) {
    std::string acc;
    for (std::size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            if (!acc.empty()) ::mkdir(acc.c_str(), 0755);  // ignore EEXIST
        }
        if (i < dir.size()) acc.push_back(dir[i]);
    }
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
    const std::string from = req.from_digest, to = req.to_digest;

    // 1. Load the transform plugin if a path was given. The reshape code is
    //    authored with the NEW (n+1) version but runs inside the deployed (n)
    //    per, so it ships as a dlopen'd .so. Once loaded, its edges feed BOTH
    //    this bulk rewrite AND GetConfig's lazy migration-on-read. Empty path =
    //    use whatever's already registered.
    if (req.plugin_so[0]) {
        std::string err;
        if (!load_migration_plugin(req.plugin_so, &err)) {
            rep.status = kInvalid;
            set_str(rep.message, "plugin load failed: " + err);
            this->log().warn(std::string("MigrateBulk: ") +
                             "plugin '" + req.plugin_so + "' -> " + err);
            return rep;
        }
        this->log().info(std::string("MigrateBulk: loaded plugin ") +
                         req.plugin_so);
    }

    // 2. Confirm a transform PATH exists from->to (after the plugin loaded).
    //    A trivial probe with empty bytes: nullopt = no path.
    if (!MigrationRegistry::instance().migrate(from, to, std::string{})) {
        rep.status = kInvalid;
        set_str(rep.message, "no transform path " + from + "->" + to);
        this->log().warn(std::string("MigrateBulk: no path ") + from + "->" + to);
        return rep;
    }

    // 3. Walk the keyspace, rewrite every value at `from` to `to` (CAS on its
    //    mod_rev so a concurrent Put isn't clobbered). Values already at `to`
    //    (or any other digest) are skipped.
    Store* store = shared_store();
    if (!store) {
        rep.status = kNotImpl;
        set_str(rep.message, "store not ready");
        return rep;
    }
    uint32_t migrated = 0, skipped = 0, conflicts = 0;
    for (auto& [node, val] : store->scan()) {
        if (val.digest != from) { ++skipped; continue; }
        auto out = MigrationRegistry::instance().migrate(from, to, val.config);
        if (!out) { ++skipped; continue; }   // shouldn't happen (path checked)
        // CAS on the value's current rev — if someone Put it meanwhile, skip
        // (the next MigrateBulk run, or lazy read, catches it).
        const int64_t nr = store->put(node, *out, to, val.mod_rev);
        if (nr == 0) ++conflicts; else ++migrated;
    }

    rep.status = kOk;
    set_str(rep.message, "migrated " + std::to_string(migrated) +
                         ", skipped " + std::to_string(skipped) +
                         ", conflicts " + std::to_string(conflicts));
    this->log().info(std::string("MigrateBulk ") + from + "->" + to +
                     ": migrated " + std::to_string(migrated) +
                     " skipped " + std::to_string(skipped) +
                     " conflicts " + std::to_string(conflicts));
    return rep;
}

PerReply PerManager::handle_call(
        const SnapshotReq& req,
        PerManagerState& /*s*/) {
    PerReply rep{};
    Store* store = shared_store();
    if (!store) {
        rep.status = kNotImpl;
        set_str(rep.message, "store not ready");
        return rep;
    }
    if (req.label[0] == '\0') {
        rep.status = kInvalid;
        set_str(rep.message, "empty label");
        return rep;
    }
    // CONFIG-SCOPED snapshot: just per's /theia/config/ keyspace (via the
    // Store's scan), NOT a full etcd backup. Restore is then a safe live re-put.
    mkdir_p(snapshot_dir());
    const std::string path = snapshot_path(req.label);
    const long n = write_config_snapshot(*store, path);
    if (n < 0) {
        rep.status = kInvalid;
        set_str(rep.message, "snapshot write failed: " + path);
        this->log().warn(std::string("Snapshot '") + req.label +
                         "' -> write FAILED (" + path + ")");
        return rep;
    }
    rep.status = kOk;
    set_str(rep.message, "snapshot " + std::to_string(n) + " keys -> " + path);
    this->log().info(std::string("Snapshot '") + req.label + "' -> " +
                     std::to_string(n) + " keys at " + path);
    return rep;
}

PerReply PerManager::handle_call(
        const RestoreSnapshotReq& req,
        PerManagerState& /*s*/) {
    PerReply rep{};
    Store* store = shared_store();
    if (!store) {
        rep.status = kNotImpl;
        set_str(rep.message, "store not ready");
        return rep;
    }
    const std::string path = snapshot_path(req.label);
    const long n = restore_config_snapshot(*store, path);
    if (n < 0) {
        rep.status = kInvalid;
        set_str(rep.message, "restore failed (missing/bad): " + path);
        this->log().warn(std::string("RestoreSnapshot '") + req.label +
                         "' -> FAILED (" + path + ")");
        return rep;
    }
    rep.status = kOk;
    set_str(rep.message, "restored " + std::to_string(n) + " keys from " + path);
    this->log().info(std::string("RestoreSnapshot '") + req.label + "' -> " +
                     std::to_string(n) + " keys from " + path);
    return rep;
}


}  // namespace system_services_per
