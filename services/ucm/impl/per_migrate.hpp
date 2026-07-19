// per_migrate — the config-migration MECHANISM, shared by the two migration
// AUTHORITIES. Extracted from UcmGate so V-UCM (the fleet authority) and UCM (the
// standalone-overlay authority) drive the SAME per calls without duplication.
//
// WHO migrates (the cardinality that motivated this split):
//   * FLEET path — V-UCM owns it. One V-UCM + one per/etcd (both master
//     singletons) → migration runs ONCE, centrally, in V-UCM's INSTALLING step,
//     BEFORE the RequestUpdate fan-out to the N boards' UCMs. The boards then
//     install BINARIES only. (Was: each of N UCMs re-ran MigrateBulk against the
//     one central per — N-1 wasted no-op passes.)
//   * STANDALONE overlay — the theia-swp Mender module (via theia-migrate) still
//     migrates for a board with NO V-UCM (bare box / recovery). Legit exception.
//
// WHY once-central is correct (not just tidy): per keys are version-free
// (/theia/config/<node> → <digest>\0<bytes>) and GetConfig(want_digest) reshapes
// on READ to the caller's schema without touching the store. So a new binary that
// starts before the bulk pass still reads correct config via migrate-on-read —
// the bulk pass is CONVERGENCE, not a hard pre-binary-start barrier. Migrating
// once against the singleton store is therefore both sufficient and symmetric.
//
// The mechanism: Snapshot("pre-<ver>") is the rollback anchor; MigrateBulk per
// step (per dlopen's the plugin FROM THE STAGED RELEASE — the n+1 reshape code
// runs inside the deployed n per); RestoreSnapshot on rollback. migration.json
// (packed by `theia release-swp --migrate`) is self-contained: each step carries
// {config_type, from_digest, to_digest, plugin}.

#pragma once

#include "NodeRef.hh"
#include "RemoteCodec.hh"
#include "TipcMux.hh"
#include "system/services/per/per.pb.h"

#include "nlohmann/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// The per migration codecs (idempotent guards let both TUs declare them).
#ifndef THEIA_CODEC_system_services_per_MigrateBulkReq
#define THEIA_CODEC_system_services_per_MigrateBulkReq
THEIA_DECLARE_REMOTE_CODEC(system_services_per_MigrateBulkReq)
#endif
#ifndef THEIA_CODEC_system_services_per_SnapshotReq
#define THEIA_CODEC_system_services_per_SnapshotReq
THEIA_DECLARE_REMOTE_CODEC(system_services_per_SnapshotReq)
#endif
#ifndef THEIA_CODEC_system_services_per_RestoreSnapshotReq
#define THEIA_CODEC_system_services_per_RestoreSnapshotReq
THEIA_DECLARE_REMOTE_CODEC(system_services_per_RestoreSnapshotReq)
#endif
#ifndef THEIA_CODEC_system_services_per_PerReply
#define THEIA_CODEC_system_services_per_PerReply
THEIA_DECLARE_REMOTE_CODEC(system_services_per_PerReply)
#endif

namespace theia {
namespace migrate {

// One migration step, decoded from migration.json.
struct MigrationStep {
    std::string config_type, from_digest, to_digest, plugin;
};

// Singleton link to per's PerManager (Snapshot/MigrateBulk/Restore) at
// 0x80010016. Shared by whichever authority drives the migration.
struct PerManLink {
    struct PerManagerTag { static constexpr const char* kNodeName = "per_manager"; };
    using PerManRef = ::theia::runtime::RemoteRef<PerManagerTag, 0x80010016u, 0u>;
    PerManRef                 ref;
    ::theia::runtime::TipcMux mux;
    bool                      started = false;
    std::mutex                mu;
    static PerManLink& instance() { static PerManLink l; return l; }
    bool ensure_started() {
        if (started) return true;
        if (!ref.connect(/*timeout_ms=*/2000)) return false;
        mux.watch_remote_ref(ref);
        mux.start();
        started = true;
        return true;
    }
};

// ---- version-delta gate ----------------------------------------------------
// major.minor.patch parse (short/padded forms tolerated; non-numeric → 0).
struct SemVer { long major = 0, minor = 0, patch = 0; };

inline SemVer parse_semver(const std::string& v) {
    SemVer s;
    long* parts[3] = {&s.major, &s.minor, &s.patch};
    size_t i = 0, p = 0;
    while (p < 3 && i <= v.size()) {
        size_t dot = v.find('.', i);
        std::string tok = v.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        // strip any trailing -abi/qualifier on the last token
        size_t dash = tok.find('-');
        if (dash != std::string::npos) tok = tok.substr(0, dash);
        *parts[p++] = tok.empty() ? 0 : std::strtol(tok.c_str(), nullptr, 10);
        if (dot == std::string::npos) break;
        i = dot + 1;
    }
    return s;
}

// The gate the SWP-interface-stability invariant demands: a PATCH bump ships the
// SAME .art + SAME configs + SAME schema digests, so it MUST NOT migrate. Returns
// true iff (from → to) is a config-migrating step: same major AND a minor delta.
// A patch-only delta (major.minor equal) → false (no migration). A major delta →
// false too (a major is a FRESH deploy, never a config migration — see the design
// doc). from == "" (unknown current) → true (be safe: let the artifact decide).
inline bool version_delta_migrates(const std::string& from, const std::string& to) {
    if (from.empty()) return true;
    SemVer f = parse_semver(from), t = parse_semver(to);
    if (f.major != t.major) return false;     // major = fresh deploy, no migration
    return f.minor != t.minor;                // migrate iff the MINOR changed
}

// ---- per calls -------------------------------------------------------------
template <typename ReqT>
inline bool per_manager_call(const ReqT& req, const char* what, std::string* err) {
    auto& link = PerManLink::instance();
    std::lock_guard<std::mutex> lk(link.mu);
    if (!link.ensure_started()) { *err = "per_manager unreachable"; return false; }
    auto result = ::theia::runtime::call<system_services_per_PerReply>(
        link.ref, req, 0, 5000);
    if (result.tag != ::theia::runtime::CallTag::Reply) {
        *err = std::string(what) + ": no reply";
        return false;
    }
    if (result.reply.status != 0) {
        *err = std::string(what) + ": " + result.reply.message;
        return false;
    }
    return true;
}

inline bool per_snapshot(const std::string& label, std::string* err) {
    system_services_per_SnapshotReq req = system_services_per_SnapshotReq_init_zero;
    std::snprintf(req.label, sizeof(req.label), "%s", label.c_str());
    return per_manager_call(req, "Snapshot", err);
}

inline bool per_restore(const std::string& label, std::string* err) {
    system_services_per_RestoreSnapshotReq req =
        system_services_per_RestoreSnapshotReq_init_zero;
    std::snprintf(req.label, sizeof(req.label), "%s", label.c_str());
    return per_manager_call(req, "RestoreSnapshot", err);
}

inline bool per_migrate_bulk(const MigrationStep& st, const std::string& plugin_abs,
                             std::string* err) {
    system_services_per_MigrateBulkReq req =
        system_services_per_MigrateBulkReq_init_zero;
    std::snprintf(req.config_type, sizeof(req.config_type), "%s", st.config_type.c_str());
    std::snprintf(req.from_digest, sizeof(req.from_digest), "%s", st.from_digest.c_str());
    std::snprintf(req.to_digest, sizeof(req.to_digest), "%s", st.to_digest.c_str());
    std::snprintf(req.plugin_so, sizeof(req.plugin_so), "%s", plugin_abs.c_str());
    return per_manager_call(req, "MigrateBulk", err);
}

// releases/<ver>/migration/migration.json → steps. Empty vector + return true =
// no migration part (the common free-swap / patch case). Present-but-unparsable →
// return false: fail CLOSED (a migration we can't read must abort, not skip).
inline bool load_migration_steps(const std::string& releases_root,
                                 const std::string& ver,
                                 std::vector<MigrationStep>* out) {
    const std::string dir = releases_root + "/" + ver + "/migration";
    std::ifstream f(dir + "/migration.json");
    if (!f.good()) return true;                        // no migration part → 0 steps
    try {
        nlohmann::json doc = nlohmann::json::parse(f);
        for (const auto& st : doc.at("steps")) {
            MigrationStep m;
            m.config_type = st.at("config_type").get<std::string>();
            m.from_digest = st.at("from_digest").get<std::string>();
            m.to_digest   = st.at("to_digest").get<std::string>();
            m.plugin      = st.at("plugin").get<std::string>();
            out->push_back(std::move(m));
        }
        return true;
    } catch (const std::exception&) {
        return false;                                  // unreadable manifest → abort
    }
}

// The absolute plugin path inside a staged release: releases/<ver>/migration/<so>.
inline std::string plugin_path(const std::string& releases_root,
                               const std::string& ver, const std::string& plugin) {
    return releases_root + "/" + ver + "/migration/" + plugin;
}

}  // namespace migrate
}  // namespace theia
