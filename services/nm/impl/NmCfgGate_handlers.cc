// User handler bodies for NmCfgGate.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/NmCfgGate.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/NmCfgGate.hh"

#include <cstdio>

namespace system_services_nm {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/NmCfgGate_state.hh.
void NmCfgGate::init(NmCfgGateState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void NmCfgGate::handle_info(const char* /*info*/, NmCfgGateState& /*s*/) {
}




NmCfgReply NmCfgGate::handle_call(
        const AddWifiReq& /*req*/,
        NmCfgGateState& /*s*/) {
    // TODO: implement AddWifi (AddWifiReq →
    //                                 NmCfgReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] AddWifi called\n",
                 kNodeName);
    return NmCfgReply{};
}

NmCfgReply NmCfgGate::handle_call(
        const RemoveWifiReq& /*req*/,
        NmCfgGateState& /*s*/) {
    // TODO: implement RemoveWifi (RemoveWifiReq →
    //                                 NmCfgReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] RemoveWifi called\n",
                 kNodeName);
    return NmCfgReply{};
}

NmCfgReply NmCfgGate::handle_call(
        const SetVpnReq& /*req*/,
        NmCfgGateState& /*s*/) {
    // TODO: implement SetVpn (SetVpnReq →
    //                                 NmCfgReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] SetVpn called\n",
                 kNodeName);
    return NmCfgReply{};
}

NmCfgReply NmCfgGate::handle_call(
        const SetAutoConnectReq& /*req*/,
        NmCfgGateState& /*s*/) {
    // TODO: implement SetAutoConnect (SetAutoConnectReq →
    //                                 NmCfgReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] SetAutoConnect called\n",
                 kNodeName);
    return NmCfgReply{};
}

NmCfgReply NmCfgGate::handle_call(
        const ConfirmReq& /*req*/,
        NmCfgGateState& /*s*/) {
    // TODO: implement ConfirmConfig (ConfirmReq →
    //                                 NmCfgReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] ConfirmConfig called\n",
                 kNodeName);
    return NmCfgReply{};
}

NmCfgReply NmCfgGate::handle_call(
        const AbortReq& /*req*/,
        NmCfgGateState& /*s*/) {
    // TODO: implement AbortConfig (AbortReq →
    //                                 NmCfgReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] AbortConfig called\n",
                 kNodeName);
    return NmCfgReply{};
}


}  // namespace system_services_nm
