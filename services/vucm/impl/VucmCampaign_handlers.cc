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

#include "NodeRef.hh"   // theia::runtime::LocalRef — publish self to the gate

#include <cstdio>
#include <string>

namespace ara::vucm {

// The gate post_event()s into this peer (defined in VucmGate_handlers.cc); the
// campaign FSM publishes ITS ref here so the gate's post_event finds the target.
theia::runtime::LocalRef<VucmCampaign>& vucm_campaign_ref();

namespace {
// The C++ VucmCampaignState enum is DENSE 0..7 (CONFIRMING=4, VALIDATING=5,
// DONE=6, ROLLBACK=7) but the WIRE CampaignState keeps the original ordinals
// stable (CONFIRMING appended =7, VALIDATING=4, DONE=5, ROLLBACK=6). A direct
// cast would be WRONG — map explicitly so the broadcast carries the wire value
// consumers (com / the GS fleet view) decode.
system_services_vucm_CampaignState wire_state(VucmCampaignState s) {
    switch (s) {
        case VucmCampaignState::CMP_IDLE:        return system_services_vucm_CampaignState_CampaignState_CMP_IDLE;
        case VucmCampaignState::CMP_PLANNING:    return system_services_vucm_CampaignState_CampaignState_CMP_PLANNING;
        case VucmCampaignState::CMP_AUTHORIZING: return system_services_vucm_CampaignState_CampaignState_CMP_AUTHORIZING;
        case VucmCampaignState::CMP_INSTALLING:  return system_services_vucm_CampaignState_CampaignState_CMP_INSTALLING;
        case VucmCampaignState::CMP_CONFIRMING:  return system_services_vucm_CampaignState_CampaignState_CMP_CONFIRMING;
        case VucmCampaignState::CMP_VALIDATING:  return system_services_vucm_CampaignState_CampaignState_CMP_VALIDATING;
        case VucmCampaignState::CMP_DONE:        return system_services_vucm_CampaignState_CampaignState_CMP_DONE;
        case VucmCampaignState::CMP_ROLLBACK:    return system_services_vucm_CampaignState_CampaignState_CMP_ROLLBACK;
    }
    return system_services_vucm_CampaignState_CampaignState_CMP_IDLE;
}
}  // namespace

// on_enter — runs on the FSM thread AFTER every committed transition (and once at
// init with new_s == old_s == CMP_IDLE). Publishes self to the gate, stamps the
// wire state into the CampaignProgress data, and broadcasts it to every
// CampaignStream subscriber (com / the GS fleet view watch the campaign walk its
// lifecycle — the same way they watch UcmProgress one layer down).
void VucmCampaign::on_enter(VucmCampaignState new_s,
                              VucmCampaignState /*old_s*/,
                              VucmCampaignData& d) {
    if (!vucm_campaign_ref().valid()) {
        vucm_campaign_ref() = theia::runtime::LocalRef<VucmCampaign>(*this);
    }
    d.state = wire_state(new_s);
    this->log().info(std::string("→ ") + VucmCampaign::state_name(new_s));
    broadcast_progress_progress(d);
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
