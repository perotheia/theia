// State struct for FgGate — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-app --kind fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-app does NOT derive from the .art — so you own this file.
// Add the fields the node's handlers / init() need here; the generated
// lib/FgGate.hh #includes it and binds it as the GenServer
// State type.

#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ara::sm {

// FgGate is the MULTI-FG authority: it tracks each Function Group's lifecycle
// state in a map (fg id → FgState ordinal) and drives that FG's mapped
// supervision sub-tree via the supervisor (sm_sup_link). A single FunctionGroupSm
// statem node remains as the MachineFG(0) reference for trace/observability, but
// the per-FG fan-out lives here.
struct FgGateState {
    // fg id → current FgState (0 FG_OFF .. 4 FG_RESTART). A missing fg defaults
    // to FG_OFF on first reference.
    std::map<uint32_t, uint8_t> fg_state;

    uint8_t state_of(uint32_t fg) const {
        auto it = fg_state.find(fg);
        return it == fg_state.end() ? 0 /*FG_OFF*/ : it->second;
    }
    void set_state(uint32_t fg, uint8_t st) { fg_state[fg] = st; }
};

}  // namespace ara::sm
