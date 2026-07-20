// State struct for ComDaemon — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-app --kind fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-app does NOT derive from the .art — so you own this file.
// Add the fields the node's handlers / init() need here; the generated
// lib/ComDaemon.hh #includes it and binds it as the GenServer
// State type.

#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ara::com {

struct ComDaemonState {
    // Network-binding gate (SM → COM, EnableBindings). One entry per named
    // binding (a service-discovery scope / VLAN, cf. sm.md §3.B "network
    // gating"): binding_name → enabled. SM disables scopes on UPDATE/DEGRADED and
    // re-enables on RUNNING. The advertisement path consults this before offering
    // a scope; an ABSENT binding is enabled-by-default (nothing has gated it).
    // Authoritative in-process record so the gate survives across calls and an
    // observer can read it.
    std::map<std::string, bool> bindings;

    // True iff `name` may currently be advertised (default: enabled — only an
    // explicit EnableBindings{enabled=false} gates it off).
    bool binding_enabled(const std::string& name) const {
        auto it = bindings.find(name);
        return it == bindings.end() ? true : it->second;
    }
};

}  // namespace ara::com
