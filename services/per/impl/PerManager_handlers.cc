// User handler bodies for PerManager.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/PerManager.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/PerManager.hh"

#include <cstdio>

namespace system_services_per {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/PerManager_state.hh.
void PerManager::init(PerManagerState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void PerManager::handle_info(const char* /*info*/, PerManagerState& /*s*/) {
}




PerReply PerManager::handle_call(
        const RegisterSchemaReq& /*req*/,
        PerManagerState& /*s*/) {
    // TODO: implement RegisterSchema (RegisterSchemaReq →
    //                                 PerReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] RegisterSchema called\n",
                 kNodeName);
    return PerReply{};
}

SchemaList PerManager::handle_call(
        const ListSchemasReq& /*req*/,
        PerManagerState& /*s*/) {
    // TODO: implement ListSchemas (ListSchemasReq →
    //                                 SchemaList).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] ListSchemas called\n",
                 kNodeName);
    return SchemaList{};
}

PerReply PerManager::handle_call(
        const MigrateBulkReq& /*req*/,
        PerManagerState& /*s*/) {
    // TODO: implement MigrateBulk (MigrateBulkReq →
    //                                 PerReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] MigrateBulk called\n",
                 kNodeName);
    return PerReply{};
}

PerReply PerManager::handle_call(
        const SnapshotReq& /*req*/,
        PerManagerState& /*s*/) {
    // TODO: implement Snapshot (SnapshotReq →
    //                                 PerReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] Snapshot called\n",
                 kNodeName);
    return PerReply{};
}

PerReply PerManager::handle_call(
        const RestoreSnapshotReq& /*req*/,
        PerManagerState& /*s*/) {
    // TODO: implement RestoreSnapshot (RestoreSnapshotReq →
    //                                 PerReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] RestoreSnapshot called\n",
                 kNodeName);
    return PerReply{};
}


}  // namespace system_services_per
