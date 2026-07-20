// User handler bodies for ComDaemon.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/ComDaemon.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/ComDaemon.hh"

#include <cstdio>

namespace ara::com {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/ComDaemon_state.hh.
void ComDaemon::init(ComDaemonState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void ComDaemon::handle_info(const char* /*info*/, ComDaemonState& /*s*/) {
}




ComEmpty ComDaemon::handle_call(
        const ComEmpty& /*req*/,
        ComDaemonState& /*s*/) {
    // TODO: implement Ping (ComEmpty →
    //                                 ComEmpty).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] Ping called\n",
                 kNodeName);
    return ComEmpty{};
}

// EnableBindings (SM → COM control surface, ComCtl): record the enable/disable
// state of a named network binding (a service-discovery scope / VLAN). SM gates
// scopes off on UPDATE/DEGRADED and back on for RUNNING (sm.md §3.B). We keep the
// authoritative per-binding state in ComDaemonState; the advertisement path
// consults binding_enabled() before offering a scope. Idempotent (a repeated
// request is a no-op re-assert). `binding_name` is a fixed char[64] (com.options),
// NUL-terminated by nanopb.
ComEmpty ComDaemon::handle_call(
        const NetworkBindingRequest& req,
        ComDaemonState& s) {
    const std::string name(req.binding_name);
    if (name.empty()) {
        this->log().warn("EnableBindings: empty binding_name — ignored");
        return ComEmpty{};
    }
    const bool was = s.binding_enabled(name);
    s.bindings[name] = req.enabled;
    if (was != req.enabled) {
        this->log().info(std::string("binding '") + name + "' " +
                         (req.enabled ? "ENABLED (advertise)"
                                      : "DISABLED (stop offering)"));
    }
    return ComEmpty{};
}


}  // namespace ara::com
