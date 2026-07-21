// State struct for ObserverNode — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-fc does NOT derive from the .art — so you own this file.
// Add the fields the node's handlers / init() need here; the generated
// lib/ObserverNode.hh #includes it and binds it as the GenServer
// State type.

#pragma once

#include <cstdint>
#include <string>

namespace system_apps {

struct ObserverNodeState {
    int polls_issued = 0;
    int replies_ok   = 0;
    int last_value   = 0;
};

}  // namespace system_apps
