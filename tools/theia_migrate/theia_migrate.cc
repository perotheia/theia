// theia-migrate — on-device config migration for the SWP OVERLAY path.
//
// The theia-swp update module overlays FC binaries WITHOUT the UCM lifecycle,
// so UcmGate's C++ migration (EvInstalled) never fires for an overlay. This
// tiny tool does exactly what UcmGate does — Snapshot + MigrateBulk over TIPC
// to per (PerManager, 0x80010016) — in C++, so NO python/probe/artheia is
// needed on the rig (the probe is a dev tool; it ships to no board).
//
// Reads releases/<ver>/migration/migration.json (the SWP artifact part packed
// by `theia release-swp --migrate`) — the same file+format UcmGate reads:
//   {"artifact": "...", "steps": [{config_type, from_digest, to_digest,
//                                  plugin, transform}, ...]}
// The plugin .so is dlopen'd BY PER (MigrateBulk's plugin_so arg = the abs
// path in the migration dir); this tool just drives the calls.
//
// Usage:
//   theia-migrate forward  <migration-dir>   Snapshot(pre-<artifact>) + MigrateBulk per step
//   theia-migrate rollback <migration-dir>   RestoreSnapshot(pre-<artifact>)
//
// Exit 0 on success; non-zero (LOUD) on any failure so the theia-swp module
// aborts the install → Mender rolls back. A dir with no migration.json is a
// no-op success (the free-swap case).

#include "NodeRef.hh"          // theia::runtime::RemoteRef / call<>
#include "TipcMux.hh"          // reply pump
#include "RemoteCodec.hh"      // THEIA_DECLARE_REMOTE_CODEC
#include "system/services/per/per.pb.h"

#include "nlohmann/json.hpp"

#include <pb_encode.h>
#include <pb_decode.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

THEIA_DECLARE_REMOTE_CODEC(system_services_per_PerReply)
THEIA_DECLARE_REMOTE_CODEC(system_services_per_SnapshotReq)
THEIA_DECLARE_REMOTE_CODEC(system_services_per_RestoreSnapshotReq)
THEIA_DECLARE_REMOTE_CODEC(system_services_per_MigrateBulkReq)

namespace {

struct Step {
    std::string config_type, from_digest, to_digest, plugin;
};

// Singleton link to per's PerManager (mirrors UcmGate's PerManLink).
struct PerManLink {
    struct Tag { static constexpr const char* kNodeName = "per_manager"; };
    using Ref = ::theia::runtime::RemoteRef<Tag, 0x80010016u, 0u>;
    Ref                       ref;
    ::theia::runtime::TipcMux  mux;
    bool                      started = false;
    bool ensure() {
        if (started) return true;
        if (!ref.connect(/*timeout_ms=*/5000)) return false;
        mux.watch_remote_ref(ref);
        mux.start();
        started = true;
        return true;
    }
};

PerManLink g_link;

template <typename ReqT>
bool per_call(const ReqT& req, const char* what) {
    if (!g_link.ensure()) {
        std::fprintf(stderr, "theia-migrate: per_manager unreachable\n");
        return false;
    }
    auto result = ::theia::runtime::call<system_services_per_PerReply>(
        g_link.ref, req, 0, 15000);
    if (result.tag != ::theia::runtime::CallTag::Reply) {
        std::fprintf(stderr, "theia-migrate: %s — no reply\n", what);
        return false;
    }
    if (result.reply.status != 0) {
        std::fprintf(stderr, "theia-migrate: %s failed: %s\n", what,
                     result.reply.message);
        return false;
    }
    return true;
}

bool snapshot(const std::string& label) {
    system_services_per_SnapshotReq req = system_services_per_SnapshotReq_init_zero;
    std::snprintf(req.label, sizeof(req.label), "%s", label.c_str());
    return per_call(req, "Snapshot");
}

bool restore(const std::string& label) {
    system_services_per_RestoreSnapshotReq req =
        system_services_per_RestoreSnapshotReq_init_zero;
    std::snprintf(req.label, sizeof(req.label), "%s", label.c_str());
    return per_call(req, "RestoreSnapshot");
}

bool migrate_bulk(const Step& s, const std::string& plugin_abs) {
    system_services_per_MigrateBulkReq req =
        system_services_per_MigrateBulkReq_init_zero;
    std::snprintf(req.config_type, sizeof(req.config_type), "%s", s.config_type.c_str());
    std::snprintf(req.from_digest, sizeof(req.from_digest), "%s", s.from_digest.c_str());
    std::snprintf(req.to_digest,   sizeof(req.to_digest),   "%s", s.to_digest.c_str());
    std::snprintf(req.plugin_so,   sizeof(req.plugin_so),   "%s", plugin_abs.c_str());
    return per_call(req, "MigrateBulk");
}

// Parse migration.json → (artifact, steps). Returns false only on a PRESENT but
// unparsable manifest (fail closed); a MISSING file → true with 0 steps.
bool load(const std::string& dir, std::string* artifact, std::vector<Step>* steps) {
    std::ifstream f(dir + "/migration.json");
    if (!f.good()) return true;                       // no migration part
    try {
        nlohmann::json doc = nlohmann::json::parse(f);
        *artifact = doc.at("artifact").get<std::string>();
        for (const auto& st : doc.at("steps")) {
            steps->push_back({st.at("config_type").get<std::string>(),
                              st.at("from_digest").get<std::string>(),
                              st.at("to_digest").get<std::string>(),
                              st.at("plugin").get<std::string>()});
        }
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "theia-migrate: migration.json unparsable: %s\n", e.what());
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: theia-migrate <forward|rollback> <migration-dir>\n");
        return 2;
    }
    const std::string verb = argv[1];
    const std::string dir  = argv[2];

    std::string artifact;
    std::vector<Step> steps;
    if (!load(dir, &artifact, &steps)) return 1;      // unparsable → fail closed
    if (steps.empty()) {
        std::printf("theia-migrate: no migration steps (free-swap)\n");
        return 0;
    }
    const std::string label = "pre-" + artifact;

    if (verb == "forward") {
        if (!snapshot(label)) return 1;
        std::printf("theia-migrate: snapshot '%s' taken\n", label.c_str());
        for (const auto& s : steps) {
            const std::string so = dir + "/" + s.plugin;
            if (!migrate_bulk(s, so)) return 1;
            std::printf("theia-migrate: %s %s -> %s\n",
                        s.config_type.c_str(), s.from_digest.c_str(),
                        s.to_digest.c_str());
        }
        return 0;
    }
    if (verb == "rollback") {
        if (!restore(label)) {
            std::fprintf(stderr, "theia-migrate: CONFIG RESTORE FAILED — config "
                "may be AHEAD of the rolled-back SW; restore '%s' manually\n",
                label.c_str());
            return 1;
        }
        std::printf("theia-migrate: config restored from '%s'\n", label.c_str());
        return 0;
    }
    std::fprintf(stderr, "theia-migrate: unknown verb '%s'\n", verb.c_str());
    return 2;
}
