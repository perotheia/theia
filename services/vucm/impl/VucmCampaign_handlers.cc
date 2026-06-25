// User handler bodies for VucmCampaign (STATEM variant).
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Declarations are in lib/VucmCampaign.hh.
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

#include "lib/VucmCampaign.hh"

#include <cstdio>

namespace ara::vucm {



// on_enter — runs on the FSM thread AFTER every committed
// transition. The framework also fires it once at init with
// new_s == old_s == CMP_IDLE. SAFE to call
// cast/post_event/broadcast from here; UNSAFE to transition.
void VucmCampaign::on_enter(VucmCampaignState new_s,
                              VucmCampaignState /*old_s*/,
                              VucmCampaignData& /*d*/) {
    static const char* names[] = {
        "CMP_IDLE",
        "CMP_PLANNING",
        "CMP_AUTHORIZING",
        "CMP_INSTALLING",
        "CMP_VALIDATING",
        "CMP_DONE",
        "CMP_ROLLBACK",
    };
    const auto idx = static_cast<std::size_t>(new_s);
    std::fprintf(stderr, "[%s] → %s\n",
                 kNodeName,
                 idx < sizeof(names)/sizeof(names[0]) ? names[idx] : "?");
    // TODO: fan out to subscribe_<port>_<msg> registrations here.
}
// ---- config update — services/per casts ConfigUpdated when this statem
//      node's etcd-backed `config VucmConfig` changes. The GenServer base
//      (GenStateM derives from it) decoded the envelope + logged; apply the
//      typed config here (ParseFromString cfg.config into VucmConfig, honor
//      the changed mask; reach the FSM via h.data / post_event(*this, …)).
//      Empty default — a node that only reads config at boot leaves this as-is.
void VucmCampaign::on_config_update(
        const platform_runtime_ConfigUpdated& /*cfg*/,
        ::theia::runtime::GenStateMHolder<VucmCampaignState, VucmCampaignData>& /*h*/) {
}




}  // namespace ara::vucm
