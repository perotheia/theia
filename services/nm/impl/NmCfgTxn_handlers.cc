// User handler bodies for NmCfgTxn (STATEM variant).
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Declarations are in lib/NmCfgTxn.hh.
//
// What lives here (user code):
//   - on_enter — fires after every committed FSM transition (and
//     once at init with new==old). Cast / send_after / log here.
//     Returns void, so transition_to() is COMPILE-TIME forbidden.
//   - handle_call overloads for any server-port operations — map
//     external requests onto FSM events or daemon state.
//
// Default bodies are no-ops with a stderr log so traffic is
// observable. Replace with real behaviour as the FC matures.

#include "lib/NmCfgTxn.hh"

#include <cstdio>

namespace system_services_nm {



// on_enter — runs on the FSM thread AFTER every committed
// transition. The framework also fires it once at init with
// new_s == old_s == STEADY. SAFE to call
// cast/post_event/broadcast from here; UNSAFE to transition.
void NmCfgTxn::on_enter(NmCfgTxnState new_s,
                              NmCfgTxnState /*old_s*/,
                              NmCfgTxnData& /*d*/) {
    static const char* names[] = {
        "STEADY",
        "VALIDATING",
        "PENDING",
    };
    const auto idx = static_cast<std::size_t>(new_s);
    std::fprintf(stderr, "[%s] → %s\n",
                 kNodeName,
                 idx < sizeof(names)/sizeof(names[0]) ? names[idx] : "?");
    // TODO: fan out to subscribe_<port>_<msg> registrations here.
}



}  // namespace system_services_nm
