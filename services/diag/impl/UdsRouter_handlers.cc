// User handler bodies for UdsRouter.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/UdsRouter.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/UdsRouter.hh"

#include <cstdio>

namespace ara::diag {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/UdsRouter_state.hh.
void UdsRouter::init(UdsRouterState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void UdsRouter::handle_info(const char* /*info*/, UdsRouterState& /*s*/) {
}

// ---- config update — services/per casts ConfigUpdated when this node's
//      etcd-backed `config DiagConfig` changes. The GenServer base decoded
//      the envelope + logged; apply the typed config here (ParseFromString
//      cfg.config into DiagConfig, honor the changed mask). Empty default —
//      a node that only reads config at boot leaves this as-is.
void UdsRouter::on_config_update(
        const platform_runtime_ConfigUpdated& /*cfg*/,
        UdsRouterState& /*s*/) {
}





UdsReply UdsRouter::handle_call(
        const UdsRequest& /*req*/,
        UdsRouterState& /*s*/) {
    // TODO: implement Handle (UdsRequest →
    //                                 UdsReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] Handle called\n",
                 kNodeName);
    return UdsReply{};
}


}  // namespace ara::diag
